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
# -*- coding: utf-8 -*-
"""extract_facts.py — tool-perf-analysis 事实抽取器（性能报告解读的取数层）

把两类采集产物统一为同一份 facts JSON（供 render_report.py 与 LLM 解读消费，
并产出 tool-ops-tune ops-verdict verdict.py 可直接消费的 --scalar 串）：

  输入 A（950 / dav-3510）：npu-compute 生成的 .npu-rep 报告
      → 先用 unpack_npu_rep.py 解包 → Summary.jsonl / HardwareInfo.jsonl /
        PipeUtilization.csv 等 7 组 CSV / PipeTrace.json / summary.db(PcSampling)
  输入 B（非 950）：msopprof 生成的 PROF_* 采集目录
      → OpBasicInfo.csv（Task Duration / Block Dim / 频率）+ PipeUtilization.csv 等

设计对齐（tool-ops-tune 判定进脚本原则）：
  - 本脚本只做「取数 + 口径换算 + 标量生产」，不做判定（判定 = tool-ops-tune
    ops-verdict/scripts/verdict.py 规则引擎）；
  - NA 语义：不适用/未采集，一律按缺失处理，绝不按 0 参与统计；
  - ratio 类指标 0~1 / 0~100 双口径归一为百分数（与 verdict.py 同款经验）；
  - 带宽占峰值换算读 tool-ops-tune ops-verdict soc-spec.yaml（不硬编码峰值）。

用法：
  python3 extract_facts.py --rep <report.npu-rep> [--soc-spec <soc-spec.yaml>]
  python3 extract_facts.py --prof <PROF_xxx dir>  [--soc-spec <soc-spec.yaml>]
  可选 --kernel-name <name>（msopprof 多 kernel CSV 防混采过滤）
       --workdir <dir>（解包临时目录，缺省 mkdtemp；已有则复用不重解）

输出（stdout 之外的 -o 文件，schema opstune-perf-analysis.facts/1.0）：
  {
    "schema", "source_tool", "soc", "chip", "op": {...OpInfoSummary...},
    "hardware": {...HardwareInfo...}, "block_stats": {...逐 block 关键统计...},
    "cross_checks": {...交叉验证...}, "pcsampling": {...PcSampling stall 枚举...},
    "trace_summary": {...PipeTrace 采样汇总...}, "scalars": {verdict --scalar 名值},
    "scalar_str": "...", "missing": [...提取失败项与原因...],
    "csv_paths": {...定位到的明细文件...}
  }
"""

from __future__ import annotations

import argparse
import csv
import json
import sqlite3
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

SCHEMA = "opstune-perf-analysis.facts/1.0"

# ---- 列名候选（npu-compute / msopprof / 三代 msprof 列名兼容；逐候选取首个可解析者）----
ICACHE_COLS = [
    "aiv_icache_miss_rate",
    "aic_icache_miss_rate",
    "ai_icache_miss_rate",
    "icache_miss_rate",
    "ICacheMissRate(%)",
]
L2_HIT_COLS = [
    "aiv_total_hit_rate(%)",
    "aic_total_hit_rate(%)",
    "ai_total_hit_rate(%)",
    "total_hit_rate(%)",
    "l2_hit_rate",
]
L2_READ_HIT_COLS = ["aiv_read_hit_rate(%)", "aic_read_hit_rate(%)"]
L2_WRITE_HIT_COLS = ["aiv_write_hit_rate(%)", "aic_write_hit_rate(%)"]
VEC_FP32_COLS = ["aiv_vec_fp32_ratio", "ai_vec_fp32_ratio", "vec_fp32_ratio"]
VEC_CFLT_COLS = [
    "aiv_vec_total_cflt_ratio",
    "ai_vec_total_cflt_ratio",
    "vec_total_cflt_ratio",
]
MTE2_BW_COLS = [
    "aiv_mte2_active_bw(GB/s)",
    "aic_mte2_active_bw(GB/s)",
    "ai_mte2_active_bw(GB/s)",
    "mte2_active_bw(GB/s)",
]
GM_UB_KB_COLS = [
    "GM_to_UB_datas(KB)",
    "gm_to_ub_datas(KB)",
    "read_main_memory_datas(KB)",
]
OPBI_TASK_COLS = ["Task Duration(us)", "Task Duration", "Task Duration(us) "]
OPBI_BLOCKDIM_COLS = ["Block Dim", "BlockDim", "Block DimNum"]
OPBI_FREQ_COLS = ["Current Freq", "Current Freq(MHz)"]
OPBI_RATED_COLS = ["Rated Freq", "Rated Freq(MHz)"]
OPBI_TYPE_COLS = ["Op Type"]
OPBI_NAME_COLS = ["Op Name"]

# PipeTrace 流水名 → pipe 语义（npu-compute 文档口径）
TRACE_PIPE_MAP = {
    "SCALAR": "scalar",
    "VECTOR": "vector",
    "CUBE": "cube",
    "MTE1": "mte1",
    "MTE2": "mte2",
    "MTE3": "mte3",
    "FIXP": "fixpipe",
}


# 芯片型号 → soc 键（tool-ops-tune soc-spec.yaml 的 a2/a3/a5）
def chip_to_soc(chip: str, arch: str) -> str | None:
    chip = (chip or "").upper()
    if "950" in chip or (arch or "").startswith("3510"):
        return "a5"
    if (
        "910_9" in chip.replace(" ", "_")
        or (arch or "").startswith("220")
        and "910_9" in chip
    ):
        return "a3"
    if "910" in chip:
        return "a2"
    if (arch or "").startswith("2201"):
        return "a2"
    return None


def parse_float(v):
    if v is None:
        return None
    s = str(v).strip().replace("%", "")
    if s in ("", "NA", "N/A", "-", "None", "nan"):
        return None
    try:
        return float(s)
    except ValueError:
        return None


def ratio_to_pct(v):
    """ratio 类指标 0~1 / 0~100 双口径归一。

    ratio 真实上界是 1.0（100% = 1.0），>1 的值只能已经是百分数——分界取 1.0：
    用 1.5 会把 (1, 1.5] 区间的百分数真值（如 icache_miss_rate=1.2%）再放大
    100 倍，污染判定。
    """
    if v is None:
        return None
    return v * 100 if 0 <= v <= 1 else v


def load_csv_rows(path: Path) -> list[dict]:
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        return [
            r for r in csv.DictReader(f) if any((v or "").strip() for v in r.values())
        ]


def filter_rows_by_kernel(
    rows: list[dict], kernel_name: str | None
) -> tuple[list, list | None]:
    """多 kernel（Op Name 列多值）时按 kernel 过滤；无法过滤 → 返回 (None, names) 由调用方记 missing。"""
    if not rows or "Op Name" not in rows[0]:
        return rows, None
    names = sorted(
        {str(r.get("Op Name") or "").strip() for r in rows if r.get("Op Name")}
    )
    if len(names) <= 1:
        return rows, None
    if kernel_name and kernel_name in names:
        return [
            r for r in rows if str(r.get("Op Name") or "").strip() == kernel_name
        ], None
    return rows, names  # 混采且未过滤：rows 原样返回，names 报给 missing


def col_first(row: dict, cols: list[str]):
    for c in cols:
        v = parse_float(row.get(c))
        if v is not None:
            return v, c
    return None, None


def mean_of(rows, cols, transform=None, skip_zero_valid=True):
    """对候选列逐行取值（首个可解析者），mean 聚合。NA 跳过；返回 (mean, n, col_used)。"""
    vals, used = [], None
    for r in rows:
        v, c = col_first(r, cols)
        if v is not None:
            used = used or c
            vals.append(transform(v) if transform else v)
    if not vals:
        return None, 0, None
    return sum(vals) / len(vals), len(vals), used


def sum_of(rows, cols):
    vals, used = [], None
    for r in rows:
        v, c = col_first(r, cols)
        if v is not None:
            used = used or c
            vals.append(v)
    if not vals:
        return None, 0, None
    return sum(vals), len(vals), used


# ---------------------------------------------------------------- npu-rep 解包


def unpack_rep(rep_path: Path, workdir: Path) -> Path:
    """解包 .npu-rep 到 workdir（目录已存在则复用）。返回解包产物目录。"""
    out = workdir / rep_path.stem
    if out.exists() and any(out.iterdir()):
        return out
    if out.exists():
        out.rmdir()
    here = Path(__file__).resolve().parent
    script = here / "unpack_npu_rep.py"
    r = subprocess.run(
        [sys.executable, str(script), str(rep_path), str(out)],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        raise RuntimeError(f"npu-rep 解包失败: {r.stderr.strip()}")
    # 嵌套子报告：根 rep 内若只有目录（每算子一个 opN 目录形态），取第一个含文件的子目录
    files = [p for p in out.iterdir() if p.is_file()]
    subdirs = [p for p in out.iterdir() if p.is_dir()]
    if not files and len(subdirs) >= 1:
        return subdirs[0]
    return out


# ---------------------------------------------------------------- summary/hw 读取


def load_opinfo(unpacked: Path) -> tuple[dict | None, Path | None]:
    for name in ("summary.jsonl", "Summary.jsonl"):
        p = unpacked / name
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("category") == "OpInfoSummary":
                return d, p
    return None, None


def load_summary_sections(unpacked: Path) -> dict:
    """summary.jsonl 的非 OpInfoSummary 行（Section Task 级汇总），按 category 键。"""
    out = {}
    for name in ("summary.jsonl", "Summary.jsonl"):
        p = unpacked / name
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("category") and d["category"] != "OpInfoSummary":
                out[d["category"]] = d
        if out:
            break
    return out


def load_hardware(unpacked: Path) -> tuple[dict, Path | None]:
    p = unpacked / "HardwareInfo.jsonl"
    hw = {}
    if p.exists():
        for line in p.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("category"):
                hw[d["category"]] = d
    return hw, (p if hw else None)


# ---------------------------------------------------------------- CSV 定位（两源通用）


def find_csv(root: Path, names: list[str]) -> Path | None:
    for n in names:
        direct = root / n
        if direct.exists():
            return direct
    for n in names:  # msopprof PROF_* 根下平铺；msprof 嵌套形态 rglob 兜底
        for cand in sorted(root.rglob(n)):
            return cand
    return None


# ---------------------------------------------------------------- 逐 block 统计


def block_stats(pipe_csv: Path, kernel_name: str | None) -> tuple[dict, list | None]:
    """PipeUtilization.csv 逐 block 统计（NA 剔除；tail 识别；离散度）。
    返回 (stats, mixed_kernel_names)。"""
    rows = load_csv_rows(pipe_csv)
    rows, mixed = filter_rows_by_kernel(rows, kernel_name)
    stats: dict = {"blocks": len(rows)}
    # 逐核耗时（aiv/aic 双列取大者）
    times = []
    for r in rows:
        aiv = parse_float(r.get("aiv_time(us)"))
        aic = parse_float(r.get("aic_time(us)"))
        cand = [v for v in (aiv, aic) if v is not None]
        if cand:
            times.append(max(cand))
    if times:
        stats["time_us"] = {
            "mean": round(statistics.mean(times), 3),
            "min": round(min(times), 3),
            "max": round(max(times), 3),
            "median": round(statistics.median(times), 3),
            "tail_ratio": round(max(times) / statistics.mean(times), 3)
            if statistics.mean(times)
            else None,
            "imbalance_pct": round((max(times) - min(times)) / max(times) * 100, 2)
            if max(times) > 0
            else None,
        }
    # pipe ratio 统计（aic_/aiv_ 双侧兼容，取存在侧）
    for pipe in ("cube", "vec", "scalar", "mte1", "mte2", "mte3", "fixpipe"):
        for prefix in ("aiv", "aic"):
            col = f"{prefix}_{pipe}_ratio"
            vals = [ratio_to_pct(parse_float(r.get(col))) for r in rows]
            vals = [v for v in vals if v is not None]
            if vals:
                stats[f"{pipe}_ratio_pct"] = {
                    "side": prefix,
                    "mean": round(statistics.mean(vals), 2),
                    "min": round(min(vals), 2),
                    "max": round(max(vals), 2),
                }
                break
    # 主导 pipe 与求和指纹
    means = {k[:-10]: v["mean"] for k, v in stats.items() if k.endswith("_ratio_pct")}
    if means:
        stats["max_pipe"] = max(means, key=means.get)
        stats["pipe_sum_mean_pct"] = round(sum(means.values()), 2)
    # mte2 active bw
    bw, n, col = mean_of(rows, MTE2_BW_COLS)
    if bw is not None:
        stats["mte2_active_bw_gbps"] = {"mean": round(bw, 2), "col": col, "blocks": n}
    # simt 指纹（ArithmeticUtilization 在 npu-compute rep；msopprof 无此文件时跳过）
    return stats, mixed


# ---------------------------------------------------------------- PipeTrace 汇总


def trace_summary(unpacked: Path) -> dict | None:
    p = unpacked / "PipeTrace.json"
    if not p.exists():
        return None
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    ev = d.get("traceEvents") or []
    if not ev:
        return {"events": 0}
    lanes: dict[str, list] = {}
    for e in ev:
        if e.get("ph") == "X":
            lanes.setdefault(str(e.get("pid")), []).append(e)
    per_core = {}
    for pid, es in lanes.items():
        t0 = min(e["ts"] for e in es)
        t1 = max(e["ts"] + e["dur"] for e in es)
        busy_by_pipe = {}
        for e in es:
            busy_by_pipe[e["name"]] = busy_by_pipe.get(e["name"], 0.0) + e["dur"]
        per_core[pid] = {
            "span_us": round(t1 - t0, 2),
            "busy_us_by_pipe": {
                k: round(v, 2) for k, v in sorted(busy_by_pipe.items())
            },
        }
    pipe_total: dict[str, float] = {}
    for es in lanes.values():
        for e in es:
            pipe_total[e["name"]] = pipe_total.get(e["name"], 0.0) + e["dur"]
    n_cores = len(per_core)
    return {
        "events": len(ev),
        "sampled_lanes": n_cores,
        "per_core": per_core,
        "pipe_busy_total_us": {k: round(v, 2) for k, v in sorted(pipe_total.items())},
    }


# ---------------------------------------------------------------- PcSampling（npu-rep 独有）


def pcsampling(unpacked: Path) -> dict | None:
    p = unpacked / "summary.db"
    if not p.exists():
        return None
    try:
        con = sqlite3.connect(f"file:{p}?mode=ro", uri=True)
        try:
            tables = {
                r[0]
                for r in con.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                )
            }
            if "PcSampling" not in tables:
                return None
            cur = con.execute("SELECT * FROM PcSampling")
            cols = [d[0] for d in cur.description]
            rows = cur.fetchall()
        finally:
            con.close()
    except sqlite3.Error:
        return None
    if not rows:
        return None
    # stall 列白名单（schema 语义列），而非「除 address 外全部」——防止
    # 表结构演进引入 core_id/count 等数值列被当 stall 累计，污染 top_stall
    STALL_COLUMNS = {
        "ibuf_empty",
        "nop_cycles",
        "scoreboard_not_ready",
        "register_bank_conflict",
        "resource_conflict",
        "warp_level_sync",
        "divergence_stack_spill",
        "others",
    }
    stall_cols = [c for c in cols if c in STALL_COLUMNS]
    if not stall_cols:
        return None
    agg = {c: 0.0 for c in stall_cols}
    try:
        for row in rows:
            for c, v in zip(cols, row):
                if c in agg:
                    agg[c] += float(v or 0)
    except (TypeError, ValueError):
        # 非数值内容：按该表不可解析降级（其余取数路径同款兜底），不崩溃整链
        return None
    total = sum(agg.values())
    if total <= 0:
        return None
    dist = {
        c: round(v / total * 100, 2)
        for c, v in sorted(agg.items(), key=lambda kv: -kv[1])
    }
    top = max(dist, key=dist.get)
    return {
        "samples_total": int(total),
        "address_rows": len(rows),
        "top_stall": top,
        "top_stall_pct": dist[top],
        "distribution_pct": dist,
    }


# ---------------------------------------------------------------- soc-spec 峰值


def load_soc_spec(path: Path) -> dict:
    try:
        import yaml

        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except Exception:
        return {}


def soc_reference(soc_spec: dict, soc: str) -> dict:
    block = soc_spec.get("socs", {}).get(soc, {})
    ref = block.get("reference", {})
    out = {"soc": soc, "name": block.get("name", soc)}
    for key in ("ai_core_cnt", "vector_core_cnt", "gm_bw_gbps"):
        if key in ref:
            entry = dict(ref[key])
            entry.pop("note", None)
            out[key] = entry
    return out


# ---------------------------------------------------------------- 主流程


def detect_source(arg: Path) -> str:
    if arg.is_dir():
        return "msopprof"
    if arg.suffix == ".npu-rep" or arg.name.endswith(".npu-rep"):
        return "npu-compute"
    raise SystemExit(
        f"ERROR: 无法判定输入形态（需 .npu-rep 文件或 msopprof PROF_* 目录）: {arg}"
    )


def main() -> int:
    here = Path(__file__).resolve().parent
    default_spec = here.parents[0] / "references" / "soc-spec.yaml"
    ap = argparse.ArgumentParser(description="tool-perf-analysis 事实抽取器")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--rep", help="npu-compute 生成的 .npu-rep 报告路径")
    g.add_argument("--prof", help="msopprof 采集目录（PROF_*，含 OpBasicInfo.csv）")
    ap.add_argument("--kernel-name", default=None, help="多 kernel CSV 过滤（防混采）")
    ap.add_argument(
        "--soc",
        default=None,
        choices=["a2", "a3", "a5"],
        help="SoC 覆盖（msopprof 产物无 HardwareInfo 时必填：a2=910B/910_93, a3=910_9x, a5=950）",
    )
    ap.add_argument("--soc-spec", default=str(default_spec))
    ap.add_argument("--workdir", default=None, help="解包临时目录（复用不重解）")
    ap.add_argument("-o", "--output", required=True, help="facts JSON 输出路径")
    args = ap.parse_args()

    missing: list[dict] = []
    src_arg = Path(args.rep) if args.rep else Path(args.prof)
    if not src_arg.exists():
        print(f"ERROR: 输入不存在: {src_arg}", file=sys.stderr)
        return 1
    source = detect_source(src_arg)

    if source == "npu-compute":
        workdir = (
            Path(args.workdir)
            if args.workdir
            else Path(tempfile.mkdtemp(prefix="ops-diagnose-"))
        )
        workdir.mkdir(parents=True, exist_ok=True)
        try:
            unpacked = unpack_rep(src_arg, workdir)
        except RuntimeError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 2
        opinfo, opinfo_path = load_opinfo(unpacked)
        hw, _hw_path = load_hardware(unpacked)
        sections = load_summary_sections(unpacked)
    else:
        unpacked = src_arg
        opinfo, opinfo_path = None, None
        hw = {}
        sections = {}
        # msopprof：OpBasicInfo.csv 承担 OpInfoSummary 角色
        opbi = find_csv(src_arg, ["OpBasicInfo.csv"])
        if opbi:
            rows = load_csv_rows(opbi)
            rows, mixed = filter_rows_by_kernel(rows, args.kernel_name)
            if mixed:
                missing.append(
                    {
                        "metric": "op_info",
                        "reason": f"OpBasicInfo 含多个 kernel（{mixed}）且未指定 --kernel-name",
                    }
                )
            elif rows:
                r = rows[0]
                opinfo = {"category": "OpInfoSummary", "source": "OpBasicInfo.csv"}
                for key, cols in (
                    ("Op Name", OPBI_NAME_COLS),
                    ("Op Type", OPBI_TYPE_COLS),
                    ("Task Duration(us)", OPBI_TASK_COLS),
                    ("Block Dim", OPBI_BLOCKDIM_COLS),
                    ("Current Freq", OPBI_FREQ_COLS),
                    ("Rated Freq", OPBI_RATED_COLS),
                ):
                    # 按候选列名逐个尝试（首个存在且非空者），使列名变体真正生效
                    opinfo[key] = next(
                        (
                            r[c]
                            for c in cols
                            if str(r.get(c) or "").strip() not in ("", "NA")
                        ),
                        "NA",
                    )
                opinfo_path = opbi
        else:
            missing.append({"metric": "op_info", "reason": "OpBasicInfo.csv 缺失"})

    # ---- SoC 识别（--soc 覆盖 > HardwareInfo 芯片识别 > 未知）----
    chip = hw.get("Device Info", {}).get("chip info", "") if hw else ""
    arch = hw.get("Device Info", {}).get("arch info", "") if hw else ""
    soc = args.soc or chip_to_soc(chip, arch)
    soc_spec = (
        load_soc_spec(Path(args.soc_spec)) if Path(args.soc_spec).exists() else {}
    )

    # ---- 定位 CSV（两源通用文件名）----
    pipe_csv = find_csv(unpacked, ["PipeUtilization.csv"])
    l2c_csv = find_csv(unpacked, ["L2Cache.csv"])
    arith_csv = find_csv(unpacked, ["ArithmeticUtilization.csv"])
    rcr_csv = find_csv(unpacked, ["ResourceConflictRatio.csv"])
    mem_csv = find_csv(unpacked, ["Memory.csv"])
    csv_paths = {}
    for name, p in (
        ("PipeUtilization", pipe_csv),
        ("L2Cache", l2c_csv),
        ("ArithmeticUtilization", arith_csv),
        ("ResourceConflictRatio", rcr_csv),
        ("Memory", mem_csv),
    ):
        if p is not None:
            csv_paths[name] = str(p)

    stats = {}
    if pipe_csv:
        stats, mixed = block_stats(Path(pipe_csv), args.kernel_name)
        if mixed:
            missing.append(
                {
                    "metric": "block_stats",
                    "reason": f"PipeUtilization 含多个 kernel（{mixed}）且未指定 --kernel-name，统计为跨 kernel 混合口径",
                }
            )
    else:
        missing.append(
            {
                "metric": "block_stats",
                "reason": "PipeUtilization.csv 缺失（L1/L2 判定不可用）",
            }
        )

    # ---- scalars 生产（对齐 tool-ops-tune facts_bridge.py 的标量契约）----
    scalars: dict[str, float] = {}
    sources: dict[str, str] = {}

    def bridge(csv_path, cols, name, transform=None, agg="mean"):
        if csv_path is None:
            missing.append({"metric": name, "reason": "产物缺失"})
            return
        rows = load_csv_rows(Path(csv_path))
        rows, mixed = filter_rows_by_kernel(rows, args.kernel_name)
        if mixed:
            missing.append(
                {
                    "metric": name,
                    "reason": f"多 kernel（{mixed}）未过滤，不产出混采标量",
                }
            )
            return
        if agg == "sum":
            v, n, col = sum_of(rows, cols)
        else:
            # transform 只在聚合后应用一次（mean_of 不传 transform，防双重换算）
            v, n, col = mean_of(rows, cols)
        if v is None:
            missing.append(
                {"metric": name, "reason": f"列不存在或全 NA（候选 {cols[:3]}…）"}
            )
            return
        if transform:
            v = transform(v)
            if v is None:
                missing.append({"metric": name, "reason": "换算失败（峰值标尺缺失）"})
                return
        scalars[name] = round(v, 2)
        sources[name] = str(csv_path)

    # L0：icache / l2 hit
    bridge(
        pipe_csv,
        ICACHE_COLS,
        "icache_miss_rate",
        transform=lambda v: ratio_to_pct(v) if v <= 1.5 else v,
    )
    bridge(l2c_csv, L2_HIT_COLS, "l2_hit_rate")
    # L1 反证：mte2_active_bw 占峰值（soc-spec 标尺）
    soc_ref = soc_reference(soc_spec, soc) if soc else {}
    gm_bw = soc_ref.get("gm_bw_gbps", {}).get("value") if soc_ref else None
    if source == "npu-compute" and opinfo:
        # npu-compute OpInfoSummary 自带 GM 带宽利用率（aicore_gm_bw_usage_rate(%)）
        usage = parse_float(opinfo.get("aicore_gm_bw_usage_rate(%)"))
        if usage is not None:
            scalars["gm_bw_usage_rate"] = round(usage, 2)
            sources["gm_bw_usage_rate"] = str(opinfo_path)

    def _pct_of_peak(bw):
        return bw / gm_bw * 100 if gm_bw else None

    if gm_bw:
        bridge(
            pipe_csv, MTE2_BW_COLS, "mte2_active_bw_pct_of_peak", transform=_pct_of_peak
        )
    else:
        missing.append(
            {
                "metric": "mte2_active_bw_pct_of_peak",
                "reason": f"soc-spec 无 {soc or '<未知 soc>'} GM 峰值标尺（chip={chip or '未知'}），需 --soc-spec 或 --soc 指定",
            }
        )
    # L1 反证：vec_fp32_ratio / vec_total_cflt_ratio
    bridge(
        arith_csv,
        VEC_FP32_COLS,
        "vec_fp32_ratio",
        transform=lambda v: ratio_to_pct(v) if v is not None and v <= 1.5 else v,
    )
    bridge(
        rcr_csv,
        VEC_CFLT_COLS,
        "vec_total_cflt_ratio",
        transform=lambda v: ratio_to_pct(v) if v is not None and v <= 1.5 else v,
    )
    # L2：task_duration_us / block_dim
    if opinfo:
        td = parse_float(opinfo.get("Task Duration(us)"))
        bd = parse_float(opinfo.get("Block Dim"))
        if td is not None:
            scalars["task_duration_us"] = round(td, 3)
            sources["task_duration_us"] = str(opinfo_path)
        if bd is not None:
            scalars["block_dim"] = bd
            sources["block_dim"] = str(opinfo_path)
    # physical_cores 解析顺序（防多 SKU 设备核利用率错位）：
    #   ① HardwareInfo 实测核数（npu-compute 源，最准确——芯片级真实值）
    #   ② soc-spec skus 按 chip info 型号前缀匹配（msopprof 源）
    #   ③ soc-spec reference 典型口径（最后兜底，来源标注「典型口径」警示）
    # report 同时呈现 HardwareInfo 核数与该标量，两者矛盾时以 HardwareInfo 为准。
    op_type = (opinfo or {}).get("Op Type") or ""
    aic = hw.get("AI Core Information", {}).get("ai cube count")
    aiv = hw.get("AI Core Information", {}).get("ai vector count")
    core_key = "vector_core_cnt" if op_type == "vector" else "ai_core_cnt"
    if aiv is not None or aic is not None:
        if op_type == "vector" and aiv:
            scalars["physical_cores"] = float(aiv)
            sources["physical_cores"] = "HardwareInfo ai vector count（芯片实测）"
        elif aic:
            scalars["physical_cores"] = float(aic)
            sources["physical_cores"] = "HardwareInfo ai cube count（芯片实测）"
    elif soc and soc_spec:
        block_spec = soc_spec.get("socs", {}).get(soc, {})
        sku_hit = None
        chip_prefix = (chip or "").split("_")[0] if chip else ""
        for s in block_spec.get("skus", []):
            # chip info 形如 "Ascend950PR_9599 V100" → 取 "Ascend950PR" 段做前缀匹配
            if chip_prefix and str(s.get("sku", "")).startswith(chip_prefix):
                sku_hit = s
                break
        if sku_hit and sku_hit.get(
            "vector_core_cnt" if op_type == "vector" else "ai_core_cnt"
        ):
            k = "vector_core_cnt" if op_type == "vector" else "ai_core_cnt"
            scalars["physical_cores"] = float(sku_hit[k])
            sources["physical_cores"] = f"soc-spec:{soc} skus[{sku_hit['sku']}] {k}"
        else:
            ref = soc_reference(soc_spec, soc)
            if ref.get(core_key):
                scalars["physical_cores"] = float(ref[core_key]["value"])
                chip_note = f"（chip={chip}）" if chip else ""
            sources["physical_cores"] = (
                f"soc-spec:{soc} reference {core_key}（典型 SKU 口径，"
                f"未命中实际 SKU{chip_note}，核利用率可能有偏差）"
            )
    # PcSampling → top_stall_pct（L0_STALL_DOMINANT 的生产者；npu-rep 独有）
    pcs = pcsampling(unpacked) if source == "npu-compute" else None
    if pcs:
        scalars["top_stall_pct"] = pcs["top_stall_pct"]
        sources["top_stall_pct"] = "summary.db PcSampling"

    # 汇总口径的 Section 行补充（npu-compute summary.jsonl：Task 级聚合，非 block 均值）
    section_summary = {}
    for cat, d in sections.items():
        section_summary[cat] = {
            k: v for k, v in d.items() if parse_float(v) is not None or k == "category"
        }

    out = {
        "schema": SCHEMA,
        "generated_at": __import__("datetime")
        .datetime.now()
        .isoformat(timespec="seconds"),
        "source_tool": source,
        "source_path": str(src_arg),
        "unpacked_dir": str(unpacked),
        "soc": soc,
        "chip": chip or None,
        "arch": arch or None,
        "op": opinfo,
        "hardware": hw,
        "section_summary": section_summary,
        "block_stats": stats,
        "trace_summary": trace_summary(unpacked) if source == "npu-compute" else None,
        "pcsampling": pcs,
        "soc_reference": soc_ref or None,
        "scalars": scalars,
        "scalar_str": ",".join(f"{k}={v}" for k, v in scalars.items()),
        "sources": sources,
        "missing": missing,
        "csv_paths": csv_paths,
    }
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    print(f"[facts] source={source} soc={soc or '?'} chip={chip or '?'}")
    if opinfo:
        print(
            f"[op] {opinfo.get('Op Name', '?')} | type={opinfo.get('Op Type', '?')} | "
            f"duration={opinfo.get('Task Duration(us)', '?')}us | blockDim={opinfo.get('Block Dim', '?')}"
        )
    if stats:
        print(
            f"[blocks] n={stats.get('blocks')} max_pipe={stats.get('max_pipe')} "
            f"pipe_sum={stats.get('pipe_sum_mean_pct')}%"
        )
    if pcs:
        print(f"[pcsampling] top_stall={pcs['top_stall']}={pcs['top_stall_pct']}%")
    print(f"[scalars] {out['scalar_str']}")
    for m in missing:
        print(f"  ⚠ missing: {m['metric']} — {m['reason']}", file=sys.stderr)
    print(f"→ {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
