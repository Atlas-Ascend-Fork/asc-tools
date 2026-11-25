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
"""Offline regression tests for bundled diagnosis and verification behavior.

Run from the skill directory: python3 -m unittest discover -s tests
Synthetic projects exercise the real diagnosis pipeline. Verification tests
replay build evidence and exercise a real Python timeout without compiling;
they do not establish CANN or NPU
compatibility. Only the installed package, Python and PyYAML are required.
"""

import importlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from copy import deepcopy
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

_TESTS_DIR = Path(__file__).resolve().parent
if str(_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(_TESTS_DIR))

from package_paths import resolve_roots  # noqa: E402

_, SKILL_ROOT = resolve_roots(__file__)
ENGINE_MODULES = SKILL_ROOT / "scripts" / "engine" / "harness/engine"
ENGINE_NAME = "harness.engine"


def load_bundled_engine():
    """Test the installed engine in a fresh process; reject a foreign import."""
    engine_root = SKILL_ROOT / "scripts/engine"
    loaded = sys.modules.get(ENGINE_NAME)
    if (
        loaded is not None
        and Path(loaded.__file__).resolve().parent != ENGINE_MODULES.resolve()
    ):
        raise RuntimeError("Run bundled behavior tests in a fresh process")
    sys.path.insert(0, str(engine_root))
    pipeline = importlib.import_module(ENGINE_NAME + ".diagnose_compile_failure")
    if Path(pipeline.__file__).resolve().parent != ENGINE_MODULES.resolve():
        raise RuntimeError("Behavior tests resolved a different engine")
    return pipeline


PIPELINE = load_bundled_engine()
VERIFY = PIPELINE.verify_runner
EVIDENCE = importlib.import_module(f"{ENGINE_NAME}.verification_evidence")


class DocumentedKnowledgeContractTest(unittest.TestCase):
    def test_probe_reports_link_to_existing_packaged_references(self):
        cause_tree = importlib.import_module(f"{ENGINE_NAME}.cause_tree")
        scenarios = (
            ("optimization_sensitive_or_compiler_limit", "probe_O0", "passed"),
            ("compiler_backend_after_preprocess", "probe_preprocess_only", "passed"),
            ("reproducible_compile_failure", "probe_direct_kernel_compile", "failed"),
        )
        for likely, probe, status in scenarios:
            with self.subTest(likely=likely):
                tree = cause_tree.build_cause_tree(
                    [],
                    category="compiler_internal",
                    phase="kernel_compile",
                    normalized={},
                    probes={
                        "interpretation": {"likely": likely},
                        "probes": [{"name": probe, "status": status}],
                    },
                )
                leaves = [
                    leaf for leaf in tree["leaves"] if leaf["kind"] == "compiler_probe"
                ]
                self.assertTrue(leaves)
                for leaf in leaves:
                    reference = (SKILL_ROOT / leaf["reference"]).resolve()
                    self.assertTrue(reference.is_relative_to(SKILL_ROOT.resolve()))
                    self.assertTrue(reference.is_file(), leaf["reference"])

    def test_version_statuses_match_documented_protocol(self):
        gate = importlib.import_module(f"{ENGINE_NAME}.version_gate")
        text = (SKILL_ROOT / "references/json-schemas.md").read_text(encoding="utf-8")
        for log, env, resolution, status in (
            (None, None, "unknown", "unknown"),
            ("8.0.RC1", None, "below_floor", "mismatch"),
            ("9.0.0", "9.1.0", "version_conflict", "mismatch"),
            ("9.2.0-beta.2", None, "supported", "match"),
        ):
            with self.subTest(log=log, env=env):
                value = gate.classify(log, env)
                self.assertEqual(
                    (value["resolution"], value["status"]), (resolution, status)
                )
                self.assertIn(f"`{resolution}`", text)
                self.assertIn(f"`{status}`", text)
        self.assertIn("requires_card_and_current_sdk_evidence", text)

    def test_symbol_query_documentation_does_not_advertise_retired_results(self):
        text = (SKILL_ROOT / "references/json-schemas.md").read_text(encoding="utf-8")
        self.assertIn("unexecuted_knowledge_request", text)
        self.assertIn("status=not_queried", text)
        for retired in ("api.query", "matching_symbol_count", "filtered_count"):
            self.assertNotIn(retired, text)


class TemporaryProjectTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="compile-debug-behavior-")
        self.addCleanup(temporary.cleanup)
        self.workspace = Path(temporary.name).resolve()
        self.project = self.workspace / "project"
        self.project.mkdir()

    def write_source(self, relative, content):
        path = self.project / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def source_snapshot(self):
        return {
            path.relative_to(self.project).as_posix(): path.read_bytes()
            for path in self.project.rglob("*")
            if path.is_file()
        }


class CommandEchoBehaviorTest(TemporaryProjectTest):
    def test_command_path_does_not_create_new_error_but_diagnostic_does(self):
        self.write_source("unit.cpp", "int value = 1;\n")
        before = EVIDENCE.failure_evidence(
            self.project,
            VERIFY.normalize_log_text("unit.cpp:1:5: error: unknown type name 'Value'"),
            target="Ascend910B1",
        )
        command = (
            "cd /work/build && /venv/bin/python /work/template-tools/compile_wrapper.py -- "
            "/sdk/bin/bisheng -c unit.cpp -o unit.o"
        )
        after = EVIDENCE.failure_evidence(
            self.project,
            VERIFY.normalize_log_text(command),
            target="Ascend910B1",
            status="passed",
        )
        self.assertEqual(after["issues"], [])
        self.assertEqual(EVIDENCE.compare_issues(before, after)["observed_new"], [])
        failed = EVIDENCE.failure_evidence(
            self.project,
            VERIFY.normalize_log_text(
                command
                + "\nunit.cpp:1:5: error: no matching function for call to 'Run'"
            ),
            target="Ascend910B1",
        )
        self.assertEqual(len(failed["issues"]), 1)
        self.assertEqual(
            EVIDENCE.compare_issues(before, failed)["observed_new"],
            [failed["issues"][0]["issue_id"]],
        )


class DiagnosisBehaviorTest(TemporaryProjectTest):
    def test_light_api_diagnostic_requests_knowledge_without_guessing_replacement(self):
        log = self.workspace / "api-failure.log"
        log.write_text(
            "kernel.cpp:3:4: error: no template named 'MatmulImpl' in namespace 'AscendC'; did you mean 'MulImpl'?\n",
            encoding="utf-8",
        )
        output = self.workspace / "diagnosis"
        with (
            patch.object(
                PIPELINE, "collect_context", side_effect=AssertionError("source read")
            ),
            patch.object(
                PIPELINE, "detect_cann", side_effect=AssertionError("environment probe")
            ),
            patch(
                "subprocess.run",
                side_effect=AssertionError("query or compiler execution"),
            ),
        ):
            bundle = PIPELINE.run_bundle(
                self.project, log, output, soc="Ascend950", context_mode="light"
            )
            result = PIPELINE.build_stdout_summary(output, bundle, "offline-api-test")
        self.assertIsNone(result["fast_path"])
        self.assertEqual(result["diagnostic_guidance"][0]["applicability"], "unknown")
        request = result["knowledge_requests"][0]
        self.assertEqual(request["skill"], "ascendc-api-knowledge-query")
        self.assertEqual(request["subject"], "MatmulImpl")
        self.assertEqual(request["target"], "Ascend950")
        self.assertEqual(request["status"], "not_queried")
        self.assertIsNone(request["platform"])
        self.assertFalse(result["verify"]["build_invoked"])

    def test_default_cli_parses_log_without_source_environment_or_build(self):
        log = self.workspace / "failure.log"
        log.write_text("unit.cpp:1:1: error: expected ';'\n", encoding="utf-8")
        output = self.workspace / "diagnosis"
        arguments = [
            "diagnose",
            "--project",
            str(self.project),
            "--build-log",
            str(log),
            "--out",
            str(output),
            "--json-stdout",
        ]
        with (
            patch.object(sys, "argv", arguments),
            patch.object(
                PIPELINE, "collect_context", side_effect=AssertionError("source read")
            ),
            patch.object(
                PIPELINE, "detect_cann", side_effect=AssertionError("environment probe")
            ),
            patch.object(VERIFY, "verify", side_effect=AssertionError("build")),
            redirect_stdout(io.StringIO()) as stream,
        ):
            self.assertEqual(PIPELINE.main(), 0)
        summary = json.loads(stream.getvalue())
        self.assertEqual(summary["context_mode"], "light")
        self.assertEqual(summary["observation_status"], "failed")
        diagnosis = json.loads((output / "diagnosis.json").read_text(encoding="utf-8"))
        self.assertEqual(diagnosis["coverage"]["source_files_read"], 0)
        self.assertFalse(diagnosis["coverage"]["build_invoked"])

    def diagnose(self, log_text):
        log = self.workspace / "build.log"
        log.write_text(log_text, encoding="utf-8")
        output = self.workspace / "diagnosis"
        before = self.source_snapshot()
        with (
            patch.object(
                PIPELINE,
                "detect_cann",
                return_value={
                    "found": False,
                    "install_dir": None,
                    "version": "unknown",
                },
            ),
            patch.object(PIPELINE, "collect_tools", return_value={}),
            patch.object(PIPELINE, "run_probes") as probes,
            patch.object(VERIFY, "verify") as verify,
            patch("subprocess.run", side_effect=AssertionError("Unexpected process")),
        ):
            files = PIPELINE.run_bundle(self.project, log, output, soc="Ascend910B1")
            summary = PIPELINE.build_stdout_summary(output, files, "offline-test")
        probes.assert_not_called()
        verify.assert_not_called()
        self.assertEqual(self.source_snapshot(), before)
        self.assertNotIn("verify", files)
        self.assertFalse(summary["verify"]["build_invoked"])
        self.assertFalse(summary["env"]["cann_found"])
        self.assertIsNone(summary["env"]["npu_devices"])
        self.assertEqual(summary["env"]["npu_probe_status"], "not_requested")
        self.assertTrue(summary["next_actions"])
        self.assertFalse(summary.get("requires_user_decision"))
        self.assertFalse(summary.get("blocked"))
        self.assertEqual(summary["top_suggestion"]["type"], "diagnostic_guidance")
        self.assertEqual(summary["top_suggestion"]["applicability"], "conditional")
        suggestions = json.loads(
            Path(files["patch_suggestions"]).read_text(encoding="utf-8")
        )["suggestions"]
        self.assertTrue(suggestions)
        self.assertTrue(all(item["patch"] == "" for item in suggestions))
        diagnosis = json.loads(Path(files["diagnosis"]).read_text(encoding="utf-8"))
        return summary, diagnosis

    def test_first_error_keeps_priority_over_candidate_note_and_later_error(self):
        self.write_source("src/compute.cpp", "void compute() { Transform(1, 2); }\n")
        summary, _ = self.diagnose(
            "src/compute.cpp:1:18: error: no matching function for call to 'Transform'\n"
            "void compute() { Transform(1, 2); }\n"
            "                 ^\n"
            "include/vector.h:20:1: note: candidate function template not viable: "
            "requires 3 arguments, but 2 were provided\n"
            "void Transform(int a, int b, int c);\n"
            "     ^\n"
            "src/compute.cpp:8:1: error: static assertion failed: unsupported architecture\n"
        )
        self.assertEqual(summary["category"], "template_error")
        self.assertEqual(
            summary["primary_error"]["message"],
            "no matching function for call to 'Transform'",
        )
        self.assertEqual(summary["primary_error"]["line"], 1)
        self.assertEqual(
            summary["diagnostic_facts"]["mechanism"]["value"],
            "overload_resolution_failure",
        )
        notes = summary["top_suggestion"]["compiler_notes"]
        self.assertTrue(
            any(note["declaration_symbol"] == "Transform" for note in notes)
        )
        self.assertTrue(
            all(note["binding"] == "same_diagnostic_block" for note in notes)
        )

    def test_undeclared_object_name_does_not_establish_tiling_or_sdk_ownership(self):
        for name in ("tiling", "regParams", "DataCopy"):
            with self.subTest(name=name):
                self.write_source(
                    "src/compute.cpp", f"void compute() {{ {name}.GetData(); }}\n"
                )
                summary, diagnosis = self.diagnose(
                    "src/compute.cpp:1:18: error: "
                    f"use of undeclared identifier '{name}'\n"
                )
                self.assertEqual(summary["category"], "unknown")
                self.assertEqual(diagnosis["classification_evidence"], [])
                facts = summary["diagnostic_facts"]
                self.assertEqual(facts["mechanism"]["value"], "undeclared_identifier")
                self.assertEqual(facts["sdk_association"]["status"], "not_established")
                self.assertIsNone(summary["root_cause_localization"])

    def test_missing_header_is_not_reclassified_as_opbuild_configuration(self):
        self.write_source("src/compute.cpp", '#include "missing.h"\n')
        for message in (
            "missing.h: No such file or directory",
            "'missing.h' file not found",
        ):
            for wrapper in (
                "",
                "CMake Error at /opt/tools/func.cmake:12 (message):\n"
                "  opbuild failed\n",
            ):
                with self.subTest(message=message, wrapper=bool(wrapper)):
                    summary, _ = self.diagnose(
                        f"src/compute.cpp:1:10: fatal error: {message}\n{wrapper}"
                    )
                    self.assertEqual(summary["category"], "header_missing")
                    self.assertEqual(summary["primary_error"]["message"], message)
                    self.assertEqual(summary["primary_error"]["line"], 1)

    def test_local_declaration_without_include_supplies_header_evidence(self):
        self.write_source("src/compute.cpp", "PacketShape plan;\n")
        self.write_source("include/contract.h", "struct PacketShape { int extent; };\n")
        summary, diagnosis = self.diagnose(
            "src/compute.cpp:1:1: error: unknown type name 'PacketShape'\n"
        )
        self.assertEqual(summary["category"], "header_missing")
        fact = diagnosis["classification_evidence"][0]
        self.assertEqual(fact["symbol"], "PacketShape")
        self.assertEqual(fact["declaration_file"], "include/contract.h")
        self.assertEqual(fact["declaration_line"], 1)
        self.assertTrue(fact["requires_include_configuration_check"])

    def test_included_declaration_does_not_supply_missing_include_evidence(self):
        self.write_source(
            "src/compute.cpp", '#include "../include/contract.h"\nPacketShape plan;\n'
        )
        self.write_source("include/contract.h", "struct PacketShape { int extent; };\n")
        summary, diagnosis = self.diagnose(
            "src/compute.cpp:2:1: error: unknown type name 'PacketShape'\n"
        )
        self.assertEqual(summary["category"], "unknown")
        self.assertEqual(diagnosis["classification_evidence"], [])

    def test_member_failure_localizes_owner_declaration_in_another_file(self):
        self.write_source("include/shape.hpp", "struct Shape {\n  int count;\n};\n")
        self.write_source(
            "src/compute.cpp",
            '#include "../include/shape.hpp"\n'
            "int compute(Shape shape) { return shape.extent; }\n",
        )
        summary, _ = self.diagnose(
            "src/compute.cpp:2:41: error: no member named 'extent' in 'Shape'\n"
        )
        self.assertEqual(
            Path(summary["primary_error"]["file"]),
            (self.project / "src/compute.cpp").resolve(),
        )
        location = summary["root_cause_localization"]
        self.assertEqual(location["root_file"], "include/shape.hpp")
        self.assertEqual(location["root_line"], 1)
        self.assertEqual(location["root_symbol"], "extent")
        self.assertEqual(location["method"], "member_owner_class_def_site")
        self.assertEqual(
            summary["diagnostic_facts"]["mechanism"]["value"], "member_lookup_failure"
        )


class VerificationBehaviorTest(TemporaryProjectTest):
    def setUp(self):
        super().setUp()
        self.source = self.write_source("compute.cpp", "int compute() { return 0; }\n")
        previous = EVIDENCE.failure_evidence(
            self.project,
            {"errors": [{"file": str(self.source), "message": "expected ';'"}]},
            target="Ascend910B1",
            build_target="device",
        )
        self.previous_path = self.workspace / "previous.json"
        self.previous_path.write_text(json.dumps(previous), encoding="utf-8")

    def passed_build(self):
        return {
            "status": "passed",
            "returncode": 0,
            "build_invoked": True,
            "expected_target": "device",
            "failure_evidence": EVIDENCE.failure_evidence(
                self.project, {}, status="passed"
            ),
            "compilation_evidence": {
                "status": "confirmed",
                "project_root": str(self.project),
                "architectures": ["2201"],
                "targets": ["device"],
                "required_sources": [str(self.source)],
                "records": [
                    {
                        "source": str(self.source),
                        "dependencies": [str(self.source)],
                        "architectures": ["2201"],
                        "target": "device",
                    }
                ],
            },
        }

    def test_cli_exception_keeps_false_true_or_unknown_dispatch_evidence(self):
        for index, dispatched in enumerate((False, True, None)):
            with self.subTest(dispatched=dispatched):
                output = self.workspace / f"exception-{index}.json"
                checkpoint = self.workspace / f"checkpoint-{index}"
                error = OSError("controlled verification failure")
                error.verification_failure = {
                    "directory": str(checkpoint),
                    "build_invoked": dispatched,
                    "executions": [],
                    "new_build_invoked": False,
                }
                with (
                    patch.object(
                        sys,
                        "argv",
                        [
                            "verify",
                            "--case",
                            str(self.project),
                            "--json-out",
                            str(output),
                            "--recovery-dir",
                            str(checkpoint),
                        ],
                    ),
                    patch.object(VERIFY, "verify", side_effect=error),
                    redirect_stdout(io.StringIO()),
                ):
                    self.assertEqual(VERIFY.main(), 1)
                report = json.loads(output.read_text(encoding="utf-8"))
                self.assertIs(report["build_invoked"], dispatched)
                self.assertEqual(report["validation_scope"]["compile"], "unconfirmed")
                self.assertEqual(
                    report["transition"],
                    "verification_not_run"
                    if dispatched is False
                    else "verification_incomplete",
                )
                self.assertIn("error", report)

    def test_cli_completed_failed_build_keeps_its_failure_with_exit_zero(self):
        output = self.workspace / "failed-build.json"
        payload = {
            "transition": "compile_failure_remaining",
            "build_invoked": True,
            "after": {"returncode": 2},
            "validation_scope": {"compile": "failed"},
        }
        with (
            patch.object(
                sys,
                "argv",
                [
                    "verify",
                    "--case",
                    str(self.project),
                    "--json-out",
                    str(output),
                    "--recovery-dir",
                    str(self.workspace / "checkpoint"),
                ],
            ),
            patch.object(VERIFY, "verify", return_value=payload),
            redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(VERIFY.main(), 0)
        self.assertEqual(json.loads(output.read_text(encoding="utf-8")), payload)

    def replay_build(self, build, with_previous=True):
        """Replace execution only; use the public evidence/transition checks."""
        execution = {
            "before": deepcopy(build),
            "after": deepcopy(build),
            "build_invoked": True,
            "verify_mode": "in_place",
            "soc_used": "Ascend910B1",
        }
        with (
            patch.object(VERIFY, "_verify_impl", return_value=execution),
            patch.object(VERIFY, "_emit_verify_audit", return_value=None),
            patch("subprocess.run", side_effect=AssertionError("Unexpected process")),
        ):
            return VERIFY.verify(
                self.project,
                soc="Ascend910B1",
                previous_result=self.previous_path if with_previous else None,
            )

    def test_matched_failure_target_and_coverage_confirm_compile_repair(self):
        result = self.replay_build(self.passed_build())
        self.assertEqual(result["transition"], "compile_failure_fixed")
        self.assertEqual(result["target_validation"]["status"], "matched")
        self.assertEqual(
            result["target_validation"]["prior_failure_coverage"]["status"], "matched"
        )
        self.assertTrue(result["error_comparison"]["disappeared"])
        self.assertEqual(result["validation_scope"]["numerical"], "not_checked")

    def test_success_without_previous_failure_is_only_compile_passed(self):
        result = self.replay_build(self.passed_build(), with_previous=False)
        self.assertEqual(result["transition"], "compile_passed")
        self.assertEqual(result["error_comparison"]["status"], "unavailable")

    def test_unrelated_compiled_source_does_not_prove_previous_failure_fixed(self):
        helper = self.write_source("helper.cpp", "int helper() { return 0; }\n")
        build = self.passed_build()
        compilation = build["compilation_evidence"]
        compilation["required_sources"] = [str(helper)]
        compilation["records"][0].update(source=str(helper), dependencies=[str(helper)])
        result = self.replay_build(build)
        self.assertEqual(result["transition"], "compile_passed")
        self.assertEqual(result["target_validation"]["status"], "matched")
        coverage = result["target_validation"]["prior_failure_coverage"]
        self.assertEqual(coverage["status"], "unconfirmed")
        self.assertEqual(coverage["missing_files"], ["compute.cpp"])

    def test_wrong_architecture_or_missing_required_source_is_unconfirmed(self):
        for missing in ("architecture", "source"):
            with self.subTest(missing=missing):
                build = self.passed_build()
                record = build["compilation_evidence"]["records"][0]
                if missing == "architecture":
                    record["architectures"] = ["3510"]
                else:
                    record["dependencies"] = []
                result = self.replay_build(build)
                self.assertEqual(result["transition"], "case_not_reproducible")
                self.assertEqual(result["target_validation"]["status"], "unconfirmed")

    def test_success_for_a_different_build_target_does_not_confirm_repair(self):
        build = self.passed_build()
        build["expected_target"] = "another_device"
        compilation = build["compilation_evidence"]
        compilation["targets"] = ["another_device"]
        compilation["records"][0]["target"] = "another_device"
        result = self.replay_build(build)
        self.assertEqual(result["transition"], "compile_passed")
        comparison = result["error_comparison"]["build_target_comparison"]
        self.assertEqual(comparison["status"], "conflict")

    def test_timeout_preserves_partial_log_and_cannot_confirm_repair(self):
        log = self.workspace / "timeout.log"
        partial = "compute.cpp:1:1: error: expected ';'"
        # The durable executor uses a worker/Popen, so mocking subprocess.run
        # would never exercise timeout handling on the current engine.
        command = [
            sys.executable,
            "-u",
            "-c",
            "import time; print(" + repr(partial) + ", flush=True); time.sleep(60)",
        ]
        build = VERIFY._execute_build(command, self.project, log, 2, self.project)
        self.assertTrue(build["build_invoked"])
        self.assertEqual(build["status"], "interrupted")
        # The receipt names the durable worker log; the requested path is the
        # journal namespace and need not be the physical output file.
        self.assertIn(
            "error: expected ';'", Path(build["log_path"]).read_text(encoding="utf-8")
        )
        result = self.replay_build(build)
        self.assertEqual(result["transition"], "verification_interrupted")
        self.assertEqual(result["reason"], "timeout")

    def test_dry_run_never_compiles_or_confirms_repair(self):
        before = self.source_snapshot()
        with (
            patch.dict(
                os.environ,
                {"ACD_WORKSPACE_ROOT": str(self.workspace / "build-workspaces")},
            ),
            patch.object(VERIFY, "_run_build") as build,
            patch.object(VERIFY, "_emit_verify_audit", return_value=None),
            patch("subprocess.run", side_effect=AssertionError("Unexpected process")),
        ):
            result = VERIFY.verify(
                self.project,
                soc="Ascend910B1",
                previous_result=self.previous_path,
                dry_run=True,
            )
        build.assert_not_called()
        self.assertTrue(result["dry_run"])
        self.assertFalse(result["build_invoked"])
        self.assertEqual(result["transition"], "verification_not_run")
        self.assertEqual(self.source_snapshot(), before)


class WorkspaceHygieneBehaviorTest(TemporaryProjectTest):
    def setUp(self):
        super().setUp()
        self.collector = importlib.import_module(
            f"{ENGINE_NAME}.build_context_collector"
        )
        self.inspect_module = importlib.import_module(f"{ENGINE_NAME}.inspect_context")
        self.write_source("CMakeLists.txt", "project(demo)\n")
        self.write_source("unit.cpp", "int value = 0;\n")

    def test_git_directory_and_gitfile_keep_clean_workspace_complete(self):
        git_dir = self.project / ".git"
        (git_dir / "objects").mkdir(parents=True)
        (git_dir / "HEAD").write_text("ref: refs/heads/main\n", encoding="utf-8")
        (self.project / "build").mkdir()
        (self.project / "build" / "CMakeCache.txt").write_text("", encoding="utf-8")

        result = self.inspect_module.inspect(self.project, question="build")
        summary = self.inspect_module.summarize(result)
        hygiene = result["build"]["workspace_hygiene"]
        self.assertEqual(hygiene["status"], "clean")
        self.assertTrue(hygiene["scan_complete"])
        self.assertTrue(hygiene["clean_for_capture"])
        self.assertEqual(hygiene["ignored_paths"], [".git"])
        self.assertNotIn("truncation_reason", hygiene)
        self.assertFalse(
            any(
                gap.get("reason", "").startswith("workspace_hygiene_")
                for gap in result["evidence"]["gaps"]
            )
        )
        self.assertEqual(
            summary["workspace_hygiene"],
            {
                "status": "clean",
                "scan_complete": True,
                "clean_for_capture": True,
                "symlink_count": 0,
                "candidate_generated_count": 0,
            },
        )

        # Git worktrees store .git as a regular pointer file rather than a directory.
        (git_dir / "HEAD").unlink()
        (git_dir / "objects").rmdir()
        git_dir.rmdir()
        self.write_source(".git", "gitdir: /tmp/repo/.git/worktrees/demo\n")

        worktree_result = self.inspect_module.inspect(self.project, question="build")
        worktree_hygiene = worktree_result["build"]["workspace_hygiene"]
        self.assertEqual(worktree_hygiene["status"], "clean")
        self.assertTrue(worktree_hygiene["scan_complete"])
        self.assertTrue(worktree_hygiene["clean_for_capture"])
        self.assertEqual(worktree_hygiene["ignored_paths"], [".git"])
        self.assertNotIn("truncation_reason", worktree_hygiene)
        self.assertFalse(
            any(
                gap.get("reason", "").startswith("workspace_hygiene_")
                for gap in worktree_result["evidence"]["gaps"]
            )
        )

    def test_git_ignore_preserves_artifacts_symlinks_and_budget_truncation(self):
        (self.project / ".git").mkdir()
        (self.project / "build_out").mkdir()
        dirty = self.collector._workspace_hygiene(self.project)
        self.assertEqual(dirty["status"], "attention_required")
        self.assertTrue(dirty["scan_complete"])
        self.assertFalse(dirty["clean_for_capture"])
        self.assertEqual(dirty["ignored_paths"], [".git"])
        self.assertIn("build_out", dirty["candidate_generated_paths"])

        (self.project / "build_out").rmdir()
        # Keep root entry count (4: .git, CMakeLists.txt, src, unit.cpp) below max_entries
        # so .git is scanned deterministically regardless of filesystem scandir order.
        for idx in range(4):
            self.write_source(f"src/extra_{idx}.cpp", "int x = 1;\n")
        truncated = self.collector._workspace_hygiene(self.project, max_entries=5)
        self.assertEqual(truncated["status"], "incomplete")
        self.assertFalse(truncated["scan_complete"])
        self.assertIsNone(truncated["clean_for_capture"])
        self.assertEqual(truncated["ignored_paths"], [".git"])
        self.assertEqual(truncated["truncation_reason"], "entry_budget_exceeded")

        dir_truncated = self.collector._workspace_hygiene(self.project, max_dirs=1)
        self.assertEqual(dir_truncated["status"], "incomplete")
        self.assertFalse(dir_truncated["scan_complete"])
        self.assertIsNone(dir_truncated["clean_for_capture"])
        self.assertEqual(dir_truncated["ignored_paths"], [".git"])
        self.assertEqual(
            dir_truncated["truncation_reason"], "directory_budget_exceeded"
        )

        class _FakeGitLink:
            name = ".git"
            path = str(self.project / ".git")

            def is_symlink(self):
                return True

        class _FakeScandir:
            def __enter__(self):
                return iter([_FakeGitLink()])

            def __exit__(self, exc_type, exc, tb):
                return False

        with patch.object(self.collector.os, "scandir", lambda _p: _FakeScandir()):
            linked = self.collector._workspace_hygiene(self.project)
        self.assertEqual(linked["status"], "attention_required")
        self.assertTrue(linked["scan_complete"])
        self.assertFalse(linked["clean_for_capture"])
        self.assertEqual(linked["ignored_paths"], [])
        self.assertEqual(linked["symlinks_outside_build"], [".git"])


if __name__ == "__main__":
    unittest.main()
