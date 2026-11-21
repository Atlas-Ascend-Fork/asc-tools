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
    "scenario,records",
    [
        ("reset", 2),
        ("reset-finalize", 2),
        ("no-reset", 0),
        ("return-with-raw", 2),
        ("exit-with-raw", 2),
        ("finalize-no-reset", 2),
        ("partial-reset-finalize", 2),
        ("restart-finalize", 3),
        ("finalize-no-device", 0),
        ("silent-finalize-error", 2),
        ("reference-only", 0),
        ("reference-finalize", 2),
        ("cleanup-failure", 2),
        ("restart", 3),
        ("no-device", 0),
    ],
)
@pytest.mark.parametrize("launch", ["direct", "shell"])
def test_application_lifecycle_and_raw_delivery(tmp_path, scenario, records, launch):
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
    manifests = list(tmp_path.rglob("application_manifest.json"))
    assert len(manifests) == 1, output
    manifest = json.loads(manifests[0].read_text())
    assert manifest["localQueueDrained"]
    assert manifest["processId"] > 0
    assert manifest["passId"] == 0
    assert manifest["tailConfirmed"] == finalized
    assert manifest["complete"] == finalized
    assert manifest["endReason"] == (
        "acl_finalize" if finalized else "process_exit_without_finalize"
    )
    assert manifest["failureReason"] == (
        None if finalized else "acl_finalize_not_observed"
    )
    assert manifest["processedRecords"] == records
    assert manifest["processingErrors"] == manifest["droppedRecords"] == 0
    devices = {item["deviceId"]: item for item in manifest["devices"]}
    assert set(devices) == (set() if "no-device" in scenario else {0, 1})
    missing_reset = scenario in (
        "no-reset",
        "reference-only",
        "cleanup-failure",
        "return-with-raw",
        "exit-with-raw",
    )
    state = "active" if missing_reset else "inactive"
    assert all(item["state"] == state for item in devices.values())
    assert manifest["localCollectionHealthy"] == (not missing_reset)
    for device_id, device in devices.items():
        stopped = scenario in (
            "reset",
            "reset-finalize",
            "restart",
            "restart-finalize",
        ) or (scenario == "partial-reset-finalize" and device_id == 0)
        assert device["stopAttempted"] == stopped
        assert device["stopStatus"] == (0 if stopped else None)
        assert device["endReason"] == (
            "reset_stop"
            if stopped
            else "runtime_finalize"
            if finalized
            else "unconfirmed"
        )
    assert devices.get(0, {}).get("deviceEpoch", 0) == (
        2 if scenario in ("restart", "restart-finalize") else bool(devices)
    )
    raw_file = manifests[0].with_name("application_raw.jsonl")
    raw = [json.loads(line) for line in raw_file.read_text().splitlines()]
    assert raw[0]["recordType"] == "metadata"
    assert raw[0]["processId"] == manifest["processId"]
    assert all(row["passId"] == 0 for row in raw)
    assert len(raw) == records + 1
    assert all(row["payloadHex"] == "726177" for row in raw[1:])
    assert {row["deviceId"] for row in raw[1:]} == (set(devices) if records else set())
    hardware_files = list(tmp_path.rglob("HardwareInfo.jsonl"))
    assert len(hardware_files) == 1
    hardware = [json.loads(line) for line in hardware_files[0].read_text().splitlines()]
    if devices:
        device_info = [row for row in hardware if row["category"] == "Device Info"]
        assert {row.get("Device Id") for row in device_info} == set(devices)
        assert all(row["npu count"] == 8 for row in device_info)
    assert not list(tmp_path.rglob("summary.jsonl"))


def test_raw_artifacts_survive_pack_and_import(tmp_path):
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
    manifest = next(restored.rglob("application_manifest.json"))
    assert json.loads(manifest.read_text())["processedRecords"] == 2
    raw = [
        json.loads(line)
        for line in manifest.with_name("application_raw.jsonl").read_text().splitlines()
    ]
    assert len(raw) == 3


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
    manifest_path = next(output.rglob("application_manifest.json"))
    manifest = json.loads(manifest_path.read_text())
    assert manifest["complete"]
    assert len(manifest["result_units"]) == 2 * launches_per_device
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
    reports = [
        (path, json.loads(path.read_text()))
        for path in output.rglob("application_manifest.json")
    ]
    reports = [(path, manifest) for path, manifest in reports if manifest["complete"]]
    assert len(reports) == complete_reports, result.stdout + result.stderr
    assert len({manifest["processId"] for _, manifest in reports}) == complete_reports
    assert {manifest["devices"][0]["deviceId"] for _, manifest in reports} == set(
        range(complete_reports)
    )
    for path, manifest in reports:
        assert path.parent.name == f"pid-{manifest['processId']}"
        assert manifest["tailConfirmed"] and manifest["localQueueDrained"]
        assert manifest["processedRecords"] == 1
        raw = path.with_name("application_raw.jsonl")
        assert json.loads(raw.read_text().splitlines()[1])["payloadHex"] == "726177"


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

    manifests = list(output.rglob("application_manifest.json"))
    assert len(manifests) == len(device_groups)
    observed_groups = []
    for path in manifests:
        manifest = json.loads(path.read_text())
        assert manifest["complete"] and manifest["tailConfirmed"]
        device_ids = {device["deviceId"] for device in manifest["devices"]}
        observed_groups.append(device_ids)
        if len(device_groups) > 1:
            assert path.parent.name == f"pid-{manifest['processId']}"
        trace = json.loads(path.with_name("CommunicationTrace.json").read_text())
        tasks = [event for event in trace if event["name"] == "AiCpu-Comm-Task"]
        assert len(tasks) == len(device_ids)
        assert path.with_name("app_summary.jsonl").is_file()
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
    assert len(list(tmp_path.rglob("application_manifest.json"))) == 2


def test_finalize_returns_with_runtime_hook_active(tmp_path, application_binaries):
    cli, probe = application_binaries
    result = subprocess.run(
        application_command(cli, probe, tmp_path, "finalize-busy"),
        capture_output=True,
        text=True,
        timeout=15,
    )
    assert result.returncode == 3, result.stdout + result.stderr
    manifests = list(tmp_path.rglob("application_manifest.json"))
    assert len(manifests) == 1, result.stdout + result.stderr
    manifest = json.loads(manifests[0].read_text())
    assert manifest["localQueueDrained"]
    assert not manifest["complete"]
    assert manifest["failureReason"] == "acl_finalize_with_inflight_access"
