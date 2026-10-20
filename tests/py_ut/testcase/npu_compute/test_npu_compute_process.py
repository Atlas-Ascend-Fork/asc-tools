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
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest


@pytest.fixture(autouse=True)
def collection_work_directory(tmp_path, monkeypatch):
    # Collection writes reports in cwd; isolate each test from the checkout.
    monkeypatch.chdir(tmp_path)


BUILD_DIR = Path(
    os.environ.get("NPU_COMPUTE_BUILD_DIR", "/tmp/asc_tools_npu_compute_integration")
)
BIN_DIR = Path(os.environ.get("NPU_COMPUTE_TEST_BIN_DIR", str(BUILD_DIR / "bin")))
CLI = BIN_DIR / "npu-compute"
BASE_COMMAND = [str(CLI), "--section", "Memory"]
NESTED_COLLECTION_ERROR = "nested npu-compute collection is not supported"
NESTED_COLLECTION_DETECTION_ERROR = "nested collection detection failed"
COLLECTION_ACTIVE_ENVIRONMENT = "NPU_COMPUTE_COLLECTION_ACTIVE"
NESTED_COLLECTION_MARKER = ".npu-compute-nested-collection"
FIXTURE = BIN_DIR / "npu_compute_rep_fixture_app"


def collecting_program(*arguments):
    return [str(FIXTURE), "--mode", "exec", "--", *arguments]


def run_tool(*arguments, cwd=None, env=None):
    assert CLI.is_file(), f"npu-compute was not built: {CLI}"
    return subprocess.run(
        [str(CLI), *arguments],
        cwd=cwd,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def run_cli(*program_and_arguments, cwd=None, env=None, collect=False):
    if collect:
        program_and_arguments = collecting_program(*program_and_arguments)
    return run_tool(*BASE_COMMAND[1:], *program_and_arguments, cwd=cwd, env=env)


def wait_for_file(path, process, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and path.stat().st_size > 0:
            return
        if process.poll() is not None:
            raise AssertionError(f"npu-compute exited early with {process.returncode}")
        time.sleep(0.01)
    raise AssertionError(f"timed out waiting for {path}")


def process_identity_program(identity_path, sleep_seconds):
    code = (
        "import os, pathlib, sys, time; " + "pathlib.Path(sys.argv[1]).write_text("
        "f'{os.getpid()} {os.getppid()}', encoding='utf-8'); "
        "time.sleep(float(sys.argv[2]))"
    )
    return collecting_program(
        sys.executable, "-c", code, str(identity_path), str(sleep_seconds)
    )


def nested_collection_command():
    code = "pass"
    return [str(CLI), "--section", "Memory", sys.executable, "-c", code]


def nested_collection_script(tmp_path, swallow_failure):
    script = tmp_path / "nested_collection.py"
    exit_code = "0" if swallow_failure else "result.returncode"
    script.write_text(
        "import subprocess, sys\n"
        f"result = subprocess.run({json.dumps(nested_collection_command())}, check=False)\n"
        f"sys.exit({exit_code})\n",
        encoding="utf-8",
    )
    return [sys.executable, str(script)]


def assert_nested_collection_rejected(result, tmp_path):
    assert result.returncode == 3
    assert NESTED_COLLECTION_ERROR in result.stderr
    assert "hardware information was not generated" not in result.stderr
    assert list(tmp_path.glob("*.npu-rep")) == []

    assert "npu-compute: data-directory=" not in result.stderr
    assert list(tmp_path.glob("npu-compute-*")) == []


def test_cli_remains_parent_until_app_exits(tmp_path):
    identity_path = tmp_path / "identity.txt"
    process = subprocess.Popen(
        [*BASE_COMMAND, *process_identity_program(identity_path, 0.3)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        wait_for_file(identity_path, process)
        app_pid, app_parent_pid = map(int, identity_path.read_text().split())

        assert process.poll() is None
        assert app_pid != process.pid
        assert app_parent_pid == process.pid
        assert process.wait(timeout=5) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def test_app_exit_status_is_preserved():
    result = run_cli("/bin/sh", "-c", "exit 7")

    assert result.returncode == 7
    assert "exited with status 7" in result.stderr


def test_program_not_found_returns_127():
    program = "/definitely/not/a/real/npu-compute-program"
    result = run_cli(program)

    assert result.returncode == 127
    assert (
        f"failed to start program '{program}': No such file or directory"
        in result.stderr
    )


def test_program_not_executable_returns_126(tmp_path):
    program = tmp_path / "not-executable"
    program.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    program.chmod(0o644)

    result = run_cli(str(program))

    assert result.returncode == 126
    assert (
        f"failed to start program '{program}': not an executable file" in result.stderr
    )


def test_directory_keeps_permission_denied_error(tmp_path):
    result = run_cli(str(tmp_path))

    assert result.returncode == 126
    assert f"failed to start program '{tmp_path}': Permission denied" in result.stderr


def test_child_environment_is_overridden_without_changing_parent(monkeypatch):
    monkeypatch.setenv("NPU_COMPUTE_REPLAY_MODE", "parent-value")
    code = (
        "import json, os; print(json.dumps({"
        "'replay': os.environ.get('NPU_COMPUTE_REPLAY_MODE'), "
        "'injection': os.environ['ACL_API_INJECTION']}))"
    )
    result = run_cli(sys.executable, "-c", code, env=os.environ.copy(), collect=True)
    assert result.returncode == 0, result.stderr
    child_environment = json.loads(result.stdout)
    assert child_environment["replay"] is None
    assert Path(child_environment["injection"]).is_absolute()
    assert os.environ["NPU_COMPUTE_REPLAY_MODE"] == "parent-value"


def test_program_is_found_through_path(tmp_path):
    program = tmp_path / "npu-compute-path-probe"
    program.write_text(
        "#!/bin/sh\nexit 0\n",
        encoding="utf-8",
    )
    program.chmod(0o755)
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join((str(tmp_path), environment.get("PATH", "")))

    result = run_cli(program.name, env=environment, collect=True)

    assert result.returncode == 0


def test_direct_nested_collection_is_rejected(tmp_path):
    result = run_cli(*nested_collection_command(), cwd=tmp_path, collect=True)

    assert_nested_collection_rejected(result, tmp_path)


@pytest.mark.parametrize("swallow_failure", (False, True))
def test_nested_collection_started_by_script_is_rejected(swallow_failure, tmp_path):
    result = run_cli(
        *nested_collection_script(tmp_path, swallow_failure), cwd=tmp_path, collect=True
    )

    assert_nested_collection_rejected(result, tmp_path)


def test_nested_collection_removes_nonempty_data_directory(tmp_path):
    script = tmp_path / "nested_collection_with_partial_data.py"
    script.write_text(
        "import os, pathlib, subprocess\n"
        f"subprocess.run({json.dumps(nested_collection_command())}, check=False)\n"
        "pathlib.Path(pathlib.Path(os.environ['NPU_COMPUTE_NESTED_MARKER_PATH']).parent, 'partial.csv').write_text("
        "'name,value\\npartial,1\\n', encoding='utf-8')\n",
        encoding="utf-8",
    )

    result = run_cli(sys.executable, str(script), cwd=tmp_path, collect=True)

    assert result.returncode == 3
    assert NESTED_COLLECTION_ERROR in result.stderr
    assert "npu-compute: data-directory=" not in result.stderr
    assert list(tmp_path.glob("npu-compute-*")) == []


@pytest.mark.parametrize("arguments", (("--help",), ("--list-sections",)))
def test_non_collection_commands_ignore_active_collection_marker(arguments, tmp_path):
    environment = os.environ.copy()
    environment[COLLECTION_ACTIVE_ENVIRONMENT] = "1"

    result = run_tool(*arguments, cwd=tmp_path, env=environment)

    assert result.returncode == 0
    assert NESTED_COLLECTION_ERROR not in result.stderr


def test_nested_marker_creation_failure_prevents_app_launch(tmp_path):
    app_marker = tmp_path / "app-ran"
    environment = os.environ.copy()
    environment[COLLECTION_ACTIVE_ENVIRONMENT] = "1"
    environment["NPU_COMPUTE_NESTED_MARKER_PATH"] = str(tmp_path / "missing/marker")

    result = run_cli("/usr/bin/touch", str(app_marker), cwd=tmp_path, env=environment)

    assert result.returncode == 5
    assert NESTED_COLLECTION_DETECTION_ERROR in result.stderr
    assert not app_marker.exists()
    assert list(tmp_path.glob("*.npu-rep")) == []


def test_nested_marker_removal_failure_prevents_report_packaging(tmp_path):
    code = (
        "import os, pathlib; "
        + "pathlib.Path(os.environ['NPU_COMPUTE_NESTED_MARKER_PATH']).mkdir()"
    )

    result = run_cli(sys.executable, "-c", code, cwd=tmp_path, collect=True)

    assert result.returncode == 5
    assert NESTED_COLLECTION_DETECTION_ERROR in result.stderr
    assert list(tmp_path.glob("*.npu-rep")) == []


def test_sigterm_is_forwarded_and_child_is_reaped(tmp_path):
    identity_path = tmp_path / "signal-identity.txt"
    process = subprocess.Popen(
        [*BASE_COMMAND, *process_identity_program(identity_path, 30)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    app_pid = None
    try:
        wait_for_file(identity_path, process)
        app_pid = int(identity_path.read_text().split()[0])
        process.send_signal(signal.SIGTERM)

        stdout, stderr = process.communicate(timeout=5)
        assert stdout == ""
        assert process.returncode == 128 + signal.SIGTERM
        assert "signal 15" in stderr
        with pytest.raises(ProcessLookupError):
            os.kill(app_pid, 0)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if app_pid is not None:
            try:
                os.kill(app_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


@pytest.mark.parametrize(
    ("termination_signal", "description"),
    [(signal.SIGSEGV, "Segmentation fault"), (signal.SIGTERM, "Terminated")],
)
def test_child_signal_reports_description_and_number(
    tmp_path, termination_signal, description
):
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    result = run_cli(
        sys.executable,
        "-c",
        "import os, resource, signal; "
        "resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); "
        f"os.kill(os.getpid(), {termination_signal.value})",
        cwd=tmp_path,
        env=environment,
    )

    assert result.returncode == 128 + termination_signal
    assert (
        f"APP terminated by signal {description} ({termination_signal.value})"
        in result.stderr
    )
    assert list(tmp_path.glob("*.npu-rep")) == []
