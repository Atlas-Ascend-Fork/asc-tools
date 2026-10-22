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
"""delta_compare.py — 前后对比硬校验（口径一致性 + 精度门禁 + 噪声带三段结论）

设计对齐（docs/算子调优AGENT设计文档.md §6 数据契约）：
  - HARD_KEYS 口径不一致 → exit 2 拒绝比较（性能数字在口径漂移下无意义）。
  - MECHANISM_KEYS（tiling_key/block_dim）默认 WARN 写入报告；--strict 恢复禁比
    （分核/tiling 类优化本身就会改变这两个字段——这是对优化本质的建模）。
  - 精度门禁（第三道，脚本化）：任一侧 precision != pass → exit 2。
  - 噪声带三段结论（实测纪律）：±5% 内不判收益也不判劣化 → 加采样轮次。

用法：
  python3 delta_compare.py --baseline baseline/result.json --after round-01/result.json \
      --output comparison.md
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

HARD_KEYS = (
    "op",
    "soc",
    "mode",
    "timing_method",
    "baseline_kind",
    "cann_version",
    "device_id",
    "shape",
    "dtype",
    "format",
    "warmup",
    "repeat",
)
MECHANISM_KEYS = ("tiling_key", "block_dim")
NOISE_BAND = 0.05  # ±5%：噪声带内不归因（run 间噪声 >5% 时小于噪声的收益不得宣称）


def load(p: str) -> dict:
    return json.loads(Path(p).read_text(encoding="utf-8"))


def fmt_speedup(b, a):
    if b and a and a > 0:
        return b / a
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="前后对比硬校验")
    ap.add_argument(
        "--baseline", required=True, help="snapshot.py 产出的基线 result.json"
    )
    ap.add_argument("--after", required=True, help="优化后 result.json（同口径）")
    ap.add_argument("--output", required=True, help="comparison.md 输出")
    ap.add_argument(
        "--strict",
        action="store_true",
        help="机制变量（tiling_key/block_dim）差异也算口径不一致",
    )
    ap.add_argument("--noise-band", type=float, default=NOISE_BAND)
    args = ap.parse_args()

    base, after = load(args.baseline), load(args.after)

    # ---- 门禁 1：口径硬校验 ----
    hard = tuple(HARD_KEYS) + (MECHANISM_KEYS if args.strict else ())
    diffs = [
        f"{k}: {base.get(k)!r} != {after.get(k)!r}"
        for k in hard
        if base.get(k) is not None
        and after.get(k) is not None
        and base.get(k) != after.get(k)
    ]
    if diffs:
        print(
            "ERROR: 基线与优化后口径不一致，禁止比较：\n  " + "\n  ".join(diffs),
            file=sys.stderr,
        )
        print("任一口径字段变化都应重建基线（跨口径比较禁令）。", file=sys.stderr)
        return 2

    # ---- 门禁 2：精度门禁（第三道）----
    if base.get("precision") != "pass" or after.get("precision") != "pass":
        print(
            "ERROR: 只有精度均为 pass 的结果才能生成性能结论 "
            f"(baseline={base.get('precision')}, after={after.get('precision')})",
            file=sys.stderr,
        )
        return 2

    # 机制变量 WARN ----
    mech_warns = [
        f"{k}: {base.get(k)!r} → {after.get(k)!r}"
        for k in MECHANISM_KEYS
        if base.get(k) is not None
        and after.get(k) is not None
        and base.get(k) != after.get(k)
    ]

    b_us, a_us = base.get("kernel_avg_us"), after.get("kernel_avg_us")
    speedup = fmt_speedup(b_us, a_us)

    # ---- 三段结论 ----
    if speedup is None:
        conclusion = "UNRESOLVED：缺少可比的 kernel_avg_us（或标记为 partial/dry_run）"
        action = "补齐性能数据或更换计时口径后重比"
    elif speedup > 1 + args.noise_band:
        conclusion = (
            f"提升：speedup={speedup:.3f}x（>{1 + args.noise_band:.2f} 超出噪声带）"
        )
        action = "KEEP：该轮保留，进入下一轮单变量迭代"
    elif speedup < 1 - args.noise_band:
        conclusion = f"劣化：speedup={speedup:.3f}x（<{1 - args.noise_band:.2f}）"
        action = "REVERT：回滚本轮改动；回滚后须复跑确认回到基线水平再进入下一轮"
    else:
        conclusion = (
            f"噪声带内（{speedup:.3f}x，±{args.noise_band * 100:.0f}% 内）："
            "不判收益也不判劣化"
        )
        # F9：repeat 是 HARD key——翻倍复测必须「以新 repeat 同步重采基线与优化侧、
        # 重建快照（新目录）后再比」，直接翻倍 after 会被口径硬校验 exit 2 拒绝
        action = (
            "增加采样轮次（repeat 翻倍）复测：以新 repeat **同步重采基线与优化侧**、"
            "在新目录重建两侧快照后再比（repeat 属口径指纹，单侧翻倍会被 HARD 校验拒绝）；"
            "仍落带内按无稳定收益处置"
        )

    # 误差指纹对照（精度数字必须同量级）
    err_lines = []
    for k in ("mismatch", "max_abs_err", "max_rel_err"):
        bv, av = base.get(k), after.get(k)
        if bv is not None or av is not None:
            err_lines.append(f"| {k} | {bv} | {av} |")

    # ---- 抖动诊断段（F20：分布字段存在时输出；缺失时向后兼容不输出）----
    jitter_lines = []
    for k in ("kernel_p50_us", "kernel_p95_us", "kernel_max_us"):
        bv, av = base.get(k), after.get(k)
        if bv is not None or av is not None:
            jitter_lines.append(f"| {k} | {bv} | {av} |")
    jitter_notes = []
    for side_name, snap in (("baseline", base), ("after", after)):
        p50, p95, avg = (
            snap.get("kernel_p50_us"),
            snap.get("kernel_p95_us"),
            snap.get("kernel_avg_us"),
        )
        if p50 is not None and p95 is not None and avg:
            if p50 * 2 < p95:
                jitter_notes.append(
                    f"{side_name}: p95（{p95}）> 2×p50（{p50}）——疑似双峰分布，"
                    "先查是否有两种稳态（如 DVFS 档位 / 缓存对齐边界）再下结论"
                )
            elif p95 / avg > 1.5:
                jitter_notes.append(
                    f"{side_name}: p95 偏离均值 {p95 / avg:.2f}x——长尾显著，"
                    "单均值口径下噪声带结论的置信度不足"
                )
    b_p50, b_p95 = base.get("kernel_p50_us"), base.get("kernel_p95_us")
    a_p50, a_p95 = after.get("kernel_p50_us"), after.get("kernel_p95_us")
    if b_p50 and b_p95 and a_p50 and a_p95:
        if (b_p95 / b_p50) < (a_p95 / a_p50) * 0.5:
            jitter_notes.append(
                "优化侧 p95/p50 比值显著大于基线（≥2x）——收益集中在快轮次、"
                "长尾反而恶化时，geomean/avg 口径可能掩盖劣化"
            )

    md = [
        f"# 前后对比：{base.get('op')} — {after.get('variant', 'optimized')}",
        "",
        f"- 生成时间：{datetime.datetime.now().isoformat(timespec='seconds')}",
        f"- 口径：{base.get('soc')} / CANN {base.get('cann_version')} / device {base.get('device_id')}"
        f" / timing={base.get('timing_method')} / warmup={base.get('warmup')} repeat={base.get('repeat')}",
        f"- 精度：baseline={base.get('precision')}  after={after.get('precision')}（工程现有测试口径）",
        "",
        "## 结论",
        "",
        f"**{conclusion}**",
        "",
        f"处置：{action}",
        "",
        "## 数据",
        "",
        "| 项 | baseline | after |",
        "|---|---|---|",
        f"| kernel_avg_us | {b_us} | {a_us} |",
        f"| speedup | — | {speedup if speedup is None else round(speedup, 3)} |",
        f"| status | {base.get('status')} | {after.get('status')} |",
        *err_lines,
    ]
    if mech_warns:
        md += [
            "",
            "## 机制变量（预期内变化，WARN 记录）",
            "",
            *[f"- {w}" for w in mech_warns],
        ]
    if jitter_lines:
        md += [
            "",
            "## 抖动诊断（分布字段）",
            "",
            "| 项 | baseline | after |",
            "|---|---|---|",
            *jitter_lines,
        ]
        if jitter_notes:
            md += ["", *[f"- ⚠ {n}" for n in jitter_notes]]
    if base.get("verdict_file") or after.get("verdict_file"):
        md += [
            "",
            "## 关联判定书",
            "",
            f"- baseline verdict: {base.get('verdict_file')}",
            f"- after verdict: {after.get('verdict_file')}",
        ]

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"结论: {conclusion}")
    print(f"处置: {action}")
    if mech_warns:
        print("WARN 机制变量变化（预期内）: " + "; ".join(mech_warns))
    print(f"→ {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
