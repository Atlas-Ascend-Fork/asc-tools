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
"""trace_bubble.py — cannsim/trace 空泡与重叠度分析（REVIEW2 G7/G8/G13 / 反馈 P1-1②）

设计对齐：
  - L1_SERIAL_NO_OVERLAP 的串行指纹此前是派生推断（pipe 求和≈100%），
    L1_LATENCY_SYNC_SUSPECT 置信度 medium、无直接证据（G13）；
  - pipeline.md 机制卡承诺「DoubleBuffer 生效 = MTE 与计算重叠比例 >30%
    （部分 10-30%，未生效 <5%）」只有文字，无度量工具（G8）；
  - L0_STALL_DOMINANT 的 top_stall_pct 无生产者（G7）。
  - 本脚本从 trace_core*.json（cannsim report 产出的 Chrome Trace Format）直接
    提取：pipe 重叠比例 → overlap_pct、空泡占比 → top_stall_pct，输出 verdict.py
    可直接消费的 --scalar 串——三个缺口一个工具补齐。

trace 格式（cannsim report 实测，与 ops-simulator 文档一致）：
  Chrome Trace Format：traceEvents[]，ph="X" duration 事件（ts/dur，单位 ns），
  metadata 事件（ph="M" name="process_name"）携带 pid → pipeline 名映射。
  cannsim 把 VECTOR 细分为 RVECSU/RVECEX/RVECLD/RVECST/RVECLP、FIXPIPE 写作
  FIXP——归并处理（与 cannbot ops-simulator 同款认知，独立实现）。

用法：
  python3 trace_bubble.py --trace report/trace_core0.json [--scalar-out scalars.json]
  python3 trace_bubble.py --trace-dir report/          # 多核聚合（逐核 + 汇总）
输出标量（verdict --scalar 直接消费）：
  overlap_pct     ：主计算 pipe 与 MTE2 的 busy 交集 / 并集（pipeline.md 阈值
                    >30% 生效 / 10-30% 部分 / <5% 未生效的直接度量）
  top_stall_pct   ：主 pipe 空泡中最大单一类别占比（L0_STALL_DOMINANT 消费；
                    分类粒度：数据等待 / 同步 / 标量开销 / 结构性，先粗后细）
  bubble_pct      ：主 pipe 空泡总占比（LATENCY/SYNC 可疑档的直接证据）
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

SCHEMA = "opstune.trace-bubble/1.0"

# cannsim PID/名称 → 统一 pipe 名（RVECSU 等细分归并 VECTOR；FIXP=FIXPIPE）
PIPE_ALIASES = {
    "SCALAR": "SCALAR",
    "SCALARLDST": "SCALARLDST",
    "VEC": "VECTOR",
    "VECTOR": "VECTOR",
    "RVECSU": "VECTOR",
    "RVECEX": "VECTOR",
    "RVECLD": "VECTOR",
    "RVECST": "VECTOR",
    "RVECLP": "VECTOR",
    "CUBE": "CUBE",
    "MTE1": "MTE1",
    "MTE2": "MTE2",
    "MTE3": "MTE3",
    "FIXP": "FIXPIPE",
    "FIXPIPE": "FIXPIPE",
    "FLOWCTRL": "FLOWCTRL",
    "FLOWCONTROL": "FLOWCTRL",
    "CACHEMISS": "CACHEMISS",
    "ICACHELOAD": "CACHEMISS",
}

COMPUTE_PIPES = ("VECTOR", "CUBE")  # 主计算 pipe（空泡归因对象）


class Event:
    __slots__ = ("ts", "end", "pipe")

    def __init__(self, ts: float, dur: float, pipe: str):
        self.ts = ts
        self.end = ts + dur
        self.pipe = pipe


def load_events(trace_path: Path) -> tuple[list[Event], dict]:
    """读 Chrome Trace：duration 事件 + process_name 映射。返回 (events, pid_map)。"""
    data = json.loads(trace_path.read_text(encoding="utf-8"))
    events_raw = data.get("traceEvents", data if isinstance(data, list) else [])
    pid_map: dict[int, str] = {}
    events: list[Event] = []
    for e in events_raw:
        if not isinstance(e, dict):
            continue
        ph = e.get("ph")
        if ph == "M" and e.get("name") == "process_name":
            args = e.get("args", {}) or {}
            name = str(args.get("name", "")).strip()
            pid = e.get("pid")
            if name and pid is not None:
                pid_map[pid] = name
        elif ph == "X":
            pid = e.get("pid")
            raw_name = str(e.get("name", "")).strip()
            pipe = PIPE_ALIASES.get(pid_map.get(pid, ""), None) or PIPE_ALIASES.get(
                raw_name.split("_")[0].upper(), None
            )
            if pipe is None:
                # args 里找（部分 trace 把 pipe 放 args）
                pipe = PIPE_ALIASES.get(
                    str((e.get("args") or {}).get("pipe", "")).upper()
                )
            if pipe and isinstance(e.get("dur"), (int, float)):
                events.append(Event(float(e["ts"]), float(e["dur"]), pipe))
    events.sort(key=lambda ev: ev.ts)
    return events, pid_map


def busy_intervals(events: list[Event], pipe: str) -> list[tuple[float, float]]:
    """某 pipe 的 busy 区间（合并重叠）。"""
    ivs = sorted((ev.ts, ev.end) for ev in events if ev.pipe == pipe)
    merged: list[tuple[float, float]] = []
    for s, e in ivs:
        if merged and s <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], e))
        else:
            merged.append((s, e))
    return merged


def total_len(ivs: list[tuple[float, float]]) -> float:
    return sum(e - s for s, e in ivs)


def intersect_len(a: list[tuple[float, float]], b: list[tuple[float, float]]) -> float:
    """两区间集交集总长（双指针）。"""
    i = j = 0
    total = 0.0
    while i < len(a) and j < len(b):
        s = max(a[i][0], b[j][0])
        e = min(a[i][1], b[j][1])
        if e > s:
            total += e - s
        if a[i][1] < b[j][1]:
            i += 1
        else:
            j += 1
    return total


def union_len(a: list[tuple[float, float]], b: list[tuple[float, float]]) -> float:
    return total_len(a) + total_len(b) - intersect_len(a, b)


def analyze_core(events: list[Event]) -> dict:
    """单核分析：主计算 pipe 的 overlap / bubble / 空泡分类。"""
    pipes = {ev.pipe for ev in events}
    main_pipe = next((p for p in COMPUTE_PIPES if p in pipes), None)
    if main_pipe is None:
        return {"error": "无 VECTOR/CUBE 计算事件（trace 只含其它 pipe？）"}

    main_ivs = busy_intervals(events, main_pipe)
    span = (events[-1].end - events[0].ts) if events else 0.0
    main_busy = total_len(main_ivs)
    bubble_pct = (span - main_busy) / span * 100 if span > 0 else None

    # overlap：主 pipe vs MTE2（pipeline.md 阈值的直接度量对象）
    mte2_ivs = busy_intervals(events, "MTE2")
    ov_union = union_len(main_ivs, mte2_ivs)
    overlap_pct = (
        intersect_len(main_ivs, mte2_ivs) / ov_union * 100 if ov_union > 0 else None
    )

    # 空泡分类（粗粒度四类，对齐空泡分析体系一级分类）：
    # 主 pipe 空泡窗口内看谁 busy → 归因
    def coverage(ivs, s, e) -> float:
        return intersect_len(ivs, [(s, e)]) / (e - s) if e > s else 0.0

    other_pipes = [
        p
        for p in ("MTE2", "MTE3", "MTE1", "SCALAR", "SCALARLDST", "FIXPIPE")
        if p in pipes
    ]
    other_ivs = {p: busy_intervals(events, p) for p in other_pipes}
    # 主 pipe gap 列表
    gaps = []
    for i in range(len(main_ivs) - 1):
        gaps.append((main_ivs[i][1], main_ivs[i + 1][0]))
    # 首尾也算（kernel 边界空泡）
    if main_ivs:
        gaps.append((events[0].ts, main_ivs[0][0]))
        gaps.append((main_ivs[-1][1], events[-1].end))
    cat_busy = {p: 0.0 for p in ("DATA", "SCALAR", "STRUCT", "IDLE")}
    total_gap = 0.0
    for s, e in gaps:
        if e <= s:
            continue
        w = e - s
        total_gap += w
        best, best_cov = None, 0.0
        for p in other_pipes:
            cov = coverage(other_ivs[p], s, e)
            if cov > best_cov:
                best, best_cov = p, cov
        if best in ("MTE2", "MTE3", "MTE1") and best_cov > 0.3:
            cat_busy["DATA"] += w  # 数据搬运等待
        elif best in ("SCALAR", "SCALARLDST") and best_cov > 0.3:
            cat_busy["SCALAR"] += w  # 标量开销
        elif best is not None and best_cov > 0.3:
            cat_busy["STRUCT"] += w  # 结构性/其它 pipe 活跃
        else:
            cat_busy["IDLE"] += w  # 全 idle：同步/排空类
    top_stall_pct = (
        max((v / total_gap * 100 for v in cat_busy.values()), default=None)
        if total_gap > 0
        else None
    )
    top_stall_cat = max(cat_busy, key=cat_busy.get) if total_gap > 0 else None

    return {
        "main_pipe": main_pipe,
        "span": round(span, 1),
        "main_busy_pct": round(main_busy / span * 100, 2) if span > 0 else None,
        "bubble_pct": round(bubble_pct, 2) if bubble_pct is not None else None,
        "overlap_pct": round(overlap_pct, 2) if overlap_pct is not None else None,
        "bubble_categories_pct": {
            k: round(v / total_gap * 100, 1) for k, v in cat_busy.items()
        }
        if total_gap > 0
        else {},
        "top_stall_pct": round(top_stall_pct, 1) if top_stall_pct is not None else None,
        "top_stall_category": top_stall_cat,
    }


def verdict_scalar(core_results: dict[str, dict]) -> dict:
    """多核汇总 → verdict --scalar（跨核取 max——与木桶口径一致）。"""
    oks = [r for r in core_results.values() if "error" not in r]
    if not oks:
        return {}
    out = {}
    ov = [r["overlap_pct"] for r in oks if r.get("overlap_pct") is not None]
    ts = [r["top_stall_pct"] for r in oks if r.get("top_stall_pct") is not None]
    bb = [r["bubble_pct"] for r in oks if r.get("bubble_pct") is not None]
    if ov:
        out["overlap_pct"] = max(ov)
    if ts:
        out["top_stall_pct"] = max(ts)
    if bb:
        out["bubble_pct"] = max(bb)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description="trace 空泡与重叠度分析（overlap/top_stall/bubble 标量生产者）"
    )
    ap.add_argument("--trace", default=None, help="单核 trace_core*.json")
    ap.add_argument(
        "--trace-dir", default=None, help="report 目录（全部 trace_core*.json 聚合）"
    )
    ap.add_argument(
        "--scalar-out", default=None, help="scalars.json 输出（verdict --scalar 消费）"
    )
    args = ap.parse_args()

    traces: list[Path] = []
    if args.trace:
        traces = [Path(args.trace)]
    elif args.trace_dir:
        traces = sorted(Path(args.trace_dir).glob("trace_core*.json"))
    if not traces:
        print(
            "ERROR: 需要 --trace 或 --trace-dir（未发现 trace_core*.json）",
            file=sys.stderr,
        )
        return 1

    core_results: dict[str, dict] = {}
    for t in traces:
        try:
            events, _ = load_events(t)
            core_results[t.name] = analyze_core(events)
        except (json.JSONDecodeError, OSError) as e:
            core_results[t.name] = {"error": f"读取/解析失败：{e}"}

    scalars = verdict_scalar(core_results)
    result = {
        "schema": SCHEMA,
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "traces": [str(t) for t in traces],
        "per_core": core_results,
        "scalars": scalars,
        "scalar_str": ",".join(f"{k}={v}" for k, v in scalars.items()),
        "interpretation": {
            "overlap_pct": "主计算 pipe × MTE2 busy 交集/并集（pipeline.md：>30% 生效 /"
            " 10-30% 部分 / <5% 未生效）",
            "top_stall_pct": "空泡最大单一类别占比（L0_STALL_DOMINANT 消费；类别"
            " DATA/SCALAR/STRUCT/IDLE）",
            "bubble_pct": "主 pipe 空泡总占比（L1_LATENCY_SYNC_SUSPECT / "
            "L1_SERIAL_NO_OVERLAP 的直接证据）",
        },
        "next": "python3 ../../ops-verdict/scripts/verdict.py --rules <L1/L0 规则包> "
        "--scalar "
        + ",".join(f"{k}={v}" for k, v in scalars.items())
        + " -o verdict.json（trace 证据与 CSV 证据互补）",
    }
    if args.scalar_out:
        out = Path(args.scalar_out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(f"→ {out}")
    print(
        f"trace_bubble: {len(traces)} 核 → scalar: {result['scalar_str'] or '(无——检查 trace pipe 映射)'}"
    )
    for name, r in core_results.items():
        if "error" in r:
            print(f"  ⚠ {name}: {r['error']}", file=sys.stderr)
        else:
            print(
                f"  {name}: main={r['main_pipe']} busy={r['main_busy_pct']}% "
                f"bubble={r['bubble_pct']}% overlap(mte2)={r['overlap_pct']}% "
                f"top_stall={r.get('top_stall_pct')}%({r.get('top_stall_category')})"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
