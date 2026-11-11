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
"""脚本入口测试。

用途：验证包内脚本可启动、入口帮助可用、shell 包装器语法合法。
使用方法：python3 -m unittest discover -s tests   （在技能根目录执行）
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SKILL_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = SKILL_ROOT / "scripts"


def run_python(args, timeout=120):
    environment = dict(os.environ)
    for name in ("PYTHONDONTWRITEBYTECODE", "PYTHONPYCACHEPREFIX"):
        environment.pop(name, None)
    return subprocess.run(
        [sys.executable, *args],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=environment,
    )


def copy_package(destination):
    """Copy release inputs without inheriting the test runner's caches."""
    return shutil.copytree(
        SKILL_ROOT,
        destination,
        ignore=shutil.ignore_patterns(".git", "__pycache__", ".pytest_cache", "*.pyc"),
    )


class ScriptEntryTest(unittest.TestCase):
    def _verify_with_stub(self, arguments, payload):
        """Exercise wrapper and bundled CLI parsing, replacing only compilation."""
        with tempfile.TemporaryDirectory(prefix="compile-debug-payload-") as raw:
            payload_path = Path(raw) / "payload.json"
            payload_path.write_text(json.dumps(payload), encoding="utf-8")
            code = f"""
import json
import runpy
import sys
from pathlib import Path
from unittest.mock import patch
scope = runpy.run_path({str(SCRIPTS / "run.py")!r})
scope['_ensure_engine_on_path']()
from harness.engine import verify_runner
sys.platform = 'linux'
payload = json.loads(Path({str(payload_path)!r}).read_text(encoding='utf-8'))
with patch.object(verify_runner, 'verify', return_value=payload) as build:
    result = scope['main']({["verify", *arguments]!r})
    if build.called:
        print('ENGINE_ARGS=' + json.dumps({{
            'case': str(build.call_args.args[0]),
            'calls': build.call_count,
            **{{
            key: build.call_args.kwargs[key]
            for key in ('cmake_args', 'build_target', 'soc')
            }},
        }}))
    raise SystemExit(result)
"""
            return run_python(["-c", code])

    def _read_summary_without_engine(self, arguments):
        code = f"""
import runpy
from unittest.mock import patch
scope = runpy.run_path({str(SCRIPTS / "run.py")!r})
def forbidden(*args, **kwargs):
    raise AssertionError('saved report reading must not import or invoke the engine')
with patch.dict(scope['main'].__globals__, {{
    '_ensure_engine_on_path': forbidden, '_dispatch': forbidden,
}}):
    raise SystemExit(scope['main']({["verify", *arguments]!r}))
"""
        return run_python(["-c", code])

    def test_verify_mixed_output_form_creates_nested_report_once(self):
        for output_equals in (False, True):
            with self.subTest(output_equals=output_equals):
                with tempfile.TemporaryDirectory(prefix="compile-debug-mixed-") as raw:
                    project = Path(raw) / "project"
                    project.mkdir()
                    output = Path(raw) / "new-reports" / "round-1" / "result.json"
                    flags = (
                        ["--json-out=" + str(output)]
                        if output_equals
                        else ["--json-out", str(output)]
                    )
                    payload = {
                        "transition": "verification_not_run",
                        "build_invoked": False,
                        "reason": "soc_requires_sku",
                    }
                    result = self._verify_with_stub([str(project), *flags], payload)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    forwarded = json.loads(result.stdout.split("ENGINE_ARGS=", 1)[1])
                    self.assertEqual(forwarded["case"], str(project.resolve()))
                    self.assertEqual(forwarded["calls"], 1)
                    self.assertEqual(
                        json.loads(output.read_text(encoding="utf-8")), payload
                    )
                    self.assertEqual(result.stdout.splitlines()[0], str(output))

    def test_verify_rejects_conflicting_path_forms_before_build(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-conflict-") as raw:
            output = Path(raw) / "result.json"
            variants = [
                [raw, "--case", raw, "--json-out", str(output)],
                [raw, str(output), "--json-out=" + str(output)],
                ["--case", raw, "--case=" + raw, "--json-out", str(output)],
                ["--case", raw, "--json-out", str(output), "--json-out=" + str(output)],
                [raw],
            ]
            for arguments in variants:
                with self.subTest(arguments=arguments):
                    result = self._verify_with_stub(
                        arguments, {"transition": "unexpected"}
                    )
                    self.assertEqual(result.returncode, 64, result.stderr)
                    self.assertNotIn("ENGINE_ARGS=", result.stdout)
                    self.assertFalse(output.exists())

    def test_verify_compact_summary_keeps_verdict_and_validation_gaps(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-receipt-") as raw:
            project = Path(raw) / "project"
            project.mkdir()
            output = Path(raw) / "docs/reports/result.json"
            payload = {
                "transition": "case_not_reproducible",
                "reason": "compilation_of_requested_sources_and_architecture_unconfirmed",
                "build_invoked": True,
                "dry_run": False,
                "after": {
                    "returncode": 0,
                    "log_path": "actual/verify_before.log",
                    "recipe_cwd": "actual/project",
                },
                "built_target": ["host_library", "tiling_library"],
                "expected_target": "device_binary",
                "built_arch": None,
                "compilation_evidence": {
                    "status": "unconfirmed",
                    "reason": "fresh_compilation_of_required_sources_not_observed",
                    "records": [
                        {"dependency_sha256": {"header": "evidence-hash-" * 5000}}
                    ],
                },
                "target_validation": {
                    "status": "unconfirmed",
                    "requested_arch": "2201",
                    "observed_architectures": [],
                    "prior_failure_coverage": {
                        "status": "unconfirmed",
                        "reason": "previous_failure_source_not_observed_in_current_dependencies",
                        "required_files": ["host.cpp"],
                        "missing_files": ["host.cpp"],
                        "unresolved_files": [],
                        "reanchored_files": [],
                    },
                },
                "contract_check": {"status": "not_applicable"},
                "validation_scope": {
                    "compile": "unconfirmed",
                    "contract": "not_checked",
                    "numerical": "not_checked",
                },
            }
            result = self._verify_with_stub([str(project), str(output)], payload)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(result.stderr.split("[verify] ", 1)[1])
            self.assertEqual(summary["schema_version"], "verify-summary-v1")
            self.assertEqual(summary["transition"], payload["transition"])
            self.assertEqual(summary["reason"], payload["reason"])
            self.assertEqual(summary["build_returncode"], 0)
            self.assertEqual(summary["built_target"], payload["built_target"])
            self.assertEqual(summary["expected_target"], "device_binary")
            self.assertIsNone(summary["built_arch"])
            self.assertEqual(
                summary["prior_failure_coverage"]["missing_files_count"], 1
            )
            self.assertEqual(summary["validation_scope"], payload["validation_scope"])
            self.assertNotIn("evidence-hash", result.stderr)
            self.assertLess(len(result.stderr), 2500)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), payload)

            original = output.read_bytes()
            original_time = output.stat().st_mtime_ns
            original_files = sorted(str(p) for p in Path(raw).rglob("*"))
            output.chmod(0o444)
            try:
                replay = self._read_summary_without_engine(["--summary", str(output)])
            finally:
                output.chmod(0o666)
            self.assertEqual(replay.returncode, 0, replay.stderr)
            self.assertEqual(json.loads(replay.stdout), summary)
            self.assertEqual(output.read_bytes(), original)
            self.assertEqual(output.stat().st_mtime_ns, original_time)
            self.assertEqual(
                sorted(str(p) for p in Path(raw).rglob("*")), original_files
            )

    def test_verify_summary_preserves_escape_warning_with_fixed_and_contract_ok(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-warning-") as raw:
            project = Path(raw) / "project"
            project.mkdir()
            output = Path(raw) / "result.json"
            payload = {
                "transition": "compile_failure_fixed",
                "after": {"returncode": 0},
                "build_invoked": True,
                "contract_check": {"status": "ok"},
                "validation_scope": {
                    "compile": "confirmed",
                    "contract": "ok",
                    "numerical": "not_checked",
                },
                "escape_suspect": {
                    "check": "original_error_symbols_presence",
                    "missing_symbols": ["original_symbol_one", "original_symbol_two"],
                    "evidence_log": "actual/original-build.log",
                    "advice": "long-advice-not-for-summary " * 2000,
                },
            }
            result = self._verify_with_stub([str(project), str(output)], payload)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(result.stderr.split("[verify] ", 1)[1])
            self.assertEqual(summary["transition"], "compile_failure_fixed")
            self.assertEqual(summary["contract_check"]["status"], "ok")
            self.assertEqual(
                summary["escape_suspect"],
                {
                    "check": "original_error_symbols_presence",
                    "missing_symbols_count": 2,
                    "evidence_log": "actual/original-build.log",
                },
            )
            self.assertNotIn("original_symbol_one", result.stderr)
            self.assertNotIn("long-advice-not-for-summary", result.stderr)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), payload)
            replay = self._read_summary_without_engine(["--summary", str(output)])
            self.assertEqual(replay.returncode, 0, replay.stderr)
            self.assertEqual(json.loads(replay.stdout), summary)

    def test_saved_summary_keeps_unknown_fields_and_contract_violation(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-old-report-") as raw:
            output = Path(raw) / "old.json"
            payload = {
                "transition": "compile_passed",
                "contract_check": {"status": "violated"},
            }
            output.write_text(json.dumps(payload), encoding="utf-8")
            result = self._read_summary_without_engine(["--summary=" + str(output)])
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(result.stdout)
            self.assertEqual(summary["contract_check"]["status"], "violated")
            self.assertIsNone(summary["build_invoked"])
            self.assertIsNone(summary["build_returncode"])
            self.assertIsNone(summary["escape_suspect"])
            self.assertIsNone(summary["validation_scope"]["compile"])
            self.assertIsNone(summary["validation_scope"]["numerical"])
            self.assertIsNone(summary["prior_failure_coverage"]["missing_files_count"])
            self.assertEqual(sorted(p.name for p in Path(raw).iterdir()), ["old.json"])

    def test_saved_summary_rejects_missing_malformed_and_mixed_inputs(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-invalid-report-") as raw:
            output = Path(raw) / "saved.json"
            missing = self._read_summary_without_engine(["--summary", str(output)])
            self.assertEqual(missing.returncode, 2)
            for payload in (
                "not json",
                "[]",
                "{}",
                '{"transition":"compile_passed","after":[]}',
                '{"transition":"compile_passed","build_invoked":"true"}',
                '{"transition":"compile_passed","built_target":{"records":[]}}',
            ):
                with self.subTest(payload=payload):
                    output.write_text(payload, encoding="utf-8")
                    result = self._read_summary_without_engine(
                        ["--summary", str(output)]
                    )
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertFalse(result.stdout)
                    self.assertEqual(output.read_text(encoding="utf-8"), payload)
            for arguments in (
                ["--summary"],
                ["--summary="],
                ["--summary", str(output), "--case", raw],
                [raw, "--summary", str(output)],
            ):
                with self.subTest(arguments=arguments):
                    result = self._read_summary_without_engine(arguments)
                    self.assertEqual(result.returncode, 64, result.stderr)
                    self.assertFalse(result.stdout)

    def test_verify_cmake_values_reach_engine_without_splitting(self):
        values = ["-DOPTION=ON", "-DLABEL:STRING=hello world;-DLITERAL=value"]
        for flag_form in (False, True):
            with self.subTest(flag_form=flag_form):
                with tempfile.TemporaryDirectory(prefix="compile-debug-argv-") as raw:
                    project = Path(raw) / "project"
                    project.mkdir()
                    output = Path(raw) / "report.json"
                    arguments = (
                        ["--case", str(project), "--json-out", str(output)]
                        if flag_form
                        else [str(project), str(output)]
                    )
                    arguments += [
                        "--target",
                        "original_target",
                        "--soc",
                        "Ascend910B1",
                        "--cmake-arg",
                        values[0],
                        "--cmake-arg=" + values[1],
                    ]
                    payload = {
                        "transition": "new_compile_error",
                        "after": {
                            "returncode": 2,
                            "log_path": str(Path(raw) / "actual.log"),
                        },
                        "compilation_evidence": {"status": "unconfirmed"},
                    }
                    result = self._verify_with_stub(arguments, payload)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    forwarded = json.loads(result.stdout.split("ENGINE_ARGS=", 1)[1])
                    self.assertEqual(forwarded["cmake_args"], values)
                    self.assertEqual(forwarded["build_target"], "original_target")
                    self.assertEqual(forwarded["soc"], "Ascend910B1")
                    self.assertEqual(
                        json.loads(output.read_text(encoding="utf-8")), payload
                    )
                    self.assertEqual(result.stdout.splitlines()[0], str(output))
                    summary = json.loads(result.stderr.split("[verify] ", 1)[1])
                    self.assertEqual(summary["transition"], "new_compile_error")
                    self.assertEqual(summary["build_returncode"], 2)
                    self.assertEqual(summary["log_path"], payload["after"]["log_path"])
                    self.assertEqual(summary["compilation_evidence"], "unconfirmed")
                    self.assertIsNone(summary["recipe_cwd"])

    def test_verify_rejects_ambiguous_or_missing_arguments_before_build(self):
        invalid_arguments = [
            (["--cmake-args", "-DONE=1 -DTWO=2"], 64, "Repeat --cmake-arg"),
            (["--cmake-args=-DONE=1"], 64, "Repeat --cmake-arg"),
            (["--cmake-arg", "--target", "kept"], 2, "expected one argument"),
            (["--target", "one", "--target", "two"], 2, "only once"),
        ]
        for flags, expected_code, message in invalid_arguments:
            with self.subTest(flags=flags):
                with tempfile.TemporaryDirectory(prefix="compile-debug-reject-") as raw:
                    output = Path(raw) / "result.json"
                    result = self._verify_with_stub(
                        [raw, str(output), *flags], {"transition": "unexpected"}
                    )
                    self.assertEqual(result.returncode, expected_code, result.stderr)
                    self.assertIn(message, result.stderr)
                    self.assertNotIn("ENGINE_ARGS=", result.stdout)
                    self.assertNotIn("[verify]", result.stderr)
                    self.assertFalse(output.exists())

    def test_verify_summary_preserves_absent_build_and_fallback_log(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-summary-") as raw:
            output = Path(raw) / "result.json"
            payload = {
                "transition": "verification_not_run",
                "after": {},
                "logs": {"after": str(Path(raw) / "verification_not_run.log")},
            }
            result = self._verify_with_stub([raw, str(output)], payload)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(result.stderr.split("[verify] ", 1)[1])
            self.assertEqual(summary["transition"], "verification_not_run")
            self.assertIsNone(summary["build_returncode"])
            self.assertIsNone(summary["compilation_evidence"])
            self.assertEqual(summary["log_path"], payload["logs"]["after"])

    def test_run_py_help_lists_diagnose(self):
        result = run_python([str(SCRIPTS / "run.py"), "--help"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("diagnose", result.stdout)

    def test_preflight_reports_json(self):
        result = run_python([str(SCRIPTS / "preflight.py"), "--json"])
        # rc 契约：成功为 0，其他失败为 1，平台不支持为 2；均输出合法 JSON。
        self.assertIn(result.returncode, (0, 1, 2), result.stderr)
        payload = json.loads(result.stdout)
        self.assertIn("ok", payload)
        if payload["ok"] is False:
            self.assertTrue(payload.get("errors"))

    def test_preflight_unsupported_platform_exit_is_format_independent(self):
        script = str(SCRIPTS / "preflight.py")
        for flags in ([], ["--json"]):
            with self.subTest(flags=flags):
                code = (
                    "import runpy, sys; "
                    f"sys.argv = {[script, *flags]!r}; "
                    "sys.platform = 'unsupported-test'; "
                    "runpy.run_path(sys.argv[0], run_name='__main__')"
                )
                result = run_python(["-c", code])
                self.assertEqual(result.returncode, 2, result.stderr)
                if flags:
                    payload = json.loads(result.stdout)
                    self.assertFalse(payload["ok"])
                    self.assertTrue(
                        any("Unsupported platform" in e for e in payload["errors"])
                    )
                else:
                    self.assertIn("Unsupported platform", result.stdout)

    def test_selfcheck_inventory_covers_all_runtime_and_top_level_knowledge(self):
        import runpy

        # The entrypoint deliberately protects its own installation. Its
        # process-wide cache setting must not escape into this test runner.
        with patch.object(sys, "dont_write_bytecode", sys.dont_write_bytecode):
            scope = runpy.run_path(str(SCRIPTS / "face_selfcheck.py"))
        required = set(scope["REQUIRED_FILES"])
        runtime = {
            path.relative_to(SKILL_ROOT).as_posix()
            for pattern in ("*.py", "*.sh")
            for path in SCRIPTS.rglob(pattern)
        }
        knowledge = {
            path.relative_to(SKILL_ROOT).as_posix()
            for path in (SCRIPTS / "engine/harness/knowledge").glob("*.yaml")
            if path.name != "api_signatures.yaml"
        }
        # The API KB deliberately supports either a single file or shards;
        # dedicated tests verify both layouts and incomplete shard rejection.
        self.assertEqual((runtime | knowledge) - required, set())

    def test_selfcheck_and_preflight_reject_missing_lazy_capabilities(self):
        for relative in (
            "scripts/engine/harness/engine/inspect_context.py",
            "scripts/engine/harness/engine/repair_memory.py",
            "scripts/engine/harness/knowledge/compile_mechanisms.yaml",
        ):
            with self.subTest(missing=relative):
                with tempfile.TemporaryDirectory(
                    prefix="compile-debug-missing-capability-"
                ) as raw:
                    package = Path(raw) / "tool-compile-debug"
                    copy_package(package)
                    (package / relative).unlink()
                    for script in ("face_selfcheck.py", "preflight.py"):
                        entry = str(package / "scripts" / script)
                        args = [entry, "--json"]
                        if script == "preflight.py" and sys.platform != "linux":
                            # The CLI correctly rejects this platform first.
                            # Exercise its real installation check separately;
                            # Linux acceptance also exercises the whole CLI.
                            args = [
                                "-c",
                                "import json, runpy; "
                                f"scope = runpy.run_path({entry!r}); "
                                "checks = scope['_base_checks'](); "
                                "ok = scope['_check_installation'](checks); "
                                "print(json.dumps(checks)); "
                                "raise SystemExit(0 if ok else 1)",
                            ]
                        result = run_python(args)
                        payload = json.loads(result.stdout)
                        self.assertEqual(result.returncode, 1, payload)
                        self.assertIn(
                            f"missing required file: {relative}", payload["errors"]
                        )

    def test_face_selfcheck_ok(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-clean-") as raw:
            package = Path(raw) / "tool-compile-debug"
            copy_package(package)
            result = run_python(
                [str(package / "scripts" / "face_selfcheck.py"), "--json"]
            )
            payload = json.loads(result.stdout)
            self.assertEqual(result.returncode, 0, payload["errors"])
            self.assertTrue(payload["ok"], payload["errors"])
            self.assertRegex(payload["skill_version"], r"^[0-9]+\.[0-9]+\.[0-9]+$")

    def test_face_selfcheck_rejects_bytecode_in_package(self):
        for relative in ("tests/__pycache__/probe.pyc", "scripts/stray.pyc"):
            with self.subTest(path=relative):
                with tempfile.TemporaryDirectory(
                    prefix="compile-debug-bytecode-"
                ) as raw:
                    package = Path(raw) / "tool-compile-debug"
                    copy_package(package)
                    artifact = package / relative
                    artifact.parent.mkdir(parents=True, exist_ok=True)
                    artifact.write_bytes(b"bytecode pollution")
                    result = run_python(
                        [str(package / "scripts" / "face_selfcheck.py"), "--json"]
                    )
                    payload = json.loads(result.stdout)
                    self.assertEqual(result.returncode, 1, payload["errors"])
                    self.assertFalse(payload["ok"])
                    self.assertTrue(
                        any("bytecode artifact" in e for e in payload["errors"])
                    )

    def test_face_selfcheck_reads_and_validates_release_version(self):
        import yaml

        text = (SKILL_ROOT / "SKILL.md").read_text(encoding="utf-8")
        _, header, body = text.split("---", 2)
        for metadata, expected in (
            ({"version": "1.0"}, "1.0"),
            ({"version": "2.7.3"}, "2.7.3"),
            ({}, None),
            ({"version": 1.0}, None),
            ({"version": "1.x"}, None),
            ([], None),
        ):
            with self.subTest(metadata=metadata):
                with tempfile.TemporaryDirectory(
                    prefix="compile-debug-version-"
                ) as raw:
                    package = Path(raw) / "tool-compile-debug"
                    copy_package(package)
                    document = yaml.safe_load(header)
                    document["metadata"] = metadata
                    (package / "SKILL.md").write_text(
                        "---\n"
                        + yaml.safe_dump(document, allow_unicode=True)
                        + "---"
                        + body,
                        encoding="utf-8",
                    )
                    result = run_python(
                        [str(package / "scripts" / "face_selfcheck.py"), "--json"]
                    )
                    payload = json.loads(result.stdout)
                    self.assertEqual(result.returncode, 0 if expected else 1, payload)
                    self.assertEqual(payload["skill_version"], expected)
                    if expected is None:
                        self.assertTrue(
                            any("metadata.version" in e for e in payload["errors"])
                        )

    def test_face_selfcheck_excludes_git_metadata_but_checks_other_hidden_files(self):
        for git_directory in (False, True):
            with self.subTest(git_directory=git_directory):
                with tempfile.TemporaryDirectory(prefix="compile-debug-git-") as raw:
                    package = Path(raw) / "tool-compile-debug"
                    copy_package(package)
                    metadata = package / ".git"
                    if git_directory:
                        metadata.mkdir()
                        (metadata / "FETCH_HEAD").write_bytes(b"")
                        (metadata / "hook.pyc").write_bytes(b"not a package artifact")
                    else:
                        metadata.write_bytes(b"gitdir: ../repository/worktrees/skill\n")
                    args = [str(package / "scripts" / "face_selfcheck.py"), "--json"]
                    result = run_python(args)
                    payload = json.loads(result.stdout)
                    self.assertEqual(result.returncode, 0, payload["errors"])
                    self.assertTrue(payload["ok"], payload["errors"])
                    self.assertFalse(
                        any(Path(name).parts[0] == ".git" for name in payload["files"])
                    )

                    (package / ".empty-package-file").write_bytes(b"")
                    result = run_python(args)
                    payload = json.loads(result.stdout)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(
                        "empty or whitespace-only package file: .empty-package-file",
                        payload["errors"],
                    )

    def test_face_selfcheck_rejects_empty_and_whitespace_files(self):
        paths = (
            "scripts/engine/__init__.py",
            "scripts/engine/harness/__init__.py",
            "scripts/engine/harness/engine/__init__.py",
            "references/empty.md",
        )
        for relative in paths:
            for content in (b"", b" \t\n"):
                with self.subTest(path=relative, content=content):
                    with tempfile.TemporaryDirectory(
                        prefix="compile-debug-empty-"
                    ) as raw:
                        package = Path(raw) / "tool-compile-debug"
                        copy_package(package)
                        (package / relative).write_bytes(content)
                        result = run_python(
                            [str(package / "scripts" / "face_selfcheck.py"), "--json"]
                        )
                        self.assertEqual(result.returncode, 1, result.stderr)
                        payload = json.loads(result.stdout)
                        self.assertFalse(payload["ok"])
                        self.assertIn(
                            f"empty or whitespace-only package file: {relative}",
                            payload["errors"],
                        )

    def test_incomplete_shards_fail_selfcheck_and_preflight_engine_check(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-incomplete-") as raw:
            package = Path(raw) / "tool-compile-debug"
            copy_package(package)
            shards = (
                package / "scripts" / "engine" / "harness/knowledge" / "api_signatures"
            )
            parts = sorted(shards.glob("part-*.yaml"))
            if not parts:
                self.skipTest("本包使用单文件知识库")
            parts[-1].unlink()
            result = run_python(
                [str(package / "scripts" / "face_selfcheck.py"), "--json"]
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            payload = json.loads(result.stdout)
            self.assertFalse(payload["ok"])
            self.assertTrue(
                any("shard file set mismatch" in e for e in payload["errors"])
            )

            script = str(package / "scripts" / "preflight.py")
            code = (
                "import json, runpy; "
                f"scope = runpy.run_path({script!r}); "
                "checks = scope['_base_checks'](); "
                "ok = scope['_check_engine'](checks); "
                "print(json.dumps(checks)); raise SystemExit(0 if ok else 1)"
            )
            result = run_python(["-c", code])
            self.assertEqual(result.returncode, 1, result.stderr)
            payload = json.loads(result.stdout)
            self.assertTrue(
                any("shard file set mismatch" in e for e in payload["errors"])
            )

    def test_all_python_scripts_compile(self):
        targets = sorted(SCRIPTS.rglob("*.py"))
        self.assertGreater(len(targets), 20)
        for target in targets:
            # 内置 compile 只做语法编译，不落 pyc，包目录保持零 bytecode 污染
            compile(target.read_text(encoding="utf-8"), str(target), "exec")

    def test_all_runtime_modules_import_without_package_writes(self):
        with tempfile.TemporaryDirectory(prefix="compile-debug-import-") as raw:
            package = Path(raw) / "tool-compile-debug"
            copy_package(package)
            before = {
                p.relative_to(package): p.read_bytes()
                for p in package.rglob("*")
                if p.is_file()
            }
            code = f"""
import importlib, importlib.util, json, sys
from pathlib import Path
package = Path({str(package)!r})
sys.pycache_prefix = {str(Path(raw) / "bytecode-cache")!r}
sys.path.insert(0, str(package / 'scripts/engine'))
failures = {{}}
loaded = []
for path in sorted((package / 'scripts').rglob('*.py')):
    name = str(path.relative_to(package))
    bytecode_policy = sys.dont_write_bytecode
    try:
        if path.is_relative_to(package / 'scripts/engine/harness'):
            parts = list(path.relative_to(package / 'scripts/engine').with_suffix('').parts)
            if parts[-1] == '__init__': parts.pop()
            importlib.import_module('.'.join(parts))
        else:
            spec = importlib.util.spec_from_file_location('acceptance_' + path.stem, path)
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
        loaded.append(name)
    except Exception as exc:
        failures[name] = type(exc).__name__ + ': ' + str(exc)
    finally:
        sys.dont_write_bytecode = bytecode_policy
print(json.dumps({{'loaded': loaded, 'failures': failures}}))
"""
            result = run_python(["-c", code])
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["failures"], {})
            self.assertEqual(
                len(report["loaded"]), len(list((package / "scripts").rglob("*.py")))
            )
            after = {
                p.relative_to(package): p.read_bytes()
                for p in package.rglob("*")
                if p.is_file()
            }
            self.assertEqual(before, after)
            self.assertTrue(list((Path(raw) / "bytecode-cache").rglob("*.pyc")))

    def test_test_discovery_preserves_default_bytecode_policy(self):
        code = f"""
import json, sys, unittest
before = sys.dont_write_bytecode
loader = unittest.TestLoader()
suite = loader.discover({str(SKILL_ROOT / "tests")!r})
print(json.dumps({{'before': before, 'after': sys.dont_write_bytecode,
                  'errors': loader.errors, 'tests': suite.countTestCases()}}))
"""
        result = run_python(["-c", code])
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report["before"])
        self.assertFalse(report["after"])
        self.assertEqual(report["errors"], [])
        self.assertGreater(report["tests"], 0)

    def test_shell_wrappers_pass_syntax_check(self):
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash 不可用（Windows 无 Git Bash 时跳过语法检查）")
        wrappers = sorted(SCRIPTS.glob("*.sh"))
        wrappers.append(SCRIPTS / "engine" / "harness/runtime/build_one.sh")
        self.assertGreaterEqual(len(wrappers), 4)
        for wrapper in wrappers:
            result = subprocess.run(
                [bash, "-n", str(wrapper)], capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0, f"{wrapper.name}: {result.stderr}")


if __name__ == "__main__":
    unittest.main()
