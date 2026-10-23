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
"""mc2_search.py — MC² 配平系统搜索器（REVIEW2 G6-M2 / 反馈 P1-4）

设计对齐：
  - mc2-balancing.md 机制卡写明「统一搜索算法（禁止经验值交付）」：四剪枝 +
    Top-N 输出——此前只有 markdown 描述，全仓无执行载体（REVIEW2 G6 定性）。
  - 本脚本是执行载体：输入 TilingData + R（隔离测试产出）→ 长短块排布候选
    搜索 → 四剪枝 → Top-N 候选 + 剪枝记录（配平方案卡必须附本输出）。

搜索空间（机制卡原文语义）：
  长块 M ∈ 候选集（baseM 合法对齐粒度的倍数序列）
  短块 M = 长块 M / ρ（对齐修正），ρ = max(R, 1/R)（长块与短块 M 之比）
  块数切分：长块数 + 短块数在总 M（矩阵行数）预算内排布

四剪枝（可行性，机制卡编号）：
  ① 核利用率：usedCoreNum × 块数覆盖不下降（候选的覆盖块数 ≥ 当前方案的）
  ② L1/L0 容量：baseM/baseN/baseK 组合不超片上预算（soc-spec.yaml 容量表）
  ③ 膨胀下界：计算膨胀 + 通信膨胀估算不超过收益（短块填充带来的冗余开销）
  ④ 对齐约束：M/N/K 对齐要求（512B 地址对齐 → 分形对齐 16/32 粒度）

输出（opstune.mc2-search/1.0）：
  candidates[]：Top-N（--top，默认 5）——每候选带 block 布局、容量占用计算
                过程、剪枝检查通过记录
  pruned[]：被剪枝候选 + 剪枝原因（可审计证据：剪枝记录本身是证据）
  meta：R / ρ / 搜索空间统计 / soc 容量参数溯源

用法：
  python3 mc2_search.py --m 4096 --base-n 256 --base-k 256 --used-core-num 16 \
      --r-value 2.4 --soc a5 [-o candidates.json]
  # R 来自 isolation_test.py 的 r_value；TilingData 来自 host 侧打印（机制卡 MC-1）
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

SCHEMA = "opstune.mc2-search/1.0"
SOC_SPEC_FILE = "soc-spec" + ".yaml"  # 判定引擎同源峰值标尺
DEFAULT_SOC_SPEC = (
    Path(__file__).resolve().parents[2] / "ops-verdict" / "references" / SOC_SPEC_FILE
)

# 对齐粒度（机制卡剪枝④：分形对齐 16 / 512B 地址对齐折算元素粒度）
FRACTAL_ALIGN = 16  # M/N/K 分形对齐粒度（Cube MAC 16×16×16）
ELEM_ALIGN_512B = {  # 512B 地址对齐折算的最小元素数（dtype 元素大小 → 元素数）
    "fp16": 256,
    "bf16": 256,
    "fp32": 128,
    "int8": 512,
}
# 512B 对齐是搬运效率推荐态而非硬约束（DataCopyPad 场景允许非 512B 对齐；
# 分形对齐 16 才是 Cube 的硬约束）——剪枝④按此分两级：分形不齐 → 剪枝；
# 512B 不齐 → 记录 warn 不剪枝（候选照常输出，标注 warn_align_512b）


def load_soc_caps(soc: str, soc_spec_path: Path) -> dict:
    """从 soc-spec.yaml 取容量约束（剪枝②的真源，不硬编码）。"""
    import yaml

    spec = yaml.safe_load(Path(soc_spec_path).read_text(encoding="utf-8"))
    block = spec.get("socs", {}).get(soc)
    if not block:
        raise SystemExit(f"ERROR: soc-spec 无 {soc} 条目（{soc_spec_path}）")
    buf = block.get("buffer", {})
    ref = block.get("reference", {})
    return {
        "l1_kb": buf.get("l1_kb"),
        "l0a_kb": buf.get("l0a_kb"),
        "l0b_kb": buf.get("l0b_kb"),
        "l0c_kb": buf.get("l0c_kb"),
        "ub_kb": buf.get("ub_kb"),
        "core_num": ref.get("ai_core_cnt", {}).get("value"),
        "source": f"soc-spec:{soc}",
    }


def align_up(v: int, grain: int) -> int:
    return ((v + grain - 1) // grain) * grain


def long_block_candidates(
    m: int, base_m_min: int, base_m_max: int, dtype: str, step_grain: int
) -> list[int]:
    """长块 M 候选集：[base_m_min, min(base_m_max, m)] 内按 step_grain 步进的
    对齐值 + m 本身（整除免短块收尾）。"""
    align = max(FRACTAL_ALIGN, ELEM_ALIGN_512B.get(dtype, FRACTAL_ALIGN))
    hi = min(base_m_max, align_up(m, align))
    cands = set()
    v = align_up(base_m_min, align)
    while v <= hi:
        cands.add(v)
        v = align_up(v + step_grain, align)
    cands.add(hi)
    return sorted(cands)


def prune(candidate: dict, ctx: dict) -> tuple[list[str], list[str]]:
    """四剪枝：返回 (剪枝原因, 警告) 两列表（原因空 = 通过全部剪枝）。"""
    reasons, warns = [], []
    # ④ 对齐约束：分形对齐 16 是 Cube 硬约束（剪枝）；512B 对齐是搬运推荐态（警告）
    for k in ("long_m", "short_m"):
        if candidate[k] % FRACTAL_ALIGN != 0:
            reasons.append(f"④ {k}={candidate[k]} 不满足分形对齐 {FRACTAL_ALIGN}")
    if candidate["long_m"] % ctx["elem_align_512b"] != 0:
        warns.append(
            f"长块 M={candidate['long_m']} 非 512B 对齐粒度"
            f"（{ctx['elem_align_512b']} 元素）——搬运效率推荐态，DataCopyPad 场景可接受"
        )
    if candidate["short_m"] % ctx["elem_align_512b"] != 0:
        warns.append(
            f"短块 M={candidate['short_m']} 非 512B 对齐粒度——同上，记录不剪枝"
        )
    if ctx["base_n"] % FRACTAL_ALIGN != 0 or ctx["base_k"] % FRACTAL_ALIGN != 0:
        reasons.append("④ baseN/baseK 未分形对齐（输入参数问题）")

    # ② L1/L0 容量：A/B tile + 双 buffer 不超预算（元素 × dtype 字节 × buffer 份数）
    elem_bytes = ctx["elem_bytes"]
    buf_num = ctx.get("l1_buffer_num", 2)
    a_kb = candidate["long_m"] * ctx["base_k"] * elem_bytes / 1024
    b_kb = ctx["base_n"] * ctx["base_k"] * elem_bytes / 1024
    l1_use_kb = (a_kb + b_kb) * buf_num
    if l1_use_kb > ctx["l1_kb"]:
        reasons.append(
            f"② L1 超预算：({a_kb:.0f}+{b_kb:.0f})×{buf_num} = {l1_use_kb:.0f}KB"
            f" > {ctx['l1_kb']}KB"
        )
    l0a_use_kb = candidate["long_m"] * ctx["base_k"] * elem_bytes / 1024
    l0b_use_kb = ctx["base_n"] * ctx["base_k"] * elem_bytes / 1024
    if l0a_use_kb > ctx["l0a_kb"]:
        reasons.append(f"② L0A 超预算：{l0a_use_kb:.0f}KB > {ctx['l0a_kb']}KB")
    if l0b_use_kb > ctx["l0b_kb"]:
        reasons.append(f"② L0B 超预算：{l0b_use_kb:.0f}KB > {ctx['l0b_kb']}KB")
    l0c_use_kb = candidate["long_m"] * ctx["base_n"] * elem_bytes / 1024
    if l0c_use_kb > ctx["l0c_kb"]:
        reasons.append(f"② L0C 超预算：{l0c_use_kb:.0f}KB > {ctx['l0c_kb']}KB")

    # ① 核利用率：候选覆盖块数（长+短块要占满核）不下降
    if candidate["blocks_total"] < ctx["current_blocks"]:
        reasons.append(
            f"① 核覆盖下降：候选块数 {candidate['blocks_total']}"
            f" < 当前 {ctx['current_blocks']}"
        )
    if candidate["blocks_total"] < ctx["used_core_num"]:
        reasons.append(
            f"① 块数 < usedCoreNum（{candidate['blocks_total']}"
            f" < {ctx['used_core_num']}）：有空闲核"
        )

    # ③ 膨胀下界：短块膨胀（补齐对齐引入的冗余计算）+ 排布浪费 ≤ 掩盖收益
    # 收益上界（机制卡：配平收益 ≤ min 侧掩盖，估为 (ρ-1)/ρ × T_max 占比）
    # 膨胀 = 冗余元素 / 总元素；排布浪费 = 尾部空隙 / 总 M
    inflate_pct = candidate.get("inflation_pct", 0.0) + candidate.get(
        "tail_waste_pct", 0.0
    )
    if inflate_pct > ctx["max_inflation_pct"]:
        reasons.append(
            f"③ 膨胀 {inflate_pct:.1f}% > 上限 {ctx['max_inflation_pct']}%"
            f"（对齐冗余 {candidate.get('inflation_pct', 0):.1f}% + "
            f"尾部浪费 {candidate.get('tail_waste_pct', 0):.1f}%）"
        )
    return reasons, warns


def build_candidate(long_m: int, rho: float, m: int, ctx: dict) -> dict:
    # 短块 M = 长块 M / ρ（对齐修正向上）
    short_m = align_up(max(1, round(long_m / rho)), FRACTAL_ALIGN)
    # 排布：R>1（计算慢）→ 长块主导；R<1（通信慢）→ 短块多塞间隙。
    # 简化排布（机制卡「实际排布按 Top-N 实测修正」）：长块铺满为主，
    # 末段用短块收尾降低尾部浪费。
    long_blocks = m // long_m
    remain = m - long_blocks * long_m
    short_blocks = 0
    if remain > 0:
        if remain % short_m == 0 and remain // short_m > 0:
            short_blocks = remain // short_m
        else:
            # 短块收不齐：短块数 = ceil，末块对齐膨胀记入 inflation
            short_blocks = -(-remain // short_m)
    blocks_total = long_blocks + short_blocks
    covered = long_blocks * long_m + abs(short_blocks) * short_m
    inflation_pct = max(0.0, (covered - m) / m * 100) if m > 0 else 0.0
    tail_waste_pct = max(0.0, (covered - m) / m * 100) if m > 0 else 0.0
    return {
        "variant": f"long{long_m}_short{short_m}",
        "long_m": long_m,
        "short_m": short_m,
        "rho": round(rho, 3),
        "long_blocks": long_blocks,
        "short_blocks": short_blocks,
        "blocks_total": blocks_total,
        "inflation_pct": round(inflation_pct, 2),
        "tail_waste_pct": round(tail_waste_pct, 2),
        "capacity_kb": {
            "l1": round(
                (long_m + ctx["base_n"])
                * ctx["base_k"]
                * ctx["elem_bytes"]
                * ctx.get("l1_buffer_num", 2)
                / 1024,
                1,
            ),
            "l0a": round(long_m * ctx["base_k"] * ctx["elem_bytes"] / 1024, 1),
            "l0b": round(ctx["base_n"] * ctx["base_k"] * ctx["elem_bytes"] / 1024, 1),
            "l0c": round(long_m * ctx["base_n"] * ctx["elem_bytes"] / 1024, 1),
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description="MC² 配平系统搜索器（四剪枝 + Top-N，禁经验值交付）"
    )
    ap.add_argument("--m", type=int, required=True, help="矩阵 M（总行数，TilingData）")
    ap.add_argument("--base-n", type=int, required=True, help="baseN（TilingData）")
    ap.add_argument("--base-k", type=int, required=True, help="baseK（TilingData）")
    ap.add_argument(
        "--used-core-num",
        type=int,
        required=True,
        help="usedCoreNum（TilingData；机制卡 MC-1 硬约束）",
    )
    ap.add_argument(
        "--r-value",
        type=float,
        required=True,
        help="隔离测试 R = T_compute/T_comm（isolation_test.py 的 r_value）",
    )
    ap.add_argument("--soc", default="a5", help="a2/a3/a5（容量约束读 soc-spec）")
    ap.add_argument("--dtype", default="fp16", choices=["fp16", "bf16", "fp32", "int8"])
    ap.add_argument("--base-m-min", type=int, default=64, help="长块 M 搜索下界")
    ap.add_argument("--base-m-max", type=int, default=512, help="长块 M 搜索上界")
    ap.add_argument("--step", type=int, default=32, help="搜索步进（粒度）")
    ap.add_argument(
        "--l1-buffer-num", type=int, default=2, help="L1 buffer 份数（pingpong=2）"
    )
    ap.add_argument(
        "--max-inflation-pct",
        type=float,
        default=15.0,
        help="剪枝③膨胀上限 %%（默认 15，🟡 工程初值）",
    )
    ap.add_argument(
        "--current-blocks",
        type=int,
        default=None,
        help="当前方案块数（剪枝①基准；缺省取 used-core-num）",
    )
    ap.add_argument("--top", type=int, default=5, help="Top-N 输出")
    ap.add_argument("--soc-spec", default=str(DEFAULT_SOC_SPEC))
    ap.add_argument("-o", "--output", default=None, help="candidates.json 输出")
    args = ap.parse_args()

    caps = load_soc_caps(args.soc, Path(args.soc_spec))
    rho = max(args.r_value, 1.0 / args.r_value) if args.r_value > 0 else 1.0
    ctx = {
        "base_n": args.base_n,
        "base_k": args.base_k,
        "used_core_num": args.used_core_num,
        "current_blocks": args.current_blocks
        if args.current_blocks is not None
        else args.used_core_num,
        "elem_bytes": {"fp16": 2, "bf16": 2, "fp32": 4, "int8": 1}[args.dtype],
        "elem_align": max(FRACTAL_ALIGN, ELEM_ALIGN_512B[args.dtype]),
        "elem_align_512b": ELEM_ALIGN_512B[args.dtype],
        "l1_kb": caps["l1_kb"],
        "l0a_kb": caps["l0a_kb"],
        "l0b_kb": caps["l0b_kb"],
        "l0c_kb": caps["l0c_kb"],
        "l1_buffer_num": args.l1_buffer_num,
        "max_inflation_pct": args.max_inflation_pct,
    }

    # 搜索 + 剪枝
    candidates, pruned = [], []
    space = long_block_candidates(
        args.m, args.base_m_min, args.base_m_max, args.dtype, args.step
    )
    for lm in space:
        c = build_candidate(lm, rho, args.m, ctx)
        reasons, warns = prune(c, ctx)
        if warns:
            c["warn_align_512b"] = warns
        if reasons:
            pruned.append({"variant": c["variant"], "reasons": reasons})
        else:
            candidates.append(c)

    # 排序取 Top-N：块数多（核覆盖好）优先，其次膨胀小（机制卡：估算 ≠ 实测，
    # Top-N 供逐个实测——排序只是实测顺序，不是最终结论）
    candidates.sort(
        key=lambda c: (-c["blocks_total"], c["inflation_pct"] + c["tail_waste_pct"])
    )
    top = candidates[: args.top]

    result = {
        "schema": SCHEMA,
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "soc": args.soc,
        "soc_caps": {
            k: caps[k] for k in ("l1_kb", "l0a_kb", "l0b_kb", "l0c_kb", "core_num")
        },
        "soc_caps_source": caps["source"],
        "input": {
            "m": args.m,
            "base_n": args.base_n,
            "base_k": args.base_k,
            "used_core_num": args.used_core_num,
            "r_value": args.r_value,
            "dtype": args.dtype,
            "rho": round(rho, 3),
        },
        "search_space": {
            "long_m_range": [min(space), max(space)] if space else [],
            "n_explored": len(space),
        },
        "candidates": top,
        "pruned": pruned,
        "notes": [
            "排序 = 实测顺序（块数优先、膨胀次之），不是最终结论——每候选须逐个实测"
            "（tileCnt 扫描、跨 rank max 口径）后择优（mc2-balancing.md）",
            f"剪枝③膨胀上限 {args.max_inflation_pct}% 为 🟡 工程初值，实测校准后回写",
        ],
        "next": "逐候选实测同口径（msprof task-based、同 warmup/loops、跨 rank max），"
        "delta_compare 出三段结论；配平方案卡必须附本输出（含剪枝记录）",
    }
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(f"→ {out}")
    print(
        f"MC² 配平搜索：R={args.r_value} ρ={rho:.2f} → 探索 {len(space)} / "
        f"通过 {len(candidates)} / 剪枝 {len(pruned)}，Top-{len(top)}："
    )
    for c in top:
        print(
            f"  {c['variant']}: 长块×{c['long_blocks']} + 短块×{c['short_blocks']}"
            f" = {c['blocks_total']} 块，膨胀 {c['inflation_pct'] + c['tail_waste_pct']:.1f}%，"
            f"L1 {c['capacity_kb']['l1']}KB"
        )
    if not top:
        print(
            "  （无候选通过四剪枝——检查 TilingData/R 输入或放宽 --max-inflation-pct）",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
