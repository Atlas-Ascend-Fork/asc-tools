# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SKILL_ROOT = Path(__file__).resolve().parents[1]
HARDWARE_FIXTURE = (
    SKILL_ROOT / "tests" / "fixtures" / "real-capture" / "HardwareInfo.jsonl"
)


def run_inspector(script, contents):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "report"
        path.write_text(contents, encoding="utf-8")
        return subprocess.run(
            [
                sys.executable,
                str(SKILL_ROOT / "scripts" / script),
                str(path),
                "--format",
                "json",
            ],
            capture_output=True,
            text=True,
            check=False,
        )


def hardware_records(device_id):
    records = [
        json.loads(line)
        for line in HARDWARE_FIXTURE.read_text(encoding="utf-8").splitlines()
    ]
    for index, record in enumerate(records[1:], start=1):
        category = record.pop("category")
        records[index] = {"category": category, "Device Id": device_id, **record}
    return records


def inspect_hardware(records):
    return run_inspector(
        "inspect_hardware_info.py",
        "".join(json.dumps(record) + "\n" for record in records),
    )


class CurrentHardwareInfoTest(unittest.TestCase):
    def test_kernel_snapshot_reports_its_device_id(self):
        result = inspect_hardware(hardware_records(0))
        self.assertEqual(result.returncode, 0, result.stderr)
        categories = json.loads(result.stdout)["categories"]
        self.assertEqual(len(categories), 5)
        for category in categories[1:]:
            self.assertEqual(category["fields"][1]["name"], "Device Id")
            self.assertEqual(category["fields"][1]["value"], 0)

    def test_application_snapshots_preserve_each_device(self):
        result = inspect_hardware(hardware_records(5) + hardware_records(0))
        self.assertEqual(result.returncode, 0, result.stderr)
        inspection = json.loads(result.stdout)
        self.assertEqual(inspection["record_count"], 10)
        self.assertEqual(
            [
                category["fields"][1]["value"]
                for category in inspection["categories"]
                if category["name"] == "AI Core Information"
            ],
            [5, 0],
        )

    def test_application_snapshot_requires_complete_categories_per_device(self):
        result = inspect_hardware(hardware_records(0) + hardware_records(1)[:-1])
        self.assertEqual(result.returncode, 2)
        self.assertIn("Device Id 1 缺少类别：Memory Information", result.stderr)

    def test_application_snapshot_rejects_a_device_record_from_another_group(self):
        records = hardware_records(0) + hardware_records(1)
        records[1], records[6] = records[6], records[1]
        result = inspect_hardware(records)
        self.assertEqual(result.returncode, 2)
        self.assertIn("同一快照的 Device Id 不一致", result.stderr)


class PipeTraceDfxTest(unittest.TestCase):
    def test_dfx_events_extend_the_time_range_without_inflating_busy_time(self):
        document = {
            "displayTimeUnit": "ns",
            "profilingType": "op",
            "schemaVersion": 1,
            "traceEvents": [
                {
                    "cname": "rail_idle",
                    "dur": 0.25,
                    "name": "VECTOR",
                    "ph": "X",
                    "pid": "group0.veccore0",
                    "tid": "VECTOR",
                    "ts": 2,
                },
                {
                    "cname": "rail_idle",
                    "dur": 0.001,
                    "name": "MarkStamp1031",
                    "ph": "X",
                    "pid": "group0.veccore0",
                    "tid": "VECTOR",
                    "ts": 1,
                },
                {
                    "cname": "cyan",
                    "dur": 2,
                    "name": "Region7",
                    "ph": "X",
                    "pid": "group0.veccore0",
                    "tid": "VECTOR",
                    "ts": 1,
                },
            ],
        }
        result = run_inspector("inspect_pipe_trace.py", json.dumps(document))
        self.assertEqual(result.returncode, 0, result.stderr)
        inspection = json.loads(result.stdout)
        self.assertEqual(inspection["event_count"], 3)
        self.assertEqual(inspection["dfx_event_count"], 2)
        self.assertEqual(inspection["trace_start_us"], 1)
        self.assertEqual(inspection["trace_end_us"], 3)
        self.assertEqual(
            inspection["pipelines"],
            [{"name": "VECTOR", "event_count": 1, "duration_sum_us": 0.25}],
        )
        self.assertEqual(inspection["tracks"][0]["event_count"], 3)
        self.assertEqual(inspection["tracks"][0]["duration_sum_us"], 0.25)
