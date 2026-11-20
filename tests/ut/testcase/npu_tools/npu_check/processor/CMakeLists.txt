# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

set(NPU_CHECK_SOURCE_DIR "${CMAKE_SOURCE_DIR}/npu_tools/npu_check/src/processor")
set(NPU_CHECK_DBI_ROOT "${CMAKE_SOURCE_DIR}/npu_tools/npu_check/src/acl_san")
set(NPU_CHECK_DBI_INCLUDE_DIR "${NPU_CHECK_DBI_ROOT}")
set(NPU_CHECK_TEST_REPORT_RENDERER_SOURCES
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report_renderer.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/report_catalog.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/report_fields.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/report_normalizer.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/report_summary.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/report_text_renderer.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/synccheck_adapter.cpp"
)

if(NOT TARGET GTest::gtest)
  find_package(GTest REQUIRED)
endif()

add_executable(npu_check_unit_tests
  "${NPU_CHECK_TEST_ROOT}/shared/gtest_main.cpp"
  "${NPU_CHECK_TEST_ROOT}/shared/log_capture.cpp"
  "checker/unified_memory_test.cpp"
  "checker/allocation_registry_test.cpp"
  "checker/memcheck_test.cpp"
  "checker/gm_initcheck_test.cpp"
  "checker/init_state_table_test.cpp"
  "checker/initcheck_test.cpp"
  "checker/initcheck_memory_test.cpp"
  "checker/host_order_test.cpp"
  "checker/checker_test.cpp"
  "diagnostic/report_buffer_test.cpp"
  "checker/synccheck_test.cpp"
  "ipc/uds_server_test.cpp"
  "${NPU_CHECK_TEST_ROOT}/shared/wire_protocol_test.cpp"
  ${NPU_CHECK_TEST_REPORT_RENDERER_SOURCES}
)
target_compile_features(npu_check_unit_tests PRIVATE cxx_std_17)
target_compile_definitions(npu_check_unit_tests PRIVATE
  _GLIBCXX_USE_CXX11_ABI=0
)
target_include_directories(npu_check_unit_tests PRIVATE
  "${NPU_CHECK_SOURCE_DIR}"
  "${NPU_CHECK_DBI_INCLUDE_DIR}"
  "${INJECTION_ACL_INCLUDE_DIR}"
)
target_link_libraries(npu_check_unit_tests PRIVATE
  npu_check_core acl_san npu_sanitizer_plog plog_test_library Threads::Threads GTest::gtest Boost::filesystem c_sec_static)
add_test(NAME npu_check_unit_tests COMMAND npu_check_unit_tests)

add_executable(npu_check_simt_tests
  "${NPU_CHECK_TEST_ROOT}/shared/gtest_main.cpp" "checker/simt_memory_record_test.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_trace_buffer.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_simt_trace.cpp")
target_compile_features(npu_check_simt_tests PRIVATE cxx_std_17)
target_compile_definitions(npu_check_simt_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_include_directories(npu_check_simt_tests PRIVATE
  "${NPU_CHECK_DBI_ROOT}" "${NPU_CHECK_DBI_ROOT}/../../include")
target_link_libraries(npu_check_simt_tests PRIVATE GTest::gtest c_sec_static)
add_test(NAME npu_check_simt_tests COMMAND npu_check_simt_tests)

# Keep global allocator fault injection out of the main unit/LLT executables.
add_executable(npu_check_memstate_allocation_tests
  "${NPU_CHECK_TEST_ROOT}/shared/gtest_main.cpp"
  "${NPU_CHECK_TEST_ROOT}/shared/log_capture.cpp" "checker/mem_state_tracker_allocation_test.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/racecheck.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/racecheck/sync_analysis.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/racecheck/happens_before_graph.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/racecheck/types.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/mem_state_tracker.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/racecheck_reports.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/report_fields.cpp")
target_compile_features(npu_check_memstate_allocation_tests PRIVATE cxx_std_17)
target_compile_definitions(npu_check_memstate_allocation_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(npu_check_memstate_allocation_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(npu_check_memstate_allocation_tests PRIVATE
  "${NPU_CHECK_SOURCE_DIR}" "${NPU_CHECK_DBI_ROOT}" "${NPU_CHECK_DBI_ROOT}/../../include")
target_link_libraries(npu_check_memstate_allocation_tests PRIVATE npu_check_core npu_sanitizer_plog plog_test_library GTest::gtest)
add_test(NAME npu_check_memstate_allocation_tests COMMAND npu_check_memstate_allocation_tests)

add_executable(npu_check_cube_memory_tests
  "${NPU_CHECK_TEST_ROOT}/shared/gtest_main.cpp"
  "checker/cube_memory_test.cpp"
  "checker/racecheck_translation_test.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_register_dependency.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_memory_cbdata.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_trace_buffer.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_translate_device_data.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_device_data_log.cpp"
  "${NPU_CHECK_DBI_ROOT}/aclsan_active_probe_plan.cpp"
  "${NPU_CHECK_DBI_ROOT}/device_instr/arch/dav_3510/register_state_manager.cpp"
  ${NPU_CHECK_TEST_REPORT_RENDERER_SOURCES}
  "${NPU_CHECK_DBI_ROOT}/device_instr/arch/dav_3510/decoder.cpp")
target_compile_features(npu_check_cube_memory_tests PRIVATE cxx_std_17)
target_compile_definitions(npu_check_cube_memory_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_include_directories(npu_check_cube_memory_tests PRIVATE
  "${NPU_CHECK_SOURCE_DIR}" "${NPU_CHECK_DBI_INCLUDE_DIR}" "${NPU_CHECK_DBI_ROOT}/../../include"
  "${CMAKE_SOURCE_DIR}/npu_tools/injection/include" "${INJECTION_PROFILING_INCLUDE_DIR}")
target_link_libraries(npu_check_cube_memory_tests PRIVATE
  npu_check_core npu_sanitizer_plog Threads::Threads GTest::gtest c_sec_static)
add_test(NAME npu_check_cube_memory_tests COMMAND npu_check_cube_memory_tests)

add_executable(npu_check_tool_manager_test
  "${NPU_CHECK_TEST_ROOT}/shared/gtest_main.cpp"
  "tool_manager/tool_manager_test.cpp"
  "${NPU_CHECK_SOURCE_DIR}/../cli/options.cpp"
  "${NPU_CHECK_SOURCE_DIR}/../cli/uds_client.cpp"
  ${NPU_CHECK_TEST_REPORT_RENDERER_SOURCES}
)
target_compile_features(npu_check_tool_manager_test PRIVATE cxx_std_17)
target_compile_definitions(npu_check_tool_manager_test PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_include_directories(npu_check_tool_manager_test PRIVATE
  "${NPU_CHECK_SOURCE_DIR}"
  "${NPU_CHECK_SOURCE_DIR}/../cli"
  "${NPU_CHECK_DBI_INCLUDE_DIR}"
)
target_link_libraries(npu_check_tool_manager_test PRIVATE
  npu_check_core acl_san injection_headers npu_sanitizer_plog Threads::Threads GTest::gtest Boost::filesystem)
add_test(NAME npu_check_tool_manager_test COMMAND npu_check_tool_manager_test)
set_tests_properties(npu_check_tool_manager_test PROPERTIES TIMEOUT 45)

add_executable(npu_check_kernel_attributes_test
  "${NPU_CHECK_TEST_ROOT}/shared/gtest_main.cpp"
  "tool_manager/kernel_attributes_test.cpp"
  "${NPU_CHECK_SOURCE_DIR}/tool_manager/kernel_attributes.cpp"
)
target_compile_features(npu_check_kernel_attributes_test PRIVATE cxx_std_17)
target_compile_definitions(npu_check_kernel_attributes_test PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(npu_check_kernel_attributes_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(npu_check_kernel_attributes_test PRIVATE "${NPU_CHECK_SOURCE_DIR}")
target_link_libraries(npu_check_kernel_attributes_test PRIVATE injection_headers Threads::Threads GTest::gtest)
add_test(NAME npu_check_kernel_attributes_test COMMAND npu_check_kernel_attributes_test)

add_test(NAME npu_check_kernel_attribute_ownership
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tool_manager/check_kernel_attribute_ownership.sh" "${CMAKE_SOURCE_DIR}")

add_executable(npu_check_report_renderer_smoke
  "diagnostic/report_renderer_smoke.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/mem_state_tracker.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/racecheck_reports.cpp"
  ${NPU_CHECK_TEST_REPORT_RENDERER_SOURCES}
)
target_compile_features(npu_check_report_renderer_smoke PRIVATE cxx_std_17)
target_compile_definitions(npu_check_report_renderer_smoke PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_definitions(npu_check_report_renderer_smoke PRIVATE
  NPU_CHECK_REPORT_PATTERN_SNAPSHOT="${CMAKE_CURRENT_SOURCE_DIR}/diagnostic/report_renderer_patterns.golden")
target_include_directories(npu_check_report_renderer_smoke PRIVATE
  "${NPU_CHECK_SOURCE_DIR}/../../include"
  "${NPU_CHECK_SOURCE_DIR}"
  "${NPU_CHECK_DBI_INCLUDE_DIR}"
)
target_link_libraries(npu_check_report_renderer_smoke PRIVATE Threads::Threads GTest::gtest npu_sanitizer_plog)
add_test(NAME npu_check_report_renderer_smoke COMMAND npu_check_report_renderer_smoke)

add_executable(npu_check_racecheck_render_tests
  "diagnostic/racecheck_render_test.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/mem_state_tracker.cpp"
  "${NPU_CHECK_SOURCE_DIR}/checker/racecheck_reports.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/device_call_stack.cpp"
  "${NPU_CHECK_SOURCE_DIR}/diagnostic/report/report_assembler.cpp"
  ${NPU_CHECK_TEST_REPORT_RENDERER_SOURCES})
target_compile_features(npu_check_racecheck_render_tests PRIVATE cxx_std_17)
target_compile_definitions(npu_check_racecheck_render_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(npu_check_racecheck_render_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(npu_check_racecheck_render_tests PRIVATE
  "${NPU_CHECK_SOURCE_DIR}" "${NPU_CHECK_SOURCE_DIR}/../shared" "${NPU_CHECK_SOURCE_DIR}/../../include")
target_link_libraries(npu_check_racecheck_render_tests PRIVATE npu_sanitizer_plog)
add_test(NAME npu_check_racecheck_render_tests COMMAND npu_check_racecheck_render_tests)

add_subdirectory(checker/racecheck)
