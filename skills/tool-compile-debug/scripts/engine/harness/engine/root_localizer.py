# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Locate upstream declaration candidates from source and compiler evidence."""

from __future__ import annotations

import re
from typing import Any

from harness.engine.common import line_number_for
from harness.engine.stage_evidence import _visible_source

# Index struct and class definitions together, retaining their source locations.
_STRUCT_RE = re.compile(r"\b(?:struct|class)\s+(\w+)")
_CONST_RE = re.compile(r"(?:constexpr\s+[\w:<>]+|#\s*define)\s+([A-Za-z_]\w*)")
_ENTRY_RE = re.compile(r"__global__\s+(?:__\w+__\s+)?void\s+([A-Za-z_]\w*)\s*\(")
# 跨文件 use-site 形态（与 _check_cross_file 一致）。
_TILING_USE_RE = re.compile(r"(?:__gm__\s+)?(?:const\s+)?(\w*TilingData)\b\s*\*")
_LAUNCH_RE = re.compile(r"\b([A-Za-z_]\w*)\s*<<<")
# Clang and GCC member diagnostics name the owning aggregate type. Namespace
# diagnostics do not identify an aggregate and are excluded from this lookup.
_MEMBER_OWNER_RES = (
    re.compile(r"no member named '([A-Za-z_]\w*)' in '([A-Za-z_]\w*)'"),
    re.compile(r"'([A-Za-z_]\w*)' has no member named '([A-Za-z_]\w*)'"),
)


def _norm(rel: str | None) -> str | None:
    return rel.replace("\\", "/") if rel else rel


def build_define_use_graph(texts: dict[str, str]) -> dict[str, Any]:
    """带 provenance 的 define 索引：symbol → (定义文件, 行)。

    返回 defs（struct/const/entry 三类），及派生的 tiling_header / entry_files 角色。
    """
    defs: dict[str, dict[str, tuple[str, int | None]]] = {
        "struct": {},
        "const": {},
        "entry": {},
    }
    entry_files: list[str] = []
    tiling_headers: list[str] = []
    candidates: dict[str, dict[str, list]] = {kind: {} for kind in defs}
    for rel, text in texts.items():
        if rel.lower().endswith(
            (".json", ".ini", ".yaml", ".yml", ".md", ".txt", ".cmake")
        ):
            continue
        text = _visible_source(text) or ""
        nrel = _norm(rel)
        for m in _STRUCT_RE.finditer(text):
            candidates["struct"].setdefault(m.group(1), []).append(
                (nrel, text.count("\n", 0, m.start()) + 1)
            )
        for m in _CONST_RE.finditer(text):
            candidates["const"].setdefault(m.group(1), []).append(
                (nrel, text.count("\n", 0, m.start()) + 1)
            )
        has_entry = False
        for m in _ENTRY_RE.finditer(text):
            candidates["entry"].setdefault(m.group(1), []).append(
                (nrel, text.count("\n", 0, m.start()) + 1)
            )
            has_entry = True
        if has_entry and nrel not in entry_files:
            entry_files.append(nrel)
        low = (nrel or "").lower()
        if "tiling" in low and low.endswith((".h", ".hpp")):
            tiling_headers.append(nrel)
    for kind, names in candidates.items():
        defs[kind] = {
            name: locations[0]
            for name, locations in names.items()
            if len(locations) == 1
        }
    return {
        "defs": defs,
        "definition_candidates": candidates,
        "ambiguities": {
            kind: {
                name: sorted(locations)
                for name, locations in names.items()
                if len(locations) > 1
            }
            for kind, names in candidates.items()
        },
        "entry_files": entry_files,
        "tiling_header": tiling_headers[0] if len(tiling_headers) == 1 else None,
        "tiling_headers": tiling_headers,
    }


def _near_miss_def(
    symbol: str,
    table: dict[str, tuple[str, int | None]],
    *,
    symptom_file: str | None = None,
    prefer_files: list[str] | None = None,
) -> tuple[str, int | None] | None:
    """Rank similarly named declarations by file role and common name prefix."""
    prefer_files = [_norm(f) for f in (prefer_files or [])]

    def _common_prefix_len(a: str, b: str) -> int:
        n = 0
        for x, y in zip(a, b):
            if x != y:
                break
            n += 1
        return n

    cands: list[tuple[str, tuple[str, int | None]]] = [
        (name, loc)
        for name, loc in table.items()
        if name != symbol and (name.startswith(symbol) or symbol.startswith(name))
    ]
    if not cands:
        return None
    if symptom_file is None and not prefer_files:
        return cands[0][1]  # Without file preferences, preserve declaration order.

    def _rank(item: tuple[str, tuple[str, int | None]]) -> tuple[int, int, int]:
        name, (rel, _ln) = item
        not_symptom = 0 if (symptom_file and _same_file(rel, symptom_file)) else 1
        preferred = 1 if rel in prefer_files else 0
        return (not_symptom, preferred, _common_prefix_len(name, symbol))

    best = max(cands, key=_rank)
    # 若最佳候选仍落在症状文件（无更好选择），返回 None 让上游走 tiling_header_fallback。
    if symptom_file and _same_file(best[1][0], symptom_file):
        return None
    return best[1]


# crossfile_* finding rule_id → 族标签
_RULE_FAMILY = {
    "crossfile_missing_tiling_type": "CX-T",
    "crossfile_shared_const_renamed": "CX-M",
    "crossfile_missing_entry_symbol": "CX-L",
}


def _broken_symbol_from_finding(finding: dict[str, Any]) -> str | None:
    ev = finding.get("evidence") or []
    if not ev:
        return None
    # Evidence may contain a tiling type, a constant, or a kernel launch expression.
    first = str(ev[0])
    mt = _TILING_USE_RE.search(first)
    if mt:
        return mt.group(1)
    ml = _LAUNCH_RE.search(first)
    if ml:
        return ml.group(1)
    m = re.search(r"\b([A-Za-z_]\w*)\b", first)
    return m.group(1) if m else None


def _shared_constant_matches_primary(
    finding: dict[str, Any], primary: dict[str, Any]
) -> bool:
    """A project-wide spelling heuristic cannot replace the compiler's first error."""
    if finding.get("rule_id") != "crossfile_shared_const_renamed":
        return True
    symbol = _broken_symbol_from_finding(finding)
    message = str(primary.get("message") or "")
    names = re.findall(
        r"(?:use of )?undeclared identifier\s+['‘]([A-Za-z_]\w*)['’]"
        r"|['‘]([A-Za-z_]\w*)['’]\s+was not declared in this scope",
        message,
    )
    return bool(symbol) and any(symbol in pair for pair in names)


def _member_owner_from_message(message: str) -> tuple[str, str] | None:
    """从 `no member named 'X' in 'ClassName'`（及 GCC 变体）抽出 (member, owner_class)。
    clang 形态 group(1)=member/group(2)=owner；GCC 形态 group(1)=owner/group(2)=member。
    命中 namespace 形态（`in namespace 'AscendC'`=API 不兼容非跨文件根）显式排除。"""
    if not message or "namespace" in message:
        return None
    m = _MEMBER_OWNER_RES[0].search(message)
    if m:
        return m.group(1), m.group(2)
    m = _MEMBER_OWNER_RES[1].search(message)
    if m:
        return m.group(2), m.group(1)
    return None


def _collapse(all_errors: list[dict[str, Any]], symbol: str) -> tuple[int, list[str]]:
    """Group diagnostics that mention the localized symbol at word boundaries.

    Different compiler message forms may refer to the same symbol. This groups
    those references for inspection; it does not prove they have one cause.
    """
    if not symbol:
        return 0, []
    pat = re.compile(rf"\b{re.escape(symbol)}\b")
    bucket = [e for e in all_errors if pat.search(e.get("message", ""))]
    files = sorted({_norm(e.get("file")) for e in bucket if e.get("file")})
    return len(bucket), files


_MEMBER_ACCESS_RE = re.compile(r"\b\w+\s*(?:->|\.)\s*([A-Za-z_]\w*)")
# A right-hand function call can supply a declaration lead for a return-type error.
_RHS_CALL_RE = re.compile(r"=\s*(?:[\w:]+::)?([A-Za-z_]\w*)\s*\(")


def _member_accesses_from_code(code_line: str) -> list[str]:
    """Extract field-access names in source order, excluding immediate calls."""
    out: list[str] = []
    for m in _MEMBER_ACCESS_RE.finditer(code_line):
        mem = m.group(1)
        # An immediately following parenthesis identifies a method call.
        tail = code_line[m.end() : m.end() + 1]
        if tail == "(":
            continue
        if mem not in out:
            out.append(mem)
    return out


# Match a data-member declaration such as ``<type> <member>[array] = init;``.
# Require a type token so assignments such as ``obj.member = value;`` are not
# mistaken for declarations.
_FIELD_DECL_TMPL = (
    r"^[ \t]*[\w:<>,][\w:<>,\s\*&]*?\b{member}\b[ \t]*"
    r"(?:\[[^\]]*\])?[ \t]*(?:=[^;]*)?;"
)


def _resolve_func_def_file(
    func: str,
    texts: dict[str, str],
    symptom_file: str | None,
    prefer: list[str] | None = None,
) -> tuple[str, int | None, str] | None:
    """Find a declaration candidate for a function used in an assignment.

    Prefer shared headers outside the symptom file. Return the candidate's file,
    line and symbol, or None when no distinct declaration is available.
    """
    prefer = [_norm(f) for f in (prefer or [])]
    # 函数定义/声明形态：行首有返回类型 token，然后 func( ；排除纯调用（前面是 `=` 或语句中间）。
    def_re = re.compile(
        rf"^[ \t]*(?:CATLASS_DEVICE\s+|__aicore__\s+|inline\s+|static\s+|constexpr\s+|__gm__\s+)*"
        rf"[\w:<>,\*&]+[ \t\*&]+{re.escape(func)}\s*\(",
        re.MULTILINE,
    )

    def _rank(rel: str) -> tuple[int, int, int]:
        nrel = _norm(rel)
        not_symptom = 0 if (symptom_file and _same_file(nrel, symptom_file)) else 1
        preferred = 1 if nrel in prefer else 0
        is_header = (
            1 if (nrel or "").lower().endswith((".h", ".hpp", ".hxx", ".cuh")) else 0
        )
        return (not_symptom, preferred, is_header)

    hits: list[tuple[str, int]] = []
    for rel, text in texts.items():
        m = def_re.search(text)
        if m:
            hits.append((rel, line_number_for(text, m.group(0).strip())))
    cands = [
        (r, ln) for r, ln in hits if not (symptom_file and _same_file(r, symptom_file))
    ]
    if not cands:
        return None
    best = max(cands, key=lambda x: _rank(x[0]))
    return (_norm(best[0]), best[1], func)


def _error_wants_pointer(message: str) -> bool:
    """Detect pointer wording to rank matching declaration candidates."""
    m = message.lower()
    return "pointer" in m or bool(re.search(r"type '[^']*\*", message))


def _resolve_member_def_file(
    members: list[str],
    texts: dict[str, str],
    symptom_file: str | None,
    prefer: list[str] | None = None,
    message: str = "",
) -> tuple[str, int | None, str] | None:
    """Rank field declarations outside the symptom file as localization leads.

    Prefer declarations whose pointer syntax matches the diagnostic, then the
    supplied preferred files and headers. Name/type agreement is a heuristic,
    not proof that a declaration is incorrect.
    """
    prefer = [_norm(f) for f in (prefer or [])]
    want_ptr = _error_wants_pointer(message)

    def _rank(item: tuple[str, int, str]) -> tuple[int, int, int, int]:
        rel, _ln, decl = item
        nrel = _norm(rel)
        not_symptom = 0 if (symptom_file and _same_file(nrel, symptom_file)) else 1
        # Prefer pointer declarations when the diagnostic explicitly mentions one.
        type_match = 1 if (want_ptr and "*" in decl) else 0
        preferred = 1 if nrel in prefer else 0
        is_header = (
            1 if (nrel or "").lower().endswith((".h", ".hpp", ".hxx", ".cuh")) else 0
        )
        return (not_symptom, type_match, preferred, is_header)

    for member in members:
        decl_re = re.compile(
            _FIELD_DECL_TMPL.format(member=re.escape(member)), re.MULTILINE
        )
        hits: list[tuple[str, int, str]] = []
        for rel, text in texts.items():
            m = decl_re.search(text)
            if m:
                hits.append(
                    (rel, line_number_for(text, m.group(0).strip()), m.group(0).strip())
                )
        if not hits:
            continue
        cands = [
            h for h in hits if not (symptom_file and _same_file(h[0], symptom_file))
        ]
        if not cands:
            continue
        best = max(cands, key=_rank)
        return (_norm(best[0]), best[1], member)
    return None


def _same_file(a: str | None, b: str | None) -> bool:
    """Compare source keys and logged paths conservatively across path roots.

    A basename fallback is used only when the paths use different coordinate
    systems; it can reject a valid candidate but cannot authorize a guess.
    """
    if not a or not b:
        return False
    if a == b:
        return True
    return a.replace("\\", "/").split("/")[-1] == b.replace("\\", "/").split("/")[-1]


def localize_root_cause(
    normalized: dict[str, Any],
    context: dict[str, Any] | None,
    static_findings: dict[str, Any] | None,
) -> dict[str, Any] | None:
    """Return an upstream declaration candidate for the primary diagnostic.

    Return None when the collected source does not provide a distinct candidate.
    Candidate fields retain the root_* protocol names; localization confidence
    does not prove which source change preserves the required behavior.
    """
    context = context or {}
    static_findings = static_findings or {}
    primary = normalized.get("primary_error") or {}
    symptom_file = _norm(primary.get("file"))

    # Cross-file localization requires more than one collected source file.
    files = context.get("files") or {}
    src = [
        f
        for f in (files.get("all_sources") or [])
        if not str(f).lower().endswith(".json")
    ]
    if len(src) <= 1:
        return None

    texts = dict(context.get("file_text") or {})
    graph = build_define_use_graph(texts)
    defs = graph["defs"]

    # Prefer structural findings that already identify a missing symbol.
    findings = [
        f
        for f in (static_findings.get("findings") or [])
        if str(f.get("rule_id", "")).startswith("crossfile_")
        and f.get("severity") == "error"
        and _shared_constant_matches_primary(f, primary)
    ]
    family = method = root_file = root_symbol = None
    root_line = None
    confidence = 0.0

    if findings:
        finding = findings[0]
        family = _RULE_FAMILY.get(finding["rule_id"], "CX-?")
        symbol = _broken_symbol_from_finding(finding) or ""
        if finding["rule_id"] == "crossfile_missing_tiling_type":
            loc = defs["struct"].get(symbol) or _near_miss_def(
                symbol,
                defs["struct"],
                symptom_file=symptom_file,
                prefer_files=graph["tiling_headers"],
            )
            if loc:
                root_file, root_line = loc
                method = "tiling_struct_def_site"
                confidence = 0.9
            elif graph["tiling_header"]:
                root_file = graph["tiling_header"]
                method = "tiling_header_fallback"
                confidence = 0.7
            root_symbol = symbol
        elif finding["rule_id"] == "crossfile_shared_const_renamed":
            loc = _near_miss_def(
                symbol,
                defs["const"],
                symptom_file=symptom_file,
                prefer_files=graph["tiling_headers"],
            ) or defs["const"].get(symbol)
            if loc:
                root_file, root_line = loc
                method = "const_def_site"
                confidence = 0.85
            elif graph["tiling_header"]:
                root_file = graph["tiling_header"]
                method = "tiling_header_fallback"
                confidence = 0.65
            root_symbol = symbol
        elif finding["rule_id"] == "crossfile_missing_entry_symbol":
            if graph["entry_files"]:
                root_file = graph["entry_files"][0]
                method = "entry_def_site_by_graph"
                confidence = 0.7
                ents = [n for n, (r, _l) in defs["entry"].items() if r == root_file]
                root_symbol = ents[0] if ents else symbol
            else:
                root_symbol = symbol

    # A compiler declaration note can provide a definition site even within the
    # symptom file. The final distance guard decides whether it adds a new locus.
    if root_file is None:
        hint = primary.get("root_hint")
        if hint and hint.get("file"):
            root_file = _norm(hint["file"])
            root_line = hint.get("line")
            root_symbol = hint.get("sym")
            method = "note_declared_here"
            confidence = 0.88
            family = family or "CX-S"

    # Missing tiling fields use a separate structural rule. The tiling header is
    # a candidate declaration site when the type exists but the member does not.
    if root_file is None:
        owner_pair = _member_owner_from_message(str(primary.get("message") or ""))
        header = graph["tiling_header"]
        header_text = _visible_source(texts.get(header, "")) if header else None
        owner_declaration = (
            re.search(
                r"\b(?:struct|class)\s+" + re.escape(owner_pair[1]) + r"\b"
                r"|\bBEGIN_TILING_DATA_DEF\s*\(\s*"
                + re.escape(owner_pair[1])
                + r"\s*\)",
                header_text,
            )
            if owner_pair and header_text is not None
            else None
        )
        field_findings = [
            f
            for f in (static_findings.get("findings") or [])
            if f.get("rule_id") == "tiling_field_mismatch"
            and f.get("severity") == "error"
            and owner_pair is not None
            and any(
                re.search(r"\." + re.escape(owner_pair[0]) + r"\b", str(e))
                or re.search(
                    r"member named ['‘]" + re.escape(owner_pair[0]) + r"['’]", str(e)
                )
                for e in (f.get("evidence") or [])
            )
            and (
                not f.get("file")
                or (symptom_file and _same_file(_norm(f.get("file")), symptom_file))
            )
        ]
        if field_findings and owner_declaration:
            root_file = header
            root_line = line_number_for(header_text, owner_declaration.group(0))
            method = "tiling_field_def_header"
            confidence = 0.8
            family = family or "CX-T.field"
            # root_symbol = 被引用但不存在的字段（用于 collapse 同根级联）
            ev = field_findings[0].get("evidence") or []
            for e in ev:
                mm = re.search(r"\.([A-Za-z_]\w*)\b", str(e)) or re.search(
                    r"member named '([A-Za-z_]\w*)'", str(e)
                )
                if mm:
                    root_symbol = mm.group(1)
                    break

    # Ordinary classes need no tiling naming convention: a compiler-provided
    # owner type can be resolved directly through the declaration index.
    if root_file is None:
        owner_pair = _member_owner_from_message(str(primary.get("message") or ""))
        if owner_pair:
            member, owner = owner_pair
            loc = defs["struct"].get(
                owner
            )  # struct/class 已并表（_STRUCT_RE 兼收 class）
            if loc and not _same_file(loc[0], symptom_file):
                root_file, root_line = loc
                root_symbol = member
                method = "member_owner_class_def_site"
                confidence = 0.82
                family = family or "CX-T.member"

    # Without a declaration note, a type-error source excerpt may identify a
    # consumed field or function. Follow that use to a candidate shared declaration.
    if root_file is None:
        msg_l = str(primary.get("message") or "").lower()
        type_mismatch = any(
            s in msg_l
            for s in (
                "cannot initialize",
                "incompatible type",
                "incompatible integer",
                "assigning to",
                "reinterpret_cast",
                "no viable conversion",
                "cannot assign",
            )
        )
        code_line = str(primary.get("code_line") or "")
        if type_mismatch and code_line:
            members = _member_accesses_from_code(code_line)
            root_field = _resolve_member_def_file(
                members,
                texts,
                symptom_file,
                prefer=graph["tiling_headers"],
                message=str(primary.get("message") or ""),
            )
            if root_field:
                root_file, root_line, root_symbol = root_field
                method = "member_type_backtrace"
                confidence = 0.78
                family = family or "CX-T.member_type"
            # A void-valued assignment can lead back to the called declaration.
            elif "void" in msg_l:
                cm = _RHS_CALL_RE.search(code_line)
                if cm:
                    root_fn = _resolve_func_def_file(
                        cm.group(1), texts, symptom_file, prefer=graph["tiling_headers"]
                    )
                    if root_fn:
                        root_file, root_line, root_symbol = root_fn
                        method = "func_return_type_backtrace"
                        confidence = 0.76
                        family = family or "CX-T.func_ret"

    # Same-file candidates add no separate locus unless a compiler declaration
    # note points at a sufficiently distant line.
    SAME_FILE_MIN_GAP = 40
    if not root_file:
        return None
    if _same_file(root_file, symptom_file):
        sym_line = primary.get("line")
        gap = abs((root_line or 0) - (sym_line or 0)) if (root_line and sym_line) else 0
        if not (method == "note_declared_here" and gap >= SAME_FILE_MIN_GAP):
            return None
        family = family or "CX-S.samefile_defsite"

    collapsed_count, collapsed_files = _collapse(
        normalized.get("all_errors") or [], root_symbol or ""
    )
    return {
        "root_file": root_file,
        "root_line": root_line,
        "root_symbol": root_symbol,
        "symptom_file": symptom_file,
        "family": family,
        "method": method,
        "confidence": confidence,
        "collapsed_count": collapsed_count,
        # A single matching diagnostic is not a cascade group.
        "collapse_status": "collapsed"
        if collapsed_count >= 2
        else "single_match_no_collapse",
        "collapsed_files": collapsed_files,
    }
