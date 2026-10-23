#!/usr/bin/env python3
# -*- coding: UTF-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

"""Runtime 原生统计筛查脚本。
用途：按明确对应关系比较原生 CSV 与 golden 统计，输出候选异常，不判定精度通过。
使用方法（在 Skill 根目录）：
    python3 scripts/dump_stats.py --pairs stats-pairs.json --atol 1e-5 --rtol 1e-5 --out stats-result
筛查阈值由项目明确提供，示例值不是默认精度判据。
"""

import argparse
import ast
import csv
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import logging
import math
from pathlib import Path
import re
import sys

import numpy as np

from tensor_io import (
    fresh_dir,
    load_tensor,
    numeric,
    read_pairs,
    write_json,
    write_text_atomic,
)


LOGGER = logging.getLogger(__name__)
STAT_COLUMNS = {
    "Max Value": "max",
    "Min Value": "min",
    "Avg Value": "avg",
    "Nan Count": "nan",
    "Negative Inf Count": "negative_inf",
    "Positive Inf Count": "positive_inf",
    "l2norm": "l2norm",
}
DTYPES = {
    "DT_FLOAT": "float32",
    "DT_FLOAT16": "float16",
    "DT_DOUBLE": "float64",
    "DT_BFLOAT16": "bfloat16",
    "DT_BF16": "bfloat16",
}
FLOAT_DTYPES = {"float16", "float32", "float64", "bfloat16"}
for kind in (
    "int8",
    "int16",
    "int32",
    "int64",
    "uint8",
    "uint16",
    "uint32",
    "uint64",
    "bool",
):
    DTYPES["DT_" + kind.upper()] = kind


def read_csv_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle, strict=True)
        if not reader.fieldnames or len(set(reader.fieldnames)) != len(
            reader.fieldnames
        ):
            raise ValueError("CSV 表头缺失或重复")
        for row in reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError("CSV 列数与表头不一致")
            yield row


def select_row(spec, base):
    if not isinstance(spec, dict) or not isinstance(spec.get("path"), str):
        raise ValueError("actual 必须是包含 CSV path 的 JSON 对象")
    index = spec.get("index")
    if (
        spec.get("io") not in {"input", "output"}
        or not isinstance(index, int)
        or isinstance(index, bool)
    ):
        raise ValueError("actual 需要明确 io=input/output 和非负整数 index")
    if spec["index"] < 0:
        raise ValueError("index 必须非负")
    rows = []
    for row in read_csv_rows(base / spec["path"]):
        if row.get("Input/Output", "").strip().lower() == spec["io"] and row.get(
            "Index", ""
        ).strip() == str(spec["index"]):
            rows.append(row)
    if len(rows) != 1:
        raise ValueError(f"CSV 中对应行需要唯一，实际 {len(rows)} 行")
    return rows[0]


def golden_statistics(array):
    values = array.astype(np.float64)
    stats = {
        "nan": int(np.isnan(values).sum()),
        "negative_inf": int(np.isneginf(values).sum()),
        "positive_inf": int(np.isposinf(values).sum()),
    }
    if array.size and np.isfinite(values).all():
        integer = array.dtype.kind in "biu"
        stats.update(
            max=int(array.max()) if integer else float(values.max()),
            min=int(array.min()) if integer else float(values.min()),
            avg=float(values.mean()),
            l2norm=float(np.linalg.norm(values.ravel())),
        )
    return stats


def parse_shape(text):
    shape = (
        [int(value) for value in text.split("x")]
        if re.fullmatch(r"\d+(?:x\d+)*", text)
        else ast.literal_eval(text)
    )
    if not isinstance(shape, (list, tuple)):
        raise ValueError("CSV Shape 必须为整数列表或维度序列")
    for dimension in shape:
        if (
            not isinstance(dimension, int)
            or isinstance(dimension, bool)
            or dimension < 0
        ):
            raise ValueError("CSV Shape 的维度必须为非负整数")
    return list(shape)


def metadata_problem(row, array, reference_format=None):
    if parse_shape(row.get("Shape", "").strip()) != list(array.shape):
        return "shape_mismatch"
    dtype = DTYPES.get(row.get("Data Type", "").strip())
    if dtype is None:
        return "unknown_dtype"
    if dtype != str(array.dtype) and not (
        dtype in FLOAT_DTYPES and array.dtype.kind == "f"
    ):
        return "dtype_mismatch"
    layout = row.get("Format", "").strip().removeprefix("FORMAT_")
    if layout not in {"ND", "NCHW", "NHWC", "NCDHW"}:
        return "layout_requires_tensor_confirmation"
    if layout != "ND" and reference_format != layout:
        return "declare_reference_layout_before_comparing"
    return None


def compare_stat_value(text, golden, key, exact_integer, tolerances):
    atol, rtol = tolerances
    if exact_integer:
        if not re.fullmatch(r"[+-]?\d+", text):
            return None
        value, reference = int(text), int(golden)
        delta = abs(value - reference)
        limit = (
            Fraction(0)
            if key in {"nan", "negative_inf", "positive_inf"}
            else Fraction(Decimal(str(atol)))
            + Fraction(Decimal(str(rtol))) * abs(reference)
        )
        return value, reference, delta, delta > limit
    value, reference = float(text), float(golden)
    if not math.isfinite(value) or not math.isfinite(reference):
        return None
    delta = abs(value - reference)
    limit = float(atol) + float(rtol) * abs(reference)
    if not math.isfinite(delta) or not math.isfinite(limit):
        raise ValueError("统计差值或容差计算溢出，无法完成筛查")
    return numeric(value), numeric(reference), numeric(delta), delta > limit


def screening_threshold(text):
    try:
        value = Decimal(text)
    except InvalidOperation as error:
        raise argparse.ArgumentTypeError("统计筛查阈值必须为非负有限值") from error
    if not value.is_finite() or value < 0:
        raise argparse.ArgumentTypeError("统计筛查阈值必须为非负有限值")
    if not math.isfinite(float(value)):
        raise argparse.ArgumentTypeError("统计筛查阈值超出浮点统计支持范围")
    return value


def stat_differences(row, expected, atol, rtol):
    compared, missing = {}, []
    dtype = DTYPES.get(row.get("Data Type", "").strip(), "")
    integer_dtype = dtype.startswith(("int", "uint")) or dtype == "bool"
    for column, key in STAT_COLUMNS.items():
        if column not in row:
            if key != "l2norm":
                missing.append(key)
            continue
        if key not in expected or not row[column].strip():
            missing.append(key)
            continue
        text = row[column].strip()
        if text.upper() in {"NA", "N/A"}:
            missing.append(key)
            continue
        exact_integer = key in {"nan", "negative_inf", "positive_inf"} or (
            integer_dtype and key in {"max", "min"}
        )
        result = compare_stat_value(
            text, expected[key], key, exact_integer, (atol, rtol)
        )
        if result is None:
            missing.append(
                key + "_integer_csv_precision_unverified" if exact_integer else key
            )
            continue
        value, golden, delta, different = result
        compared[key] = {
            "source_column": column,
            "actual": value,
            "golden": golden,
            "absolute_difference": delta,
            "different": different,
        }
    if not compared:
        missing.append("no_comparable_statistics")
    return compared, missing


def compare_pair(pair, base, atol, rtol):
    result = {
        "label": pair["label"],
        "actual": pair.get("actual"),
        "golden": pair.get("golden"),
    }
    try:
        row = select_row(pair["actual"], base)
        array = load_tensor(pair["golden"], base)
        result["actual_dtype"] = DTYPES.get(row.get("Data Type", "").strip())
        result["golden_dtype"] = str(array.dtype)
        reference_format = (
            pair["golden"].get("format") if isinstance(pair["golden"], dict) else None
        )
        problem = metadata_problem(row, array, reference_format)
        if problem:
            return {**result, "state": "incomplete", "reason": problem}
        stats, missing = stat_differences(row, golden_statistics(array), atol, rtol)
        missing = [key for key in missing if key != "l2norm"]
        candidate = any(value["different"] for value in stats.values())
        state = (
            "incomplete"
            if missing
            else ("candidate" if candidate else "no_stat_difference")
        )
        return {
            **result,
            "state": state,
            "statistics": stats,
            "unavailable": missing,
            "optional_unavailable": [] if "l2norm" in stats else ["l2norm"],
        }
    except (ValueError, TypeError, KeyError, SyntaxError, OSError, csv.Error) as error:
        return {**result, "state": "incomplete", "reason": str(error)}


def report_lines(rows):
    lines = [
        "# Runtime 统计筛查",
        "",
        "统计差异只用于筛查；统计相同不能证明张量逐元素相同或精度通过。缺失行、布局不明和无法比较的指标需补采 tensor。",
        "",
        "| 观察点 | 筛查结果 |",
        "| --- | --- |",
    ]
    lines.extend(f"| {row['label']} | {row['state']} |" for row in rows)
    for row in rows:
        lines.extend(["", "## " + row["label"], ""])
        if row.get("actual_dtype") and row.get("golden_dtype"):
            lines.extend(
                [
                    f"张量 dtype：actual={row['actual_dtype']}，golden={row['golden_dtype']}；"
                    "工程输出类型契约需另行核验。",
                    "",
                ]
            )
        lines.extend(
            [
                "| 指标 | actual | golden | 差值 | 候选异常 |",
                "| --- | ---: | ---: | ---: | --- |",
            ]
        )
        for item in row.get("statistics", {}).values():
            lines.append(
                f"| {item['source_column']} | {item['actual']} | {item['golden']} "
                f"| {item['absolute_difference']} | {item['different']} |"
            )
        if row.get("optional_unavailable"):
            lines.extend(
                ["", "本次未采到的可选指标：" + ", ".join(row["optional_unavailable"])]
            )
        if row.get("unavailable") or row.get("reason"):
            lines.extend(
                ["", "未完成项：" + str(row.get("reason", row.get("unavailable")))]
            )
    return lines


def summarize(args):
    path = Path(args.pairs).resolve()
    pairs = read_pairs(path)
    rows = [compare_pair(pair, path.parent, args.atol, args.rtol) for pair in pairs]
    state = (
        "incomplete"
        if any(row["state"] == "incomplete" for row in rows)
        else "screened"
    )
    report = {
        "state": state,
        "precision_verified": False,
        "actual_source": "runtime_stats_csv",
        "optional_statistics": ["l2norm"],
        "atol": str(args.atol),
        "rtol": str(args.rtol),
        "pairs": rows,
        "candidates": [
            row["label"]
            for row in rows
            if any(item["different"] for item in row.get("statistics", {}).values())
        ],
    }
    out = fresh_dir(args.out)
    lines = report_lines(rows)
    write_text_atomic(out / "report.md", "\n".join(lines) + "\n")
    write_json(out / "summary.json", report)
    LOGGER.info(
        "统计筛查：%d 项，候选 %d；未作精度通过判定。",
        len(rows),
        len(report["candidates"]),
    )
    return 2 if state == "incomplete" else int(bool(report["candidates"]))


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument(
        "--atol",
        type=screening_threshold,
        required=True,
        help="统计量筛查阈值，不是逐元素判据",
    )
    parser.add_argument("--rtol", type=screening_threshold, required=True)
    args = parser.parse_args()
    try:
        return summarize(args)
    except (ValueError, OSError, TypeError, KeyError) as error:
        LOGGER.error("未完成：%s", error)
        return 2


if __name__ == "__main__":
    sys.exit(main())
