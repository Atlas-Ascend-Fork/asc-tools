# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Evidence-only cause tree. Domain knowledge is queried by the host skills.

This module never searches a knowledge index, downloads resources or infers an
API replacement from spelling. Query requests are questions, not evidence.
"""

from __future__ import annotations

import re
from dataclasses import asdict, dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any

import yaml


@dataclass
class Leaf:
    kind: str
    title: str
    actions: list[str]
    reference: str | None = None
    probe_names: list[str] | None = None
    evidence_kind: str | None = None
    applicability: str | None = None
    applicability_reasons: list[str] | None = None
    source_provenance: dict[str, Any] | None = None

    def as_dict(self) -> dict[str, Any]:
        return {key: value for key, value in asdict(self).items() if value is not None}


def _project_root() -> Path:
    # harness/engine/cause_tree.py → repo/skills/cause_tree.py → repo/
    return Path(__file__).resolve().parents[2]


@lru_cache(maxsize=1)
def _load_conditional_error_signatures() -> list[dict[str, Any]]:
    path = _project_root() / "harness/config/error_signatures.yaml"
    rules = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(rules, list) or not all(isinstance(rule, dict) for rule in rules):
        raise ValueError("Invalid diagnostic rule configuration")
    return [rule for rule in rules if rule.get("conditional_advice")]


_ERROR_CODE_RE = re.compile(
    r"\b("
    r"ACL_ERROR_[A-Z][A-Z0-9_]*"
    r"|ACL_RT_[A-Z][A-Z0-9_]*"
    r"|ACLNN_ERR_[A-Z][A-Z0-9_]*"
    r"|ACLDVPP_ERR_[A-Z][A-Z0-9_]*"
    r"|RT_ERROR_[A-Z][A-Z0-9_]*"
    r"|HCCL_E_[A-Z][A-Z0-9_]*"
    r"|GE_ERROR_[A-Z][A-Z0-9_]*"
    r"|OP_LOG_[A-Z][A-Z0-9_]*"
    r")\b"
)


def _collect_text_corpus(normalized: dict[str, Any]) -> str:
    """把 normalized 里所有可能含错误码/API 名的文本合并。"""
    parts: list[str] = []
    primary = normalized.get("primary_error") or {}
    for k in ("raw", "message"):
        v = primary.get(k)
        if isinstance(v, str):
            parts.append(v)
    for f in normalized.get("findings") or []:
        for k in ("message", "raw"):
            v = f.get(k)
            if isinstance(v, str):
                parts.append(v)
        for ev in f.get("evidence") or []:
            if isinstance(ev, str):
                parts.append(ev)
    for tail in normalized.get("tail_lines") or []:
        if isinstance(tail, str):
            parts.append(tail)
    return "\n".join(parts)


def extract_error_codes(text: str) -> list[str]:
    if not text:
        return []
    seen: dict[str, int] = {}
    for m in _ERROR_CODE_RE.finditer(text):
        code = m.group(1)
        seen[code] = seen.get(code, 0) + 1
    # 按出现次数 + 字典序排
    return sorted(seen.keys(), key=lambda c: (-seen[c], c))


_UNDECLARED_NAME_RE = re.compile(
    r"(?:use of undeclared identifier|unknown type name|no member named)\s+'([A-Za-z_]\w+)'"
)


_DEPRECATED_NAME_RE = re.compile(r"'([A-Za-z_]\w+)'\s+is deprecated")


_NS_MEMBER_RE = re.compile(
    r"no (?:member|template|type) named '([A-Za-z_]\w*)' in namespace '(AscendC[A-Za-z0-9_:]*)'"
)


def extract_namespace_member_names(text: str) -> list[str]:
    """Extract symbols reported as missing members of the AscendC namespace. This strict subset excludes generic include and type errors."""
    if not text:
        return []
    seen: dict[str, int] = {}
    for m in _NS_MEMBER_RE.finditer(text):
        n = m.group(1)
        seen[n] = seen.get(n, 0) + 1
    return sorted(seen, key=lambda n: (-seen[n], n))


def extract_undeclared_names(text: str) -> list[str]:
    if not text:
        return []
    seen: dict[str, int] = {}
    for m in _UNDECLARED_NAME_RE.finditer(text):
        n = m.group(1)
        seen[n] = seen.get(n, 0) + 1
    # 弃用符号也作为查询线索，不能从名字推断替代关系。
    for m in _DEPRECATED_NAME_RE.finditer(text):
        n = m.group(1)
        seen[n] = seen.get(n, 0) + 1
    return sorted(seen, key=lambda n: (-seen[n], n))


_NO_MATCH_FN_RE = re.compile(
    r"no matching (?:member )?function for call to '([A-Za-z_][\w:]*)'"
)


def extract_no_matching_function_names(text: str) -> list[str]:
    """Extract function names from a no-matching-function diagnostic for version and signature analysis."""
    if not text:
        return []
    seen: dict[str, int] = {}
    for m in _NO_MATCH_FN_RE.finditer(text):
        n = m.group(1).split("::")[-1]
        if n:
            seen[n] = seen.get(n, 0) + 1
    return sorted(seen, key=lambda n: (-seen[n], n))


def _build_compiler_probe_leaves(probes: dict[str, Any] | None) -> list[Leaf]:
    if not probes:
        return []
    interp = probes.get("interpretation") or {}
    likely = interp.get("likely")
    probe_entries = probes.get("probes") or []
    by_name = {p.get("name"): p for p in probe_entries if p.get("name")}
    out: list[Leaf] = []
    if likely == "optimization_sensitive_or_compiler_limit":
        names = [
            name for name in ("probe_O0", "probe_O2", "probe_O3") if name in by_name
        ]
        if names:
            out.append(
                Leaf(
                    kind="compiler_probe",
                    title="Recorded optimization probe observations; task binding unverified",
                    actions=[
                        "Reuse the supplied records for "
                        + ", ".join(names)
                        + "; no other probe execution is established.",
                        "Check the original diagnostic, translation unit, source state and complete recipe before interpreting a split; probe names or an interpretation alone do not bind it to this failure.",
                        "Changing optimization is a recipe change, not proof of a valid repair. Only run another probe if a specific unresolved hypothesis requires it; verify the repair against the original task.",
                    ],
                    probe_names=names,
                    evidence_kind="optimization_sensitivity",
                    reference="references/error-categories/compiler-internal.md",
                )
            )
    elif likely == "compiler_backend_after_preprocess":
        names = [
            name
            for name in ("probe_preprocess_only", "probe_direct_kernel_compile")
            if name in by_name
        ]
        if names:
            out.append(
                Leaf(
                    kind="compiler_probe",
                    title="Recorded preprocessing/compile probes; task binding unverified",
                    actions=[
                        "Reuse the supplied records for "
                        + ", ".join(names)
                        + "; absent records are not completed probes.",
                        "Check the translation unit, source state, command and preprocessing differences against the original failure before drawing a backend conclusion; preserve available evidence without requiring a repeat.",
                    ],
                    probe_names=names,
                    evidence_kind="preprocess",
                    reference="references/error-categories/compiler-internal.md",
                )
            )
    elif (
        likely == "reproducible_compile_failure"
        and "probe_direct_kernel_compile" in by_name
        and by_name["probe_direct_kernel_compile"].get("status") == "failed"
    ):
        out.append(
            Leaf(
                kind="compiler_probe",
                title="Recorded direct compiler failure; original-task equivalence unverified",
                actions=[
                    "Reuse the recorded failed probe and compare its diagnostic, source state, translation unit and full recipe with the original build before claiming reproduction.",
                    "A failed probe alone does not establish the original cause or a valid repair; acquire further evidence only for an unresolved hypothesis.",
                ],
                probe_names=["probe_direct_kernel_compile"],
                evidence_kind="direct_kernel_compile",
                reference="references/error-categories/compiler-internal.md",
            )
        )
    return out


def _build_conditional_contract_leaves(normalized: dict[str, Any]) -> list[Leaf]:
    primary = normalized.get("primary_error") or {}
    message = str(primary.get("message") or "")
    if not message:
        return []
    leaves = []
    for rule in _load_conditional_error_signatures():
        flags = 0 if rule.get("case_sensitive") else re.IGNORECASE
        if not rule.get("regex") or not re.search(rule["regex"], message, flags):
            continue
        leaves.append(
            Leaf(
                kind="compiler_contract_candidate",
                title="编译器消息对应的调用契约待核验",
                actions=[rule["conditional_advice"]],
                evidence_kind="compiler_diagnostic",
                applicability="unknown",
                applicability_reasons=rule.get("applicability_reasons")
                or ["call_contract_unverified"],
                source_provenance={
                    "kind": "compiler_diagnostic",
                    "rule_id": rule.get("id"),
                    "message": message,
                    "file": primary.get("file"),
                    "line": primary.get("line"),
                },
            )
        )
    return leaves


def extract_api_names(text: str) -> list[str]:
    """Extract explicitly qualified symbols without a maintained API roster."""
    return list(dict.fromkeys(re.findall(r"\bAscendC::([A-Za-z_]\w*)", text)))


def knowledge_request(
    symbol: str,
    *,
    namespace: str | None = None,
    cann_version: str | None = None,
    target: str | None = None,
    question: str | None = None,
    skill: str = "ascendc-api-knowledge-query",
) -> dict[str, Any]:
    """Describe an unresolved question; only the host may execute the query."""
    if not isinstance(symbol, str) or not symbol.strip():
        raise ValueError("a nonempty query subject is required")
    if skill not in {"knowledge-query", "ascendc-api-knowledge-query"}:
        raise ValueError("unsupported knowledge consumer")
    return {
        "skill": skill,
        "subject": symbol,
        "namespace": namespace,
        "cann_version": cann_version,
        "target": target,
        "platform": None,
        "status": "not_queried",
        "question": question
        or "Check the applicable declaration, header, call constraints and target support.",
        "required_evidence": [
            "card body and source",
            "version applicability",
            "current SDK declaration and effective command",
        ],
        "execution": "host_loads_skill_on_unresolved_gap",
    }


def build_cause_tree(
    candidates: list[dict[str, Any]],
    *,
    category: str,
    phase: str,
    normalized: dict[str, Any],
    probes: dict[str, Any] | None = None,
    max_secondary: int = 4,
    max_leaves: int = 8,
    include_category_advice: bool = False,
) -> dict[str, Any]:
    """Keep observed candidates and bounded query needs separate from conclusions.

    include_category_advice remains accepted for caller compatibility; broad
    category patch recipes have been retired. No domain tables are loaded.
    """
    if max_secondary < 0 or max_leaves < 1:
        raise ValueError("invalid cause tree budgets")
    primary = None
    secondary = []
    if candidates:
        head = candidates[0]
        primary = {
            key: head.get(key)
            for key in ("cause", "confidence", "evidence", "files", "rank")
        }
        primary["source"] = (
            "primary_error" if head.get("rank") == 1 else "static_finding"
        )
        secondary = [
            {
                key: item.get(key)
                for key in ("rank", "cause", "confidence", "evidence", "files")
            }
            for item in candidates[1 : 1 + max_secondary]
        ]
    text = _collect_text_corpus(normalized)
    codes = extract_error_codes(text)
    names = extract_api_names(text)
    undeclared = extract_undeclared_names(text)
    leaves = _build_compiler_probe_leaves(probes) + _build_conditional_contract_leaves(
        normalized
    )
    seen = set()
    unique = []
    for leaf in leaves:
        key = (leaf.kind, leaf.title)
        if key not in seen:
            seen.add(key)
            unique.append(leaf.as_dict())
    # Query only the primary diagnostic. Cascaded tail errors are not new tasks.
    error = normalized.get("primary_error") or {}
    message = str(error.get("message") or error.get("raw") or "")
    subjects = list(
        dict.fromkeys(
            extract_api_names(message) + extract_no_matching_function_names(message)
        )
    )
    requests = []
    if category in {
        "api_incompatible",
        "template_error",
        "kernel_compile_error",
        "host_compile_error",
        "soc_mismatch",
        "version_mismatch",
    }:
        subjects += [
            name
            for name in extract_namespace_member_names(message)
            if name not in subjects
        ]
        for subject in subjects[:3]:
            requests.append(
                knowledge_request(
                    subject,
                    cann_version=normalized.get("cann_version"),
                    target=normalized.get("target_soc"),
                )
            )
    if (
        not requests
        and message
        and (
            extract_error_codes(message)
            or category in {"soc_mismatch", "version_mismatch"}
            or any(leaf.kind == "compiler_contract_candidate" for leaf in leaves)
        )
    ):
        requests.append(
            knowledge_request(
                message,
                skill="knowledge-query",
                cann_version=normalized.get("cann_version"),
                target=normalized.get("target_soc"),
                question="Resolve the diagnostic's remaining knowledge gap against the card body and current build evidence; preserve the original target and computational contract.",
            )
        )
    return {
        "primary": primary,
        "secondary": secondary,
        "leaves": unique[:max_leaves],
        "scanned_error_codes": codes,
        "scanned_api_names": names,
        "scanned_undeclared_names": undeclared,
        "target_arch": None,
        "knowledge_requests": requests,
        "knowledge_provider": "cannbot-knowledge",
    }
