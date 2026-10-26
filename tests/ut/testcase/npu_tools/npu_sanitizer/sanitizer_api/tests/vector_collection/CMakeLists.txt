# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(vector_collection_tests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../../.." ABSOLUTE)
set(SAN "${REPO_ROOT}/npu_tools/npu_check/src/acl_san")
set(COMMON_TEST "${CMAKE_CURRENT_LIST_DIR}/../../../common/tests")
set(RUNTIME_HEADERS "${REPO_ROOT}/tests/ut/testcase/npu_tools/injection/tests/stubs/runtime/include")
add_executable(vector_collection_test integration_test.cpp
  "${SAN}/aclsan_trace_runtime.cpp"
  "${SAN}/device_instr/arch/dav_3510/decoder.cpp"
  "${SAN}/device_instr/arch/dav_3510/register_state_manager.cpp"
  "${SAN}/aclsan_memory_cbdata.cpp" "${SAN}/aclsan_register_dependency.cpp"
  "${SAN}/aclsan_translate_device_data.cpp" "${SAN}/aclsan_device_data_log.cpp"
  "${COMMON_TEST}/plog_test_library.cpp")
target_compile_features(vector_collection_test PRIVATE cxx_std_17)
target_compile_options(vector_collection_test PRIVATE
  -Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
target_include_directories(vector_collection_test PRIVATE
  "${SAN}" "${SAN}/../../include" "${COMMON_TEST}" "${SAN}/../shared"
  "${RUNTIME_HEADERS}" "${REPO_ROOT}/npu_tools/injection/include")
target_link_options(vector_collection_test PRIVATE -Wl,--gc-sections)
target_link_libraries(vector_collection_test PRIVATE Threads::Threads)
if(TARGET injection_headers AND TARGET unified_dlog_headers AND TARGET c_sec_static)
  target_link_libraries(vector_collection_test PRIVATE injection_headers unified_dlog_headers c_sec_static)
else()
  find_path(DLOG_INCLUDE_DIR dlog_pub.h
    HINTS "$ENV{ASCEND_HOME_PATH}/include" "$ENV{ASCEND_HOME_PATH}/pkg_inc/base")
  find_path(CANN_INCLUDE_DIR acl/acl_rt.h HINTS "$ENV{ASCEND_HOME_PATH}/include")
  if(NOT DLOG_INCLUDE_DIR OR NOT CANN_INCLUDE_DIR)
    message(FATAL_ERROR "Set DLOG_INCLUDE_DIR and CANN_INCLUDE_DIR to the CANN header directories")
  endif()
  target_include_directories(vector_collection_test PRIVATE
    "${DLOG_INCLUDE_DIR}" "${CANN_INCLUDE_DIR}" "${CANN_INCLUDE_DIR}/../pkg_inc")
endif()
add_test(NAME aclsan.vector_collection COMMAND vector_collection_test)
