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
project(npubit_integration_tests LANGUAGES CXX)
enable_testing()
add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)
set(SAN "${REPO_ROOT}/npu_tools/npu_check/src/acl_san")
set(PROC "${REPO_ROOT}/npu_tools/npu_check/src/processor")
set(SECURITY "${REPO_ROOT}/npu_tools/utils/security")
set(COMMON_TEST "${CMAKE_CURRENT_LIST_DIR}/../../shared")
set(RUNTIME_HEADERS "${REPO_ROOT}/tests/ut/testcase/npu_tools/injection/tests/stubs/runtime/include")
if(TARGET npu_bit)
  get_target_property(NPUBIT_INCLUDE_DIR npu_bit INTERFACE_INCLUDE_DIRECTORIES)
else()
  find_path(NPUBIT_INCLUDE_DIR npubit.h HINTS "$ENV{ASCEND_HOME_PATH}/tools/npu_bit/include")
  if(NOT NPUBIT_INCLUDE_DIR)
    message(FATAL_ERROR "Set NPUBIT_INCLUDE_DIR to the Npubit headers directory")
  endif()
endif()
find_path(DLOG_INCLUDE_DIR dlog_pub.h HINTS "$ENV{ASCEND_HOME_PATH}/include" "$ENV{ASCEND_HOME_PATH}/pkg_inc/base")
if(NOT DLOG_INCLUDE_DIR)
  message(FATAL_ERROR "Set DLOG_INCLUDE_DIR to CANN's dlog_pub.h directory")
endif()
find_path(CANN_INCLUDE_DIR acl/acl_rt.h HINTS "$ENV{ASCEND_HOME_PATH}/include")
if(NOT CANN_INCLUDE_DIR)
  message(FATAL_ERROR "Set CANN_INCLUDE_DIR to CANN headers directory")
endif()
add_library(npubit_mock STATIC mock.cpp)
set_target_properties(npubit_mock PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(npubit_mock PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${NPUBIT_INCLUDE_DIR}")
add_library(npubit_test_checker STATIC
  "${PROC}/checker/memcheck_callbacks.cpp" "${PROC}/checker/memcheck.cpp" "${PROC}/checker/allocation_registry.cpp"
  "${PROC}/checker/memory_state.cpp" "${PROC}/checker/memory_analysis.cpp"
  "${PROC}/checker/memory_analysis_state.cpp" "${PROC}/checker/memory_initialization.cpp"
  "${PROC}/checker/memory_resources.cpp" "${PROC}/checker/memory_global_history.cpp"
  "${PROC}/checker/memory_host.cpp" "${PROC}/checker/memory_host_checks.cpp"
  "${PROC}/checker/memory_host_races.cpp" "${PROC}/checker/host_order.cpp"
  "${PROC}/checker/racecheck/types.cpp" "${PROC}/checker/racecheck/dependency_graph.cpp"
  "${PROC}/checker/racecheck/happens_before_graph.cpp")
target_compile_features(npubit_test_checker PRIVATE cxx_std_17)
target_compile_options(npubit_test_checker PRIVATE -ffunction-sections -fdata-sections)
target_include_directories(npubit_test_checker PRIVATE "${PROC}" "${SAN}" "${SAN}/../../include" "${SAN}/../shared"
  "${DLOG_INCLUDE_DIR}" "${CANN_INCLUDE_DIR}" "${CANN_INCLUDE_DIR}/../pkg_inc" "${REPO_ROOT}/npu_tools/injection/include")
add_executable(npubit_integration_test integration_test.cpp
  "${SAN}/aclsan_vf.cpp" "${SAN}/aclsan_trace_runtime.cpp"
  "${SAN}/device_instr/arch/dav_3510/decoder.cpp"
  "${SAN}/device_instr/arch/dav_3510/register_state_manager.cpp"
  "${SAN}/aclsan_memory_cbdata.cpp" "${SAN}/aclsan_register_dependency.cpp"
  "${SAN}/aclsan_translate_device_data.cpp" "${SAN}/aclsan_device_data_log.cpp"
  "${COMMON_TEST}/plog_test_library.cpp")
target_compile_features(npubit_integration_test PRIVATE cxx_std_17)
target_compile_options(npubit_integration_test PRIVATE -Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
target_include_directories(npubit_integration_test BEFORE PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(npubit_integration_test PRIVATE "${SAN}" "${SAN}/../../include" "${PROC}" "${COMMON_TEST}"
  "${SAN}/../shared" "${SECURITY}" "${DLOG_INCLUDE_DIR}" "${RUNTIME_HEADERS}" "${CANN_INCLUDE_DIR}" "${REPO_ROOT}/npu_tools/injection/include" "${CANN_INCLUDE_DIR}/../pkg_inc")
target_link_libraries(npubit_integration_test PRIVATE npubit_mock npubit_test_checker pthread)
target_link_options(npubit_integration_test PRIVATE -Wl,--gc-sections)
add_test(NAME npubit_integration COMMAND npubit_integration_test)
# Registry coverage uses the actual Boost dependency.
find_package(Boost REQUIRED COMPONENTS filesystem)
target_sources(npubit_integration_test PRIVATE "${SAN}/device_runtime/device_binary_registry.cpp"
    "${SAN}/device_runtime/device_symbolizer.cpp" "${SAN}/dbi/kernel_param_metadata.cpp" "${SECURITY}/file_io.cpp")
target_compile_definitions(npubit_integration_test PRIVATE NPUBIT_TEST_REGISTRY=1)
target_link_libraries(npubit_integration_test PRIVATE Boost::filesystem)
if(TARGET c_sec_static)
  target_link_libraries(npubit_integration_test PRIVATE c_sec_static)
else()
  find_library(C_SEC_LIBRARY NAMES c_sec HINTS "$ENV{ASCEND_HOME_PATH}/lib64" "${CANN_INCLUDE_DIR}/../lib64")
  if(NOT C_SEC_LIBRARY)
    message(FATAL_ERROR "Set C_SEC_LIBRARY to the CANN securec library")
  endif()
  target_link_libraries(npubit_integration_test PRIVATE "${C_SEC_LIBRARY}")
endif()
message(STATUS "Npubit integration tests use mock functions; registry coverage enabled")
