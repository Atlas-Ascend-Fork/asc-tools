#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""verdict.py — 算子调优判定引擎（规则-指标绑定契约 + 分层路由 + 反证 + 置信度）

设计对齐（见 docs/算子调优AGENT设计文档.md §5 核心设计决策）：
  - 对标 Nsight Compute Guided Analysis：规则声明绑定指标，未采集则规则不触发
    （机制上杜绝「指标没采却报规则」）；REQUIRED 缺失 → rule: SKIPPED，OPTIONAL 缺失 → 降级不阻断（optional 仅用于反证；condition 内引用必须 required——加载校验 V4 强制）。
  - 判定进脚本、解释进 LLM：本引擎输出结构化 verdict.json（机器可读契约），
    Agent/LLM 只消费判定书做解释与编排，不做判定。
  - 分层瀑布：L3 → L2 → L1 → L0 依序评估；L3 路由规则可禁用/改道下层规则（如 MTE2 污染）。
  - 每条命中规则带反证（counter_evidence）与置信度；无证据不下结论。

用法：
  # 单核判定（PipeUtilization.csv）
  python3 verdict.py --rules ../rules/L1-single-core.yaml --csv PipeUtilization.csv \
      --scalar mte2_active_bw_pct_of_peak=82 --soc a5 -o verdict.json

  # 多 kernel CSV（msprof op_summary 全量导出）：必须 --kernel-name 过滤（G15）
  python3 verdict.py --rules ../rules/L1-single-core.yaml --csv op_summary.csv \
      --kernel-name my_custom_kernel --soc a5 -o verdict.json

  # 核间判定（CSV + OpBasicInfo 衍生量）
  python3 verdict.py --rules ../rules/L2-inter-core.yaml --csv PipeUtilization.csv \
      --scalar physical_cores=48,block_dim=24,task_duration_us=210 -o verdict.json

  # MC² 判定（隔离测试标量输入）
  python3 verdict.py --rules ../rules/L3-inter-card.yaml \
      --scalar t_comm_us=420,t_compute_us=880,mte2_ratio_full=62,mte2_ratio_pure=41 -o verdict.json

输出（verdict.json 契约，report/agent 消费同一数据源）：
  {
    "schema": "opstune.verdict/1.0",
    "generated_at": "...", "rules_file": "...", "soc": "a5",
    "facts": {...指标事实（含来源 CSV/行数）...},
    "findings": [ {rule_id, name, severity, confidence, evidence, counter_evidence[],
                   speedup_range, mechanism_refs, ...} ],
    "skipped": [ {rule_id, reason, missing} ],
    "verdict": "NEEDS_OPTIMIZATION | NEAR_LIMIT | NO_BOUND_FOUND | INSUFFICIENT_DATA",
    "headline": "...", "recommended_next": [...]
  }

阈值纪律：所有阈值带 threshold_source 溯源（实践沉淀 / 🟡 待标定）。
变更规则或阈值必须先过 evals/run_evals.py（fixture CSV → 断言判定输出）全绿。
"""

from __future__ import annotations

import argparse
import csv
import datetime
import json
import sys
from pathlib import Path

SCHEMA_VERDICT = (
    "opstune.verdict/1.1"  # 1.1：拆分 NO_BOUND_FOUND / NEAR_LIMIT（检视 F7）
)


# --------------------------------------------------------------------------
# CSV 加载与指标事实构建
# --------------------------------------------------------------------------


def load_csv_rows(csv_path: str) -> list[dict]:
    """读 CSV（兼容 utf-8-sig BOM 与脏数据，容错处理 msprof 导出的真实表头）。"""
    rows = []
    with open(csv_path, encoding="utf-8-sig", errors="replace") as f:
        for row in csv.DictReader(f):
            rows.append(row)
    return rows


def parse_float(value) -> float:
    try:
        return float(str(value).strip().replace("%", ""))
    except (ValueError, AttributeError, TypeError):
        return None  # 缺失值：None 而非 0.0 —— 0.0 会污染 mean 与条件判定


def ratio_to_pct(v: float) -> float:
    """兼容 0~1 比例与 0~100 百分数两种版本字段。"""
    if v is None:
        return None
    return v * 100 if 0 <= v <= 1.5 else v


def first_present(row: dict, columns: list) -> float:
    for col in columns:
        if col in row and str(row.get(col, "")).strip() not in ("", "N/A", "NA", "-"):
            v = parse_float(row[col])
            if v is not None:
                return v
    return None


def pipe_metrics_from_rows(rows: list[dict], spec: dict) -> dict:
    """从 PipeUtilization 行构建 pipe 指标：per-row 取候选列最大值（aic/aiv 双列兼容），
    跨行 mean / max 双口径 —— 判定用 mean，max 仅识别长尾（单核峰值不代表全 kernel）。"""
    out = {}
    pipe_cols = []
    for name, m in spec.items():
        if m.get("source") != "pipe":
            continue
        pipe_cols.append((name, m))
        per_row = [first_present(r, m["columns"]) for r in rows]
        per_row = [
            ratio_to_pct(v)
            if m.get("normalize") == "ratio_to_pct" and v is not None
            else v
            for v in per_row
        ]
        valid = [v for v in per_row if v is not None]
        out[f"{name}"] = {
            "mean": (sum(valid) / len(valid)) if valid else None,
            "max": max(valid) if valid else None,
            "rows_present": len(valid),
            "rows_total": len(rows),
        }
    # 衍生量：主导 pipe / pipe 求和（串行指纹）——仅聚合 mean 语义
    means = {
        n: out[n]["mean"]
        for n, _ in pipe_cols
        if out.get(n, {}).get("mean") is not None
    }
    if means:
        out["__max_pipe_mean"] = {
            "mean": max(means.values()),
            "source": "derived:max_of_pipe_means",
        }
        out["__max_pipe_name"] = {
            "mean_name": max(means, key=means.get),
            "source": "derived",
        }
        out["__pipe_sum_mean"] = {
            "mean": sum(means.values()),
            "source": "derived:sum_of_pipe_means",
        }
    return out


def time_metrics_from_rows(rows: list[dict]) -> dict:
    """逐核耗时（核间层）：aiv_time/aic_time 双列兼容，取大者为该核主体时间。"""
    times = []
    for r in rows:
        aiv = parse_float(r.get("aiv_time(us)"))
        aic = parse_float(r.get("aic_time(us)"))
        cand = [v for v in (aiv, aic) if v is not None]
        if cand:
            times.append(max(cand))
    if not times:
        return {}
    mean = sum(times) / len(times)
    return {
        "row_count": {"value": float(len(times))},
        "time_mean_us": {"value": mean},
        "time_max_us": {"value": max(times)},
        "time_min_us": {"value": min(times)},
        "dominant_time_us": {
            "value": mean
        },  # 逐核均值（与 yaml 描述一致；L2_HEAD_OVERHEAD 消费）
        "__tail_ratio": {"value": (max(times) / mean) if mean > 0 else None},
        "__imbalance_pct": {
            "value": ((max(times) - min(times)) / max(times) * 100)
            if max(times) > 0
            else None
        },
        "__two_cluster_gap_pct": {"value": _two_cluster_gap(times)},
    }


def _two_cluster_gap(times: list[float]):
    """两簇检测：前后半段核均值差距 %（NUMA/L2 slice 负载偏斜提示）。"""
    if len(times) < 4:
        return None
    half = len(times) // 2
    m1, m2 = sum(times[:half]) / half, sum(times[half:]) / (len(times) - half)
    if m2 <= 0:
        return None
    return abs(m1 - m2) / m2 * 100


def split_rows_by_kernel(rows: list[dict]) -> dict[str, list[dict]]:
    """按 Op Name 列分组（多 kernel 混采防护，REVIEW2 G15）。

    - 无 Op Name 列（逐核明细 CSV 形态，本仓 fixtures / PipeUtilization 常态）
      → 单组 "<all>"，保持向后兼容；
    - 有 Op Name 列（msprof op_summary 全量导出形态）→ 按 kernel 名分组，
      调用方必须指定 --kernel-name，否则全行 mean/max 是跨 kernel 平均——
      数学上无意义且静默产出错误判定。
    """
    if not rows or "Op Name" not in rows[0]:
        return {"<all>": rows}
    groups: dict[str, list[dict]] = {}
    for r in rows:
        groups.setdefault(str(r.get("Op Name") or "").strip() or "<empty>", []).append(
            r
        )
    return groups


# --------------------------------------------------------------------------
# 条件求值
# --------------------------------------------------------------------------


def get_metric_value(facts: dict, metrics_spec: dict, name: str, agg: str):
    """取指标值。pipe 类返回 {mean,max} 聚合桶；scalar/derived 返回 {value 或 mean}。"""
    if name not in facts:
        return None
    entry = facts[name]
    if agg == "value":
        if "value" in entry:
            return entry["value"]
        return entry.get("mean")
    if agg == "mean":
        return entry.get("mean", entry.get("value"))
    if agg == "max":
        return entry.get("max", entry.get("value"))
    if agg == "rank":
        # 该 pipe 在 group=pipe 集合中是否占比最大 → 1 最大
        max_name = facts.get("__max_pipe_name", {}).get("mean_name")
        return 1.0 if name == max_name else (0.0 if max_name else None)
    return entry.get("mean")


def eval_leaf(
    cond: dict, facts: dict, metrics_spec: dict, scalars: dict
) -> bool | None:
    """叶子条件：{metric, agg?, op, value}。返回 True/False/None（证据不足 → None，不可判）。"""
    name = cond["metric"]
    agg = cond.get("agg", "mean")
    if name in metrics_spec and metrics_spec[name].get("source") == "scalar":
        value = scalars.get(name)
    else:
        value = get_metric_value(facts, metrics_spec, name, agg)
    # 复合值：{metric: X, factor: k} → 另一指标 × 系数（如 time_max > time_mean×1.25）
    target = cond["value"]
    if isinstance(target, dict):
        t_name, t_factor = target["metric"], target.get("factor", 1.0)
        if t_name in metrics_spec and metrics_spec[t_name].get("source") == "scalar":
            t_val = scalars.get(t_name)
        else:
            t_val = get_metric_value(facts, metrics_spec, t_name, agg)
        if t_val is None or value is None:
            return None
        target = t_val * t_factor
    if value is None or target is None:
        return None
    op = cond["op"]
    if op == "gt":
        return value > target
    if op == "gte":
        return value >= target
    if op == "lt":
        return value < target
    if op == "lte":
        return value <= target
    if op == "eq":
        return value == target
    if op == "between":
        lo, hi = target if isinstance(target, (list, tuple)) else (target, target)
        return lo <= value <= hi
    if op == "gt_ratio":
        # value 与 target 本身已是比值语义（如 __tail_ratio 直接与 1.25 比较）
        return value > target
    if op == "lt_ratio":
        return value < target
    raise ValueError(f"unknown op: {op}")


def eval_cond(cond, facts: dict, metrics_spec: dict, scalars: dict):
    """递归求值 all/any/叶子。三值逻辑：None=证据不足。
    all: 任一 False → False；有 None 无 False → None；否则 True。
    any: 任一 True → True；有 None 无 True → None；否则 False。"""
    if isinstance(cond, list):
        cond = {"all": cond}
    if "all" in cond:
        saw_none = False
        for sub in cond["all"]:
            r = eval_cond(sub, facts, metrics_spec, scalars)
            if r is False:
                return False
            if r is None:
                saw_none = True
        return None if saw_none else True
    if "any" in cond:
        saw_none = False
        for sub in cond["any"]:
            r = eval_cond(sub, facts, metrics_spec, scalars)
            if r is True:
                return True
            if r is None:
                saw_none = True
        return None if saw_none else False
    return eval_leaf(cond, facts, metrics_spec, scalars)


# --------------------------------------------------------------------------
# 规则求值
# --------------------------------------------------------------------------


def required_metrics_present(rule: dict, facts: dict, scalars: dict) -> list:
    """返回缺失的 REQUIRED 指标（绑定契约：缺失 → SKIPPED，不触发不误报）。"""
    missing = []
    spec = rule.get("_metrics_spec", {})
    for m in rule.get("metrics_required", []):
        src = spec.get(m, {}).get("source", "pipe")
        if src == "scalar":
            if scalars.get(m) is None:
                missing.append(m)
        elif m not in facts or facts[m].get("mean", facts[m].get("value")) is None:
            missing.append(m)
    return missing


def severity_of(rule: dict, facts: dict, metrics_spec: dict, scalars: dict):
    sev = rule.get("severity", {})
    if "default" in sev and "metric" not in sev:
        return sev["default"], ""
    if "metric" in sev:
        v = get_metric_value(facts, metrics_spec, sev["metric"], "mean")
        if v is None:
            v = scalars.get(sev["metric"])
        if v is not None:
            for band in sev.get("bands", []):
                lo, hi = band.get("min"), band.get("max")
                if lo is not None and v < lo:
                    continue
                if hi is not None and v > hi:
                    continue
                return band.get("level", "light"), band.get("note", "")
        return sev.get("default", "light"), ""
    return "light", ""


def eval_rule(
    rule: dict, facts: dict, metrics_spec: dict, scalars: dict, disabled: set
):
    rid = rule["id"]
    if rid in disabled:
        return {
            "rule_id": rid,
            "status": "SKIPPED",
            "reason": f"disabled by routing rule ({rid} 被上层路由禁用)",
        }

    missing = required_metrics_present(rule, facts, scalars)
    if missing:
        return {
            "rule_id": rid,
            "status": "SKIPPED",
            "reason": f"REQUIRED 指标缺失: {', '.join(missing)}（绑定契约：未采集不判定）",
            "missing": missing,
        }

    result = eval_cond(rule["condition"], facts, metrics_spec, scalars)
    if result is None:
        optional_missing = [
            m
            for m in rule.get("metrics_optional", [])
            if m not in facts and scalars.get(m) is None
        ]
        return {
            "rule_id": rid,
            "status": "INSUFFICIENT_DATA",
            "reason": "条件求值证据不足（存在 None 分支）",
            "missing_optional": optional_missing,
        }
    if not result:
        finding = {"rule_id": rid, "status": "NOT_HIT"}
        w = rule.get("watch")
        if w:
            wv = eval_cond(w["condition"], facts, metrics_spec, scalars)
            if wv is True:
                finding["watch"] = w["note"]
        return finding

    # ---- 命中：组装 finding（判定书条目契约）----
    sev, sev_note = severity_of(rule, facts, metrics_spec, scalars)
    finding = {
        "rule_id": rid,
        "status": "HIT",
        "name": rule.get("name", rid),
        "layer": rule.get("_layer", ""),
        "severity": sev,
        "confidence": rule.get("confidence", "medium"),
        "threshold_source": rule.get("threshold_source", ""),
        "evidence": _evidence_text(rule, facts, metrics_spec, scalars),
        "counter_evidence": _counter_evidence(rule, facts, metrics_spec, scalars),
        "speedup_range": rule.get("speedup_range"),
        "speedup_source": rule.get("speedup_source", ""),
        "mechanism_refs": rule.get("mechanism_refs", []),
    }
    if sev_note:
        finding["severity_note"] = sev_note
    # 路由副作用：命中本规则可禁用其它规则 / 改道机制路由
    routing = rule.get("routing")
    if routing:
        finding["routing"] = routing
    return finding


def _evidence_text(rule, facts, metrics_spec, scalars) -> str:
    parts = []
    for m in rule.get("metrics_required", []):
        v = get_metric_value(facts, metrics_spec, m, "mean")
        if v is None:
            v = scalars.get(m)
        if v is not None:
            parts.append(f"{m}={v:.1f}")
    return "；".join(parts) if parts else "(derived condition)"


def _counter_evidence(rule, facts, metrics_spec, scalars) -> list:
    """反证条目：条件为真时以 annotate 记录（供 LLM 解释与人工复核），
    不推翻判定 —— 判定只出自规则条件，反证是质量的第二道闸。"""
    out = []
    for ce in rule.get("counter_evidence", []):
        r = eval_cond(
            {
                "metric": ce["condition"]["metric"],
                "agg": ce["condition"].get("agg", "value"),
                "op": ce["condition"]["op"],
                "value": ce["condition"]["value"],
            },
            facts,
            metrics_spec,
            scalars,
        )
        if r is True:
            out.append({"id": ce["id"], "note": ce["note"]})
        elif r is None:
            out.append(
                {
                    "id": ce["id"],
                    "note": ce["note"] + "（反证指标缺失，未评估）",
                    "unevaluated": True,
                }
            )
    return out


# --------------------------------------------------------------------------
# 规则包加载校验（schema 纪律机制化：违规启动即报错，检视 §7a/F5/F16）
# --------------------------------------------------------------------------

# 引擎内置 derived 指标白名单（不声明在词汇表也可被规则引用）
DERIVED_WHITELIST = {
    "__max_pipe_mean",
    "__max_pipe_name",
    "__pipe_sum_mean",
    "__tail_ratio",
    "__imbalance_pct",
    "__two_cluster_gap_pct",
    "r_value",
    "__mte2_pollution_gap",
    "__mte2_pure_ratio_pct",
    "row_count",
    "time_mean_us",
    "time_max_us",
    "time_min_us",
    "dominant_time_us",
    "__occupancy_pct",
    "__head_overhead_pct",
    "__host_dispatch_pct",
}


def _leaves_of(cond) -> list:
    """递归展开条件树的全部叶子（含 watch / counter_evidence 由调用方分别传入）。"""
    out = []
    if cond is None:
        return out
    if isinstance(cond, list):
        for sub in cond:
            out.extend(_leaves_of(sub))
        return out
    if isinstance(cond, dict):
        if "metric" in cond and "op" in cond:
            out.append(cond)
        for key in ("all", "any"):
            if key in cond:
                out.extend(_leaves_of(cond[key]))
    return out


def _leaf_metrics(cond) -> set:
    """条件树引用的全部指标名（含复合 value 引用的 metric）。"""
    names = set()
    for leaf in _leaves_of(cond):
        names.add(leaf["metric"])
        if isinstance(leaf.get("value"), dict) and "metric" in leaf["value"]:
            names.add(leaf["value"]["metric"])
    return names


def validate_rules_pack(
    pack: dict, rules_path: str, all_ids: set | None = None
) -> list[str]:
    """加载期校验，返回错误清单（非空 → 调用方报错退出）。

    校验项（rule-schema.md 已承诺而未实现的三条 + optional 语义强制）：
      V1 引用完整性：condition / watch / counter_evidence / severity 引用的指标
         必须在词汇表（metrics）或 derived 白名单内——未声明引用 = 配置错误；
      V2 路由目标存在：routing.on_hit 的 disable_rule 目标必须存在于已知规则集
         （all_ids 提供时按跨包全集校验；否则按本包 id 集合校验）；
      V3 同指标区间无重叠：同一指标的多条规则构成档位类判定（如 L3 R 值五档）
         时，区间不得重叠（消除「同一值双命中」类矛盾，F3 的机制化护栏）；
      V4 optional 语义：metrics_optional 中的指标禁止出现在 condition 中
         （引擎对 condition 内 None 一律 INSUFFICIENT_DATA，不存在「可选指标
         缺失时跳过叶子」的降级求值——optional 仅用于反证，rule-schema.md 明确）。
    """
    errs = []
    metrics = pack.get("metrics", {})
    rules = pack.get("rules", [])
    known = set(metrics) | DERIVED_WHITELIST
    ids = all_ids if all_ids is not None else {r.get("id") for r in rules}

    for rule in rules:
        rid = rule.get("id", "<no-id>")
        # V1 引用完整性
        refs = _leaf_metrics(rule.get("condition"))
        refs |= _leaf_metrics(
            rule.get("watch", {}).get("condition") if rule.get("watch") else None
        )
        for ce in rule.get("counter_evidence", []):
            refs |= _leaf_metrics(ce.get("condition"))
        if isinstance(rule.get("severity"), dict) and "metric" in rule["severity"]:
            refs.add(rule["severity"]["metric"])
        undeclared = refs - known
        if undeclared:
            errs.append(
                f"[V1] {rid}: 引用了未声明指标 {sorted(undeclared)}"
                f"（词汇表 + derived 白名单均无；rule-schema.md：未声明引用=配置错误）"
            )
        # V4 optional 不进 condition
        opt = set(rule.get("metrics_optional", []))
        cond_refs = _leaf_metrics(rule.get("condition"))
        bad_opt = opt & cond_refs
        if bad_opt:
            errs.append(
                f"[V4] {rid}: optional 指标 {sorted(bad_opt)} 出现在 condition 中"
                f"（optional 仅用于反证；condition 引用必须 required，缺失即 INSUFFICIENT_DATA）"
            )
        # V1b REQUIRED 引用同样校验（防拼写错误）
        req_undeclared = set(rule.get("metrics_required", [])) - known
        if req_undeclared:
            errs.append(
                f"[V1] {rid}: metrics_required 含未声明指标 {sorted(req_undeclared)}"
            )
        # V2 路由目标存在。on_hit 支持两种形态（rule-schema.md）：
        #   位置式列表 [disable_rule, <rule_id>]（首元素为动作名，第二元素为目标）
        #   映射式 dict {disable_rule: <rule_id>} 或字符串列表（每元素一个目标）
        routing = rule.get("routing") or {}
        on_hit = routing.get("on_hit", [])
        targets = []
        if isinstance(on_hit, list) and on_hit and on_hit[0] == "disable_rule":
            targets = on_hit[1:]  # 位置式：跳过动作名
        elif isinstance(on_hit, list):
            for act in on_hit:
                if isinstance(act, dict) and "disable_rule" in act:
                    targets.append(act["disable_rule"])
                elif isinstance(act, str):
                    targets.append(act)
        for target in targets:
            if target not in ids:
                errs.append(
                    f"[V2] {rid}: routing.on_hit 禁用了不存在的规则 {target}（悬空路由）"
                )

    # V3 同指标区间无重叠（档位类判定）
    per_metric: dict[str, list[tuple]] = {}
    for rule in rules:
        cond = rule.get("condition")
        leaves = _leaves_of(cond)
        if not leaves:
            continue
        # 仅当规则的全部叶子都引用同一指标且为区间形状（带数值边界的
        # gt/gte/lt/lte/between）才视为该指标的档位类规则
        metrics_in_cond = {leaf["metric"] for leaf in leaves}
        if len(metrics_in_cond) != 1:
            continue
        m = leaves[0]["metric"]
        lo = hi = None
        shape_ok = True
        for leaf in leaves:
            op, val = leaf["op"], leaf["value"]
            if op == "between" and isinstance(val, list) and len(val) == 2:
                lo, hi = float(val[0]), float(val[1])
            elif isinstance(val, (int, float)):
                if op in ("gt", "gte") and lo is None:
                    lo = float(val)
                elif op in ("lt", "lte") and hi is None:
                    hi = float(val)
                else:
                    shape_ok = False
                    break
            else:
                shape_ok = False
                break
        if shape_ok and (lo is not None or hi is not None):
            per_metric.setdefault(m, []).append((lo, hi, rule.get("id")))
    for m, bands in per_metric.items():
        bands.sort(key=lambda b: b[0] if b[0] is not None else float("-inf"))
        for i in range(len(bands) - 1):
            lo1, hi1, id1 = bands[i]
            lo2, hi2, id2 = bands[i + 1]
            if hi1 is not None and lo2 is not None and hi1 > lo2:
                errs.append(
                    f"[V3] 指标 {m} 档位区间重叠：{id1}({lo1},{hi1}) 与 "
                    f"{id2}({lo2},{hi2})（同一值可双命中，机制方向可能矛盾）"
                )
    return errs


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------


def load_rules(path: str) -> dict:
    import yaml

    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f)


def _rule_default_severity(rule: dict) -> str:
    """取规则 default severity（severity 为 {default: X} 或分档 dict）。"""
    sev = rule.get("severity", {})
    if isinstance(sev, dict):
        return sev.get("default", "light")
    return sev or "light"


def near_limit_threshold(all_hits_seeds: list[dict]) -> float | None:
    """「接近上限」触发阈值（数据驱动，检视 F7）。

    threshold = min(已加载规则中 default severity=critical 且 confidence=high 的
    speedup_range 上界)——不硬编码 1.15，规则集演进自动联动（当前规则集下
    L3_MC2_STRONG_COMPUTE [1.0,1.2] 成为 argmin）。无可参照规则时返回 None
    （该分支退化为不可达，与「无此类规则」的规则集状态一致）。
    """
    bounds = [
        r["speedup_range"][1]
        for r in all_hits_seeds
        if _rule_default_severity(r) == "critical"
        and r.get("confidence") == "high"
        and r.get("speedup_range")
    ]
    return min(bounds) if bounds else None


def headline_and_verdict(
    findings: list, skipped: list, rule_seeds: list | None = None
) -> tuple[str, str, list]:
    hits = [f for f in findings if f.get("status") == "HIT"]
    # NOT_HIT = 数据在、条件不成立（证据完整地排除了该规则）
    evaluated = [s for s in skipped if s.get("status") == "NOT_HIT"]
    if not hits:
        if not evaluated:
            # 没有任何规则被完整评估（全部因指标缺失跳过）→ 证据不足，不判 NEAR_LIMIT
            return (
                "证据不足，无法判定：全部规则因 REQUIRED 指标缺失被跳过（绑定契约：未采集不判定）",
                "INSUFFICIENT_DATA",
                ["按 skipped[].missing 提示补采指标后重跑 verdict.py"],
            )
        # 「未命中任何规则」≠「接近上限」：前者是「该换更深的指标」，后者是「该收手」
        # ——拆分为独立 verdict 值 NO_BOUND_FOUND（schema 1.1，检视 F7）
        return (
            "未命中任何瓶颈规则（各 pipe 占比均衡且低于判定线）",
            "NO_BOUND_FOUND",
            ["评估是否已达性能目标；如仍需提升，采集 L0 源码级指标做深度分析"],
        )
    order = {"critical": 0, "moderate": 1, "light": 2}
    conf = {"high": 0, "medium": 1, "low": 2}
    hits.sort(
        key=lambda f: (
            order.get(f.get("severity"), 3),
            conf.get(f.get("confidence"), 3),
        )
    )
    top = hits[0]
    headline = f"{top['name']}（{top['severity']}）" + (
        f"；另有 {len(hits) - 1} 项命中" if len(hits) > 1 else ""
    )
    # 接近上限判定（数据驱动阈值）：唯一 critical+high 命中且其 speedup 上界 ≤ 阈值
    # → NEAR_LIMIT 提示（阈值 = 全部 critical+high 规则上界的 min，见 near_limit_threshold）
    threshold = near_limit_threshold(rule_seeds or findings)
    if (
        len(hits) == 1
        and top["severity"] == "critical"
        and top["confidence"] == "high"
        and top.get("speedup_range")
        and threshold is not None
        and top["speedup_range"][1] <= threshold
    ):
        return (
            headline,
            "NEAR_LIMIT",
            [
                f"唯一瓶颈已接近理论上限（预估收益上界 {top['speedup_range'][1]}x，"
                f"阈值 {threshold}x 由规则集 critical+high 上界派生），确认标尺后再投入"
            ],
        )
    return (
        headline,
        "NEEDS_OPTIMIZATION",
        [f"按 speedup_range 排序实施机制：{h['rule_id']}" for h in hits[:3]],
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="算子调优判定引擎（分层规则 + 绑定契约 + 反证）"
    )
    ap.add_argument(
        "--rules",
        required=True,
        help="规则包 YAML（可逗号分隔多个，按层排序：L3,L2,L1,L0）",
    )
    ap.add_argument(
        "--csv", default=None, help="PipeUtilization.csv（pipe/时间类指标源）"
    )
    ap.add_argument(
        "--kernel-name",
        default=None,
        help="目标 kernel 名（Op Name 列存在且多 kernel 时必填——G15 混采防护；"
        "缺省且检测到多 kernel 时报错退出，不静默跨 kernel 平均）",
    )
    ap.add_argument(
        "--scalar",
        default="",
        help="标量指标 key=value,key=value（bw 百分比/核数/隔离测试值等）",
    )
    ap.add_argument(
        "--soc", default=None, help="SoC 标识（a2/a3/a5），用于规则包 soc 域过滤"
    )
    ap.add_argument(
        "--min-confidence",
        default="low",
        choices=["low", "medium", "high"],
        help="过滤输出置信度下限（不改变触发，只影响报告呈现）",
    )
    ap.add_argument("-o", "--output", required=True, help="verdict.json 输出路径")
    args = ap.parse_args()

    scalars = {}
    if args.scalar:
        for kv in args.scalar.split(","):
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    scalars[k.strip()] = float(v)
                except ValueError:
                    # 非数值标量（脏输入如 N/A）：按缺失处理（None 语义 → 相关规则
                    # INSUFFICIENT/SKIPPED 降级），不进 facts 参与比较（检视 F18：
                    # 字符串进比较会 TypeError 崩溃）
                    print(
                        f"WARN: 标量 {k.strip()}={v!r} 非数值，按缺失处理（不参与判定）",
                        file=sys.stderr,
                    )
                    scalars[k.strip()] = None

    rows = load_csv_rows(args.csv) if args.csv else []
    # ---- G15 kernel 归属校验：多 kernel CSV 混采防护（fail loud，不静默跨 kernel 平均）----
    kernel_groups = split_rows_by_kernel(rows)
    if len(kernel_groups) > 1:
        available = ", ".join(sorted(kernel_groups))
        if not args.kernel_name:
            print(
                f"ERROR: CSV 含 {len(kernel_groups)} 个 kernel（Op Name 多值：{available}）。"
                "全行 mean/max 是跨 kernel 平均，判定无意义——请用 --kernel-name 指定目标 kernel"
                "（多 kernel 程序逐 kernel 判定），或改喂单 kernel 导出的 CSV。",
                file=sys.stderr,
            )
            return 4
        if args.kernel_name not in kernel_groups:
            print(
                f"ERROR: --kernel-name {args.kernel_name!r} 不在 CSV Op Name 集合中"
                f"（实际：{available}）。检查 kernel 名拼写或去掉过滤重采。",
                file=sys.stderr,
            )
            return 4
        rows = kernel_groups[args.kernel_name]
        print(
            f"[kernel] 按 Op Name={args.kernel_name} 过滤：{len(rows)} 行"
            f"（共 {sum(len(v) for v in kernel_groups.values())} 行）",
            file=sys.stderr,
        )
    all_findings, all_skipped = [], []
    disabled: set = set()
    facts_by_pack: dict = {}  # pack_stem → facts（审计分层快照，F4）
    rules_files = [p.strip() for p in args.rules.split(",") if p.strip()]
    soc = args.soc

    # ---- 加载校验（schema 纪律机制化：V1 引用完整性 / V2 悬空路由 / V3 区间重叠 / V4 optional）----
    packs = []  # (path, pack) 供下方求值循环复用
    all_rule_ids: set = set()
    all_rule_seeds: list = []  # 规则种子（severity/confidence/speedup_range 供阈值派生）
    for rf in rules_files:
        pack = load_rules(rf)
        packs.append((rf, pack))
        all_rule_ids |= {r.get("id") for r in pack.get("rules", [])}
        all_rule_seeds.extend(pack.get("rules", []))
    # routing.on_hit 的 disable_rule 语义是跨包的（rule-schema.md）：目标可能在未加载的
    # 规则包中。校验全集 = 本次加载的包 + 同目录其余规则包（规则宇宙），防止单包运行时
    # 误报「悬空」。
    rule_universe = set(all_rule_ids)
    for extra in Path(rules_files[0]).parent.glob("*.yaml"):
        if str(extra) not in rules_files:
            try:
                extra_pack = load_rules(str(extra))
                rule_universe |= {r.get("id") for r in extra_pack.get("rules", [])}
            except Exception:
                pass  # 解析失败的旁路包不阻断本次校验（它自身被加载时才校验）
    load_errors = []
    for rf, pack in packs:
        errs = validate_rules_pack(pack, rf, all_ids=rule_universe)
        load_errors.extend(f"{Path(rf).name}: {e}" for e in errs)
    if load_errors:
        print("ERROR: 规则包加载校验失败（配置错误，拒绝求值）：", file=sys.stderr)
        for e in load_errors:
            print(f"  {e}", file=sys.stderr)
        return 3

    for rf, pack in packs:
        if soc and pack.get("soc") and soc not in pack["soc"]:
            all_skipped.append(
                {
                    "rule_id": f"pack:{Path(rf).stem}",
                    "status": "SKIPPED",
                    "reason": f"规则包 soc 域 {pack['soc']} 不含 {soc}",
                }
            )
            continue
        metrics_spec = pack.get("metrics", {})
        facts = {}
        if rows:
            facts.update(pipe_metrics_from_rows(rows, metrics_spec))
            facts.update(time_metrics_from_rows(rows))
        # 规则包循环内按包名累积 facts（F4：全瀑布跑法下循环外只剩末包指标，
        # L1 pipe 指标丢失，审计承诺失效；分层结构天然保留指标来源信息）
        facts_by_pack[Path(rf).stem] = facts
        # MC² 隔离实验标量 → 衍生指标（r_value 等）
        if (
            "t_comm_us" in scalars
            and "t_compute_us" in scalars
            and scalars.get("t_comm_us")
            and scalars.get("t_compute_us")
        ):
            facts["r_value"] = {"value": scalars["t_compute_us"] / scalars["t_comm_us"]}
        if "mte2_ratio_full" in scalars and "mte2_ratio_pure" in scalars:
            full, pure = scalars["mte2_ratio_full"], scalars["mte2_ratio_pure"]
            if full is not None and pure is not None:
                facts["__mte2_pollution_gap"] = {"value": full - pure}
                if full:
                    facts["__mte2_pure_ratio_pct"] = {"value": pure / full * 100}
        # scalar 声明也并入 facts（供 get_metric_value 统一读取）；非数值(None)不并入
        for name, m in metrics_spec.items():
            if (
                m.get("source") == "scalar"
                and name in scalars
                and scalars[name] is not None
            ):
                facts[name] = {"value": scalars[name]}
        # L2 derived：occupancy 与头开销（依赖 scalar 提供的 physical_cores / task_duration_us）
        #  — 三个 derived 此前从未计算：L2_HEAD_OVERHEAD 永 SKIPPED、L2_UNDERUTILIZATION
        #    的 critical 档与头开销 critical 档是死代码（检视 F6）
        if "row_count" in facts and scalars.get("physical_cores"):
            facts["__occupancy_pct"] = {
                "value": facts["row_count"]["value"] / scalars["physical_cores"] * 100
            }
        if (
            scalars.get("task_duration_us")
            and facts.get("time_max_us", {}).get("value") is not None
        ):
            # 木桶口径：头开销以最慢核为基准（Task 总时长 − 最慢核主体时间）
            td, tmax = scalars["task_duration_us"], facts["time_max_us"]["value"]
            if td > 0:
                facts["__head_overhead_pct"] = {"value": (td - tmax) / td * 100}
        # L2 derived：host 下发占比（P2-4/G17 scene=cluster 档；host_dispatch_us
        # 暂无生产者——真实集群数据到位前本规则维持 SKIPPED，绑定契约兜底）
        if scalars.get("host_dispatch_us") and scalars.get("task_duration_us"):
            td2 = scalars["task_duration_us"]
            if td2 > 0:
                facts["__host_dispatch_pct"] = {
                    "value": scalars["host_dispatch_us"] / td2 * 100
                }

        rules = sorted(pack.get("rules", []), key=lambda r: r.get("priority", 999))
        for rule in rules:
            rule["_metrics_spec"] = metrics_spec
            rule["_layer"] = pack.get("layer", "")
            res = eval_rule(rule, facts, metrics_spec, scalars, disabled)
            if res.get("status") == "HIT":
                all_findings.append(res)
                # 路由副作用生效：禁用后续规则（跨包也生效，如 L3 污染禁用 L1_MTE2_BOUND）
                for act in (res.get("routing", {}) or {}).get("on_hit", []):
                    if isinstance(act, dict) and act.get("disable_rule"):
                        disabled.add(act["disable_rule"])
                    elif isinstance(act, str):
                        disabled.add(act)
            elif res.get("status") in ("SKIPPED", "INSUFFICIENT_DATA"):
                all_skipped.append(res)
            elif res.get("status") == "NOT_HIT":
                all_skipped.append(res)

    headline, verdict, next_steps = headline_and_verdict(
        all_findings, all_skipped, rule_seeds=all_rule_seeds
    )

    # 事实快照（审计：判定书可回溯到指标值与来源）——按规则包分层输出（F4）：
    # 全瀑布 --rules L3,L2,L1,L0 时保留每包的完整指标事实（含 L1 pipe 指标），
    # 不再只剩最后一个包的 facts；scalars 字段维持现状（不依赖包）
    def _snap(f: dict) -> dict:
        return {
            k: {
                kk: (round(vv, 2) if isinstance(vv, float) else vv)
                for kk, vv in v.items()
            }
            for k, v in f.items()
        }

    facts_snapshot = (
        {pack_stem: _snap(f) for pack_stem, f in facts_by_pack.items()}
        if (rows or any(v is not None and v for v in scalars.values()))
        else {}
    )

    # watch 观察档（检视 F29：R2 风险对策的「watch 档灰度输出」要求可见，
    # 此前只在 verdict.json 内部、终端与 recommended_next 均不呈现）
    watches = [
        (s["rule_id"], s["watch"])
        for s in all_skipped
        if s.get("status") == "NOT_HIT" and s.get("watch")
    ]
    if watches:
        # watch 档进 recommended_next（判定书与终端同源）
        next_steps.append(
            f"watch 档灰度输出：{', '.join(rid for rid, _ in watches)}"
            f"（指标已具备优化价值但未达判定线，可低成本先行）"
        )

    out = {
        "schema": SCHEMA_VERDICT,
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "soc": soc,
        "rules_files": rules_files,
        "csv": args.csv,
        "kernel_name": args.kernel_name,
        "rows": len(rows),
        "kernels_in_csv": sorted(k for k in kernel_groups if k != "<all>")
        if len(kernel_groups) > 1
        else None,
        "scalars": {k: v for k, v in scalars.items()},
        "facts": facts_snapshot,
        "findings": all_findings,
        "skipped": all_skipped,
        "verdict": verdict,
        "headline": headline,
        "recommended_next": next_steps,
    }
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    # 人读摘要（Agent/终端共用）
    print(f"verdict: {verdict}  |  {headline}")
    for f in all_findings:
        if f.get("status") == "HIT":
            print(
                f"  HIT  [{f['severity']:>8}/{f['confidence']:<6}] {f['rule_id']}: {f['name']}  "
                f"(est {f.get('speedup_range')})  evidence: {f['evidence']}"
            )
            for ce in f.get("counter_evidence", []):
                print(f"        ⚠ 反证[{ce['id']}]: {ce['note']}")
    if watches:
        print(f"  watch 档（未达判定线，观察项 {len(watches)}）：")
        for rid, note in watches:
            print(f"    👁 watch [{rid}]: {note}")
    n_skip = len([s for s in all_skipped if s.get("status") == "SKIPPED"])
    n_ins = len([s for s in all_skipped if s.get("status") == "INSUFFICIENT_DATA"])
    print(f"  skipped={n_skip}  insufficient_data={n_ins}")
    print(f"  → {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
