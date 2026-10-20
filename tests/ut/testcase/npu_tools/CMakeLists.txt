# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

set(NPU_TOOLS_TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NPU_TOOLS_PY_TEST_ROOT "${CMAKE_SOURCE_DIR}/tests/py_ut/testcase")
set(NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
# Host DBI tests emulate the CANN runtime and toolchain layout under this build-local root.
set(NPU_TOOLS_TEST_CANN_ROOT "${CMAKE_CURRENT_BINARY_DIR}/fake-cann")
set(NPU_TOOLS_TEST_CANN_RUNTIME_DIR "${NPU_TOOLS_TEST_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64")
set(INJECTION_SOURCE_DIR "${CMAKE_SOURCE_DIR}/npu_tools/injection")
set(NPU_COMPUTE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/npu_tools/npu_compute")
set(NPU_COMPUTE_PY_TEST_DIR "${NPU_TOOLS_PY_TEST_ROOT}/npu_compute")

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(injection_set_test_output target)
  set_target_properties(${target} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
  )
endfunction()

function(npu_compute_set_test_output target)
  set_target_properties(${target} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
  )
endfunction()

# 测试专用 INTERFACE 目标（仅构建树使用、不安装），直接使用普通路径；
# $<BUILD_INTERFACE:...> 在 INTERFACE→INTERFACE 链式消费时会被部分 CMake 版本
# 重导出为损坏的相对路径（本仓验证环境 CMake 3.28 即如此）。
add_library(injection_test_headers INTERFACE)
target_include_directories(injection_test_headers INTERFACE
  "${INJECTION_SOURCE_DIR}/include"
  "${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/runtime/include"
  "${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/prof_api/include"
  "${INJECTION_ACL_INCLUDE_DIR}"
)

add_library(npu_compute_test_headers INTERFACE)
target_compile_definitions(npu_compute_test_headers INTERFACE _GLIBCXX_USE_CXX11_ABI=0)
target_include_directories(npu_compute_test_headers INTERFACE
  "${NPU_COMPUTE_SOURCE_DIR}/include"
  "${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/runtime/include"
  "${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/prof_api/include"
  "${NPUCOMPUTE_ACL_INCLUDE_DIR}"
)
target_link_libraries(npu_compute_test_headers INTERFACE injection_test_headers)

add_subdirectory(injection/tests/stubs/prof_api)
add_subdirectory(injection/tests/stubs/runtime)
add_subdirectory(injection/tests)

add_subdirectory(npu_compute/stubs/acl_pti_callback)
add_subdirectory(npu_compute/stubs/hardware_api)
add_subdirectory(npu_compute/tests)

add_subdirectory(npu_sanitizer/common/tests)
add_subdirectory(npu_sanitizer/sanitizer_api/tests)
add_subdirectory(npu_sanitizer/npu_check/tests)

# npu_tools 迁移模块统一 GTest 大用例（构建即运行，对齐 tikcpp_utest 的 run_llt_test 机制）
add_subdirectory(llt)

add_dependencies(npu_compute_test_artifacts npu_tools_utest)

# Execute each dedicated GTest binary once, with its output streamed directly.
# The unified LLT retains its original POST_BUILD execution during normal builds.
# Keep the runner source here; only the generated script belongs in the build tree.
string(CONFIGURE [=[
set(_npu_compute_stub_library_path
  "@NPU_TOOLS_TEST_CANN_RUNTIME_DIR@:@NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR@")
if(DEFINED ENV{LD_LIBRARY_PATH} AND NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
  string(APPEND _npu_compute_stub_library_path ":$ENV{LD_LIBRARY_PATH}")
endif()
set(ENV{LD_LIBRARY_PATH} "${_npu_compute_stub_library_path}")

if(NPU_COMPUTE_PRINT_UNIFIED)
  execute_process(COMMAND "@CMAKE_COMMAND@" -E echo "$<TARGET_FILE:npu_tools_utest>")
  return()
endif()

function(run_gtest executable working_directory timeout)
  execute_process(COMMAND "${executable}"
    WORKING_DIRECTORY "${working_directory}"
    TIMEOUT "${timeout}"
    RESULT_VARIABLE result)
  if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "GTest executable ${executable} failed: ${result}")
  endif()
endfunction()

if(NOT DEFINED NPU_COMPUTE_RUN_UNIFIED)
  set(NPU_COMPUTE_RUN_UNIFIED $<NOT:$<BOOL:@NPU_TOOLS_LLT_RUN_POST_BUILD@>>)
endif()
if(NPU_COMPUTE_RUN_UNIFIED)
  run_gtest([==[$<TARGET_FILE:npu_tools_utest>]==] [==[@CMAKE_CURRENT_BINARY_DIR@/llt]==] 300)
endif()
]=] NPU_COMPUTE_NATIVE_TEST_CONTENT @ONLY)
foreach(test_target IN LISTS NPU_COMPUTE_NATIVE_GTEST_TARGETS)
  string(APPEND NPU_COMPUTE_NATIVE_TEST_CONTENT
    "run_gtest([==[$<TARGET_FILE:${test_target}>]==] [==[${CMAKE_CURRENT_BINARY_DIR}/npu_compute/tests]==] 120)\n")
endforeach()
set(NPU_COMPUTE_NATIVE_TEST_SCRIPT
  "${CMAKE_CURRENT_BINARY_DIR}/run_npu_compute_native_tests-$<CONFIG>.cmake")
file(GENERATE OUTPUT "${NPU_COMPUTE_NATIVE_TEST_SCRIPT}"
  CONTENT "${NPU_COMPUTE_NATIVE_TEST_CONTENT}")
add_custom_target(run_npu_compute_native_tests ALL
  COMMAND ${CMAKE_COMMAND} -P "${NPU_COMPUTE_NATIVE_TEST_SCRIPT}"
  DEPENDS npu_tools_utest ${NPU_COMPUTE_NATIVE_GTEST_TARGETS}
  COMMENT "Run npu-compute native GTest UT"
  VERBATIM
)
if(TARGET collect_coverage_data)
  add_dependencies(collect_coverage_data run_npu_compute_native_tests)
endif()
