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
"""Conservative, deterministic checks for high-value Host Runtime contracts.

The checker emits candidates and repair contracts. It is not a C++ parser and
never proves that a source file is safe merely because it returns no findings.

用途：检查 Host Runtime 源码中的输出绑定、分配释放配对、拷贝容量和
Event 记录/等待顺序等高风险契约。

使用方法：
    python3 runtime_contract_lint.py <source_file>
    python3 runtime_contract_lint.py --help
"""

from __future__ import annotations

import argparse
import errno
import json
import os
import re
import stat
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "runtime-contract-lint/2.0"
DEFAULT_HARDWARE_TARGET = "ascend910b1"
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx"})
MAX_SOURCE_BYTES = 4 * 1024 * 1024

OUTPUT_CONTRACTS: dict[str, tuple[tuple[int, str, str, str], ...]] = {
    "aclrtCreateContext": ((0, "context", "non-null handle", "aclrtContext*"),),
    "aclrtCreateStream": ((0, "stream", "non-null handle", "aclrtStream*"),),
    "aclrtCreateStreamWithConfig": ((0, "stream", "non-null handle", "aclrtStream*"),),
    "aclrtCreateEvent": ((0, "event", "non-null handle", "aclrtEvent*"),),
    "aclrtCreateEventWithFlag": ((0, "event", "non-null handle", "aclrtEvent*"),),
    "aclrtCreateEventExWithFlag": ((0, "event", "non-null handle", "aclrtEvent*"),),
    "aclrtCreateNotify": ((0, "notify", "non-null handle", "aclrtNotify*"),),
    "aclrtGetCurrentContext": (
        (0, "context", "writable current Context output", "aclrtContext*"),
    ),
    "aclrtGetDevice": ((0, "device_id", "non-negative device id", "int32_t*"),),
    "aclrtGetDeviceCount": ((0, "device_count", "writable device count", "uint32_t*"),),
    "aclrtGetDeviceSatMode": (
        (0, "overflow_mode", "writable overflow mode", "aclrtFloatOverflowMode*"),
    ),
    "aclrtGetRunMode": ((0, "run_mode", "ACL_HOST or ACL_DEVICE", "aclrtRunMode*"),),
    "aclrtGetEventId": ((1, "event_id", "non-negative event id", "uint32_t*"),),
    "aclrtGetNotifyId": ((1, "notify_id", "non-negative notify id", "uint32_t*"),),
    "aclrtGetVersion": (
        (0, "major", "non-negative version component", "int32_t*"),
        (1, "minor", "non-negative version component", "int32_t*"),
        (2, "patch", "non-negative version component", "int32_t*"),
    ),
    "aclrtGetMemInfo": (
        (1, "free_bytes", "free_bytes <= total_bytes", "size_t*"),
        (2, "total_bytes", "free_bytes <= total_bytes", "size_t*"),
    ),
    "aclrtDeviceGetStreamPriorityRange": (
        (0, "least_priority", "writable least priority", "int32_t*"),
        (1, "greatest_priority", "writable greatest priority", "int32_t*"),
    ),
    "aclrtDeviceCanAccessPeer": ((0, "can_access_peer", "0 or 1", "int32_t*"),),
    "aclrtGetDeviceUtilizationRate": (
        (1, "utilization", "writable utilization record", "aclrtUtilizationInfo*"),
    ),
    "aclrtQueryEventStatus": (
        (
            1,
            "event_status",
            "defined recorded-status enum",
            "aclrtEventRecordedStatus*",
        ),
    ),
    "aclrtStreamQuery": (
        (1, "stream_status", "COMPLETE or NOT_READY", "aclrtStreamStatus*"),
    ),
    "aclrtStreamGetFlags": ((1, "stream_flags", "defined stream flags", "uint32_t*"),),
    "aclrtStreamGetPriority": (
        (1, "stream_priority", "writable priority", "uint32_t*"),
    ),
    "aclrtMalloc": ((0, "device_buffer", "non-null allocation on success", "void**"),),
    "aclrtMallocHost": (
        (0, "host_buffer", "non-null allocation on success", "void**"),
    ),
    "aclrtMemAllocManaged": (
        (0, "managed_buffer", "non-null allocation on success", "void**"),
    ),
}

ALLOCATORS = {
    "aclrtMalloc": ("aclrtFree", "device"),
    "aclrtMallocHost": ("aclrtFreeHost", "host"),
    "aclrtMemAllocManaged": ("aclrtFree", "managed"),
}
DEALLOCATORS = {"aclrtFree", "aclrtFreeHost"}
EVENT_CREATORS = {
    "aclrtCreateEvent",
    "aclrtCreateEventWithFlag",
    "aclrtCreateEventExWithFlag",
}
EVENT_RECORDERS = {"aclrtRecordEvent", "aclrtRecordEventWithTimeout"}
EVENT_WAITERS = {
    "aclrtSynchronizeEvent",
    "aclrtSynchronizeEventWithTimeout",
    "aclrtQueryEventStatus",
}

COPY_SPECS = {
    "aclrtMemcpy": (1, 3, "copy"),
    "aclrtMemcpyAsync": (1, 3, "copy"),
    "aclrtMemcpy2D": (1, 4, "copy2d"),
    "aclrtMemcpy2DAsync": (1, 4, "copy2d"),
    "aclrtMemset": (1, 3, "fill"),
    "aclrtMemsetAsync": (1, 3, "fill"),
}

EVENT_FLAGS_910 = {
    "ACL_EVENT_SYNC",
    "ACL_EVENT_TIME_LINE",
    "ACL_EVENT_CAPTURE_STREAM_PROGRESS",
    "ACL_EVENT_DEVICE_USE_ONLY",
    "ACL_EVENT_EXTERNAL",
    "ACL_EVENT_IPC",
    "ACL_EVENT_RECORDED_STATUS_COMPLETE",
    "ACL_EVENT_RECORDED_STATUS_NOT_READY",
    "ACL_EVENT_STATUS_COMPLETE",
    "ACL_EVENT_STATUS_NOT_READY",
    "ACL_EVENT_STATUS_RESERVED",
    "ACL_EVENT_WAIT_STATUS_COMPLETE",
    "ACL_EVENT_WAIT_STATUS_NOT_READY",
    "ACL_EVENT_WAIT_STATUS_RESERVED",
}
EVENT_FLAGS_950 = EVENT_FLAGS_910 | {
    "ACL_EVENT_RECORD_DEFAULT",
    "ACL_EVENT_RECORD_EXTERNAL",
    "ACL_EVENT_WAIT_DEFAULT",
    "ACL_EVENT_WAIT_EXTERNAL",
}
NOTIFY_FLAGS = {"ACL_NOTIFY_DEFAULT", "ACL_NOTIFY_DEVICE_USE_ONLY"}
ACL_FAMILY_MACRO = re.compile(r"\bACL_(?:EVENT|NOTIFY)_[A-Z][A-Z_]*")

CALL_START = re.compile(r"\b(acl(?:rt)?[A-Z][A-Za-z0-9_]*)\s*\(")
IDENTIFIER = re.compile(r"^[&*\s()]*([A-Za-z_]\w*)[\s()]*$")
STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
NULL_LITERAL = re.compile(
    r"^(?:(?:static_cast|reinterpret_cast|const_cast)\s*<[^>]+>\s*\()?"
    r"(?:nullptr|NULL|__null|0[UuLl]*)(?:\))?$"
)
INT_LITERAL = re.compile(r"^[+]?(?:0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*$")
SIGNED_INT_LITERAL = re.compile(r"^[+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*$")
TYPE_WORDS = (
    r"(?:const\s+|volatile\s+|static\s+|constexpr\s+|unsigned\s+|signed\s+|"
    r"long\s+|short\s+|int\s+|uint\d+_t\s+|int\d+_t\s+|size_t\s+|ssize_t\s+|"
    r"void\s+|aclrt[A-Za-z0-9_]+\s+)"
)
DECLARATION = re.compile(
    r"(?m)(?:^|[;{}])\s*(?P<type>" + TYPE_WORDS + r")+"
    r"(?P<pointers>\**)\s*(?P<decls>[A-Za-z_]\w*(?:\s*=[^;,]+)?"
    r"(?:\s*,\s*[A-Za-z_]\w*(?:\s*=[^;,]+)?)*)\s*;"
)


def _strip_comments(text: str) -> str:
    """Remove comments while retaining line positions."""

    def blank(match: re.Match[str]) -> str:
        return "".join("\n" if c == "\n" else " " for c in match.group(0))

    return re.sub(r"//[^\n]*|/\*.*?\*/", blank, text, flags=re.DOTALL)


def _split_arguments(arguments: str) -> list[str]:
    result: list[str] = []
    start = 0
    depth = 0
    quote: str | None = None
    escaped = False
    for index, character in enumerate(arguments):
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
        elif character in "([{":
            depth += 1
        elif character in ")]}":
            depth = max(0, depth - 1)
        elif character == "," and depth == 0:
            result.append(arguments[start:index].strip())
            start = index + 1
    result.append(arguments[start:].strip())
    return result


def _calls(text: str) -> list[dict[str, Any]]:
    calls: list[dict[str, Any]] = []
    for match in CALL_START.finditer(text):
        open_at = text.find("(", match.start(), match.end())
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
                    calls.append(
                        {
                            "name": match.group(1),
                            "arguments": _split_arguments(text[open_at + 1 : index]),
                            "position": match.start(),
                            "line": text.count("\n", 0, match.start()) + 1,
                            "end": index + 1,
                        }
                    )
                    break
    return calls


def _identifier(argument: str, require_address: bool = False) -> str | None:
    if require_address and "&" not in argument:
        return None
    match = IDENTIFIER.match(argument.strip())
    return match.group(1) if match else None


def _is_definite_null(argument: str) -> bool:
    compact = re.sub(r"\s+", "", argument)
    while compact.startswith("(") and compact.endswith(")"):
        compact = compact[1:-1]
    if NULL_LITERAL.fullmatch(compact):
        return True
    return bool(re.fullmatch(r"\([^()]*\*\)(?:nullptr|NULL|__null|0[UuLl]*)", compact))


def _literal(argument: str) -> int | None:
    compact = argument.strip()
    if not SIGNED_INT_LITERAL.fullmatch(compact):
        return None
    suffixless = re.sub(r"[uUlL]+$", "", compact)
    try:
        return int(suffixless, 0)
    except ValueError:
        return None


def _declared_types(text: str) -> dict[str, set[str]]:
    declarations: dict[str, set[str]] = {}
    for match in DECLARATION.finditer(text):
        base = " ".join(match.group("type").split()) + match.group("pointers")
        for declarator in _split_arguments(match.group("decls")):
            name = re.match(r"([A-Za-z_]\w*)", declarator.strip())
            if name:
                declarations.setdefault(name.group(1), set()).add(base)
    return declarations


def _canonical_type(type_text: str) -> str:
    tokens = type_text.replace("*", " * ").split()
    tokens = [
        token
        for token in tokens
        if token not in {"const", "volatile", "static", "constexpr"}
    ]
    pointers = "".join(token for token in tokens if token == "*")
    words = [token for token in tokens if token != "*"]
    word_text = " ".join(words)
    aliases = {
        "int": "int32_t",
        "unsigned int": "uint32_t",
        "unsigned": "uint32_t",
        "long": "int64_t",
        "unsigned long": "uint64_t",
        "size_t": "uint64_t",
        "ssize_t": "int64_t",
    }
    return aliases.get(word_text, word_text) + pointers


def _diagnostic(
    severity: str,
    rule: str,
    call: dict[str, Any],
    message: str,
    repair_contract: dict[str, Any] | None = None,
    evidence: list[str] | None = None,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "severity": severity,
        "rule": rule,
        "line": call["line"],
        "message": message,
    }
    if evidence:
        result["evidence"] = evidence
    if repair_contract is not None:
        result["repair_contract"] = repair_contract
    return result


def _cleanup_wrappers(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    wrapper = re.compile(
        r"\b([A-Za-z_]\w*)\s*\(([^()]*)\)\s*\{([^{}]*)\}",
        re.DOTALL,
    )
    for match in wrapper.finditer(text):
        if "," in match.group(2):
            continue
        parameters = re.findall(r"\b([A-Za-z_]\w*)\b", match.group(2))
        if not parameters:
            continue
        parameter = parameters[-1]
        cleanup = re.search(
            rf"\b(aclrtFreeHost|aclrtFree)\s*\(\s*{re.escape(parameter)}\s*\)",
            match.group(3),
        )
        if cleanup:
            result[match.group(1)] = cleanup.group(1)
    return result


def _same_statement(text: str, position: int) -> str:
    start = max(text.rfind(";", 0, position), text.rfind("{", 0, position)) + 1
    end = text.find(";", position)
    return text[start : len(text) if end < 0 else end + 1]


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
        raise ValueError("source must be a regular file")
    if before.st_size > max_bytes:
        raise ValueError(f"source exceeds {max_bytes} bytes")

    chunks = bytearray()
    while True:
        remaining = max_bytes + 1 - len(chunks)
        if remaining <= 0:
            raise ValueError(f"source exceeds {max_bytes} bytes")
        chunk = os.read(fd, min(64 * 1024, remaining))
        if not chunk:
            break
        chunks.extend(chunk)
        if len(chunks) > max_bytes:
            raise ValueError(f"source exceeds {max_bytes} bytes")

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


def _open_regular_no_follow(path: Path, *, max_bytes: int) -> int:
    if path.suffix.casefold() not in SOURCE_SUFFIXES:
        raise ValueError(f"unsupported source suffix: {path.suffix or '<none>'}")
    expected = os.lstat(path)
    if stat.S_ISLNK(expected.st_mode):
        raise ValueError("source must not be a symbolic link")
    if not stat.S_ISREG(expected.st_mode):
        raise ValueError("source must be a regular file")
    if expected.st_size > max_bytes:
        raise ValueError(f"source exceeds {max_bytes} bytes")

    flags = _secure_flags(directory=False)
    fd = os.open(path, flags)
    try:
        if not hasattr(os, "O_CLOEXEC"):
            os.set_inheritable(fd, False)
        opened = os.fstat(fd)
        if not _same_identity(expected, opened):
            raise ValueError("file changed while opening")
        if not stat.S_ISREG(opened.st_mode):
            raise ValueError("source must be a regular file")
        if opened.st_size > max_bytes:
            raise ValueError(f"source exceeds {max_bytes} bytes")
        return fd
    except BaseException:
        os.close(fd)
        raise


def _read_source(path: Path) -> str:
    """Read a bounded UTF-8 source file without following a file symlink."""
    fd = _open_regular_no_follow(path, max_bytes=MAX_SOURCE_BYTES)
    try:
        data = _read_open_fd_bounded(fd, max_bytes=MAX_SOURCE_BYTES)
    finally:
        os.close(fd)
    return data.decode("utf-8", errors="strict")


def lint_source(
    source: Path | str,
    hardware_target: str = DEFAULT_HARDWARE_TARGET,
) -> dict[str, Any]:
    if hardware_target not in {"ascend910b1", "ascend950pr"}:
        raise ValueError(
            f"unknown hardware target {hardware_target!r}; "
            "known targets: ascend910b1, ascend950pr"
        )
    path = Path(source)
    raw = _read_source(path)
    text = _strip_comments(raw)
    calls = _calls(text)
    declared = _declared_types(text)
    diagnostics: list[dict[str, Any]] = []

    valid_macros = (
        EVENT_FLAGS_950 if hardware_target == "ascend950pr" else EVENT_FLAGS_910
    )
    valid_macros = set(valid_macros) | NOTIFY_FLAGS
    seen_macros: set[str] = set()
    for match in ACL_FAMILY_MACRO.finditer(STRING_LITERAL.sub('""', text)):
        macro = match.group(0)
        if macro in valid_macros or macro in seen_macros:
            continue
        seen_macros.add(macro)
        diagnostics.append(
            _diagnostic(
                "error",
                "unknown-acl-event-macro",
                {"line": text.count("\n", 0, match.start()) + 1},
                f"{macro} is not in the verified {hardware_target} ACL event/notify vocabulary",
                {
                    "macro": macro,
                    "valid_macros": sorted(valid_macros),
                    "requirement": "use_only_header_verified_macro_names",
                    "verification_predicate": "compile_against_the_target_acl_rt_header",
                },
            )
        )

    for call in calls:
        for index, role, predicate, output_type in OUTPUT_CONTRACTS.get(
            call["name"], ()
        ):
            if index >= len(call["arguments"]):
                continue
            argument = call["arguments"][index]
            if _is_definite_null(argument):
                diagnostics.append(
                    _diagnostic(
                        "error",
                        "required-output-is-null",
                        call,
                        f"{call['name']} argument {index + 1} ({role}) is a null output pointer",
                        {
                            "api": call["name"],
                            "argument_index": index,
                            "output_role": role,
                            "output_type": output_type,
                            "requirement": "writable_output_storage",
                            "verification_predicate": predicate,
                        },
                    )
                )
                continue
            identifier = _identifier(argument, require_address=True)
            if identifier is None or identifier not in declared:
                continue
            expected = _canonical_type(output_type).rstrip("*")
            actual_types = sorted(declared[identifier])
            if all(
                _canonical_type(actual).rstrip("*") != expected
                for actual in actual_types
            ):
                diagnostics.append(
                    _diagnostic(
                        "error",
                        "output-binding-type-mismatch",
                        call,
                        f"{call['name']} output {role} binds {identifier} with incompatible type",
                        {
                            "api": call["name"],
                            "argument_index": index,
                            "output_role": role,
                            "output_type": output_type,
                            "declared_type": actual_types,
                            "requirement": "match_contract_output_type",
                            "verification_predicate": predicate,
                        },
                    )
                )

    for call in calls:
        if (
            call["name"] != "aclrtDeviceGetStreamPriorityRange"
            or len(call["arguments"]) < 2
        ):
            continue
        first = _identifier(call["arguments"][0], require_address=True)
        second = _identifier(call["arguments"][1], require_address=True)
        if first and any(
            word in first.casefold() for word in ("greatest", "maximum", "max_")
        ):
            diagnostics.append(
                _diagnostic(
                    "error",
                    "priority-output-role-inverted",
                    call,
                    "argument 1 is least_priority but its variable name indicates greatest priority",
                    {
                        "api": call["name"],
                        "argument_index": 0,
                        "output_role": "least_priority",
                        "requirement": "bind_argument_1_to_least_priority",
                        "verification_predicate": "least_and_greatest_roles_are_preserved",
                    },
                )
            )
        if second and any(
            word in second.casefold() for word in ("least", "minimum", "min_")
        ):
            diagnostics.append(
                _diagnostic(
                    "error",
                    "priority-output-role-inverted",
                    call,
                    "argument 2 is greatest_priority but its variable name indicates least priority",
                    {
                        "api": call["name"],
                        "argument_index": 1,
                        "output_role": "greatest_priority",
                        "requirement": "bind_argument_2_to_greatest_priority",
                        "verification_predicate": "least_and_greatest_roles_are_preserved",
                    },
                )
            )
        if (
            first
            and first in declared
            and any("aclrtStreamPriority" in item for item in declared[first])
        ):
            diagnostics.append(
                _diagnostic(
                    "error",
                    "priority-output-type-invalid",
                    call,
                    "priority range output uses an undeclared ACL stream-priority type",
                    {
                        "api": call["name"],
                        "argument_index": 0,
                        "output_role": "priority_range",
                        "requirement": "declare_both_outputs_as_writable_int32_t_values",
                        "verification_predicate": "compile_against_the_target_acl_rt_header",
                    },
                )
            )

    allocation_sizes: dict[str, int] = {}
    allocation_calls: list[tuple[dict[str, Any], str, str, int | None]] = []
    for allocation in calls:
        expected = ALLOCATORS.get(allocation["name"])
        if not expected or len(allocation["arguments"]) < 2:
            continue
        owner = _identifier(allocation["arguments"][0], require_address=True)
        if not owner:
            continue
        size = _literal(allocation["arguments"][1])
        if size is not None:
            allocation_sizes[owner] = size
        allocation_calls.append((allocation, owner, expected[0], size))

    wrappers = _cleanup_wrappers(text)
    for allocation, owner, expected_free, _size in allocation_calls:
        frees: list[dict[str, Any]] = []
        for call in calls:
            if (
                call["position"] > allocation["position"]
                and call["name"] in DEALLOCATORS
                and call["arguments"]
                and _identifier(call["arguments"][0]) == owner
            ):
                frees.append(call)
        for helper_name, cleanup_name in wrappers.items():
            match = re.search(
                rf"\b{re.escape(helper_name)}\s*\(\s*{re.escape(owner)}\s*\)",
                text[allocation["end"] :],
            )
            if match:
                position = allocation["end"] + match.start()
                frees.append(
                    {
                        "name": cleanup_name,
                        "arguments": [owner],
                        "position": position,
                        "line": text.count("\n", 0, position) + 1,
                        "via_helper": helper_name,
                    }
                )
        if not frees:
            diagnostics.append(
                _diagnostic(
                    "warning",
                    "owner-cleanup-unproven",
                    allocation,
                    f"no matching {expected_free} cleanup is proven for {owner}",
                    {
                        "api": expected_free,
                        "argument_index": 0,
                        "output_role": "allocation_owner",
                        "requirement": "prove_matching_cleanup_or_report_external_ownership",
                        "verification_predicate": "owner_outlives_last_use_and_cleanup_runs_exactly_once",
                    },
                )
            )
            continue
        families = {item["name"] for item in frees}
        wrong = [item for item in frees if item["name"] != expected_free]
        helper_wrong = next((item for item in wrong if item.get("via_helper")), None)
        if helper_wrong:
            diagnostics.append(
                _diagnostic(
                    "error",
                    "allocator-deallocator-mismatch",
                    helper_wrong,
                    f"{owner} reaches {helper_wrong['name']} through {helper_wrong['via_helper']}, "
                    f"but {allocation['name']} requires {expected_free}",
                    {
                        "api": helper_wrong["name"],
                        "argument_index": 0,
                        "output_role": "allocation_owner",
                        "requirement": f"replace cleanup family with {expected_free}",
                        "verification_predicate": "exactly_once_matching_cleanup_after_last_use",
                    },
                )
            )
        elif wrong and expected_free in families:
            diagnostics.append(
                _diagnostic(
                    "warning",
                    "mixed-cleanup-families-require-path-check",
                    wrong[0],
                    f"{owner} reaches both {expected_free} and {wrong[0]['name']}; prove branch-specific ownership",
                    {
                        "api": wrong[0]["name"],
                        "argument_index": 0,
                        "output_role": "allocation_owner",
                        "requirement": "prove_each_cleanup_branch_uses_the_allocation_family",
                        "verification_predicate": "only_the_matching_cleanup_runs_on_each_reachable_path",
                    },
                )
            )
        elif wrong:
            diagnostics.append(
                _diagnostic(
                    "error",
                    "allocator-deallocator-mismatch",
                    wrong[0],
                    f"{owner} comes from {allocation['name']} and must use {expected_free}",
                    {
                        "api": wrong[0]["name"],
                        "argument_index": 0,
                        "output_role": "allocation_owner",
                        "requirement": f"replace cleanup family with {expected_free}",
                        "verification_predicate": "exactly_once_matching_cleanup_after_last_use",
                    },
                )
            )

    for call in calls:
        spec = COPY_SPECS.get(call["name"])
        if not spec:
            continue
        capacity_index, count_index, kind = spec
        if max(capacity_index, count_index) >= len(call["arguments"]):
            continue
        capacity = _literal(call["arguments"][capacity_index])
        count = _literal(call["arguments"][count_index])
        if count is not None and count < 0:
            diagnostics.append(
                _diagnostic(
                    "error",
                    "negative-byte-count",
                    call,
                    "a Runtime copy/fill byte count cannot be negative",
                    {
                        "api": call["name"],
                        "argument_index": count_index,
                        "requirement": "use_non_negative_checked_byte_count",
                        "verification_predicate": "size_arithmetic_does_not_wrap",
                    },
                )
            )
        if count == 0:
            pointer_args = (
                call["arguments"][:capacity_index]
                + call["arguments"][capacity_index + 1 : count_index]
            )
            if any(
                argument.strip() and not _is_definite_null(argument)
                for argument in pointer_args
            ):
                diagnostics.append(
                    _diagnostic(
                        "error",
                        "zero-count-nonempty-operation",
                        call,
                        "a non-empty Runtime operation has a definite zero byte count",
                        {
                            "api": call["name"],
                            "argument_index": count_index,
                            "requirement": "preserve_the_non_empty_extent_or_make_the_noop_explicit",
                            "verification_predicate": "destination_and_source_extent_match_the_intended_operation",
                        },
                    )
                )
        if capacity is not None and count is not None and count > capacity:
            diagnostics.append(
                _diagnostic(
                    "error",
                    "copy-exceeds-destination-capacity",
                    call,
                    f"{kind} count {count} exceeds the supplied destination capacity {capacity}",
                    {
                        "api": call["name"],
                        "argument_index": count_index,
                        "capacity_argument_index": capacity_index,
                        "requirement": "make_count_no_larger_than_destination_capacity",
                        "verification_predicate": "last_written_byte_is_within_destination_extent",
                    },
                )
            )
        if kind in {"copy", "fill"} and count is not None:
            destination = _identifier(call["arguments"][0])
            allocated = allocation_sizes.get(destination or "")
            if allocated is not None and count > allocated:
                diagnostics.append(
                    _diagnostic(
                        "error",
                        "copy-exceeds-allocation",
                        call,
                        f"operation writes {count} bytes to {destination}, allocated extent is {allocated}",
                        {
                            "api": call["name"],
                            "argument_index": count_index,
                            "requirement": "repair_allocation_or_byte_extent_at_the_first_bad_contract",
                            "verification_predicate": "allocation_extent_covers_the_full_operation",
                        },
                    )
                )
        if kind == "copy2d":
            pitch = _literal(call["arguments"][capacity_index])
            width = _literal(call["arguments"][count_index])
            if pitch is not None and width is not None and width > pitch:
                diagnostics.append(
                    _diagnostic(
                        "error",
                        "copy-width-exceeds-pitch",
                        call,
                        f"2D width {width} exceeds the destination pitch {pitch}",
                        {
                            "api": call["name"],
                            "argument_index": count_index,
                            "pitch_argument_index": capacity_index,
                            "requirement": "keep_each_row_within_destination_pitch",
                            "verification_predicate": "all_rows_and_padding_fit_the_allocation",
                        },
                    )
                )
        if kind == "fill" and len(call["arguments"]) > 2:
            value = _literal(call["arguments"][2])
            if value is not None and value > 0xFF:
                diagnostics.append(
                    _diagnostic(
                        "error",
                        "memset-multibyte-literal",
                        call,
                        "memset uses only the low byte of a multi-byte integer literal",
                        {
                            "api": call["name"],
                            "argument_index": 2,
                            "requirement": "use_the_documented_single_byte_fill_value",
                            "verification_predicate": "observed_fill_pattern_matches_the_data_gate",
                        },
                    )
                )

    for synchronization in calls:
        if (
            synchronization["name"] not in EVENT_WAITERS
            or not synchronization["arguments"]
        ):
            continue
        event = _identifier(synchronization["arguments"][0])
        if not event:
            continue
        creation = next(
            (
                item
                for item in calls
                if item["position"] < synchronization["position"]
                and item["name"] in EVENT_CREATORS
                and item["arguments"]
                and _identifier(item["arguments"][0], require_address=True) == event
            ),
            None,
        )
        if creation is None:
            continue
        recorded = any(
            item["position"] > creation["position"]
            and item["position"] < synchronization["position"]
            and item["name"] in EVENT_RECORDERS
            and item["arguments"]
            and _identifier(item["arguments"][0]) == event
            for item in calls
        )
        if not recorded:
            diagnostics.append(
                _diagnostic(
                    "error",
                    "event-synchronized-before-record",
                    synchronization,
                    f"{event} is synchronized or queried before a preceding event record",
                    {
                        "api": synchronization["name"],
                        "argument_index": 0,
                        "output_role": "event_completion_boundary",
                        "requirement": "record_the_event_on_its_producing_stream_before_wait_or_query",
                        "verification_predicate": "record_precedes_wait_and_wait_establishes_completion",
                    },
                )
            )
        if (
            synchronization["name"] == "aclrtSynchronizeEventWithTimeout"
            and len(synchronization["arguments"]) >= 2
            and _literal(synchronization["arguments"][1]) == 0
        ):
            statement = _same_statement(text, synchronization["position"])
            assigned = re.search(
                r"\b(?:aclError|auto|const\s+aclError)\s+([A-Za-z_]\w*)\s*=", statement
            )
            result_name = assigned.group(1) if assigned else None
            tail = text[synchronization["end"] :]
            gates_work = bool(
                result_name
                and re.search(
                    rf"\b{re.escape(result_name)}\s*==\s*ACL_SUCCESS\b[^;]*\bacl(?:rt)?[A-Z]",
                    tail,
                    flags=re.DOTALL,
                )
            )
            diagnostics.append(
                _diagnostic(
                    "error" if gates_work else "warning",
                    "event-zero-timeout-gates-dependent-work"
                    if gates_work
                    else "event-zero-timeout-is-poll",
                    synchronization,
                    "timeout 0 is an immediate poll and cannot establish a dependent completion boundary",
                    {
                        "api": synchronization["name"],
                        "argument_index": 1,
                        "output_role": "completion_timeout",
                        "requirement": "use_a_documented_positive_bound_or_explicit_unbounded_wait_for_dependent_work",
                        "verification_predicate": "dependent_work_is_reachable_only_after_completed_wait",
                    }
                    if gates_work
                    else {
                        "api": synchronization["name"],
                        "argument_index": 1,
                        "output_role": "completion_timeout",
                        "requirement": "treat_timeout_zero_as_a_poll_and_do_not_use_it_as_a_completion_gate",
                        "verification_predicate": "poll_result_is_not_used_as_proof_of_device_completion",
                    },
                )
            )

    # Runtime fixes are intentionally one hypothesis at a time.  Returning a
    # pile of downstream findings makes an Agent repair several contracts in
    # one pass and can hide which observation actually changed the first
    # failure.  Keep all warning-only observations, but expose only the
    # earliest definite error; the caller must rerun lint after that repair.
    indexed_errors = [
        (index, item)
        for index, item in enumerate(diagnostics)
        if item["severity"] == "error"
    ]
    first_error = (
        min(indexed_errors, key=lambda pair: (pair[1].get("line", 10**9), pair[0]))[1]
        if indexed_errors
        else None
    )
    errors = [first_error] if first_error is not None else []
    warnings = [item for item in diagnostics if item["severity"] == "warning"]
    return {
        "schema_version": SCHEMA_VERSION,
        "valid": not errors,
        "source": str(path),
        "runtime_call_count": len(calls),
        "errors": errors,
        "warnings": warnings,
        "repair_contracts": [
            item["repair_contract"] for item in errors if "repair_contract" in item
        ],
        "first_error": first_error,
        "next_action": "repair_current_first_error_then_rerun_lint"
        if errors
        else "continue_with_current_evidence",
    }


def _text_report(result: dict[str, Any]) -> str:
    lines = [
        f"{'PASS' if result['valid'] else 'FAIL'} {result['source']}",
        f"runtime calls: {result['runtime_call_count']}",
    ]
    for item in result["errors"] + result["warnings"]:
        lines.append(
            f"{item['severity'].upper()}:{item['rule']}:{item['line']}: {item['message']}"
        )
    lines.append(f"next_action: {result['next_action']}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_pos", nargs="?", type=Path)
    parser.add_argument("--source", dest="source_opt", type=Path)
    parser.add_argument("--format", choices=("json", "text"), default="json")
    parser.add_argument("--compact", action="store_true")
    parser.add_argument(
        "--hardware-target",
        choices=("ascend910b1", "ascend950pr"),
        default=DEFAULT_HARDWARE_TARGET,
    )
    args = parser.parse_args(argv)
    source = args.source_opt or args.source_pos
    if source is None:
        parser.error("a source path is required")
    try:
        result = lint_source(source, hardware_target=args.hardware_target)
    except (OSError, UnicodeError, ValueError) as error:
        print(
            json.dumps(
                {"schema_version": SCHEMA_VERSION, "valid": False, "error": str(error)}
            )
        )
        return 2
    if args.format == "text":
        print(_text_report(result))
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
