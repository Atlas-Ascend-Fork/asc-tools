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
"""jsonl_to_cases.py — JSONL 用例 → cases.csv 适配（REVIEW2 G11 / 反馈 P1-5①）

设计对齐：
  - cases.csv 是本仓 A1 全量铁律的输入契约（io-contracts.md §1：case_id/shape/
    dtype/attrs），但生态内存量算子工程用 JSONL 用例（torch-ops-profiler
    规范：每行 {"inputs": [{name/type/dtype/shape|value}...]})——两格式不通，
    存量工程进不了 Phase 0（REVIEW2 G11 定性）。
  - 本脚本做单向适配：JSONL → cases.csv。转换规则全落脚本（判定/转换逻辑
    不进 LLM），歧义处 fail loud 列入 skipped 而非猜测。

转换规则：
  case_id   ：行号（c01 起）或 JSONL 行自带 case_id/name 字段（优先）
  shape     ：首个 type=tensor 输入的 shape（多维 x 分隔）；无 tensor 输入
              → 该行 skipped（无性能可比形态）
  dtype     ：首个 tensor 输入的 dtype（float16→fp16 等短名归一）
  attrs     ：type != tensor 的输入（attr 类）拼 K=V（value 为列表/布尔的
              原样 JSON 序列化），保留原 name

用法：
  python3 jsonl_to_cases.py --jsonl layer_norm_perf_cases.jsonl -o cases.csv
  python3 jsonl_to_cases.py --jsonl x.jsonl -o cases.csv --max-cases 20   # A1 截断同款
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

DTYPE_MAP = {
    "float16": "fp16",
    "float32": "fp32",
    "bfloat16": "bf16",
    "int8": "int8",
    "int32": "int32",
    "int64": "int64",
    "float": "fp32",
    "half": "fp16",
}


def fmt_value(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (list, tuple)):
        return "x".join(str(x) for x in v)
    return str(v)


def convert_row(idx: int, row: dict) -> tuple[dict | None, str | None]:
    """一行 JSONL → 一行 cases.csv（或 skip 原因）。"""
    case_id = row.get("case_id") or row.get("name") or f"c{idx:02d}"
    inputs = row.get("inputs")
    if not isinstance(inputs, list) or not inputs:
        return None, f"{case_id}: 无 inputs 数组"
    tensors = [i for i in inputs if isinstance(i, dict) and i.get("type") == "tensor"]
    if not tensors:
        return None, f"{case_id}: 无 tensor 输入（无可比性能形态）"
    t = tensors[0]
    shape = t.get("shape")
    if not isinstance(shape, (list, tuple)) or not shape:
        return None, f"{case_id}: tensor 输入无 shape"
    dtype = DTYPE_MAP.get(str(t.get("dtype", "")).lower())
    if not dtype:
        return None, f"{case_id}: 未知 dtype {t.get('dtype')!r}"
    attrs = []
    for i in inputs:
        if isinstance(i, dict) and i.get("type") != "tensor" and "name" in i:
            if "value" in i:
                attrs.append(f"{i['name']}={fmt_value(i['value'])}")
    return {
        "case_id": str(case_id),
        "shape": "x".join(str(d) for d in shape),
        "dtype": dtype,
        "attrs": ",".join(attrs),
    }, None


def main() -> int:
    ap = argparse.ArgumentParser(
        description="JSONL 用例 → cases.csv（A1 铁律输入适配）"
    )
    ap.add_argument(
        "--jsonl", required=True, help="JSONL 用例文件（torch-ops-profiler 规范）"
    )
    ap.add_argument("-o", "--output", required=True, help="cases.csv 输出")
    ap.add_argument(
        "--max-cases",
        type=int,
        default=0,
        help="截断上限（0=不截断；>20 取前 20 需告知用户，与 A1 铁律同款）",
    )
    args = ap.parse_args()

    rows_out, skipped = [], []
    with open(args.jsonl, encoding="utf-8") as f:
        for idx, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as e:
                skipped.append(f"line {idx}: JSON 解析失败（{e}）")
                continue
            converted, reason = convert_row(idx, row)
            if converted is None:
                skipped.append(reason)
            else:
                rows_out.append(converted)

    if not rows_out:
        print("ERROR: 无可转换行（全部 skipped，见 stderr）", file=sys.stderr)
        for s in skipped:
            print(f"  ⚠ {s}", file=sys.stderr)
        return 1

    truncated = False
    if args.max_cases and len(rows_out) > args.max_cases:
        rows_out = rows_out[: args.max_cases]
        truncated = True
        print(
            f"WARN: 用例 {len(rows_out)} > {args.max_cases}，取前 {args.max_cases}"
            f"（A1 铁律截断，报告必须告知用户）",
            file=sys.stderr,
        )

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    # csv.writer 统一转义（检视 #16 修复）：case_id/shape/dtype 来自外部 JSONL
    # 属不可信输入，手写拼接在含逗号/引号时产生错位或非法 CSV
    with open(out, "w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["case_id", "shape", "dtype", "attrs"])
        for r in rows_out:
            writer.writerow([r["case_id"], r["shape"], r["dtype"], r["attrs"]])

    print(
        f"cases.csv → {out}（{len(rows_out)} 行"
        f"{'（A1 截断）' if truncated else ''}，skipped {len(skipped)}）"
    )
    for s in skipped[:10]:
        print(f"  ⚠ skipped: {s}", file=sys.stderr)
    if len(skipped) > 10:
        print(f"  ⚠ …共 {len(skipped)} 条 skipped", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
