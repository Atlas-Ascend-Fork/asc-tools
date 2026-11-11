#!/usr/bin/env python3
# -*- coding: UTF-8 -*-
# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Exercise knowledge maintenance without a source checkout or a CANN install."""

import importlib.util
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

SKILL_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = SKILL_ROOT / "scripts/update_kb_manifest.py"
SPEC = importlib.util.spec_from_file_location("compile_debug_kb_maintenance", SCRIPT)
MAINTENANCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MAINTENANCE)


def entry_yaml(name, signature="void Call()"):
    return (
        f'- name: {name}\n  signature: "{signature}"\n'
        "  header: kernel_operator.h\n  kind: function\n"
        '  api_level: basic\n  category: test\n  brief: ""\n'
    ).encode("utf-8")


class KnowledgeMaintenanceTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="compile-debug-maintenance-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.first = self.directory / "part-0001.yaml"
        self.second = self.directory / "part-0002.yaml"
        self.first.write_bytes(entry_yaml("ApiOne", "ApiOne(int)"))
        self.second.write_bytes(entry_yaml("ApiTwo"))
        manifest = MAINTENANCE.build_manifest(self.directory, 2, 2)
        MAINTENANCE.write_manifest(self.directory, manifest)
        self.manifest = self.directory / "manifest.json"
        self.baseline = self.manifest.read_bytes()

    def invoke(self, *arguments):
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--directory",
                str(self.directory),
                *arguments,
            ],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def test_default_check_is_read_only(self):
        before = {p.name: p.read_bytes() for p in self.directory.iterdir()}
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            before, {p.name: p.read_bytes() for p in self.directory.iterdir()}
        )

    def test_failed_write_keeps_manifest_and_removes_temporary_file(self):
        original = MAINTENANCE.tempfile.NamedTemporaryFile

        def fail_on_write(*args, **kwargs):
            stream = original(*args, **kwargs)
            stream.write = Mock(side_effect=OSError(28, "simulated disk full"))
            return stream

        with patch.object(
            MAINTENANCE.tempfile, "NamedTemporaryFile", side_effect=fail_on_write
        ):
            with self.assertRaisesRegex(OSError, "simulated disk full"):
                MAINTENANCE.write_manifest(self.directory, {"entry_count": 2})
        self.assertEqual(self.manifest.read_bytes(), self.baseline)
        self.assertEqual(list(self.directory.glob("*.tmp")), [])

    def test_failed_replace_keeps_manifest_and_removes_temporary_file(self):
        with patch.object(
            MAINTENANCE.os, "replace", side_effect=PermissionError("denied")
        ):
            with self.assertRaises(PermissionError):
                MAINTENANCE.write_manifest(self.directory, {"entry_count": 2})
        self.assertEqual(self.manifest.read_bytes(), self.baseline)
        self.assertEqual(list(self.directory.glob("*.tmp")), [])

    def test_reviewed_edit_can_be_rebuilt_without_rewriting_yaml(self):
        content = entry_yaml("ApiOne", "ApiOne(float)") + entry_yaml("ApiThree")
        self.first.write_bytes(content)
        self.assertEqual(self.invoke("--check").returncode, 1)
        result = self.invoke(
            "--write", "--expected-parts", "2", "--expected-entries", "3"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.invoke("--check").returncode, 0)
        self.assertEqual(self.first.read_bytes(), content)
        self.assertEqual(json.loads(self.manifest.read_bytes())["entry_count"], 3)

    def test_missing_shard_cannot_be_accepted_with_expected_inventory(self):
        self.second.unlink()
        result = self.invoke(
            "--write", "--expected-parts", "2", "--expected-entries", "2"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing=", result.stderr)
        self.assertEqual(self.manifest.read_bytes(), self.baseline)

    def test_extra_shard_requires_explicit_new_counts(self):
        (self.directory / "part-0003.yaml").write_bytes(entry_yaml("ApiThree"))
        rejected = self.invoke(
            "--write", "--expected-parts", "2", "--expected-entries", "2"
        )
        self.assertEqual(rejected.returncode, 1)
        self.assertEqual(self.manifest.read_bytes(), self.baseline)
        accepted = self.invoke(
            "--write", "--expected-parts", "3", "--expected-entries", "3"
        )
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertEqual(self.invoke().returncode, 0)

    def test_wrong_total_does_not_replace_manifest(self):
        result = self.invoke(
            "--write", "--expected-parts", "2", "--expected-entries", "3"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("entry count mismatch", result.stderr)
        self.assertEqual(self.manifest.read_bytes(), self.baseline)

    def test_invalid_source_is_rejected_before_write(self):
        for content in (
            b"- name: ApiOne\n  name: Replaced\n",
            b"- name: ''\n",
            b"name: NotAList\n",
            b"- [broken\n",
            b"- name: ApiOne\r\n",
            b"- name: ApiOne\n#" + b"x" * MAINTENANCE.SHARD_MAX_BYTES,
        ):
            with self.subTest(content=content[:60]):
                self.first.write_bytes(content)
                result = self.invoke(
                    "--write", "--expected-parts", "2", "--expected-entries", "2"
                )
                self.assertEqual(result.returncode, 1)
                self.assertEqual(self.manifest.read_bytes(), self.baseline)

    def test_write_requires_explicit_positive_inventory(self):
        for arguments in (
            ("--write",),
            ("--write", "--expected-parts", "2"),
            ("--write", "--expected-parts", "0", "--expected-entries", "2"),
            ("--check", "--expected-entries", "2"),
        ):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.invoke(*arguments).returncode, 2)
                self.assertEqual(self.manifest.read_bytes(), self.baseline)

    def test_truncated_record_with_unchanged_count_cannot_be_accepted(self):
        self.second.write_bytes(b"- name: ApiTwo\n")
        result = self.invoke(
            "--write", "--expected-parts", "2", "--expected-entries", "2"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("signature", result.stderr)
        self.assertEqual(self.manifest.read_bytes(), self.baseline)

    def test_required_fields_must_keep_their_value_types(self):
        self.second.write_bytes(
            entry_yaml("ApiTwo").replace(b"header: kernel_operator.h", b"header: []")
        )
        result = self.invoke(
            "--write", "--expected-parts", "2", "--expected-entries", "2"
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("header", result.stderr)
        self.assertEqual(self.manifest.read_bytes(), self.baseline)

    @unittest.skipIf(os.name == "nt", "POSIX file modes")
    def test_manifest_update_preserves_read_permissions(self):
        self.manifest.chmod(0o644)
        result = self.invoke(
            "--write", "--expected-parts", "2", "--expected-entries", "2"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(stat.S_IMODE(self.manifest.stat().st_mode), 0o644)


if __name__ == "__main__":
    unittest.main()
