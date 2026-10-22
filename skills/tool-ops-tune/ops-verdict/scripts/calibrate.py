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
"""calibrate.py — 预估-实测校准回路（REVIEW2 G9 / 反馈 P1-2 ①，27-2 回路兑现）

设计对齐：
  - io-contracts.md 已冻结 calibration_data.json 契约（opstune.calibration/1.0，
    Step 5 逐轮产出），但全仓无任何脚本消费它——「校准回路」只有入口没有回路。
  - 本脚本是消费端：读多工程 calibration_data.json → 按 (mechanism, bound_rule)
    聚合 est vs measured 偏差 → 输出 speedup_range 修订建议（Markdown diff）。
  - **不自动回写规则 YAML**：修订建议经人工评审后改 YAML，改完必须过
    evals/run_evals.py 回归（反馈 P1-2 原文要求，阈值纪律不破坏）。

用法：
  python3 calibrate.py --inputs tuning_artifacts/gelu/calibration_data.json \
      tuning_artifacts/softmax/calibration_data.json -o calibration_report.md

偏差口径（每聚合组）：
  est_lo / est_hi ：该组全部条目 speedup_est 区间的均值
  measured_mean  ：实测 speedup 均值（KEEP 与 REVERT 都算——REVERT 是负样本，
                   两者都是「预估 vs 实测」的校准输入；噪声带内条目不算——
                   带内数字无方向信息）
  verdict        ：est 区间对 measured 的覆盖关系（covered / est_high / est_low /
                   flipped：KEEP 预估被实测 REVERT 推翻）
修订建议：
  est_high（系统性高估）→ 建议区间下压到 measured_mean 的 0.9~1.1 倍带；
  est_low（系统性低估）→ 建议区间上抬同带；样本 < 3 只提示不给出数值建议
  （单点不改阈值——阈值纪律的统计底线）。
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path


def load_entries(paths: list[str]) -> list[dict]:
    """读多份 calibration_data.json，展平为带工程来源的条目列表。"""
    entries = []
    for p in paths:
        data = json.loads(Path(p).read_text(encoding="utf-8"))
        if data.get("schema") != "opstune.calibration/1.0":
            print(
                f"WARN: {p} schema != opstune.calibration/1.0，仍按字段尽力解析",
                file=sys.stderr,
            )
        for e in data.get("entries", []):
            if not e.get("mechanism") or not e.get("bound_rule"):
                print(
                    f"WARN: {p} 存在缺 mechanism/bound_rule 的条目，跳过: {e}",
                    file=sys.stderr,
                )
                continue
            e["_source"] = data.get("operator", Path(p).parent.name)
            e["_path"] = p
            entries.append(e)
    return entries


def group_key(e: dict) -> str:
    return f"{e['mechanism']}@{e['bound_rule']}"


def measured_value(e: dict) -> float | None:
    v = e.get("speedup_measured")
    if v is None:
        return None
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def est_bounds(e: dict) -> tuple[float | None, float | None]:
    est = e.get("speedup_est")
    if not isinstance(est, (list, tuple)) or len(est) != 2:
        return None, None
    try:
        return float(est[0]), float(est[1])
    except (TypeError, ValueError):
        return None, None


def verdict_of(lo: float, hi: float, m: float, e: dict) -> str:
    """est 区间对 measured 的覆盖关系。"""
    if e.get("verdict") == "REVERT" and m is not None and m >= 1.0:
        return "flipped"  # 预估有收益、实测劣化被回滚 → 最强纠偏信号
    if m is None:
        return "no-data"
    if lo <= m <= hi:
        return "covered"
    if m < lo:
        return "est_high"  # 实测低于预估下界 → 系统性高估
    return "est_low"  # 实测高于预估上界 → 系统性低估


def main() -> int:
    ap = argparse.ArgumentParser(
        description="预估-实测校准回路（calibration_data.json 消费端）"
    )
    ap.add_argument(
        "--inputs",
        required=True,
        nargs="+",
        help="一份或多份 calibration_data.json（多工程聚合）",
    )
    ap.add_argument("-o", "--output", required=True, help="校准报告 Markdown 输出")
    args = ap.parse_args()

    entries = load_entries(args.inputs)
    if not entries:
        print("ERROR: 无有效条目（检查 calibration_data.json）", file=sys.stderr)
        return 1

    # 噪声带内条目无方向信息，不进聚合（io-contracts 纪律：带内不判收益/劣化）
    usable = [e for e in entries if e.get("verdict") in ("KEEP", "REVERT")]
    skipped_band = len(entries) - len(usable)
    if skipped_band:
        print(
            f"NOTE: {skipped_band} 条噪声带内/无 verdict 条目不进聚合", file=sys.stderr
        )

    groups: dict[str, list[dict]] = {}
    for e in usable:
        groups.setdefault(group_key(e), []).append(e)

    md = [
        "# 预估-实测校准报告（calibrate.py 自动生成）",
        "",
        f"- 生成时间：{datetime.datetime.now().isoformat(timespec='seconds')}",
        f"- 输入：{len(args.inputs)} 份 calibration_data.json，"
        f"{len(entries)} 条轮次记录（其中 {skipped_band} 条噪声带内不进聚合）",
        f"- 聚合维度：(mechanism, bound_rule) 共 {len(groups)} 组",
        "",
        "> **处置纪律**：本报告只产出修订**建议**（Markdown diff 形态）。修订须人工评审后",
        "> 手工改规则 YAML 的 speedup_range，改完必须过 `evals/run_evals.py` 回归全绿",
        "> 才可合入——不自动回写（阈值纪律，反馈 P1-2 原文）。",
        "",
    ]

    n_high = n_low = n_flip = 0
    for key in sorted(groups):
        g = groups[key]
        est_pairs = [est_bounds(e) for e in g]
        est_pairs = [(lo, hi) for lo, hi in est_pairs if lo is not None]
        measured = [measured_value(e) for e in g]
        measured = [m for m in measured if m is not None]
        if not est_pairs:
            continue
        lo = sum(p[0] for p in est_pairs) / len(est_pairs)
        hi = sum(p[1] for p in est_pairs) / len(est_pairs)
        m_mean = sum(measured) / len(measured) if measured else None
        m_min = min(measured) if measured else None
        m_max = max(measured) if measured else None
        verdicts = [
            verdict_of(
                est_bounds(e)[0] or lo, est_bounds(e)[1] or hi, measured_value(e), e
            )
            for e in g
        ]
        v_high = verdicts.count("est_high")
        v_low = verdicts.count("est_low")
        v_flip = verdicts.count("flipped")
        n_high += v_high
        n_low += v_low
        n_flip += v_flip

        md += [
            f"## {key}（{len(g)} 轮）",
            "",
            "| 项 | 值 |",
            "|---|---|",
            f"| est 区间（均值口径） | [{lo:.2f}, {hi:.2f}] |",
            f"| measured | mean {f'{m_mean:.3f}' if m_mean is not None else '—'}"
            f" / min {f'{m_min:.3f}' if m_min is not None else '—'}"
            f" / max {f'{m_max:.3f}' if m_max is not None else '—'}（{len(measured)} 条有数值） |",
            f"| 覆盖判定 | est_high ×{v_high} ｜ est_low ×{v_low} ｜ flipped ×{v_flip} ｜"
            f" covered ×{verdicts.count('covered')} |",
            f"| 轮次来源 | {', '.join(sorted({e['_source'] for e in g}))} |",
        ]

        # 修订建议：样本 ≥3 且方向一致才给数值（单点不改阈值）
        if len(g) >= 3 and m_mean is not None:
            if v_high >= 2 and v_high > v_low:
                md += [
                    "",
                    "**建议（est 系统性偏高）**：speedup_range 下压——",
                    "```diff",
                    f"- speedup_range: [{lo:.2f}, {hi:.2f}]",
                    f"+ speedup_range: [{m_mean * 0.9:.2f}, {m_mean * 1.1:.2f}]",
                    "```",
                    f"依据：{v_high}/{len(g)} 轮实测低于预估下界，实测均值 {m_mean:.3f}。",
                ]
            elif v_low >= 2 and v_low > v_high:
                md += [
                    "",
                    "**建议（est 系统性偏低）**：speedup_range 上抬——",
                    "```diff",
                    f"- speedup_range: [{lo:.2f}, {hi:.2f}]",
                    f"+ speedup_range: [{m_mean * 0.9:.2f}, {m_mean * 1.1:.2f}]",
                    "```",
                    f"依据：{v_low}/{len(g)} 轮实测高于预估上界，实测均值 {m_mean:.3f}。",
                ]
            elif v_flip >= 2:
                md += [
                    "",
                    f"**建议（方向翻车）**：{v_flip} 轮预估 KEEP 实测 REVERT——"
                    "先查机制适用前提是否写错（该 (mechanism, bound_rule) 组合的"
                    "机制卡「适用前提」或反证条目可能漏了排除条件），"
                    "比压区间更优先。",
                ]
            else:
                md += ["", "覆盖良好（或方向不一致），无需修订。"]
        else:
            md += [
                "",
                f"样本 {len(g)} < 3：只记录不给出数值建议（单点不改阈值——统计底线）。",
            ]
        md.append("")

    md += [
        "## 汇总",
        "",
        f"- est_high（高估）{n_high} 轮 ｜ est_low（低估）{n_low} 轮 ｜ "
        f"flipped（方向翻车）{n_flip} 轮",
        "- 跑得越多 🟡 越多的问题开始收敛：上述建议经评审回写后，"
        "speedup_est 的可信度随调优次数复利提升。",
    ]

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"calibration report → {out}（{len(groups)} 组 / {len(usable)} 轮）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
