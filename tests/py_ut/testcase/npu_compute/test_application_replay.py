# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""CPU-only CLI/processor/ACLPTI/injection integration, using the Runtime test backend."""

import json
import os
from pathlib import Path
import select
import subprocess

import pytest


@pytest.mark.parametrize(
    "scenario",
    [
        "reset",
        "reset-finalize",
        "no-reset",
        "return-with-raw",
        "exit-with-raw",
        "finalize-no-reset",
        "partial-reset-finalize",
        "restart-finalize",
        "finalize-no-device",
        "silent-finalize-error",
        "reference-only",
        "reference-finalize",
        "cleanup-failure",
        "restart",
        "no-device",
    ],
)
@pytest.mark.parametrize("launch", ["direct", "shell"])
def test_application_lifecycle_and_report_delivery(tmp_path, scenario, launch):
    binary_dir = os.environ.get("NPU_COMPUTE_TEST_BIN_DIR")
    probe = os.environ.get("NPU_COMPUTE_APPLICATION_PROBE")
    if not binary_dir or not probe:
        pytest.skip(
            "requires CPU integration binaries and NPU_COMPUTE_APPLICATION_PROBE"
        )
    app = (
        [probe, scenario]
        if launch == "direct"
        else ["/bin/sh", "-c", '"$@"', "probe", probe, scenario]
    )
    result = subprocess.run(
        [
            str(Path(binary_dir) / "npu-compute"),
            "--replay-mode",
            "application",
            "--section",
            "Communication",
            "--no-pack",
            "--export",
            str(tmp_path),
            *app,
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    output = result.stdout + result.stderr
    finalized = "finalize" in scenario
    assert result.returncode == (0 if finalized else 3), output
    assert "application-probe:ok" in output, output
    assert ("aclFinalize() may be missing" in output) == (not finalized), output
    assert not list(tmp_path.rglob("application_manifest.json"))
    assert not list(tmp_path.rglob("application_raw.jsonl"))
    assert not list(tmp_path.rglob("task-tracks.jsonl"))
    assert not list(tmp_path.rglob(".communication-staging"))
    assert len(list(tmp_path.rglob("CommunicationTrace.json"))) == 1
    devices = set() if "no-device" in scenario else {0, 1}
    hardware_files = list(tmp_path.rglob("HardwareInfo.jsonl"))
    assert len(hardware_files) == 1
    hardware = [json.loads(line) for line in hardware_files[0].read_text().splitlines()]
    if devices:
        device_info = [row for row in hardware if row["category"] == "Device Info"]
        assert {row.get("Device Id") for row in device_info} == set(devices)
        assert all(row["npu count"] == 8 for row in device_info)
    assert not list(tmp_path.rglob("summary.jsonl"))


def test_packed_report_omits_diagnostic_artifacts(tmp_path):
    binary_dir = os.environ.get("NPU_COMPUTE_TEST_BIN_DIR")
    probe = os.environ.get("NPU_COMPUTE_APPLICATION_PROBE")
    if not binary_dir or not probe:
        pytest.skip(
            "requires CPU integration binaries and NPU_COMPUTE_APPLICATION_PROBE"
        )
    cli = str(Path(binary_dir) / "npu-compute")
    packed = tmp_path / "application.npu-rep"
    collect = subprocess.run(
        [
            cli,
            "--replay-mode",
            "application",
            "--section",
            "Communication",
            "--export",
            str(packed),
            probe,
            "finalize-no-reset",
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert collect.returncode == 0, collect.stdout + collect.stderr
    assert packed.is_file()
    restored = tmp_path / "restored"
    restored.mkdir()
    imported = subprocess.run(
        [cli, "--import", str(packed), "--export", str(restored)],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert imported.returncode == 0, imported.stdout + imported.stderr
    assert len(list(restored.rglob("CommunicationTrace.json"))) == 1
    assert len(list(restored.rglob("app_communication_summary.jsonl"))) == 1
    assert not list(restored.rglob("app_summary.jsonl"))
    assert not list(restored.rglob("application_manifest.json"))
    assert not list(restored.rglob("application_raw.jsonl"))
    assert not list(restored.rglob(".communication-staging"))


@pytest.mark.parametrize("pack", [False, True])
@pytest.mark.parametrize(
    "scenario,launches_per_device",
    [
        ("summary-finalize", 70),
        ("large-summary-finalize", 500),
        ("launch-variants-finalize", 70),
    ],
)
def test_per_launch_summary_multidevice_pack_and_import(
    tmp_path, pack, scenario, launches_per_device
):
    binary_dir = os.environ.get("NPU_COMPUTE_TEST_BIN_DIR")
    probe = os.environ.get("NPU_COMPUTE_APPLICATION_PROBE")
    if not binary_dir or not probe:
        pytest.skip("requires CPU integration binaries")
    cli = str(Path(binary_dir) / "npu-compute")
    output = tmp_path / "output"
    output.mkdir()
    options = [] if pack else ["--no-pack"]
    result = subprocess.run(
        [
            cli,
            "--replay-mode",
            "application",
            "--section",
            "Communication",
            *options,
            "--export",
            str(output),
            probe,
            scenario,
        ],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    if pack:
        report = next(output.rglob("*.npu-rep"))
        restored = tmp_path / "restored"
        restored.mkdir()
        result = subprocess.run(
            [cli, "--import", str(report), "--export", str(restored)],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        output = restored
    summaries = list(output.rglob("summary.jsonl"))
    assert len(summaries) == 2 * launches_per_device
    identities = set()
    for path in summaries:
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        assert len(rows) == 1
        row = rows[0]
        assert row["category"] == "OpInfoSummary"
        assert row["Op Name"] == 'comm/"kernel'
        assert row["collection_status"] == "complete"
        assert row["communication_time"] == "NA"
        assert row["Task Id"] != "NA"
        assert row["Stream Id"] == 3
        if scenario == "launch-variants-finalize":
            assert row["Task Id"] == (
                row["Device Id"] * launches_per_device + row["launch_index"] + 1
            )
        identities.add((row["Device Id"], row["launch_index"]))
    assert identities == {
        (device, index) for device in (0, 1) for index in range(launches_per_device)
    }
    hardware = [
        json.loads(line)
        for line in next(output.rglob("HardwareInfo.jsonl")).read_text().splitlines()
    ]
    assert {
        row["Device Id"] for row in hardware if row["category"] == "Device Info"
    } == {0, 1}
    assert all(
        row["npu count"] == 8 for row in hardware if row["category"] == "Device Info"
    )


@pytest.fixture
def application_binaries():
    binary_dir = os.environ.get("NPU_COMPUTE_TEST_BIN_DIR")
    probe = os.environ.get("NPU_COMPUTE_APPLICATION_PROBE")
    if not binary_dir or not probe:
        pytest.skip("requires CPU integration binaries")
    cli = str(Path(binary_dir) / "npu-compute")
    return cli, probe


def application_command(cli, probe, output, scenario, pack=False):
    return [
        cli,
        "--replay-mode",
        "application",
        "--section",
        "Communication",
        *([] if pack else ["--no-pack"]),
        "--export",
        str(output),
        "--",
        probe,
        scenario,
    ]


@pytest.mark.parametrize(
    "scenario,exit_code,complete_reports,pack",
    [
        ("fork-finalize", 0, 2, False),
        ("fork-finalize", 0, 2, True),
        ("fork-disconnect", 3, 1, False),
        ("fork-app-failure", 17, 2, False),
    ],
)
def test_forked_application_sessions(
    tmp_path, application_binaries, scenario, exit_code, complete_reports, pack
):
    cli, probe = application_binaries
    output = tmp_path / "ranks.npu-rep" if pack else tmp_path
    result = subprocess.run(
        application_command(cli, probe, output, scenario, pack),
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert result.returncode == exit_code, result.stdout + result.stderr
    if pack:
        restored = tmp_path / "restored"
        restored.mkdir()
        imported = subprocess.run(
            [cli, "--import", str(output), "--export", str(restored)],
            capture_output=True,
            text=True,
            timeout=20,
        )
        assert imported.returncode == 0, imported.stdout + imported.stderr
        output = restored
    reports = list(output.rglob("CommunicationTrace.json"))
    assert len(reports) == complete_reports, result.stdout + result.stderr
    assert len({path.parent.name for path in reports}) == complete_reports
    assert all(path.parent.name.startswith("pid-") for path in reports)
    summaries = [
        json.loads(path.with_name("app_communication_summary.jsonl").read_text())
        for path in reports
    ]
    assert {row["Device Id"] for row in summaries} == set(range(complete_reports))


@pytest.mark.parametrize(
    "scenario,pack,device_groups",
    [
        ("communication-finalize", False, [{0, 1}]),
        ("fork-communication", False, [{0}, {1}]),
        ("fork-communication", True, [{0}, {1}]),
    ],
)
def test_communication_artifacts_follow_application_sessions(
    tmp_path, application_binaries, scenario, pack, device_groups
):
    cli, probe = application_binaries
    output = tmp_path / "communication.npu-rep" if pack else tmp_path
    result = subprocess.run(
        application_command(cli, probe, output, scenario, pack),
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    if pack:
        restored = tmp_path / "restored"
        restored.mkdir()
        imported = subprocess.run(
            [cli, "--import", str(output), "--export", str(restored)],
            capture_output=True,
            text=True,
            timeout=20,
        )
        assert imported.returncode == 0, imported.stdout + imported.stderr
        output = restored

    traces = list(output.rglob("CommunicationTrace.json"))
    assert len(traces) == len(device_groups)
    assert not list(output.rglob("app_summary.jsonl"))
    assert not list(output.rglob("application_manifest.json"))
    assert not list(output.rglob("application_raw.jsonl"))
    assert not list(output.rglob(".communication-staging"))
    observed_groups = []
    for path in traces:
        summary = path.with_name("app_communication_summary.jsonl")
        device_ids = {
            json.loads(line)["Device Id"] for line in summary.read_text().splitlines()
        }
        observed_groups.append(device_ids)
        if len(device_groups) > 1:
            assert path.parent.name.startswith("pid-")
        trace = json.loads(path.read_text())
        tasks = [event for event in trace if event["name"] == "AiCpu-Comm-Task"]
        assert len(tasks) == len(device_ids)
    assert {frozenset(ids) for ids in observed_groups} == {
        frozenset(ids) for ids in device_groups
    }


def test_forked_application_finalize_waits_for_its_own_report(
    tmp_path, application_binaries
):
    cli, probe = application_binaries
    paused_r, paused_w = os.pipe()
    release_r, release_w = os.pipe()
    done_r, done_w = os.pipe()
    env = dict(
        os.environ,
        ASCEND_GLOBAL_LOG_LEVEL="0",
        NPU_COMPUTE_TEST_REPORT_PAUSED_FD=str(paused_w),
        NPU_COMPUTE_TEST_REPORT_RELEASE_FD=str(release_r),
        NPU_COMPUTE_TEST_SIBLING_DONE_FD=str(done_w),
    )
    try:
        with subprocess.Popen(
            application_command(cli, probe, tmp_path, "fork-gate"),
            env=env,
            pass_fds=(paused_w, release_r, done_w),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        ) as proc:
            try:
                for fd in (paused_r, done_r):
                    assert select.select([fd], [], [], 10)[0]
                    assert os.read(fd, 1) == b"x"
                assert proc.poll() is None
                assert os.write(release_w, b"x") == 1
                output = proc.communicate(timeout=15)[0]
                assert proc.returncode == 0, output
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    proc.communicate(timeout=10)
    finally:
        for fd in (paused_r, paused_w, release_r, release_w, done_r, done_w):
            os.close(fd)
    assert len(list(tmp_path.rglob("CommunicationTrace.json"))) == 2


def test_finalize_returns_with_runtime_hook_active(tmp_path, application_binaries):
    cli, probe = application_binaries
    result = subprocess.run(
        application_command(cli, probe, tmp_path, "finalize-busy"),
        capture_output=True,
        text=True,
        timeout=15,
    )
    assert result.returncode == 3, result.stdout + result.stderr
    assert len(list(tmp_path.rglob("CommunicationTrace.json"))) == 1
