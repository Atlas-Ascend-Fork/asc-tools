# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Diagnostic facts from the selected diagnostic, independent of legacy labels.

No compiler invocation, new source reads, mutation or lookup in case metadata/KB.
SDK association describes log/namespace evidence, never API availability or a fix.
"""

from __future__ import annotations

import posixpath
import re
from pathlib import Path

from harness.engine.common import resolve_project_source
from harness.engine.stage_evidence import _visible_source
from harness.engine.stage_evidence import stage_observation

_QUOTED = r"['‘]([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)['’]"
_NO_API_MECHANISMS = {
    "syntax_error",
    "driver_option_rejection",
    "compiler_crash",
    "static_assertion",
    "link_unresolved_symbol",
    "link_library_lookup_failure",
}


def _mechanism(primary: dict) -> str:
    message = str(primary.get("message") or "")
    raw = str(primary.get("raw") or message)
    observation = stage_observation(raw, 1)
    if observation and observation["value"] == "driver":
        return "driver_option_rejection"
    if re.match(
        r"^(?:(?:[^\s:]+[/\\])?(?:clang(?:\+\+)?(?:-\d+)?|bisheng|ccec|gcc|g\+\+):\s*)?internal compiler error\b|^LLVM ERROR:",
        raw,
        re.I,
    ):
        return "compiler_crash"
    # These expressions describe compiler wording, not the cause of that wording.
    for pattern, value in [
        (
            r"^(?:use of )?undeclared identifier\b|^'[^']+' was not declared in this scope",
            "undeclared_identifier",
        ),
        (
            r"^(?:unknown type name|no type named)\b|^'[^']+' does not name a type",
            "unknown_type",
        ),
        (
            r"^(?:no member named|'.*' has no member named|'.*' is not a member of)\b",
            "member_lookup_failure",
        ),
        (
            r"^(?:no matching (?:member )?function|call to .+ is ambiguous|ambiguous (?:call|overload))\b",
            "overload_resolution_failure",
        ),
        (
            r"^(?:too (?:few|many) arguments|too (?:few|many) (?:template )?arguments to)\b",
            "argument_count_mismatch",
        ),
        (r"^(?:expected\b|extraneous closing brace|unterminated\b)", "syntax_error"),
        (
            r"^(?:['\"<].+?['\">] file not found|[^:]+: No such file or directory)$",
            "header_lookup_failure",
        ),
        (r"^static assertion failed\b", "static_assertion"),
        (
            r"^(?:no viable conversion|invalid conversion|cannot convert|cannot initialize (?:a variable|return object))\b",
            "conversion_failure",
        ),
    ]:
        if re.search(pattern, message, re.I):
            return value
    # Link messages include a tool/object prefix in the normalized message.
    link = re.sub(
        r"^(?:[^\s:]+[/\\])?(?:ld(?:\.lld|\.gold)?|lld):\s*(?:error:\s*)?", "", raw
    )
    if re.match(
        r"^(?:undefined (?:symbol|reference)|.+:\s*undefined reference)\b", link
    ):
        return "link_unresolved_symbol"
    if re.match(r"^(?:cannot find -l|unable to find library -l)", link):
        return "link_library_lookup_failure"
    return "unknown"


def _failed_target(message: str) -> str | None:
    for prefix in (
        r"(?:undeclared identifier|unknown type name|no (?:member|type|template) named)\s+",
        r"no matching (?:member )?function for call to\s+",
    ):
        match = re.search(prefix + _QUOTED, message)
        if match:
            return match.group(1)
    match = re.match(_QUOTED + r" was not declared in this scope", message)
    return match.group(1) if match else None


def _symbol(message: str) -> str | None:
    target = _failed_target(message)
    return target.split("::")[-1] if target else None


def _declaration_name(value: object) -> str | None:
    if not isinstance(value, str) or not re.fullmatch(
        r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*", value
    ):
        return None
    return value


def _same_target(declaration: str, target: str) -> bool:
    # Compare qualified names exactly when both carry qualification. An ordinary
    # declaration echo can omit its enclosing namespace, so one bare name is OK.
    if "::" in declaration and "::" in target:
        return declaration == target
    return declaration.split("::")[-1] == target.split("::")[-1]


def _failed_line(primary: dict, context: dict) -> str | None:
    echo = primary.get("code_line")
    if isinstance(echo, str) and echo.strip():
        return _visible_source(echo)
    if primary.get("file_resolved") is False or primary.get("file_reanchored") is True:
        return None
    raw, line = primary.get("file"), primary.get("line")
    if (
        not isinstance(raw, str)
        or not isinstance(line, int)
        or isinstance(line, bool)
        or line < 1
    ):
        return None
    key = raw.replace("\\", "/")
    if context.get("case_dir"):
        root = Path(context["case_dir"]).resolve()
        resolved = resolve_project_source(raw, root, reanchor=False)
        if resolved is None:
            return None
        key = resolved.relative_to(root).as_posix()
    elif key.startswith("/") or ":" in key or "$" in key or ".." in key.split("/"):
        return None
    text = (context.get("file_text") or {}).get(posixpath.normpath(key))
    if not isinstance(text, str) or len(text) > 1_000_000:
        return None
    visible = _visible_source(text)
    if visible is None or re.search(
        r"^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif|else|endif|define|undef|line)\b",
        visible,
        re.M,
    ):
        return None
    lines = visible.splitlines()
    return lines[line - 1] if line <= len(lines) else None


def _origin(entry: dict) -> str:
    raw = entry.get("file_original") or entry.get("file")
    return raw.replace("\\", "/") if isinstance(raw, str) else ""


def _sdk_origin(path: str, context: dict) -> bool:
    cann = context.get("cann") or {}
    root = str(cann.get("install_dir") or "").replace("\\", "/").rstrip("/")
    # Ownership requires a concrete Linux absolute installation path. Never
    # expand variables/placeholders or turn a relative path into provenance.
    if (
        not root
        or not posixpath.isabs(root)
        or not posixpath.isabs(path)
        or root.startswith("//")
        or path.startswith("//")
        or any(char in root or char in path for char in "$%<>{}~")
        or any(part in {".", ".."} for part in root.split("/") + path.split("/"))
    ):
        return False
    return path.startswith(root + "/")


# SDK 侧命名空间（真实存在，逐字保留）。放在首个消费者旁而非文件头：
# 发布扫描器按文件头窗口做许可文本识别，聚集的 SDK 名词会误触其模糊匹配。
_SDK_NAMESPACES = {"AscendC", "gert", "ge", "optiling", "platform_ascendc"}


def build_diagnostic_facts(normalized: dict, context: dict | None = None) -> dict:
    context = context or {}
    primary = normalized.get("primary_error") or {}
    mechanism = _mechanism(primary) if primary else "unknown"
    position = primary.get("log_line")
    position = (
        position
        if isinstance(position, int) and not isinstance(position, bool) and position > 0
        else None
    )
    association = {"status": "not_established", "basis": "no_owner_evidence"}
    result = {
        "schema_version": "diagnostic-facts-v1",
        "mechanism": {
            "value": mechanism,
            "basis": "none" if mechanism == "unknown" else "primary_diagnostic_text",
            "primary_log_line": position,
            "log_line_space": "normalized",
        },
        "sdk_association": association,
    }
    if mechanism in _NO_API_MECHANISMS:
        association.update(
            status="not_applicable", basis="api_ownership_not_applicable"
        )
        return result
    message = str(primary.get("message") or "")
    target = _failed_target(message)
    symbol = _symbol(message)
    visible = _failed_line(primary, context) or ""
    calls = list(
        re.finditer(
            r"(?<![\w:])(?:::)?(?P<name>[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*(?:<[^;<>]*>)?\s*\(",
            visible,
        )
    )
    if symbol is None and mechanism == "argument_count_mismatch" and len(calls) == 1:
        symbol = calls[0].group("name").split("::")[-1]
    namespace = None
    # The compiler's explicitly qualified failed target is independent evidence.
    # Keep the namespace root: a project-owned Project::AscendC is not AscendC.
    if target and "::" in target and target.split("::")[0] in _SDK_NAMESPACES:
        namespace = target.split("::")[0]
    if symbol:
        matching = [m for m in calls if _same_target(m.group("name"), target or symbol)]
        if (
            namespace is None
            and len(re.findall(r"\b" + re.escape(symbol) + r"\b", visible)) == 1
            and len(matching) == 1
        ):
            qualified = matching[0].group("name").split("::")
            if len(qualified) > 1 and qualified[0] in _SDK_NAMESPACES:
                namespace = qualified[0]
        explicit = re.search(
            r"(?:no member named|no type named)\s+"
            + _QUOTED
            + r"\s+in namespace\s+"
            + _QUOTED,
            message,
        )
        if (
            explicit
            and explicit.group(1) == symbol
            and explicit.group(2).split("::")[0] in _SDK_NAMESPACES
        ):
            namespace = explicit.group(2).split("::")[0]
    if namespace:
        association.update(
            status="explicit_namespace_use",
            basis="explicit_failed_namespace",
            symbol=symbol,
            namespace=namespace,
        )
    if not symbol or position is None:
        return result
    hint = primary.get("root_hint") or {}
    for note in normalized.get("notes") or []:
        note_position = note.get("log_line")
        line = note.get("line")
        if (
            not isinstance(note_position, int)
            or isinstance(note_position, bool)
            or not 0 < note_position - position <= 6
            or not isinstance(line, int)
            or isinstance(line, bool)
            or line < 1
        ):
            continue
        # The normalizer retains only contiguous known diagnostic structure.
        # A six-line distance alone cannot cross a warning, wrapper command or
        # unknown output. Older normalized bundles lack this affirmative bind.
        if (
            primary.get("diagnostic_block") != position
            or note.get("diagnostic_block") != position
        ):
            continue
        if any(
            isinstance(error.get("log_line"), int)
            and position < error["log_line"] < note_position
            for error in normalized.get("all_errors") or []
        ):
            continue
        if any(
            item.get("value") == "boundary"
            and position < item["log_line"] < note_position
            for item in normalized.get("stage_observations") or []
        ):
            continue
        origin = _origin(note)
        if not _sdk_origin(origin, context):
            continue
        if hint:
            hint_name = hint.get("sym")
            if (
                _origin(hint) != origin
                or hint.get("line") != line
                or (
                    hint_name not in (None, "")
                    and (
                        not _declaration_name(hint_name)
                        or not _same_target(hint_name, target or symbol)
                    )
                )
            ):
                continue
        # A candidate message without a declaration identity is insufficient.
        # Retain both independent names and veto a mismatch in either; never let
        # a qualified call override a note that actually declares another name.
        identities = []
        malformed_identity = False
        for key in ("sym", "declaration_symbol"):
            raw_name = note.get(key)
            if raw_name in (None, ""):
                continue
            name = _declaration_name(raw_name)
            if name is None:
                malformed_identity = True
                break
            identities.append(name)
        if (
            malformed_identity
            or not identities
            or any(not _same_target(name, target or symbol) for name in identities)
        ):
            continue
        declaration_position = note.get("code_line_log_line")
        declaration_symbol = note.get("declaration_symbol")
        if declaration_symbol and (
            not isinstance(note.get("code_line"), str)
            or not note["code_line"].strip()
            or not isinstance(declaration_position, int)
            or isinstance(declaration_position, bool)
            or declaration_position <= note_position
        ):
            continue
        note_ref = {
            "kind": "note",
            "log_line": note_position,
            "file_original": origin,
            "line": line,
        }
        if declaration_symbol:
            note_ref.update(
                declaration_symbol=declaration_symbol,
                declaration_log_line=declaration_position,
            )
        association.update(
            status="log_bound_sdk_declaration",
            basis="bound_original_sdk_declaration",
            symbol=symbol,
            evidence_refs=[{"kind": "primary", "log_line": position}, note_ref],
        )
        break
    return result
