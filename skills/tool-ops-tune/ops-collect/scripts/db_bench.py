#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""db_bench.py — CSV vs summary.db 消费性能基准（Roadmap #4「查询更快」的验证载体）

目的：把「DB 格式比读 CSV 快」从口号变成**可复跑、带等价性门禁的实测数字**。
  - 数字只在套件真实消费形态下测（不测与消费无关的极端形态）：
      W1 facts 标量提取 — facts_bridge 核心形态（逐 Section 目标列 mean，
          NA 剔除在 CSV 侧由 Python 完成 / DB 侧由 WHERE IS NOT NULL 下推）
      W2 verdict 全表加载 — verdict.py 形态（整表 dict 化喂规则引擎）
      W3 分析查询批 — 报告/复盘形态（AVG/MAX/MIN/SUM/COUNT/过滤 6 条）
      W4 PcSampling stall 分布 — L0 判定形态（DB 独有，CSV 通道标 N/A）
  - **等价性断言是正确性门禁**：W1-W3 双通道数值必须相等（容差 1e-9），
    不等 = 转换或查询 bug → exit 1（提速数字不作门禁——机器相关）。
  - 提速比如实逐负载呈现：DB 并非处处更快（W2 全表 dict 化实测 DB 更慢，
    SQLite 行对象化开销 > csv.DictReader）——报告如实写慢，不粉饰。

用法：
  python3 db_bench.py --dir <npu-compute 解包目录> [-o report.json] [--repeat 30]
  目录无 summary.db 时自动 csv_to_db.py 构造（等价性门禁即转换器校验）。

输出（opstune.db-bench/1.0）：
  { "workloads": {W1..W4: {csv_ms, db_ms, speedup, csv_value, db_value, equivalent}},
    "report": {...行数/列数/输入摘要...} }
退出码：0 全绿（等价全过）/ 1 等价性失败或输入错误。
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
import time
from pathlib import Path

SCHEMA = "opstune.db-bench/1.0"
HERE = Path(__file__).resolve().parent
CSV_TO_DB = HERE / "csv_to_db.py"
UNIT_TABLE = "FieldUnitMap"
SECTIONS = (
    "PipeUtilization",
    "L2Cache",
    "ArithmeticUtilization",
    "ResourceConflictRatio",
    "Memory",
    "MemoryL0",
    "MemoryUB",
)
# W1 标量提取契约（每 Section 一个语义明确的标量；csv 侧带单位后缀 / db 侧为
# 规范化名，两通道候选序列必须同语义序——等价性门禁的比对基准）
W1_COLUMNS = {
    "PipeUtilization": (
        ["aiv_vec_ratio", "aic_vec_ratio"],
        ["aiv_vec_ratio", "aic_vec_ratio"],
    ),
    "L2Cache": (
        ["aiv_read_hit_rate", "aic_read_hit_rate"],
        ["aiv_read_hit_rate(%)", "aic_read_hit_rate(%)"],
    ),
}
# W3 查询批（全部落 PipeUtilization + L2Cache，与报告/复盘消费一致）。
# 契约元组：(标签, DB SQL, CSV 等价所需 Section, 规范化列, CSV 带单位后缀列)
W3_QUERIES = (
    (
        "AVG(aiv_vec_ratio) | PipeUtilization",
        "SELECT AVG(aiv_vec_ratio) FROM PipeUtilization WHERE aiv_vec_ratio IS NOT NULL",
        ("PipeUtilization", "aiv_vec_ratio", "aiv_vec_ratio"),
    ),
    (
        "MAX/MIN(aiv_mte2_ratio) | PipeUtilization",
        "SELECT MAX(aiv_mte2_ratio), MIN(aiv_mte2_ratio) FROM PipeUtilization",
        ("PipeUtilization", "aiv_mte2_ratio", "aiv_mte2_ratio"),
    ),
    (
        "SUM(aiv_read_close_miss) | L2Cache",
        "SELECT SUM(aiv_read_close_miss) FROM L2Cache",
        ("L2Cache", "aiv_read_close_miss", "aiv_read_close_miss"),
    ),
    (
        "MAX(aiv_time)-MIN(aiv_time) | PipeUtilization",
        "SELECT MAX(aiv_time) - MIN(aiv_time) FROM PipeUtilization",
        ("PipeUtilization", "aiv_time", "aiv_time(us)"),
    ),
    (
        "COUNT(aiv_vec_ratio>0.5) | PipeUtilization",
        "SELECT COUNT(*) FROM PipeUtilization WHERE aiv_vec_ratio > 0.5",
        ("PipeUtilization", "aiv_vec_ratio", "aiv_vec_ratio"),
    ),
    (
        "AVG(aic_scalar_ratio) | PipeUtilization",
        "SELECT AVG(aic_scalar_ratio) FROM PipeUtilization WHERE aic_scalar_ratio IS NOT NULL",
        ("PipeUtilization", "aic_scalar_ratio", "aic_scalar_ratio"),
    ),
)
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


def open_ro(db_path: Path) -> sqlite3.Connection:
    return sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)


def table_columns(db: sqlite3.Connection, table: str) -> set[str]:
    """实际列集合（PRAGMA 先验）——SQLite 对双引号未知标识符按字符串字面量
    处理（AVG('不存在的列') 合法且返回 0.0，不报错！），「列不存在」不能交给
    OperationalError 判定（检视 #4 修复，facts_bridge._table_columns 同款）。"""
    return {d[1] for d in db.execute(f'PRAGMA table_info("{table}")')}


def median_ms(fn, repeat: int) -> float:
    ts = []
    for _ in range(repeat):
        t0 = time.perf_counter()
        fn()
        ts.append((time.perf_counter() - t0) * 1000.0)
    return round(statistics.median(ts), 3)


# ---------------------------------------------------------------- W1: facts 标量提取


def w1_csv(data_dir: Path) -> dict[str, float]:
    """逐 Section 目标列 mean（候选列首个可解析者），NA 剔除。"""
    out = {}
    for section, (db_cols, csv_cols) in W1_COLUMNS.items():
        path = data_dir / f"{section}.csv"
        if not path.exists():
            continue
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            rows = list(csv.DictReader(f))
        vals = []
        for r in rows:
            for col in csv_cols:
                v = str(r.get(col) or "").strip()
                if v not in ("", "NA", "N/A", "-"):
                    try:
                        vals.append(float(v.replace("%", "")))
                    except ValueError:
                        continue
                    break
        if vals:
            out[section] = sum(vals) / len(vals)
    return out


def w1_db(db_path: Path) -> dict[str, float]:
    """同标量 DB 通道：COALESCE 逐行回退后 AVG——与 w1_csv 的逐行取首个
    可解析候选列**同语义**（检视 #5 修复：此前按列取首个有值列整列求均值，
    aiv_*/aic_* 部分行 NULL 的混排 Section 下两通道必然分叉，门禁误报）。

    单列形态退化为该列 AVG（COALESCE 至少两参——列集过滤后只剩一列时
    直接单列聚合，语义等价：逐行回退的候选序里只有一个可解析源）。"""
    out = {}
    db = open_ro(db_path)
    try:
        for section, (db_cols, _) in W1_COLUMNS.items():
            # 列存在性 PRAGMA 先验（检视 #4）：双引号未知列不报错而返回 0.0
            actual = table_columns(db, section)
            present = [c for c in db_cols if c in actual]
            if not present:
                continue
            if len(present) == 1:
                col = present[0]
                v = db.execute(
                    f'SELECT AVG("{col}") FROM "{section}" WHERE "{col}" IS NOT NULL'
                ).fetchone()[0]
            else:
                coalesce = ", ".join(f'"{c}"' for c in present)
                v = db.execute(
                    f"SELECT AVG(COALESCE({coalesce})) FROM"
                    f' "{section}" WHERE COALESCE({coalesce}) IS NOT NULL'
                ).fetchone()[0]
            if v is not None:
                out[section] = v
    finally:
        db.close()
    return out


# ---------------------------------------------------------------- W2: verdict 全表加载


def w2_csv(data_dir: Path, table: str) -> list[dict]:
    with open(data_dir / f"{table}.csv", encoding="utf-8-sig", errors="replace") as f:
        return list(csv.DictReader(f))


def w2_db(db_path: Path, table: str) -> list[dict]:
    """行 dict 化 + FieldUnitMap 还原单位后缀键名（verdict --db 同款加载器）。"""
    db = open_ro(db_path)
    try:
        units = dict(db.execute(f"SELECT field_name, unit FROM {UNIT_TABLE}"))
        cur = db.execute(f'SELECT * FROM "{table}" ORDER BY rowid')
        names = [d[0] for d in cur.description]
        keys = [f"{n}({units[n]})" if units.get(n) else n for n in names]
        return [
            {k: ("NA" if v is None else v) for k, v in zip(keys, row)}
            for row in cur.fetchall()
        ]
    finally:
        db.close()


# ---------------------------------------------------------------- W3: 分析查询批


def w3_csv(data_dir: Path) -> dict[str, float]:
    """6 条查询的 CSV 等价实现（只解析所需 Section，与消费语义一致）。

    缺 Section 文件按「无数据」跳过（检视 #2 修复：Section 子集目录——
    npu-compute --sections 追加可能只采部分 Section——不再 FileNotFoundError
    traceback，与 w1_csv 容错同款）。
    """
    needed: dict[str, list[str]] = {}
    for _, _, (section, _, csv_col) in W3_QUERIES:
        needed.setdefault(section, []).append(csv_col)
    loaded = {}
    for section, _cols in needed.items():
        section_csv = data_dir / f"{section}.csv"
        if not section_csv.exists():
            continue
        with open(section_csv, encoding="utf-8-sig", errors="replace") as f:
            loaded[section] = list(csv.DictReader(f))
    out = {}
    for label, _, (section, _db_col, csv_col) in W3_QUERIES:
        if section not in loaded:
            continue  # Section 缺失：该查询双通道都按「无数据」处理（DB 侧同理跳过）
        rows = loaded[section]
        vals = []
        for r in rows:
            v = str(r.get(csv_col) or "").strip()
            if v not in ("", "NA", "N/A", "-"):
                try:
                    vals.append(float(v.replace("%", "")))
                except ValueError:
                    continue
        if "AVG" in label:
            out[label] = sum(vals) / len(vals) if vals else None
        elif "MAX/MIN" in label:
            out[label] = (max(vals), min(vals)) if vals else None
        elif "SUM" in label:
            out[label] = sum(vals) if vals else None
        elif "MAX(aiv_time)-MIN" in label:
            out[label] = max(vals) - min(vals) if vals else None
        elif "COUNT" in label:
            out[label] = float(sum(1 for v in vals if v > 0.5))
    return out


def w3_db(db_path: Path) -> dict[str, float]:
    """DB 通道查询批：列存在性 PRAGMA 先验（检视 #4）+ Section 缺失同跳过。"""
    db = open_ro(db_path)
    try:
        out = {}
        for label, sql, (section, db_col, _) in W3_QUERIES:
            # 缺 Section 表 / 缺列：按「无数据」跳过（与 w3_csv 的容错一致；
            # 不交给 OperationalError——双引号未知列在启用 double-quoted-string
            # misfeature 的构建上不报错而返回 0.0/列名字符串，门禁会被污染）
            actual = table_columns(db, section)
            if db_col not in actual:
                continue
            row = db.execute(sql).fetchone()
            if "MAX/MIN" in label:
                out[label] = tuple(row)
            else:
                out[label] = row[0]
        return out
    finally:
        db.close()


# ---------------------------------------------------------------- W4: PcSampling


def w4_db(db_path: Path) -> dict | None:
    """stall 分布（DB 独有；白名单防表结构演进引入非 stall 列污染）。"""
    db = open_ro(db_path)
    try:
        tables = {
            r[0]
            for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")
        }
        if "PcSampling" not in tables:
            return None
        cur = db.execute("SELECT * FROM PcSampling")
        cols = [d[0] for d in cur.description]
        rows = cur.fetchall()
    finally:
        db.close()
    if not rows:
        return None
    agg = {c: 0.0 for c in STALL_COLUMNS if c in cols}
    for row in rows:
        for c, v in zip(cols, row):
            if c in agg:
                agg[c] += float(v or 0)
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


def close_enough(a, b, tol: float = 1e-9) -> bool:
    if a is None and b is None:
        return True
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(close_enough(x, y, tol) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(float(a) - float(b)) <= tol
    return a == b


def main() -> int:
    ap = argparse.ArgumentParser(
        description="CSV vs summary.db 消费性能基准（等价性门禁 + 提速比实测）"
    )
    ap.add_argument(
        "--dir",
        required=True,
        help="npu-compute 解包目录（含 Section CSV；无 summary.db 时自动转换构造）",
    )
    ap.add_argument(
        "--db",
        default=None,
        help="对照 summary.db 路径（缺省 <dir>/summary.db；不存在则 csv_to_db 构造）",
    )
    ap.add_argument(
        "--repeat",
        type=int,
        default=30,
        help="每负载重复次数（取中位数；冒烟场景可 --repeat 3）",
    )
    ap.add_argument(
        "--table", default="PipeUtilization", help="W2 全表加载的目标表（缺省主输入）"
    )
    ap.add_argument("-o", "--output", default=None, help="report JSON 输出路径")
    args = ap.parse_args()

    data_dir = Path(args.dir)
    if not data_dir.is_dir():
        print(f"ERROR: --dir 不是目录: {data_dir}", file=sys.stderr)
        return 1
    if not (data_dir / f"{args.table}.csv").exists():
        print(
            f"ERROR: 目录缺 {args.table}.csv（非 7 组 Section 解包形态——PmSampling 专项报告"
            "（如 verify_pipeline_pm.npu-rep）只有 summary.db/objdump，无 Section CSV；"
            "PcSampling 的消费链走 facts_bridge --db，不经本基准）: {data_dir}",
            file=sys.stderr,
        )
        return 1
    repeat = max(1, args.repeat)

    db_path = Path(args.db) if args.db else data_dir / "summary.db"
    built = False
    if not db_path.exists():
        tmp = Path(tempfile.mkdtemp(prefix="db-bench-"))
        db_path = tmp / "summary.db"
        p = subprocess.run(
            [
                sys.executable,
                str(CSV_TO_DB),
                "--csv-dir",
                str(data_dir),
                "--db",
                str(db_path),
                "--quiet",
            ],
            capture_output=True,
            text=True,
        )
        if p.returncode != 0:
            print(f"ERROR: csv_to_db 构造失败: {p.stderr.strip()}", file=sys.stderr)
            return 1
        built = True

    # ---- W1：facts 标量提取 ----
    w1_c = median_ms(lambda: w1_csv(data_dir), repeat)
    w1_d = median_ms(lambda: w1_db(db_path), repeat)
    w1_csv_v, w1_db_v = w1_csv(data_dir), w1_db(db_path)
    w1_eq = all(
        section in w1_db_v and close_enough(v, w1_db_v[section])
        for section, v in w1_csv_v.items()
    ) and set(w1_csv_v) == set(w1_db_v)

    # ---- W2：verdict 全表加载 ----
    w2_c = median_ms(lambda: w2_csv(data_dir, args.table), repeat)
    w2_d = median_ms(lambda: w2_db(db_path, args.table), repeat)
    w2_rows_c, w2_rows_d = w2_csv(data_dir, args.table), w2_db(db_path, args.table)

    # 数值经 DB REAL 化后失去字符串尾零（'36.787880' vs 36.78788）——比对按
    # 数值语义（parse_float 同款容差），非字符串字面
    def _num_or_na(v):
        s = str(v).strip()
        if s in ("", "NA", "N/A", "-"):
            return None
        try:
            return float(s.replace("%", ""))
        except ValueError:
            return s

    w2_eq = len(w2_rows_c) == len(w2_rows_d) and all(
        _num_or_na(csv_row.get("aiv_time(us)"))
        == _num_or_na(db_row.get("aiv_time(us)"))
        and _num_or_na(csv_row.get("aiv_vec_ratio"))
        == _num_or_na(db_row.get("aiv_vec_ratio"))
        for csv_row, db_row in zip(w2_rows_c, w2_rows_d)
    )

    # ---- W3：分析查询批 ----
    w3_c = median_ms(lambda: w3_csv(data_dir), repeat)
    w3_d = median_ms(lambda: w3_db(db_path), repeat)
    w3_csv_v, w3_db_v = w3_csv(data_dir), w3_db(db_path)
    # 等价比对按双通道共同命中集合（Section 子集目录各自缺列时按交集断言；
    # 全 7 组目录下两集合应当一致）
    common_w3 = set(w3_csv_v) & set(w3_db_v)
    w3_eq = bool(common_w3) and all(
        close_enough(w3_csv_v[k], w3_db_v[k]) for k in common_w3
    )

    # ---- W4：PcSampling（DB 独有）----
    w4 = w4_db(db_path)
    w4_d = median_ms(lambda: w4_db(db_path), repeat) if w4 else None

    def ratio(c: float, d: float | None) -> float | None:
        return round(c / d, 2) if d and d > 0 else None

    workloads = {
        "W1-facts-scalar-extract": {
            "desc": "facts_bridge 核心形态：逐 Section 目标列 mean",
            "csv_ms": w1_c,
            "db_ms": w1_d,
            "speedup_csv_over_db": ratio(w1_c, w1_d),
            "csv_value": w1_csv_v,
            "db_value": w1_db_v,
            "equivalent": w1_eq,
        },
        "W2-verdict-full-load": {
            "desc": f"verdict.py 形态：{args.table} 全表 dict 化（DB 行对象化有额外开销，如实呈现）",
            "csv_ms": w2_c,
            "db_ms": w2_d,
            "speedup_csv_over_db": ratio(w2_c, w2_d),
            "rows": len(w2_rows_c),
            "equivalent": w2_eq,
        },
        "W3-analytical-batch": {
            "desc": "报告/复盘形态：AVG/MAX/MIN/SUM/COUNT/过滤 6 条",
            "csv_ms": w3_c,
            "db_ms": w3_d,
            "speedup_csv_over_db": ratio(w3_c, w3_d),
            "csv_value": w3_csv_v,
            "db_value": w3_db_v,
            "equivalent": w3_eq,
        },
        "W4-pcsampling-dist": {
            "desc": "L0 判定形态：PcSampling stall 分布（DB 独有，无 CSV 对应物）",
            "csv_ms": None,
            "db_ms": w4_d,
            "speedup_csv_over_db": None,
            "db_value": w4,
            "equivalent": True,
        },
    }

    row_counts = {}
    for s in SECTIONS:
        if (data_dir / f"{s}.csv").exists():
            with open(data_dir / f"{s}.csv", encoding="utf-8-sig") as f:
                row_counts[s] = max(0, sum(1 for _ in f) - 1)
    report = {
        "schema": SCHEMA,
        "dir": str(data_dir),
        "db_path": str(db_path),
        "db_built_by_bench": built,
        "repeat": repeat,
        "row_counts": row_counts,
        "workloads": workloads,
        "all_equivalent": all(w["equivalent"] for w in workloads.values()),
    }

    print(
        f"[db-bench] dir={data_dir} rows={row_counts} repeat={repeat} db_built={built}"
    )
    for name, w in workloads.items():
        sp = w.get("speedup_csv_over_db")
        sp_text = f"speedup={sp}x" if sp is not None else "—"
        print(
            f"  {name}: csv={w['csv_ms']}ms db={w['db_ms']}ms {sp_text} equivalent={w['equivalent']}"
        )
    if w4:
        print(
            f"  W4 top_stall: {w4['top_stall']}={w4['top_stall_pct']}% (samples={w4['samples_total']})"
        )

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(f"[done] {out}")

    if not report["all_equivalent"]:
        print(
            "ERROR: 等价性断言失败——双通道数值不一致，转换器或查询实现有 bug，禁止引用本数字",
            file=sys.stderr,
        )
        return 1
    print("[gate] 等价性全过（W1-W3 双通道数值一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
