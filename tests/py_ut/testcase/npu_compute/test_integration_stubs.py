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


REPO_ROOT = Path(__file__).resolve().parents[4]
CPP_TEST_ROOT = REPO_ROOT / "tests/ut/testcase/npu_tools"
BIN_DIR = Path(
    os.environ.get(
        "NPU_COMPUTE_TEST_BIN_DIR",
        "/tmp/asc_tools_npu_compute_integration/bin",
    )
)

PROBE = r"""
import ctypes
import json
import os
import sys

bin_dir = sys.argv[1]
output = sys.argv[2]
os.environ["ACL_API_INJECTION"] = os.path.join(bin_dir, "libnpu_compute_processor.so")

ctypes.CDLL(os.path.join(bin_dir, "libprofapi.so"), mode=ctypes.RTLD_GLOBAL)
runtime = ctypes.CDLL(os.path.join(os.environ["NPU_COMPUTE_TEST_RUNTIME_DIR"], "libruntime.so"))

aclrt_init = runtime.aclrtInit
aclrt_init.argtypes = []
aclrt_init.restype = ctypes.c_int
aclrt_malloc = runtime.aclrtMalloc
aclrt_malloc.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_size_t, ctypes.c_int]
aclrt_malloc.restype = ctypes.c_int
aclrt_memset = runtime.aclrtMemset
aclrt_memset.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_size_t]
aclrt_memset.restype = ctypes.c_int
aclrt_free = runtime.aclrtFree
aclrt_free.argtypes = [ctypes.c_void_p]
aclrt_free.restype = ctypes.c_int

init_result = aclrt_init()
device_ptr = ctypes.c_void_p()
malloc_result = aclrt_malloc(ctypes.byref(device_ptr), 64, 0)
memset_result = -1
free_result = -1
if malloc_result == 0 and device_ptr.value:
    memset_result = aclrt_memset(device_ptr, 64, 0, 64)
    free_result = aclrt_free(device_ptr)

print(json.dumps({
    "init_result": init_result,
    "malloc_result": malloc_result,
    "memset_result": memset_result,
    "free_result": free_result,
}))
"""


def test_prof_api_loads_only_the_formal_compute_entry():
    source = (
        CPP_TEST_ROOT / "injection/tests/stubs/prof_api/prof_api_stub.cpp"
    ).read_text(encoding="utf-8")

    assert 'dlsym(handle, "acltoolInitialize")' in source
    assert "NpuComputeInit" not in source


def test_prof_api_constants_are_private_to_the_stub():
    public_header = REPO_ROOT / "npu_tools/npu_compute/include/npu_compute/prof_api.h"
    prof_common = (
        CPP_TEST_ROOT / "injection/tests/stubs/prof_api/include/profiling/prof_common.h"
    ).read_text(encoding="utf-8")
    prof_stub = (
        CPP_TEST_ROOT / "injection/tests/stubs/prof_api/prof_api_stub.cpp"
    ).read_text(encoding="utf-8")

    assert not public_header.exists()
    assert "#define COMPUTE_AICORE_METRICS_NUM 10" in prof_common
    assert "#define MSPROF_INVALID_AICORE_METRIC UINT32_MAX" in prof_common
    assert "PROF_CONFIG_ATTR_AICORE_METRICS = 0" in prof_common
    assert "PROF_CONFIG_ATTR_INSTR = 1" in prof_common
    assert "struct MsprofConfigAttr {\n    uint32_t id;" in prof_common
    assert prof_common.count("struct MsprofConfigInfo {") == 1
    assert "size_t numAttrs;\n    const struct MsprofConfigAttr* attrs;" in prof_common
    assert "COMPUTE_INVALID_AICORE_METRIC_EVENT" not in prof_common
    assert "MSPROF_AICOREMETRICS" not in prof_common
    assert "npucompute::prof" not in prof_common
    assert "npucompute::prof" not in prof_stub


def test_runtime_registers_and_runs_through_connected_backends():
    with tempfile.TemporaryDirectory(prefix="npu-compute-stub-test-") as output:
        result = subprocess.run(
            [
                str(BIN_DIR / "npu-compute"),
                "--section",
                "PipeUtilization",
                sys.executable,
                "-c",
                PROBE,
                str(BIN_DIR),
                output,
            ],
            env=os.environ.copy(),
            text=True,
            capture_output=True,
            check=False,
        )

    # No kernel is launched; the session ends normally with no report data.
    assert result.returncode == 3, result.stderr
    assert json.loads(result.stdout) == {
        "init_result": 0,
        "malloc_result": 0,
        "memset_result": 0,
        "free_result": 0,
    }
