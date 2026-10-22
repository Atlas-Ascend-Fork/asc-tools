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
"""tiling_candidates.py — Tiling 候选参数生成器·四族（REVIEW2 G6 短期 / 整改 P0-5）

设计对齐：
  - 机制卡 ascendc-tiling.md「禁止经验值交付」与 mc2-balancing.md 四剪枝此前
    只有 markdown 描述，全仓无候选生成执行载体——Block Dim 是最低垂果实
    （L2 yaml 引用 4→20 核 4.75x 实测案例），也是最容易翻车的（容量联动
    device fault 是 troubleshooting 运行类第一条）。
  - 本脚本把候选生成 + 容量预算校验变成可执行载体：L2_UNDERUTILIZATION
    命中后不再靠 LLM 经验拍 block_dim / tile。
  - 推导逻辑以契约方式移植一线 tiling 推导脚本
    （elewise/reduction/broadcast/conversion 四族 fallback 参考实现），
    不直接复制其代码——本仓输出的是「候选表 + 逐候选容量占用计算过程」，
    单候选交付仍走 perf-implementer 单变量铁律。

覆盖四族（首期）：
  elementwise：一维展开 → 多核均分（512 元素对齐）→ UB 切分（256B 对齐，双缓冲）
  reduction：A/R 轴折叠 → 行块切分（rows_per_ub 受 UB 预算）→ 多核按 outer 分
  broadcast：输出 shape 折叠 → UB 切分（多输入共享预算）→ 多核按 tile 数分
  conversion：channel-flat → tile 元素对齐 → 多核按 tile 分（offset 表）

用法：
  python3 tiling_candidates.py --family elementwise --shape 8448,4096 --dtype fp16 \\
      --soc a5 [--input-count 1] [--output candidates.json]

输出（opstune.tiling-candidates/1.0）：
  { soc, family, shape, dtype, soc_spec_source,
    candidates: [ {variant, block_dim, tile..., capacity_kb, capacity_note,
                   derived: {…计算过程}, aligned: true/false} ],
    pruned: [ {variant, reason} ],  // 剪枝记录（容量超限/未对齐）——可审计证据
    warning: "候选表仅供参数选择，实施须单变量 + 容量复核 + 全量精度" }
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

SCHEMA = "opstune.tiling-candidates/1.0"

# soc-spec 峰值标尺文件名（判定引擎同源数据）
SOC_SPEC_NAME = "soc-spec" + ".yaml"

# 常量（与参考实现同款契约值；来源标注）
ELEM_ALIGN_512 = 512  # [tuning-practice] 多核元素对齐粒度
ALIGN_256B = 256  # [tuning-practice] UB 对齐（字节）
MIN_TILING_BITS = 32768  # [tuning-practice] 每核最小 4KB（bits）
BUFFER_NUM_DEFAULT = 2  # [tuning-practice] 双缓冲队列数
DTYPE_BYTES = {
    "fp16": 2,
    "float16": 2,
    "bf16": 2,
    "bfloat16": 2,
    "fp32": 4,
    "float32": 4,
    "int8": 1,
    "int32": 4,
}


def load_soc_spec(path: Path) -> dict:
    import yaml

    return yaml.safe_load(path.read_text(encoding="utf-8"))


def soc_caps(soc_spec: dict, soc: str) -> dict:
    """解析 SoC 约束：核数（reference 默认）+ UB 容量（同架构一致）。"""
    block = soc_spec.get("socs", {}).get(soc)
    if not block:
        raise SystemExit(
            f"ERROR: soc-spec 无 {soc} 条目（支持：{list(soc_spec.get('socs', {}))}）"
        )
    ref = block.get("reference", {})
    buf = block.get("buffer", {})
    return {
        "core_num": ref.get("ai_core_cnt", {}).get("value")
        or ref.get("vector_core_cnt", {}).get("value")
        or 1,
        "vec_core_num": ref.get("vector_core_cnt", {}).get("value") or 1,
        "ub_kb": buf.get("ub_kb", 192),
        "l1_kb": buf.get("l1_kb", 512),
        "soc_name": block.get("name", soc),
    }


def _align_up(v: int, a: int) -> int:
    return (v + a - 1) // a * a


def _multicore_core_num(total_elems: int, elem_bytes: int, available_cores: int) -> int:
    """每核最小 4KB 约束下的用核数（与参考实现 calc_multicore_split 同款契约）。"""
    min_bits = elem_bytes * 8
    core_num = (total_elems * min_bits + MIN_TILING_BITS - 1) // MIN_TILING_BITS
    return max(1, min(core_num, available_cores))


def candidates_elementwise(
    shape: list[int], dtype: str, caps: dict, input_count: int
) -> tuple[list, list]:
    dim0 = 1
    for d in shape:
        dim0 *= d
    elem_bytes = DTYPE_BYTES.get(dtype, 2)
    cores = _multicore_core_num(dim0, elem_bytes, caps["core_num"])
    ub_bytes = caps["ub_kb"] * 1024

    out, pruned = [], []
    # 候选 1：满核均分（Block Dim 提升机制——L2_UNDERUTILIZATION 的首选候选）
    block_former = _align_up((dim0 + cores - 1) // cores, ELEM_ALIGN_512)
    block_num = (dim0 + block_former - 1) // block_former
    block_tail = dim0 - (block_num - 1) * block_former
    # 候选 2/3：UB tile 缩减变体（容量联动防护：tile 越小 UB 占用越低，搬运次数越多）
    for variant, buf_div in (
        ("full-cores", BUFFER_NUM_DEFAULT * max(input_count, 1)),
        ("half-tile", BUFFER_NUM_DEFAULT * max(input_count, 1) * 2),
    ):
        divisor = buf_div * elem_bytes
        max_elem = ub_bytes // divisor
        align_elems = max(ALIGN_256B // elem_bytes, 1)
        ub_former = max((max_elem // align_elems) * align_elems, align_elems)
        if variant == "half-tile":
            ub_former = max(ub_former // 2 // align_elems * align_elems, align_elems)
        cap_kb = round(
            ub_former * elem_bytes * BUFFER_NUM_DEFAULT * max(input_count, 1) / 1024, 1
        )
        if cap_kb > caps["ub_kb"]:
            pruned.append(
                {
                    "variant": variant,
                    "reason": f"UB 预算超限（{cap_kb}KB > {caps['ub_kb']}KB）",
                }
            )
            continue
        out.append(
            {
                "variant": variant,
                "block_dim": cores,
                "block_former": block_former,
                "block_num": block_num,
                "block_tail": block_tail if block_tail > 0 else block_former,
                "ub_former": ub_former,
                "ub_loop_former": (block_former + ub_former - 1) // ub_former,
                "capacity_kb": cap_kb,
                "capacity_note": f"UB {cap_kb}KB / {caps['ub_kb']}KB（双缓冲 × {max(input_count, 1)} 输入）",
                "derived": {
                    "dim0": dim0,
                    "cores": cores,
                    "per_core_min_4kb": "dim0×dtype_bits / 32768 向上取整，min(可用核数)",
                    "block_former": f"ceil(dim0/cores) 向上对齐 {ELEM_ALIGN_512} 元素",
                    "ub_former": f"(ub_size / (buffer_num×inputs×elem_bytes)) 向下对齐 {ALIGN_256B}B",
                },
                "aligned": block_former % ELEM_ALIGN_512 == 0
                and ub_former % align_elems == 0,
            }
        )
    return out, pruned


def candidates_reduction(
    shape: list[int], dtype: str, caps: dict, axis: int
) -> tuple[list, list]:
    """行块切分（AR 型：outer × reduce 折叠）。axis 为归约轴（-1 语义 = 最后一维）。"""
    if axis < 0:
        axis = len(shape) + axis
    outer = 1
    for i, d in enumerate(shape):
        if i != axis:
            outer *= d
    r = shape[axis]
    elem_bytes = DTYPE_BYTES.get(dtype, 2)
    ub_bytes = caps["ub_kb"] * 1024

    out, pruned = [], []
    # rows_per_ub 受 UB 预算（一行 r 元素 × 双缓冲）
    row_bytes = r * elem_bytes
    rows_per_ub = max(ub_bytes // (row_bytes * BUFFER_NUM_DEFAULT), 1)
    full_load = outer <= rows_per_ub
    for variant, cores in (
        ("full-load", min(outer, caps["core_num"])),
        ("split-rows", caps["core_num"]),
    ):
        if variant == "full-load" and not full_load:
            pruned.append(
                {
                    "variant": variant,
                    "reason": f"outer({outer}) > rows_per_ub({rows_per_ub})，整行驻留不可行",
                }
            )
            continue
        tiles_per_core = max(outer // cores, 1)
        cap_kb = round(
            min(rows_per_ub, outer) * row_bytes * BUFFER_NUM_DEFAULT / 1024, 1
        )
        if cap_kb > caps["ub_kb"]:
            pruned.append(
                {
                    "variant": variant,
                    "reason": f"UB 预算超限（{cap_kb}KB > {caps['ub_kb']}KB）",
                }
            )
            continue
        out.append(
            {
                "variant": variant,
                "block_dim": cores,
                "rows_per_ub": min(rows_per_ub, outer),
                "tiles_per_core": tiles_per_core,
                "r_chunk": r,
                "is_full_load": variant == "full-load",
                "capacity_kb": cap_kb,
                "capacity_note": f"UB {cap_kb}KB / {caps['ub_kb']}KB（行块 × 双缓冲）",
                "derived": {
                    "outer": outer,
                    "reduce_dim": r,
                    "rows_per_ub": "ub_size / (行字节 × buffer_num=2)",
                    "cores": "min(outer, 物理核数)（full-load）/ 物理核数（split）",
                },
                "aligned": True,
            }
        )
    return out, pruned


def candidates_broadcast(
    shape: list[int], dtype: str, caps: dict, input_count: int
) -> tuple[list, list]:
    """输出 shape 折叠后按末轴 UB 切分（broadcast 侧 stride=0 不占额外预算）。"""
    total = 1
    for d in shape:
        total *= d
    last = shape[-1] if shape else 1
    elem_bytes = DTYPE_BYTES.get(dtype, 2)
    ub_bytes = caps["ub_kb"] * 1024
    align_elems = max(ALIGN_256B // elem_bytes, 1)
    max_tile = max(
        ub_bytes // (BUFFER_NUM_DEFAULT * max(input_count, 1) * elem_bytes), align_elems
    )
    ub_former = max_tile // align_elems * align_elems

    out, pruned = [], []
    ub_outer = (last + ub_former - 1) // ub_former
    total_tiles = ub_outer
    for d in shape[:-1]:
        total_tiles *= d
    for variant, cores in (
        ("balanced", min(total_tiles, caps["core_num"])),
        ("few-cores", max(min(total_tiles, caps["core_num"]) // 2, 1)),
    ):
        block_former = max(total_tiles // cores, 1)
        cap_kb = round(
            ub_former * elem_bytes * BUFFER_NUM_DEFAULT * max(input_count, 1) / 1024, 1
        )
        if cap_kb > caps["ub_kb"]:
            pruned.append(
                {
                    "variant": variant,
                    "reason": f"UB 预算超限（{cap_kb}KB > {caps['ub_kb']}KB）",
                }
            )
            continue
        out.append(
            {
                "variant": variant,
                "block_dim": cores,
                "ub_split_axis": len(shape) - 1,
                "ub_former": ub_former,
                "ub_outer": ub_outer,
                "block_former": block_former,
                "capacity_kb": cap_kb,
                "capacity_note": f"UB {cap_kb}KB / {caps['ub_kb']}KB（tile × 双缓冲 × {max(input_count, 1)} 输入）",
                "derived": {
                    "total_tiles": total_tiles,
                    "ub_former": f"ub_size/（buffer×inputs×dtype）对齐 {ALIGN_256B}B",
                },
                "aligned": ub_former % align_elems == 0,
            }
        )
    return out, pruned


def candidates_conversion(
    shape: list[int], dtype: str, caps: dict, channel: int
) -> tuple[list, list]:
    """channel-flat 形态：按 (channel, 空间) 折叠，tile 元素对齐。"""
    space = 1
    for d in shape:
        space *= d
    space = space // channel if channel else space
    elem_bytes = DTYPE_BYTES.get(dtype, 2)
    ub_bytes = caps["ub_kb"] * 1024
    align_elems = max(ALIGN_256B // elem_bytes, 1)
    tile = max(
        ub_bytes // (BUFFER_NUM_DEFAULT * 2 * elem_bytes) // align_elems * align_elems,
        align_elems,
    )

    out, pruned = [], []
    tiles = (space + tile - 1) // tile * channel
    for variant, cores in (
        ("full", min(tiles, caps["core_num"])),
        ("half", max(min(tiles, caps["core_num"]) // 2, 1)),
    ):
        per_core = max(tiles // cores, 1)
        cap_kb = round(tile * elem_bytes * BUFFER_NUM_DEFAULT * 2 / 1024, 1)
        if cap_kb > caps["ub_kb"]:
            pruned.append({"variant": variant, "reason": "UB 预算超限"})
            continue
        out.append(
            {
                "variant": variant,
                "block_dim": cores,
                "tile_elements": tile,
                "channel": channel,
                "tiles_per_core": per_core,
                "capacity_kb": cap_kb,
                "capacity_note": f"UB {cap_kb}KB / {caps['ub_kb']}KB（in+out 双缓冲）",
                "derived": {
                    "tiles": tiles,
                    "tile": f"ub_size/(buffer×2×dtype) 对齐 {ALIGN_256B}B",
                },
                "aligned": tile % align_elems == 0,
            }
        )
    return out, pruned


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(
        description="Tiling 候选生成器·四族（容量预算校验 + 剪枝记录）"
    )
    ap.add_argument(
        "--family",
        required=True,
        choices=["elementwise", "reduction", "broadcast", "conversion"],
        help="算子族（首期四族；matmul/mc2 走 Blaze 模板，sort/simt 见族表）",
    )
    ap.add_argument("--shape", required=True, help="逗号分隔维度（如 8448,4096）")
    ap.add_argument("--dtype", required=True, help="fp16/bf16/fp32/int8")
    ap.add_argument(
        "--soc", required=True, help="a2/a3/a5（核数与 UB 容量读 soc-spec）"
    )
    ap.add_argument(
        "--soc-spec",
        default=str(here.parents[1] / "ops-verdict" / "references" / SOC_SPEC_NAME),
    )
    ap.add_argument(
        "--input-count", type=int, default=1, help="输入张量数（UB 预算摊入）"
    )
    ap.add_argument(
        "--axis", type=int, default=-1, help="reduction 族：归约轴（默认末轴）"
    )
    ap.add_argument("--channel", type=int, default=1, help="conversion 族：channel 数")
    ap.add_argument("--output", default=None, help="candidates.json（缺省只打印）")
    args = ap.parse_args()

    shape = [int(x) for x in args.shape.split(",") if x.strip()]
    soc_spec = load_soc_spec(Path(args.soc_spec))
    caps = soc_caps(soc_spec, args.soc)

    if args.family == "elementwise":
        cands, pruned = candidates_elementwise(
            shape, args.dtype, caps, args.input_count
        )
    elif args.family == "reduction":
        cands, pruned = candidates_reduction(shape, args.dtype, caps, args.axis)
    elif args.family == "broadcast":
        cands, pruned = candidates_broadcast(shape, args.dtype, caps, args.input_count)
    else:
        cands, pruned = candidates_conversion(shape, args.dtype, caps, args.channel)

    result = {
        "schema": SCHEMA,
        "soc": args.soc,
        "family": args.family,
        "shape": shape,
        "dtype": args.dtype,
        "input_count": args.input_count,
        "soc_caps": caps,
        "candidates": cands,
        "pruned": pruned,
        "warning": (
            "候选表仅供参数选择起点：实施须单变量铁律（一次一个机制）、"
            "容量运行时复核（GetCoreMemSize 为准，本表为静态估算）、"
            "全量 case 精度验证（semantic_risk=altered 须边界用例）"
        ),
    }
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(f"→ {out}")
    print(f"{args.family} × {args.soc}：{len(cands)} 候选 / {len(pruned)} 剪枝")
    for c in cands:
        print(
            f"  [{c['variant']}] block_dim={c['block_dim']}  {c['capacity_note']}  aligned={c['aligned']}"
        )
    for p in pruned:
        print(f"  ✂ pruned [{p['variant']}]: {p['reason']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
