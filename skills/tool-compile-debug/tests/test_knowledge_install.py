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
"""Installation transactions, interactive CLI, and knowledge dependency boundaries.

用途：验证双层发行安装器 scripts/install.sh 的客户端映射、共享知识库更新、冲突回滚与安全边界。
使用方法：python3 -m unittest discover -s tests -p test_knowledge_install.py
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

_TESTS_DIR = Path(__file__).resolve().parent
if str(_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(_TESTS_DIR))

from package_paths import resolve_roots  # noqa: E402

DIST_ROOT, SKILL_ROOT = resolve_roots(__file__)
INSTALL_SH = (DIST_ROOT / "scripts" / "install.sh") if DIST_ROOT else None


def _load_installer_module():
    if INSTALL_SH is not None and INSTALL_SH.is_file():
        text = INSTALL_SH.read_text(encoding="utf-8")
        marker = 'exec "$python_bin" - "${BASH_SOURCE[0]:-}" "$@" <<\'PY\'\n'
        start = text.index(marker) + len(marker)
        end = text.rindex("\nPY\n")
        source = text[start:end] + "\n"
        origin = str(INSTALL_SH)
    else:
        canonical = Path(__file__).resolve().parents[2] / "installer" / "main.py"
        source = canonical.read_text(encoding="utf-8")
        origin = str(canonical)
    module = types.ModuleType("compile_debug_installer")
    module.__file__ = origin
    exec(compile(source, origin, "exec"), module.__dict__)
    return module


INSTALL = _load_installer_module()


def _find_bash():
    candidates = (
        (
            "C:/Program Files/Git/bin/bash.exe",
            "C:/Program Files (x86)/Git/bin/bash.exe",
        )
        if os.name == "nt"
        else (shutil.which("bash"),)
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            probe = subprocess.run(
                [candidate, "-c", ":"], capture_output=True, text=True
            )
            if probe.returncode == 0:
                return candidate
    return None


BASH = _find_bash()


class KnowledgeInstallTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="compile-knowledge-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.project = self.root / "project"
        self.project.mkdir()
        self.knowledge = self.root / "knowledge"
        self._populate_knowledge_fixture(self.knowledge)
        self.lock = INSTALL.read_lock()

    def _populate_knowledge_fixture(self, kb_root: Path) -> None:
        for name in INSTALL.QUERIES:
            skill_dir = kb_root / ".agents/skills" / name
            skill_dir.mkdir(parents=True, exist_ok=True)
            (skill_dir / "SKILL.md").write_text(
                f"---\nname: {name}\n---\nfixture\n", encoding="utf-8"
            )
        scripts_dir = kb_root / ".agents/skills/knowledge-query/scripts"
        scripts_dir.mkdir(parents=True, exist_ok=True)
        for script_name in ("knowledge_index.py", "knowledge_query.py"):
            (scripts_dir / script_name).write_text(
                "import sys\nsys.exit(0)\n", encoding="utf-8"
            )
        registry = kb_root / "governance/schemas/registries.yaml"
        registry.parent.mkdir(parents=True, exist_ok=True)
        registry.write_text("platforms:\n  a2: {}\n  a3: {}\n", encoding="utf-8")
        (kb_root / "requirements.txt").write_text("PyYAML>=6,<7\n", encoding="utf-8")

    def invoke(self, argv, *, script_path=None):
        with (
            patch.object(Path, "home", return_value=self.home),
            patch.dict(
                os.environ,
                {"HOME": str(self.home), "USERPROFILE": str(self.home)},
                clear=False,
            ),
        ):
            os.environ.pop("CANNBOT_KNOWLEDGE_ROOT", None)
            os.environ.pop("CLAUDE_CONFIG_DIR", None)
            return INSTALL.cli_entry(
                argv,
                script_path=script_path
                if script_path is not None
                else (str(INSTALL_SH) if INSTALL_SH else ""),
            )

    def test_dependency_roster_exact(self):
        self.assertEqual(
            list(INSTALL.QUERIES),
            ["knowledge-query", "ascendc-api-knowledge-query"],
        )
        self.assertEqual(self.lock["consumer_skills"], list(INSTALL.QUERIES))
        self.assertRegex(self.lock["commit"], r"^[0-9a-f]{40}$")

    def test_rejects_extended_or_malformed_locks(self):
        path = self.root / "invalid.json"
        cases = [
            "null",
            "[]",
            "1",
            '"text"',
            '{"commit":"a","commit":"b"}',
            json.dumps({**self.lock, "python_minimum": "3.9"}),
            json.dumps(
                {
                    **self.lock,
                    "consumer_skills": [
                        *INSTALL.QUERIES,
                        "runtime-api-knowledge-query",
                    ],
                }
            ),
        ]
        for raw in cases:
            with self.subTest(raw=raw):
                path.write_text(raw, encoding="utf-8")
                with self.assertRaises(ValueError):
                    INSTALL.read_lock(path)

    def test_validate_package_accepts_pure_skill_and_rejects_outer_or_incomplete(self):
        INSTALL.validate_package(SKILL_ROOT)
        if DIST_ROOT is not None and (DIST_ROOT / "README.md").is_file():
            with self.assertRaisesRegex(ValueError, "pure"):
                INSTALL.validate_package(DIST_ROOT)

        broken = self.root / "broken-skill"
        shutil.copytree(
            SKILL_ROOT,
            broken,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        (broken / "scripts/run.py").unlink()
        with self.assertRaisesRegex(ValueError, "run.py"):
            INSTALL.validate_package(broken)

    def test_parse_clients_and_destinations(self):
        self.assertEqual(
            INSTALL.parse_clients("all"),
            ["codex", "gemini", "opencode", "claude"],
        )
        self.assertEqual(
            INSTALL.parse_clients("opencode,codex,opencode"),
            ["opencode", "codex"],
        )
        with self.assertRaisesRegex(ValueError, "Unsupported client"):
            INSTALL.parse_clients("unknown-agent")

        primary, claude = INSTALL.resolve_destinations(
            "project", ["codex", "gemini"], self.project, None, self.home
        )
        self.assertEqual(primary, self.project / ".agents/skills")
        self.assertIsNone(claude)

        primary_c, claude_c = INSTALL.resolve_destinations(
            "project", ["claude"], self.project, None, self.home
        )
        self.assertEqual(primary_c, self.project / ".claude/skills")
        self.assertIsNone(claude_c)

        primary_all, claude_all = INSTALL.resolve_destinations(
            "project", INSTALL.ALL_CLIENTS, self.project, None, self.home
        )
        self.assertEqual(primary_all, self.project / ".agents/skills")
        self.assertEqual(claude_all, self.project / ".claude/skills")

        custom_dir = self.root / "custom-skills"
        primary_custom, claude_custom = INSTALL.resolve_destinations(
            "custom", [], None, custom_dir, self.home
        )
        self.assertEqual(primary_custom, custom_dir.resolve())
        self.assertIsNone(claude_custom)

    def test_cli_parameter_conflicts_are_rejected(self):
        bad_Combinations = [
            ["codex", "--clients", "opencode"],
            ["codex", str(self.project), "--project", str(self.project)],
            ["--global", "--project", str(self.project)],
            ["--global", "--target", str(self.root / "custom")],
            ["--global", "--platform", "a2"],
            ["--target", str(self.root / "custom"), "--clients", "codex"],
            ["--without-knowledge", "--knowledge-root", str(self.knowledge)],
            ["--without-knowledge", "--pinned"],
            ["--pinned", "--knowledge-ref", "dev"],
            ["--offline", "--update"],
            ["--read-only-knowledge", "--fetch-root", str(self.root / "new-kb")],
        ]
        for argv in bad_Combinations:
            with self.subTest(argv=argv):
                with self.assertRaises(SystemExit) as ctx:
                    INSTALL.main(argv, script_path="")
                self.assertEqual(ctx.exception.code, 2)

    def test_invalid_or_overlapping_roots_rejected_before_fetch(self):
        for proj, dest in (
            (SKILL_ROOT, self.root / "new-checkout"),
            (self.project, SKILL_ROOT / "new-checkout"),
            (self.root / "missing-project", self.root / "new-checkout"),
        ):
            with (
                self.subTest(project=proj, destination=dest),
                patch.object(INSTALL, "fetch_pinned_checkout") as fetch,
            ):
                rc = self.invoke(
                    [
                        "--clients",
                        "opencode",
                        "--project",
                        str(proj),
                        "--package-root",
                        str(SKILL_ROOT),
                        "--fetch-root",
                        str(dest),
                    ]
                )
                self.assertEqual(rc, 2)
                fetch.assert_not_called()
                self.assertFalse(dest.exists())

    def test_without_knowledge_installs_pure_skill_and_baseline_state(self):
        rc = self.invoke(
            [
                "--clients",
                "codex,gemini,opencode",
                "--project",
                str(self.project),
                "--package-root",
                str(SKILL_ROOT),
                "--without-knowledge",
            ]
        )
        self.assertEqual(rc, 0)
        installed = self.project / ".agents/skills/tool-compile-debug"
        self.assertTrue((installed / "SKILL.md").is_file())
        self.assertTrue((installed / "scripts/run.py").is_file())
        self.assertFalse((installed / "README.md").exists())
        self.assertFalse((installed / "tests").exists())
        self.assertFalse((installed / "scripts/install.sh").exists())
        self.assertFalse((self.project / ".asc-tools").exists())
        self.assertFalse((self.project / ".claude").exists())
        self.assertIsNotNone(INSTALL.load_baseline_digest(self.home, installed))

        # Local modification must NOT be silently overwritten without --force (even with --yes).
        (installed / "SKILL.md").write_text(
            (installed / "SKILL.md").read_text(encoding="utf-8") + "\n# user edit\n",
            encoding="utf-8",
        )
        rc_conflict = self.invoke(
            [
                "--yes",
                "--clients",
                "opencode",
                "--project",
                str(self.project),
                "--package-root",
                str(SKILL_ROOT),
                "--without-knowledge",
            ]
        )
        self.assertEqual(rc_conflict, 2)
        self.assertIn(
            "# user edit", (installed / "SKILL.md").read_text(encoding="utf-8")
        )

        # Explicit --force replaces modified copy cleanly.
        rc_forced = self.invoke(
            [
                "--force",
                "--clients",
                "opencode",
                "--project",
                str(self.project),
                "--package-root",
                str(SKILL_ROOT),
                "--without-knowledge",
            ]
        )
        self.assertEqual(rc_forced, 0)
        self.assertNotIn(
            "# user edit", (installed / "SKILL.md").read_text(encoding="utf-8")
        )

    def test_baseline_allows_seamless_upgrade_when_unmodified(self):
        rc = self.invoke(
            [
                "--clients",
                "opencode",
                "--project",
                str(self.project),
                "--package-root",
                str(SKILL_ROOT),
                "--without-knowledge",
            ]
        )
        self.assertEqual(rc, 0)
        newer_source = self.root / "newer-skill"
        shutil.copytree(
            SKILL_ROOT,
            newer_source,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        (newer_source / "references/cannbot-contract.md").write_text(
            (newer_source / "references/cannbot-contract.md").read_text(
                encoding="utf-8"
            )
            + "\n<!-- release update -->\n",
            encoding="utf-8",
        )
        rc_upgrade = self.invoke(
            [
                "--clients",
                "opencode",
                "--project",
                str(self.project),
                "--package-root",
                str(newer_source),
                "--without-knowledge",
            ]
        )
        self.assertEqual(rc_upgrade, 0)
        self.assertIn(
            "<!-- release update -->",
            (
                self.project
                / ".agents/skills/tool-compile-debug/references/cannbot-contract.md"
            ).read_text(encoding="utf-8"),
        )

    def test_config_preserves_unrelated_values_and_uses_last_platform_assignment(self):
        old = b"# retained\nexport CANNBOT_KNOWLEDGE_ROOT=/old\nCANNBOT_KNOWLEDGE_ROOT=/duplicate\nCUSTOM=keep\n"
        text = INSTALL.config_content(old, Path("/new root"), "a2").decode("utf-8")
        self.assertEqual(text.count("CANNBOT_KNOWLEDGE_ROOT="), 1)
        self.assertIn("CUSTOM=keep", text)
        self.assertIn("CANNBOT_KNOWLEDGE_PLATFORM=a2", text)
        root_line = next(
            line
            for line in text.splitlines()
            if line.startswith("CANNBOT_KNOWLEDGE_ROOT=")
        )
        self.assertEqual(
            shlex.split(root_line)[0].split("=", 1)[1], str(Path("/new root"))
        )

        cfg = self.project / ".cannbot/knowledge.env"
        cfg.parent.mkdir(parents=True)
        cfg.write_text(
            "CANNBOT_KNOWLEDGE_PLATFORM=a2\nexport CANNBOT_KNOWLEDGE_PLATFORM=invalid-fixture\n",
            encoding="utf-8",
        )
        self.assertEqual(
            INSTALL.config_values(cfg)["CANNBOT_KNOWLEDGE_PLATFORM"],
            "invalid-fixture",
        )

    def test_malformed_platform_registry_stops_before_index_commands(self):
        registry = self.knowledge / "governance/schemas/registries.yaml"
        for content in ("[unterminated", "null", "[]", "platforms: []"):
            with self.subTest(content=content):
                registry.write_text(content, encoding="utf-8")
                with patch.object(INSTALL, "run") as mocked_run:
                    with self.assertRaisesRegex(ValueError, "registry"):
                        INSTALL.prepare_knowledge(
                            self.knowledge,
                            platform=None,
                            offline=True,
                            read_only=True,
                        )
                    mocked_run.assert_not_called()

    def test_read_only_knowledge_rejects_missing_index_without_building(self):
        responses = [subprocess.CompletedProcess([], 1, "", "missing index")]
        with patch.object(INSTALL, "run", side_effect=responses) as mocked_run:
            with self.assertRaisesRegex(ValueError, "Knowledge index unavailable"):
                INSTALL.prepare_knowledge(
                    self.knowledge,
                    platform="a2",
                    offline=False,
                    read_only=True,
                )
        self.assertEqual(mocked_run.call_count, 1)

    def test_pinned_checkout_rejects_wrong_commit_or_dirty_tree(self):
        wrong_commit = [
            subprocess.CompletedProcess([], 0, str(self.knowledge.resolve()), ""),
            subprocess.CompletedProcess([], 0, "0" * 40, ""),
        ]
        with patch.object(INSTALL, "run", side_effect=wrong_commit) as mocked_run:
            with self.assertRaisesRegex(ValueError, "differs"):
                INSTALL.verify_pinned_checkout(self.knowledge.resolve(), self.lock)
        self.assertEqual(mocked_run.call_count, 2)

        dirty_tree = [
            subprocess.CompletedProcess([], 0, str(self.knowledge.resolve()), ""),
            subprocess.CompletedProcess([], 0, self.lock["commit"], ""),
            subprocess.CompletedProcess([], 0, " M requirements.txt\n", ""),
        ]
        with patch.object(INSTALL, "run", side_effect=dirty_tree):
            with self.assertRaisesRegex(ValueError, "modified tracked files"):
                INSTALL.verify_pinned_checkout(self.knowledge.resolve(), self.lock)

    def test_shared_knowledge_fast_forward_and_dirty_warn_skip(self):
        if not shutil.which("git"):
            self.skipTest("git unavailable")
        origin = self.root / "remote-kb"
        origin.mkdir()
        self._populate_knowledge_fixture(origin)
        INSTALL.run(["git", "-C", origin, "init", "-b", "master"])
        INSTALL.run(["git", "-C", origin, "config", "user.email", "test@example.com"])
        INSTALL.run(["git", "-C", origin, "config", "user.name", "Tester"])
        INSTALL.run(["git", "-C", origin, "add", "."])
        INSTALL.run(["git", "-C", origin, "commit", "-m", "init"])

        shared_kb = self.home / ".cannbot/cannbot-knowledge"
        INSTALL.repository(
            shared_kb, origin.as_posix(), "master", offline=False, update=True
        )
        first_head = INSTALL.run(
            ["git", "-C", shared_kb, "rev-parse", "HEAD"]
        ).stdout.strip()

        # Advance upstream and verify auto fast-forward update.
        (origin / "knowledge_note.md").write_text("v2\n", encoding="utf-8")
        INSTALL.run(["git", "-C", origin, "add", "knowledge_note.md"])
        INSTALL.run(["git", "-C", origin, "commit", "-m", "v2"])
        expected_head = INSTALL.run(
            ["git", "-C", origin, "rev-parse", "HEAD"]
        ).stdout.strip()
        self.assertNotEqual(first_head, expected_head)

        INSTALL.repository(
            shared_kb, origin.as_posix(), "master", offline=False, update=True
        )
        self.assertEqual(
            INSTALL.run(["git", "-C", shared_kb, "rev-parse", "HEAD"]).stdout.strip(),
            expected_head,
        )

        # Dirty tracked file warns and skips update instead of aborting!
        (shared_kb / "requirements.txt").write_text(
            "PyYAML>=6,<7\n# local tweak\n", encoding="utf-8"
        )
        (origin / "knowledge_note.md").write_text("v3\n", encoding="utf-8")
        INSTALL.run(["git", "-C", origin, "add", "knowledge_note.md"])
        INSTALL.run(["git", "-C", origin, "commit", "-m", "v3"])

        INSTALL.repository(
            shared_kb, origin.as_posix(), "master", offline=False, update=True
        )
        self.assertEqual(
            INSTALL.run(["git", "-C", shared_kb, "rev-parse", "HEAD"]).stdout.strip(),
            expected_head,
        )
        self.assertIn(
            "# local tweak",
            (shared_kb / "requirements.txt").read_text(encoding="utf-8"),
        )

    def test_offline_managed_repository_reuse_does_not_require_git(self):
        shared_kb = self.home / ".cannbot/cannbot-knowledge"
        self._populate_knowledge_fixture(shared_kb)
        with patch.object(INSTALL.shutil, "which", return_value=None):
            INSTALL.repository(
                shared_kb,
                "https://gitcode.com/cann/cannbot-knowledge.git",
                "master",
                offline=True,
                update=False,
            )
            rc = self.invoke(
                [
                    "--clients",
                    "opencode",
                    "--project",
                    str(self.project),
                    "--package-root",
                    str(SKILL_ROOT),
                    "--offline",
                    "--without-knowledge",
                ]
            )
        self.assertEqual(rc, 0)

    def test_existing_guard_prevents_parallel_install(self):
        guard = self.project / ".compile-debug-install.lock"
        guard.write_text("other-pid", encoding="utf-8")
        rc = self.invoke(
            [
                "--clients",
                "opencode",
                "--project",
                str(self.project),
                "--package-root",
                str(SKILL_ROOT),
                "--without-knowledge",
            ]
        )
        self.assertEqual(rc, 2)
        self.assertEqual(guard.read_text(encoding="utf-8"), "other-pid")

    def test_baseline_save_failure_rolls_back_upgrade_and_preserves_unrestorable_backup(
        self,
    ):
        self.assertEqual(
            self.invoke(
                [
                    "--clients",
                    "opencode",
                    "--project",
                    str(self.project),
                    "--package-root",
                    str(SKILL_ROOT),
                    "--without-knowledge",
                ]
            ),
            0,
        )
        installed = self.project / ".agents/skills/tool-compile-debug"
        old_digest = INSTALL.package_digest(installed)

        newer_pkg = self.root / "newer-baseline-fail"
        shutil.copytree(
            SKILL_ROOT,
            newer_pkg,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        (newer_pkg / "references/cannbot-contract.md").write_text(
            (newer_pkg / "references/cannbot-contract.md").read_text(encoding="utf-8")
            + "\n<!-- v3 -->\n",
            encoding="utf-8",
        )
        with patch.object(
            INSTALL,
            "save_baseline_digest",
            side_effect=PermissionError("read-only home"),
        ):
            rc_fail = self.invoke(
                [
                    "--clients",
                    "opencode",
                    "--project",
                    str(self.project),
                    "--package-root",
                    str(newer_pkg),
                    "--without-knowledge",
                ]
            )
        self.assertEqual(rc_fail, 2)
        self.assertEqual(INSTALL.package_digest(installed), old_digest)
        self.assertEqual(INSTALL.load_baseline_digest(self.home, installed), old_digest)

        # If a rollback rename itself fails, InstallTransaction preserves the backup on disk!
        target_file = self.root / "preserve-test/item"
        target_file.parent.mkdir(parents=True)
        target_file.write_text("precious\n", encoding="utf-8")
        original_rename = Path.rename

        def fail_restore(self_path, dst):
            if dst == target_file:
                raise OSError("restore blocked")
            return original_rename(self_path, dst)

        with INSTALL.InstallTransaction() as tx:
            slot = tx.stash_existing(target_file)
            tx.record(target_file, slot)
            with patch.object(Path, "rename", fail_restore):
                tx.rollback()
        self.assertTrue(slot.is_file())
        self.assertEqual(slot.read_text(encoding="utf-8"), "precious\n")

    def test_all_commands_have_timeout_and_redact_credentials(self):
        with patch.object(
            INSTALL.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                ["git"], 1, "", "fatal: https://user:secret@example.com/repo.git"
            ),
        ) as mocked_run:
            with self.assertRaises(ValueError) as ctx:
                INSTALL.run(["git", "fetch"])
        self.assertGreater(mocked_run.call_args.kwargs["timeout"], 0)
        self.assertEqual(mocked_run.call_args.kwargs["env"]["GIT_TERMINAL_PROMPT"], "0")
        self.assertNotIn("secret", str(ctx.exception))
        self.assertIn("<redacted>@", str(ctx.exception))

    @unittest.skipUnless(
        BASH and INSTALL_SH and INSTALL_SH.is_file(), "bash or install.sh unavailable"
    )
    def test_bash_entrypoint_help_and_invalid_python_rejection(self):
        help_res = subprocess.run(
            [BASH, INSTALL_SH.as_posix(), "--help"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(help_res.returncode, 0, help_res.stderr)
        self.assertIn("--clients", help_res.stdout)
        self.assertIn("--pinned", help_res.stdout)

        bad_py = subprocess.run(
            [BASH, INSTALL_SH.as_posix(), "--python", "/nonexistent/python3"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(bad_py.returncode, 2)
        self.assertIn("not executable", bad_py.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX symlink & PTY acceptance runs on Linux")
    def test_posix_symlink_security_clients_rollback_and_pty(self):
        # 1) Reject symlink inside source package before copying sensitive target.
        secret_dir = self.root / "secret-dir"
        secret_dir.mkdir()
        (secret_dir / "token.txt").write_text("sensitive", encoding="utf-8")
        poisoned = self.root / "poisoned-skill"
        shutil.copytree(
            SKILL_ROOT,
            poisoned,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        (poisoned / "references/leak").symlink_to(secret_dir, target_is_directory=True)
        rc_poison = self.invoke(
            [
                "--clients",
                "opencode",
                "--project",
                str(self.project),
                "--package-root",
                str(poisoned),
                "--without-knowledge",
            ]
        )
        self.assertEqual(rc_poison, 2)
        self.assertFalse((self.project / ".agents").exists())

        # 2) Full --clients all installation (pure skill + 2 query links + .claude relative links).
        rc_all = self.invoke(
            [
                "--clients",
                "all",
                "--project",
                str(self.project),
                "--package-root",
                str(SKILL_ROOT),
                "--knowledge-root",
                str(self.knowledge),
            ]
        )
        self.assertEqual(rc_all, 0)
        expected_names = {INSTALL.SKILL, *INSTALL.QUERIES}
        for rel in (".agents/skills", ".claude/skills"):
            self.assertEqual(
                {p.name for p in (self.project / rel).iterdir()}, expected_names
            )
        claude_link = self.project / ".claude/skills" / INSTALL.SKILL
        self.assertTrue(claude_link.is_symlink())
        self.assertFalse(Path(os.readlink(claude_link)).is_absolute())

        # 3) Unrelated symlink on query skill is NOT silently overwritten online without --force.
        other_proj = self.root / "other-project"
        (other_proj / ".agents/skills").mkdir(parents=True)
        custom_target = self.root / "user-maintained-query"
        custom_target.mkdir()
        user_link = other_proj / ".agents/skills/knowledge-query"
        user_link.symlink_to(custom_target, target_is_directory=True)
        rc_unrel = self.invoke(
            [
                "--clients",
                "opencode",
                "--project",
                str(other_proj),
                "--package-root",
                str(SKILL_ROOT),
                "--knowledge-root",
                str(self.knowledge),
            ]
        )
        self.assertEqual(rc_unrel, 2)
        self.assertEqual(user_link.resolve(), custom_target.resolve())

        # 4) Pre-existing valid symlink is preserved if a later config write fails.
        proj_rollback = self.root / "rollback-project"
        (proj_rollback / ".agents/skills").mkdir(parents=True)
        valid_link = proj_rollback / ".agents/skills/knowledge-query"
        valid_link.symlink_to(
            self.knowledge / ".agents/skills/knowledge-query", target_is_directory=True
        )
        with patch.object(INSTALL, "write_config", side_effect=OSError("disk full")):
            rc_fail = self.invoke(
                [
                    "--clients",
                    "opencode",
                    "--project",
                    str(proj_rollback),
                    "--package-root",
                    str(SKILL_ROOT),
                    "--knowledge-root",
                    str(self.knowledge),
                ]
            )
        self.assertEqual(rc_fail, 2)
        self.assertTrue(valid_link.is_symlink())
        self.assertFalse(
            (proj_rollback / ".agents/skills/ascendc-api-knowledge-query").exists()
        )
        self.assertFalse((proj_rollback / ".agents/skills/tool-compile-debug").exists())

        # 5) Late failure during upgrade or --force replacement restores displaced directories intact.
        proj_upgrade_rb = self.root / "upgrade-rollback-project"
        proj_upgrade_rb.mkdir()
        self.assertEqual(
            self.invoke(
                [
                    "--clients",
                    "opencode",
                    "--project",
                    str(proj_upgrade_rb),
                    "--package-root",
                    str(SKILL_ROOT),
                    "--without-knowledge",
                ]
            ),
            0,
        )
        old_digest = INSTALL.package_digest(
            proj_upgrade_rb / ".agents/skills/tool-compile-debug"
        )
        unmanaged_query = proj_upgrade_rb / ".agents/skills/knowledge-query"
        unmanaged_query.mkdir(parents=True)
        (unmanaged_query / "custom.md").write_text("keep-me\n", encoding="utf-8")

        newer_pkg = self.root / "newer-rb-skill"
        shutil.copytree(
            SKILL_ROOT,
            newer_pkg,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        (newer_pkg / "references/cannbot-contract.md").write_text(
            (newer_pkg / "references/cannbot-contract.md").read_text(encoding="utf-8")
            + "\n<!-- v2 -->\n",
            encoding="utf-8",
        )
        with patch.object(INSTALL, "write_config", side_effect=OSError("disk full")):
            rc_up_fail = self.invoke(
                [
                    "--force",
                    "--clients",
                    "opencode",
                    "--project",
                    str(proj_upgrade_rb),
                    "--package-root",
                    str(newer_pkg),
                    "--knowledge-root",
                    str(self.knowledge),
                ]
            )
        self.assertEqual(rc_up_fail, 2)
        self.assertEqual(
            INSTALL.package_digest(
                proj_upgrade_rb / ".agents/skills/tool-compile-debug"
            ),
            old_digest,
        )
        self.assertEqual(
            INSTALL.load_baseline_digest(
                self.home, proj_upgrade_rb / ".agents/skills/tool-compile-debug"
            ),
            old_digest,
        )
        self.assertFalse(unmanaged_query.is_symlink())
        self.assertEqual(
            (unmanaged_query / "custom.md").read_text(encoding="utf-8"), "keep-me\n"
        )

        # 6) Coalesced .claude/skills -> ../.agents/skills does not self-symlink or delete payload.
        proj_coalesced = self.root / "coalesced-project"
        (proj_coalesced / ".agents/skills").mkdir(parents=True)
        (proj_coalesced / ".claude").mkdir(parents=True)
        (proj_coalesced / ".claude/skills").symlink_to(
            "../.agents/skills", target_is_directory=True
        )
        rc_coal = self.invoke(
            [
                "--clients",
                "all",
                "--project",
                str(proj_coalesced),
                "--package-root",
                str(SKILL_ROOT),
                "--knowledge-root",
                str(self.knowledge),
            ]
        )
        self.assertEqual(rc_coal, 0)
        coal_skill = proj_coalesced / ".agents/skills/tool-compile-debug"
        self.assertTrue(coal_skill.is_dir())
        self.assertFalse(coal_skill.is_symlink())
        self.assertTrue((coal_skill / "SKILL.md").is_file())
        self.assertTrue(
            (proj_coalesced / ".claude/skills/tool-compile-debug/SKILL.md").is_file()
        )

    @unittest.skipIf(os.name == "nt", "POSIX PTY interactive prompt runs on Linux")
    def test_posix_pty_interactive_install_and_cancel(self):
        import pty
        import select

        if not BASH or not INSTALL_SH or not INSTALL_SH.is_file():
            self.skipTest("bash or install.sh unavailable")

        def run_pty(keystrokes: bytes, extra_args=()):
            master_fd, slave_fd = pty.openpty()
            output_chunks = []
            env = {
                **os.environ,
                "HOME": str(self.home),
                "TERM": "dumb",
                "NO_COLOR": "1",
            }
            env.pop("CANNBOT_KNOWLEDGE_ROOT", None)
            env.pop("CLAUDE_CONFIG_DIR", None)
            proc = subprocess.Popen(
                [
                    BASH,
                    str(INSTALL_SH),
                    "--package-root",
                    str(SKILL_ROOT),
                    "--without-knowledge",
                    *extra_args,
                ],
                cwd=self.project,
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=slave_fd,
                env=env,
                start_new_session=True,
                close_fds=True,
            )
            os.close(slave_fd)
            try:
                os.write(master_fd, keystrokes)
                while proc.poll() is None:
                    ready, _, _ = select.select([master_fd], [], [], 0.2)
                    if ready:
                        try:
                            data = os.read(master_fd, 4096)
                            if data:
                                output_chunks.append(data)
                        except OSError:
                            break
                while True:
                    ready, _, _ = select.select([master_fd], [], [], 0.05)
                    if not ready:
                        break
                    try:
                        data = os.read(master_fd, 4096)
                        if not data:
                            break
                        output_chunks.append(data)
                    except OSError:
                        break
                return proc.wait(timeout=30), b"".join(output_chunks).decode(
                    "utf-8", errors="replace"
                )
            finally:
                os.close(master_fd)

        # Cancel with 'q' at Step 1 -> exit code 130.
        rc_cancel, out_cancel = run_pty(b"q\n")
        self.assertEqual(rc_cancel, 130, out_cancel)
        self.assertFalse((self.project / ".agents").exists())

        # Default Enter (codex/gemini/opencode) -> Default Enter (Current project) -> Default Enter (Proceed? Y).
        rc_ok, out_ok = run_pty(b"\n\n\n")
        self.assertEqual(rc_ok, 0, out_ok)
        self.assertTrue(
            (self.project / ".agents/skills/tool-compile-debug/SKILL.md").is_file()
        )


if __name__ == "__main__":
    unittest.main()
