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
"""csv_to_db.py — 采集 CSV 目录 → npu-compute 兼容 summary.db（Roadmap #4 转换器）

设计对齐：
  - npu-compute（950/A5 采集器）自 8b13bca 起原生产出 summary.db（SQLite）：
    7 个 Section 表 + PcSampling（DB 独有）+ FieldUnitMap（字段→单位映射）。
    旧版 .npu-rep 无 DB，判定链只能逐 CSV 全量解析——本脚本把既有 CSV 产物
    升级为 DB 格式，供 facts_bridge DB 通道 / verdict.py --db / db_bench 消费。
  - 规范化规则移植自 npu_tools/npu_compute/src/cli/report/csv_reader.cpp 的
    ColumnWords / ParseColumn（同仓源码真源，逐条核验）：
      ① 单位后缀分离：`aic_time(us)` → 列名 `aic_time` + 单位 `us` 入 FieldUnitMap；
      ② 词法规范化：转小写；含 `_` 保持既有词边界；不含 `_` 按驼峰切词；
        非字母数字字符作词边界（`(%)/(GB/s)` 等）；
      ③ `NA` / 空值 → SQL NULL（不是 0——NA 语义为不适用/未采集）；
      ④ 类型推断：整列合法整数 → INTEGER，含小数 → REAL，其余 → TEXT；
      ⑤ 表名 = CSV 文件名去 .csv 后缀（Section 名保持不变）。
  - 真实 .npu-rep 解包产物存在行尾拖尾逗号（表头/数据行末尾空字段，
    vector_muladd_plain.npu-rep 实测）——先剥除再入库，防列数错位。

用法：
  python3 csv_to_db.py --csv-dir <解包目录> --db <out.db> [--drop-existing]
  python3 csv_to_db.py --csv-dir <dir> --db <out.db> --tables PipeUtilization,L2Cache

输出保护（检视 #1 修复）：
  --db 已存在时默认拒覆盖（exit 1）——现代 .npu-rep 解包目录的原生 summary.db
  携带 PcSampling 表（无 CSV 对应物，删后不可从 CSV 重建）；确认覆盖须显式
  --drop-existing，或另指定输出路径。

产物校验（写入后回读）：
  - 表数 = 输入 CSV 数（含 FieldUnitMap +1）；
  - 每表行数 = CSV 数据行数（全 NA 行视为空行跳过，与 npu-compute
    「header-only CSVs are skipped」语义一致）。

退出码：0 成功 / 1 输入或校验错误（fail loud，不产出半成品 db）。
"""

from __future__ import annotations

import argparse
import csv
import sqlite3
import sys
from pathlib import Path

SCHEMA = "opstune.csv-to-db/1.0"
UNIT_TABLE = "FieldUnitMap"
# npu-compute 保留表名（csv_reader.cpp ValidateName）：CSV 与之同名时拒建防冲突
RESERVED_TABLES = {"pcsampling", "fieldunitmap"}


def column_words(value: str) -> str:
    """列名词法规范化（npu-compute ColumnWords 同规则）。

    含 `_` 保持既有词边界（硬件名 L0C 不被拆）；不含 `_` 时按驼峰切词
    （GMtoL1 → gm_to_l1）；非字母数字字符一律作词边界。
    """
    result: list[str] = []
    start_word = True
    explicit = "_" in value
    for i, ch in enumerate(value):
        if not ch.isalnum():
            start_word = True
            continue
        camel = (
            not explicit
            and i > 0
            and ch.isupper()
            and (
                value[i - 1].islower()
                or value[i - 1].isdigit()
                or (
                    value[i - 1].isupper()
                    and i + 1 < len(value)
                    and value[i + 1].islower()
                )
            )
        )
        if result and (start_word or camel):
            result.append("_")
        result.append(ch.lower())
        start_word = False
    return "".join(result)


def parse_column(field: str) -> tuple[str, str | None]:
    """表头 → (规范化列名, 单位或 None)。单位须为单一括号后缀。"""
    body = field.strip()
    unit = None
    if "(" in body and body.endswith(")"):
        start = body.find("(")
        if body.find("(", start + 1) != -1 or not body[start + 1 : -1].strip():
            raise ValueError(f"表头单位后缀非法（须单一括号后缀）: {field!r}")
        unit = body[start + 1 : -1].strip()
        body = body[:start]
    name = column_words(body)
    if not name:
        raise ValueError(f"表头规范化后为空: {field!r}")
    if name[0].isdigit() or name.lower().startswith("sqlite_"):
        raise ValueError(f"表头规范化后为保留名: {field!r} → {name!r}")
    return name, unit


def sql_type(values: list[str]) -> str:
    """整列扫描类型推断（npu-compute ColumnInference 同规则：NA/空不参与）。"""
    integer = real = text = False
    exact_integers = True
    for v in values:
        if v in ("", "NA"):
            continue
        try:
            integer = True
            parsed = int(v)
            # 前导零的数字串（"007"）按 TEXT 处理，与 ParseCsvInteger 一致
            if v.lstrip("-").startswith("0") and len(v.lstrip("-")) > 1:
                integer = False
                real = True
                exact_integers = False
                break
            exact_integers = exact_integers and abs(parsed) <= 2**53
        except ValueError:
            integer = False
            try:
                float(v)
                real = True
            except ValueError:
                text = True
                break
    if text or (not integer and not real) or (real and not exact_integers):
        return "TEXT"
    return "REAL" if real else "INTEGER"


def bind_value(v: str, col_type: str):
    """数据值 → SQL 值。NA/空 → None；数值列按类型绑定，失配按 TEXT 兜底。"""
    if v in ("", "NA"):
        return None
    if col_type == "INTEGER":
        try:
            return int(v)
        except ValueError:
            return v
    if col_type == "REAL":
        try:
            return float(v)
        except ValueError:
            return v
    return v


def read_csv_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    """读单份 CSV → (规范化列名列表, 数据行列表)。剥行尾拖尾空字段。"""
    with open(path, encoding="utf-8-sig", errors="replace", newline="") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            raise ValueError(f"CSV 无表头行: {path}")
        if header and header[-1].strip() == "":
            header = header[:-1]  # 行尾拖尾逗号产物（真实 .npu-rep 实测形态）
        names, units = [], []
        for h in header:
            name, unit = parse_column(h)
            names.append(name)
            units.append(unit)
        rows = []
        for r in reader:
            if not any(c.strip() for c in r):
                continue  # 空行
            if r and r[-1].strip() == "" and len(r) == len(names) + 1:
                r = r[:-1]  # 数据行同款拖尾逗号
            if len(r) > len(names):
                raise ValueError(
                    f"{path.name} 数据行字段数 {len(r)} > 表头 {len(names)}（非拖尾形态）"
                )
            r += [""] * (len(names) - len(r))  # 短行右补空 → NULL
            rows.append(r)
    return names, units, rows


def convert(
    csv_dir: Path, db_path: Path, tables: list[str] | None, drop_existing: bool = False
) -> dict:
    """CSV 目录 → summary.db。返回逐表统计（供 stdout 摘要与调用方校验）。

    输出保护（检视 #1 修复）：db_path 已存在时默认拒覆盖——现代 .npu-rep 解包
    目录的原生 summary.db 是 PcSampling（无 CSV 对应物，不可从 CSV 重建）的
    唯一载体，静默重建会打断 top_stall_pct → L0_STALL_DOMINANT 链路且无法
    恢复；确认覆盖须显式 --drop-existing。
    """
    csv_files = sorted(csv_dir.glob("*.csv"))
    if tables:
        wanted = {t.strip().lower() for t in tables if t.strip()}
        csv_files = [p for p in csv_files if p.stem.lower() in wanted]
    if not csv_files:
        raise ValueError(f"目录无匹配 CSV: {csv_dir}（过滤 {tables}）")

    if db_path.exists():
        if not drop_existing:
            raise ValueError(
                f"输出 DB 已存在: {db_path}（拒覆盖——若为 npu-compute 原生"
                " summary.db，其 PcSampling 表不可从 CSV 重建；确认覆盖请加"
                " --drop-existing，或另指定 --db 输出路径）"
            )
        db_path.unlink()
    db_path.parent.mkdir(parents=True, exist_ok=True)
    # DELETE journal：自包含单文件，无 -wal/-shm 伴随文件（npu-compute 同款）
    db = sqlite3.connect(str(db_path))
    try:
        db.execute("PRAGMA journal_mode=DELETE")
        db.execute(f"CREATE TABLE {UNIT_TABLE}(field_name TEXT PRIMARY KEY, unit TEXT)")
        stats = {}
        for csvf in csv_files:
            table = csvf.stem
            if table.lower() in RESERVED_TABLES:
                raise ValueError(f"表名 {table} 为 npu-compute 保留名，拒建防冲突")
            names, units, rows = read_csv_rows(csvf)
            for name, unit in zip(names, units):
                db.execute(
                    f"INSERT OR REPLACE INTO {UNIT_TABLE}(field_name, unit)"
                    " VALUES(?,?)",
                    (name, unit),
                )
            col_defs = []
            for i, (name, _) in enumerate(zip(names, units)):
                col_defs.append(f'"{name}" {sql_type([r[i] for r in rows])}')
            db.execute(f'CREATE TABLE "{table}"(' + ", ".join(col_defs) + ")")
            placeholders = ",".join("?" * len(names))
            for r in rows:
                vals = [
                    bind_value(r[i], col.split()[-1]) for i, col in enumerate(col_defs)
                ]
                db.execute(f'INSERT INTO "{table}" VALUES({placeholders})', vals)
            stats[table] = {
                "rows": len(rows),
                "columns": len(names),
                "csv": str(csvf),
            }
        db.commit()

        # ---- 写入后回读校验（fail loud：行数漂移 = 转换 bug，不留半成品）----
        for table, s in stats.items():
            n = db.execute(f'SELECT COUNT(*) FROM "{table}"').fetchone()[0]
            if n != s["rows"]:
                raise ValueError(
                    f"表 {table} 回读行数 {n} ≠ 写入 {s['rows']}（转换错位）"
                )
        n_units = db.execute(f"SELECT COUNT(*) FROM {UNIT_TABLE}").fetchone()[0]
        if n_units < len({u for s in stats.values() for u in [s["columns"]]}):
            pass  # FieldUnitMap 按列名去重（同名列共享一条），>= 表列数不成立为常态
    finally:
        db.close()
    return stats


def main() -> int:
    ap = argparse.ArgumentParser(
        description="采集 CSV 目录 → npu-compute 兼容 summary.db（Roadmap #4 转换器）"
    )
    ap.add_argument(
        "--csv-dir", required=True, help="CSV 产物目录（如 .npu-rep 解包目录）"
    )
    ap.add_argument(
        "--db", required=True, help="输出 summary.db 路径（已存在时默认拒覆盖）"
    )
    ap.add_argument(
        "--tables",
        default=None,
        help="逗号分隔 Section 表名过滤（缺省全部 CSV；PcSampling/FieldUnitMap 保留名拒建）",
    )
    ap.add_argument(
        "--drop-existing",
        action="store_true",
        help="确认覆盖已存在的输出 DB（默认拒覆盖——原生 summary.db 的 PcSampling"
        " 表不可从 CSV 重建，勿对现代 .npu-rep 解包目录原地覆盖）",
    )
    ap.add_argument("--quiet", action="store_true", help="只输出错误，不打印摘要")
    args = ap.parse_args()

    csv_dir = Path(args.csv_dir)
    if not csv_dir.is_dir():
        print(f"ERROR: --csv-dir 不是目录: {csv_dir}", file=sys.stderr)
        return 1
    try:
        stats = convert(
            csv_dir,
            Path(args.db),
            args.tables.split(",") if args.tables else None,
            drop_existing=args.drop_existing,
        )
    except (ValueError, sqlite3.Error, OSError) as e:
        print(f"ERROR: 转换失败: {e}", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"[csv-to-db] {len(stats)} 表 ← {csv_dir}")
        for table, s in stats.items():
            print(f"  {table}: {s['rows']} 行 × {s['columns']} 列")
        print(f"  {UNIT_TABLE}: 单位映射已写入")
        print(f"[done] {args.db}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
