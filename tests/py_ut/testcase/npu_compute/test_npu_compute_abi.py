# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
PUBLIC_HEADER = REPO_ROOT / "npu_tools/npu_compute/include/npu_compute/npu_compute.h"
INJECTION_HOOK_HEADER = (
    REPO_ROOT / "npu_tools/injection/include/injection/injection_hook.h"
)
ACLPTI_RUNTIME_API_HEADER = (
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/runtime_api.h"
)
ACLPTI_RUNTIME_API_USERS = (
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/profiling/replay_memory.cpp",
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/profiling/range_profiler.cpp",
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/handler/runtime_api_handlers.cpp",
)
ACLPTI_SOURCE = REPO_ROOT / "npu_tools/npu_compute/src/acl_pti"
ACLPTI_MANAGER_HEADER = ACLPTI_SOURCE / "manager.h"
ACLPTI_MANAGER_SOURCE = ACLPTI_SOURCE / "manager.cpp"
ACLPTI_INITIALIZATION_HEADER = ACLPTI_SOURCE / "initialization.h"
ACLPTI_INITIALIZATION_SOURCE = ACLPTI_SOURCE / "initialization.cpp"
ACLPTI_CALLBACK_API_SOURCE = ACLPTI_SOURCE / "callback/api.cpp"
ACLPTI_CMAKE = ACLPTI_SOURCE / "CMakeLists.txt"
ACLPTI_PROFILING_API_SOURCE = ACLPTI_SOURCE / "profiling/api.cpp"
REPLAY_RUNTIME_HEADER = ACLPTI_SOURCE / "profiling/replay_runtime.h"
PRODUCT_CMAKE = REPO_ROOT / "npu_tools/npu_compute/CMakeLists.txt"
LIBRARY_SOURCE = REPO_ROOT / "npu_tools/npu_compute/src/compute/npu_compute.cpp"
LIBRARY_CMAKE = REPO_ROOT / "npu_tools/npu_compute/src/compute/CMakeLists.txt"
INJECTION_CMAKE = REPO_ROOT / "npu_tools/injection/CMakeLists.txt"
RANGE_PROFILER_SOURCE = (
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/profiling/range_profiler.cpp"
)
REPLAY_MEMORY_HEADER = (
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/profiling/replay_memory.h"
)
REPLAY_MEMORY_SOURCE = (
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/profiling/replay_memory.cpp"
)
RUNTIME_HANDLERS_SOURCE = (
    REPO_ROOT / "npu_tools/npu_compute/src/acl_pti/handler/runtime_api_handlers.cpp"
)
SECTION_CONFIG_HEADER = (
    REPO_ROOT / "npu_tools/npu_compute/src/compute/runtime/section_config.h"
)
SECTION_CONFIG_SOURCE = (
    REPO_ROOT / "npu_tools/npu_compute/src/compute/runtime/section_config.cpp"
)
VERSION_SCRIPT = REPO_ROOT / "npu_tools/npu_compute/src/compute/libnpu_compute.map"
CLI_CMAKE = REPO_ROOT / "npu_tools/npu_compute/src/cli/CMakeLists.txt"
CLI_LAUNCHER = REPO_ROOT / "npu_tools/npu_compute/src/cli/launch/launcher.cpp"
INJECTION_PATH_HEADER = (
    REPO_ROOT / "npu_tools/npu_compute/src/cli/launch/injection_path.h"
)
INJECTION_PATH_SOURCE = (
    REPO_ROOT / "npu_tools/npu_compute/src/cli/launch/injection_path.cpp"
)


def test_injection_library_declares_lifecycle_exports():
    header = PUBLIC_HEADER.read_text(encoding="utf-8")
    source = LIBRARY_SOURCE.read_text(encoding="utf-8")

    assert '#ifdef __cplusplus\nextern "C" {' in header
    assert "NPU_COMPUTE_EXPORT int acltoolInitialize();" in header
    assert "NPU_COMPUTE_EXPORT int acltoolShutdown();" in header

    obsolete_exports = (
        "NpuComputeInit",
        "NpuComputeStartProfiling",
        "NpuComputeInjectionLibraryName",
        "NpuComputeInjectionLibraryPath",
    )
    for symbol in obsolete_exports:
        assert symbol not in header
        assert symbol not in source


def test_aclpti_uses_original_runtime_c_api_directly():
    public_header = INJECTION_HOOK_HEADER.read_text(encoding="utf-8")

    assert "GetOriginalRuntimeFunction" not in public_header
    assert not ACLPTI_RUNTIME_API_HEADER.exists()
    for source_path in ACLPTI_RUNTIME_API_USERS:
        source = source_path.read_text(encoding="utf-8")
        assert "GetOriginalRuntimeFunction" not in source
        assert "acltoolGetOriginalRuntimeApi" in source


def test_range_profiler_resolves_the_device_it_uses():
    handlers = RUNTIME_HANDLERS_SOURCE.read_text(encoding="utf-8")
    range_profiler = RANGE_PROFILER_SOURCE.read_text(encoding="utf-8")

    assert "aclrtGetDevice" not in handlers
    assert "aclrtGetDevice" in range_profiler


def test_replay_memory_resolves_the_restore_memcpy_it_uses():
    header = REPLAY_MEMORY_HEADER.read_text(encoding="utf-8")
    source = REPLAY_MEMORY_SOURCE.read_text(encoding="utf-8")
    range_profiler = RANGE_PROFILER_SOURCE.read_text(encoding="utf-8")
    restore_body = source.split("aclptiResult ReplayMemory::Restore", 1)[1].split(
        "bool ReplayMemory::FindShadowBuffer", 1
    )[0]

    assert "Restore() const" in header
    assert "Restore(aclrtMemcpyFunc" not in header
    assert "acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy)" in restore_body
    assert "replayMemory.Restore()" in range_profiler


def test_aclpti_uses_a_focused_initialization_module():
    assert not ACLPTI_MANAGER_HEADER.exists()
    assert not ACLPTI_MANAGER_SOURCE.exists()
    assert ACLPTI_INITIALIZATION_HEADER.is_file()
    assert ACLPTI_INITIALIZATION_SOURCE.is_file()

    initialization_header = ACLPTI_INITIALIZATION_HEADER.read_text(encoding="utf-8")
    initialization_source = ACLPTI_INITIALIZATION_SOURCE.read_text(encoding="utf-8")
    callback_api = ACLPTI_CALLBACK_API_SOURCE.read_text(encoding="utf-8")
    replay_runtime_header = REPLAY_RUNTIME_HEADER.read_text(encoding="utf-8")
    cmake = ACLPTI_CMAKE.read_text(encoding="utf-8")

    assert "namespace aclpti::initialization" in initialization_header
    assert "aclptiResult InitializeDependencies();" in initialization_header
    assert "static std::mutex initializationMutex" in initialization_source
    assert "static bool initialized = false" in initialization_source
    assert "GetReplayRuntime().Initialize()" in initialization_source
    assert "bool initialized_ = false;" in replay_runtime_header
    assert 'include "acl_pti/initialization.h"' in callback_api
    assert "initialization::InitializeDependencies()" in callback_api
    assert "initialization.cpp" in cmake
    assert "manager.cpp" not in cmake.split()


def test_range_config_calls_replay_runtime_directly():
    profiling_api = ACLPTI_PROFILING_API_SOURCE.read_text(encoding="utf-8")

    assert "GetManager" not in profiling_api
    assert "GetReplayRuntime().SetConfig" in profiling_api


def test_replay_runtime_owns_the_profiling_health_policy():
    header = REPLAY_RUNTIME_HEADER.read_text(encoding="utf-8")
    source = (ACLPTI_SOURCE / "profiling/replay_runtime.cpp").read_text(
        encoding="utf-8"
    )
    handlers = RUNTIME_HANDLERS_SOURCE.read_text(encoding="utf-8")
    public_interface = header.split("public:", 1)[1].split("private:", 1)[0]
    private_implementation = header.split("private:", 1)[1]

    assert "ProfilingAvailable" not in public_interface
    assert "aclptiResult StopProfiling();" in public_interface
    assert "ProfilingAvailable" in private_implementation
    assert ".ProfilingAvailable()" not in handlers
    assert "GetReplayRuntime().StopProfiling()" in handlers
    assert "MapProfilingResult(profiling::ReplayRuntime&" not in handlers
    assert "ProfilingAvailable()" in source
    assert "StopProfiling()" in source


def test_unavailable_replay_runtime_reports_profiling_failure_to_acl_callers():
    source = (ACLPTI_SOURCE / "profiling/replay_runtime.cpp").read_text(
        encoding="utf-8"
    )
    handlers = RUNTIME_HANDLERS_SOURCE.read_text(encoding="utf-8")

    for method, next_method in (
        ("MirrorMalloc", "MirrorFree"),
        ("MirrorMemcpy", "MirrorMemset"),
        ("MirrorMemset", "ReplayKernel"),
        ("ReplayKernel", "ProfilingAvailable"),
    ):
        implementation = source.split(f"ReplayRuntime::{method}", 1)[1].split(
            f"ReplayRuntime::{next_method}", 1
        )[0]
        unavailable_branch = implementation.split("if (!ProfilingAvailable())", 1)[
            1
        ].split("}", 1)[0]
        assert "return ACLPTI_ERROR_PROFILING_FAILED;" in unavailable_branch

    assert (
        "status == ACLPTI_ERROR_RESULT_UNRELIABLE ? ACL_ERROR_INTERNAL_ERROR : ACL_ERROR_PROFILING_FAILURE"
        in handlers
    )


def test_injection_library_uses_an_export_allowlist():
    cmake = INJECTION_CMAKE.read_text(encoding="utf-8")
    header = INJECTION_HOOK_HEADER.read_text(encoding="utf-8")

    assert "add_library(acl_tool_injection SHARED" in cmake
    assert "ACL_TOOL_INJECTION_BUILD=1" in cmake
    assert "CXX_VISIBILITY_PRESET hidden" in cmake
    assert "install(TARGETS acl_tool_injection" in cmake
    assert "ACL_TOOL_INJECTION_EXPORT" in header


def test_cli_resolves_injection_path_without_linking_injection_library():
    cmake = CLI_CMAKE.read_text(encoding="utf-8")
    launcher = CLI_LAUNCHER.read_text(encoding="utf-8")

    assert INJECTION_PATH_HEADER.is_file()
    assert INJECTION_PATH_SOURCE.is_file()
    assert "injection_path.cpp" in cmake
    assert "PRIVATE npu_compute_headers npu_compute" not in cmake
    assert 'include "launch/injection_path.h"' in launcher
    assert "ResolveInjectionLibraryPath" in launcher
    assert "NpuComputeInjectionLibraryPath" not in launcher


def test_product_cmake_uses_cann_runtime_and_profapi():
    cmake = INJECTION_CMAKE.read_text(encoding="utf-8")
    npu_tools_tests_cmake = (
        REPO_ROOT / "tests/ut/testcase/npu_tools/CMakeLists.txt"
    ).read_text(encoding="utf-8")

    assert "INJECTION_BUILD_TESTS" not in cmake
    backend_block = cmake.split("add_library(injection_runtime_backend INTERFACE)", 1)[
        1
    ].split("find_package(Threads REQUIRED)", 1)[0]
    assert "add_subdirectory(injection/tests/stubs/runtime)" in npu_tools_tests_cmake
    assert "add_subdirectory(injection/tests/stubs/prof_api)" in npu_tools_tests_cmake

    assert "find_library(INJECTION_ACL_RT_LIBRARY" in backend_block
    assert "find_library(INJECTION_PROFAPI_LIBRARY" in backend_block
    assert "Injection::acl_rt" in backend_block
    assert "Injection::profapi" in backend_block
    assert "acl_runtime_stub" not in backend_block
    assert "acl_prof_api_stub" not in backend_block


def test_replay_waits_after_msprof_start_before_launching_kernel():
    source = RANGE_PROFILER_SOURCE.read_text(encoding="utf-8")

    assert source.index("MsprofStart(") < source.index(
        "launchStatus = launchFunction(function, pipeline);"
    )


def test_section_parameter_storage_uses_stable_pointer_vector():
    header = SECTION_CONFIG_HEADER.read_text(encoding="utf-8")
    source = SECTION_CONFIG_SOURCE.read_text(encoding="utf-8")
    compact_header = "".join(header.split())
    compact_source = "".join(source.split())

    assert "std::vector<constchar*>section_pointers_;" in compact_header
    assert "aclptiRangeProfilerSetConfigParamsparams_{};" in compact_header
    assert "params_.sections=section_pointers_.data();" in compact_source
    assert "params_.numSections=section_pointers_.size();" in compact_source
    assert "ParamsDeleter" not in header
    assert "params_->numSection" not in source
