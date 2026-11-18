#!/usr/bin/env python3
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""用途：验证构建失败立即停止、错误码传播和日志留存，无需安装 CANN。"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SKILL_ROOT = Path(__file__).resolve().parents[1]
BUILD_ONE = SKILL_ROOT / "scripts/engine/harness/runtime/build_one.sh"
BASH = "C:/Program Files/Git/bin/bash.exe" if os.name == "nt" else shutil.which("bash")
COMMAND_TIMEOUT_SECONDS = 20
CONFIGURE_FAILURE = 31
BUILD_FAILURE = 32
RECIPE_FAILURE = 37
METADATA_FAILURE = 43
TEE_FAILURE = 73


@unittest.skipUnless(BASH and Path(BASH).is_file(), "bash unavailable")
class BuildOneFailureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="build-one-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "project with spaces"
        self.project.mkdir()
        (self.project / "CMakeLists.txt").write_text("project(fixture)\n")
        self.toolkit = self.root / "selected toolkit"
        self.toolkit.mkdir()
        (self.toolkit / "version.info").write_text("Version=9.0.0\n")
        self.bindir = self.root / "bin"
        self.bindir.mkdir()
        self.calls = self.root / "calls.log"
        self.env = {
            key: value
            for key, value in os.environ.items()
            if key
            not in {
                "SOC_VERSION",
                "CMAKE_ASC_ARCHITECTURES",
                "ASC_DIR",
                "ASCEND_TOOLKIT_HOME",
                "CMAKE_PREFIX_PATH",
                "BASH_ENV",
            }
        }
        self.env.update(
            ASCEND_CANN_PACKAGE_PATH=self.toolkit.as_posix(),
            PATH=self.bindir.as_posix() + os.pathsep + self.env["PATH"],
            BUILD_TEST_BIN=self.bindir.as_posix(),
            CALL_LOG=self.calls.as_posix(),
            CONFIGURE_STATUS="0",
            BUILD_STATUS="0",
        )
        # Keep these shell-control tests independent of SDK/Python discovery.
        # The engine's metadata parsing has its own tests.
        self.write_tool("python3", 'printf "9.0.0\\n"\n')
        self.write_tool(
            "cmake",
            'if [ "$1" = "-S" ]; then\n'
            '  echo configure >> "$CALL_LOG"\n'
            '  echo "configure stdout"\n'
            '  echo "configure stderr" >&2\n'
            '  echo "fixture cache" > build/CMakeCache.txt\n'
            '  exit "$CONFIGURE_STATUS"\n'
            'elif [ "$1" = "--build" ]; then\n'
            '  echo build >> "$CALL_LOG"\n'
            '  echo "build output"\n'
            '  exit "$BUILD_STATUS"\n'
            "fi\nexit 99\n",
        )

    def write_tool(self, name, body):
        path = self.bindir / name
        path.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8", newline="\n")
        path.chmod(0o755)

    def run_build(self, project=None):
        return subprocess.run(
            [
                BASH,
                "-c",
                'export PATH="$(cd "$BUILD_TEST_BIN" && pwd):$PATH"; exec bash "$@"',
                "build-one-test",
                BUILD_ONE.as_posix(),
                (project or self.project).as_posix(),
            ],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=COMMAND_TIMEOUT_SECONDS,
            check=False,
        )

    def assert_status(self, result, expected):
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def test_configure_failure_stops_build_and_keeps_logs(self):
        self.env["CONFIGURE_STATUS"] = str(CONFIGURE_FAILURE)
        result = self.run_build()
        self.assert_status(result, CONFIGURE_FAILURE)
        self.assertEqual(self.calls.read_text().splitlines(), ["configure"])
        log = (self.project / "logs/build.log").read_text(encoding="utf-8")
        self.assertIn("configure stdout", log)
        self.assertIn("configure stderr", log)
        self.assertEqual(
            (self.project / "logs/CMakeCache.txt").read_text(), "fixture cache\n"
        )

    def test_build_failure_is_preserved(self):
        self.env["BUILD_STATUS"] = str(BUILD_FAILURE)
        result = self.run_build()
        self.assert_status(result, BUILD_FAILURE)
        self.assertEqual(self.calls.read_text().splitlines(), ["configure", "build"])
        self.assertIn("build output", (self.project / "logs/build.log").read_text())

    def test_success_runs_both_stages(self):
        self.assert_status(self.run_build(), 0)
        self.assertEqual(self.calls.read_text().splitlines(), ["configure", "build"])

    def test_original_recipe_failure_is_preserved(self):
        (self.project / "build.sh").write_text(
            f"echo recipe-error >&2\nexit {RECIPE_FAILURE}\n", newline="\n"
        )
        self.assert_status(self.run_build(), RECIPE_FAILURE)
        self.assertFalse(self.calls.exists())
        self.assertIn("recipe-error", (self.project / "logs/build.log").read_text())

    def test_missing_project_stops_before_creating_outputs(self):
        result = self.run_build(self.root / "missing project")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("=== CASE_DIR=", result.stdout)
        self.assertFalse(self.calls.exists())
        self.assertFalse((self.root / "build").exists())

    def test_output_directory_failure_stops_before_build(self):
        (self.project / "build").write_text("occupied\n")
        result = self.run_build()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("=== CASE_DIR=", result.stdout)
        self.assertFalse(self.calls.exists())

    def test_metadata_failure_stops_before_configure(self):
        self.write_tool(
            "python3", f"echo metadata-error >&2\nexit {METADATA_FAILURE}\n"
        )
        self.assert_status(self.run_build(), METADATA_FAILURE)
        self.assertFalse(self.calls.exists())
        self.assertIn("metadata-error", (self.project / "logs/build.log").read_text())

    def test_tee_failure_does_not_report_success(self):
        # Drain input before failing, so SIGPIPE cannot replace the fixture status.
        self.write_tool("tee", f'cat > "$1"\nexit {TEE_FAILURE}\n')
        self.assert_status(self.run_build(), TEE_FAILURE)
        self.assertEqual(self.calls.read_text().splitlines(), ["configure", "build"])


if __name__ == "__main__":
    unittest.main()
