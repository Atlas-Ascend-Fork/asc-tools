# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16.0)
project(npu_check_tool LANGUAGES C CXX)

find_package(Threads REQUIRED)

set(NPU_CHECK_REPORT_RENDERER_SOURCES
  diagnostic/report_renderer.cpp
  diagnostic/report/report_catalog.cpp
  diagnostic/report/report_normalizer.cpp
  diagnostic/report/report_summary.cpp
  diagnostic/report/report_text_renderer.cpp
  diagnostic/report/synccheck_adapter.cpp
)
set(NPU_CHECK_DBI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../acl_san")
set(NPU_CHECK_DBI_SOURCE_DIR "${NPU_CHECK_DBI_ROOT}/dbi")
set(NPU_CHECK_DBI_INCLUDE_DIR "${NPU_CHECK_DBI_ROOT}")

if(NOT TARGET npu_check_common)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../shared
    ${CMAKE_CURRENT_BINARY_DIR}/npu_check_common)
endif()

if(NOT TARGET acl_san)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../acl_san
    ${CMAKE_CURRENT_BINARY_DIR}/sanitizer_api)
endif()

add_library(npu_check_core STATIC
  checker/checker.cpp
  checker/memcheck_callbacks.cpp
  checker/synccheck_callbacks.cpp
  diagnostic/report/device_call_stack.cpp
  diagnostic/report/report_assembler.cpp
  diagnostic/report/report_fields.cpp
  checker/allocation_registry.cpp
  checker/memcheck.cpp
  checker/mem_state_tracker.cpp
  checker/racecheck_reports.cpp
  checker/racecheck.cpp
  checker/initcheck.cpp
  checker/racecheck/sync_analysis.cpp
  checker/racecheck/types.cpp
  checker/racecheck/happens_before_graph.cpp
  checker/racecheck/memory_access_state.cpp
  checker/synccheck.cpp
  diagnostic/report_buffer.cpp
  ipc/uds_server.cpp
  tool_manager/kernel_attributes.cpp
  tool_manager/tool_manager.cpp
)

target_compile_features(npu_check_core PUBLIC cxx_std_17)
target_include_directories(npu_check_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../acl_san
)
target_link_libraries(npu_check_core
  PUBLIC npu_check_common
  PRIVATE acl_san acl_tool_injection npu_sanitizer_plog Threads::Threads Boost::filesystem
)
set_target_properties(npu_check_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

add_library(npu_check SHARED
  tool_manager/entry.cpp
  ${NPU_CHECK_REPORT_RENDERER_SOURCES}
)
target_compile_features(npu_check PRIVATE cxx_std_17)
target_include_directories(npu_check
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../acl_san
)
target_link_libraries(npu_check
  PRIVATE npu_check_core acl_san acl_tool_injection Threads::Threads
)
target_compile_options(npu_check PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(npu_check PRIVATE
  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/npu_check.map"
  "LINKER:-z,nodelete"
)
set_target_properties(npu_check PROPERTIES
  OUTPUT_NAME npu_check_processor
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)
