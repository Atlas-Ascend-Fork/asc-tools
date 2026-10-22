#!/usr/bin/env python3
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
# timing_harness_torch_event.py — torch_npu 侧 event 计时模板（短 kernel 多 launch 单 event）
#
# 对应：REVIEW2 G4 / 整改 P0-2；timing_harness/index.md 的 torch 形态。
# 与 C++ 模板同一结构：warmup 剔除 → 稳态 repeat ≥3 → 每轮 N 次 launch 用首尾
# event 差值 / N → 取中位；p50/p95/max 一并产出（snapshot.py 分布字段数据源）。
#
# 集成方式（模板拷贝纪律：直接拷贝 + 最小适配，仅 [MODIFY] 一处）：
#   1. 拷入 torch 工程测试目录；
#   2. [MODIFY] run_once() 替换为被测调用（torch.ops.npu.xxx / torch.nn.functional.xxx）；
#   3. python3 timing_harness_torch_event.py --warmup 5 --repeat 20 --output result.json
#
# 产物契约：opstune.timing/1.0（与 C++ 模板同 schema，字段同名同义）。

import argparse
import json
import statistics
from pathlib import Path

SCHEMA = "opstune.timing/1.0"


# ---------------------------------------------------------------------------
# [MODIFY] 被测算子的一次完整调用：替换为真实的 torch 调用。
# 张量分配与初始化放 setup_tensors()（循环外），此处只留调用——
# 否则计时混入 malloc/初始化，口径失效。
# ---------------------------------------------------------------------------
def setup_tensors(device: str):
    """示例形态（按算子实际输入修改）：
    import torch, torch_npu
    x = torch.randn(8448, 4096, dtype=torch.float16, device=device)
    return (x,)
    """
    return ()


def run_once(tensors) -> None:
    """示例形态：
    torch.ops.npu.gelu(x)   # 或 F.gelu(x) 对标原生
    """
    _ = tensors


def main() -> int:
    ap = argparse.ArgumentParser(
        description="torch_npu event 计时（短 kernel 多 launch 单 event）"
    )
    ap.add_argument("--warmup", type=int, default=5)
    ap.add_argument("--repeat", type=int, default=20)
    ap.add_argument(
        "--launch-per-round",
        type=int,
        default=10,
        help="每轮 launch 数（总时长 ≥100us 为宜；短 kernel 摊薄 launch 开销）",
    )
    ap.add_argument("--device", default="npu:0")
    ap.add_argument("--output", required=True, help="结果 JSON 路径（由调用方指定）")
    args = ap.parse_args()
    if args.repeat < 3:
        print("ERROR: repeat >= 3（稳态采样下限，ops-collect 计时口径表）", flush=True)
        return 1

    import torch  # noqa: F401  （模板运行环境：torch + torch_npu）
    import torch_npu  # noqa: F401

    dev = torch.device(args.device)
    tensors = setup_tensors(dev)

    # ---- warmup 轮（剔除冷跑：首次含初始化与缓存预热，实测可差 25%）----
    for _ in range(args.warmup):
        run_once(tensors)
    torch.npu.synchronize()

    # ---- 稳态轮：多 launch 单 event 对（Event 记时在 device 侧，不含 host 抖动）----
    per_round_us: list[float] = []
    ev_start = torch.npu.Event(enable_timing=True)
    ev_end = torch.npu.Event(enable_timing=True)
    for _ in range(args.repeat):
        ev_start.record()
        for _ in range(args.launch_per_round):
            run_once(tensors)
        ev_end.record()
        torch.npu.synchronize()
        per_round_us.append(
            ev_start.elapsed_time(ev_end) * 1000.0 / args.launch_per_round
        )

    p50 = statistics.median(per_round_us)
    srt = sorted(per_round_us)
    p95 = srt[min(int(0.95 * (len(srt) - 1) + 0.5), len(srt) - 1)]
    mx = srt[-1]

    result = {
        "schema": SCHEMA,
        "timing_method": "event",
        "harness": "torch_npu.Event multi-launch single-pair",
        "launch_per_round": args.launch_per_round,
        "warmup": args.warmup,
        "repeat": args.repeat,
        "kernel_avg_us": round(p50, 3),  # 口径：kernel_avg_us = 稳态中位数
        "kernel_p50_us": round(p50, 3),
        "kernel_p95_us": round(p95, 3),
        "kernel_max_us": round(mx, 3),
        "per_round_us": [round(v, 3) for v in per_round_us],
    }
    out = Path(args.output)
    out.write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(
        f"kernel_avg_us(p50)={p50:.3f}  p95={p95:.3f}  max={mx:.3f}"
        f"  (warmup={args.warmup} repeat={args.repeat} × {args.launch_per_round} launch)"
    )
    print(
        f"→ {out}（喂 snapshot.py：--kernel-avg-us {p50:.3f} --timing-method event ...）"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
