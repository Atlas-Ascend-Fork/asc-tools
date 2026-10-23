# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Issue #97: pipeline and track totals retain small finite durations."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


INSPECT_SCRIPT = Path(__file__).resolve().parents[1] / "scripts/inspect_pipe_trace.py"


class PipeTracePrecisionTest(unittest.TestCase):
    def test_duration_totals_preserve_small_events_in_either_order(self):
        for durations in ((1e16, 1.0, 1.0), (1.0, 1.0, 1e16)):
            with self.subTest(durations=durations):
                document = {
                    "displayTimeUnit": "ns",
                    "profilingType": "op",
                    "schemaVersion": 1,
                    "traceEvents": [
                        {
                            "cname": "yellow",
                            "dur": duration,
                            "name": "MTE2",
                            "ph": "X",
                            "pid": "group0.veccore0",
                            "tid": "MTE2",
                            "ts": 0.0,
                        }
                        for duration in durations
                    ],
                }
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "PipeTrace.json"
                    path.write_text(json.dumps(document), encoding="utf-8")
                    result = subprocess.run(
                        [
                            sys.executable,
                            "-X",
                            "utf8",
                            str(INSPECT_SCRIPT),
                            str(path),
                            "--format",
                            "json",
                        ],
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        check=False,
                    )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                output = json.loads(result.stdout)
                for collection in ("pipelines", "tracks"):
                    with self.subTest(collection=collection):
                        self.assertEqual(len(output[collection]), 1)
                        self.assertEqual(output[collection][0]["event_count"], 3)
                        self.assertEqual(
                            output[collection][0]["duration_sum_us"],
                            10000000000000002.0,
                        )
