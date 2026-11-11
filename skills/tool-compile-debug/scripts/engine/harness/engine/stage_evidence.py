# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Bounded evidence for build stage, current source domain and path hints.

No classification/gold mutation, compiler execution or additional source reads.
Source domains describe a directly bound current source snapshot, never a
historical translation unit or an internal compiler pass.
"""

from __future__ import annotations

import hashlib
import posixpath
import re
import shlex
from pathlib import Path
from typing import Any

from harness.engine.common import resolve_project_source

_MAX_SOURCE_CHARS = 1_000_000
_DRIVER = re.compile(
    r"^(?:[^\s:]+[/\\])?(?P<tool>bisheng|ccec|clang(?:\+\+)?(?:-\d+)?|gcc|g\+\+)"
    r":\s*(?:fatal )?error:\s*(?:"
    r"unsupported (?:npu )?(?:architecture|arch|soc)|unknown (?:argument|option)|"
    r"unrecognized command.line option|invalid (?:argument|value)|no input files)",
    re.I,
)
_CONFIGURE = re.compile(r"^(?:--\s*)?Configuring incomplete, errors occurred!?$", re.I)
_GENERATE = re.compile(r"^CMake Generate step failed\.", re.I)
_BOUNDARY = re.compile(
    r"^(?:--\s*(?:Configuring done|Generating done|Build files have been written)|"
    r"\$\s*\S|\[[^\]]+\]\s*(?:Building|Linking|Built target)|"
    r"(?:[^\s]+[/\\])?(?:cmake|make|ninja|clang\+\+|clang|bisheng|ccec)\s+-)",
    re.I,
)
_LITERAL = re.compile(
    r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
    re.S,
)
_FUNCTION = re.compile(
    r"\s*(?P<qual>(?:(?:extern|static|inline|constexpr|__global__|__aicore__|"
    r"__device__|__host__)\s+)*)"
    r"(?P<ret>void|int|auto|bool|float|double|half|size_t|u?int(?:8|16|32|64)_t)"
    r"\s+(?P<name>[A-Za-z_]\w*)\s*\((?P<params>[^(){};\[\]#]*)\)\s*"
    r"(?:noexcept\s*)?$"
)


def stage_observation(line: str, log_line: int) -> dict[str, Any] | None:
    """Capture only explicit tool diagnostics, in normalized-log line space."""
    value, basis = None, None
    if _DRIVER.match(line):
        value, basis = "driver", "driver_diagnostic"
    elif _CONFIGURE.match(line):
        value, basis = "configure", "configure_failure_marker"
    elif _GENERATE.match(line):
        value, basis = "generate", "generate_failure_marker"
    elif _BOUNDARY.match(line):
        value, basis = "boundary", "build_or_command_boundary"
    if value:
        return {
            "value": value,
            "basis": basis,
            "log_line": log_line,
            "log_line_space": "normalized",
            "detail": line[:240],
        }
    return None


def _unknown(reason: str) -> dict[str, Any]:
    return {"value": "unknown", "basis": "none", "blockers": [reason]}


def _compatible_driver_crash(driver: dict, crash: str) -> bool:
    """Do not attribute a different compiler's adjacent crash to this driver."""
    matched = _DRIVER.match(driver.get("detail") or "")
    if not matched:
        return False
    tool = matched.group("tool").lower()
    if crash.startswith("LLVM ERROR:"):
        return tool not in {"gcc", "g++"}
    return bool(re.match(r"^(?:[^\s:]+[/\\])?" + re.escape(tool) + r":", crash, re.I))


def _stage(primary: dict, normalized: dict) -> dict:
    raw = str(primary.get("raw") or "")
    message = str(primary.get("message") or "")
    kind = primary.get("kind")
    position = primary.get("log_line")
    direct = stage_observation(raw, position) if isinstance(position, int) else None
    if direct and direct["value"] != "boundary":
        return direct
    value, basis = None, None
    if kind == "link_error":
        value, basis = "link", "linker_diagnostic"
    elif kind == "build_rule_error":
        value, basis = "build", "build_rule_diagnostic"
    elif (
        kind in {"error", "fatal_error"}
        and primary.get("file")
        and isinstance(primary.get("line"), int)
    ):
        if kind == "fatal_error" and re.search(
            r"^(?:['\"<].+?['\">] file not found|[^:]+: No such file or directory)$",
            message,
            re.I,
        ):
            value, basis = "preprocessing", "missing_include_diagnostic"
        else:
            value, basis = "compilation", "source_diagnostic"
    elif kind == "cmake_error":
        # CMake(message) may wrap a compiler or custom script failure. A
        # failed configure/generate marker is needed before assigning a stage.
        markers = [
            x
            for x in normalized.get("stage_observations", [])
            if x.get("value") in {"configure", "generate"}
            and isinstance(position, int)
            and isinstance(x.get("log_line"), int)
            and 0 <= x["log_line"] - position <= 12
            and not any(
                isinstance(e.get("log_line"), int)
                and position < e["log_line"] < x["log_line"]
                for e in normalized.get("all_errors", [])
            )
            and not any(
                e.get("value") == "boundary"
                and position < e["log_line"] < x["log_line"]
                for e in normalized.get("stage_observations", [])
            )
        ]
        if len({x["value"] for x in markers}) == 1:
            return dict(markers[0])
    elif kind == "package_error" and re.match(
        r"(?:msobjdump:|.*\bpackage failed\b)", raw, re.I
    ):
        value, basis = "packaging", "packaging_diagnostic"
    # An earlier explicit invalid driver option can precede an LLVM abort.
    # Do not replace a located source/link diagnostic with another TU's event.
    if (
        value is None
        and kind == "compiler_internal"
        and not primary.get("file")
        and isinstance(position, int)
    ):
        preceding = [
            x
            for x in normalized.get("stage_observations", [])
            if x.get("value") == "driver"
            and isinstance(x.get("log_line"), int)
            and position - x["log_line"] == 1
            and _compatible_driver_crash(x, raw)
            and not any(
                isinstance(e.get("log_line"), int)
                and x["log_line"] < e["log_line"] < position
                for e in normalized.get("all_errors", [])
            )
        ]
        if preceding:
            return dict(preceding[-1])
    if value:
        return {
            "value": value,
            "basis": basis,
            "log_line": position,
            "log_line_space": "normalized",
            "detail": raw[:240],
        }
    return _unknown("no_direct_stage_evidence")


def _visible_source(text: str) -> str | None:
    """Mask literals/comments, retaining offsets; reject incomplete lexemes."""
    out, i = [], 0
    while i < len(text):
        if text.startswith(('R"', "//", "/*"), i) or text[i] in "\"'":
            m = _LITERAL.match(text, i)
            if not m:
                return None
            out.append("".join("\n" if c == "\n" else " " for c in m.group()))
            i = m.end()
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _source_domain(primary: dict, context: dict) -> dict:
    raw, line = primary.get("file"), primary.get("line")
    if (
        not isinstance(raw, str)
        or not isinstance(line, int)
        or isinstance(line, bool)
        or line < 1
    ):
        return _unknown("no_source_locus")
    if (
        primary.get("file_resolved") is False
        or primary.get("file_reanchored") is True
        or (context.get("source_coverage") or {}).get("failed_source_reanchored")
    ):
        return _unknown("unbound_or_reanchored_source")
    texts = context.get("file_text") or {}
    if not isinstance(texts, dict):
        return _unknown("source_not_loaded")
    if context.get("case_dir"):
        root = Path(context["case_dir"]).resolve()
        resolved = resolve_project_source(raw, root, reanchor=False)
        if resolved is None:
            return _unknown("source_not_owned")
        key = resolved.relative_to(root).as_posix()
    else:
        key = raw.replace("\\", "/")
        if key.startswith("/") or ":" in key or "$" in key or ".." in key.split("/"):
            return _unknown("source_not_owned")
        key = posixpath.normpath(key)
    text = texts.get(key)
    if not isinstance(text, str):
        return _unknown("source_not_loaded")
    if len(text) > _MAX_SOURCE_CHARS:
        return _unknown("source_scan_limit")
    lines = text.splitlines(keepends=True)
    if line > len(lines):
        return _unknown("source_locus_out_of_range")
    echo = primary.get("code_line")
    if (
        isinstance(echo, str)
        and echo.strip()
        and echo.strip() != lines[line - 1].strip()
    ):
        return _unknown("diagnostic_echo_differs_from_current_source")
    visible = _visible_source(text)
    if visible is None:
        return _unknown("incomplete_source_lexing")
    if re.search(
        r"^\s*#\s*(?:if|ifdef|ifndef|elif|else|endif|define|undef|line)\b",
        visible,
        re.M,
    ):
        return _unknown("preprocessor_context_unresolved")
    # Line splicing and templates, lambdas, nested scopes or malformed braces
    # are outside this small source reader; callers can inspect/preprocess them.
    if re.search(r"\\\r?\n", text) or re.search(r"\btemplate\s*<", visible):
        return _unknown("complex_source_syntax")
    # Existing context can refute lexical attributes/main. This is not a
    # preprocessor or proof that the uncollected build environment has no macros.
    names = r"(?:main|__aicore__|__device__|__host__)"
    macro = re.compile(r"^[ \t]*#[ \t]*(?:define|undef)[ \t]+" + names + r"\b", re.M)
    for other_key, other_text in texts.items():
        if other_key == key or not isinstance(other_text, str):
            continue
        if macro.search(other_text):
            return _unknown("context_attribute_macro_unresolved")
    commands = (context.get("build") or {}).get("compile_commands") or []
    for command in commands:
        if not isinstance(command, dict):
            continue
        args = command.get("arguments")
        if not isinstance(args, list):
            try:
                args = shlex.split(str(command.get("command") or ""), posix=True)
            except ValueError:
                return _unknown("command_syntax_unresolved")
        if re.search(
            r"(?:^|\s)-[DU]\s*" + names + r"\b", " ".join(str(a) for a in args)
        ):
            return _unknown("command_attribute_macro_unresolved")
    # Includes and pragmas have no local function declaration of their own.
    visible = re.sub(
        r"^[ \t]*#[^\n]*", lambda m: " " * len(m.group()), visible, flags=re.M
    )
    start = sum(len(x) for x in lines[: line - 1])
    end = start + len(lines[line - 1])
    col = primary.get("column")
    if col is not None:
        # Compiler columns can be byte offsets or display columns. Without a
        # recorded unit, non-ASCII/tab lines cannot safely locate a function.
        if any(ord(char) > 127 or char == "\t" for char in lines[line - 1]):
            return _unknown("source_column_unit_unresolved")
        if (
            not isinstance(col, int)
            or isinstance(col, bool)
            or not 0 < col <= len(lines[line - 1].rstrip("\r\n"))
        ):
            return _unknown("source_column_out_of_range")
        start, end = start + col - 1, start + col
    else:
        locus = visible[start:end]
        if not locus.strip():
            return _unknown("no_visible_source_locus")
        end = start + len(locus.rstrip())
        start += len(locus) - len(locus.lstrip())
    stack, functions, boundary = [], [], 0
    for i, c in enumerate(visible):
        if c == "{":
            match = _FUNCTION.fullmatch(visible[boundary:i]) if not stack else None
            # Exclude masked includes/comments and whitespace preceding a
            # declaration: a diagnostic there is outside the function.
            decl = boundary + match.start("qual") if match else boundary
            stack.append((i, decl, match))
            boundary = i + 1
        elif c == "}":
            if not stack:
                return _unknown("unbalanced_source_scopes")
            opening, decl, match = stack.pop()
            if match and opening < start and end <= i:
                functions.append((opening, decl, i + 1, match))
            boundary = i + 1
        elif c == ";":
            boundary = i + 1
    if stack:
        return _unknown("unbalanced_source_scopes")
    if len(functions) != 1:
        return _unknown("no_unique_simple_enclosing_function")
    opening, decl, closing, match = functions[0]
    body = visible[opening:closing]
    # Square-bracket syntax can introduce a lambda with arbitrary attributes,
    # multiline captures or macro suffixes. Do not attempt a partial lambda
    # grammar. Arrays/subscripts are conservatively outside this reader too.
    if "[" in body or "]" in body or re.search(r"\b(?:class|struct|union)\b", body):
        return _unknown("nested_source_scope")
    quals = set(match.group("qual").split())
    device = bool(quals & {"__aicore__", "__device__"})
    host = "__host__" in quals or match.group("name") == "main"
    if device and host:
        return _unknown("conflicting_function_attributes")
    if not device and not host:
        return _unknown("unannotated_helper")
    return {
        "value": "device" if device else "host",
        "basis": "explicit_device_attribute"
        if device
        else "main_function"
        if match.group("name") == "main"
        else "explicit_host_attribute",
        "file": key,
        "line": line,
        "function": match.group("name"),
        "source_text_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "binding": "current_source_only",
        "blockers": [],
    }


def build_stage_evidence(
    normalized: dict, context: dict | None = None, *, legacy_phase: str = "unknown"
) -> dict[str, Any]:
    """Independent axes; legacy phase/category and caller inputs are unchanged."""
    primary = normalized.get("primary_error") or {}
    context = context or {}
    stage = (
        _stage(primary, normalized) if primary else _unknown("no_primary_diagnostic")
    )
    source = (
        _source_domain(primary, context)
        if primary
        else _unknown("no_primary_diagnostic")
    )
    path = str(primary.get("file_original") or primary.get("file") or "").replace(
        "\\", "/"
    )
    parts, name = path.lower().split("/"), posixpath.basename(path).lower()
    host = "op_host" in parts or name.endswith("_host.cpp")
    device = "op_kernel" in parts or name.endswith(".asc")
    hint = {
        "value": "host"
        if host and not device
        else "device"
        if device and not host
        else "unknown",
        "basis": "path_only",
        "path": path,
    }
    conflicts = []
    if (
        source["value"] != "unknown"
        and hint["value"] != "unknown"
        and source["value"] != hint["value"]
    ):
        conflicts.append(
            {
                "axes": ["source_context", "path_hint"],
                "values": [source["value"], hint["value"]],
                "reason": "path_does_not_establish_source_domain",
            }
        )
    legacy_domain = {"host_compile": "host", "kernel_compile": "device"}.get(
        legacy_phase
    )
    if legacy_domain and source["value"] not in {"unknown", legacy_domain}:
        conflicts.append(
            {
                "axes": ["source_context", "phase"],
                "values": [source["value"], legacy_phase],
                "reason": "legacy_phase_is_not_execution_evidence",
            }
        )
    legacy_stage = {
        "cmake_configure": "configure",
        "cmake_generate": "generate",
        "link": "link",
        "package": "packaging",
    }.get(legacy_phase)
    if stage["value"] != "unknown" and (
        (legacy_stage and stage["value"] != legacy_stage)
        or (legacy_domain and stage["value"] not in {"compilation"})
    ):
        conflicts.append(
            {
                "axes": ["failure_stage", "phase"],
                "values": [stage["value"], legacy_phase],
                "reason": "direct_diagnostic_differs_from_legacy_phase",
            }
        )
    return {
        "failure_stage": stage,
        "source_context": source,
        "path_hint": hint,
        "conflicts": conflicts,
    }
