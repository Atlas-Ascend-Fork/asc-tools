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
import subprocess
import tempfile
from pathlib import Path

import pytest


EXPECTED_KEYS = [
    [
        "category",
        "cpu physical count",
        "cpu logical count",
        "memory total size(MB)",
        "disk total size(GB)",
    ],
    ["category", "npu count", "chip info", "arch info"],
    [
        "category",
        "control cpu count",
        "ai cpu count",
        "ai cpu frequency(MHZ)",
    ],
    [
        "category",
        "ai core count",
        "ai cube count",
        "ai vector count",
        "ai cube frequency(MHZ)",
        "ai vector frequency(MHZ)",
    ],
    ["category", "hbm total(MB)", "hbm used(MB)", "hbm frequency(MHZ)"],
]
EXPECTED_CATEGORIES = [
    "Host Info",
    "Device Info",
    "CPU Information",
    "AI Core Information",
    "Memory Information",
]
REAL_CANN_LIBRARIES = {
    "libascendcl.so",
    "libruntime.so",
    "libprofapi.so",
    "libplatform.so",
}
REAL_DRIVER_LIBRARIES = {"libascend_hal.so", "libdrvdsmi_host.so"}


def _required_path(name):
    value = os.environ.get(name)
    assert value, f"{name} must be set"
    path = Path(value).resolve()
    assert path.exists(), f"{name} does not exist: {path}"
    return path


def _is_under(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _loaded_library_paths(output_directory):
    loader_logs = sorted(output_directory.glob("loader.*"))
    assert loader_logs, f"LD_DEBUG did not create a loader log in {output_directory}"

    loaded = []
    for loader_log in loader_logs:
        for line in loader_log.read_text(errors="replace").splitlines():
            marker = "calling init: "
            if marker not in line:
                continue
            candidate = line.split(marker, 1)[1].strip()
            if candidate.startswith("/"):
                loaded.append(Path(candidate).resolve())
    assert loaded, f"loader logs contain no initialized library paths: {loader_logs}"
    return loaded


def _loaded_matches(paths, library_name):
    return [
        path
        for path in paths
        if path.name == library_name or path.name.startswith(library_name + ".")
    ]


def _assert_real_library_sources(output_directory, app):
    loaded = _loaded_library_paths(output_directory)
    build_root = app.parent.parent.resolve()
    cann_root = _required_path("ASCEND_HOME_PATH")
    library_search_roots = [
        Path(value).resolve()
        for value in os.environ.get("LD_LIBRARY_PATH", "").split(":")
        if value
    ]

    for library_name in sorted(REAL_CANN_LIBRARIES):
        matches = _loaded_matches(loaded, library_name)
        assert matches, f"real CANN library was not loaded: {library_name}"
        assert any(_is_under(path, cann_root) for path in matches), (
            f"{library_name} was not loaded from ASCEND_HOME_PATH: {matches}"
        )

    for library_name in sorted(REAL_DRIVER_LIBRARIES):
        matches = _loaded_matches(loaded, library_name)
        assert matches, f"real Driver library was not loaded: {library_name}"
        assert any(
            _is_under(path, root)
            for path in matches
            for root in library_search_roots
            if not _is_under(root, build_root)
        ), f"{library_name} was not loaded from the configured Driver paths: {matches}"

    for library_name in ("libruntime.so", "libprofapi.so"):
        assert not any(
            _is_under(path, build_root)
            for path in _loaded_matches(loaded, library_name)
        ), f"{library_name} unexpectedly came from the test build"
    assert not _loaded_matches(loaded, "libnpu_compute_hardware_api_stub.so"), (
        "Hardware API Stub was loaded by the real HardwareInfo test"
    )


def _assert_callback_flow(stderr, callback_id):
    assert stderr.count("[acl_pti_callback_stub] subscribe") == 1

    enable_lines = [
        line
        for line in stderr.splitlines()
        if "[acl_pti_callback_stub] enable=1" in line
    ]
    callbacks = (13, 0, 16, 17, 18, 3, 12, 15, 14)
    assert len(enable_lines) == len(callbacks)
    for line, cbid in zip(enable_lines, callbacks):
        assert f"cbid={cbid}" in line
        assert "result=0" in line

    disable_lines = [
        line
        for line in stderr.splitlines()
        if "[acl_pti_callback_stub] enable=0" in line
    ]
    assert len(disable_lines) == len(callbacks)
    for line, cbid in zip(disable_lines, reversed(callbacks)):
        assert f"cbid={cbid}" in line
        assert "result=0" in line

    event = (
        f"[acl_pti_callback_stub] event domain=1 cbid={callback_id} "
        "site=1 retval=0 dispatched=1"
    )
    assert event in stderr
    assert stderr.count("[acl_pti_callback_stub] event domain=1") == 1
    assert (
        f"runtime callback domain=1 cbid={callback_id} site=1 retval=0 accepted=1"
        in stderr
    )
    published = (
        "[real_hardware_callback_app] HardwareInfo.jsonl available after "
        f"callback cbid={callback_id}"
    )
    cleaned = "[real_hardware_callback_app] Device cleanup completed"
    assert stderr.count(published) == 1
    assert stderr.count(cleaned) == 1
    assert stderr.index(published) < stderr.index(cleaned)


def _assert_hardware_info(output_directory):
    jsonl_files = list(output_directory.glob("HardwareInfo*.jsonl"))
    assert len(jsonl_files) == 1, (
        f"expected one HardwareInfo JSONL file, found {jsonl_files}"
    )
    output = jsonl_files[0]
    assert output.is_file() and not output.is_symlink()

    lines = output.read_text(encoding="utf-8").splitlines()
    assert len(lines) == 5
    records = [json.loads(line) for line in lines]
    assert [record["category"] for record in records] == EXPECTED_CATEGORIES
    for record, keys in zip(records, EXPECTED_KEYS):
        assert list(record) == keys

    host, device, cpu, ai_core, memory = records
    assert host["cpu physical count"] >= 0
    assert host["cpu logical count"] >= 0
    assert host["memory total size(MB)"] >= 0
    assert host["disk total size(GB)"] >= 0

    assert device["npu count"] == 1
    assert device["chip info"]
    assert device["arch info"]

    for field in (
        "control cpu count",
        "ai cpu count",
        "ai cpu frequency(MHZ)",
    ):
        assert cpu[field] > 0
    for field in (
        "ai core count",
        "ai cube count",
        "ai vector count",
        "ai cube frequency(MHZ)",
        "ai vector frequency(MHZ)",
    ):
        assert ai_core[field] > 0
    assert memory["hbm total(MB)"] > 0
    assert 0 <= memory["hbm used(MB)"] <= memory["hbm total(MB)"]
    assert memory["hbm frequency(MHZ)"] > 0


@pytest.mark.parametrize("callback_id", [13, 0, 16, 17])
def test_real_hardware_collection_with_callback_stub(callback_id):
    app = _required_path("NPU_COMPUTE_REAL_HW_APP")
    test_library = _required_path("NPU_COMPUTE_REAL_HW_LIBRARY")
    output_root = _required_path("NPU_COMPUTE_REAL_HW_OUTPUT_ROOT")
    assert output_root.is_dir(), f"output root is not a directory: {output_root}"
    output_directory = Path(
        tempfile.mkdtemp(
            prefix=f"npu-compute-real-hardware-cbid-{callback_id}-",
            dir=output_root,
        )
    ).resolve()

    environment = os.environ.copy()
    for name in ("ACL_API_INJECTION", "LD_PRELOAD", "INJECTION_TEST_CALLBACK_EVENTS"):
        environment.pop(name, None)
    environment["NPU_COMPUTE_DEBUG"] = "1"
    environment["LD_DEBUG"] = "libs"
    environment["LD_DEBUG_OUTPUT"] = str(output_directory / "loader")

    result = subprocess.run(
        [str(app), str(test_library), str(output_directory), str(callback_id)],
        text=True,
        capture_output=True,
        env=environment,
        timeout=90,
        check=False,
    )
    assert result.returncode == 0, (
        f"real HardwareInfo driver failed; output retained at {output_directory}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )

    _assert_callback_flow(result.stderr, callback_id)
    assert "[libnpu-compute] HardwareInfo:" not in result.stderr
    _assert_hardware_info(output_directory)
    _assert_real_library_sources(output_directory, app)
