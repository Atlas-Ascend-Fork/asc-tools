# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""JSONL records are separated by LF, not Unicode characters in JSON strings."""

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
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.jsonl"
            path.write_bytes(content.encode("utf-8"))
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(path), "--format", "json"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                env=dict(os.environ, PYTHONIOENCODING="utf-8"),
                check=False,
            )
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


if __name__ == "__main__":
    unittest.main()
