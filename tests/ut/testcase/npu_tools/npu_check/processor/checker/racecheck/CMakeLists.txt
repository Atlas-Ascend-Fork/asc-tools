# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

if(NOT TARGET npu_sanitizer_plog)
  message(FATAL_ERROR
    "racecheck tests must be configured from the repository test tree so npu_sanitizer_plog is available")
endif()

if(NOT TARGET GTest::gtest)
  find_package(GTest REQUIRED)
endif()

set(REPO_ROOT "${CMAKE_SOURCE_DIR}")
# Exercise the production synchronization graph together with the memstate checker.
add_executable(racecheck_integration_tests
  "${NPU_CHECK_TEST_ROOT}/shared/gtest_main.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/memory_analysis.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/memory_state.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/allocation_registry.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/acl_san/aclsan_translate_device_data.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/acl_san/aclsan_memory_cbdata.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/acl_san/aclsan_register_dependency.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/acl_san/aclsan_device_data_log.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/acl_san/device_instr/arch/dav_3510/decoder.cpp"
  "${NPU_CHECK_TEST_ROOT}/processor/checker/racecheck_test.cpp"
  "${NPU_CHECK_TEST_ROOT}/processor/checker/racecheck/happens_before_graph_test.cpp"
  "${NPU_CHECK_TEST_ROOT}/processor/checker/racecheck/memory_access_state_test.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/dependency_graph.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/sync_analysis.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/happens_before_graph.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/types.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/memory_access_state.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/mem_state_tracker.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck_reports.cpp"
  ${NPU_CHECK_TEST_REPORT_RENDERER_SOURCES})
target_compile_features(racecheck_integration_tests PRIVATE cxx_std_17)
target_compile_definitions(racecheck_integration_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(racecheck_integration_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(racecheck_integration_tests PRIVATE
  "${REPO_ROOT}/npu_tools/npu_check/src/processor"
  "${REPO_ROOT}/npu_tools/npu_check/src/shared"
  "${REPO_ROOT}/npu_tools/npu_check/src/acl_san"
  "${REPO_ROOT}/npu_tools/npu_check/include")
target_link_libraries(racecheck_integration_tests PRIVATE
  npu_check_core GTest::gtest Threads::Threads npu_sanitizer_plog c_sec_static)
add_test(NAME racecheck_integration_tests COMMAND racecheck_integration_tests)
