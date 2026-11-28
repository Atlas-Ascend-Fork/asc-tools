# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Check JSONL record boundaries and supported summary formats."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SKILL_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = SKILL_ROOT / "scripts" / "inspect_summary.py"
FIXTURE = SKILL_ROOT / "tests" / "fixtures" / "real-capture" / "summary.jsonl"


def inspect_content(content, output_format="json"):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "summary.jsonl"
        path.write_bytes(content.encode("utf-8"))
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(path), "--format", output_format],
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=dict(os.environ, PYTHONIOENCODING="utf-8"),
            check=False,
        )


class SummaryJsonlBoundariesTest(unittest.TestCase):
    def inspect_name(
        self, name, separator="\n", final_newline=True, ensure_ascii=False
    ):
        records = [
            json.loads(line)
            for line in FIXTURE.read_text(encoding="utf-8").splitlines()
        ]
        records[-1]["Op Name"] = name
        content = separator.join(
            json.dumps(record, ensure_ascii=ensure_ascii) for record in records
        )
        if final_newline:
            content += separator
        result = inspect_content(content)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["record_count"], len(records))
        fields = {
            field["name"]: field["value"]
            for field in output["op_info_summary"]["fields"]
        }
        self.assertEqual(fields["Op Name"], name)

    def test_raw_unicode_line_separators_remain_inside_json_strings(self):
        for character in ("\u0085", "\u2028", "\u2029"):
            with self.subTest(character=repr(character)):
                self.inspect_name("synthetic" + character + "kernel")

    def test_lf_crlf_and_missing_final_newline(self):
        for separator in ("\n", "\r\n"):
            for final_newline in (False, True):
                with self.subTest(
                    separator=repr(separator), final_newline=final_newline
                ):
                    self.inspect_name("synthetic_kernel", separator, final_newline)

    def test_escaped_newline_and_unicode_remain_inside_json_strings(self):
        self.inspect_name("synthetic\n\u2028kernel", ensure_ascii=True)


class SummaryJsonlFormatsTest(unittest.TestCase):
    def setUp(self):
        self.op_info = {
            "category": "OpInfoSummary",
            "Op Name": "synthetic_kernel",
            "Op Type": "mix",
            "Task Duration(us)": 10.0,
            "Block Dim": 4,
            "Mix Block Dim": 8,
            "Device Id": 0,
            "Pid": 1234,
            "Current Freq": "NA",
            "Rated Freq": 1650,
            "aic_flops_theoretical": 486.60,
            "aiv_flops_theoretical": 30.41,
            "aic_flops": 1.25,
            "aiv_flops": "NA",
            "aicore_parallel_utilization": 75.0,
            "aicore_parallel_balance": 90.0,
            "aicore_gm_bw_theoretical(GB/s)": "NA",
            "aicore_gm_read_bw(GB/s)": "NA",
            "aicore_gm_write_bw(GB/s)": "NA",
            "aicore_gm_bw_usage_rate(%)": "NA",
        }
        self.hints = {"category": "PerformanceHints", "hints": []}

    def inspect_records(self, *records, output_format="json"):
        return inspect_content(
            "".join(json.dumps(record) + "\n" for record in records), output_format
        )

    def test_current_op_info_explains_flops_and_parallel_percentages(self):
        result = self.inspect_records(self.hints, self.op_info)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)["op_info_summary"]
        fields = {field["name"]: field for field in output["fields"]}
        self.assertEqual(fields["aic_flops"]["unit"], "TFLOPS")
        self.assertEqual(fields["aiv_flops"]["value"], "NA")
        self.assertEqual(fields["aicore_parallel_utilization"]["unit"], "%")
        self.assertEqual(fields["aicore_parallel_balance"]["unit"], "%")
        self.assertEqual(output["selected_values"]["Mix Block Dim"], 8)

    def test_legacy_op_info_retains_null_values_and_ratio_units(self):
        result = inspect_content(FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)["op_info_summary"]
        fields = {field["name"]: field for field in output["fields"]}
        self.assertIsNone(fields["Mix Block Dim"]["value"])
        self.assertEqual(fields["aicore_parallel_balance"]["unit"], "比值")

    def test_current_pmu_fields_describe_the_na_sentinel(self):
        pipe = json.loads(FIXTURE.read_text(encoding="utf-8").splitlines()[0])
        pipe = {name: "NA" if value is None else value for name, value in pipe.items()}
        result = self.inspect_records(pipe, self.hints, self.op_info)
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = json.loads(result.stdout)["section_records"][0]["fields"]
        self.assertEqual(fields[0]["json_types"], ["number", "NA"])

    def test_roofline_data_points_and_empty_record_are_read(self):
        point = {"p_peak": 1e12, "b_peak": 1600, "w": 16, "t": 1e-8, "q": "NA"}
        for points in ({}, {"aiv_simd_b16": point}):
            with self.subTest(points=points):
                roofline = {"category": "Roofline", **points}
                result = self.inspect_records(roofline, self.hints, self.op_info)
                self.assertEqual(result.returncode, 0, result.stderr)
                output = json.loads(result.stdout)
                self.assertEqual(output["section_categories"], [])
                self.assertEqual(len(output["roofline"]["points"]), len(points))
                if points:
                    point_data = output["roofline"]["points"][0]
                    self.assertEqual(point_data["name"], "aiv_simd_b16")
                    values = {
                        field["name"]: field["value"] for field in point_data["fields"]
                    }
                    self.assertEqual(values["t"], 1e-8)
                    self.assertEqual(values["q"], "NA")

    def test_roofline_fields_are_validated(self):
        roofline = {"category": "Roofline", "aic_b32": {"p_peak": 1e12}}
        result = self.inspect_records(roofline, self.hints, self.op_info)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Roofline.aic_b32 缺少字段", result.stderr)

    def test_pm_sampling_context_preserves_decimal_strings(self):
        self.op_info["PmSamplingContexts"] = [
            {
                "replay_id": "4",
                "original_entry": "18446744073709551615",
                "companion_entry": "1",
                "pc_offset": "2",
            }
        ]
        result = self.inspect_records(self.hints, self.op_info)
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = json.loads(result.stdout)["op_info_summary"]["fields"]
        self.assertEqual(fields[-1]["name"], "PmSamplingContexts")
        self.assertEqual(
            fields[-1]["value"][0]["original_entry"], "18446744073709551615"
        )

    def test_application_launch_summary_preserves_metadata_and_reasons(self):
        self.op_info.update(
            {
                "result_unit_id": "p1234_d0_e1_k4",
                "kernel_instance_id": "k4",
                "device_epoch": 1,
                "launch_index": 2,
                "Task Id": "NA",
                "Stream Id": 3,
                "communication_time": "NA",
                "compute_communication_overlap_time": "NA",
                "compute_overhead_time": "NA",
                "communication_semantics": "api_call",
                "collection_status": "partial",
                "unavailable_reasons": {
                    "communication_time": "communication_intervals_not_parsed",
                    "aiv_flops": "missing_task_identity",
                },
            }
        )
        result = self.inspect_records(self.op_info)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertIsNone(output["performance_hints"])
        fields = {
            field["name"]: field["value"]
            for field in output["op_info_summary"]["fields"]
        }
        self.assertEqual(fields["launch_index"], 2)
        self.assertEqual(fields["Task Id"], "NA")
        self.assertEqual(
            fields["unavailable_reasons"]["aiv_flops"], "missing_task_identity"
        )


if __name__ == "__main__":
    unittest.main()
