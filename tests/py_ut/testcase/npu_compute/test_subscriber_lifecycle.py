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
import sys
import tempfile
from pathlib import Path

import pytest


@pytest.fixture(autouse=True)
def plog_stdout_environment(monkeypatch):
    monkeypatch.setenv("ASCEND_SLOG_PRINT_TO_STDOUT", "1")
    monkeypatch.setenv("ASCEND_GLOBAL_LOG_LEVEL", "0")


BIN_DIR = Path(
    os.environ.get(
        "NPU_COMPUTE_TEST_BIN_DIR",
        "/tmp/asc_tools_npu_compute_integration/bin",
    )
)

SUPPORTED_SECTIONS = [
    "PipeUtilization",
    "Memory",
    "MemoryL0",
    "MemoryUB",
    "L2Cache",
]

PROBE = r"""
import ctypes
import json
import os
import sys
import threading

bin_dir = sys.argv[1]
call_count = int(sys.argv[2])
sections = sys.argv[3]
output = sys.argv[4]
os.environ["ACL_API_INJECTION"] = os.path.join(bin_dir, "libnpu_compute_processor.so")

ctypes.CDLL(os.path.join(bin_dir, "libprofapi.so"), mode=ctypes.RTLD_GLOBAL)
compute = ctypes.CDLL(
    os.path.join(bin_dir, "libnpu_compute_processor.so"), mode=ctypes.RTLD_GLOBAL
)
runtime = ctypes.CDLL(os.path.join(os.environ["NPU_COMPUTE_TEST_RUNTIME_DIR"], "libruntime.so"))

aclrt_init = runtime.aclrtInit
aclrt_init.argtypes = []
aclrt_init.restype = ctypes.c_int
initialize = compute.acltoolInitialize
initialize.argtypes = []
initialize.restype = ctypes.c_int
shutdown = compute.acltoolShutdown
shutdown.argtypes = []
shutdown.restype = ctypes.c_int

initial_result = aclrt_init()
barrier = threading.Barrier(call_count)
results = [None] * call_count


def invoke(index):
    barrier.wait()
    results[index] = initialize()


threads = [threading.Thread(target=invoke, args=(index,)) for index in range(call_count)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()

shutdown_results = [shutdown(), shutdown()]
print(json.dumps({
    "initial_result": initial_result,
    "results": results,
    "shutdown_results": shutdown_results,
    "initialize_after_shutdown": initialize(),
}))
"""


def execute_probe(sections, call_count=16):
    environment = os.environ.copy()
    environment["NPU_COMPUTE_DEBUG"] = "1"
    with tempfile.TemporaryDirectory(prefix="npu-compute-subscriber-test-") as output:
        result = subprocess.run(
            [
                str(BIN_DIR / "npu-compute"),
                *[
                    value
                    for section in sections.split(",")
                    for value in ("--section", section)
                ],
                sys.executable,
                "-c",
                PROBE,
                str(BIN_DIR),
                str(call_count),
                sections,
                output,
            ],
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        result.stderr = result.stdout + result.stderr
    if result.stdout:
        assert result.returncode == 3, (
            result.stderr
        )  # Initialization alone produces no PMU data.
        return result, json.loads(
            next(
                line
                for line in reversed(result.stdout.splitlines())
                if line.lstrip().startswith("{")
            )
        )
    assert result.returncode == 2, result.stderr
    return result, None


def run_probe(sections, call_count=16):
    _, probe = execute_probe(sections, call_count)
    return probe


def test_concurrent_entry_returns_cached_success():
    probe = run_probe("PipeUtilization")

    assert probe["initial_result"] == 0
    assert probe["results"] == [0] * 16
    # Final draining belongs to ACLPTI teardown, so early shutdown is rejected.
    assert probe["shutdown_results"] == [-1, -1]
    assert probe["initialize_after_shutdown"] == 0


def test_concurrent_entry_returns_cached_configuration_failure():
    probe = run_probe("Unknown", call_count=8)

    assert probe is None


@pytest.mark.parametrize("section", SUPPORTED_SECTIONS)
def test_each_supported_section_initializes_successfully(section):
    result, probe = execute_probe(section, call_count=1)

    assert probe == {
        "initial_result": 0,
        "results": [0],
        "shutdown_results": [-1, -1],
        "initialize_after_shutdown": 0,
    }
    assert result.stderr.count(f"[aclpti] selected section name={section}") == 1
    assert f"[libnpu-compute] configured sections={section}" in result.stderr


@pytest.mark.parametrize(
    "sections",
    ("", "HardwareInfo", "Unknown", ",Memory", "Memory,,L2Cache", "Memory,"),
)
def test_invalid_sections_fail_before_subscribe(sections):
    result, probe = execute_probe(sections, call_count=1)

    assert probe is None
    assert "[aclpti] subscribe result=" not in result.stderr
    assert "[libnpu-compute] subscriber initialized" not in result.stderr


def test_duplicate_sections_are_forwarded_once_in_first_occurrence_order():
    result, probe = execute_probe("Memory,Memory,L2Cache", call_count=1)

    assert probe == {
        "initial_result": 0,
        "results": [0],
        "shutdown_results": [-1, -1],
        "initialize_after_shutdown": 0,
    }
    memory = "[aclpti] selected section name=Memory"
    l2_cache = "[aclpti] selected section name=L2Cache"
    assert result.stderr.count(memory) == 1
    assert result.stderr.count(l2_cache) == 1
    assert result.stderr.index(memory) < result.stderr.index(l2_cache)
    assert "[aclpti] requested sections=2" in result.stderr
    assert "[libnpu-compute] configured sections=Memory,L2Cache" in result.stderr
