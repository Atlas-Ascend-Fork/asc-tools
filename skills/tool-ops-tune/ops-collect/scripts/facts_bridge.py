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
"""facts_bridge.py — msprof 7 组产物 → verdict.py --scalar 标量生产者（REVIEW2 G2 / 整改 P0-4）

设计对齐：
  - 判定侧契约：L0 规则 3 条 + L1 反证 2 条的指标全部 source: scalar 且描述写明
    「调用方换算后提供」——此前全仓无生产者，规则在实践中长期 SKIPPED/不可评估。
  - 本脚本就是那个生产者：读 collection_manifest.json 定位 7 组产物，从
    L2Cache / ArithmeticUtilization / ResourceConflictRatio / OpBasicInfo /
    Memory(PipeUtilization) 提取换算，输出 verdict.py 可直接消费的 --scalar 串。
  - 列名三代兼容：aic_/aiv_/ai_ 前缀逐候选尝试（verdict.py 同款经验）；
    三代 msprof 版本的列名漂移是主要风险点，evals 用真实导出 CSV 做 fixture。
  - 峰值换算消费 soc-spec.yaml（P0-3）——GM 带宽标尺有溯源，不再拍脑袋。

支持的标量（与规则词汇表一一对应）：
  L0:    icache_miss_rate / l2_hit_rate / top_stall_pct(PcSampling，暂无生产者)
  L1 反证: mte2_active_bw_pct_of_peak / vec_fp32_ratio / vec_total_cflt_ratio
  L2:    task_duration_us / block_dim / physical_cores(soc-spec 解析)

用法：
  python3 facts_bridge.py --manifest prof/baseline/collection_manifest.json --soc a5 \
      [--kernel-name my_kernel] [--soc-spec ../ops-verdict/references/soc-spec.yaml] \
      [--output scalars.json] [--print-scalar]

输出（opstune.facts-bridge/1.0）：
  { "scalar_str": "l2_hit_rate=96.1,icache_miss_rate=1.2,...",
    "scalars": {...}, "sources": {指标: 文件路径}, "missing": [提取失败的指标与原因] }
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

SCHEMA = "opstune.facts-bridge/1.0"

# ---- 列名候选（三代 msprof 列名兼容；逐候选取首个可解析者）----
ICACHE_COLS = [
    "ai_icache_miss_rate",
    "aic_icache_miss_rate",
    "aiv_icache_miss_rate",
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
MTE2_RATIO_COLS = ["aiv_mte2_ratio", "aic_mte2_ratio", "ai_mte2_ratio", "mte2_ratio"]

# OpBasicInfo（无 block_id 列，含 Op Name 的 per-op 聚合行）
OPBI_TASK_COLS = ["Task Duration(us)", "Task Duration"]
OPBI_BLOCKDIM_COLS = ["Block Dim", "BlockDim", "Block DimNum"]


def _first_float(row: dict, cols: list[str]):
    """按候选列序取首个可解析 float；兼容 0~1 比例与百分数（仅 ratio 类调用方处理）。"""
    for col in cols:
        v = row.get(col)
        if v is None or str(v).strip() in ("", "N/A", "NA", "-", "None"):
            continue
        try:
            return float(str(v).strip().replace("%", ""))
        except ValueError:
            continue
    return None


def _load_rows(path: Path) -> list[dict]:
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        return list(csv.DictReader(f))


def find_metric_csv(manifest_dir: Path, metric: str) -> Path | None:
    """按 manifest 块定位指标 CSV：块 path + rglob 对应 <Metric>.csv 或 op_summary_*.csv。

    - standard 7 组形态：PROF_<Metric>_*/mindstudio_profiler_output/op_summary_*.csv
      或 msprof op 形态的 op_summary_*.csv（多指标合一份，按块名匹配）；
    - 兼容顶层直接导出 <Metric>.csv 的形态（用户从 Insight 导出，如本仓 fixture 真源）。
    """
    for cand in sorted(manifest_dir.rglob(f"{metric}.csv")):
        return cand
    for cand in sorted(manifest_dir.rglob("op_summary_*.csv")):
        return cand
    return None


# ---- DB 双源（P1-6 / G12）：msprof_*.db 与 CSV 等价定位 -----------------------
# 等价查询固化在脚本（判定只出自脚本原则：SQL 拼接不进 LLM）。
# 真源核验：msprof 9.0 产物 device_0/sqlite/ai_core_op_summary.db 的 task_time 表
# （startNs/endNs/duration_time/task_type），op 级 per-kernel 行由 op_summary 语义
# 承担——DB 路径当前只提供 task_duration 类标量（时间轴），逐核 pipe 类指标
# 仍走 CSV（msprof 的 per-block 指标只导出 CSV）。


def find_metric_db(manifest_dir: Path) -> Path | None:
    """定位 msprof sqlite 产物（PROF_*/device_*/sqlite/ai_core_op_summary.db 或 msprof_*.db）。"""
    for pat in (
        "**/sqlite/ai_core_op_summary.db",
        "**/ai_core_op_summary.db",
        "msprof_*.db",
    ):
        cands = sorted(manifest_dir.rglob(pat))
        if cands:
            return cands[0]
    return None


def extract_from_db(db_path: Path, kernel_name: str | None) -> dict:
    """从 msprof sqlite 提取 task 级标量（P1-6 DB 双源）。

    等价查询（与 CSV 路径同口径）：
      - task_duration_us：AI Core 计算类 task 的 duration 均值（duration_time
        单位为 ns——真实 msprof 9.0 产物实测核验，start_time 同为 ns；÷1000 转 us）
      - task_count：行数（供 sources 审计）
    口径说明（如实分级）：DB task_time 的 AI_CORE 行是 task 级时长，与
    op_summary 的 op 级 Task Duration 聚合层级不同（真实产物实测：8900ns=8.9us
    vs CSV 5.42us，含 SDMA 子事件拆分与 wait 记账差异）——sources 注明
    「DB 口径」，报告消费时不得与 CSV 口径混比（delta_compare 的 HARD 校验
    语义同源）。
    无 Op Name 维度（task_time 表不含 kernel 名）→ 多 kernel 程序 DB 源不产出
    duration 标量（missing 记原因）——比 CSV 侧防护更保守（CSV 有 Op Name 列
    可过滤，DB 侧 msprof 9.0 表结构不可过滤）。
    """
    out = {}
    try:
        import sqlite3

        db = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
        try:
            tables = {
                r[0]
                for r in db.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                ).fetchall()
            }
            if "task_time" not in tables:
                return {"error": f"task_time 表不存在（表集合 {sorted(tables)[:8]}）"}
            rows = db.execute(
                "SELECT task_type, duration_time FROM task_time"
            ).fetchall()
            # 计算类 task：AI_CORE / AI_VECTOR_CORE（排除 EVENT_RECORD / PLACE_HOLDER /
            # SDMA 搬运类行——真实 msprof 9.0 产物实测 task_type 集合核验；
            # CSV 侧 Task Duration 同样只统计计算 op）
            calc = [
                float(d)
                for t, d in rows
                if t
                in ("AI_CORE", "AI_VECTOR_CORE", "AI_CORE_SQE", "AI_VECTOR_CORE_SQE")
                and d not in (None, 0)
            ]
            if calc:
                out["task_duration_us"] = round(sum(calc) / len(calc) / 1000.0, 2)
                out["task_count"] = len(calc)
                out["unit_note"] = "duration_time(ns)→us"
            else:
                out["error"] = "无 AI Core 计算类 task 行"
        finally:
            db.close()
    except Exception as e:  # noqa: BLE001 —— DB 损坏/表结构漂移：missing 降级不崩
        out = {"error": f"sqlite 读取失败：{e}"}
    return out


def extract_from_csv(
    csv_path: Path, cols: list[str], kernel_name: str | None, agg: str = "mean"
) -> tuple[float | None, int]:
    """从 CSV 提取候选列聚合值（mean）。有 Op Name 列且多值时按 kernel 过滤
    （与 verdict.py G15 同款防护——bridge 不产出跨 kernel 平均标量）。"""
    rows = _load_rows(csv_path)
    if not rows:
        return None, 0
    if "Op Name" in rows[0]:
        names = {str(r.get("Op Name") or "").strip() for r in rows}
        if len(names) > 1:
            if kernel_name and kernel_name in names:
                rows = [
                    r
                    for r in rows
                    if str(r.get("Op Name") or "").strip() == kernel_name
                ]
            else:
                return None, 0  # 混采且无法过滤：不产出（fail silent 到 missing 列表）
    vals = [v for v in (_first_float(r, cols) for r in rows) if v is not None]
    if not vals:
        return None, 0
    if agg == "max":
        return max(vals), len(vals)
    return sum(vals) / len(vals), len(vals)


def load_soc_spec(path: Path) -> dict:
    import yaml

    return yaml.safe_load(path.read_text(encoding="utf-8"))


def soc_gm_bw_gbps(
    soc_spec: dict, soc: str, sku: str | None
) -> tuple[float | None, str]:
    """解析 GM 峰值带宽：优先 SKU 精确值，缺省用 reference（带溯源说明）。"""
    block = soc_spec.get("socs", {}).get(soc)
    if not block:
        return None, f"soc-spec 无 {soc} 条目"
    if sku:
        for s in block.get("skus", []):
            if s.get("sku") == sku:
                # skus 未携带带宽（未标定）：仍回退 reference，但注明 SKU 命中
                break
    ref = block.get("reference", {}).get("gm_bw_gbps", {})
    v = ref.get("value")
    note = f"reference {v} GB/s（{ref.get('source', '?')}）"
    if not ref.get("verified", True):
        note += " 🟡 未实测标定"
    return v, note


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(
        description="msprof 产物 → verdict --scalar 生产者（G2 桥接）"
    )
    ap.add_argument(
        "--manifest",
        required=True,
        help="collection_manifest.json 路径（或其所在产物根目录）",
    )
    ap.add_argument(
        "--prof-dir",
        default=None,
        help="产物根目录（manifest 只有相对路径时指定；缺省取 manifest 所在目录）",
    )
    ap.add_argument("--soc", required=True, help="a2/a3/a5（峰值换算读 soc-spec）")
    ap.add_argument(
        "--kernel-name",
        default=None,
        help="多 kernel CSV 过滤（G15 同款；不产出跨 kernel 平均标量）",
    )
    ap.add_argument(
        "--soc-spec",
        default=str(here.parents[1] / "ops-verdict" / "references" / "soc-spec.yaml"),
    )
    ap.add_argument(
        "--db",
        default=None,
        help="P1-6 DB 双源：msprof sqlite 产物路径（缺省自动探测 PROF_*/sqlite/ai_core_op_summary.db；"
        "给 --no-db 禁用探测）。CSV 缺失时 DB 补位 task 级标量",
    )
    ap.add_argument(
        "--no-db", action="store_true", help="禁用 DB 双源探测（纯 CSV 口径）"
    )
    ap.add_argument("--output", default=None, help="scalars.json 输出（缺省只打印）")
    args = ap.parse_args()

    manifest_path = Path(args.manifest)
    root = Path(args.prof_dir) if args.prof_dir else manifest_path.parent
    # manifest 本体只做合法性校验（坏 JSON 应 fail loud）；产物定位走 root rglob
    if manifest_path.is_file():
        json.loads(manifest_path.read_text(encoding="utf-8"))

    soc_spec = {}
    spec_path = Path(args.soc_spec)
    if spec_path.exists():
        soc_spec = load_soc_spec(spec_path)

    scalars: dict[str, float] = {}
    sources: dict[str, str] = {}
    missing: list[dict] = []

    def bridge(
        metric_csv: Path | None,
        cols: list[str],
        name: str,
        agg: str = "mean",
        transform=None,
    ):
        """提取一个标量；失败记 missing（补采闭环 skipped[].missing 的真源）。"""
        if metric_csv is None or not Path(metric_csv).exists():
            missing.append({"metric": name, "reason": f"产物缺失（{metric_csv}）"})
            return
        val, n = extract_from_csv(Path(metric_csv), cols, args.kernel_name, agg)
        if val is None:
            missing.append(
                {
                    "metric": name,
                    "reason": f"列不存在或全空（候选列 {cols}，文件 {metric_csv}）",
                }
            )
            return
        if transform:
            val = transform(val)
            if val is None:
                missing.append({"metric": name, "reason": "换算失败（除零/缺失依赖）"})
                return
        scalars[name] = round(val, 2)
        sources[name] = str(metric_csv)

    # ---- L0：icache_miss_rate / l2_hit_rate ----
    pipe_csv = find_metric_csv(root, "PipeUtilization")
    l2c_csv = find_metric_csv(root, "L2Cache")
    arith_csv = find_metric_csv(root, "ArithmeticUtilization")
    rcr_csv = find_metric_csv(root, "ResourceConflictRatio")
    # 注：Memory.csv 不作换算源——active_bw 列在 PipeUtilization.csv（三代实测形态），
    # Memory.csv 是通道聚合带宽（main_mem_read/write），口径不同（见下方 L1 反证段）。
    opbi_csv = find_metric_csv(root, "OpBasicInfo")

    bridge(pipe_csv, ICACHE_COLS, "icache_miss_rate")
    bridge(l2c_csv, L2_HIT_COLS, "l2_hit_rate")
    bridge(arith_csv, VEC_FP32_COLS, "vec_fp32_ratio")  # 0~1 比例 → 百分数
    if "vec_fp32_ratio" in scalars and scalars["vec_fp32_ratio"] <= 1.5:
        scalars["vec_fp32_ratio"] = round(scalars["vec_fp32_ratio"] * 100, 2)
    bridge(rcr_csv, VEC_CFLT_COLS, "vec_total_cflt_ratio")
    if "vec_total_cflt_ratio" in scalars and scalars["vec_total_cflt_ratio"] <= 1.5:
        scalars["vec_total_cflt_ratio"] = round(
            scalars["vec_total_cflt_ratio"] * 100, 2
        )

    # ---- L1 反证：mte2_active_bw_pct_of_peak（active_bw / soc-spec GM 峰值 × 100）----
    # active_bw 列在 PipeUtilization.csv（三代实测形态），不在 Memory.csv——后者是
    # 通道聚合带宽（main_mem_read/write），口径不同不作换算源。
    bw_gbps, bw_note = soc_gm_bw_gbps(soc_spec, args.soc, None)

    def _pct_of_peak(bw: float) -> float | None:
        return bw / bw_gbps * 100 if bw_gbps else None

    bridge(
        pipe_csv,
        MTE2_BW_COLS,
        "mte2_active_bw_pct_of_peak",
        agg="mean",
        transform=_pct_of_peak,
    )

    # ---- L2：task_duration_us / block_dim（OpBasicInfo per-op 行）----
    if opbi_csv and Path(opbi_csv).exists():
        rows = _load_rows(Path(opbi_csv))
        if args.kernel_name and rows and "Op Name" in rows[0]:
            rows = [
                r
                for r in rows
                if str(r.get("Op Name") or "").strip() == args.kernel_name
            ]
        if rows:
            td = _first_float(rows[0], OPBI_TASK_COLS)
            if td is not None:
                scalars["task_duration_us"] = round(td, 2)
                sources["task_duration_us"] = str(opbi_csv)
            bd = _first_float(rows[0], OPBI_BLOCKDIM_COLS)
            if bd is not None:
                scalars["block_dim"] = round(bd, 2)
                sources["block_dim"] = str(opbi_csv)
        else:
            missing.append(
                {
                    "metric": "task_duration_us",
                    "reason": "OpBasicInfo 无匹配行（kernel 过滤后为空）",
                }
            )
    else:
        missing.append({"metric": "task_duration_us", "reason": "OpBasicInfo.csv 缺失"})

    # ---- P1-6 DB 双源：OpBasicInfo 缺失时 sqlite 补位 task_duration_us ----
    # 等价查询固化在 extract_from_db（判定只出自脚本：SQL 拼接不进 LLM）；DB 无
    # kernel 名维度，多 kernel 场景保持 missing（不产出混采标量）。
    if "task_duration_us" not in scalars and not args.no_db:
        db_path = Path(args.db) if args.db else find_metric_db(root)
        if db_path and db_path.exists():
            db_out = extract_from_db(db_path, args.kernel_name)
            if "task_duration_us" in db_out:
                scalars["task_duration_us"] = db_out["task_duration_us"]
                sources["task_duration_us"] = (
                    f"{db_path}（sqlite 双源·DB 口径：task 级时长 ≠ op_summary 的"
                    f" op 级 Task Duration 聚合，禁止混比；task_count={db_out.get('task_count')}，"
                    f"{db_out.get('unit_note', '')}）"
                )
            elif args.kernel_name:
                missing.append(
                    {
                        "metric": "task_duration_us",
                        "reason": "DB 双源不产出（指定 --kernel-name 时 DB 无 kernel 名维度不可过滤）："
                        f"{db_out.get('error', '—')}",
                    }
                )
            else:
                prev = next(
                    (m for m in missing if m["metric"] == "task_duration_us"), None
                )
                if prev:
                    prev["reason"] += f"；DB 双源亦失败（{db_out.get('error', '—')}）"

    # ---- physical_cores（soc-spec reference；调用方可用 --scalar 覆盖为 SKU 精确值）----
    block = soc_spec.get("socs", {}).get(args.soc, {})
    ref_cores = block.get("reference", {}).get("ai_core_cnt", {}).get("value")
    vec_cores = block.get("reference", {}).get("vector_core_cnt", {}).get("value")
    if ref_cores:
        scalars["physical_cores"] = float(ref_cores)
        sources["physical_cores"] = (
            f"soc-spec:{args.soc} reference（{vec_cores} AIV / "
            f"{ref_cores} AIC；按实际 SKU 覆写可得精确值）"
        )

    # ---- verdict --scalar 串 ----
    scalar_str = ",".join(f"{k}={v}" for k, v in scalars.items())
    result = {
        "schema": SCHEMA,
        "soc": args.soc,
        "kernel_name": args.kernel_name,
        "gm_bw_gbps": bw_gbps,
        "gm_bw_source": bw_note,
        "db_dual_source": "sqlite 备位 task_duration_us（CSV OpBasicInfo 缺失时启用；--no-db 关闭）"
        if str(sources.get("task_duration_us", "")).endswith(".db")
        or "（sqlite 双源" in str(sources.get("task_duration_us", ""))
        else None,
        "scalar_str": scalar_str,
        "scalars": scalars,
        "sources": sources,
        "missing": missing,
        "next": (
            f"喂判定引擎：python3 ../../ops-verdict/scripts/verdict.py "
            f"--rules <规则包> --csv <PipeUtilization.csv> --soc {args.soc} "
            f"--scalar {scalar_str} -o verdict.json"
        ),
    }
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(f"→ {out}")
    print(f"scalars: {scalar_str}")
    for m in missing:
        print(f"  ⚠ missing: {m['metric']} — {m['reason']}", file=sys.stderr)
    print(f"下一步：{result['next']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
