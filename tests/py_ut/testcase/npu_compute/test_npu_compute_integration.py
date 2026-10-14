# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest


BIN_DIR = Path(
    os.environ.get(
        "NPU_COMPUTE_TEST_BIN_DIR",
        "/tmp/asc_tools_npu_compute_integration/bin",
    )
)
CLI = BIN_DIR / "npu-compute"
APP = BIN_DIR / "npu_compute_stub_demo_app"

HARDWARE_INFO_TEST_APP = r"""
import os
import pathlib
import sys

output = pathlib.Path(os.environ["NPU_COMPUTE_NESTED_MARKER_PATH"]).parent
mode = sys.argv[1]
exit_code = int(sys.argv[2])
hardware_info = output / "HardwareInfo.jsonl"

if mode == "regular":
    pass
else:
    hardware_info.unlink()

if mode == "directory":
    hardware_info.mkdir()
elif mode == "symlink":
    target = output / "HardwareInfo.target"
    target.write_text('{"category":"Host Info"}\n', encoding="utf-8")
    hardware_info.symlink_to(target)
elif mode not in ("missing", "regular"):
    raise SystemExit(f"unknown HardwareInfo test mode: {mode}")

raise SystemExit(exit_code)
"""


def run_collection(
    *app_arguments, sections=("PipeUtilization",), extra_environment=None
):
    environment = os.environ.copy()
    existing_library_path = environment.get("LD_LIBRARY_PATH", "")
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        value for value in (str(BIN_DIR), existing_library_path) if value
    )
    environment["NPU_COMPUTE_DEBUG"] = "1"
    environment["ACL_TOOL_INJECTION_DEBUG"] = "1"
    if extra_environment:
        environment.update(extra_environment)
    started = time.monotonic()
    command = [str(CLI)]
    for section in sections:
        command.extend(("--section", section))
    command.extend((str(APP), *app_arguments))
    result = subprocess.run(
        command,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    return result, time.monotonic() - started


def assert_markers_in_order(output, markers):
    position = 0
    for marker in markers:
        next_position = output.find(marker, position)
        assert next_position >= 0, f"missing marker {marker!r} in:\n{output}"
        position = next_position + len(marker)


def extract_output_path(stderr):
    prefix = "npu-compute: report= "
    for line in stderr.splitlines():
        if line.startswith(prefix):
            return Path(line[len(prefix) :])
    raise AssertionError(f"missing demo output path in:\n{stderr}")


def run_hardware_info_result_app(mode, work_directory, exit_code=0):
    environment = os.environ.copy()
    existing_library_path = environment.get("LD_LIBRARY_PATH", "")
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(
        value for value in (str(BIN_DIR), existing_library_path) if value
    )
    return subprocess.run(
        [
            str(CLI),
            "--section",
            "PipeUtilization",
            str(BIN_DIR / "npu_compute_rep_fixture_app"),
            "--mode",
            "exec",
            "--",
            sys.executable,
            "-c",
            HARDWARE_INFO_TEST_APP,
            mode,
            str(exit_code),
        ],
        env=environment,
        cwd=work_directory,
        text=True,
        capture_output=True,
        check=False,
    )


def test_cli_accepts_regular_hardware_info_file(tmp_path):
    result = run_hardware_info_result_app("regular", tmp_path)

    assert result.returncode == 0, result.stderr
    assert "npu-compute: data-directory=" not in result.stderr
    assert list(tmp_path.glob("npu-compute-*")) == []
    assert len(list(tmp_path.glob("*.npu-rep"))) == 1


def test_cli_packages_remaining_files_without_hardware_info(tmp_path):
    result = run_hardware_info_result_app("missing", tmp_path)

    assert result.returncode == 3
    assert "npu-compute: data-directory=" not in result.stderr
    assert len(list(tmp_path.glob("*.npu-rep"))) == 1
    assert "HardwareInfo.jsonl" in result.stderr


@pytest.mark.parametrize("mode", ("directory", "symlink"))
def test_cli_skips_non_regular_hardware_info_path(mode, tmp_path):
    result = run_hardware_info_result_app(mode, tmp_path)

    assert result.returncode == 3
    assert "npu-compute: data-directory=" not in result.stderr
    assert list(tmp_path.glob("npu-compute-*")) == []
    assert len(list(tmp_path.glob("*.npu-rep"))) == 1
    assert "HardwareInfo.jsonl" in result.stderr


def test_app_failure_takes_priority_over_missing_hardware_info(tmp_path):
    result = run_hardware_info_result_app("missing", tmp_path, exit_code=7)

    assert result.returncode == 7
    assert "npu-compute: data-directory=" not in result.stderr
    assert list(tmp_path.iterdir()) == []
    assert "exited with status 7" in result.stderr
    assert "hardware information was not generated" not in result.stderr


def test_each_collection_publishes_unique_report():
    first, _ = run_collection()
    second, _ = run_collection()

    assert first.returncode == 3, first.stderr
    assert second.returncode == 3, second.stderr
    first_output = extract_output_path(first.stderr)
    second_output = extract_output_path(second.stderr)
    assert first_output.is_absolute()
    assert second_output.is_absolute()
    assert first_output != second_output
    assert first_output.is_file()
    assert second_output.is_file()
    first_output.unlink()
    second_output.unlink()


def test_collection_runs_the_connected_replay_chain():
    result, elapsed = run_collection(
        "--app-value",
        "value-from-user",
        "--sleep-ms",
        "200",
    )

    assert result.returncode == 3, result.stderr
    assert "[demo] argv[1]=--app-value" in result.stderr
    assert "[demo] argv[2]=value-from-user" in result.stderr
    assert result.stderr.count("[aclpti] subscribe result=0") == 1
    completion_marker = "[aclpti] runtime handler registration complete"
    initialization_log, marker, _ = result.stderr.partition(completion_marker)
    assert marker, f"missing completion marker: {completion_marker}"
    for api_id in range(16):
        expected_log = f"[aclpti] register runtime handler apiId={api_id} result=0"
        assert expected_log in initialization_log, (
            f"missing eager registration before completion: {expected_log}"
        )
    assert elapsed >= 0.18
    assert_markers_in_order(
        result.stderr,
        [
            "[aclpti] runtime handler registration complete",
            "[aclpti] subscribe result=0",
            "[libnpu-compute] subscriber initialized",
            "[aclpti] selected section name=PipeUtilization",
            "[libnpu-compute] configured sections=PipeUtilization",
            "[aclpti] shadow malloc",
            "[aclpti] mirror memcpy",
            "[aclpti] prepare replay round=0",
            "[prof_api_stub] MsprofStart",
            "[aclpti] launch replay kernel round=0",
            "[prof_api_stub] MsprofStop",
            "[aclpti] record replay status round=0",
            "[aclpti] release replay round=0",
            "[aclpti] release replay round=1",
            "[aclpti] RangeProfiler data module shutdown result=0",
            "[demo] completed",
        ],
    )


def test_app_nonzero_exit_is_preserved_after_successful_collection():
    result, _ = run_collection("--exit-code", "7")

    assert result.returncode == 7
    assert "[libnpu-compute] subscriber initialized" in result.stderr
    assert "[demo] exiting with requested status 7" in result.stderr
    assert "exited with status 7" in result.stderr


def test_multiple_sections_reach_aclpti_in_order():
    result, _ = run_collection(sections=("Memory", "L2Cache"))

    assert result.returncode == 3, result.stderr
    assert_markers_in_order(
        result.stderr,
        [
            "[aclpti] selected section name=Memory",
            "[aclpti] selected section name=L2Cache",
            "[libnpu-compute] configured sections=Memory,L2Cache",
        ],
    )


def test_all_supported_sections_reach_aclpti_in_order():
    sections = (
        "PipeUtilization",
        "Memory",
        "MemoryL0",
        "MemoryUB",
        "L2Cache",
    )
    joined_sections = ",".join(sections)

    result, _ = run_collection(sections=sections)

    assert result.returncode == 3, result.stderr
    assert result.stderr.count("[aclpti] subscribe result=0") == 1
    assert result.stderr.count("[aclpti] range config result=0") == 1
    assert (
        result.stderr.count(f"[libnpu-compute] configured sections={joined_sections}")
        == 1
    )
    assert_markers_in_order(
        result.stderr,
        [
            *(f"[aclpti] selected section name={section}" for section in sections),
            f"[libnpu-compute] configured sections={joined_sections}",
            "[demo] completed",
        ],
    )
