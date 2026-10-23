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

"""summarize.py 单元测试。
用途：核验该脚本的数据行为、边界与错误处理。
使用方法（在 Skill 根目录）：python3 tests/summarize_test.py
"""

import importlib
from decimal import Decimal
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
summarize = importlib.import_module("summarize")


class SummarizeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.array = np.array([[1, 2], [3, 4]], np.float32)
        np.save(self.root / "golden.npy", self.array)

    def test_coordinates_detect_permutation_despite_equal_statistics(self):
        actual = self.array[:, ::-1].copy()
        result = summarize.coordinates(actual, self.array, {"exact": True}, 3)
        self.assertEqual(result["mismatch_count"], 4)
        self.assertEqual(result["mismatch_fraction"], 1)
        self.assertEqual(result["top_errors"][0]["coordinate"], [0, 0])
        self.assertEqual(result["top_errors"][0]["actual"], 2)
        self.assertEqual(result["top_errors"][0]["golden"], 1)

    def test_saved_mask_covers_all_errors_when_topk_is_limited(self):
        actual = np.array([[2, 2], [4, 4]], np.float32)
        np.save(self.root / "actual.npy", actual)
        pair = {
            "label": "r/output0",
            "actual": "actual.npy",
            "golden": "golden.npy",
            "criterion": {"exact": True},
            "expected_actual_dtype": "float32",
        }
        out = self.root / "result"
        out.mkdir()
        args = SimpleNamespace(pairs=self.root / "pairs.json", topk=1, timeout=1)
        official = {"state": "completed", "metrics": {"MaxAbsoluteError": "1"}}
        with patch.object(summarize, "file_compare", return_value=official):
            row = summarize.compare_pair(pair, Path("tool.py"), out, 0, args)
        details = row["details"]
        mask = np.load(out / details["mismatch_mask"], allow_pickle=False)
        self.assertEqual(mask.dtype, np.dtype(bool))
        np.testing.assert_array_equal(mask, [[True, False], [True, False]])
        self.assertEqual(details["mismatch_count"], int(mask.sum()))
        self.assertEqual(len(details["top_errors"]), 1)

    def test_mask_is_absent_without_criterion_and_false_for_equal_values(self):
        path = self.root / "mask.npy"
        result = summarize.coordinates(self.array, self.array, {}, 1, path)
        self.assertEqual(result["state"], "unjudged")
        self.assertIsNone(result["mismatch_mask"])
        self.assertFalse(path.exists())
        summarize.coordinates(self.array, self.array, {"exact": True}, 1, path)
        mask = np.load(path, allow_pickle=False)
        self.assertEqual(mask.shape, self.array.shape)
        self.assertFalse(mask.any())

    def test_nonfinite_and_zero_reference(self):
        actual = np.array([np.nan, np.inf, -np.inf, 1], np.float32)
        golden = np.array([0, np.inf, np.inf, 0], np.float32)
        row = summarize.coordinates(actual, golden, {"atol": 0, "rtol": 0}, 10)
        self.assertEqual(row["mismatch_count"], 3)
        self.assertEqual(row["max_relative_error"], "Infinity")
        self.assertEqual(row["actual_nonfinite"], 3)

    def test_nan_equality_requires_explicit_project_policy(self):
        array = np.array([np.nan, np.inf, -np.inf], np.float32)
        row = summarize.coordinates(array, array, {"exact": True}, 3)
        self.assertEqual(row["mismatch_count"], 1)
        row = summarize.coordinates(array, array, {"exact": True, "equal_nan": True}, 3)
        self.assertEqual(row["state"], "pass")
        self.assertEqual(row["mismatch_count"], 0)

    def test_equal_nan_requires_json_boolean(self):
        values = np.array([np.nan], np.float32)
        for invalid in ("false", "true", 0, 1, None):
            with (
                self.subTest(value=invalid),
                self.assertRaisesRegex(ValueError, "equal_nan"),
            ):
                summarize.coordinates(
                    values, values, {"exact": True, "equal_nan": invalid}, 1
                )
        self.assertEqual(
            summarize.coordinates(
                values, values, {"exact": True, "equal_nan": False}, 1
            )["state"],
            "mismatch",
        )

    def test_missing_criterion_and_incompatible_metadata(self):
        self.assertEqual(
            summarize.coordinates(self.array, self.array, {}, 3)["state"], "unjudged"
        )
        for golden in (self.array.ravel(), self.array.astype(np.int32)):
            with (
                self.subTest(shape=golden.shape, dtype=golden.dtype),
                self.assertRaises(ValueError),
            ):
                summarize.coordinates(self.array, golden, {"exact": True}, 3)

    def test_mixed_float_precision_keeps_numeric_difference(self):
        for actual, golden in (
            (
                np.array([1, 1.5], np.float16),
                np.array([1, 1.25], np.float32),
            ),
            (
                np.array([1, 1.5], np.float32),
                np.array([1, 1.25], np.float64),
            ),
        ):
            with self.subTest(actual=actual.dtype, golden=golden.dtype):
                result = summarize.coordinates(
                    actual, golden, {"atol": 0.1, "rtol": 0}, 2
                )
                self.assertEqual(result["state"], "mismatch")
                self.assertEqual(result["mismatch_count"], 1)
                self.assertEqual(result["top_errors"][0]["coordinate"], [1])

    def test_mixed_float_pair_preserves_files_and_reports_both_dtypes(self):
        actual_path = self.root / "actual-half.npy"
        golden_path = self.root / "golden-mixed.npy"
        np.save(actual_path, np.array([1, 2], np.float16))
        np.save(golden_path, np.array([1, 2], np.float32))
        before = actual_path.read_bytes()
        pair = {
            "label": "r/y",
            "actual": actual_path.name,
            "golden": golden_path.name,
            "criterion": {"exact": True},
            "expected_actual_dtype": "float16",
        }
        out = self.root / "mixed-result"
        out.mkdir()
        args = SimpleNamespace(pairs=self.root / "pairs.json", topk=2, timeout=1)

        def check_official_inputs(_tool, actual, golden, _folder, _timeout):
            self.assertEqual(np.load(actual).dtype, np.dtype("float16"))
            self.assertEqual(np.load(golden).dtype, np.dtype("float32"))
            return {"state": "completed", "metrics": {"MaxAbsoluteError": "0"}}

        with patch.object(summarize, "file_compare", side_effect=check_official_inputs):
            row = summarize.compare_pair(pair, Path("tool.py"), out, 0, args)
        self.assertEqual(actual_path.read_bytes(), before)
        self.assertEqual(row["actual_dtype"], "float16")
        self.assertEqual(row["golden_dtype"], "float32")
        self.assertEqual(row["state"], "pass")
        self.assertEqual(row["numeric_state"], "pass")
        self.assertEqual(row["dtype_contract_state"], "pass")
        self.assertIn(
            "actual=float16，golden=float32", "\n".join(summarize.report_lines([row]))
        )
        wrong_type = {**pair, "expected_actual_dtype": "float32"}
        with patch.object(summarize, "file_compare", side_effect=check_official_inputs):
            wrong = summarize.compare_pair(wrong_type, Path("tool.py"), out, 1, args)
        self.assertEqual(wrong["numeric_state"], "pass")
        self.assertEqual(wrong["dtype_contract_state"], "mismatch")
        self.assertEqual(wrong["state"], "mismatch")

    def test_mixed_float_main_keeps_unjudged_dtype_contract(self):
        np.save(self.root / "actual-half.npy", np.array([1, 2], np.float16))
        np.save(self.root / "golden-mixed.npy", np.array([1, 2], np.float32))
        self._assert_unjudged_dtype_contract("actual-half.npy", "golden-mixed.npy")

    def test_same_dtype_without_project_expectation_is_unjudged(self):
        np.save(self.root / "actual.npy", self.array)
        self._assert_unjudged_dtype_contract("actual.npy", "golden.npy")

    def test_tolerance_requires_both_nonnegative_finite_values(self):
        for criterion in (
            {"atol": 0},
            {"atol": -1, "rtol": 0},
            {"atol": 0, "rtol": np.inf},
            {"exact": True, "atol": 0, "rtol": 0},
            {"exact": "false"},
            {"exact": 1},
        ):
            with self.subTest(criterion=criterion), self.assertRaises(ValueError):
                summarize.coordinates(self.array, self.array, criterion, 3)

    def test_tolerance_boundary_and_out_of_tolerance_coordinate(self):
        golden = np.array([0, 10, 20], np.float32)
        actual = np.array([0.25, 10.5, 20.75], np.float32)
        criterion = {"atol": 0.25, "rtol": 0.025}
        self.assertEqual(
            summarize.coordinates(actual, golden, criterion, 3)["state"], "pass"
        )
        actual[2] = 21
        result = summarize.coordinates(actual, golden, criterion, 3)
        self.assertEqual(result["mismatch_count"], 1)
        self.assertEqual(result["mismatch_fraction"], 1 / 3)
        self.assertEqual(result["top_errors"][0]["coordinate"], [2])
        self.assertEqual(result["max_absolute_error"], 1)

    def test_int64_tolerance_straddles_float64_precision_boundary(self):
        limit = 2**53
        actual = np.array([limit - 1, limit, limit + 1], np.int64)
        golden = np.zeros(3, np.int64)
        row = summarize.coordinates(actual, golden, {"atol": limit, "rtol": 0}, 3)
        self.assertEqual(row["state"], "mismatch")
        self.assertEqual(row["mismatch_count"], 1)
        self.assertEqual(row["top_errors"][0]["coordinate"], [2])
        self.assertEqual(row["top_errors"][0]["absolute_error"], limit + 1)
        self.assertEqual(row["max_absolute_error"], limit + 1)

    def test_large_integer_relative_tolerance_keeps_fractional_boundary(self):
        base = 2**53 + 1
        golden = np.array([base, base], np.int64)
        actual = np.array([base + 2**52, base + 2**52 + 1], np.int64)
        row = summarize.coordinates(actual, golden, {"atol": 0, "rtol": 0.5}, 2)
        self.assertEqual(row["mismatch_count"], 1)
        self.assertEqual(row["top_errors"][0]["coordinate"], [1])

    def test_int64_signed_extremes_keep_exact_absolute_error(self):
        actual = np.array([-(2**63)], np.int64)
        golden = np.array([2**63 - 1], np.int64)
        row = summarize.coordinates(actual, golden, {"atol": 2**64 - 2, "rtol": 0}, 1)
        self.assertEqual(row["mismatch_count"], 1)
        self.assertEqual(row["max_absolute_error"], 2**64 - 1)
        self.assertEqual(row["top_errors"][0]["absolute_error"], 2**64 - 1)

    def test_unsigned_max_and_signed_min_relative_limit(self):
        unsigned = summarize.coordinates(
            np.array([2**64 - 1], np.uint64),
            np.array([0], np.uint64),
            {"atol": 2**64 - 2, "rtol": 0},
            1,
        )
        self.assertEqual(unsigned["mismatch_count"], 1)
        self.assertEqual(unsigned["max_absolute_error"], 2**64 - 1)
        actual = np.array([0], np.int64)
        golden = np.array([-(2**63)], np.int64)
        equal = summarize.coordinates(actual, golden, {"atol": 0, "rtol": 1}, 1)
        outside = summarize.coordinates(actual, golden, {"atol": 0, "rtol": 0.5}, 1)
        self.assertEqual(equal["state"], "pass")
        self.assertEqual(outside["mismatch_count"], 1)

    def test_large_decimal_threshold_survives_pairs_json_and_report(self):
        np.save(self.root / "actual-int.npy", np.array([2**53 + 2], np.int64))
        np.save(self.root / "golden-int.npy", np.array([0], np.int64))
        tool = self.root / "official.py"
        tool.touch()
        completed = {"state": "completed", "metrics": {"MaxAbsoluteError": "1"}}
        for index, threshold in enumerate(
            ("9007199254740993.9", "9.0071992547409939e15")
        ):
            with self.subTest(threshold=threshold):
                pairs = self.root / f"integer-pairs-{index}.json"
                pairs.write_text(
                    '[{"label":"r/y","actual":"actual-int.npy",'
                    '"golden":"golden-int.npy","expected_actual_dtype":"int64",'
                    f'"criterion":{{"atol":{threshold},"rtol":0}}}}]'
                )
                out = self.root / f"integer-result-{index}"
                argv = [
                    "summarize.py",
                    "--pairs",
                    str(pairs),
                    "--out",
                    str(out),
                    "--tool",
                    str(tool),
                ]
                with (
                    patch.object(sys, "argv", argv),
                    patch.object(summarize, "file_compare", return_value=completed),
                ):
                    self.assertEqual(summarize.main(), 1)
                summary = json.loads((out / "summary.json").read_text())
                row = summary["pairs"][0]
                self.assertEqual(row["state"], "mismatch")
                self.assertEqual(row["details"]["max_absolute_error"], 2**53 + 2)
                self.assertEqual(row["details"]["mismatch_count"], 1)
                self.assertEqual(Decimal(row["criterion"]["atol"]), Decimal(threshold))

    def test_official_exit_and_metrics_both_required(self):
        kl_output = "KullbackLeiblerDivergence: 0.02\n"
        kl_metrics = {"KullbackLeiblerDivergence": "0.02"}
        cases = (
            (1, "MaxAbsoluteError: 1.0\n", "incomplete", {"MaxAbsoluteError": "1.0"}),
            (0, "", "incomplete", {}),
            (
                0,
                "MaxAbsoluteError: 1.0\nCosineSimilarity: 0.99\n",
                "completed",
                {"MaxAbsoluteError": "1.0", "CosineSimilarity": "0.99"},
            ),
            (0, kl_output, "completed", kl_metrics),
            (1, kl_output, "incomplete", kl_metrics),
        )
        for index, (code, output, state, metrics) in enumerate(cases):
            with (
                self.subTest(code=code, state=state),
                patch.object(
                    summarize.subprocess,
                    "run",
                    return_value=SimpleNamespace(
                        returncode=code, stdout=output, stderr=""
                    ),
                ),
            ):
                row = summarize.file_compare(
                    Path("tool.py"),
                    Path("a.npy"),
                    Path("g.npy"),
                    self.root / f"official-{index}",
                    1,
                )
            self.assertEqual(row["state"], state)
            self.assertEqual(row["metrics"], metrics)

    def test_official_timeout_preserves_partial_logs(self):
        error = subprocess.TimeoutExpired(
            ["tool.py"], 1, output=b"partial", stderr=b"slow"
        )
        with patch.object(summarize.subprocess, "run", side_effect=error):
            row = summarize.file_compare(
                Path("tool.py"), Path("a.npy"), Path("g.npy"), self.root / "timeout", 1
            )
        self.assertEqual(row["state"], "timeout")
        self.assertEqual(row["metrics"], {})
        self.assertEqual((self.root / row["source"]).read_bytes(), b"partial")
        self.assertEqual((self.root / "timeout/stderr.log").read_bytes(), b"slow")

    def test_official_metrics_prefer_saved_result_file(self):
        def write_result(*_args, **_kwargs):
            folder = self.root / "saved/result"
            folder.mkdir()
            (folder / "file_result_sample.txt").write_text(
                "MaxAbsoluteError: 2.437500\nCosineSimilarity: 0.949829\n"
                "KullbackLeiblerDivergence: 0.02\n"
            )
            return SimpleNamespace(
                returncode=0, stdout="MaxAbsoluteError: 999\n", stderr=""
            )

        with patch.object(summarize.subprocess, "run", side_effect=write_result):
            row = summarize.file_compare(
                Path("tool.py"), Path("a.npy"), Path("g.npy"), self.root / "saved", 1
            )
        self.assertEqual(row["state"], "completed")
        self.assertEqual(row["metrics"]["MaxAbsoluteError"], "2.437500")
        self.assertEqual(row["metrics"]["KullbackLeiblerDivergence"], "0.02")
        self.assertEqual(row["source"], "saved/result/file_result_sample.txt")

    def test_report_keeps_actual_counts_and_coordinates(self):
        detail = summarize.coordinates(
            self.array + 2, self.array, {"atol": 0, "rtol": 0}, 2
        )
        row = {"label": "r/y", "shape": [2, 2], "state": "mismatch", "details": detail}
        text = "\n".join(summarize.report_lines([row]))
        self.assertIn("| r/y | [2, 2] | 4 | mismatch | 4 | 0 / 0 | 2.0 | 2.0 |", text)
        self.assertEqual(len(detail["top_errors"]), 2)
        self.assertEqual(json.loads(text.split("```json\n")[1].split("\n```")[0]), row)

    def test_main_exit_status_and_saved_reports_agree(self):
        tool = self.root / "official.py"
        tool.touch()
        pairs = self.root / "pairs.json"
        cases = (
            (0, {"exact": True}, 0, 0, "pass"),
            (2, {"atol": 0, "rtol": 0}, 0, 1, "mismatch"),
            (2, {}, 0, 2, "unjudged"),
            (0, {"exact": True}, 1, 2, "incomplete"),
            (0, {"exact": True, "equal_nan": "false"}, 0, 2, "incomplete"),
        )
        for index, (offset, criterion, tool_code, exit_code, state) in enumerate(cases):
            with self.subTest(state=state):
                np.save(self.root / "actual.npy", self.array + offset)
                pair = {
                    "label": "r/y",
                    "actual": "actual.npy",
                    "golden": "golden.npy",
                    "expected_actual_dtype": "float32",
                    "criterion": criterion,
                }
                pairs.write_text(json.dumps([pair]))
                out = self.root / f"report-{index}"
                argv = [
                    "summarize.py",
                    "--pairs",
                    str(pairs),
                    "--out",
                    str(out),
                    "--tool",
                    str(tool),
                ]
                official = SimpleNamespace(
                    returncode=tool_code,
                    stdout=f"MaxAbsoluteError: {offset}\n",
                    stderr="",
                )
                with (
                    patch.object(sys, "argv", argv),
                    patch.object(summarize.subprocess, "run", return_value=official),
                ):
                    self.assertEqual(summarize.main(), exit_code)
                summary = json.loads((out / "summary.json").read_text())
                self.assertEqual(summary["pairs"][0]["state"], state)
                self.assertEqual(
                    summary["precision_verified"], state in {"pass", "mismatch"}
                )
                report = (out / "report.md").read_text()
                embedded = json.loads(report.split("```json\n")[1].split("\n```")[0])
                self.assertEqual(embedded, summary["pairs"][0])

    def test_interrupted_batch_keeps_completed_and_pending_pairs(self):
        out, pairs, args = self._batch_inputs()
        calls = []

        def compare(*_args):
            snapshot = json.loads((out / "summary.json").read_text())
            current = len(calls)
            self.assertFalse(snapshot["execution_complete"])
            self.assertEqual(snapshot["pairs"][current]["state"], "running")
            calls.append(current)
            if current == 2:
                self.assertEqual(
                    [r["state"] for r in snapshot["pairs"]],
                    ["pass", "pass", "running", "pending"],
                )
                raise KeyboardInterrupt
            return {"state": "completed", "metrics": {"MaxAbsoluteError": "0"}}

        with patch.object(summarize, "file_compare", side_effect=compare):
            with self.assertRaises(KeyboardInterrupt):
                summarize.run_pairs(pairs, Path("tool.py"), out, args, None)
        saved = json.loads((out / "summary.json").read_text())
        self.assertEqual(
            [r["state"] for r in saved["pairs"]],
            ["pass", "pass", "incomplete", "pending"],
        )
        self.assertFalse(saved["precision_verified"])
        self.assertFalse(saved["execution_complete"])
        self.assertEqual(saved["pairs"][2]["reason"], "interrupted")
        self.assertEqual(len(calls), 3)
        self.assertTrue((out / saved["pairs"][0]["details"]["mismatch_mask"]).is_file())

    def test_failed_pair_does_not_hide_later_results_or_pass_batch(self):
        out, pairs, args = self._batch_inputs()
        completed = {"state": "completed", "metrics": {"MaxAbsoluteError": "0"}}
        with patch.object(
            summarize,
            "file_compare",
            side_effect=[
                completed,
                {"state": "timeout", "metrics": {}},
                completed,
                completed,
            ],
        ):
            summary = summarize.run_pairs(pairs, Path("tool.py"), out, args, None)
        self.assertEqual(
            [r["state"] for r in summary["pairs"]],
            ["pass", "incomplete", "pass", "pass"],
        )
        self.assertTrue(summary["execution_complete"])
        self.assertFalse(summary["precision_verified"])

    def test_invalid_stats_are_rejected_before_official_comparison(self):
        out, _pairs, args = self._batch_inputs()
        stats = self.root / "stats.json"
        stats.write_text('{"actual_source":"tensor_summary"}')
        argv = [
            "summarize.py",
            "--pairs",
            str(args.pairs),
            "--out",
            str(out),
            "--stats",
            str(stats),
        ]
        with (
            patch.object(sys, "argv", argv),
            patch.object(summarize, "file_compare") as compare,
            self.assertLogs(summarize.LOGGER, level="ERROR") as logs,
        ):
            self.assertEqual(summarize.main(), 2)
        self.assertIn("未完成：统计汇总未声明 Runtime 原生 CSV 来源", logs.output[0])
        compare.assert_not_called()
        self.assertEqual(list(out.iterdir()), [])

    def test_cli_invalid_inputs_exit_two_without_traceback(self):
        _out, _pairs, args = self._batch_inputs()
        tool = self.root / "official.py"
        tool.touch()
        valid_pairs = args.pairs.read_text()
        cases = (
            ("pairs", "not-json"),
            ("pairs", "[null]"),
            ("pairs", None),
            ("stats", "not-json"),
            ("stats", "[]"),
            ("stats", "null"),
            ("stats", '{"actual_source":"runtime_stats_csv"}'),
            ("stats", '{"actual_source":"runtime_stats_csv","pairs":null}'),
            ("stats", '{"actual_source":"runtime_stats_csv","pairs":[null]}'),
            (
                "stats",
                '{"actual_source":"runtime_stats_csv","pairs":[{"statistics":[]}]}',
            ),
            (
                "stats",
                '{"actual_source":"runtime_stats_csv","pairs":[{"optional_unavailable":"l2norm"}]}',
            ),
            ("stats", None),
            ("tool", None),
        )
        for index, (option, content) in enumerate(cases):
            with self.subTest(option=option, content=content):
                args.pairs.write_text(valid_pairs)
                invalid = self.root / f"invalid-{index}.json"
                if content is not None:
                    invalid.write_text(content)
                options = {
                    "pairs": args.pairs,
                    "tool": tool,
                    "out": self.root / f"out-{index}",
                }
                options[option] = invalid
                command = [sys.executable, str(Path(summarize.__file__))]
                for key, value in options.items():
                    command.extend([f"--{key}", str(value)])
                result = subprocess.run(
                    command, capture_output=True, text=True, check=False
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertEqual(len(result.stderr.splitlines()), 1, result.stderr)
                self.assertTrue(result.stderr.startswith("未完成："), result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_main_report_write_error_returns_two(self):
        out, _pairs, args = self._batch_inputs()
        argv = ["summarize.py", "--pairs", str(args.pairs), "--out", str(out)]
        with (
            patch.object(sys, "argv", argv),
            patch.object(summarize, "official_tool", return_value=Path("tool.py")),
            patch.object(summarize, "write_json", side_effect=OSError("写入失败")),
            patch.object(summarize, "file_compare") as compare,
            self.assertLogs(summarize.LOGGER, level="ERROR") as logs,
        ):
            self.assertEqual(summarize.main(), 2)
        self.assertIn("未完成：写入失败", logs.output[0])
        compare.assert_not_called()

    def test_dtype_mismatch_does_not_hide_missing_numeric_criterion(self):
        _out, pairs, args = self._batch_inputs()
        pair = {**pairs[0], "expected_actual_dtype": "float16"}
        official = {"state": "completed", "metrics": {"MaxAbsoluteError": "0"}}
        for index, (criterion, code) in enumerate((({}, 2), ({"exact": True}, 1))):
            with self.subTest(criterion=criterion):
                pair["criterion"] = criterion
                args.pairs.write_text(json.dumps([pair]))
                out = self.root / f"dtype-result-{index}"
                argv = ["summarize.py", "--pairs", str(args.pairs), "--out", str(out)]
                with (
                    patch.object(sys, "argv", argv),
                    patch.object(
                        summarize, "official_tool", return_value=Path("tool.py")
                    ),
                    patch.object(summarize, "file_compare", return_value=official),
                ):
                    self.assertEqual(summarize.main(), code)
                summary = json.loads((out / "summary.json").read_text())
                self.assertEqual(
                    summary["pairs"][0]["dtype_contract_state"], "mismatch"
                )
                self.assertEqual(summary["precision_verified"], code == 1)

    def test_invalid_criterion_does_not_stop_later_pairs(self):
        out, pairs, args = self._batch_inputs()
        for index, value in enumerate(([], "invalid", None)):
            pairs[index]["criterion"] = value
        completed = {"state": "completed", "metrics": {"MaxAbsoluteError": "0"}}
        with patch.object(summarize, "file_compare", return_value=completed) as compare:
            result = summarize.run_pairs(pairs, Path("tool.py"), out, args, None)
        self.assertEqual(
            [row["state"] for row in result["pairs"]],
            ["incomplete", "incomplete", "incomplete", "pass"],
        )
        self.assertFalse(result["precision_verified"])
        self.assertEqual(compare.call_count, 1)

    def test_report_write_failure_does_not_mark_batch_complete(self):
        out, _pairs, _args = self._batch_inputs()
        rows = [{"label": "y", "state": "running"}]
        summarize.save_progress(out, rows, None)
        previous = (out / "summary.json").read_bytes()
        rows[0].update(state="pass", numeric_state="pass", dtype_contract_state="pass")
        with patch.object(
            summarize, "write_text_atomic", side_effect=OSError("写入失败")
        ):
            with self.assertRaises(OSError):
                summarize.save_progress(out, rows, None, execution_complete=True)
        self.assertEqual((out / "summary.json").read_bytes(), previous)
        self.assertFalse(json.loads(previous)["execution_complete"])

    def _batch_inputs(self):
        np.save(self.root / "actual.npy", self.array)
        pairs = [
            {
                "label": f"r{i}/y",
                "actual": "actual.npy",
                "golden": "golden.npy",
                "criterion": {"exact": True},
                "expected_actual_dtype": "float32",
            }
            for i in range(4)
        ]
        path = self.root / "pairs.json"
        path.write_text(json.dumps(pairs))
        out = self.root / "batch"
        out.mkdir()
        return out, pairs, SimpleNamespace(pairs=path, topk=1, timeout=1)

    def _assert_unjudged_dtype_contract(self, actual, golden):
        pair = {
            "label": "r/y",
            "actual": actual,
            "golden": golden,
            "criterion": {"exact": True},
        }
        pairs_path = self.root / "pairs.json"
        pairs_path.write_text(json.dumps([pair]))
        tool = self.root / "official.py"
        tool.touch()
        out = self.root / "main-result"
        argv = [
            "summarize.py",
            "--pairs",
            str(pairs_path),
            "--out",
            str(out),
            "--tool",
            str(tool),
        ]
        completed = {"state": "completed", "metrics": {"MaxAbsoluteError": "0"}}
        with (
            patch.object(sys, "argv", argv),
            patch.object(summarize, "file_compare", return_value=completed),
        ):
            self.assertEqual(summarize.main(), 2)
        summary = json.loads((out / "summary.json").read_text())
        self.assertFalse(summary["precision_verified"])
        self.assertEqual(summary["pairs"][0]["state"], "unjudged")
        self.assertEqual(summary["pairs"][0]["numeric_state"], "pass")
        self.assertEqual(summary["pairs"][0]["dtype_contract_state"], "unjudged")


if __name__ == "__main__":
    unittest.main()
