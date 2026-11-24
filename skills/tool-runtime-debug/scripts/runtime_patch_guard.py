#!/usr/bin/env python3
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Guard Runtime repairs against verifier and evidence tampering.

用途：比较 Runtime 修复前后的源码快照，拦截验证器、Golden 参考、失败分支
和受保护文件的改动，并限制补丁范围。

使用方法：
    python3 runtime_patch_guard.py <before.cpp> <after.cpp>
    python3 runtime_patch_guard.py --before <before.cpp> --after <after.cpp>
    python3 runtime_patch_guard.py --help
"""

from __future__ import annotations

import argparse
import contextlib
import errno
import hashlib
import json
import os
import re
import stat
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from runtime_contract_lint import (  # noqa: E402
    ALLOCATORS,
    EVENT_RECORDERS,
    OUTPUT_CONTRACTS,
    _canonical_type,
    _calls,
    _declared_types,
    _identifier,
    _strip_comments,
)


SCHEMA_VERSION = "runtime-patch-guard/2.0"
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx"})
WORKSPACE_SOURCE_SUFFIXES = SOURCE_SUFFIXES | frozenset(
    {".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".ipp", ".tpp", ".asc", ".cce"}
)
MAX_SOURCE_BYTES = 4 * 1024 * 1024
MAX_PROTECTED_BYTES = 16 * 1024 * 1024
IGNORED_DIR_NAMES = frozenset(
    {
        ".git",
        ".hg",
        ".svn",
        "__pycache__",
        ".pytest_cache",
        ".mypy_cache",
        ".ruff_cache",
        ".tox",
        ".venv",
        "venv",
        "node_modules",
    }
)
OBSERVATION_CALL = re.compile(r"(?:std::)?(?:printf|fprintf|puts)\s*\(|std::cout\s*<<")
RUNTIME_CALL = re.compile(r"\bacl(?:rt)?[A-Z][A-Za-z0-9_]*\s*\(")
CLEANUP_CALL = re.compile(
    r"\b(acl(?:rt)?(?:Free|Destroy|Reset)[A-Za-z0-9_]*|aclFinalize)\s*\("
)
MARKER = re.compile(r"\[(?:VERIFY|DATA|RESOURCE|OBS)\]", re.IGNORECASE)
RETURN_STATEMENT = re.compile(r"\breturn\s+[^;]+;")
SLEEP_CALL = re.compile(
    r"\b(?:sleep|usleep|nanosleep)\s*\(|(?:std::)?this_thread::sleep_for\s*\(",
    re.IGNORECASE,
)
FUNCTION_START = re.compile(
    r"(?m)^[^#\n;{}]*\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?\{"
)
PROTECTED_NAME = re.compile(
    r"^(?:main|run_.*_carrier|.*(?:verify|validate|check|gate|judge).*)$", re.IGNORECASE
)
PREDICATE_NAME = re.compile(
    r"\b\w*(?:verify|verified|passed|validated)\w*\b", re.IGNORECASE
)
VERIFICATION_TERM = re.compile(
    r"\b(?:verify|verified|passed|validated|match(?:es)?|reference|expected|actual|golden|"
    r"oracle|toleran(?:ce|t)|compare|comparison|result|status|success|failure|error|rc|ret)\b",
    re.IGNORECASE,
)
COMPARISON_OPERATOR = re.compile(r"==|!=|<=|>=|<|>|&&|\|\|")
CONTROL_FLOW = re.compile(r"\b(?:if|else\s+if|while|for|switch)\s*\(", re.IGNORECASE)
VERIFICATION_CALL = re.compile(
    r"\b(?:memcmp|strcmp|compare|isclose|allclose|assert|validate|check)\w*\s*\(",
    re.IGNORECASE,
)
CPP_IGNORED = {
    "nullptr",
    "true",
    "false",
    "static_cast",
    "reinterpret_cast",
    "const_cast",
    "sizeof",
}
STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
PROTECTED_FILE_BASENAMES = frozenset({"task.md", "cmakelists.txt"})
PROTECTED_FILE_MARKERS = (
    "golden",
    "oracle",
    "expected",
    "verify",
    "validate",
    "judge",
    "check",
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _normalize(value: str) -> str:
    return " ".join(value.split())


def _without_comments(text: str) -> str:
    return _strip_comments(text)


def _matching_brace(text: str, open_at: int) -> int:
    depth = 0
    for index in range(open_at, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index + 1
    return len(text)


def _matching_paren(text: str, open_at: int) -> int:
    depth = 0
    quote: str | None = None
    escaped = False
    for index in range(open_at, len(text)):
        character = text[index]
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    return len(text)


def _split_parameters(parameters: str) -> list[str]:
    parameters = parameters.strip()
    if not parameters or parameters == "void":
        return []
    depth = 0
    start = 0
    result: list[str] = []
    quote: str | None = None
    escaped = False
    for index, character in enumerate(parameters):
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character in "([{<":
            depth += 1
        elif character in ")]}>":
            depth = max(0, depth - 1)
        elif character == "," and depth == 0:
            result.append(parameters[start:index].strip())
            start = index + 1
    result.append(parameters[start:].strip())
    return result


def _parameter_signature(parameters: str) -> str:
    """Normalize parameter types while dropping only declarator names/defaults."""
    normalized: list[str] = []
    for parameter in _split_parameters(parameters):
        parameter = parameter.split("=", 1)[0].strip()
        tokens = parameter.split()
        if len(tokens) > 1 and re.fullmatch(r"[A-Za-z_]\w*", tokens[-1]):
            # The final identifier is a parameter name when a type token
            # precedes it.  A one-token type such as ``int`` is retained.
            tokens.pop()
        normalized.append(" ".join(tokens))
    return ",".join(normalized)


def _parameter_count(parameters: str) -> int:
    return len(_split_parameters(parameters))


def _namespace_ranges(text: str) -> list[tuple[int, int, str]]:
    ranges: list[tuple[int, int, str]] = []
    for match in re.finditer(
        r"\bnamespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\{", text
    ):
        open_at = text.find("{", match.start(), match.end())
        ranges.append((match.start(), _matching_brace(text, open_at), match.group(1)))
    return ranges


def _qualified_function_name(text: str, match: re.Match[str], name: str) -> str:
    prefix = match.group("prefix").strip()
    qualifier_match = re.search(r"((?:[A-Za-z_]\w*::)+)$", prefix)
    explicit = qualifier_match.group(1)[:-2] if qualifier_match else ""
    namespaces = [
        qualifier
        for start, end, qualifier in _namespace_ranges(text)
        if start < match.start() < end
    ]
    namespace = "::".join(namespaces)
    return "::".join(part for part in (namespace, explicit, name) if part)


def _function_records(text: str) -> list[dict[str, Any]]:
    clean = _without_comments(text)
    definition = re.compile(
        r"(?m)^(?P<prefix>[^#\n;{}]*?)\b(?P<name>[A-Za-z_]\w*)\s*"
        r"\((?P<parameters>[^;{}]*)\)\s*(?:const\s*)?\{"
    )
    records: list[dict[str, Any]] = []
    occurrences: Counter[tuple[str, int]] = Counter()
    for match in definition.finditer(clean):
        name = match.group("name")
        if name in {"if", "for", "while", "switch", "catch"}:
            continue
        open_at = clean.find("{", match.start(), match.end())
        if open_at < 0:
            continue
        end = _matching_brace(clean, open_at)
        qualified = _qualified_function_name(clean, match, name)
        parameter_count = _parameter_count(match.group("parameters"))
        signature = _parameter_signature(match.group("parameters"))
        occurrence = occurrences[(qualified, signature)]
        occurrences[(qualified, signature)] += 1
        records.append(
            {
                "identity": f"{qualified}({signature})/{occurrence}",
                "qualified_name": qualified,
                "name": name,
                "parameter_count": parameter_count,
                "parameter_signature": signature,
                "start": match.start(),
                "end": end,
                "body": clean[open_at + 1 : max(open_at + 1, end - 1)],
            }
        )
    return records


def _generic_calls(text: str) -> list[tuple[str, int]]:
    clean = _without_comments(text)
    calls: list[tuple[str, int]] = []
    keywords = {"if", "for", "while", "switch", "catch", "sizeof"}
    for match in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", clean):
        name = match.group(1)
        if name in keywords:
            continue
        open_at = clean.find("(", match.start(), match.end())
        end = _matching_paren(clean, open_at)
        calls.append(
            (name, _parameter_count(clean[open_at + 1 : max(open_at + 1, end - 1)]))
        )
    return calls


def _protected_returns(text: str) -> list[str]:
    clean = _without_comments(text)
    records = _function_records(clean)
    roots = {
        index
        for index, record in enumerate(records)
        if record["name"] == "main" or PROTECTED_NAME.match(record["name"])
    }
    reachable = set(roots)
    pending = list(roots)
    while pending:
        index = pending.pop()
        body = records[index]["body"]
        for called, argument_count in _generic_calls(body):
            matches = [
                candidate
                for candidate, record in enumerate(records)
                if record["name"] == called
                and record["parameter_count"] == argument_count
            ]
            if not matches:
                # An unresolved expression, macro, or function-pointer-like
                # call must not let an overload escape the protected surface.
                matches = [
                    candidate
                    for candidate, record in enumerate(records)
                    if record["name"] == called
                ]
            for candidate in matches:
                if candidate not in reachable:
                    reachable.add(candidate)
                    pending.append(candidate)
    returns: list[str] = []
    for index in sorted(reachable):
        record = records[index]
        # 保留函数归属与顺序，避免把失败 return 移到其他载体来抵消删除。
        returns.extend(
            record["identity"] + f":{return_index}:" + _normalize(item)
            for return_index, item in enumerate(
                RETURN_STATEMENT.findall(record["body"])
            )
        )
    return sorted(returns)


def _protected_function_signatures(text: str) -> list[str]:
    clean = _without_comments(text)
    records = _function_records(clean)
    roots = {
        index
        for index, record in enumerate(records)
        if record["name"] == "main" or PROTECTED_NAME.match(record["name"])
    }
    reachable = set(roots)
    pending = list(roots)
    while pending:
        index = pending.pop()
        for called, argument_count in _generic_calls(records[index]["body"]):
            matches = [
                candidate
                for candidate, record in enumerate(records)
                if record["name"] == called
                and record["parameter_count"] == argument_count
            ]
            if not matches:
                matches = [
                    candidate
                    for candidate, record in enumerate(records)
                    if record["name"] == called
                ]
            for candidate in matches:
                if candidate not in reachable:
                    reachable.add(candidate)
                    pending.append(candidate)
    return sorted(records[index]["identity"] for index in reachable)


def _verification_predicates(text: str) -> list[str]:
    """Freeze comparisons/control-flow used as verification, independent of names."""
    clean = STRING_LITERAL.sub(
        lambda match: " " * len(match.group(0)), _without_comments(text)
    )
    values: list[str] = []
    # 分号之前保留比较、容差/参考值和状态判断；普通循环边界不在验证面内。
    for statement in clean.split(";"):
        normalized = _normalize(statement)
        if not normalized:
            continue
        if (
            (
                COMPARISON_OPERATOR.search(statement)
                and VERIFICATION_TERM.search(statement)
            )
            or VERIFICATION_CALL.search(statement)
            or PREDICATE_NAME.search(statement)
            or re.search(
                r"\b(?:golden|oracle|reference|expected|actual|matches?)\w*\s*=",
                statement,
                re.IGNORECASE,
            )
        ):
            values.append(normalized)
    # 条件可能没有分号（例如 if (...) { ... }），单独冻结其控制表达式。
    for match in CONTROL_FLOW.finditer(clean):
        open_at = clean.find("(", match.start(), match.end())
        condition = clean[
            open_at + 1 : max(open_at + 1, _matching_paren(clean, open_at) - 1)
        ]
        if (
            COMPARISON_OPERATOR.search(condition)
            and VERIFICATION_TERM.search(condition)
        ) or VERIFICATION_CALL.search(condition):
            values.append(
                _normalize(clean[match.start() : _matching_paren(clean, open_at)])
            )
    return values


def _observation_calls(text: str) -> list[str]:
    clean = _without_comments(text)
    masked = STRING_LITERAL.sub(lambda match: " " * len(match.group(0)), clean)
    values: list[str] = []
    for match in OBSERVATION_CALL.finditer(masked):
        start = match.start()
        if masked.startswith("std::cout", start):
            end = clean.find(";", start)
            values.append(_normalize(clean[start : len(clean) if end < 0 else end + 1]))
            continue
        open_at = masked.find("(", start, match.end())
        depth = 0
        for index in range(open_at, len(clean)):
            if masked[index] == "(":
                depth += 1
            elif masked[index] == ")":
                depth -= 1
                if depth == 0:
                    values.append(_normalize(clean[start : index + 1]))
                    break
    return values


def _marker_lines(text: str) -> list[str]:
    return sorted(
        match.group(0).upper() for match in MARKER.finditer(_without_comments(text))
    )


def _runtime_call_surfaces(text: str) -> list[str]:
    values: list[str] = []
    for match in RUNTIME_CALL.finditer(text):
        fragment = text[match.start() :]
        calls = _calls(fragment)
        if calls and calls[0]["position"] == 0:
            values.append(_normalize(fragment[: calls[0]["end"]]))
    return values


def _runtime_call_records(text: str) -> list[dict[str, Any]]:
    """Capture stable API/argument anchors for one-point Host replacements."""
    clean = _without_comments(text)
    records: list[dict[str, Any]] = []
    occurrence: Counter[tuple[str, str]] = Counter()
    for call in _calls(clean):
        statement_start = (
            max(
                clean.rfind(";", 0, call["position"]),
                clean.rfind("{", 0, call["position"]),
            )
            + 1
        )
        statement_end = clean.find(";", call["end"])
        if statement_end < 0:
            statement_end = clean.find("}", call["end"])
        if statement_end < 0:
            statement_end = len(clean)
        before = _normalize(clean[statement_start : call["position"]])
        after = _normalize(clean[call["end"] : statement_end + 1])
        anchor = _sha256((before + "\0" + after).encode("utf-8"))[:20]
        key = (call["name"], anchor)
        ordinal = occurrence[key]
        occurrence[key] += 1
        records.append(
            {
                "api": call["name"],
                "anchor": anchor,
                "occurrence": ordinal,
                "line": call["line"],
                "arguments": list(call["arguments"]),
            }
        )
    return records


def _canonical_declared_types(
    declarations: dict[str, set[str]], name: str | None
) -> set[str]:
    if not name or name not in declarations:
        return set()
    return {_canonical_type(value) for value in declarations[name]}


def _traceable_argument_replacement(
    before_argument: str | None,
    after_argument: str | None,
    before_declarations: dict[str, set[str]],
    after_declarations: dict[str, set[str]],
) -> bool:
    if before_argument == after_argument:
        return True
    if before_argument is None or after_argument is None:
        return False
    before_name = _identifier(before_argument)
    after_name = _identifier(after_argument)
    if after_name:
        after_types = _canonical_declared_types(after_declarations, after_name)
        if not after_types:
            return False
        before_types = _canonical_declared_types(before_declarations, before_name)
        return not before_types or bool(before_types & after_types)
    # A scalar extent may be corrected without introducing a new variable;
    # arbitrary expressions and pointer substitutions remain unproven.
    literal = re.compile(r"^[+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+)(?:[uUlL]*)$")
    return bool(
        literal.fullmatch(before_argument.strip())
        and literal.fullmatch(after_argument.strip())
    )


def _runtime_argument_edits(
    baseline_records: list[dict[str, Any]],
    candidate_records: list[dict[str, Any]],
    before_declarations: dict[str, set[str]],
    after_text: str,
) -> tuple[list[dict[str, Any]], bool]:
    after_declarations = _declared_types(_without_comments(after_text))
    baseline_by_key: dict[tuple[str, str, int], dict[str, Any]] = {
        (item["api"], item["anchor"], item["occurrence"]): item
        for item in baseline_records
    }
    candidate_by_key: dict[tuple[str, str, int], dict[str, Any]] = {
        (item["api"], item["anchor"], item["occurrence"]): item
        for item in candidate_records
    }
    edits: list[dict[str, Any]] = []
    unmatched_nonmutable = False
    for key in sorted(set(baseline_by_key) | set(candidate_by_key)):
        before = baseline_by_key.get(key)
        after = candidate_by_key.get(key)
        if before is None or after is None:
            api = (before or after)["api"]
            if not _is_mutable_runtime_name(api):
                unmatched_nonmutable = True
            continue
        width = max(len(before["arguments"]), len(after["arguments"]))
        for index in range(width):
            old = (
                before["arguments"][index] if index < len(before["arguments"]) else None
            )
            new = after["arguments"][index] if index < len(after["arguments"]) else None
            if old == new:
                continue
            edits.append(
                {
                    "api": before["api"],
                    "parameter_index": index,
                    "anchor": before["anchor"],
                    "occurrence": before["occurrence"],
                    "line_before": before["line"],
                    "line_after": after["line"],
                    "before_argument": old,
                    "after_argument": new,
                    "traceable": _traceable_argument_replacement(
                        old, new, before_declarations, after_declarations
                    ),
                }
            )
    return edits, not unmatched_nonmutable


def _runtime_names(text: str) -> list[str]:
    names: list[str] = []
    for call in _calls(_without_comments(text)):
        name = call["name"]
        if "Free" in name:
            name = "aclrtFreeFamily"
        elif name == "aclFinalize":
            name = "aclFinalizeFamily"
        names.append(name)
    return names


def _is_mutable_runtime_name(name: str) -> bool:
    """Return whether an API may be added/removed by a Runtime repair.

    Context/device setup and memory/copy calls remain identity-checked.  The
    explicitly repairable business surface is synchronization/wait/event and
    resource release.  Argument edits to an existing call are still guarded by
    dependency and output-type checks below.
    """

    if name in {
        "aclrtFree",
        "aclrtFreeHost",
        "aclrtFreeFamily",
        "aclFinalize",
        "aclFinalizeFamily",
    }:
        return True
    return any(
        token in name
        for token in (
            "Synchronize",
            "Wait",
            "Query",
            "RecordEvent",
            "CreateEvent",
            "DestroyEvent",
            "Destroy",
            "Reset",
            "Notify",
            "EventElapsed",
        )
    )


def _is_timing_runtime_name(name: str) -> bool:
    return any(
        token in name
        for token in (
            "Synchronize",
            "Wait",
            "Query",
            "RecordEvent",
            "CreateEvent",
            "DestroyEvent",
            "EventElapsed",
        )
    )


def _immutable_runtime_names(names: list[str]) -> list[str]:
    return [name for name in names if not _is_mutable_runtime_name(name)]


def _runtime_dependencies(text: str) -> set[str]:
    clean = STRING_LITERAL.sub('""', _without_comments(text))
    values: set[str] = set()
    for call in _calls(clean):
        for argument in call["arguments"]:
            for identifier in re.findall(r"\b[A-Za-z_]\w*\b", argument):
                if identifier in CPP_IGNORED or identifier.startswith(("ACL_", "acl")):
                    continue
                values.add(identifier)
    return values


def _sleep_calls(text: str) -> list[str]:
    clean = STRING_LITERAL.sub(
        lambda match: " " * len(match.group(0)), _without_comments(text)
    )
    return [_normalize(match.group(0)) for match in SLEEP_CALL.finditer(clean)]


def _cleanup_family(name: str) -> str:
    if "Free" in name:
        return "free"
    if name == "aclFinalize":
        return "finalize"
    for family in ("Event", "Stream", "Context", "Device"):
        if family in name:
            return family.casefold()
    return "destroy_other"


def _cleanup_families(text: str) -> dict[str, int]:
    counts = Counter(
        _cleanup_family(match.group(1))
        for match in CLEANUP_CALL.finditer(_without_comments(text))
    )
    return dict(sorted(counts.items()))


def _cleanup_binding_family(name: str) -> str:
    if name == "aclrtFreeHost":
        return "free_host"
    if name == "aclrtFree":
        return "free_device"
    if name == "aclFinalize":
        return "finalize"
    if name.startswith("aclrtDestroy"):
        return "destroy_" + name[len("aclrtDestroy") :].casefold()
    if name.startswith("aclrtReset"):
        return "reset_" + name[len("aclrtReset") :].casefold()
    return name


def _cleanup_bindings(text: str) -> list[str]:
    values: list[str] = []
    for call in _calls(_without_comments(text)):
        if not CLEANUP_CALL.fullmatch(call["name"] + "(") or not call["arguments"]:
            continue
        owner = _identifier(call["arguments"][0]) or _normalize(call["arguments"][0])
        values.append(f"{_cleanup_binding_family(call['name'])}:{owner}")
    return values


def _allocation_owners(text: str) -> dict[str, str]:
    owners: dict[str, str] = {}
    for call in _calls(_without_comments(text)):
        expected = ALLOCATORS.get(call["name"])
        if expected and call["arguments"]:
            owner = _identifier(call["arguments"][0], require_address=True)
            if owner:
                owners[owner] = (
                    "free_host" if expected[0] == "aclrtFreeHost" else "free_device"
                )
    return owners


def _cleanup_mutation_is_safe(
    baseline: dict[str, Any], candidate: dict[str, Any]
) -> bool:
    before = Counter(baseline.get("cleanup_bindings", []))
    after = Counter(candidate.get("cleanup_bindings", []))
    removed: list[tuple[str, int]] = []
    for binding, count in before.items():
        delta = count - after.get(binding, 0)
        if delta > 0:
            removed.append((binding, delta))
    if not removed:
        return True
    candidate_bindings = set(after)
    for binding, delta in removed:
        family, owner = binding.split(":", 1)
        # A repeated identical release is statically removable when one copy
        # remains.  This is the only generic duplicate proof available here.
        if before[binding] > 1 and after.get(binding, 0) > 0:
            continue
        expected = baseline.get("allocation_owners", {}).get(owner)
        if (
            expected
            and family != expected
            and f"{expected}:{owner}" in candidate_bindings
        ):
            # The allocator family proves this was the wrong paired release.
            continue
        return False
    return True


def _event_record_wait_proven(text: str) -> bool:
    # 查询调用本身不证明完成；此检查不推断查询后的控制流程。
    calls = _calls(_without_comments(text))
    wait_names = {
        "aclrtSynchronizeEvent",
        "aclrtSynchronizeEventWithTimeout",
    }
    for wait in calls:
        if wait["name"] not in wait_names or not wait["arguments"]:
            continue
        if (
            wait["name"] == "aclrtSynchronizeEventWithTimeout"
            and len(wait["arguments"]) > 1
        ):
            timeout = wait["arguments"][1].strip()
            if timeout in {"0", "0U", "0u", "0L", "0l"}:
                continue
        event = _identifier(wait["arguments"][0])
        if not event:
            continue
        if any(
            call["name"] in EVENT_RECORDERS
            and call["position"] < wait["position"]
            and call["arguments"]
            and _identifier(call["arguments"][0]) == event
            for call in calls
        ):
            return True
    return False


def _carrier_names(text: str) -> set[str]:
    clean = STRING_LITERAL.sub('""', _without_comments(text))
    pattern = re.compile(
        r"(?m)^[ \t]*[^\n;{}=]*\b(?:static|inline)\b[^\n;{}=]*?"
        r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?\{"
    )
    return {
        match.group(1) for match in pattern.finditer(clean) if match.group(1) != "main"
    }


def _output_binding_types(text: str) -> dict[str, str]:
    declared = _declared_types(text)
    bindings: dict[str, str] = {}
    for call in _calls(_without_comments(text)):
        for index, _role, _predicate, _expected in OUTPUT_CONTRACTS.get(
            call["name"], ()
        ):
            if index >= len(call["arguments"]):
                continue
            name = _identifier(call["arguments"][index], require_address=True)
            if name and name in declared:
                bindings[f"{call['name']}:{index}"] = "|".join(
                    sorted(_canonical_type(item) for item in declared[name])
                )
    return bindings


def _output_binding_names(text: str) -> dict[str, str]:
    names: dict[str, str] = {}
    for call in _calls(_without_comments(text)):
        for index, _role, _predicate, _expected in OUTPUT_CONTRACTS.get(
            call["name"], ()
        ):
            if index >= len(call["arguments"]):
                continue
            name = _identifier(call["arguments"][index], require_address=True)
            if name:
                names[f"{call['name']}:{index}"] = name
    return names


def _output_declared_names(text: str) -> dict[str, str]:
    return {
        name: "|".join(sorted(_canonical_type(value) for value in values))
        for name, values in _declared_types(text).items()
    }


def _is_protected_file(relative: Path) -> bool:
    parts = [part.casefold() for part in relative.parts]
    if not parts:
        return False
    if parts[-1] in PROTECTED_FILE_BASENAMES:
        return True
    return any(marker in part for part in parts for marker in PROTECTED_FILE_MARKERS)


def _is_ignored_dir(relative: Path) -> bool:
    return any(
        part in IGNORED_DIR_NAMES
        or part == "build"
        or part.startswith("build-")
        or part.startswith("cmake-build-")
        for part in relative.parts
    )


@dataclass(frozen=True)
class _WorkspaceFile:
    path: Path
    relative: str
    stat: os.stat_result


def _secure_flags(*, directory: bool) -> int:
    if not hasattr(os, "O_NOFOLLOW"):
        raise OSError(errno.ENOTSUP, "secure no-follow open is unavailable")
    flags = os.O_RDONLY | os.O_NOFOLLOW
    if directory:
        if (
            not hasattr(os, "O_DIRECTORY")
            or os.open not in getattr(os, "supports_dir_fd", set())
            or os.stat not in getattr(os, "supports_dir_fd", set())
            or os.stat not in getattr(os, "supports_follow_symlinks", set())
        ):
            raise OSError(errno.ENOTSUP, "secure directory open is unavailable")
        flags |= os.O_DIRECTORY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    return flags


def _same_identity(before: os.stat_result, after: os.stat_result) -> bool:
    return (
        before.st_dev == after.st_dev
        and before.st_ino == after.st_ino
        and stat.S_IFMT(before.st_mode) == stat.S_IFMT(after.st_mode)
    )


def _read_open_fd_bounded(fd: int, *, max_bytes: int) -> bytes:
    before = os.fstat(fd)
    if not stat.S_ISREG(before.st_mode):
        raise ValueError("file must be a regular file")
    if before.st_size > max_bytes:
        raise ValueError(f"file exceeds {max_bytes} bytes")

    chunks = bytearray()
    while True:
        remaining = max_bytes + 1 - len(chunks)
        if remaining <= 0:
            raise ValueError(f"file exceeds {max_bytes} bytes")
        chunk = os.read(fd, min(64 * 1024, remaining))
        if not chunk:
            break
        chunks.extend(chunk)
        if len(chunks) > max_bytes:
            raise ValueError(f"file exceeds {max_bytes} bytes")

    after = os.fstat(fd)
    if (
        not _same_identity(before, after)
        or before.st_size != after.st_size
        or before.st_mtime_ns != after.st_mtime_ns
        or before.st_ctime_ns != after.st_ctime_ns
        or len(chunks) != before.st_size
    ):
        raise ValueError("file changed while reading")
    return bytes(chunks)


def _canonical_relative_parts(value: str) -> tuple[str, ...]:
    if not isinstance(value, str) or not value:
        raise ValueError("source_relative must use a canonical POSIX relative path")
    if value.startswith("/") or value.endswith("/") or "\\" in value or "//" in value:
        raise ValueError("source_relative must use a canonical POSIX relative path")
    parts = tuple(value.split("/"))
    if any(part in {"", ".", ".."} for part in parts):
        raise ValueError("source_relative must use a canonical POSIX relative path")
    return parts


def _open_regular_no_follow(
    path: Path,
    *,
    max_bytes: int,
    expected: os.stat_result | None = None,
) -> int:
    current = os.lstat(path)
    if stat.S_ISLNK(current.st_mode):
        raise ValueError(f"file must not be a symlink: {path}")
    if not stat.S_ISREG(current.st_mode):
        raise ValueError(f"file must be a regular file: {path}")
    if expected is not None and not _same_identity(expected, current):
        raise ValueError("file changed while opening")
    if current.st_size > max_bytes:
        raise ValueError(f"file exceeds {max_bytes} bytes: {path}")

    flags = _secure_flags(directory=False)
    try:
        fd = os.open(path, flags)
    except OSError as error:
        if error.errno in {errno.ELOOP, errno.ENOTDIR}:
            raise ValueError("file changed while opening") from error
        raise
    try:
        if not hasattr(os, "O_CLOEXEC"):
            os.set_inheritable(fd, False)
        opened = os.fstat(fd)
        if not _same_identity(current, opened):
            raise ValueError("file changed while opening")
        if expected is not None and not _same_identity(expected, opened):
            raise ValueError("file changed while opening")
        if not stat.S_ISREG(opened.st_mode):
            raise ValueError(f"file must be a regular file: {path}")
        if opened.st_size > max_bytes:
            raise ValueError(f"file exceeds {max_bytes} bytes: {path}")
        return fd
    except BaseException:
        os.close(fd)
        raise


def _open_root_dir(root: Path) -> tuple[int, os.stat_result]:
    expected = os.lstat(root)
    if stat.S_ISLNK(expected.st_mode) or not stat.S_ISDIR(expected.st_mode):
        raise ValueError("workspace root must be a non-symlink directory")
    flags = _secure_flags(directory=True)
    try:
        fd = os.open(root, flags)
    except OSError as error:
        if error.errno in {errno.ELOOP, errno.ENOTDIR}:
            raise ValueError("workspace root changed while opening") from error
        raise
    try:
        if not hasattr(os, "O_CLOEXEC"):
            os.set_inheritable(fd, False)
        opened = os.fstat(fd)
        if not _same_identity(expected, opened) or not stat.S_ISDIR(opened.st_mode):
            raise ValueError("workspace root changed while opening")
        return fd, opened
    except BaseException:
        os.close(fd)
        raise


def _open_relative_regular(
    root_fd: int,
    parts: tuple[str, ...],
    *,
    expected: os.stat_result | None,
) -> int:
    if not parts:
        raise ValueError("source_relative must name a regular file")

    final_fd: int | None = None
    try:
        with contextlib.ExitStack() as stack:
            parent_fd = root_fd
            for index, part in enumerate(parts):
                before = os.stat(part, dir_fd=parent_fd, follow_symlinks=False)
                is_final = index == len(parts) - 1
                if stat.S_ISLNK(before.st_mode):
                    raise ValueError(f"workspace path must not be a symlink: {part}")

                if is_final:
                    if not stat.S_ISREG(before.st_mode):
                        raise ValueError(
                            "workspace source_relative must name a regular file"
                        )
                    if expected is not None and not _same_identity(expected, before):
                        raise ValueError("file changed while opening")
                    flags = _secure_flags(directory=False)
                else:
                    if not stat.S_ISDIR(before.st_mode):
                        raise ValueError(
                            f"workspace parent must be a directory: {part}"
                        )
                    flags = _secure_flags(directory=True)

                try:
                    fd = os.open(part, flags, dir_fd=parent_fd)
                except OSError as error:
                    if error.errno in {errno.ELOOP, errno.ENOTDIR}:
                        raise ValueError("file changed while opening") from error
                    raise
                try:
                    if not hasattr(os, "O_CLOEXEC"):
                        os.set_inheritable(fd, False)
                    opened = os.fstat(fd)
                    if not _same_identity(before, opened):
                        raise ValueError("file changed while opening")
                    if is_final:
                        if expected is not None and not _same_identity(
                            expected, opened
                        ):
                            raise ValueError("file changed while opening")
                        if not stat.S_ISREG(opened.st_mode):
                            raise ValueError(
                                "workspace source_relative must name a regular file"
                            )
                        final_fd = fd
                        break
                    if not stat.S_ISDIR(opened.st_mode):
                        raise ValueError(
                            f"workspace parent must be a directory: {part}"
                        )
                except BaseException:
                    os.close(fd)
                    raise

                stack.callback(os.close, fd)
                parent_fd = fd
    except BaseException:
        if final_fd is not None:
            os.close(final_fd)
        raise

    if final_fd is None:
        raise ValueError("workspace source_relative must name a regular file")
    return final_fd


def _workspace_files(root: Path) -> tuple[_WorkspaceFile, ...]:
    root_info = root.lstat()
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise ValueError("workspace root must be a non-symlink directory")
    files: list[_WorkspaceFile] = []

    def raise_walk_error(error: OSError) -> None:
        raise error

    for current_text, dirnames, filenames in os.walk(
        root, topdown=True, followlinks=False, onerror=raise_walk_error
    ):
        current = Path(current_text)
        retained_dirs: list[str] = []
        for name in dirnames:
            path = current / name
            relative = path.relative_to(root)
            if _is_ignored_dir(relative):
                continue
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                if (
                    relative.suffix.casefold() in WORKSPACE_SOURCE_SUFFIXES
                    or _is_protected_file(relative)
                ):
                    raise ValueError(
                        f"relevant workspace path must not be a symlink: {relative}"
                    )
                continue
            if stat.S_ISDIR(metadata.st_mode):
                retained_dirs.append(name)
        dirnames[:] = retained_dirs

        for name in filenames:
            path = current / name
            relative = path.relative_to(root)
            is_source = relative.suffix.casefold() in WORKSPACE_SOURCE_SUFFIXES
            is_protected = _is_protected_file(relative)
            if not is_source and not is_protected:
                continue
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                raise ValueError(
                    f"relevant workspace path must not be a symlink: {relative}"
                )
            if not stat.S_ISREG(metadata.st_mode):
                raise ValueError(
                    f"relevant workspace path must be a regular file: {relative}"
                )
            files.append(_WorkspaceFile(path, relative.as_posix(), metadata))

    return tuple(sorted(files, key=lambda record: record.relative))


def _read_bounded(
    path: Path,
    *,
    root: Path | None,
    max_bytes: int,
    expected: os.stat_result | None = None,
) -> bytes:
    if root is None:
        fd = _open_regular_no_follow(path, max_bytes=max_bytes, expected=expected)
        try:
            return _read_open_fd_bounded(fd, max_bytes=max_bytes)
        finally:
            os.close(fd)

    try:
        relative = path.relative_to(root).as_posix()
    except ValueError as error:
        raise ValueError(f"file escapes workspace root: {path}") from error
    parts = _canonical_relative_parts(relative)
    with contextlib.ExitStack() as stack:
        root_fd, _ = _open_root_dir(root)
        stack.callback(os.close, root_fd)
        file_fd = _open_relative_regular(root_fd, parts, expected=expected)
        stack.callback(os.close, file_fd)
        return _read_open_fd_bounded(file_fd, max_bytes=max_bytes)


def _protected_file_hashes(
    root: Path,
    files: tuple[_WorkspaceFile, ...] | None = None,
) -> dict[str, str]:
    hashes: dict[str, str] = {}
    if files is None:
        files = _workspace_files(root)
    for record in files:
        relative = Path(record.relative)
        if not _is_protected_file(relative):
            continue
        data = _read_bounded(
            record.path,
            root=root,
            max_bytes=MAX_PROTECTED_BYTES,
            expected=record.stat,
        )
        hashes[record.relative] = _sha256(data)
    return hashes


def _workspace_source(
    root: Path,
    files: tuple[_WorkspaceFile, ...] | None = None,
) -> _WorkspaceFile:
    if files is None:
        files = _workspace_files(root)
    candidates = [
        record
        for record in files
        if Path(record.relative).suffix.casefold() in SOURCE_SUFFIXES
    ]
    root_mains = [
        record
        for record in candidates
        if len(Path(record.relative).parts) == 1
        and Path(record.relative).stem == "main"
    ]
    if len(root_mains) == 1:
        return root_mains[0]
    if len(root_mains) > 1:
        raise ValueError("workspace has multiple root main source files")
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise ValueError(f"workspace has no supported Host source file: {root}")
    raise ValueError(
        "workspace has ambiguous Host source files without a unique root main.*"
    )


def _source_file_hashes(
    root: Path, files: tuple[_WorkspaceFile, ...]
) -> dict[str, str]:
    return {
        record.relative: _sha256(
            _read_bounded(
                record.path,
                root=root,
                max_bytes=MAX_SOURCE_BYTES,
                expected=record.stat,
            )
        )
        for record in files
        if Path(record.relative).suffix.casefold() in WORKSPACE_SOURCE_SUFFIXES
    }


def _surface(text: str) -> dict[str, Any]:
    clean = _without_comments(text)
    return {
        "observations": _observation_calls(clean),
        "markers": _marker_lines(clean),
        "returns": _protected_returns(clean),
        "protected_function_signatures": _protected_function_signatures(clean),
        "verification_predicates": _verification_predicates(clean),
        "runtime_names": _runtime_names(clean),
        "runtime_dependencies": sorted(_runtime_dependencies(clean)),
        "sleep_calls": _sleep_calls(clean),
        "cleanup_families": _cleanup_families(clean),
        "cleanup_bindings": _cleanup_bindings(clean),
        "allocation_owners": _allocation_owners(clean),
        "carriers": sorted(_carrier_names(clean)),
        "output_bindings": _output_binding_types(clean),
        "output_binding_names": _output_binding_names(clean),
        "declared_types": _output_declared_names(clean),
        "runtime_calls": _runtime_call_surfaces(clean),
    }


def _checked_snapshot_source(root: Path, source_relative: str) -> tuple[str, ...]:
    return _canonical_relative_parts(source_relative)


def make_snapshot(source: Path | str) -> dict[str, Any]:
    path = Path(source)
    metadata = path.lstat()
    if stat.S_ISLNK(metadata.st_mode):
        raise ValueError("source or workspace must not be a symlink")
    if stat.S_ISDIR(metadata.st_mode):
        workspace = path
        files = _workspace_files(workspace)
        source_record = _workspace_source(workspace, files)
        source_path = source_record.path
        data = _read_bounded(
            source_path,
            root=workspace,
            max_bytes=MAX_SOURCE_BYTES,
            expected=source_record.stat,
        )
        protected_files = _protected_file_hashes(workspace, files)
        source_files = _source_file_hashes(workspace, files)
        if source_files[source_record.relative] != _sha256(data):
            raise ValueError("main source changed while making workspace snapshot")
    elif stat.S_ISREG(metadata.st_mode):
        workspace = None
        source_path = path
        if path.suffix.casefold() not in SOURCE_SUFFIXES:
            raise ValueError(f"unsupported source suffix: {path.suffix or '<none>'}")
        data = _read_bounded(
            source_path,
            root=None,
            max_bytes=MAX_SOURCE_BYTES,
            expected=None,
        )
        protected_files = {}
    else:
        raise ValueError("source must be a regular file or workspace directory")
    text = data.decode("utf-8", errors="strict")
    result = {
        "schema_version": SCHEMA_VERSION,
        "source": str(path),
        "sha256": _sha256(data),
        "surface": _surface(text),
        "runtime_call_records": _runtime_call_records(text),
        "declared_types": {
            name: sorted(values) for name, values in _declared_types(text).items()
        },
    }
    if workspace is not None:
        result.update(
            {
                "source_kind": "workspace",
                "source_relative": source_record.relative,
                "protected_files": protected_files,
                "source_files": source_files,
            }
        )
    else:
        result.update({"source_kind": "source", "protected_files": protected_files})
    return result


def _check_output_types(before: dict[str, Any], after: dict[str, Any]) -> bool:
    before_decls = before["declared_types"]
    for binding, candidate_type in after["output_bindings"].items():
        if (
            binding in before["output_bindings"]
            and candidate_type != before["output_bindings"][binding]
        ):
            return False
        if binding not in before["output_bindings"]:
            # A newly bound output must use an already declared variable whose
            # type was stable in the baseline; otherwise the repair invents a
            # type while changing the call.
            name = after.get("output_binding_names", {}).get(binding)
            if name not in before_decls:
                return False
            if candidate_type != before_decls[name]:
                return False
    return True


def check_snapshot(snapshot: dict[str, Any], source: Path | str) -> dict[str, Any]:
    path = Path(source)
    if snapshot.get("source_kind") == "workspace":
        parts = _checked_snapshot_source(path, snapshot.get("source_relative"))
        source_relative = "/".join(parts)
        source_path = path.joinpath(*parts)
        candidate_bytes = _read_bounded(
            source_path,
            root=path,
            max_bytes=MAX_SOURCE_BYTES,
            expected=None,
        )
        candidate_text = candidate_bytes.decode("utf-8", errors="strict")
        baseline_sources = snapshot.get("source_files")
        if not isinstance(baseline_sources, dict) or baseline_sources.get(
            source_relative
        ) != snapshot.get("sha256"):
            raise ValueError(
                "workspace snapshot lacks complete source_files; remake the snapshot"
            )
        candidate = _surface(candidate_text)
        candidate_files = _workspace_files(path)
        candidate_sources = _source_file_hashes(path, candidate_files)
        if candidate_sources.get(source_relative) != _sha256(candidate_bytes):
            raise ValueError("main source changed while checking workspace snapshot")
        source_scope_preserved = set(candidate_sources) == set(
            baseline_sources
        ) and all(
            digest == baseline_sources[relative]
            for relative, digest in candidate_sources.items()
            if relative != source_relative
        )
        candidate_protected = _protected_file_hashes(path, candidate_files)
        protected_unchanged = candidate_protected == snapshot.get("protected_files", {})
    else:
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError("candidate source must not be a symlink")
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("candidate source must be a regular file")
        if path.suffix.casefold() not in SOURCE_SUFFIXES:
            raise ValueError(f"unsupported source suffix: {path.suffix or '<none>'}")
        source_path = path
        candidate_bytes = _read_bounded(
            source_path,
            root=None,
            max_bytes=MAX_SOURCE_BYTES,
            expected=None,
        )
        candidate_text = candidate_bytes.decode("utf-8", errors="strict")
        candidate = _surface(candidate_text)
        protected_unchanged = True
        source_scope_preserved = True
    baseline = snapshot["surface"]
    baseline_runtime_names = baseline["runtime_names"]
    candidate_runtime_names = candidate["runtime_names"]
    baseline_records = snapshot.get("runtime_call_records", [])
    candidate_records = _runtime_call_records(candidate_text)
    baseline_declarations = {
        name: set(values) for name, values in snapshot.get("declared_types", {}).items()
    }
    argument_edits, anchors_stable = _runtime_argument_edits(
        baseline_records, candidate_records, baseline_declarations, candidate_text
    )
    baseline_cleanup = baseline["cleanup_families"]
    candidate_cleanup = candidate["cleanup_families"]
    cleanup_mutation_allowed = _cleanup_mutation_is_safe(baseline, candidate)
    baseline_timing_names = [
        name for name in baseline_runtime_names if _is_timing_runtime_name(name)
    ]
    candidate_timing_names = [
        name for name in candidate_runtime_names if _is_timing_runtime_name(name)
    ]
    removed_timing = Counter(baseline_timing_names) - Counter(candidate_timing_names)
    sleep_replacement_allowed = not (
        baseline.get("sleep_calls", []) and removed_timing
    ) or _event_record_wait_proven(candidate_text)
    checks = {
        "observation_calls_unchanged": candidate["observations"]
        == baseline["observations"],
        "verifier_markers_unchanged": candidate["markers"] == baseline["markers"],
        "return_semantics_unchanged": candidate["returns"] == baseline["returns"],
        "protected_function_signatures_unchanged": candidate[
            "protected_function_signatures"
        ]
        == baseline.get("protected_function_signatures", []),
        "verification_predicates_unchanged": candidate["verification_predicates"]
        == baseline.get("verification_predicates"),
        # Runtime API additions/removals are allowed only on the business
        # synchronization/wait/event/release surface.  Other call names keep
        # their order and identity, so cross-domain changes still fail closed.
        "runtime_calls_preserved": _immutable_runtime_names(candidate_runtime_names)
        == _immutable_runtime_names(baseline_runtime_names),
        "runtime_argument_edit_scope": len(argument_edits) <= 1,
        "runtime_argument_edits_traceable": all(
            item["traceable"] for item in argument_edits
        ),
        "runtime_argument_anchors_stable": anchors_stable,
        "cleanup_mutations_allowed": cleanup_mutation_allowed,
        # Kept as a compatibility key for consumers of guard/2.0.  It now
        # means that cleanup changes stay on the explicitly mutable surface.
        "cleanup_families_preserved": cleanup_mutation_allowed,
        "root_carrier_retained": set(baseline["carriers"]).issubset(
            set(candidate["carriers"])
        ),
        "output_binding_types_unchanged": _check_output_types(baseline, candidate),
        "sleep_calls_unchanged": candidate["sleep_calls"]
        == baseline.get("sleep_calls", []),
        "sleep_sync_boundary_preserved": sleep_replacement_allowed,
        "protected_files_unchanged": protected_unchanged,
        "source_files_within_scope": source_scope_preserved,
    }
    checks["immutable_verification_surface_unchanged"] = all(
        checks[name]
        for name in (
            "observation_calls_unchanged",
            "verifier_markers_unchanged",
            "return_semantics_unchanged",
            "protected_function_signatures_unchanged",
            "verification_predicates_unchanged",
            "output_binding_types_unchanged",
            "protected_files_unchanged",
            "source_files_within_scope",
        )
    )
    checks["runtime_semantics_changed"] = (
        candidate["runtime_calls"] != baseline["runtime_calls"]
        or candidate_cleanup != baseline_cleanup
    )
    checks["candidate_hash_changed"] = snapshot.get("sha256") != _sha256(
        candidate_bytes
    )
    required = [
        "observation_calls_unchanged",
        "verifier_markers_unchanged",
        "return_semantics_unchanged",
        "protected_function_signatures_unchanged",
        "verification_predicates_unchanged",
        "runtime_calls_preserved",
        "runtime_argument_edit_scope",
        "runtime_argument_edits_traceable",
        "runtime_argument_anchors_stable",
        "cleanup_mutations_allowed",
        "root_carrier_retained",
        "output_binding_types_unchanged",
        "sleep_calls_unchanged",
        "sleep_sync_boundary_preserved",
        "protected_files_unchanged",
        "source_files_within_scope",
        "runtime_semantics_changed",
        "candidate_hash_changed",
    ]
    errors = [
        {
            "rule": name,
            "message": {
                "observation_calls_unchanged": "observation calls were changed",
                "verifier_markers_unchanged": "verification/data/resource markers were changed",
                "return_semantics_unchanged": "protected verifier exit semantics were changed",
                "protected_function_signatures_unchanged": "a protected call target's qualified parameter signature or overload set was changed",
                "verification_predicates_unchanged": "verification predicate declarations or statements were changed",
                "runtime_calls_preserved": "a non-repairable Runtime API was added or removed",
                "runtime_argument_edit_scope": "more than one Runtime call argument was replaced in one patch",
                "runtime_argument_edits_traceable": "a Runtime argument replacement lacks a candidate declaration/type trace",
                "runtime_argument_anchors_stable": "a non-repairable Runtime call lost its source anchor",
                "cleanup_mutations_allowed": "all cleanup responsibility was removed in one patch",
                "root_carrier_retained": "a local Runtime carrier/helper was removed",
                "output_binding_types_unchanged": "an output binding type was changed or invented",
                "sleep_calls_unchanged": "sleep was added or used as a synchronization substitute",
                "sleep_sync_boundary_preserved": "an existing sleep remains after removing a synchronization/wait boundary without a proven Event Record-to-Wait dependency",
                "protected_files_unchanged": "TASK/CMake/Golden/verifier assets were changed",
                "source_files_within_scope": "a source file was added, removed, or changed outside the main repair source",
                "runtime_semantics_changed": "candidate contains no Runtime semantic change",
                "candidate_hash_changed": "candidate is byte-identical to the baseline",
            }[name],
        }
        for name in required
        if not checks[name]
    ]
    return {
        "schema_version": SCHEMA_VERSION,
        "valid": not errors,
        "source": str(path),
        "errors": errors,
        "warnings": [
            "开发文本门禁不证明任意 C++ 程序语义；正式判官须在模型工作区外独立执行。"
        ],
        "checks": checks,
        "runtime_argument_edits": argument_edits,
        "baseline_sha256": snapshot.get("sha256"),
        "candidate_sha256": _sha256(candidate_bytes),
        "next_action": "accept_candidate_then_run_independent_verification"
        if not errors
        else "reject_candidate_and_preserve_verifier",
    }


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before_pos", nargs="?", type=Path)
    parser.add_argument("after_pos", nargs="?", type=Path)
    parser.add_argument("--before", dest="before_opt", type=Path)
    parser.add_argument("--after", dest="after_opt", type=Path)
    parser.add_argument("--format", choices=("json", "text"), default="json")
    parser.add_argument("--compact", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    before = args.before_opt or args.before_pos
    after = args.after_opt or args.after_pos
    if before is None or after is None:
        raise SystemExit("before and after source paths are required")
    try:
        result = check_snapshot(make_snapshot(before), after)
    except (OSError, UnicodeError, ValueError) as error:
        print(
            json.dumps(
                {"schema_version": SCHEMA_VERSION, "valid": False, "error": str(error)}
            )
        )
        return 2
    if args.format == "text":
        print(f"{'PASS' if result['valid'] else 'FAIL'} {result['source']}")
        for item in result["errors"]:
            print(f"ERROR:{item['rule']}: {item['message']}")
    else:
        print(
            json.dumps(
                result,
                ensure_ascii=False,
                indent=None if args.compact else 2,
                sort_keys=True,
                separators=(",", ":") if args.compact else None,
            )
        )
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
