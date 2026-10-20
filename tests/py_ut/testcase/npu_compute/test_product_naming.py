# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
LEGACY_NAMES = (
    "cann" + "_compute",
    "cann" + "-compute",
    "lib" + "cann" + "-compute",
    "CANN" + "_COMPUTE_",
    "Cann" + "Compute",
)
TEXT_SUFFIXES = {
    ".cc",
    ".cmake",
    ".cpp",
    ".h",
    ".md",
    ".py",
    ".txt",
}
TARGET_DECLARATION = re.compile(r"add_(?:library|executable)\(\s*([^\s)]+)")


def test_npu_compute_product_naming_is_consistent():
    product_root = REPO_ROOT / "npu_tools/npu_compute"
    assert product_root.is_dir()
    assert not (REPO_ROOT / ("cann" + "_compute")).exists()

    files = [REPO_ROOT / "CMakeLists.txt"]
    files.extend(
        path
        for root in (product_root, REPO_ROOT / "tests")
        for path in root.rglob("*")
        if path.is_file()
        and "third_party" not in path.relative_to(REPO_ROOT).parts
        and (path.name == "CMakeLists.txt" or path.suffix in TEXT_SUFFIXES)
    )

    violations = []
    for path in files:
        content = path.read_text(encoding="utf-8")
        for legacy_name in LEGACY_NAMES:
            if legacy_name in content:
                violations.append(f"{path.relative_to(REPO_ROOT)}: {legacy_name}")

    assert violations == []


def test_npu_compute_build_and_log_names_are_consistent():
    product_cmake = (REPO_ROOT / "npu_tools/npu_compute" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    library_cmake = (
        REPO_ROOT / "npu_tools/npu_compute" / "src" / "compute" / "CMakeLists.txt"
    ).read_text(encoding="utf-8")
    cli_cmake = (
        REPO_ROOT / "npu_tools/npu_compute" / "src" / "cli" / "CMakeLists.txt"
    ).read_text(encoding="utf-8")
    cli_main = (
        REPO_ROOT / "npu_tools/npu_compute" / "src" / "cli" / "main.cpp"
    ).read_text(encoding="utf-8")
    library_source = (
        REPO_ROOT
        / "npu_tools/npu_compute"
        / "src"
        / "compute"
        / "runtime"
        / "npu_compute_runtime.cpp"
    ).read_text(encoding="utf-8")

    assert "project(npu_compute LANGUAGES CXX)" in product_cmake
    assert "add_library(npu_compute SHARED" in library_cmake
    assert "OUTPUT_NAME npu_compute_processor" in library_cmake
    assert "add_executable(npu_compute_cli" in cli_cmake
    assert "OUTPUT_NAME npu-compute" in cli_cmake
    assert '"[ERROR] npu-compute: %s\\n"' in cli_main
    assert '"[libnpu-compute]' in library_source


def test_package_build_includes_npu_compute_and_sanitizer():
    top_level_cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    npu_tools_cmake = (REPO_ROOT / "npu_tools" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    sanitizer_cmake = (REPO_ROOT / "npu_tools/npu_check" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    build_script = (REPO_ROOT / "build.sh").read_text(encoding="utf-8")
    test_script = (
        REPO_ROOT / "tests/ut/testcase/npu_tools/npu_compute/run_tests.sh"
    ).read_text(encoding="utf-8")

    for build_file in (top_level_cmake, npu_tools_cmake, build_script, test_script):
        assert "ASC_TOOLS_BUILD_NPU_COMPUTE" not in build_file

    assert (
        'set(NPU_SANITIZER_INSTALL_BASE_DIR "${CMAKE_SYSTEM_PROCESSOR}-linux")'
        in sanitizer_cmake
    )
    assert (
        'set(NPU_SANITIZER_INSTALL_BINDIR "${NPU_SANITIZER_INSTALL_BASE_DIR}/bin")'
        in sanitizer_cmake
    )
    assert (
        'set(NPU_SANITIZER_INSTALL_INTERNAL_DIR "tools/npu_tools")' in sanitizer_cmake
    )
    assert "install(TARGETS npu_check_cli" in sanitizer_cmake
    assert "install(TARGETS npu_check acl_san" in sanitizer_cmake

    for submodule in ("src/cli", "src/processor", "src/acl_san"):
        submodule_cmake = (
            REPO_ROOT / "npu_tools/npu_check" / submodule / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        assert "install(" not in submodule_cmake

    assert "add_subdirectory(npu_tools)" in top_level_cmake
    for subdirectory in ("injection", "npu_compute", "npu_check"):
        assert f"add_subdirectory({subdirectory})" in npu_tools_cmake

    sanitizer_subdirectories = (
        "src/shared",
        "src/acl_san",
        "src/cli",
        "src/processor",
    )
    subdirectory_positions = [
        sanitizer_cmake.index(f"add_subdirectory({subdirectory})")
        for subdirectory in sanitizer_subdirectories
    ]
    assert subdirectory_positions == sorted(subdirectory_positions)


def test_sanitizer_uses_shared_npu_tools_output_directories():
    sanitizer_cmake = (REPO_ROOT / "npu_tools/npu_check/CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    shared_library_output_block = sanitizer_cmake.split(
        "foreach(target IN ITEMS acl_san npu_check)", 1
    )[1].split("endforeach()", 1)[0]
    cli_output_block = sanitizer_cmake.split(
        "set_target_properties(npu_check_cli PROPERTIES", 1
    )[1].split(")", 1)[0]

    assert "NPU_SANITIZER_LIBRARY_OUTPUT_DIR" not in sanitizer_cmake
    assert "NPU_SANITIZER_RUNTIME_OUTPUT_DIR" not in sanitizer_cmake
    assert (
        'LIBRARY_OUTPUT_DIRECTORY "${NPU_TOOLS_LIBRARY_OUTPUT_DIR}"'
        in shared_library_output_block
    )
    assert (
        'ARCHIVE_OUTPUT_DIRECTORY "${NPU_TOOLS_LIBRARY_OUTPUT_DIR}"'
        not in shared_library_output_block
    )
    assert (
        'RUNTIME_OUTPUT_DIRECTORY "${NPU_TOOLS_RUNTIME_OUTPUT_DIR}"' in cli_output_block
    )


def test_npu_tools_cmake_is_free_of_test_ownership():
    forbidden_markers = (
        "ENABLE_TEST",
        "BUILD_TEST",
        "TEST_",
        "add_test(",
        "enable_testing(",
        "acl_runtime_stub",
        "acl_prof_api_stub",
        "NPU_COMPUTE_ENABLE_TEST_CONTROLS",
    )
    product_cmake_files = sorted((REPO_ROOT / "npu_tools").rglob("CMakeLists.txt"))

    assert product_cmake_files
    for cmake_file in product_cmake_files:
        content = cmake_file.read_text(encoding="utf-8")
        assert not any(marker in content for marker in forbidden_markers), cmake_file


def test_npu_compute_test_ownership_is_in_tests_directory():
    npu_tools_tests_cmake = (
        REPO_ROOT / "tests/ut/testcase/npu_tools/CMakeLists.txt"
    ).read_text(encoding="utf-8")
    npu_compute_tests_cmake = (
        REPO_ROOT / "tests/ut/testcase/npu_tools/npu_compute/tests/CMakeLists.txt"
    ).read_text(encoding="utf-8")

    assert "add_subdirectory(npu_compute/tests)" in npu_tools_tests_cmake
    assert "add_library(acl_pti_test" in npu_compute_tests_cmake
    assert "add_library(npu_compute_test" in npu_compute_tests_cmake
    assert "add_library(npu_compute_launcher_core_test" in npu_compute_tests_cmake


def test_cmake_targets_use_component_prefixes_and_merge_data_module():
    product_root = REPO_ROOT / "npu_tools/npu_compute"
    product_cmake = (product_root / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "NPU_COMPUTE_BUILD_DEMO" not in product_cmake
    assert "add_subdirectory(demo)" not in product_cmake

    # Standalone smoke examples define their own demo targets outside the product build.
    cmake_files = [product_root / "CMakeLists.txt"] + sorted(
        (product_root / "src").rglob("CMakeLists.txt")
    )
    cmake_content = "\n".join(path.read_text(encoding="utf-8") for path in cmake_files)
    targets = TARGET_DECLARATION.findall(cmake_content)
    local_targets = [target for target in targets if "::" not in target]

    assert local_targets
    invalid_targets = [
        target
        for target in local_targets
        if not (target.startswith(("acl_", "npu_compute_")) or target == "npu_compute")
    ]
    assert not invalid_targets, f"Unexpected product target names: {invalid_targets}"
    assert "asc_cc_" not in cmake_content

    acl_pti_cmake = (product_root / "src" / "acl_pti" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    assert "add_library(acl_pti SHARED" in acl_pti_cmake
    assert "data/api.cpp" in acl_pti_cmake
    assert "data/profiling_data_manager.cpp" in acl_pti_cmake
    assert "data/data_processor.cpp" in acl_pti_cmake
    assert "data/data_receiver.cpp" in acl_pti_cmake
    assert "data/raw_data_decoder.cpp" in acl_pti_cmake
    assert "add_library(acl_pti_data_module_impl" not in cmake_content
    assert "OUTPUT_NAME pti_data_module_impl" not in cmake_content


def test_runtime_handlers_use_specific_flat_names():
    acl_pti_source = REPO_ROOT / "npu_tools/npu_compute" / "src" / "acl_pti"
    handler_source = acl_pti_source / "handler"
    header = handler_source / "runtime_api_handlers.h"
    source = handler_source / "runtime_api_handlers.cpp"
    replay_runtime_header = acl_pti_source / "profiling" / "replay_runtime.h"

    assert not (acl_pti_source / "runtime_replacement").exists()
    assert not (acl_pti_source / "replacement").exists()
    assert header.is_file()
    assert source.is_file()
    assert replay_runtime_header.is_file()

    header_content = header.read_text(encoding="utf-8")
    assert "namespace aclpti::handler" in header_content
    assert "class RuntimeApiHandlers" not in header_content
    assert "bool RegisterRuntimeApiHandlers();" in header_content
    assert "ReplayMemory*" not in header_content
    assert "RangeProfiler*" not in header_content

    replay_runtime_content = replay_runtime_header.read_text(encoding="utf-8")
    assert "class ReplayRuntime" in replay_runtime_content
    assert "ReplayMemory replayMemory_;" in replay_runtime_content
    assert "RangeProfiler rangeProfiler_;" in replay_runtime_content
    assert "ReplayRuntime& GetReplayRuntime();" in replay_runtime_content

    assert not (acl_pti_source / "manager.h").exists()
    assert not (acl_pti_source / "manager.cpp").exists()
    assert (acl_pti_source / "initialization.h").is_file()
    assert (acl_pti_source / "initialization.cpp").is_file()
