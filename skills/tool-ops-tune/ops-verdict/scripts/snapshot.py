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
"""snapshot.py — 调优结果指纹快照（不可变基线的载体）

设计对齐（docs/算子调优AGENT设计文档.md §6 数据契约）：
  - 口径指纹思想：性能结论只有在同口径下才可比。
  - HARD_KEYS（环境/口径 12 字段）不一致禁止比较；MECHANISM_KEYS（tiling_key/block_dim）
    是优化本身的变量 → 降级 WARN 并写进报告（对优化本质的建模）。
  - 精度门禁第三道（脚本化）：precision != pass → 本快照可保存，但禁止进入性能结论。

用法：
  python3 snapshot.py --op gelu --variant baseline --mode board --soc a5 \
      --precision pass --kernel-avg-us 14.1 --timing-method event \
      --baseline-kind self_before_after --cann-version 8.3.0 \
      --device-id 0 --shape 8448x4096 --dtype fp16 --format ND \
      --warmup 5 --repeat 20 -o tuning_artifacts/gelu/baseline/result.json
"""

from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
from pathlib import Path

SCHEMA = "opstune.snapshot/1.0"

# 口径指纹：delta_compare.py 硬校验字段（不一致 → 拒绝比较）
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
# 机制变量：分核/tiling 类优化本身会改 → 降级 WARN
MECHANISM_KEYS = ("tiling_key", "block_dim")

TIMING_METHODS = ("event", "sim", "msprof", "msprof_task_duration", "profiler_summary")


def env_fingerprint() -> dict:
    """尽力采集环境指纹；失败字段置 unavailable，不阻断（离线诊断也允许建快照）。"""
    fp = {}
    for key, cmd in {
        "npu_smi": ["npu-smi", "info"],
        "cann_version_info": [
            "bash",
            "-c",
            "cat $ASCEND_HOME_PATH/version.info 2>/dev/null | head -3",
        ],
    }.items():
        try:
            fp[key] = (
                subprocess.run(
                    cmd, capture_output=True, text=True, timeout=10
                ).stdout.strip()[:500]
                or "unavailable"
            )
        except Exception:
            fp[key] = "unavailable"
    return fp


def main() -> int:
    ap = argparse.ArgumentParser(description="调优结果指纹快照")
    ap.add_argument("--op", required=True)
    ap.add_argument(
        "--variant", required=True, help="baseline / round-01-<mechanism> / optimized"
    )
    ap.add_argument("--mode", default="board", choices=["board", "sim"])
    ap.add_argument("--soc", required=True, help="a2/a3/a5")
    ap.add_argument(
        "--precision",
        required=True,
        choices=["pass", "fail"],
        help="精度门禁结果（工程现有测试口径，禁止自创容差）",
    )
    ap.add_argument("--kernel-avg-us", type=float, default=None)
    ap.add_argument(
        "--kernel-p50-us",
        type=float,
        default=None,
        help="稳态 p50（µs，可选分布字段；供 delta_compare 抖动诊断）",
    )
    ap.add_argument(
        "--kernel-p95-us", type=float, default=None, help="稳态 p95（µs，可选分布字段）"
    )
    ap.add_argument(
        "--kernel-max-us", type=float, default=None, help="稳态 max（µs，可选分布字段）"
    )
    ap.add_argument(
        "--timing-method",
        required=True,
        choices=TIMING_METHODS,
        help="event=host aclrtEvent / sim=cannsim / msprof / msprof_task_duration"
        "=长kernel无event设施时 / profiler_summary",
    )
    ap.add_argument(
        "--baseline-kind", required=True, choices=["system", "self_before_after"]
    )
    ap.add_argument("--cann-version", default="unknown")
    ap.add_argument("--repo-commit", default=None)
    ap.add_argument("--device-id", default=None)
    ap.add_argument("--shape", required=True)
    ap.add_argument("--dtype", required=True)
    ap.add_argument("--format", default="ND")
    ap.add_argument("--tiling-key", default=None)
    ap.add_argument("--block-dim", type=int, default=None)
    ap.add_argument("--warmup", type=int, required=True)
    ap.add_argument("--repeat", type=int, required=True)
    ap.add_argument(
        "--status",
        default="ok",
        choices=["ok", "partial", "dry_run"],
        help="dry_run=未上板仅离线结论；partial=部分 case/指标未采集",
    )
    ap.add_argument(
        "--mismatch", type=int, default=None, help="精度 mismatch 数（误差指纹）"
    )
    ap.add_argument("--max-abs-err", type=float, default=None)
    ap.add_argument("--max-rel-err", type=float, default=None)
    ap.add_argument("--verdict-file", default=None, help="关联 verdict.json 路径")
    ap.add_argument("--extra", default="", help="任意扩展键值 k=v,k=v")
    ap.add_argument(
        "--no-env-probe", action="store_true", help="跳过环境探测（离线/CI）"
    )
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    extra = {}
    if args.extra:
        for kv in args.extra.split(","):
            if "=" in kv:
                k, v = kv.split("=", 1)
                extra[k] = v

    # 口径指纹（HARD）：字段名与 argparse 目标名同名，从契约元组直接构建——
    # 单一真源（新增 HARD 字段只改 HARD_KEYS 元组，不在此重复手写字段名）
    hard_fields = {key: getattr(args, key) for key in HARD_KEYS}
    snap = {
        "schema": SCHEMA,
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        # 口径指纹（HARD）+ 溯源 commit（非 HARD：仅环境留痕）
        **hard_fields,
        "repo_commit": args.repo_commit,
        # 机制变量（WARN 级）
        "tiling_key": args.tiling_key,
        "block_dim": args.block_dim,
        # 性能与精度
        "variant": args.variant,
        "precision": args.precision,
        "kernel_avg_us": args.kernel_avg_us,
        # 可选分布字段（F20：±5% 噪声带判定的统计基础；MC² 抖动双维纪律的落盘载体）
        "kernel_p50_us": args.kernel_p50_us,
        "kernel_p95_us": args.kernel_p95_us,
        "kernel_max_us": args.kernel_max_us,
        "mismatch": args.mismatch,
        "max_abs_err": args.max_abs_err,
        "max_rel_err": args.max_rel_err,
        "status": args.status,
        "verdict_file": args.verdict_file,
        "env": {} if args.no_env_probe else env_fingerprint(),
        "extra": extra,
    }

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(snap, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"snapshot → {out}")
    if args.precision != "pass":
        print(
            "⚠ 精度门禁未通过：本快照禁止用于性能结论（delta_compare 将拒绝生成对比）",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
