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
  - **npu-compute DB 通道（Roadmap #4）**：npu-compute（950/A5）解包目录自带
    summary.db（7 Section 表 + PcSampling + FieldUnitMap），DB 是 CSV 的同数据
    入库副本（与 msprof DB 双源的「task 级口径差异」不同，可安全互通）——
    逐指标 DB SQL 优先（AVG+NULL-skip 下推），CSV 补位；PcSampling 是 DB 独有
    数据源，本脚本因此成为 top_stall_pct（L0_STALL_DOMINANT）的首个生产者。

支持的标量（与规则词汇表一一对应）：
  L0:    icache_miss_rate / l2_hit_rate / top_stall_pct(PcSampling——npu-compute DB 通道生产)
  L1 反证: mte2_active_bw_pct_of_peak / vec_fp32_ratio / vec_total_cflt_ratio
  L2:    task_duration_us / block_dim / physical_cores(soc-spec 解析)

用法：
  # msprof 产物（存量路径不变）：
  python3 facts_bridge.py --manifest prof/baseline/collection_manifest.json --soc a5 \
      [--kernel-name my_kernel] [--soc-spec ../ops-verdict/references/soc-spec.yaml] \
      [--output scalars.json] [--print-scalar]
  # npu-compute 解包目录（DB 通道；Summary.jsonl 提供 OpInfoSummary）：
  python3 facts_bridge.py --manifest <unpacked_dir> --source npu-compute --soc a5 \
      [--db <summary.db>] [--output scalars.json]
  # 显式通道：--source msprof|npu-compute|auto（auto 按目录特征判源）；
  # --no-db 禁用一切 DB 读取（纯 CSV 口径）。

输出（opstune.facts-bridge/1.1）：
  { "source_tool": "msprof|npu-compute",
    "scalar_str": "l2_hit_rate=96.1,icache_miss_rate=1.2,...",
    "scalars": {...}, "sources": {指标: 文件路径+通道注记},
    "pcsampling": {...top_stall 分布...} | null,
    "missing": [提取失败的指标与原因] }
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

SCHEMA = "opstune.facts-bridge/1.1"

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
# npu-compute L2Cache 列形态（真实 .npu-rep 实测）：只有 read/write 分项 hit_rate，
# 无 total——按读命中率口径补位（npu-compute 官方 csv_reader 列集核验），
# sources 注记口径防混比
L2_HIT_COLS_NPUCOMPUTE = [
    "aiv_read_hit_rate(%)",
    "aic_read_hit_rate(%)",
]
VEC_FP32_COLS = ["aiv_vec_fp32_ratio", "ai_vec_fp32_ratio", "vec_fp32_ratio"]
VEC_CFLT_COLS = [
    "aiv_vec_total_cflt_ratio",
    "ai_vec_total_cflt_ratio",
    "vec_total_cflt_ratio",
]
# npu-compute ResourceConflictRatio 无 total 列——stu/ldu/sfu 三个分项和
# 作为 total 口径补位（与 CSV/DB 双通道同算法，保等价；sources 注记分量和口径）
VEC_CFLT_PART_COLS = [
    "aiv_vec_stu_cflt_ratio",
    "aiv_vec_ldu_cflt_ratio",
    "aiv_vec_sfu_cflt_ratio",
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
    - 兼容顶层直接导出 <Metric>.csv 的形态（用户从 Insight 导出，如本仓 fixture 真源；
      npu-compute .npu-rep 解包目录同为此形态——Section CSV 顶层平铺）。
    """
    for cand in sorted(manifest_dir.rglob(f"{metric}.csv")):
        return cand
    for cand in sorted(manifest_dir.rglob("op_summary_*.csv")):
        return cand
    return None


# ---- npu-compute DB 通道（Roadmap #4）----------------------------------------
# summary.db 结构（npu_tools/npu_compute/src/cli/report/summary_sqlite_finalizer.cpp
# 真源核验）：7 个 Section 表（与同名 CSV 同数据，NA→NULL）+ PcSampling（DB 独有）
# + FieldUnitMap（field_name→unit）。DB 与 CSV 可安全互通（同一次采集的双格式，
# 与下方 msprof DB 双源的「task 级 vs op 级口径差异」不同）。
# 等价查询固化在脚本（判定只出自脚本原则：SQL 拼接不进 LLM）。

NPUCOMPUTE_DB = "summary.db"
# PcSampling stall 状态白名单（DDL 语义列，npu-compute pm_sampling_file_validator
# 核验）——防表结构演进引入非 stall 数值列被当 stall 累计，污染 top_stall
STALL_COLUMNS = (
    "ibuf_empty",
    "nop_cycles",
    "scoreboard_not_ready",
    "register_bank_conflict",
    "resource_conflict",
    "warp_level_sync",
    "divergence_stack_spill",
    "others",
)


def find_npucompute_db(root: Path) -> Path | None:
    """定位 npu-compute 解包目录的 summary.db（顶层或 collection-* 子目录）。

    npu-compute 大小写两代产物形态都接受：summary.db / Summary.db，
    Summary.jsonl / summary.jsonl（真实 .npu-rep 两种形态实测）。
    """
    for name in ("summary.db", "Summary.db"):
        top = root / name
        if top.exists():
            return top
        for pat in (f"*/{name}", f"**/{name}"):
            cands = sorted(root.rglob(pat))
            if cands:
                return cands[0]
    return None


def detect_source(root: Path) -> str:
    """通道自动判定：目录特征 → npu-compute / msprof。

    npu-compute 解包特征：Summary.jsonl/summary.jsonl（OpInfoSummary 承担
    OpBasicInfo 角色）或 summary.db；msprof 特征：PROF_*/ 结构或 OpBasicInfo.csv。
    """
    for name in ("Summary.jsonl", "summary.jsonl"):
        if (root / name).exists():
            return "npu-compute"
    if list(root.rglob("Summary.jsonl")) or list(root.rglob("summary.jsonl")):
        return "npu-compute"
    if find_npucompute_db(root):
        return "npu-compute"
    return "msprof"


def open_ro_db(db_path: Path):
    import sqlite3

    return sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)


def _table_columns(db, table: str) -> set[str]:
    """实际列集合（PRAGMA 校验）——SQLite 对双引号未知标识符按字符串字面量
    处理（AVG('aiv_total_hit_rate') 合法且返回 0.0，不报错！），必须先验列
    存在再 AVG，否则「列不存在」被静默吞成 0.0 标量（真值污染）。"""
    return {d[1] for d in db.execute(f'PRAGMA table_info("{table}")')}


def db_column_mean(
    db_path: Path, table: str, cols: list[str]
) -> tuple[float | None, str | None]:
    """DB 通道标量：候选列 COALESCE 逐行回退后 AVG（NA→NULL 语义内建）。

    与 CSV 侧 extract_from_csv 的逐行 _first_float **同语义**（检视 #3 修复：
    此前「候选列逐个 AVG 取首个有值列」是按列选择——同一 Section 表内
    aiv_*/aic_* 部分行有值部分行 NULL 的混排形态（AIC/AIV block 混排报告
    常态）下，DB 侧只统计首个有值列自身的行子集，CSV 侧逐行合并两列，
    双通道标量分叉、sources 注记却都显示「合法等价」）。COALESCE 保持
    候选序逐行回退，聚合语义与 CSV 侧一致。
    返回 (值, 命中列集描述)。表或列不存在返回 (None, None)——调用方按
    missing 降级。
    """
    try:
        db = open_ro_db(db_path)
        try:
            tables = {
                r[0]
                for r in db.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                ).fetchall()
            }
            if table not in tables:
                return None, None
            actual = _table_columns(db, table)
            present = [c for c in cols if c in actual]
            if not present:
                return None, None
            if len(present) == 1:
                col = present[0]
                v = db.execute(
                    f'SELECT AVG("{col}") FROM "{table}" WHERE "{col}" IS NOT NULL'
                ).fetchone()[0]
                hit = col
            else:
                coalesce = ", ".join(f'"{c}"' for c in present)
                v = db.execute(
                    f"SELECT AVG(COALESCE({coalesce})) FROM"
                    f' "{table}" WHERE COALESCE({coalesce}) IS NOT NULL'
                ).fetchone()[0]
                hit = "+".join(present)
            if v is not None:
                return float(v), hit
            return None, None
        finally:
            db.close()
    except Exception:  # noqa: BLE001 —— DB 损坏/不可读：降级 CSV，missing 记因
        return None, None


def db_column_mean_sum(
    db_path: Path, table: str, col_sets: list[list[str]]
) -> tuple[float | None, list[str]]:
    """DB 通道分量和口径：候选组内各列均值先取（同组三列须同表同在）再求和。"""
    try:
        db = open_ro_db(db_path)
        try:
            tables = {
                r[0]
                for r in db.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                ).fetchall()
            }
            if table not in tables:
                return None, []
            actual = _table_columns(db, table)
            for cols in col_sets:
                if not all(c in actual for c in cols):
                    continue
                vals = []
                for col in cols:
                    v = db.execute(
                        f'SELECT AVG("{col}") FROM "{table}" WHERE "{col}" IS NOT NULL'
                    ).fetchone()[0]
                    if v is None:
                        vals = []
                        break
                    vals.append(float(v))
                if vals:
                    return sum(vals), cols
            return None, []
        finally:
            db.close()
    except Exception:  # noqa: BLE001
        return None, []


def pcsampling_stall(db_path: Path) -> dict | None:
    """PcSampling 八状态 stall 分布 → top_stall_pct（L0_STALL_DOMINANT 生产者）。

    - address 十六进制 ↔ symbolizer.json 定位留给消费方（本层只出分布）；
    - 白名单列累计，防表结构演进污染（tool-perf-analysis extract_facts 同款纪律）；
    - 无 PcSampling 表（未选 PmSampling Section / SIMD 算子无采样）返回 None——
      如实 missing，不静默造 0。
    """
    try:
        db = open_ro_db(db_path)
        try:
            tables = {
                r[0]
                for r in db.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'"
                ).fetchall()
            }
            if "PcSampling" not in tables:
                return None
            cur = db.execute("SELECT * FROM PcSampling")
            cols = [d[0] for d in cur.description]
            rows = cur.fetchall()
        finally:
            db.close()
    except Exception:  # noqa: BLE001 —— DB 损坏：missing 降级不崩
        return None
    if not rows:
        return None
    agg = {c: 0.0 for c in STALL_COLUMNS if c in cols}
    if not agg:
        return None
    try:
        for row in rows:
            for c, v in zip(cols, row):
                if c in agg:
                    agg[c] += float(v or 0)
    except (TypeError, ValueError):
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


def load_opinfo_summary(root: Path) -> dict | None:
    """npu-compute Summary.jsonl 的 OpInfoSummary 行（OpBasicInfo 等价角色）。

    大小写两代形态都接受（Summary.jsonl / summary.jsonl，真实 .npu-rep 实测）。
    """
    for cand in (root / "Summary.jsonl", root / "summary.jsonl"):
        if not cand.exists():
            continue
        try:
            for line in cand.read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                d = json.loads(line)
                if d.get("category") == "OpInfoSummary":
                    return d
        except (OSError, json.JSONDecodeError):
            continue
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


def extract_cflt_parts_from_csv(
    csv_path: Path, col_sets: list[list[str]], kernel_name: str | None
) -> float | None:
    """CSV 侧分量和口径：候选组内各列 mean 求和（与 DB 侧 db_column_mean_sum
    同算法——先各列 mean 再求和，保双通道等价）。"""
    rows = _load_rows(csv_path)
    if not rows:
        return None
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
                return None
    for cols in col_sets:
        if not all(c in rows[0] for c in cols):
            continue
        vals = []
        for col in cols:
            per_row = [
                v for v in (_first_float(r, [col]) for r in rows) if v is not None
            ]
            if not per_row:
                vals = []
                break
            vals.append(sum(per_row) / len(per_row))
        if vals:
            return sum(vals)
    return None


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
        description="采集产物 → verdict --scalar 生产者（G2 桥接；msprof 7 组 / npu-compute DB 通道）"
    )
    ap.add_argument(
        "--manifest",
        required=True,
        help="collection_manifest.json 路径（或产物根目录——npu-compute 传解包目录）",
    )
    ap.add_argument(
        "--prof-dir",
        "--prof",  # 入口文档（SKILL.md）使用的别名
        dest="prof_dir",
        default=None,
        help="产物根目录（manifest 只有相对路径时指定；缺省取 manifest 所在目录）",
    )
    ap.add_argument(
        "--source",
        default="auto",
        choices=["auto", "msprof", "npu-compute"],
        help="产物来源通道：auto 按目录特征判源（Summary.jsonl/summary.db → npu-compute）",
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
        help="sqlite 产物路径：npu-compute 通道缺省探测 <dir>/summary.db；"
        "msprof 通道缺省探测 PROF_*/sqlite/ai_core_op_summary.db（P1-6 DB 双源）",
    )
    ap.add_argument(
        "--no-db", action="store_true", help="禁用一切 DB 读取（纯 CSV 口径）"
    )
    ap.add_argument("--output", default=None, help="scalars.json 输出（缺省只打印）")
    ap.add_argument(
        "--print-scalar",
        action="store_true",
        help="stdout 只打印 verdict.py 可直接消费的 --scalar 串（供管道拼接）",
    )
    args = ap.parse_args()

    manifest_path = Path(args.manifest)
    # 入参二义性按 help 文档口径消解（检视 #12/High 修复）：--manifest 可传
    # manifest 文件或产物根目录——传目录时 root 取该目录本身（取 .parent 会在
    # prof/{baseline,after} 布局下 rglob 命中字母序靠前的另一轮采集，
    # 把「优化后」CSV 当「基线」标量喂给 verdict，结论方向反转）；
    # 目录不存在时 fail loud，不静默上移搜索范围。
    if args.prof_dir:
        root = Path(args.prof_dir)
    elif manifest_path.is_dir():
        root = manifest_path
    elif manifest_path.is_file():
        root = manifest_path.parent
    else:
        # 路径不存在（拼错）：fail loud——不静默上移到 parent 的 rglob
        print(
            f"ERROR: --manifest 路径不存在（文件或目录）: {manifest_path}",
            file=sys.stderr,
        )
        return 1
    if not root.is_dir():
        print(f"ERROR: 产物根目录不存在: {root}", file=sys.stderr)
        return 1
    # manifest 本体只做合法性校验（坏 JSON 应 fail loud）；产物定位走 root rglob
    if manifest_path.is_file():
        json.loads(manifest_path.read_text(encoding="utf-8"))

    # ---- 通道判定（auto 按目录特征；显式指定按用户口径）----
    source_tool = args.source
    if source_tool == "auto":
        source_tool = detect_source(root)
    npu_db_path: Path | None = None
    if source_tool == "npu-compute" and not args.no_db:
        npu_db_path = (
            Path(args.db)
            if args.db and Path(args.db).exists()
            else find_npucompute_db(root)
        )
        if npu_db_path is None:
            # npu-compute 目录无 DB（旧版 .npu-rep / --no-pack 无 sqlite 环境）：
            # 如实降级 CSV 通道并注记（DB 增益见 SKILL.md csv_to_db 升级路径）
            print(
                "[warn] npu-compute 目录未见 summary.db——DB 通道降级 CSV（旧版报告可用"
                " csv_to_db.py 升级为 DB 格式）",
                file=sys.stderr,
            )

    soc_spec = {}
    spec_path = Path(args.soc_spec)
    if spec_path.exists():
        soc_spec = load_soc_spec(spec_path)

    scalars: dict[str, float] = {}
    sources: dict[str, str] = {}
    missing: list[dict] = []

    def bridge_db_first(
        table: str,
        db_cols: list[str],
        csv_path: Path | None,
        csv_cols: list[str],
        name: str,
        transform=None,
        note_db: str | None = None,
        note_csv: str | None = None,
    ):
        """npu-compute 通道标量：DB SQL 优先（AVG+NULL-skip 下推），CSV 补位。

        DB 是 CSV 的同数据入库副本（单次采集双格式），数值口径等价——
        sources 逐指标注记实际通道，报告消费可溯源。
        """
        if npu_db_path is not None:
            v, col = db_column_mean(npu_db_path, table, db_cols)
            if v is not None:
                if transform:
                    v = transform(v)
                if v is not None:
                    scalars[name] = round(v, 2)
                    sources[name] = f"{npu_db_path}::{table}.{col}{note_db or ''}"
                    return
        # CSV 补位（DB 无该表/该列/被 --no-db 禁用）
        if csv_path is not None and Path(csv_path).exists():
            val, _n = extract_from_csv(Path(csv_path), csv_cols, args.kernel_name)
            if val is not None:
                if transform:
                    val = transform(val)
                if val is not None:
                    scalars[name] = round(val, 2)
                    sources[name] = f"{csv_path}{note_csv or ''}"
                    return
        missing.append(
            {
                "metric": name,
                "reason": f"列不存在或全空（DB 候选 {db_cols} / CSV 候选 {csv_cols[:3]}…，"
                f"表 {table}，CSV {csv_path}）",
            }
        )

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

    if source_tool == "npu-compute":
        # ---- npu-compute 通道（Roadmap #4）：DB SQL 优先，CSV 补位 ----
        # DB 列名为规范化形（单位后缀已拆入 FieldUnitMap）；候选序与 CSV 同语义
        # （db_column_mean 用 COALESCE 逐行回退，与 CSV 侧 _first_float 逐行
        #   语义对齐——检视 #3）。
        # npu-compute L2Cache 无 total hit_rate 列——read 口径补位（列集真源核验）。
        bridge_db_first(
            "PipeUtilization",
            ICACHE_COLS,
            pipe_csv,
            ICACHE_COLS,
            "icache_miss_rate",
        )
        bridge_db_first(
            "L2Cache",
            [c.replace("(%)", "") for c in L2_HIT_COLS + L2_HIT_COLS_NPUCOMPUTE],
            l2c_csv,
            L2_HIT_COLS + L2_HIT_COLS_NPUCOMPUTE,
            "l2_hit_rate",
            note_csv="（npu-compute 无 total 列时 read_hit_rate 口径补位）",
        )
        bridge_db_first(
            "ArithmeticUtilization",
            VEC_FP32_COLS,
            arith_csv,
            VEC_FP32_COLS,
            "vec_fp32_ratio",
        )
        if "vec_fp32_ratio" in scalars and scalars["vec_fp32_ratio"] <= 1.5:
            scalars["vec_fp32_ratio"] = round(scalars["vec_fp32_ratio"] * 100, 2)
        # vec_total_cflt_ratio：total 列缺失时 stu+ldu+sfu 分量均值和口径
        # （npu-compute ResourceConflictRatio 列集真源核验——无 total 列；
        #   分量和 = total 的成分展开，语义同源，sources 注记口径）
        if npu_db_path is not None:
            v, cols_hit = db_column_mean(
                npu_db_path, "ResourceConflictRatio", VEC_CFLT_COLS
            )
            if v is None:
                v, cols_hit = db_column_mean_sum(
                    npu_db_path, "ResourceConflictRatio", [VEC_CFLT_PART_COLS]
                )
                if v is not None:
                    v = v  # 分量和口径
                    note_cflt = (
                        "（stu+ldu+sfu 分量均值和口径——npu-compute 无 total 列）"
                    )
                else:
                    note_cflt = None
            else:
                note_cflt = None
            if v is not None:
                if v <= 1.5:
                    v = v * 100
                scalars["vec_total_cflt_ratio"] = round(v, 2)
                # cols_hit：total 命中为 str（COALESCE 命中列集），分量和命中为 list
                suffix = f".{cols_hit[-1]}" if cols_hit else ""
                sources["vec_total_cflt_ratio"] = (
                    f"{npu_db_path}::ResourceConflictRatio{suffix}{note_cflt or ''}"
                )
            else:
                missing.append(
                    {
                        "metric": "vec_total_cflt_ratio",
                        "reason": "ResourceConflictRatio 无 total 及分项冲突列"
                        f"（候选 {VEC_CFLT_COLS} / {VEC_CFLT_PART_COLS}）",
                    }
                )
        else:
            # CSV 补位通道：total 列直接候选；缺失时分量和口径（与 DB 侧同算法）
            if rcr_csv is not None and Path(rcr_csv).exists():
                v, _n = extract_from_csv(Path(rcr_csv), VEC_CFLT_COLS, args.kernel_name)
                note_cflt = None
                if v is None:
                    v = extract_cflt_parts_from_csv(
                        Path(rcr_csv), [VEC_CFLT_PART_COLS], args.kernel_name
                    )
                    if v is not None:
                        note_cflt = (
                            "（stu+ldu+sfu 分量均值和口径——npu-compute 无 total 列）"
                        )
                if v is not None:
                    if v <= 1.5:
                        v = v * 100
                    scalars["vec_total_cflt_ratio"] = round(v, 2)
                    sources["vec_total_cflt_ratio"] = f"{rcr_csv}{note_cflt or ''}"
                else:
                    missing.append(
                        {
                            "metric": "vec_total_cflt_ratio",
                            "reason": "ResourceConflictRatio 无 total 及分项冲突列"
                            f"（候选 {VEC_CFLT_COLS} / {VEC_CFLT_PART_COLS}）",
                        }
                    )
            else:
                missing.append(
                    {
                        "metric": "vec_total_cflt_ratio",
                        "reason": f"产物缺失（{rcr_csv}）",
                    }
                )
        # task_duration_us / block_dim：Summary.jsonl OpInfoSummary（无 OpBasicInfo）
        opinfo = load_opinfo_summary(root)
        if opinfo:
            td = _first_float(opinfo, OPBI_TASK_COLS)
            bd = _first_float(opinfo, OPBI_BLOCKDIM_COLS)
            if td is not None:
                scalars["task_duration_us"] = round(td, 2)
                sources["task_duration_us"] = f"{root}/Summary.jsonl::OpInfoSummary"
            if bd is not None:
                scalars["block_dim"] = round(bd, 2)
                sources["block_dim"] = f"{root}/Summary.jsonl::OpInfoSummary"
            if td is None:
                missing.append(
                    {
                        "metric": "task_duration_us",
                        "reason": "OpInfoSummary Task Duration(us) 为 null/缺失"
                        "（多 kernel 报告形态——逐 op 拆子报告，逐个解包判定）",
                    }
                )
        else:
            missing.append(
                {
                    "metric": "task_duration_us",
                    "reason": "Summary.jsonl OpInfoSummary 缺失（npu-compute 通道）",
                }
            )
        # top_stall_pct：PcSampling（DB 独有）——L0_STALL_DOMINANT 的生产者
        if npu_db_path is not None:
            pcs = pcsampling_stall(npu_db_path)
            if pcs:
                scalars["top_stall_pct"] = pcs["top_stall_pct"]
                sources["top_stall_pct"] = (
                    f"{npu_db_path}::PcSampling（DB 独有数据源；CSV 通道不存在该指标）"
                )
            else:
                missing.append(
                    {
                        "metric": "top_stall_pct",
                        "reason": "summary.db 无 PcSampling 表/空表"
                        "（未选 PmSampling Section 或 SIMD 算子无 SIMT 采样）",
                    }
                )
    else:
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

    if source_tool == "npu-compute":
        bridge_db_first(
            "PipeUtilization",
            [c.replace("(GB/s)", "") for c in MTE2_BW_COLS],
            pipe_csv,
            MTE2_BW_COLS,
            "mte2_active_bw_pct_of_peak",
            transform=_pct_of_peak,
        )
    else:
        bridge(
            pipe_csv,
            MTE2_BW_COLS,
            "mte2_active_bw_pct_of_peak",
            agg="mean",
            transform=_pct_of_peak,
        )

    # ---- L2：task_duration_us / block_dim（OpBasicInfo per-op 行；msprof 通道）----
    # npu-compute 通道在上方 Summary.jsonl 段已完成，此处 msprof 路径行为不变。
    if source_tool != "npu-compute":
        if opbi_csv and Path(opbi_csv).exists():
            rows = _load_rows(Path(opbi_csv))
            op_names = {
                str(r.get("Op Name") or "").strip() for r in rows if r.get("Op Name")
            }
            if args.kernel_name and op_names:
                rows = [
                    r
                    for r in rows
                    if str(r.get("Op Name") or "").strip() == args.kernel_name
                ]
            elif len(op_names) > 1:
                # G15 同款防护（检视 #19 修复）：多 Op Name 且无 --kernel-name →
                # 不取 rows[0] 张冠李戴，记 missing 待补采
                missing.append(
                    {
                        "metric": "task_duration_us",
                        "reason": f"OpBasicInfo 含多个 Op Name（{sorted(op_names)}）且未指定 "
                        "--kernel-name：不产出张冠李戴标量，请补 --kernel-name 重跑",
                    }
                )
                rows = []
            if rows:
                # 指定 kernel 时按其全部行聚合（mean），不取首行
                tds = [
                    v
                    for v in (_first_float(r, OPBI_TASK_COLS) for r in rows)
                    if v is not None
                ]
                bds = [
                    v
                    for v in (_first_float(r, OPBI_BLOCKDIM_COLS) for r in rows)
                    if v is not None
                ]
                if tds:
                    scalars["task_duration_us"] = round(sum(tds) / len(tds), 2)
                    sources["task_duration_us"] = str(opbi_csv)
                if bds:
                    scalars["block_dim"] = round(sum(bds) / len(bds), 2)
                    sources["block_dim"] = str(opbi_csv)
            elif not missing or missing[-1].get("metric") != "task_duration_us":
                missing.append(
                    {
                        "metric": "task_duration_us",
                        "reason": "OpBasicInfo 无匹配行（kernel 过滤后为空）",
                    }
                )
        else:
            missing.append(
                {"metric": "task_duration_us", "reason": "OpBasicInfo.csv 缺失"}
            )

    # ---- P1-6 DB 双源：OpBasicInfo 缺失时 sqlite 补位 task_duration_us ----
    # 等价查询固化在 extract_from_db（判定只出自脚本：SQL 拼接不进 LLM）；DB 无
    # kernel 名维度，多 kernel 场景保持 missing（不产出混采标量）。
    # 仅 msprof 通道（npu-compute 的 task_duration 已由 Summary.jsonl 段产出）。
    if (
        source_tool != "npu-compute"
        and "task_duration_us" not in scalars
        and not args.no_db
    ):
        db_path = Path(args.db) if args.db else find_metric_db(root)
        if db_path and db_path.exists():
            # G15 同款防护（检视 #14 修复）：DB 无 kernel 名维度，指定
            # --kernel-name 时拒绝 DB 补位——不产出跨 kernel 平均标量
            if args.kernel_name:
                db_out = {
                    "error": "指定 --kernel-name 时 DB 源不可过滤（无 kernel 名维度）"
                }
            else:
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
    pcs_out = pcsampling_stall(npu_db_path) if npu_db_path else None
    result = {
        "schema": SCHEMA,
        "source_tool": source_tool,
        "soc": args.soc,
        "kernel_name": args.kernel_name,
        "gm_bw_gbps": bw_gbps,
        "gm_bw_source": bw_note,
        "db_dual_source": "sqlite 备位 task_duration_us（CSV OpBasicInfo 缺失时启用；--no-db 关闭）"
        if str(sources.get("task_duration_us", "")).endswith(".db")
        or "（sqlite 双源" in str(sources.get("task_duration_us", ""))
        else None,
        "npucompute_db": str(npu_db_path) if npu_db_path else None,
        "pcsampling": pcs_out,
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
    if args.print_scalar:
        # 管道模式：stdout 只有裸 --scalar 串（human 摘要走 stderr，不污染管道）
        print(scalar_str)
    else:
        print(f"scalars: {scalar_str}")
    for m in missing:
        print(f"  ⚠ missing: {m['metric']} — {m['reason']}", file=sys.stderr)
    if not args.print_scalar:
        print(f"下一步：{result['next']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
