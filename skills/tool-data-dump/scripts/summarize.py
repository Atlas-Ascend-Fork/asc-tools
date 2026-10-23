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

"""官方精度比对与报告脚本。
用途：调用配套 msaccucmp file_compare，补充逐元素超差数量、坐标及完整掩码。
使用方法（在 Skill 根目录，先加载执行端 CANN 环境）：
    python3 scripts/summarize.py --pairs tensor-pairs.json --out result
    python3 scripts/summarize.py --pairs tensor-pairs.json --stats stats-result/summary.json --out result-with-stats
"""

import argparse
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import json
import logging
import math
import os
from pathlib import Path
import re
import subprocess
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


def official_tool(path):
    if path or os.environ.get("MSACCUCMP_PATH"):
        tool = Path(path or os.environ["MSACCUCMP_PATH"]).resolve()
    else:
        root = os.environ.get("ASCEND_HOME_PATH") or os.environ.get(
            "ASCEND_TOOLKIT_HOME"
        )
        if not root:
            raise ValueError("请指定 --tool 或加载执行端的 CANN 环境")
        tool = Path(root) / "tools/operator_cmp/compare/msaccucmp.py"
    if not tool.is_file():
        raise ValueError("找不到配套 msaccucmp.py")
    return tool


def official_metrics(folder, stdout):
    files = list((folder / "result").glob("file_result*.txt"))
    if len(files) > 1:
        return {}, None
    text = files[0].read_text() if files else stdout
    source = (
        str(Path(folder.name) / files[0].relative_to(folder))
        if files
        else folder.name + "/stdout.log"
    )
    metrics = dict(
        re.findall(
            r"^\s*([A-Z][A-Za-z]+(?:Error|Similarity|Distance|Divergence))\s*:\s*(\S+)\s*$",
            text,
            re.M,
        )
    )
    return metrics, source


def file_compare(tool, actual, golden, folder, timeout):
    folder.mkdir()
    args = [
        sys.executable,
        str(tool),
        "file_compare",
        "-m",
        str(actual),
        "-g",
        str(golden),
        "-out",
        str(folder / "result"),
    ]
    try:
        result = subprocess.run(
            args, capture_output=True, text=True, timeout=timeout, check=False
        )
    except subprocess.TimeoutExpired as error:
        (folder / "stdout.log").write_bytes(error.stdout or b"")
        (folder / "stderr.log").write_bytes(error.stderr or b"")
        return {
            "state": "timeout",
            "metrics": {},
            "source": str(folder.name + "/stdout.log"),
        }
    (folder / "stdout.log").write_text(result.stdout)
    (folder / "stderr.log").write_text(result.stderr)
    metrics, source = official_metrics(folder, result.stdout)
    return {
        "state": "completed" if result.returncode == 0 and metrics else "incomplete",
        "exit_code": result.returncode,
        "metrics": metrics,
        "source": source,
    }


def relative_errors(error, golden, finite, same):
    relative = np.full(error.shape, np.inf)
    with np.errstate(divide="ignore", invalid="ignore", over="ignore"):
        np.divide(
            error.astype(np.float64),
            np.abs(golden.astype(np.float64)),
            out=relative,
            where=golden != 0,
        )
    relative[error == 0] = 0
    relative[~finite & ~same] = np.inf
    return relative


def coordinates(actual, golden, criterion, topk, mask_path=None):
    if "equal_nan" in criterion and not isinstance(criterion["equal_nan"], bool):
        raise ValueError("equal_nan 必须是 JSON 布尔值")
    if actual.shape != golden.shape:
        raise ValueError("shape 不一致，不能广播或 reshape")
    if actual.dtype != golden.dtype and not (
        actual.dtype.kind == golden.dtype.kind == "f"
        and max(actual.dtype.itemsize, golden.dtype.itemsize) <= 8
    ):
        raise ValueError("仅支持同 dtype 或已配对的不同浮点精度张量")
    if actual.size == 0:
        raise ValueError("空张量没有有效比较数据")
    a, g = actual.ravel(), golden.ravel()
    if a.dtype.kind in "biu":
        error = np.abs(a.astype(object) - g.astype(object))
        finite = np.ones(a.size, dtype=bool)
        same = a == g
    else:
        a, g = a.astype(np.float64), g.astype(np.float64)
        finite = np.isfinite(a) & np.isfinite(g)
        same = (a == g) | (
            criterion.get("equal_nan", False) & np.isnan(a) & np.isnan(g)
        )
        with np.errstate(invalid="ignore", over="ignore"):
            error = np.where(finite, np.abs(a - g), np.where(same, 0.0, np.inf))
    bad = mismatch_mask(error, g, finite, same, criterion)
    relative = relative_errors(error, g, finite, same)
    candidates = np.flatnonzero(bad if bad is not None else ~same)
    chosen = candidates[np.argsort(-error[candidates], kind="stable")[:topk]]
    top = [
        {
            "coordinate": list(map(int, np.unravel_index(int(i), actual.shape))),
            "actual": scalar(actual.ravel()[i]),
            "golden": scalar(golden.ravel()[i]),
            "absolute_error": scalar(error[i]),
        }
        for i in chosen
    ]
    return {
        "state": "unjudged" if bad is None else ("mismatch" if bad.any() else "pass"),
        "elements": a.size,
        "mismatch_count": None if bad is None else int(bad.sum()),
        "mismatch_fraction": None if bad is None else float(bad.mean()),
        "max_absolute_error": scalar(error.max()),
        "max_relative_error": numeric(relative.max()),
        "actual_nonfinite": int((~np.isfinite(a)).sum()),
        "golden_nonfinite": int((~np.isfinite(g)).sum()),
        "top_errors": top,
        "mismatch_mask": save_mask(bad, actual.shape, mask_path),
    }


def save_mask(mask, shape, path):
    if mask is None or path is None:
        return None
    np.save(path, mask.reshape(shape), allow_pickle=False)
    return Path(path).name


def scalar(value):
    return (
        int(value) if isinstance(value, (int, np.integer, np.bool_)) else numeric(value)
    )


def valid_tolerance(value):
    if isinstance(value, (bool, np.bool_)) or not isinstance(
        value, (int, float, Decimal, np.integer, np.floating)
    ):
        return False
    try:
        number = Decimal(str(value))
    except (InvalidOperation, ValueError):
        return False
    return number.is_finite() and number >= 0


def integer_mismatch_mask(error, golden, atol, rtol):
    absolute, relative = Fraction(str(atol)), Fraction(str(rtol))
    denominator = math.lcm(absolute.denominator, relative.denominator)
    absolute_term = absolute.numerator * (denominator // absolute.denominator)
    relative_term = relative.numerator * (denominator // relative.denominator)
    limit = absolute_term + relative_term * np.abs(golden.astype(object))
    return error * denominator > limit


def mismatch_mask(error, golden, finite, same, criterion):
    if "exact" in criterion and not isinstance(criterion["exact"], bool):
        raise ValueError("exact 必须是 JSON 布尔值")
    if criterion.get("exact"):
        if "atol" in criterion or "rtol" in criterion:
            raise ValueError("exact 与 atol/rtol 不能同时指定")
        return ~same
    atol, rtol = criterion.get("atol"), criterion.get("rtol")
    if atol is None and rtol is None:
        return None
    if not all(valid_tolerance(value) for value in (atol, rtol)):
        raise ValueError("项目判据必须同时给出非负有限 atol/rtol")
    if error.dtype.kind == "O":
        return integer_mismatch_mask(error, golden, atol, rtol)
    try:
        atol, rtol = float(atol), float(rtol)
    except OverflowError as exc:
        raise ValueError("浮点判据超出 float64 范围") from exc
    if not math.isfinite(atol) or not math.isfinite(rtol):
        raise ValueError("浮点判据超出 float64 范围")
    with np.errstate(over="ignore", invalid="ignore"):
        limit = atol + rtol * np.abs(golden.astype(np.float64))
    if np.any(finite & (~np.isfinite(error) | ~np.isfinite(limit))):
        raise ValueError("误差计算溢出，请使用工程支持该数值范围的比较器")
    return (finite & (error > limit)) | (~finite & ~same)


def dtype_contract(pair, actual):
    expected = pair.get("expected_actual_dtype")
    if expected is None:
        return "unjudged"
    if not isinstance(expected, str) or not expected:
        raise ValueError("expected_actual_dtype 必须是 NumPy dtype 名称")
    try:
        required = np.dtype(expected)
    except (TypeError, ValueError) as error:
        raise ValueError("expected_actual_dtype 不是有效 NumPy dtype") from error
    return "pass" if actual.dtype == required else "mismatch"


def report_criterion(criterion):
    return {
        key: str(value) if isinstance(value, Decimal) else value
        for key, value in criterion.items()
    }


def compare_pair(pair, tool, out, index, args):
    base = args.pairs.resolve().parent
    actual_spec = pair["actual"]
    actual_path = (
        base / (actual_spec if isinstance(actual_spec, str) else actual_spec["path"])
    ).resolve()
    if actual_path.suffix != ".npy":
        raise ValueError("actual 必须为官方转换后的原始 NPY")
    actual, golden = (
        load_tensor(pair["actual"], base),
        load_tensor(pair["golden"], base),
    )
    criterion = pair.get("criterion", {})
    if not isinstance(criterion, dict):
        raise ValueError("criterion 必须是 JSON 对象")
    details = coordinates(
        actual, golden, criterion, args.topk, out / f"mismatch-{index}.npy"
    )
    dtype_state = dtype_contract(pair, actual)
    reference = out / f"golden-{index}.npy"
    np.save(reference, golden, allow_pickle=False)
    official = file_compare(
        tool, actual_path, reference, out / f"official-{index}", args.timeout
    )
    numeric_state = details["state"]
    state = numeric_state if official["state"] == "completed" else "incomplete"
    if state == "pass" and dtype_state == "unjudged":
        state = "unjudged"
    if state != "incomplete" and dtype_state == "mismatch":
        state = "mismatch"
    return {
        "label": pair["label"],
        "actual": pair["actual"],
        "golden": pair["golden"],
        "shape": list(actual.shape),
        "dtype": str(actual.dtype),
        "actual_dtype": str(actual.dtype),
        "golden_dtype": str(golden.dtype),
        "expected_actual_dtype": pair.get("expected_actual_dtype"),
        "numeric_state": numeric_state,
        "dtype_contract_state": dtype_state,
        "criterion": report_criterion(criterion),
        "official": official,
        "details": details,
        "state": state,
    }


def statistics_lines(statistics):
    if statistics is None:
        return ["本次辅助报告未传入原生 stats 汇总。", ""]
    if not isinstance(statistics.get("pairs"), list):
        raise ValueError("统计汇总的 pairs 必须为列表")
    lines = [
        "## 原生 stats 与 golden",
        "",
        "| 调用/张量 | 指标 | actual | golden | 差值 | 筛查状态 |",
        "| --- | --- | ---: | ---: | ---: | --- |",
    ]
    notes = []
    for row in statistics["pairs"]:
        if not isinstance(row, dict) or not isinstance(row.get("statistics", {}), dict):
            raise ValueError("统计汇总的配对项和 statistics 必须为 JSON 对象")
        for item in row.get("statistics", {}).values():
            lines.append(
                f"| {row['label']} | {item['source_column']} | {item['actual']} "
                f"| {item['golden']} | {item['absolute_difference']} | {row['state']} |"
            )
        if row.get("optional_unavailable"):
            unavailable = row["optional_unavailable"]
            if not isinstance(unavailable, list) or any(
                not isinstance(item, str) for item in unavailable
            ):
                raise ValueError("未采到的可选指标必须为字符串列表")
            notes.append(
                f"- {row['label']} 本次未采到的可选指标：{', '.join(row['optional_unavailable'])}"
            )
        if row.get("reason") or row.get("unavailable"):
            notes.append(
                f"- {row['label']} 未完成项：{row.get('reason', row.get('unavailable'))}"
            )
    lines.extend(["", *notes])
    lines.extend(["", "统计一致不代表逐元素精度通过。", ""])
    return lines


def report_lines(rows, statistics=None):
    lines = [
        "# Dump 与 golden 数据层比对",
        "",
        "官方指标与项目判据分别列出；退出码只覆盖显式列出的比较项，不代表未采集输出通过。",
        "",
        "| 张量/调用 | shape | 元素数 | 综合状态 | 超差数量 | 非有限值 actual / golden | 最大绝对误差 | 最大相对误差 |",
        "| --- | --- | ---: | --- | ---: | --- | ---: | ---: |",
    ]
    lines = lines[:3] + [""] + statistics_lines(statistics) + lines[3:]
    for row in rows:
        detail = row.get("details", {})
        lines.append(
            f"| {row['label']} | {row.get('shape', '未完成')} "
            f"| {detail.get('elements', '未完成')} | {row['state']} "
            f"| {detail.get('mismatch_count', '未完成')} "
            f"| {detail.get('actual_nonfinite', '未完成')} / {detail.get('golden_nonfinite', '未完成')} "
            f"| {detail.get('max_absolute_error', '未完成')} "
            f"| {detail.get('max_relative_error', '未完成')} |"
        )
    for row in rows:
        lines.extend(
            [
                "",
                "## " + row["label"],
                "",
            ]
        )
        if row.get("actual_dtype") and row.get("golden_dtype"):
            lines.extend(
                [
                    f"张量 dtype：actual={row['actual_dtype']}，golden={row['golden_dtype']}；"
                    f"期望 actual={row.get('expected_actual_dtype') or '未提供'}。"
                    f"数值={row['numeric_state']}，输出类型契约={row['dtype_contract_state']}。",
                    "",
                ]
            )
        lines.extend(
            [
                "```json",
                json.dumps(row, ensure_ascii=False, indent=2, allow_nan=False),
                "```",
            ]
        )
    return lines


def save_progress(out, rows, statistics, *, execution_complete=False):
    summary = {
        "pairs": rows,
        "scope": [row["label"] for row in rows],
        "execution_complete": execution_complete,
        "precision_verified": execution_complete
        and all(
            row["state"] in {"pass", "mismatch"}
            and row.get("numeric_state") in {"pass", "mismatch"}
            and row.get("dtype_contract_state") in {"pass", "mismatch"}
            for row in rows
        ),
    }
    if statistics is not None:
        summary["runtime_statistics"] = statistics
    write_text_atomic(
        out / "report.md", "\n".join(report_lines(rows, statistics)) + "\n"
    )
    write_json(out / "summary.json", summary)
    return summary


def run_pairs(pairs, tool, out, args, statistics):
    rows = [{"label": pair["label"], "state": "pending"} for pair in pairs]
    save_progress(out, rows, statistics)
    for index, pair in enumerate(pairs):
        rows[index] = {"label": pair["label"], "state": "running"}
        save_progress(out, rows, statistics)
        try:
            rows[index] = compare_pair(pair, tool, out, index, args)
        except (ValueError, KeyError, OSError, TypeError) as error:
            rows[index] = {
                "label": pair["label"],
                "state": "incomplete",
                "reason": str(error),
            }
        except KeyboardInterrupt:
            rows[index] = {
                "label": pair["label"],
                "state": "incomplete",
                "reason": "interrupted",
            }
            save_progress(out, rows, statistics)
            raise
        save_progress(out, rows, statistics)
    return save_progress(out, rows, statistics, execution_complete=True)


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", type=Path, required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--tool")
    parser.add_argument("--stats", type=Path, help="已有原生 stats 汇总 summary.json")
    parser.add_argument(
        "--timeout", type=float, default=120, help="单对官方比较的秒数上限"
    )
    parser.add_argument("--topk", type=int, default=10)
    args = parser.parse_args()
    if args.topk < 1 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("topk 和 timeout 必须大于零")
    try:
        pairs = read_pairs(args.pairs, parse_float=Decimal)
        stats = None
        if args.stats:
            stats = json.loads(args.stats.read_text())
            if not isinstance(stats, dict):
                raise ValueError("统计汇总必须是 JSON 对象")
            if stats.get("actual_source") != "runtime_stats_csv":
                raise ValueError("统计汇总未声明 Runtime 原生 CSV 来源")
        tool = official_tool(args.tool)
        summary = run_pairs(pairs, tool, fresh_dir(args.out), args, stats)
    except (ValueError, OSError, TypeError, KeyError) as error:
        LOGGER.error("未完成：%s", error)
        return 2
    return (
        2
        if not summary["precision_verified"]
        else int(any(r["state"] == "mismatch" for r in summary["pairs"]))
    )


if __name__ == "__main__":
    raise SystemExit(main())
