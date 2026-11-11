# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Build a structured cause tree from normalized diagnostics, source evidence, and the packaged knowledge base. Leaves carry evidence and advisory next actions; they never apply a patch."""

from __future__ import annotations

import re
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any, Iterable

try:
    import yaml  # type: ignore
except Exception:
    yaml = None

# Use the shared version policy for applicability and provenance filtering.
# version_gate 是版本判定的唯一权威（登记表驱动四态）；本模块只用它的两个纯判据
# （载荷→检测域、显式 since/until→冲突 reason）+ 无载荷时的 log 侧单源归一。
from harness.engine.common import KnowledgeBaseIntegrityError
from harness.engine.common import load_api_signature_kb
from harness.engine.version_gate import classify as _classify_version
from harness.engine.version_gate import kb_knowledge_domain as _kb_knowledge_domain
from harness.engine.version_gate import normalize_version as _normalize_version
from harness.engine.version_gate import version_field_blocked as _version_field_blocked

# -----------------------------
# KB 加载（惰性 + 缓存）
# -----------------------------


def _project_root() -> Path:
    # harness/engine/cause_tree.py → repo/skills/cause_tree.py → repo/
    return Path(__file__).resolve().parents[2]


@lru_cache(maxsize=1)
def _load_conditional_error_signatures() -> list[dict[str, Any]]:
    if yaml is None:
        return []
    path = _project_root() / "harness/knowledge/error_signatures.yaml"
    try:
        return [
            rule
            for rule in (yaml.safe_load(path.read_text(encoding="utf-8")) or [])
            if rule.get("conditional_advice")
        ]
    except (OSError, ValueError, yaml.YAMLError):
        return []


@lru_cache(maxsize=1)
def _load_error_codes() -> list[dict[str, Any]]:
    if yaml is None:
        return []
    path = _project_root() / "harness/knowledge/cann_error_codes.yaml"
    if not path.exists():
        return []
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or []
    except Exception:
        return []


@lru_cache(maxsize=1)
def _load_fix_hints() -> list[dict[str, Any]]:
    if yaml is None:
        return []
    path = _project_root() / "harness/knowledge/fix_hints.yaml"
    if not path.exists():
        return []
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or []
    except Exception:
        return []


@lru_cache(maxsize=1)
def _load_api_signatures() -> list[dict[str, Any]]:
    if yaml is None:
        return []
    try:
        return load_api_signature_kb(_project_root() / "harness/knowledge")
    except KnowledgeBaseIntegrityError:
        raise
    except Exception:
        return []


@lru_cache(maxsize=1)
def _load_fix_templates() -> list[dict[str, Any]]:
    if yaml is None:
        return []
    path = _project_root() / "harness/knowledge/fix_templates.yaml"
    if not path.exists():
        return []
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or []
    except Exception:
        return []


@lru_cache(maxsize=1)
def _load_api_version_map() -> list[dict[str, Any]]:
    if yaml is None:
        return []
    path = _project_root() / "harness/knowledge/api_version_map.yaml"
    if not path.exists():
        return []
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or []
    except Exception:
        return []


@lru_cache(maxsize=1)
def _load_soc_feature_matrix() -> dict[str, Any]:
    if yaml is None:
        return {}
    path = _project_root() / "harness/knowledge/soc_feature_matrix.yaml"
    if not path.exists():
        return {}
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except Exception:
        return {}


@lru_cache(maxsize=1)
def _load_operator_idioms() -> list[dict[str, Any]]:
    if yaml is None:
        return []
    path = _project_root() / "harness/knowledge/operator_idiom_kb.yaml"
    if not path.exists():
        return []
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or []
    except Exception:
        return []


@lru_cache(maxsize=1)
def _load_reserved_identifiers() -> dict[str, Any]:
    if yaml is None:
        return {}
    path = _project_root() / "harness/knowledge/reserved_identifiers.yaml"
    if not path.exists():
        return {}
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except Exception:
        return {}


def reset_kb_cache() -> None:
    """单测用：重置 KB lru_cache。"""
    _load_error_codes.cache_clear()
    _load_fix_hints.cache_clear()
    _load_api_signatures.cache_clear()
    _load_fix_templates.cache_clear()
    _load_api_version_map.cache_clear()
    _load_soc_feature_matrix.cache_clear()
    _load_operator_idioms.cache_clear()
    _load_reserved_identifiers.cache_clear()
    _load_conditional_error_signatures.cache_clear()


# -----------------------------
# 扫日志
# -----------------------------


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


_ASCENDC_API_RE = re.compile(
    # AscendC API 通常 PascalCase，长度 >=3，常见关键字
    r"\b(AscendC::)?(DataCopy|LocalTensor|GlobalTensor|TPipe|TQue|TBuf|Add|Sub|Mul|Compare|Duplicate|Brcb|Gather|GatherMask|Adds|Cast|Conv2D|Gemm|Fixpipe|Softmax|LayerNorm|MatMul|BatchNorm|ReduceMax|ReduceMin|ReduceSum|Mins|Maxs|Muls|Power|Reciprocal|Exp|Ln|Sqrt|Abs|AscendQuant)\b"
)


def extract_api_names(text: str) -> list[str]:
    if not text:
        return []
    seen: dict[str, int] = {}
    for m in _ASCENDC_API_RE.finditer(text):
        api = m.group(2)
        seen[api] = seen.get(api, 0) + 1
    return sorted(seen.keys(), key=lambda a: (-seen[a], a))


# -----------------------------
# Leaf 生成
# -----------------------------


@dataclass
class Leaf:
    kind: str  # error_code | api_signature | patch_template | category_hint
    title: str
    actions: list[str]
    fix_hint_id: str | None = None
    error_code: str | None = None
    api_name: str | None = None
    patch_kind: str | None = None
    reference: str | None = None
    probe_names: list[str] | None = None
    evidence_kind: str | None = None

    # 投影层据此区分「强结论可下用户裁决令」vs「弱提示仅供参考」。
    target_arch: str | None = None
    # 免得它从 title/actions 的中文散文里反解（散文解析是新的失败面）。
    wrong_name: str | None = None
    signature: str | None = None
    applicability: str | None = None
    applicability_reasons: list[str] | None = None
    namespace: str | None = None
    api_level: str | None = None
    signature_candidates: list[dict[str, Any]] | None = None
    signature_candidate_count: int | None = None
    signature_omitted_count: int | None = None
    source_provenance: dict[str, Any] | None = None

    def as_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "kind": self.kind,
            "title": self.title,
            "actions": list(self.actions),
        }
        for k in (
            "fix_hint_id",
            "error_code",
            "api_name",
            "patch_kind",
            "reference",
            "probe_names",
            "evidence_kind",
            "target_arch",
            "wrong_name",
            "signature",
            "applicability",
            "applicability_reasons",
            "namespace",
            "api_level",
            "signature_candidates",
            "signature_candidate_count",
            "signature_omitted_count",
            "source_provenance",
        ):
            v = getattr(self, k)
            if v or (
                k in {"signature_candidate_count", "signature_omitted_count"}
                and v is not None
            ):
                out[k] = v
        return out


def _build_error_code_leaves(
    error_codes: Iterable[str],
    *,
    kb_codes: list[dict],
    kb_hints: list[dict],
) -> list[Leaf]:
    if not kb_codes or not kb_hints:
        return []
    code_index = {c["code"]: c for c in kb_codes if c.get("code")}
    hint_index = {h["id"]: h for h in kb_hints if h.get("id")}
    out: list[Leaf] = []
    for code in error_codes:
        entry = code_index.get(code)
        if not entry:
            continue
        hint_id = entry.get("fix_hint_id")
        if not hint_id:
            continue
        hint = hint_index.get(hint_id)
        if not hint:
            continue
        actions = list(hint.get("suggested_actions") or [])
        out.append(
            Leaf(
                kind="error_code",
                title=f"{code} → {hint.get('title', '')}".strip(" →"),
                actions=actions[:5],
                fix_hint_id=hint_id,
                error_code=code,
                reference=hint.get("reference_doc"),
            )
        )
    return out


def _has_complete_template_head(signature: str) -> bool:
    """签名以完整（含嵌套尖括号）的 template 头开始即为可用声明形态。

    深度计数穿过嵌套（`template <typename T = Vec<half>>`）；截断头深度不归零
    返回 False。抽取器常把模板参数头塞进 return_type，此谓词用于豁免该污染形态。
    """
    m = re.match(r"template\s*<", signature)
    if not m:
        return False
    depth, i = 0, m.end() - 1
    for ch in signature[i:]:
        depth += (ch == "<") - (ch == ">")
        if depth == 0:
            return True
        if depth < 0:
            return False
    return False


def signature_quality_issues(entry: dict[str, Any]) -> list[str]:
    """Reject unusable declarations without inventing missing C++ or SDK facts.

    This checks extraction integrity, not whether a declaration is supported by
    the target compiler. Source/target applicability is checked separately.
    """
    name = str(entry.get("name") or "")
    signature = str(entry.get("signature") or "").strip()
    kind = entry.get("kind") or "function"
    if entry.get("quality_status") == "invalid_declaration":
        return ["invalid_declaration"]
    if not name or not signature:
        return ["missing_declaration"]
    if kind in {"class", "struct", "enum", "type_alias", "macro", "constexpr_constant"}:
        return []
    issues = []
    match = re.search(r"\b" + re.escape(name) + r"\s*\(", signature)
    if not match:
        return ["missing_function_declarator"]
    prefix = signature[: match.start()].strip()
    # A bare MACRO(args) or Call(args) has no return type. An orphaned
    # enable_if tail is not a declaration either (old DataCopy extraction).
    if (
        not prefix
        or re.match(r"^(?:[{};]|return\b)", prefix)
        or ("return_type" in entry and not entry["return_type"])
    ):
        issues.append("call_or_macro_invocation")
    # 赋值右侧的调用（`int h = Ceil(...)`、`mIter = ConstCeil(...)`）是调用点不是声明：
    # 成员调用 `pipe.InitBuffer(...)`、流插入 `cout << Ceil(...)` 同族）。控制流关键字开头
    # 的 `if (Ceil(...))` / `assert(Ceil(...))` 同为调用点。
    # >> 结束且 prefix 内含 <；流提取（cin >> x）的 >> 前无未闭合 <。<</./-> 无此歧义。
    _shift_kill = prefix.rstrip().endswith(("=", ".", "->", "<<")) or (
        prefix.rstrip().endswith(">>") and "<" not in prefix
    )
    if _shift_kill or re.match(
        r"^(?:if|for|while|switch|else|return|assert|ASSERT|CHECK)\b", prefix
    ):
        issues.append("call_or_macro_invocation")
    non_type = re.compile(
        r"=\s|\\param|#\w|^\s*(?:private|public|protected):|\*/", re.IGNORECASE
    )
    if non_type.search(
        str(entry.get("return_type") or "")
    ) and not _has_complete_template_head(signature):
        issues.append("non_type_return")
    if re.match(r"^(?:private|public|protected):|#\w", prefix):
        issues.append("non_type_return")
    if re.match(r"(?:bool|true|false|typename|class)\s*>|^\w+\s*>::", prefix):
        issues.append("truncated_template")
    for left, right in (("(", ")"), ("[", "]")):
        depth = 0
        for ch in signature:
            depth += (ch == left) - (ch == right)
            if depth < 0:
                break
        if depth != 0:
            issues.append("unbalanced_declaration")
            break
    if "..." in signature or "…" in signature:
        issues.append("incomplete_or_variadic_signature")
    # 同一调用点形态可能同时命中多条规则（如空 return_type + 流插入前缀），去重保持输出稳定。
    return sorted(set(issues))


def _api_layer(entry: dict[str, Any]) -> str:
    namespace = str(entry.get("namespace") or "")
    header = str(entry.get("header") or "").replace("\\", "/")
    if (
        "MicroAPI" in namespace
        or namespace.endswith("::Reg")
        or "/micro_api/" in header
        or "/reg_compute/" in header
    ):
        return "micro"
    if "simt_api/" in header:
        return "simt"
    return str(entry.get("api_level") or "unknown")


def _failed_call_namespace(symbol: str, primary: dict) -> str | None:
    """Use an unambiguous qualified call echoed for this diagnostic only.

    This bounded syntax check does not resolve aliases, macros, overloads or
    argument types. Complex expressions keep the existing unknown context.
    """
    message = str(primary.get("message") or "")
    names = re.findall(r"['‘]([A-Za-z_]\w*(?:::\w+)*)['’]", message)
    if not any(name.split("::")[-1] == symbol for name in names):
        return None
    line = primary.get("code_line")
    if not isinstance(line, str) or any(c in line for c in "\r\n\\\"'#"):
        return None
    # Do not borrow a namespace mentioned in a comment or another same-name
    # expression; the diagnostic column need not name the final identifier.
    if (
        "/*" in line
        or "//" in line
        or len(re.findall(r"\b" + re.escape(symbol) + r"\b", line)) != 1
    ):
        return None
    call = re.match(
        r"\s*(?:return\s+)?(?:::)?(?P<namespace>AscendC(?:::[A-Za-z_]\w*)*)::"
        + re.escape(symbol)
        + r"\s*(?:<[^;<>]*>)?\s*\(",
        line,
    )
    return call.group("namespace") if call else None


def _detected_version_domain(normalized: dict | None) -> tuple[int, int] | None:
    """Build the version domain used to filter version-sensitive candidates.

    Prefer an existing version compatibility payload; otherwise normalize the
    logged toolkit version. Unknown versions use the registry floor as a
    conservative baseline. The domain filters applicability only; name-existence
    checks use the complete signature universe."""
    vc = (normalized or {}).get("version_compatibility")
    if not isinstance(vc, dict):
        vc = _classify_version((normalized or {}).get("cann_version"), None)
    return _kb_knowledge_domain(vc)


def _symbol_context(symbol: str, text: str, normalized: dict | None = None) -> dict:
    """Use explicit observations; an unqualified name does not imply AscendC."""
    normalized = normalized or {}
    supplied = normalized.get("api_context") or {}
    context = dict(supplied) if supplied.get("symbol") == symbol else {}
    namespaces = set(
        re.findall(
            r"no member named '" + re.escape(symbol) + r"' in namespace '([^']+)'", text
        )
    )
    namespaces.update(
        re.findall(
            r"\b((?:AscendC::)?MicroAPI|AscendC(?:::\w+)*)::"
            + re.escape(symbol)
            + r"\b",
            text,
        )
    )
    failed_namespace = _failed_call_namespace(
        symbol, normalized.get("primary_error") or {}
    )
    if failed_namespace:
        # The corpus can contain unrelated notes/tail diagnostics. An explicit
        # failed call supplies its own namespace before any display limit.
        namespaces = {failed_namespace}
    if namespaces:
        if len(namespaces) == 1:
            observed = next(iter(namespaces))
            if context.get("namespace") and context["namespace"] != observed:
                context["context_conflict"] = True
            context["namespace"] = observed
        else:
            context["context_conflict"] = True
    if context.get("namespace") in {"AscendC::MicroAPI", "AscendC::Reg"}:
        context.setdefault("api_level", "micro")
    elif context.get("namespace") == "AscendC::Simt":
        context.setdefault("api_level", "simt")
    context.setdefault("cann_version", normalized.get("cann_version"))
    context.setdefault("cann_build", normalized.get("cann_build"))
    context.setdefault("target_arch", normalized.get("_knowledge_target_arch"))

    # 与来源判据继承章通道共用。直呼 _signature_selection 的手造 context 不带它
    # → 过滤不激活（存量测试与调用方零扰动）。
    domain = _detected_version_domain(normalized)
    if domain is not None:
        context.setdefault("version_domain", domain)
    return context


def _inherited_from_domain(source: dict) -> tuple[int, int] | None:
    """Return the version declared by a source-provenance record, if present.

    ``source_provenance.inherited_from`` 为 mapping 时取 version/cann_version 键，
    A string is treated as a version directly; malformed or missing values
    return ``None``.
    """
    inherited = source.get("inherited_from")
    if isinstance(inherited, dict):
        inherited = inherited.get("version") or inherited.get("cann_version")
    if not inherited:
        return None
    return _normalize_version(inherited)


def _source_version_accepted(source: dict, context: dict) -> bool:
    """Select source-version evidence using exact matches and declared knowledge-base version domains."""
    inherited = _inherited_from_domain(source)
    if inherited is not None:
        detected = context.get("version_domain")
        if detected is not None:
            return detected >= inherited
    return str(source.get("cann_version")) == str(context.get("cann_version"))


def _signature_selection(
    entries: list[dict], context: dict
) -> tuple[list[dict], str, list[str]]:
    """Select compatible declarations conservatively. Missing facts remain unknown, and exact argument matching does not replace compiler overload resolution."""
    usable, rejected = [], []
    version_blocked: list[str] = []
    for entry in entries:
        issues = signature_quality_issues(entry)
        if issues and issues != ["incomplete_or_variadic_signature"]:
            rejected.extend(issues)
            continue
        namespaces = {entry.get("namespace"), *(entry.get("namespace_aliases") or [])}
        if (
            context.get("namespace")
            and entry.get("namespace")
            and context["namespace"] not in namespaces
        ):
            continue
        if (
            context.get("api_level")
            and _api_layer(entry) != "unknown"
            and context["api_level"] != _api_layer(entry)
        ):
            continue
        args = context.get("argument_types")
        params = entry.get("params")
        if isinstance(args, list) and isinstance(params, list):
            required = sum("=" not in p and p != "void" for p in params)
            if not required <= len(args) <= len(params):
                continue
            # Container families differ even when element types are templated.
            conflict = False
            for arg, param in zip(args, params):
                families = ("LocalTensor", "GlobalTensor", "RegTensor", "MaskReg")
                af = next((f for f in families if f in str(arg)), None)
                pf = next((f for f in families if f in param), None)
                if af and pf and af != pf:
                    conflict = True
            if conflict:
                continue
        arch = context.get("target_arch")
        if (
            arch
            and entry.get("arch_scope") is not None
            and arch not in entry["arch_scope"]
        ):
            continue

        # 冲突 → 不进适用性候选。只认显式字段（未标=按 kb_meta 默认 9.0.0 起一直
        # version_blocked：全部候选被版本挡下时顶替 no_compatible_declaration 出示
        # Keep the leaf for evidence, but exclude unknown applicability from
        # fast paths; the knowledge evidence remains visible.
        blocked = _version_field_blocked(entry, context.get("version_domain"))
        if blocked:
            version_blocked.append(blocked)
            continue
        usable.append(entry)
    if not usable:
        return (
            [],
            "unknown",
            sorted(set(rejected + version_blocked) or ["no_compatible_declaration"]),
        )
    reasons = []
    if len(usable) != 1:
        reasons.append("overload_unresolved")
    if context.get("context_conflict"):
        reasons.append("context_conflict")
    for entry in usable:
        reasons.extend(signature_quality_issues(entry))
        for field in (
            "namespace",
            "api_level",
            "argument_types",
            "target_arch",
            "cann_version",
            "cann_build",
            "include_entrypoints",
        ):
            if not context.get(field):
                reasons.append("missing_" + field)
        source = entry.get("source_provenance") or {}
        if not all(
            source.get(k)
            for k in ("cann_version", "package_timestamp", "header_sha256", "line")
        ):
            reasons.append("unverified_source")
        elif not _source_version_accepted(source, context):
            reasons.append("source_version_mismatch")
        elif str(source["package_timestamp"]) != str(context.get("cann_build")):
            reasons.append("source_build_mismatch")
        if entry.get("arch_scope") is None:
            reasons.append("unverified_arch_scope")
        if context.get("namespace") in (entry.get("namespace_aliases") or []):
            if context.get("namespace") not in (
                entry.get("verified_namespace_aliases") or []
            ):
                reasons.append("namespace_alias_visibility_unverified")
        required_headers = entry.get("include_entrypoints") or []
        if not required_headers or not set(required_headers).intersection(
            context.get("include_entrypoints") or []
        ):
            reasons.append("include_entrypoint_unverified")

        if entry.get("verified_argument_types") != context.get(
            "argument_types"
        ) or not context.get("argument_types"):
            reasons.append("argument_types_unverified")
    return usable, "unknown" if reasons else "matched", sorted(set(reasons))


def _signature_leaf_fields(entries: list[dict], context: dict) -> dict:
    selected, applicability, reasons = _signature_selection(entries, context)
    candidates = [
        {
            k: entry[k]
            for k in (
                "namespace",
                "header",
                "signature",
                "api_level",
                "kind",
                "source_provenance",
                "guard_conditions",
                "namespace_aliases",
                "namespace_alias_provenance",
            )
            if k in entry
        }
        for entry in selected[:3]
    ]
    chosen = selected[0] if len(selected) == 1 else {}
    return dict(
        signature=chosen.get("signature"),
        reference=chosen.get("header"),
        namespace=chosen.get("namespace"),
        api_level=_api_layer(chosen),
        applicability=applicability,
        applicability_reasons=reasons,
        signature_candidates=candidates,
        signature_candidate_count=len(selected),
        signature_omitted_count=max(0, len(selected) - len(candidates)),
        source_provenance=chosen.get("source_provenance"),
    )


def query_api_signatures(
    symbol: str,
    *,
    namespace: str | None = None,
    api_level: str | None = None,
    cann_version: str | None = None,
    cann_build: str | None = None,
    target_arch: str | None = None,
    limit: int = 5,
) -> dict:
    """Thin KB query: no project scan, environment probe, build or repair action.

    Reuse diagnostic selection before display limiting. A query supplies filters,
    not compiler-observed argument/visibility facts, so matches remain candidates.
    """
    if not symbol.strip() or not 1 <= limit <= 50:
        raise ValueError("symbol must be nonempty and limit must be between 1 and 50")
    symbol = symbol.strip().lstrip(":")
    if "::" in symbol:
        qualified_namespace, symbol = symbol.rsplit("::", 1)
        if namespace and namespace != qualified_namespace:
            raise ValueError("qualified symbol conflicts with --namespace")
        namespace = qualified_namespace
    context = {
        k: v
        for k, v in {
            "symbol": symbol,
            "namespace": namespace,
            "api_level": api_level,
            "cann_version": cann_version,
            "cann_build": cann_build,
            "target_arch": target_arch,
        }.items()
        if v is not None
    }
    # No version observation means no hard since/until filtering. Do not pretend
    # an unknown SDK is the knowledge floor; expose the missing fact instead.
    context["version_domain"] = _normalize_version(cann_version)
    resolution = _classify_version(cann_version, None)["resolution"]
    if namespace in {"AscendC::MicroAPI", "AscendC::Reg"}:
        context.setdefault("api_level", "micro")
    elif namespace == "AscendC::Simt":
        context.setdefault("api_level", "simt")
    entries = [entry for entry in _load_api_signatures() if entry.get("name") == symbol]
    selected, applicability, reasons = _signature_selection(entries, context)
    if resolution != "supported":
        reasons = sorted(set(reasons + ["version_" + resolution]))
        applicability = "unknown"
    fields = (
        "name",
        "namespace",
        "api_level",
        "kind",
        "header",
        "signature",
        "params",
        "source_provenance",
        "guard_conditions",
        "arch_scope",
        "namespace_aliases",
        "namespace_alias_provenance",
        "include_entrypoints",
        "since",
        "until",
    )
    return {
        "query": context,
        "version_resolution": resolution,
        "matching_symbol_count": len(entries),
        "candidate_count": len(selected),
        "filtered_count": len(entries) - len(selected),
        "omitted_count": max(0, len(selected) - limit),
        "candidates": [
            {k: entry[k] for k in fields if k in entry} for entry in selected[:limit]
        ],
        "applicability": applicability,
        "applicability_reasons": reasons,
        "absence_proven": False,
        "next_evidence": (
            "Check current SDK declarations/docs and exact namespace; an empty KB query does not prove absence."
            if not selected
            else "Bind the actual command, SDK/header provenance, includes and argument types before choosing a repair."
        ),
    }


def _build_api_signature_leaves(
    api_names: Iterable[str],
    *,
    kb_apis: list[dict],
    max_per_api: int = 1,
    text: str = "",
    normalized: dict | None = None,
) -> list[Leaf]:
    if not kb_apis:
        return []
    by_name: dict[str, list[dict]] = {}
    for a in kb_apis:
        n = a.get("name")
        if n:
            by_name.setdefault(n, []).append(a)
    out: list[Leaf] = []
    for api in api_names:
        sigs = by_name.get(api) or []
        if sigs and max_per_api:
            fields = _signature_leaf_fields(
                sigs, _symbol_context(api, text, normalized)
            )
            out.append(
                Leaf(
                    kind="api_signature",
                    title=f"对照 AscendC API 签名：{api}",
                    actions=[
                        f"{api}：按完整 namespace、调用层、实参类型和实际 include 入口核对声明候选；短名命中不证明重载适用。",
                        "未能匹配的证据：" + ", ".join(fields["applicability_reasons"])
                        if fields["applicability_reasons"]
                        else "当前实例与已验证声明条件匹配。",
                    ],
                    api_name=api,
                    # Keep both API-signature channels structurally identical so
                    # downstream consumers can use the evidence fields safely.
                    **fields,
                )
            )
    return out


# 从 build.log 抓"未声明标识符"名（编译器报的**错名**），供 KB near-name 反向匹配。
# bisheng/clang 形态：`use of undeclared identifier 'X'` / `unknown type name 'X'`。
_UNDECLARED_NAME_RE = re.compile(
    r"(?:use of undeclared identifier|unknown type name|no member named)\s+'([A-Za-z_]\w+)'"
)

# `'GetLength' is deprecated:`（clang）或 `'X' is deprecated`（gcc）——不含 "undeclared"，
# 上面的正则抓不到。单列一条抓 deprecated 符号，并入 undeclared 候选走同一 KB(api_version_map)匹配。
_DEPRECATED_NAME_RE = re.compile(r"'([A-Za-z_]\w+)'\s+is deprecated")

# 而那个命名空间确实存在且已被找到（否则编译器会先报 namespace 不存在）。
# 反例（都不能据此判"API 不存在"）：
#   `unknown type name 'X'`            → X 是类型/关键字，最常见真因是**少了 #include**
#   `use of undeclared identifier 'X'`  → 同上，也可能是命名空间写错导致的连带错
# （最高频的被冤符号是编译器关键字 `__aicore__`，11 次）。加这道门控后误判归零。
_NS_MEMBER_RE = re.compile(
    r"no member named '([A-Za-z_]\w*)' in namespace '(AscendC[A-Za-z0-9_:]*)'"
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
    # 并入 deprecated 符号（-Werror=deprecated 下的硬 error），走同一 KB 版本映射匹配。
    for m in _DEPRECATED_NAME_RE.finditer(text):
        n = m.group(1)
        seen[n] = seen.get(n, 0) + 1
    return sorted(seen, key=lambda n: (-seen[n], n))


# 符号**已声明**（否则报 undeclared），三形态正则（_UNDECLARED_NAME_RE）覆盖不了它，
# 跨版本映射（api_version_map 的归并迁移对，如旧归约族 → 新归一化接口）在该形态
# 完全不可达。等价正则与 log_normalizer.NO_MATCH_FN_RE 同源（多认 `member ` 变体）。
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


def _kb_affix_root(bad: str, kb_names: set[str], min_root_len: int = 3) -> str | None:
    """Find the longest indexed prefix at an uppercase suffix boundary.

    This is a name-search heuristic, not an API existence or rename proof.
    Namespace, declaration integrity and call applicability are separate gates.
    """
    best: str | None = None
    for name in kb_names:
        if (
            len(name) < min_root_len
            or not bad.startswith(name)
            or len(bad) <= len(name)
        ):
            continue
        if not bad[len(name) :][:1].isupper():
            continue  # 非驼峰边界 → 不是"真名+后缀"结构
        if best is None or len(name) > len(best):
            best = name
    return best


def _build_kb_near_name_leaves(
    undeclared: Iterable[str],
    *,
    kb_apis: list[dict],
    cutoff: float = 0.78,
    max_leaves: int = 3,
    affix_exact_only: set[str] | None = None,
    text: str = "",
    normalized: dict | None = None,
) -> list[Leaf]:
    """Suggest a likely intrinsic when an undeclared identifier closely matches a knowledge-base entry. The result is advisory and requires source and version evidence."""
    import difflib

    if not kb_apis:
        return []
    names: dict[str, list[dict]] = {}
    for a in kb_apis:
        n = a.get("name")
        issues = signature_quality_issues(a)
        # Keep weak/empty historical entries as lookup references, but never
        # turn extracted calls or orphaned template tails into callable names.
        if n and not set(issues).intersection(
            {
                "invalid_declaration",
                "call_or_macro_invocation",
                "truncated_template",
                "unbalanced_declaration",
                "missing_function_declarator",
                "non_type_return",
            }
        ):
            names.setdefault(n, []).append(a)
    kb_name_list = list(names)
    name_set = set(names)
    out: list[Leaf] = []
    for bad in undeclared:
        if bad in names:
            continue  # 错名恰好是 KB 真名 → 非拼写问题，不建议
        # Search by prefix relation before fuzzy spelling. Neither relation
        # establishes applicability; a fully matched call contract is required.
        affix_ok = affix_exact_only is None or bad in affix_exact_only
        affix_root = _kb_affix_root(bad, name_set) if affix_ok else None
        if affix_root:
            real, exact = affix_root, True
        else:
            close = difflib.get_close_matches(bad, kb_name_list, n=1, cutoff=cutoff)
            if not close:
                continue
            real, exact = close[0], False
        fields = _signature_leaf_fields(
            names[real], _symbol_context(bad, text, normalized)
        )
        if "no_compatible_declaration" in fields["applicability_reasons"]:
            continue
        if exact:
            title = f"'{bad}' 与已收录名 '{real}' 存在前后缀关系；需核对调用适用性"
            lead = f"候选名称：{real}；先核 namespace、调用层、实参、头入口与目标，不因名字相近改写计算。"
        else:
            title = f"未声明 '{bad}' 疑为 AscendC API '{real}' 的拼写/版本误写"
            lead = f"候选 intrinsic：{real}，需从当前工具链声明确认。"
        out.append(
            Leaf(
                kind="api_signature",
                title=title,
                actions=[
                    a
                    for a in (
                        lead,
                        "完整声明候选见 signature_candidates；多条候选尚未完成重载选择。",
                        "未能匹配的证据：" + ", ".join(fields["applicability_reasons"])
                        if fields["applicability_reasons"]
                        else "当前实例与已验证声明条件匹配。",
                    )
                    if a
                ],
                api_name=real,
                # kb_affix_exact = 结构等式命中（高置信，可机械应用）；
                # kb_near_name   = difflib 近似（低置信，只作参考）。
                evidence_kind=(
                    "kb_affix_exact"
                    if fields["applicability"] == "matched"
                    else "kb_name_candidate"
                )
                if exact
                else "kb_near_name",
                wrong_name=bad,
                **fields,
            )
        )
        if len(out) >= max_leaves:
            break
    return out


def _version_display(normalized: dict | None) -> str:
    """检测域的可读版本串（版本域降级文案用）。优先载荷 detected；无显式证词时
    按检测域（unknown→floor，按 kb_knowledge_domain 的判定）披露并注明判定依据；
    检测域不可解 → 「未知版本」。"""
    vc = (normalized or {}).get("version_compatibility") or {}
    detected = vc.get("detected")
    if isinstance(detected, dict):  # 冲突载荷 {log, env}：按问题发生地（log 侧）
        detected = detected.get("log")
    if detected:
        return str(detected)
    domain = _detected_version_domain(normalized)
    if domain is not None:
        return f"{domain[0]}.{domain[1]}（未探得版本，按支持下限域判）"
    return "未知版本"


def _build_api_version_leaves(
    undeclared: Iterable[str],
    *,
    kb_version_map: list[dict],
    max_leaves: int = 3,
    text: str = "",
    normalized: dict | None = None,
) -> list[Leaf]:
    """Offer historical migration candidates scoped by namespace and version.

    Check both sides of a migration against their declared version boundaries.
    Treat a candidate as advisory until current declarations and semantic
    equivalence have been verified.
    """
    if not kb_version_map:
        return []
    by_old = {e.get("old_name"): e for e in kb_version_map if e.get("old_name")}
    domain = _detected_version_domain(normalized)
    out: list[Leaf] = []
    for bad in undeclared:
        entry = by_old.get(bad)
        if not entry:
            continue
        context = _symbol_context(bad, text, normalized)
        if (
            context.get("namespace")
            and entry.get("namespace")
            and context["namespace"] != entry["namespace"]
        ):
            continue
        repl = entry.get("replacement") or ""
        kind_tag = entry.get("kind", "renamed")
        note = entry.get("note", "")
        since_raw = entry.get("since")
        since_dom = _normalize_version(since_raw)
        rm_raw = entry.get("removed_in")
        rm_dom = _normalize_version(rm_raw)
        downgrade: list[str] = []
        if domain is not None and since_dom is not None and since_dom > domain:
            downgrade.append("version_mapping_requires_newer_domain")
        if domain is not None and rm_dom is not None and rm_dom > domain:
            downgrade.append("version_mapping_old_name_still_present")
        if downgrade:
            detected_shown = _version_display(normalized)
            actions: list[str] = []
            if "version_mapping_requires_newer_domain" in downgrade:
                actions.append(
                    f"版本域不符：该映射的新名自 CANN {since_raw} 起才存在；当前检测 {detected_shown}，"
                    f"旧名 '{bad}' 在本域仍合法，勿迁——此报错在本域另有原因（include 入口/命名空间/"
                    "实参契约），先按当前域的真实声明排查。"
                )
                gate_version = since_raw
            else:
                actions.append(
                    f"版本域不符：历史记录标记旧名 '{bad}' 自 CANN {rm_raw} 起移除；当前检测 "
                    f"{detected_shown}，旧名在本域仍合法，勿迁——此报错在本域另有原因，先按当前域的"
                    "真实声明排查。"
                )
                gate_version = rm_raw
            actions.append(
                f"该映射仅在 CANN ≥{gate_version} 域适用；确需跨版本迁移时，先升级 toolkit 或以目标"
                "版本头文件核实后再动代码。"
            )
            if note:
                actions.append("历史来源备注（当前实例尚未核验）：" + note)
            out.append(
                Leaf(
                    kind="api_version_drift",
                    title=f"'{bad}' 的跨版本映射在本版本域不适用（{kind_tag}）",
                    actions=actions,
                    api_name=repl or bad,
                    evidence_kind="api_version_map",
                    namespace=entry.get("namespace"),
                    applicability="unknown",
                    applicability_reasons=[
                        *downgrade,
                        "version_mapping_requires_current_declaration",
                        "receiver_and_argument_contract_unverified",
                    ],
                    reference=entry.get("provenance"),
                )
            )
            if len(out) >= max_leaves:
                break
            continue
        repl_sig = entry.get("replacement_signature") or ""
        repl_conf = entry.get("replacement_confidence") or ""
        actions = []
        if repl:
            # inferred（据头文件相邻声明推断）如实标注，不伪装成官方确定映射。
            suffix = (
                "（据头文件相邻声明推断，建议核对）" if repl_conf == "inferred" else ""
            )
            actions.append(
                f"历史版本映射候选：{repl}{suffix}；核实际声明、对象类型与新旧参数契约后再迁移"
            )
        else:
            actions.append(
                "历史记录标记废弃/拟移除；核当前 build 的声明与原调用语义，不能据未声明错误直接删除调用"
            )
        if repl_sig:
            actions.append(f"新签名（注意改传参，非仅改名）：{repl_sig}")
        if note:
            actions.append("历史来源备注（当前实例尚未核验）：" + note)
        rm = entry.get("removed_in")
        if rm and rm != "unknown":
            actions.append(f"历史移除记录：{rm}；相对版本标记不是准确不可用版本")
        out.append(
            Leaf(
                kind="api_version_drift",
                title=f"'{bad}' 的跨版本映射待核验（{kind_tag}）",
                actions=actions,
                api_name=repl or bad,
                evidence_kind="api_version_map",
                namespace=entry.get("namespace"),
                applicability="unknown",
                applicability_reasons=[
                    "version_mapping_requires_current_declaration",
                    "receiver_and_argument_contract_unverified",
                ],
                reference=entry.get("provenance"),
            )
        )
        if len(out) >= max_leaves:
            break
    return out


def _build_kb_declaration_leaves(
    undeclared: Iterable[str],
    *,
    kb_apis: list[dict],
    max_leaves: int = 3,
    text: str = "",
    normalized: dict | None = None,
) -> list[Leaf]:
    """Emit declaration evidence when an undeclared symbol exactly matches a knowledge-base entry."""
    if not kb_apis:
        return []
    names: dict[str, list[dict]] = {}
    for a in kb_apis:
        n = a.get("name")
        issues = signature_quality_issues(a)
        # 同近名通道的质量门：弱/残缺历史条目只作查找参考，不出声明证据。
        if n and not set(issues).intersection(
            {
                "invalid_declaration",
                "call_or_macro_invocation",
                "truncated_template",
                "unbalanced_declaration",
                "missing_function_declarator",
                "non_type_return",
            }
        ):
            names.setdefault(n, []).append(a)
    out: list[Leaf] = []
    for bad in undeclared:
        entries = names.get(bad)
        if not entries:
            continue
        context = _symbol_context(bad, text, normalized)
        fields = _signature_leaf_fields(entries, context)
        headers = sorted({str(e.get("header")) for e in entries if e.get("header")})
        since_vals = sorted({str(e.get("since")) for e in entries if e.get("since")})
        actions: list[str] = [
            f"未声明名 '{bad}' 在 KB 声明索引中有收录（非拼写错误，不建议改名）——'未声明'指向"
            "声明不可达：先核 include 入口与版本域是否覆盖该声明，再核命名空间与调用层。"
        ]
        facts: list[str] = []
        if headers:
            facts.append("声明头入口：" + "；".join(headers))
        if since_vals:
            facts.append(
                "版本域记录（since）："
                + "；".join(since_vals)
                + "；检测域更低时该声明不在当前面内"
            )
        brief = next((str(e.get("brief")) for e in entries if e.get("brief")), "")
        if brief:
            facts.append("KB 摘要：" + brief)
        actions.extend(facts)
        reasons = fields.get("applicability_reasons") or []
        actions.append(
            "未能匹配的证据：" + ", ".join(reasons)
            if reasons
            else "当前实例与已验证声明条件匹配。"
        )
        out.append(
            Leaf(
                kind="api_signature",
                title=f"未声明名 '{bad}' 命中已收录声明：核 include 入口与版本域（非改名）",
                actions=actions,
                api_name=bad,
                evidence_kind="kb_declaration",
                # 声明级事实（头入口）必须跨过版本降级仍可见：fields.reference 只在
                # Prefer filtered candidates; retain the first header as a
                # fallback when version or architecture evidence blocks a match.
                reference=fields.get("reference") or (headers[0] if headers else None),
                **{k: v for k, v in fields.items() if k != "reference"},
            )
        )
        if len(out) >= max_leaves:
            break
    return out


# 从 normalized 日志文本反解目标 SoC/arch（build.log 带 `SOC_VERSION=...` / `--npu-arch=...` token）。
_SOC_TOKEN_RE = re.compile(
    r"(?:SOC_VERSION\s*=\s*|--npu-arch=|npu[_-]?arch[^\w]+)((?:Ascend|Kirin)\w+|(?:dav-)?\d{3,4})",
    re.IGNORECASE,
)


def _detect_target_arch(text: str, soc_matrix: dict) -> tuple[str | None, str | None]:
    """返回 (arch_code, soc_name)；任一未知为 None。
    优先匹配 build.log 的 `SOC_VERSION=...`/`--npu-arch=...` token；也接受**裸 SoC/arch 名**
    （normalizer 已抓出的 target_soc 是裸名，如 'Ascend910B1' / '2201'）。"""
    if not text or not soc_matrix:
        return None, None
    aliases = soc_matrix.get("soc_aliases") or {}
    arch_codes = soc_matrix.get("arch_codes") or {}

    def _resolve(tok: str) -> tuple[str | None, str | None]:
        tok = re.sub(r"^dav-", "", tok, flags=re.IGNORECASE)
        if tok in aliases:
            return aliases[tok], tok
        if tok in arch_codes:
            return tok, (arch_codes[tok] or {}).get("soc")
        # 前缀匹配 Ascend910B2→Ascend910B：按别名长度降序（最长匹配优先），
        # 防未来加入更短前缀（如 'Ascend910'）时 'Ascend910B2' 误配到更泛的前缀。
        for alias in sorted(aliases, key=len, reverse=True):
            if tok.startswith(alias):
                return aliases[alias], tok
        return None, None

    # 1) 带前缀的 token（日志原文，最可靠）
    for m in _SOC_TOKEN_RE.finditer(text):
        arch, name = _resolve(m.group(1))
        if arch:
            return arch, name
    # 2) 裸 SoC 名（normalizer 抓的 target_soc）——只认 Ascend* 形态，**不**认裸数字，
    #    避免把错误消息里的行号/数字误当 arch 码。
    for m in re.finditer(r"\b(?:Ascend|Kirin)\w+\b", text):
        arch, name = _resolve(m.group(0))
        if arch:
            return arch, name
    return None, None


def _build_soc_arch_gate_leaves(
    undeclared: Iterable[str],
    *,
    soc_matrix: dict,
    target_arch: str | None,
    max_leaves: int = 3,
) -> list[Leaf]:
    """报错的未声明符号若是**被目标 arch #if 摘除**的 arch 门控 intrinsic（soc_feature_matrix.yaml），
    出示"X 在 <目标 arch> 不可用，仅 <这些 arch> 提供"。这是裸 undeclared 零线索错误的领域知识——
    编译器不会说"此 API 仅某 SoC 存在"。"""
    features = (soc_matrix or {}).get("features") or []
    if not features:
        return []

    # 新 arch 摘除族），exclude 型条目（对当前 arch 的知识更精——softmax 族带
    # 「逐头直入」正解）排在宽聚合族之前，让裁决令 next[0] 带上更精修法，
    # 而非聚合族的「根本不存在/手写分解」过度断言。
    features = sorted(
        features, key=lambda f: 0 if f.get("available_arch_excludes") is not None else 1
    )
    out: list[Leaf] = []
    emitted: set[str] = set()
    undeclared_set = list(undeclared)
    for feat in features:
        names = set(feat.get("names") or [])
        if not names:
            continue
        hit = [n for n in undeclared_set if n in names and n not in emitted]
        if not hit:
            continue
        avail = feat.get("available_arch")
        excludes = feat.get("available_arch_excludes")
        # 只有当目标 arch 明确**不**在可用集（或在排除集）时才报，避免误伤
        gated_out = False
        if target_arch is not None:
            if avail is not None and target_arch not in avail:
                gated_out = True
            if excludes is not None and target_arch in excludes:
                gated_out = True
        elif avail is not None or excludes is not None:
            # 目标 arch 未知：给"可能 arch 门控"的弱提示（仍有价值，但标注不确定）
            gated_out = True
        if not gated_out:
            continue
        if feat.get("evidence_scope") == "header_visibility":
            for name in hit:
                out.append(
                    Leaf(
                        kind="soc_arch_gate",
                        title=f"{name} 的聚合头导出条件需核对（目标 arch {target_arch or '未知'}）",
                        actions=[
                            "聚合头未导出不等于完整API在此目标不可用；先查实际9.0头、namespace和逐头入口。",
                            str(feat.get("note") or ""),
                        ],
                        api_name=name,
                        target_arch=target_arch,
                        evidence_kind="header_visibility",
                        applicability="unknown",
                        applicability_reasons=[
                            "include_entrypoint_unverified",
                            "full_api_scope_unverified",
                        ],
                        reference=feat.get("provenance"),
                    )
                )
                emitted.add(name)
                if len(out) >= max_leaves:
                    return out
            continue
        arch_phrase = (
            f"目标 arch {target_arch}"
            if target_arch
            else "当前目标 arch（未能从日志解析，请核对 SOC_VERSION）"
        )
        avail_phrase = (
            f"仅 __NPU_ARCH__ {avail} 提供"
            if avail
            else f"在 __NPU_ARCH__ {excludes} 上被摘除"
        )
        actions = [
            f"'{', '.join(hit[:4])}' 属 {feat.get('group', 'arch_gated')} 族，{avail_phrase}",
            feat.get("note", ""),
        ]
        out.append(
            Leaf(
                kind="soc_arch_gate",
                title=f"未声明符号疑为 arch 门控 intrinsic（{feat.get('group', 'arch_gated')}，{arch_phrase}不可用）",
                actions=[a for a in actions if a],
                evidence_kind="soc_feature_matrix",
                api_name=hit[0],
                target_arch=target_arch,
            )
        )
        emitted.update(hit)
        if len(out) >= max_leaves:
            break
    return out


# 普通重定义域不归本 leaf）；T3 宏形态（宏展开不可见，只能按"expected identifier/
# unqualified-id 群发且症状文件在 CANN 安装树"做结构判定）。
_RESERVED_REDEF_RE = re.compile(
    r"error:\s+redefinition of '([A-Za-z_][A-Za-z0-9_]*)' as different kind of symbol"
)
_EXPECTED_ID_LINE_RE = re.compile(
    r"^(?P<path>[^\s:]+):(?P<line>\d+):(?P<col>\d+): error: expected (?:identifier|unqualified-id)",
    re.MULTILINE,
)
_CANN_TREE_PATH_RE = re.compile(r"/Ascend/")


def _build_reserved_identifier_leaves(
    text: str,
    *,
    kb_reserved: dict | None,
    max_leaves: int = 2,
) -> list[Leaf]:
    """Detect reserved-name collisions in kernel translation units and return evidence for a safe rename."""
    if not kb_reserved:
        return []
    entries = kb_reserved.get("identifiers") or []
    if not entries:
        return []
    by_name = {e.get("name"): e for e in entries if e.get("name")}
    meta = kb_reserved.get("meta") or {}
    header = meta.get("header") or "cce_aicore_intrinsics.h"
    enum_name = meta.get("header_enum") or "mem_t"

    out: list[Leaf] = []
    # T1：重定义 + 名字在保留清单（精确命中）
    seen: set[str] = set()
    for m in _RESERVED_REDEF_RE.finditer(text):
        name = m.group(1)
        if name not in by_name or name in seen:
            continue
        seen.add(name)
        out.append(
            Leaf(
                kind="reserved_identifier",
                title=f"'{name}' 是 CANN 保留标识符（{enum_name} 枚举成员，{header}）——kernel TU 内全局重定义必炸",
                actions=[
                    f"把 '{name}'（定义+所有使用点）改成非保留名，如 {name}_LIMIT",
                    "不要改 CANN 头文件——保留名是框架全局枚举，改调用处/改名才是解",
                    f"另注意：不重定义而直接用 '{name}' 会静默解析为枚举值 {by_name[name].get('value')}（编过但语义错）",
                ],
                evidence_kind="reserved_identifiers",
                api_name=name,
                reference=header,
            )
        )
        if len(out) >= max_leaves:
            return out
    # T3：expected identifier / unqualified-id 且症状文件在 CANN 安装树。
    # 该形态基本只剩"TU 宏状态污染了头"一种解释（宏撞保留名是主因）。
    if len(out) >= max_leaves:
        return out
    toolkit_hits = [
        m
        for m in _EXPECTED_ID_LINE_RE.finditer(text)
        if _CANN_TREE_PATH_RE.search(m.group("path"))
    ]
    if toolkit_hits:
        names = ", ".join(by_name)
        sample = toolkit_hits[0].group("path")
        out.append(
            Leaf(
                kind="reserved_identifier",
                title=(
                    f"expected identifier/unqualified-id 落在 CANN 安装树头（{sample}）——"
                    f"疑为 #define 撞 CANN 保留标识符（候选：{names}）"
                ),
                actions=[
                    '检查共享头/编译单元里先于 #include "kernel_operator.h" 的 #define 与全局常量名',
                    f"撞名候选（{enum_name} 枚举成员，{header}）：{names}",
                    "不要改 CANN 头；多点撞名时逐个改名后重编（修一个还剩其余）",
                ],
                evidence_kind="reserved_identifiers",
                reference=header,
            )
        )
    return out


def _build_hallucinated_api_leaves(
    undeclared: Iterable[str],
    *,
    kb_apis: list[dict],
    kb_version_map: list[dict],
    soc_matrix: dict,
    idioms: list[dict],
    text: str = "",
    cutoff: float = 0.78,
    max_leaves: int = 3,
) -> list[Leaf]:
    """Detect names absent from the knowledge base and known API families, then expose uncertainty and nearby idioms without inventing a fix."""
    import difflib

    if not idioms:  # 无配方库就不产幻觉 leaf（避免空建议）
        return []
    real_names = {a.get("name") for a in (kb_apis or []) if a.get("name")}
    version_old = {
        e.get("old_name") for e in (kb_version_map or []) if e.get("old_name")
    }
    gated_names: set[str] = set()
    for feat in (soc_matrix or {}).get("features") or []:
        gated_names.update(feat.get("names") or [])
    kb_name_list = list(real_names)

    out: list[Leaf] = []
    for bad in undeclared:
        if bad in real_names or bad in version_old or bad in gated_names:
            continue  # 真名/已知废弃/arch 门控 → 由其它 leaf 处理，非幻觉
        # difflib 有近名 → 拼写/版本误写（kb_near_name 已处理），非幻觉。**先于** tiling 跳过判，
        # 使带近名的 tiling-ish 名（如 LayoutTiling→真名）仍交近名通道，不被类型名跳过误吞。
        if kb_name_list and difflib.get_close_matches(
            bad, kb_name_list, n=1, cutoff=cutoff
        ):
            continue

        # 通道处理，**不是**幻觉。漏了这一跳会同时输出"真名是 Mins"和"MinsPad 是你臆造的"
        # （difflib 那道跳过拦不住它：`MinsPad`~`Mins` 相似度 0.727 < cutoff 0.78。）
        if _kb_affix_root(bad, set(real_names)):
            continue
        # 排除"是类型名/tiling 字段"的未声明（缺头/缺字段，非幻觉 API）；
        # 但若它精确命中惯用配方意图（_match_idiom），仍按幻觉给配方。
        if re.search(r"tiling|data$|param", bad, re.IGNORECASE) and not _match_idiom(
            bad, idioms
        ):
            continue
        idiom = _match_idiom(bad, idioms)
        if not idiom:
            # 不在配方库 → 仍可标记为疑似幻觉，但只给"确认 API 是否存在"的弱建议
            out.append(
                Leaf(
                    kind="hallucinated_api",
                    title=f"'{bad}' 未被当前KB覆盖，存在性仍待工具链确认",
                    actions=[
                        f"'{bad}' 未命中当前签名索引；索引不完整，不能据此判定API不存在。",
                        "检查准确版本的实际声明、namespace和include入口；先排除缺头/层次错误，再考虑替代实现。",
                    ],
                    api_name=bad,
                    evidence_kind="hallucinated_api",
                    applicability="unknown",
                    applicability_reasons=["not_in_signature_index"],
                )
            )
            if len(out) >= max_leaves:
                break
            continue
        # 命中配方库 → 给惯用分解
        actions: list[str] = [
            f"'{bad}' 未命中当前声明索引；名称可能对应 '{idiom.get('intent')}'，需核实际API入口。",
        ]
        rh = idiom.get("real_high_level") or {}
        if rh.get("name"):
            cn = rh.get("casing_note") or ""
            actions.append(
                f"若用高层 API，真实名是 {rh['name']}（头 {rh.get('header', '?')}）{('；' + cn) if cn else ''}"
            )
            if rh.get("arch_note"):
                actions.append(rh["arch_note"])
        decomp = idiom.get("decomposition") or []
        if decomp:
            uses = idiom.get("uses") or []
            actions.append(
                f"数学参考（{', '.join(uses)} 的实际重载与可用性需另核）："
                + " | ".join(decomp[:6])
            )
            actions.append(
                "分解前固定dtype、shape/尾块、临时缓冲、同步与误差容差，并独立验证；此配方不是可直接替换的API调用。"
            )
        out.append(
            Leaf(
                kind="hallucinated_api",
                title=f"'{bad}' 待确认；'{idiom.get('intent')}' 的声明入口与数学参考",
                actions=actions,
                api_name=(rh.get("name") or bad),
                evidence_kind="hallucinated_api",
                applicability="unknown",
                applicability_reasons=[
                    "not_in_signature_index",
                    "decomposition_preconditions_unverified",
                ],
            )
        )
        if len(out) >= max_leaves:
            break
    return out


def _match_idiom(symbol: str, idioms: list[dict]) -> dict | None:
    """把幻觉符号名匹配到惯用分解配方：先精确别名，再 intent 子串。"""
    low = symbol.lower()
    for idiom in idioms:
        aliases = [a.lower() for a in (idiom.get("aliases") or [])]
        if symbol in (idiom.get("aliases") or []) or low in aliases:
            return idiom
    for idiom in idioms:
        intent = (idiom.get("intent") or "").lower()
        if intent and intent in low:
            return idiom
    return None


def _build_template_leaves(category: str, *, kb_templates: list[dict]) -> list[Leaf]:
    out: list[Leaf] = []
    if not kb_templates:
        return out
    for t in kb_templates:
        applies = t.get("applies_to") or []
        if category in applies:
            tmpl_text = t.get("template", "").strip()
            out.append(
                Leaf(
                    kind="patch_template",
                    title=f"补丁模板：{t.get('id', 'unknown')}",
                    actions=[tmpl_text[:400]] if tmpl_text else [],
                    patch_kind=t.get("patch_kind"),
                )
            )
    return out


def _build_category_hint_leaf(category: str, phase: str) -> Leaf | None:
    mapping = {
        "cmake_config": (
            "CMake 配置阶段问题",
            [
                "复核 CMakeLists.txt 是否 find_package(ASC REQUIRED) 与 project LANGUAGES ASC",
                "检查 set_env.sh 是否 source",
                # '…​.dir/<src>.o(.d)': No such file` = 目标 build 目录未生成——真因在
                # 注册失配不在源码：①target 是否注册进父目录 CMakeLists 的构建列表
                # （add_subdirectory/EXAMPLES 列表）②SOURCES 列出的文件是否真实存在
                # ③ARCH/SOC 关键字是否与选项匹配。修 CMakeLists 注册，不要改源码。
                "error opening '*.o(.d)' / build 目录未生成 → 查 CMakeLists 注册失配（父列表漏 target / SOURCES 含不存在文件 / ARCH 键错配），修注册而非源码",
            ],
        ),
        "header_missing": (
            "头文件缺失",
            [
                "确认 #include 路径",
                'Ascend C kernel 加 #include "kernel_operator.h"',
                # 如 detail_*.hpp / *_tiling.hpp）时——①grep 工程内同名/近名文件实际位置
                # （可能被改名或移动）②include 的目录前缀是否与实际层级一致
                # ③CMake include_directories 是否覆盖该目录。多数正解是修 include 路径
                # 或找回被改名的文件，不是装系统库。
                "工程自有头（detail_*/自定义 .hpp）缺失 → grep 工程找同名或近名文件，修 include 路径/找回改名文件，而非找系统库",
            ],
        ),
        "link_error": (
            "链接错误",
            [
                "补 target_link_libraries 或修 LD_LIBRARY_PATH",
                # 导出的 kernel launch 函数是 C 符号——测试代码里声明不加 extern "C" 会被
                # C++ 名字修饰改名 → undefined reference to 'aclrtlaunch_*'。
                'aclrtlaunch_* 未定义引用 → 把测试代码里的 launch 声明包进 extern "C" {}（host_stub 导出的是 C 符号）',
            ],
        ),
        "template_error": (
            "C++ 模板/重载错误",
            [
                "先读候选声明、模板实例化链与拒绝原因；显式模板参数或转换须符合值域、符号、dtype/布局和地址空间契约，不能任意窄化或强转来凑签名",
                # X 不在 CANN 头/api_signatures（工程自命名——tile plan/形状别名/
                # 项目本地结构）→ **定义被删而非 API 不存在**——grep 工程使用点按上下文
                # 重建定义段；对 tile plan/形状类结构注意成对约束（如 L1/L0 的 M/N 两轴
                # 必须相等）。勿去 CANN 头/命名空间里猎该符号（那是白费轮次的经典路径）。
                "no template/no type named '<工程自命名>' → 先核定义、include、namespace 与生成步骤；若确认工程自有定义丢失，再根据使用点及原始契约重建（形状/tile plan 类注意成对约束），未命中 KB 不能证明定义被删",
            ],
        ),
        "api_incompatible": (
            "AscendC API 不兼容",
            [
                "对照 api_signatures.yaml 的签名调整调用形式",
                # 静态断言 = 请求的模板特化不存在（"can not find the specialization" /
                # "not implemented for this DispatchPolicy"）。58 例 fullrun 7 实例。
                "requirement 'DEPENDENT_FALSE<…>' 特化门 → 核模板实参（尤其是 MX scale/tile 类型）、实际特化声明与当前目标 arch 条件；修法须保留用户目标和运算契约，不能换无关 arch 绕过失败",
                # 含 MmadAtlasA2Pingpong/Preload/FA/MLA 族与 950 系 MmadAscend950FullLoadA）。
                "not implemented for this DispatchPolicy / policy 相关特化缺失 → DispatchPolicy 决定 kernel staging，与 BlockMmad/Epilogue 的组合必须有特化——对照该例程原始 policy（族谱真源：catlass include 的 gemm/dispatch_policy.hpp 与 epilogue/dispatch_policy.hpp，勿凭记忆自创 policy 名）",
            ],
        ),
        "kernel_compile_error": (
            "Kernel 编译错误",
            [
                "核失败单元的实际编译命令、目标与原配方；已有同状态日志时复用，只有未决假设需要时才补受控复现",
                "重建/校验 MX scale plan 时：先区分同名 MX 类型的 namespace、元素类型、存储布局与模板特化。MX_SCALE_GROUP_NUM 描述共享 scale 分组，逻辑组数按实际定义做 ceil；RoundUp<2> 仅适用于声明要求该存储对齐的布局，不能从 MX 短名推定。核对 host 长度、kernel 步长、尾块和独立数值契约，避免把配对错误同时写到两端",
                # 工程自定义类型须从声明与消费点恢复，不能仅凭名字推断。
                "重建 tile plan/形状契约（从使用点反推）：参考形态为 `using L1Shape = Shape<Int<M>,Int<N>,Int<K>>;` 与 `using L0Shape = Shape<Int<M>,Int<N>,Int<K>>;` 成对，再核 TileCopy/DispatchPolicy 的模板位。数值预算须核实际 catlass 依赖的 Catlass::Arch::<ArchTag>；当前参考树 L1_SIZE=512K、L0A/L0B=64K，L0C 的 Ascend950=256K、AtlasA2=128K 仅作定位线索。若已确认符号属工程自命名，SDK 中 rg 找不到是正常现象；先查工程定义、生成链及原始契约，证据不足时不猜形状或重建常量",
                # 仍 0 修）——真源：gemm/dispatch_policy.hpp 的 PRELOAD_STAGES 与
                # block_mmad_flash_attention_{qk,pv}*.hpp 的 CrossCoreFlag/prefixSum stages。
                "重建 staging/调度契约（kMax…Stages / PRELOAD 之类 staging 深度值位常量）：与 DispatchPolicy 选型绑定——先读 catlass include 的 gemm/dispatch_policy.hpp 对应 policy 的 PRELOAD_STAGES 形态，FA 族跨核同步位（CrossCoreFlag / prefixSumL0A/L0BStages）真源在 gemm/block/block_mmad_flash_attention_{qk,pv}*.hpp，勿凭空造深度值",
                # 按具体诊断核适用前提，避免把候选建议当成确定修法。
                "calling a __host__ function from a __aicore__ function → 核 helper 函数体及下游调用是否适用于目标设备；纯设备计算缺限定时才按声明补齐 __aicore__，Host 资源管理或系统接口留在 Host 并传递所需结果，不能只加修饰绕过调用域矛盾",
                "'__gm__ T*' 不能用 GM_ADDR 初始化 → 核生产方地址空间、真实元素类型、长度和对齐，事实支持时才转换为对应 GM 指针；强转不搬运数据、不改变物理存储类型，也不证明地址有效",
                "unknown type name 'half'; did you mean 'half2' → 核实际入口、类型声明与目标条件；近名不证明元素/向量宽度、布局和重载语义等价，原契约需要 half 时恢复合法可见性，不能直接换成 half2",
                "__VEC_SCOPE__ 内 Induction variable must have a type uint16_t → 当前调用确有该约束时核循环上界、步长、回绕和覆盖范围，再选合法类型或分块实现；不能截窄后遗漏元素或使循环无法退出",
                "static assertion ... tque's depth is zero → 深度零本身并非非法；按当前声明核 LocalTensor 引用式 AllocTensor/DeQue 与原资源、流水契约，不能只为编过增加队列深度",
                "no matching member function for call to 'InitBuffer' → 按实际对象、参数类型和原容量核重载；9.0 的 TBuf (buf, lenBytes)、队列 (que, num, lenBytes) 和 tuple 地址形式不能混用，不猜缓冲数量或长度",
                "no template named 'is_same'; did you mean 'std::is_same' → 原意确为标准类型比较且实参匹配时，按使用位置选择 std::is_same<T,U> 类型或其 ::value 布尔值，并显式包含 <type_traits>；单独补头不把 std 名字引入当前作用域，此错也不限定在 kernel",
                "use of undeclared identifier 'sqrtf'; did you mean 'sqrt' → 核当前调用域、类型及数值需求；它们与当前 SDK 的 device float sqrt(float) 声明相符时可用全局 sqrt，不能自动加 AscendC:: 或推广到其他类型、精度需求",
                "use of undeclared identifier 'Simt' → 核当前 namespace、设备入口、准确 CANN build 与实际 target 的 SIMT 声明及实例支持；报错本身不能证明无等价实现。保留计算/同步契约，禁止仅为编过删除语义调用",
                "FixpipeParams 缺成员 → 先匹配目标的 Fixpipe 重载与参数类型；9.0 的 FixpipeParamsV220 有公开成员及五参构造，{nSize, mSize, srcStride, dstStride, reluEn} 是构造调用而非聚合字段顺序。不能认定成员赋值不可用或跨目标强套 V220；保留布局、量化、dtype 和步长单位",
            ],
        ),
        "host_compile_error": (
            "Host 侧编译错误",
            [
                "检查 Host include 路径与 ge::Tensor 等签名",
                "Host TU 中设备事件或 PIPE_FIX 名称不可见 → 先核实际编译路由和调用域，以及原代码承担的同步、副作用与资源契约；设备调用放回合法设备路径，确属 Host 的同步按当前 Host API 实现，不能仅删除引用或忽略必要同步",
            ],
        ),
        "tiling_consistency": (
            "Tiling 字段不一致",
            [
                "连起 Host 生产方、启动实参、Kernel 形参和读取方式，核按值对象、设备指针或序列化缓冲的真实表示，再对齐共享布局与访问契约",
                "GET_TILING_DATA 附近名字未声明 → 核首错、作用域、当前注册方式与实际生成链；变量名不证明缺生成头。已确认依赖生成头时再核生成与 include，按值传入时不自造解包兼容宏",
            ],
        ),
        "schema_consistency": (
            "算子描述不一致",
            [
                "核生成输入、注册声明、实际生成调用与消费签名；只修已确认的同源错配并重新生成，不假定固定文件组合或参数数量，不只改会被覆盖的产物"
            ],
        ),
        "compiler_limit": (
            "触发编译器限制",
            [
                "尝试 -O0/-O2 切换；分拆大函数",
                # `L1_TILE_M == L0_TILE_M && L1_TILE_N == L0_TILE_N`（消息尾
                # "…basic blocks of L1 and L0 differ on the m and n axes is not
                # supported yet"）。约束知识：M/N 两轴 L1 与 L0 必须相等，K 轴可不同。
                # 根因多为工程侧 tile plan 定义段（成对的 L1/L0 形状别名）被删/被改。
                # 58 例 fullrun 18 实例；恢复内容是工程自定义契约（不入 KB），
                "L1/L0 tile 断言（L1_TILE_M == L0_TILE_M…）→ 工程侧 tile plan 定义段（成对的 L1/L0 形状别名）被删或被改——M/N 两轴 L1 与 L0 必须相等（K 轴可不同）；grep 工程 tile plan / 形状定义恢复之，不是编译器开关问题",
            ],
        ),
        "compiler_internal": ("疑似编译器内部错误", ["生成最小复现给昇腾团队"]),
        "environment": ("环境配置问题", ["核 ASCEND_TOOLKIT_HOME / SOC_VERSION"]),
        # 通用 manual 建议。分支提示覆盖文件名过长（FAQ 真源）与产物缺失两家族。
        "package_error": (
            "打包阶段失败",
            [
                "tar 报 file name is too long → 缩短生成名：OpType（op_kernel/op_host 文件名由它生成）/ Comment 分组名 / CMakePresets 的 vendor_name",
                "json 未生成 / kernel 产物缺失 → 打包步骤没看到上游构建产物，先修上一步构建",
            ],
        ),
    }
    if category not in mapping:
        return None
    title, actions = mapping[category]
    return Leaf(
        kind="category_hint",
        title=title,
        actions=list(actions),
    )


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
                    reference="docs/design/ccec_bisheng_probe_evidence.md",
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
                    reference="docs/design/ccec_bisheng_probe_evidence.md",
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
                reference="docs/design/ccec_bisheng_probe_evidence.md",
            )
        )
    return out


# -----------------------------
# 主入口
# -----------------------------


def _build_conditional_contract_leaves(normalized: dict[str, Any]) -> list[Leaf]:
    primary = normalized.get("primary_error") or {}
    message = str(primary.get("message") or "")
    if not message:
        return []
    leaves = []
    for rule in _load_conditional_error_signatures():
        if not rule.get("regex") or not re.search(
            rule["regex"], message, re.IGNORECASE
        ):
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


def build_cause_tree(
    candidates: list[dict[str, Any]],
    *,
    category: str,
    phase: str,
    normalized: dict[str, Any],
    probes: dict[str, Any] | None = None,
    kb_error_codes: list[dict] | None = None,
    kb_fix_hints: list[dict] | None = None,
    kb_api_signatures: list[dict] | None = None,
    kb_fix_templates: list[dict] | None = None,
    kb_api_version_map: list[dict] | None = None,
    kb_soc_feature_matrix: dict | None = None,
    kb_operator_idioms: list[dict] | None = None,
    kb_reserved_identifiers: dict | None = None,
    max_secondary: int = 4,
    max_leaves: int = 8,
    include_category_advice: bool = True,
) -> dict[str, Any]:
    """构造 cause_tree。

    KB 参数为 None 时走全局 lazy cache（生产路径）；
    显式传入 list 时使用传入值（单测/隔离场景）。
    旧分类提示/模板仅兼容研究调用；公开诊断显式禁用，改用有证据的条件 guidance。
    """
    if kb_error_codes is None:
        kb_error_codes = _load_error_codes()
    if kb_fix_hints is None:
        kb_fix_hints = _load_fix_hints()
    if kb_api_signatures is None:
        kb_api_signatures = _load_api_signatures()
    if kb_fix_templates is None:
        kb_fix_templates = _load_fix_templates()
    if kb_api_version_map is None:
        kb_api_version_map = _load_api_version_map()
    if kb_soc_feature_matrix is None:
        kb_soc_feature_matrix = _load_soc_feature_matrix()
    if kb_operator_idioms is None:
        kb_operator_idioms = _load_operator_idioms()
    if kb_reserved_identifiers is None:
        kb_reserved_identifiers = _load_reserved_identifiers()

    # primary / secondary
    primary: dict[str, Any] = {}
    secondary: list[dict[str, Any]] = []
    if candidates:
        head = candidates[0]
        primary = {
            "cause": head.get("cause", ""),
            "confidence": head.get("confidence", 0.0),
            "evidence": list(head.get("evidence") or []),
            "files": list(head.get("files") or []),
            "rank": head.get("rank", 1),
            "source": "primary_error" if head.get("rank") == 1 else "static_finding",
        }
        for c in candidates[1 : 1 + max_secondary]:
            secondary.append(
                {
                    "rank": c.get("rank"),
                    "cause": c.get("cause", ""),
                    "confidence": c.get("confidence", 0.0),
                    "evidence": list(c.get("evidence") or []),
                    "files": list(c.get("files") or []),
                }
            )
    # leaves
    text = _collect_text_corpus(normalized)
    codes = extract_error_codes(text)
    apis = extract_api_names(text)
    undeclared = extract_undeclared_names(text)

    # （重载推导失败≠名字未声明/臆造，不并入 extract_undeclared_names——近名兜底/
    # 幻觉检测的输入语义不许变），只定向喂下面的版本映射通道。
    nmf_names = extract_no_matching_function_names(text)

    leaves: list[Leaf] = []
    leaves.extend(_build_compiler_probe_leaves(probes))
    leaves.extend(_build_conditional_contract_leaves(normalized))
    leaves.extend(
        _build_error_code_leaves(codes, kb_codes=kb_error_codes, kb_hints=kb_fix_hints)
    )
    # Preserve explicit target/version alongside the symbol's own context.
    # Do not infer argument types or include reachability from a short API name.
    target_arch, _target_soc = _detect_target_arch(
        (normalized.get("target_soc") or "") + "\n" + text, kb_soc_feature_matrix
    )
    knowledge_context = dict(normalized)
    knowledge_context["_knowledge_target_arch"] = target_arch

    # 证词经 version_gate 归一成 version_compatibility 载荷（env 侧双源合成在
    # summary 层 build_stdout_summary；冲突时这里按 log 侧=问题发生地）。调用方已
    # 透传载荷时原样使用。载荷不直接进输出，只喂 _symbol_context 的检测域派生。
    knowledge_context.setdefault(
        "version_compatibility", _classify_version(normalized.get("cann_version"), None)
    )
    if isinstance(normalized.get("api_context"), dict):
        knowledge_context["api_context"] = dict(normalized["api_context"])
        knowledge_context["api_context"].setdefault("target_arch", target_arch)
    leaves.extend(
        _build_api_signature_leaves(
            apis, kb_apis=kb_api_signatures, text=text, normalized=knowledge_context
        )
    )

    # 使归并迁移知识（如旧归约族 → 新归一化接口）在该形态可达。
    version_map_feed = undeclared + [n for n in nmf_names if n not in undeclared]
    leaves.extend(
        _build_api_version_leaves(
            version_map_feed,
            kb_version_map=kb_api_version_map,
            text=text,
            normalized=knowledge_context,
        )
    )

    # 版本域线索，非改名建议）。近名通道对精确命中保持沉默（其 pin 不动），本通道
    # Preserve the "name matches but declaration is unreachable" path. Version
    # filtering can lower confidence while declaration facts remain visible.
    leaves.extend(
        _build_kb_declaration_leaves(
            undeclared,
            kb_apis=kb_api_signatures,
            text=text,
            normalized=knowledge_context,
        )
    )
    # Use the shared version policy for applicability and provenance filtering.

    # 命名空间成员查不到的（强形态）才可以。弱形态（`unknown type name` 等）上 affix 也会命中，
    # （`<算子>CustomTilingData` → 同名前缀的真实 intrinsic），真因其实是少了 #include。
    leaves.extend(
        _build_kb_near_name_leaves(
            undeclared,
            kb_apis=kb_api_signatures,
            affix_exact_only=set(extract_namespace_member_names(text)),
            text=text,
            normalized=knowledge_context,
        )
    )
    soc_token = normalized.get("target_soc")
    target_arch, _target_soc = _detect_target_arch(
        (soc_token or "") + "\n" + text, kb_soc_feature_matrix
    )
    leaves.extend(
        _build_soc_arch_gate_leaves(
            undeclared, soc_matrix=kb_soc_feature_matrix, target_arch=target_arch
        )
    )
    leaves.extend(
        _build_reserved_identifier_leaves(text, kb_reserved=kb_reserved_identifiers)
    )

    # 宽集合里的 `unknown type name` / 裸 `undeclared identifier` 真因多是缺 #include，
    ns_member = extract_namespace_member_names(text)
    leaves.extend(
        _build_hallucinated_api_leaves(
            ns_member,
            kb_apis=kb_api_signatures,
            kb_version_map=kb_api_version_map,
            soc_matrix=kb_soc_feature_matrix,
            idioms=kb_operator_idioms,
            text=text,
        )
    )
    if include_category_advice:
        leaves.extend(_build_template_leaves(category, kb_templates=kb_fix_templates))
        cat_leaf = _build_category_hint_leaf(category, phase)
        if cat_leaf:
            leaves.append(cat_leaf)

    # 去重（按 kind + title）
    seen: set[tuple[str, str]] = set()
    deduped: list[Leaf] = []
    for leaf in leaves:
        key = (leaf.kind, leaf.title)
        if key in seen:
            continue
        seen.add(key)
        deduped.append(leaf)
        if len(deduped) >= max_leaves:
            break

    return {
        "primary": primary,
        "secondary": secondary,
        "leaves": [leaf.as_dict() for leaf in deduped],
        "scanned_error_codes": codes,
        "scanned_api_names": apis,
        "scanned_undeclared_names": undeclared,
        "target_arch": target_arch,
    }
