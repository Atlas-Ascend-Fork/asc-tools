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

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../../.." ABSOLUTE)
add_executable(racecheck_memstate_tests
  ../gtest_main.cpp
  ../mem_state_tracker_test.cpp
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/mem_state_tracker.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck_reports.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/diagnostic/report/report_fields.cpp")
target_compile_features(racecheck_memstate_tests PRIVATE cxx_std_17)
target_compile_definitions(racecheck_memstate_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(racecheck_memstate_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(racecheck_memstate_tests PRIVATE
  "${REPO_ROOT}/npu_tools/npu_check/src/processor"
  "${REPO_ROOT}/npu_tools/npu_check/include")
target_link_libraries(racecheck_memstate_tests PRIVATE GTest::gtest Threads::Threads)
add_test(NAME racecheck_memstate_tests COMMAND racecheck_memstate_tests)

add_executable(racecheck_memstate_allocation_tests
  ../gtest_main.cpp
  ../mem_state_tracker_allocation_test.cpp
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/mem_state_tracker.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck_reports.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/diagnostic/report/report_fields.cpp")
target_compile_features(racecheck_memstate_allocation_tests PRIVATE cxx_std_17)
target_compile_definitions(racecheck_memstate_allocation_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(racecheck_memstate_allocation_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(racecheck_memstate_allocation_tests PRIVATE
  "${REPO_ROOT}/npu_tools/npu_check/src/shared"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor"
  "${REPO_ROOT}/npu_tools/npu_check/include")
target_link_libraries(racecheck_memstate_allocation_tests PRIVATE GTest::gtest Threads::Threads npu_sanitizer_plog)
add_test(NAME racecheck_memstate_allocation_tests COMMAND racecheck_memstate_allocation_tests)

add_executable(racecheck_report_tests
  ../gtest_main.cpp
  ../racecheck_report_test.cpp
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/mem_state_tracker.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck_reports.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/diagnostic/report/report_fields.cpp")
target_compile_features(racecheck_report_tests PRIVATE cxx_std_17)
target_compile_definitions(racecheck_report_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(racecheck_report_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(racecheck_report_tests PRIVATE
  "${REPO_ROOT}/npu_tools/npu_check/src/processor"
  "${REPO_ROOT}/npu_tools/npu_check/src/shared"
  "${REPO_ROOT}/npu_tools/npu_check/include")
target_link_libraries(racecheck_report_tests PRIVATE GTest::gtest Threads::Threads npu_sanitizer_plog)
add_test(NAME racecheck_report_tests COMMAND racecheck_report_tests)

set(REPORT_DIR "${REPO_ROOT}/npu_tools/npu_check/src/processor/diagnostic")
add_executable(racecheck_render_tests
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/mem_state_tracker.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck_reports.cpp"
  ../racecheck_render_test.cpp
  "${REPORT_DIR}/report/device_call_stack.cpp"
  "${REPORT_DIR}/report/report_assembler.cpp"
  "${REPORT_DIR}/report_renderer.cpp"
  "${REPORT_DIR}/report/report_catalog.cpp"
  "${REPORT_DIR}/report/report_fields.cpp"
  "${REPORT_DIR}/report/report_normalizer.cpp"
  "${REPORT_DIR}/report/report_summary.cpp"
  "${REPORT_DIR}/report/report_text_renderer.cpp"
  "${REPORT_DIR}/report/synccheck_adapter.cpp")
target_compile_features(racecheck_render_tests PRIVATE cxx_std_17)
target_compile_definitions(racecheck_render_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(racecheck_render_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(racecheck_render_tests PRIVATE
  "${REPO_ROOT}/npu_tools/npu_check/src/processor"
  "${REPO_ROOT}/npu_tools/npu_check/src/shared"
  "${REPO_ROOT}/npu_tools/npu_check/include")
add_test(NAME racecheck_render_tests COMMAND racecheck_render_tests)

# Exercise the production synchronization graph together with the memstate checker.
add_executable(racecheck_integration_tests
  ../gtest_main.cpp
  ../racecheck_test.cpp ../happens_before_graph_test.cpp ../memory_access_state_test.cpp
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/sync_analysis.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/happens_before_graph.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/types.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck/memory_access_state.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/mem_state_tracker.cpp"
  "${REPO_ROOT}/npu_tools/npu_check/src/processor/checker/racecheck_reports.cpp"
  "${REPORT_DIR}/report_renderer.cpp"
  "${REPORT_DIR}/report/report_catalog.cpp"
  "${REPORT_DIR}/report/report_fields.cpp"
  "${REPORT_DIR}/report/report_normalizer.cpp"
  "${REPORT_DIR}/report/report_summary.cpp"
  "${REPORT_DIR}/report/report_text_renderer.cpp"
  "${REPORT_DIR}/report/synccheck_adapter.cpp")
target_compile_features(racecheck_integration_tests PRIVATE cxx_std_17)
target_compile_definitions(racecheck_integration_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(racecheck_integration_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(racecheck_integration_tests PRIVATE
  "${REPO_ROOT}/npu_tools/npu_check/src/processor"
  "${REPO_ROOT}/npu_tools/npu_check/src/shared"
  "${REPO_ROOT}/npu_tools/npu_check/src/acl_san"
  "${REPO_ROOT}/npu_tools/npu_check/include")
target_link_libraries(racecheck_integration_tests PRIVATE GTest::gtest Threads::Threads npu_sanitizer_plog)
add_test(NAME racecheck_integration_tests COMMAND racecheck_integration_tests)
