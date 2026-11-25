# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Product support policy and observed version conflicts, not a domain knowledge map."""

from __future__ import annotations
import re
from functools import lru_cache
from pathlib import Path
from typing import Any
import yaml

_REGISTRY_PATH = Path(__file__).resolve().parents[2] / "harness/config/versions.yaml"


class VersionRegistryError(ValueError):
    """Product policy is missing or invalid."""


@lru_cache(maxsize=1)
def _load_registry():
    try:
        data = yaml.safe_load(_REGISTRY_PATH.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or data.get("schema_version") != 2:
            raise ValueError("invalid policy schema")
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", str(data.get("floor", ""))):
            raise ValueError("invalid support floor")
        return data
    except (OSError, ValueError, yaml.YAMLError) as exc:
        raise VersionRegistryError(f"Cannot read support policy: {exc}") from exc


def floor_domain():
    return normalize_version(_load_registry()["floor"])


# Compatibility alias for older report readers; this is a product floor, not
# a default knowledge version and must never be substituted for an unknown SDK.
KB_CANN_BASELINE = _load_registry()["floor"]

_LOG_VERSION_RES = (
    re.compile(r"(?i)\bDetected\s+CANN_VERSION\s*[:=]\s*(\d[\w.\-]*)"),
    re.compile(r"\b(?:ascend-toolkit|cann)[\-_/](\d+\.\d+[\w.\-]*)"),
    re.compile(r"(?i)\bCANN[ _-]?version[ :=]+(\d[\w.\-]*)"),
)


def normalize_version(raw: str | None) -> tuple[int, int] | None:
    """版本串 → (major, minor)；不可解析（占位符 / latest / 空）→ None。

    只比 major.minor：9.0.0 与 9.0.0.beta1 同属 9.0 域（登记表 granularity，
    patch 不另立条目、随域继承）。
    """
    m = re.search(r"(\d+)\.(\d+)", str(raw or ""))
    return (int(m.group(1)), int(m.group(2))) if m else None


def extract_log_version(text: str) -> str | None:
    """build.log 全文 → 首个带值版本串；无 → None。占位符被正则构造排除。"""
    for pat in _LOG_VERSION_RES:
        m = pat.search(text or "")
        if m:
            return m.group(1)
    return None


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


def classify(log_raw, env_raw):
    log_domain, env_domain = normalize_version(log_raw), normalize_version(env_raw)
    domain = log_domain if log_domain is not None else env_domain
    conflict = (
        log_domain is not None and env_domain is not None and log_domain != env_domain
    )
    resolution = (
        "unknown"
        if domain is None
        else "below_floor"
        if domain < floor_domain()
        else "version_conflict"
        if conflict
        else "supported"
    )
    return {
        "support_floor": _load_registry()["floor"],
        "observed_versions": {"log": log_raw, "environment": env_raw},
        "kb_floor": _load_registry()["floor"],
        "detected": {"log": log_raw, "env": env_raw}
        if conflict
        else log_raw
        if log_domain is not None
        else env_raw,
        "source": "log+env-conflict"
        if conflict
        else "log+env"
        if log_domain is not None and env_domain is not None
        else "log"
        if log_domain is not None
        else "env"
        if env_domain is not None
        else "none",
        "status": "unknown"
        if domain is None
        else "mismatch"
        if conflict or resolution == "below_floor"
        else "match",
        "resolution": resolution,
        "conflict_in_domain": conflict,
        "knowledge_applicability": "requires_card_and_current_sdk_evidence",
        "note": "支持范围不等于API适用性；保留完整版本/build及目标，以知识正文和当前SDK声明核验。",
    }


def is_mismatch(vc):
    return bool(vc) and vc.get("status") == "mismatch"


def kb_knowledge_domain(vc):
    """Compatibility helper: unknown SDK stays unknown, never defaults to 9.0."""
    if not isinstance(vc, dict):
        return None
    value = vc.get("detected")
    return normalize_version(value.get("log") if isinstance(value, dict) else value)


def mismatch_next_actions(vc):
    return [
        "核对失败命令实际使用的SDK、编译器及环境变量，并保留日志与当前环境的各自版本。",
        "保持用户指定版本和目标，按当前声明继续取证；低于产品支持下限不构成自动升级授权。",
        "需要版本或目标知识时由宿主加载 knowledge-query 或 ascendc-api-knowledge-query，读取适用正文后回到原配方验证。",
    ]
