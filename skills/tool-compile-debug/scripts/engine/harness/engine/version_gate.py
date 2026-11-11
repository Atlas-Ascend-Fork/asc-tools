# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Classify toolkit versions against the package knowledge-base domain."""

from __future__ import annotations

import re
from functools import lru_cache
from pathlib import Path
from typing import Any

# 版本域登记表（随包的唯一声明；位置为 ../configs/versions.yaml）。
_REGISTRY_PATH = Path(__file__).resolve().parents[2] / "harness/knowledge/versions.yaml"

# 登记表状态阶梯：verified_live > verified_static > registered > watch > frozen。
_LADDER = ("verified_live", "verified_static", "registered", "watch", "frozen")
# supported（已核实）= verified_live 或 verified_static——给用户的提示只指向
# 已核实版本，registered/watch 不承诺（versions.yaml 头部纪律）。
_VERIFIED_STATUSES = frozenset(_LADDER[:2])

# 登记表与引擎必须来自同一完整安装，不能用默认版本掩盖缺失或损坏。
_REGISTRY_REPAIR_HINT = (
    "；请重新安装或更新完整技能包，确保引擎与版本登记表来自同一发布版本"
)


class VersionRegistryError(RuntimeError):
    """版本域登记表缺失/格式错——fail-loud（禁静默回退到单版本硬编码）。"""


def normalize_version(raw: str | None) -> tuple[int, int] | None:
    """版本串 → (major, minor)；不可解析（占位符 / latest / 空）→ None。

    只比 major.minor：9.0.0 与 9.0.0.beta1 同属 9.0 域（登记表 granularity，
    patch 不另立条目、随域继承）。
    """
    m = re.search(r"(\d+)\.(\d+)", str(raw or ""))
    return (int(m.group(1)), int(m.group(2))) if m else None


def _registry_fail(msg: str) -> None:
    raise VersionRegistryError(
        f"版本域登记表 {msg}（{_REGISTRY_PATH}）{_REGISTRY_REPAIR_HINT}"
    )


@lru_cache(maxsize=1)
def _load_registry() -> dict[str, Any]:
    """Load and validate the version registry; malformed data fails clearly."""
    try:
        import yaml
    except ImportError as exc:  # PyYAML 缺失同样是「登记表不可读」
        _registry_fail(f"加载需要 PyYAML（{exc}）")
    if not _REGISTRY_PATH.is_file():
        _registry_fail("缺失")
    try:
        data = yaml.safe_load(_REGISTRY_PATH.read_text(encoding="utf-8")) or {}
    except (OSError, UnicodeError, yaml.YAMLError) as exc:
        _registry_fail(f"不可解析：{exc}")
    if not isinstance(data, dict):
        _registry_fail("顶层不是 mapping")
    floor = data.get("floor")
    ceiling = data.get("ceiling")
    floor_dom = normalize_version(floor)
    ceiling_dom = normalize_version(ceiling)
    if floor_dom is None or ceiling_dom is None:
        _registry_fail(f"floor/ceiling 必须是可解析版本串（got {floor!r}/{ceiling!r}）")
    if floor_dom > ceiling_dom:
        _registry_fail(f"floor {floor} 不得高于 ceiling {ceiling}")
    versions = data.get("versions")
    if not isinstance(versions, list) or not versions:
        _registry_fail("versions 必须是非空列表")
    supported: set[tuple[int, int]] = set()
    for entry in versions:
        if not isinstance(entry, dict):
            _registry_fail(f"version 条目不是 mapping（{entry!r}）")
        ver, status = entry.get("version"), entry.get("status")
        if not isinstance(ver, str) or not ver.strip():
            _registry_fail(f"version 条目缺 version 串（{entry!r}）")
        if status not in _LADDER:
            _registry_fail(f"version {ver} 的 status {status!r} 不在阶梯 {_LADDER}")
        domain = entry.get("domain")
        if (
            not isinstance(domain, (list, tuple))
            or len(domain) != 2
            or not all(isinstance(x, int) for x in domain)
        ):
            _registry_fail(f"version {ver} 的 domain 必须是 [major, minor] 整数对")
        if normalize_version(ver) != (int(domain[0]), int(domain[1])):
            _registry_fail(f"version {ver} 与 domain {list(domain)} 互不一致")
        if status in _VERIFIED_STATUSES:
            supported.add((int(domain[0]), int(domain[1])))
    if not supported:
        _registry_fail("没有任何 verified_live/verified_static 版本——supported 域为空")
    return {
        "floor": str(floor),
        "ceiling": str(ceiling),
        "floor_domain": floor_dom,
        "ceiling_domain": ceiling_dom,
        "supported_domains": sorted(supported),
    }


def floor_domain() -> tuple[int, int]:
    """支持下限的 (major, minor)——preflight 等外围的「≥floor」判据从这里派生。"""
    return _load_registry()["floor_domain"]


def ceiling_domain() -> tuple[int, int]:
    """最高已核实版本的 (major, minor)。"""
    return _load_registry()["ceiling_domain"]


# CANN 知识覆盖下限，保留 KB_CANN_BASELINE 作为公开兼容键。
KB_CANN_BASELINE = _load_registry()["floor"]

# An ahead-of-ceiling version keeps diagnosis available with advisory
# version-sensitive guidance. Set this flag to ``True`` only when policy
# explicitly requires suppressing such results.
AHEAD_SOFT_SUPPRESS = False

# log 侧版本痕迹（best-effort，按可信度排序）。首字符必须数字——排除 $ 占位符与环境变量名。
_LOG_VERSION_RES = (
    re.compile(r"(?i)\bDetected\s+CANN_VERSION\s*[:=]\s*(\d[\w.\-]*)"),
    re.compile(r"\b(?:ascend-toolkit|cann)[\-_/](\d+\.\d+[\w.\-]*)"),
    re.compile(r"(?i)\bCANN[ _-]?version[ :=]+(\d[\w.\-]*)"),
)

_UNKNOWN_NOTE = (
    "未能从 log/env 核实目标 CANN 版本——版本相关结论（API 名/签名/arch 门控/"
    "deprecated）请以目标版本 toolkit 头文件为准。"
)


def extract_log_version(text: str) -> str | None:
    """build.log 全文 → 首个带值版本串；无 → None。占位符被正则构造排除。"""
    for pat in _LOG_VERSION_RES:
        m = pat.search(text or "")
        if m:
            return m.group(1)
    return None


def _state_for(domain: tuple[int, int], view: dict[str, Any]) -> str:
    """Classify a known domain as supported, below_floor, or ahead.

    ``ahead`` covers domains at or above the floor that are not verified in the
    registry. Unknown input is handled by ``classify`` before this function.
    """
    if domain in view["supported_domains"]:
        return "supported"
    if domain < view["floor_domain"]:
        return "below_floor"
    return "ahead"


def _external_status(state: str) -> str:
    """Map internal version states to the public match/mismatch/unknown values."""
    if state == "supported":
        return "match"
    if state == "below_floor":
        return "mismatch"
    # An ahead version is advisory unless policy requests suppression.
    return "mismatch" if AHEAD_SOFT_SUPPRESS else "match"


def _conflict_outcome(
    lv: tuple[int, int],
    ev: tuple[int, int],
    log_raw: str | None,
    env_raw: str | None,
    view: dict[str, Any],
) -> dict[str, Any]:
    """Resolve conflicting versions from the build log and local environment.

    * 任一侧 < floor（跨 floor 冲突）：保守判 mismatch（resolution=below_floor）。
    * 都 ≥ floor 但域不同（域内冲突）：取 log 侧（问题发生地）判定——log 侧
      supported → ``match`` with ``resolution=conflict_in_domain``; an ahead log
      remains advisory and is reported with the conflict flag.
    """
    if lv < view["floor_domain"] or ev < view["floor_domain"]:
        return {
            "status": "mismatch",
            "resolution": "below_floor",
            "conflict_in_domain": True,
            "note": (
                f"log={log_raw} 与 env={env_raw} 的 CANN 版本不一致，且一侧低于支持"
                f"下限 {view['floor']}——保守判 mismatch；以问题发生地（log）优先核对。"
            ),
        }
    log_state = _state_for(lv, view)
    if log_state == "supported":
        return {
            "status": "match",
            "resolution": "conflict_in_domain",
            "conflict_in_domain": True,
            "note": (
                f"log={log_raw} 与 env={env_raw} 探测到不同的 CANN 版本，均在支持"
                "下限之上——按问题发生地（log 侧）出知识，双版本如实披露。"
            ),
        }
    # The log side is ahead of the verified ceiling; preserve both versions.
    return {
        "status": _external_status("ahead"),
        "resolution": "ahead",
        "conflict_in_domain": True,
        "note": (
            f"log={log_raw} 与 env={env_raw} 探测到不同的 CANN 版本；log 侧超出已"
            f"核实范围（核验至 {view['ceiling']}）——版本相关结论降级为参考，双版本如实披露。"
        ),
    }


def classify(log_raw: str | None, env_raw: str | None) -> dict[str, Any]:
    """Classify log and environment versions into a stable compatibility payload.

    Public ``status`` is one of match, mismatch, or unknown. ``resolution``
    distinguishes supported, below_floor, ahead, unknown, and an in-domain
    conflict with a supported log version. Conflicts preserve both raw versions
    and set ``conflict_in_domain``.

    Supported domains come from verified registry entries. Domains below the
    floor produce mismatch; other known domains produce ahead, whose public
    status follows ``AHEAD_SOFT_SUPPRESS``. A match therefore does not by itself
    prove that version-sensitive advice has been verified for the target.
    """
    lv, ev = normalize_version(log_raw), normalize_version(env_raw)
    view = _load_registry()
    payload: dict[str, Any] = {
        "kb_baseline": KB_CANN_BASELINE,
        "kb_floor": view["floor"],
        "ceiling": view["ceiling"],
        "supported_domains": [list(d) for d in view["supported_domains"]],
    }
    if lv is None and ev is None:
        return {
            **payload,
            "detected": None,
            "source": "none",
            "status": "unknown",
            "resolution": "unknown",
            "conflict_in_domain": False,
            "note": _UNKNOWN_NOTE,
        }
    if lv is not None and ev is not None and lv != ev:
        return {
            **payload,
            "detected": {"log": log_raw, "env": env_raw},
            "source": "log+env-conflict",
            **_conflict_outcome(lv, ev, log_raw, env_raw, view),
        }
    if lv is not None and ev is not None:
        detected, source, domain = log_raw, "log+env", lv
    elif lv is not None:
        detected, source, domain = log_raw, "log", lv
    else:
        detected, source, domain = env_raw, "env", ev
    state = _state_for(domain, view)
    out: dict[str, Any] = {
        **payload,
        "detected": detected,
        "source": source,
        "status": _external_status(state),
        "resolution": state,
        "conflict_in_domain": False,
    }
    if state == "below_floor":
        out["note"] = (
            f"本工具线自 CANN {view['floor']} 起支持，检测到 {detected}"
            "——版本相关结论仅供参考，以目标版本 toolkit 头文件为准。"
        )
    elif state == "ahead":
        out["note"] = (
            f"检测到 {detected} 超出已核实范围（核验至 {view['ceiling']}）"
            "——版本相关结论已整体降级为参考。"
        )
    return out


def is_mismatch(vc: dict[str, Any] | None) -> bool:
    """外部抑制判据（语义保持）：status == mismatch。

    A mismatch is only an explicit public ``status=mismatch``. Supported,
    unknown, and an in-domain conflict with a supported log remain usable while
    version-sensitive advice is still marked in the payload.
    """
    return bool(vc) and vc.get("status") == "mismatch"


def kb_knowledge_domain(vc: dict[str, Any] | None) -> tuple[int, int] | None:
    """Return the registry domain used for version-sensitive knowledge.

    Unknown input uses the floor as a conservative baseline. Consumers keep
    applicability evidence visible while excluding later-version entries from
    fast-path advice. Other resolutions use the detected domain, preferring the
    log version when log and environment disagree. An unparseable detected
    value returns None and leaves the applicability filter inactive.

    Name existence checks are deliberately independent from this applicability
    filter.
    """
    if not isinstance(vc, dict):
        return None
    if vc.get("resolution") == "unknown":
        return _load_registry()["floor_domain"]
    detected = vc.get("detected")
    if isinstance(detected, dict):  # 冲突载荷 {log, env}：按问题发生地
        detected = detected.get("log")
    return normalize_version(detected)


def version_field_blocked(
    fields: dict[str, Any] | None, domain: tuple[int, int] | None
) -> str | None:
    """Return whether explicit ``since``/``until`` excludes a detected domain.

    返回冲突 reason（``since_after_detected`` / ``until_before_detected``）或
    None（适用 / 过滤不激活）。区间语义：``[since, until)``——until 是**首个不适用
    域**（Reduce 族 9.1 头文件搬家 → 旧记录 until: 9.1.0，9.0.x 仍有效）。

    This function only enforces explicit bounds. Missing bounds do not exclude
    an entry; an unknown domain leaves filtering inactive.
    """
    if domain is None or not isinstance(fields, dict):
        return None
    since_dom = normalize_version(fields.get("since"))
    if since_dom is not None and since_dom > domain:
        return "since_after_detected"
    until_dom = normalize_version(fields.get("until"))
    if until_dom is not None and until_dom <= domain:
        return "until_before_detected"
    return None


def mismatch_next_actions(vc: dict[str, Any]) -> list[str]:
    """Keep diagnosis useful without changing an already established target."""
    resolution = vc.get("resolution")
    floor = vc.get("kb_floor") or KB_CANN_BASELINE
    ceiling = vc.get("ceiling") or _load_registry()["ceiling"]
    detected = vc.get("detected")
    if isinstance(detected, dict):
        shown = f"log={detected.get('log')} / env={detected.get('env')}"
    else:
        shown = str(detected or "未知版本")
    if resolution == "below_floor":
        headline = (
            f"【版本低于支持下限】本工具自 CANN {floor} 起支持，检测到 {shown}"
            "——保留通用诊断，版本相关结论仅供参考，以目标版本 toolkit 头文件核实。"
        )
        options = (
            "已有明确目标时保持当前版本，按其真实声明和原构建配方继续排查；"
            f"迁移到支持范围（自 {floor} 起）是可选方案，不是继续排查的前提。"
        )
    elif resolution == "ahead":
        # Versions outside the verified domains retain advisory guidance.
        headline = (
            f"【版本超出已核实范围】检测到 {shown} 超出已核实范围（核验至 {ceiling}）"
            "——版本相关结论降级为参考，需以实际声明和构建证据核实。"
        )
        options = (
            "已有明确目标时继续使用当前版本，逐项核对版本敏感建议；"
            f"已核实版本域（最高至 {ceiling}）的环境可用于对照，"
            "对照成功不能替代用户目标环境中的验证。"
        )
    else:
        headline = (
            "【版本未知】未能从日志或环境核实 CANN 版本——保留通用诊断，"
            "版本敏感结论需要实际声明或工具链证据。"
        )
        options = (
            "已有明确目标时按原构建配方继续收集证据；补充版本信息后再筛选版本敏感建议。"
        )
    return [
        headline,
        (
            "快速路径和修复建议受版本域及条目适用边界约束；"
            "需按实际头文件、目标和调用契约复核。"
        ),
        options,
        (
            "可运行 preflight 补充本机 toolkit 证据，并与失败日志核对。"
            "只有现有目标、材料和授权不能决定版本、接口或功能取舍时才请求确认；"
            "已明确的目标无需重复确认，不擅自升级或更换目标。"
        ),
    ]
