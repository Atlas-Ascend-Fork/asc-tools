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

"""dump_stats.py 单元测试。
用途：核验该脚本的数据行为、边界与错误处理。
使用方法（在 Skill 根目录）：python3 tests/dump_stats_test.py
"""

import argparse
import csv
from decimal import Decimal
import importlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
dump_stats = importlib.import_module("dump_stats")


class DumpStatsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.array = np.array([[1, 2], [3, 4]], np.float32)
        np.save(self.root / "golden.npy", self.array)
        self.row = {
            "Input/Output": "Output",
            "Index": "0",
            "Shape": "2x2",
            "Data Type": "DT_FLOAT",
            "Format": "ND",
            "Max Value": "4",
            "Min Value": "1",
            "Avg Value": "2.5",
            "Nan Count": "0",
            "Negative Inf Count": "0",
            "Positive Inf Count": "0",
        }
        self.pair = {
            "label": "request0/y",
            "actual": {"path": "stats.csv", "io": "output", "index": 0},
            "golden": "golden.npy",
        }
        self.write_csv([self.row])

    def write_csv(self, rows, name="stats.csv"):
        with (self.root / name).open("w", newline="", encoding="utf-8-sig") as file:
            writer = csv.DictWriter(file, fieldnames=rows[0])
            writer.writeheader()
            writer.writerows(rows)

    def test_native_columns_and_missing_optional_l2norm(self):
        result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
        self.assertEqual(result["state"], "no_stat_difference")
        self.assertEqual(result["actual_dtype"], "float32")
        self.assertEqual(result["golden_dtype"], "float32")
        self.assertEqual(result["optional_unavailable"], ["l2norm"])
        self.assertEqual(result["statistics"]["max"]["source_column"], "Max Value")
        self.assertIn("l2norm", "\n".join(dump_stats.report_lines([result])))

    def test_large_integer_max_min_keep_exact_csv_digits(self):
        value = 2**53
        np.save(self.root / "golden.npy", np.array([[value]], np.int64))
        self.write_csv(
            [
                {
                    **self.row,
                    "Shape": "1x1",
                    "Data Type": "DT_INT64",
                    "Max Value": str(value + 1),
                    "Min Value": str(value + 1),
                    "Avg Value": str(value),
                }
            ]
        )
        result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
        self.assertEqual(result["state"], "candidate")
        for key in ("max", "min"):
            self.assertEqual(result["statistics"][key]["actual"], value + 1)
            self.assertEqual(result["statistics"][key]["golden"], value)
            self.assertEqual(result["statistics"][key]["absolute_difference"], 1)

    def test_large_integer_csv_without_exact_decimal_form_is_incomplete(self):
        value = 2**53 + 1
        np.save(self.root / "golden.npy", np.array([[value]], np.int64))
        self.write_csv(
            [
                {
                    **self.row,
                    "Shape": "1x1",
                    "Data Type": "DT_INT64",
                    "Max Value": "9.007199254740992e15",
                    "Min Value": "9.007199254740992e15",
                    "Avg Value": "9.007199254740992e15",
                }
            ]
        )
        result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
        self.assertEqual(result["state"], "incomplete")
        self.assertIn("max_integer_csv_precision_unverified", result["unavailable"])
        self.assertIn("min_integer_csv_precision_unverified", result["unavailable"])

    def test_large_integer_cli_threshold_keeps_decimal_digits(self):
        value = 2**53 + 1
        np.save(self.root / "golden.npy", np.array([[0]], np.int64))
        self.write_csv(
            [
                {
                    **self.row,
                    "Shape": "1x1",
                    "Data Type": "DT_INT64",
                    "Max Value": str(value),
                    "Min Value": str(value),
                    "Avg Value": "0",
                }
            ]
        )
        pairs_path = self.root / "pairs.json"
        pairs_path.write_text(json.dumps([self.pair]))
        for tolerance, code, state in (
            (str(value), 0, "no_stat_difference"),
            (str(value - 1), 1, "candidate"),
        ):
            with self.subTest(tolerance=tolerance):
                out = self.root / f"threshold-{tolerance}"
                argv = [
                    "dump_stats.py",
                    "--pairs",
                    str(pairs_path),
                    "--out",
                    str(out),
                    "--atol",
                    tolerance,
                    "--rtol",
                    "0",
                ]
                with patch.object(sys, "argv", argv):
                    self.assertEqual(dump_stats.main(), code)
                report = json.loads((out / "summary.json").read_text())
                self.assertEqual(report["atol"], tolerance)
                self.assertEqual(report["pairs"][0]["state"], state)

    def test_large_integer_relative_threshold_does_not_round(self):
        reference = 2**53 + 1
        delta = 3002399751580331
        result = dump_stats.compare_stat_value(
            str(reference + delta),
            reference,
            "max",
            True,
            (Decimal(0), Decimal("0.3333333333333333333333333333")),
        )
        self.assertTrue(result[3])

    def test_float_stat_threshold_rejects_overflow(self):
        with self.assertRaises(argparse.ArgumentTypeError):
            dump_stats.screening_threshold("1e999")

    def test_native_int64_stats_na_remains_unavailable(self):
        np.save(self.root / "golden.npy", np.array([[2**53]], np.int64))
        unavailable_columns = (
            "Max Value",
            "Min Value",
            "Avg Value",
            "Nan Count",
            "Negative Inf Count",
            "Positive Inf Count",
        )
        self.write_csv(
            [
                {
                    **self.row,
                    "Shape": "1x1",
                    "Data Type": "DT_INT64",
                    **dict.fromkeys(unavailable_columns, "NA"),
                }
            ]
        )
        result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
        self.assertEqual(result["state"], "incomplete")
        self.assertEqual(
            set(result["unavailable"]),
            {
                "max",
                "min",
                "avg",
                "nan",
                "negative_inf",
                "positive_inf",
                "no_comparable_statistics",
            },
        )

    def test_higher_precision_float_golden_is_comparable(self):
        for actual_dtype, golden_dtype in (
            ("DT_FLOAT16", np.float32),
            ("DT_FLOAT", np.float64),
        ):
            with self.subTest(actual=actual_dtype, golden=golden_dtype):
                np.save(self.root / "golden.npy", self.array.astype(golden_dtype))
                self.write_csv([{**self.row, "Data Type": actual_dtype}])
                result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
                self.assertEqual(result["state"], "no_stat_difference")
                self.assertEqual(
                    result["actual_dtype"], dump_stats.DTYPES[actual_dtype]
                )
                self.assertEqual(result["golden_dtype"], str(np.dtype(golden_dtype)))
                self.assertIn(
                    "工程输出类型契约需另行核验",
                    "\n".join(dump_stats.report_lines([result])),
                )

    def test_bfloat16_csv_stats_compare_to_float32_golden(self):
        for actual_dtype in ("DT_BFLOAT16", "DT_BF16"):
            with self.subTest(actual=actual_dtype):
                self.write_csv([{**self.row, "Data Type": actual_dtype}])
                result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
                self.assertEqual(result["state"], "no_stat_difference")
                self.assertEqual(result["actual_dtype"], "bfloat16")
                self.assertEqual(result["golden_dtype"], "float32")

    def test_optional_l2norm_uses_native_value(self):
        self.write_csv([{**self.row, "l2norm": "5.47723"}])
        result = dump_stats.compare_pair(self.pair, self.root, 1e-5, 1e-5)
        self.assertEqual(result["state"], "no_stat_difference")
        self.assertEqual(result["optional_unavailable"], [])
        self.assertEqual(result["statistics"]["l2norm"]["actual"], 5.47723)

    def test_unavailable_optional_l2norm_does_not_block_screening(self):
        for value in ("", "NA", "N/A"):
            with self.subTest(value=value):
                self.write_csv([{**self.row, "l2norm": value}])
                result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
                self.assertEqual(result["state"], "no_stat_difference")
                self.assertEqual(result["unavailable"], [])
                self.assertEqual(result["optional_unavailable"], ["l2norm"])

    def test_malformed_csv_and_pair_are_incomplete(self):
        for content in (
            "Input/Output,Index\nOutput\n",
            "Input/Output,Index\nOutput,0,extra\n",
            "Input/Output,Index,Index\nOutput,0,0\n",
            'Input/Output,Index\nOutput,"unterminated\n',
        ):
            with self.subTest(csv=content):
                (self.root / "stats.csv").write_text(content)
                result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
                self.assertEqual(result["state"], "incomplete")
        for pair in (
            {"label": "missing_actual", "golden": "golden.npy"},
            {**self.pair, "actual": "stats.csv"},
            {**self.pair, "actual": None},
        ):
            with self.subTest(pair=pair):
                result = dump_stats.compare_pair(pair, self.root, 0, 0)
                self.assertEqual(result["state"], "incomplete")

    def test_csv_shape_rejects_noninteger_dimensions(self):
        np.save(self.root / "golden.npy", self.array[:1])
        for shape in ("[True, 2]", "[1.0, 2]", "[1+0j, 2]"):
            with self.subTest(shape=shape):
                self.write_csv([{**self.row, "Shape": shape}])
                result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
                self.assertEqual(result["state"], "incomplete")
                self.assertIn("非负整数", result["reason"])

    def test_statistical_overflow_is_not_no_difference(self):
        for actual, golden, tolerance in (
            ("1e308", -1e308, (0, 2)),
            ("1e308", -1e308, (0, 0)),
            ("1e308", 1e308, (0, 2)),
        ):
            with self.subTest(actual=actual, golden=golden, tolerance=tolerance):
                with self.assertRaisesRegex(ValueError, "溢出"):
                    dump_stats.compare_stat_value(
                        actual, golden, "max", False, tolerance
                    )

    def test_csv_selects_io_and_index(self):
        self.write_csv(
            [
                {**self.row, "Input/Output": "Input", "Max Value": "99"},
                {**self.row, "Index": "1", "Max Value": "88"},
                self.row,
            ]
        )
        result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
        self.assertEqual(result["state"], "no_stat_difference")
        self.assertEqual(result["statistics"]["max"]["actual"], 4)

    def test_duplicate_calls_require_unique_csv_source(self):
        self.write_csv([self.row, self.row])
        result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
        self.assertEqual(result["state"], "incomplete")
        self.write_csv([{**self.row, "Max Value": "9"}], "request1.csv")
        pair = {
            **self.pair,
            "label": "request1/y",
            "actual": {**self.pair["actual"], "path": "request1.csv"},
        }
        result = dump_stats.compare_pair(pair, self.root, 0, 0)
        self.assertEqual(result["state"], "candidate")
        self.assertEqual(result["statistics"]["max"]["actual"], 9)

    def test_csv_index_rejects_bool_float_and_negative_values(self):
        self.write_csv([self.row])
        for index in (True, False, 0.0, "0", -1, None):
            with self.subTest(index=index):
                actual = {**self.pair["actual"], "index": index}
                result = dump_stats.compare_pair(
                    {**self.pair, "actual": actual}, self.root, 0, 0
                )
                self.assertEqual(result["state"], "incomplete")

    def test_missing_required_stat_is_not_zero(self):
        self.row.pop("Nan Count")
        self.write_csv([self.row])
        result = dump_stats.compare_pair(self.pair, self.root, 0, 0)
        self.assertEqual(result["state"], "incomplete")
        self.assertIn("nan", result["unavailable"])

    def test_dense_layout_needs_explicit_reference_axes(self):
        self.write_csv([{**self.row, "Format": "NCHW"}])
        self.assertEqual(
            dump_stats.compare_pair(self.pair, self.root, 0, 0)["state"], "incomplete"
        )
        self.pair["golden"] = {"path": "golden.npy", "format": "NCHW"}
        self.assertEqual(
            dump_stats.compare_pair(self.pair, self.root, 0, 0)["state"],
            "no_stat_difference",
        )

    def test_nonfinite_counts_are_exact_despite_numeric_tolerance(self):
        self.write_csv(
            [
                {
                    **self.row,
                    "Nan Count": "1",
                    "Negative Inf Count": "1",
                    "Positive Inf Count": "1",
                }
            ]
        )
        result = dump_stats.compare_pair(self.pair, self.root, 100, 100)
        self.assertEqual(result["state"], "candidate")
        for name in ("nan", "negative_inf", "positive_inf"):
            self.assertTrue(result["statistics"][name]["different"])
            self.assertEqual(result["statistics"][name]["absolute_difference"], 1)
        np.save(
            self.root / "golden.npy",
            np.array([[np.nan, np.inf], [-np.inf, 0]], np.float32),
        )
        result = dump_stats.compare_pair(self.pair, self.root, 100, 100)
        self.assertEqual(result["state"], "incomplete")
        self.assertEqual(set(result["unavailable"]), {"max", "min", "avg"})
        self.assertFalse(result["statistics"]["nan"]["different"])

    def test_reports_keep_screening_states_and_never_certify_precision(self):
        # A permutation preserves all six native statistics despite wrong coordinates.
        np.save(self.root / "golden.npy", self.array[:, ::-1])
        pairs_path = self.root / "pairs.json"
        pairs_path.write_text(json.dumps([self.pair]))
        for index, (change, code, state) in enumerate(
            (
                ({}, 0, "no_stat_difference"),
                ({"Max Value": "9"}, 1, "candidate"),
                ({"Nan Count": ""}, 2, "incomplete"),
                ({"Max Value": "9", "Nan Count": ""}, 2, "incomplete"),
            )
        ):
            with self.subTest(state=state):
                self.write_csv([{**self.row, **change}])
                out = self.root / f"result-{index}"
                argv = [
                    "dump_stats.py",
                    "--pairs",
                    str(pairs_path),
                    "--out",
                    str(out),
                    "--atol",
                    "0",
                    "--rtol",
                    "0",
                ]
                with (
                    patch.object(sys, "argv", argv),
                    patch("sys.stdout", new_callable=io.StringIO),
                ):
                    self.assertEqual(dump_stats.main(), code)
                report = json.loads((out / "summary.json").read_text())
                self.assertFalse(report["precision_verified"])
                self.assertEqual(report["actual_source"], "runtime_stats_csv")
                self.assertEqual(report["pairs"][0]["state"], state)
                text = (out / "report.md").read_text()
                self.assertIn(f"| request0/y | {state} |", text)
                if change.get("Max Value") == "9":
                    self.assertEqual(report["candidates"], ["request0/y"])
                    self.assertEqual(
                        report["pairs"][0]["statistics"]["max"]["absolute_difference"],
                        5,
                    )
                    self.assertIn("| Max Value | 9.0 | 4.0 | 5.0 | True |", text)

    def test_report_write_error_returns_two_without_completed_summary(self):
        pairs = self.root / "pairs.json"
        pairs.write_text(json.dumps([self.pair]))
        out = self.root / "failed-report"
        argv = [
            "dump_stats.py",
            "--pairs",
            str(pairs),
            "--out",
            str(out),
            "--atol",
            "0",
            "--rtol",
            "0",
        ]
        with (
            patch.object(sys, "argv", argv),
            patch.object(
                dump_stats, "write_text_atomic", side_effect=OSError("写入失败")
            ),
            self.assertLogs(dump_stats.LOGGER, level="ERROR"),
        ):
            self.assertEqual(dump_stats.main(), 2)
        self.assertFalse((out / "summary.json").exists())


if __name__ == "__main__":
    unittest.main()
