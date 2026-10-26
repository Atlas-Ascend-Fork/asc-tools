# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
set(SANITIZER_API_SOURCE_DIR "${CMAKE_SOURCE_DIR}/npu_tools/npu_check/src/acl_san")
set(SANITIZER_API_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(ENABLE_TEST)
  add_test(NAME aclsan.public_headers
    COMMAND bash "${SANITIZER_API_TEST_DIR}/check_public_headers.sh"
      "${SANITIZER_API_SOURCE_DIR}/../../include"
      "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}")
  add_test(NAME acl_san_exports
    COMMAND bash "${SANITIZER_API_TEST_DIR}/check_acl_san_exports.sh"
      $<TARGET_FILE:acl_san>)
  add_test(NAME aclsan.host_test_linkage_contract
    COMMAND bash "${SANITIZER_API_TEST_DIR}/check_host_test_linkage_contract.sh"
      "${CMAKE_READELF}" $<TARGET_FILE:acl_san_test>
      $<TARGET_FILE:aclsan_launch_with_host_args_hook>
      $<TARGET_FILE:aclsan_binary_load_dbi_hook>
      $<TARGET_FILE:aclsan_full_flow_host_sample>)
  add_test(NAME aclsan.probe_contract
    COMMAND bash "${SANITIZER_API_TEST_DIR}/check_probe_contract.sh")
  add_test(NAME aclsan.device_memory_access_producer_contract
    COMMAND bash "${SANITIZER_API_TEST_DIR}/check_device_memory_access_producer_contract.sh")

  add_library(acl_san_test SHARED
    ${SANITIZER_API_SOURCE_DIR}/device_runtime/device_binary_registry.cpp
    ${SANITIZER_API_SOURCE_DIR}/device_runtime/device_symbolizer.cpp
    ${SANITIZER_API_SOURCE_DIR}/device_instr/decoder_registry.cpp
    ${SANITIZER_API_SOURCE_DIR}/device_instr/arch/dav_3510/decoder.cpp
    ${SANITIZER_API_SOURCE_DIR}/device_instr/arch/dav_3510/register_state_manager.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_active_probe_plan.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_memory_cbdata.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_register_dependency.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_trace_buffer.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_trace_runtime.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_api.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_dispatch.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_hook_aclrt.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_device_data_log.cpp
    ${SANITIZER_API_SOURCE_DIR}/aclsan_translate_device_data.cpp
  )
  target_compile_features(acl_san_test PUBLIC cxx_std_17)
  target_include_directories(acl_san_test
    PUBLIC ${SANITIZER_API_SOURCE_DIR}/../../include
    PRIVATE ${SANITIZER_API_SOURCE_DIR}
  )
  target_link_libraries(acl_san_test PRIVATE c_sec_static)
target_link_libraries(acl_san_test PRIVATE
    acl_tool_injection_test
    npu_check_dbi_engine
    npu_sanitizer_plog
    ${CMAKE_DL_LIBS}
    Threads::Threads
    Boost::filesystem
  )
  target_link_options(acl_san_test PRIVATE
    "LINKER:--exclude-libs,ALL"
    "LINKER:--version-script=${SANITIZER_API_SOURCE_DIR}/cmake/acl_san.map"
  )
  set_target_properties(acl_san_test PROPERTIES
    OUTPUT_NAME acl_san_test
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
  )
  injection_set_test_output(acl_san_test)

  # aclsan_hook_cbdata / aclsan_callback_invoke / aclsan_probe_plan_callback 已并入
  # llt/npu_tools_utest（分歧桩收敛到 aclsan_boundary_stub 可安装边界）。

  add_executable(aclsan_callback_contract ${SANITIZER_API_TEST_DIR}/aclsan_callback_contract.cpp)
  target_compile_features(aclsan_callback_contract PRIVATE cxx_std_17)
  target_link_libraries(aclsan_callback_contract PRIVATE acl_san_test)
  add_test(NAME aclsan_callback_contract COMMAND aclsan_callback_contract)

  add_executable(aclsan_lifecycle_api_removed ${SANITIZER_API_TEST_DIR}/aclsan_lifecycle_api_removed.cpp)
  target_compile_features(aclsan_lifecycle_api_removed PRIVATE cxx_std_17)
  target_link_libraries(aclsan_lifecycle_api_removed PRIVATE ${CMAKE_DL_LIBS})
  add_dependencies(aclsan_lifecycle_api_removed acl_san)
  add_test(NAME aclsan_lifecycle_api_removed COMMAND aclsan_lifecycle_api_removed
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/npu_tools/lib64")

  if(TARGET acl_runtime_stub)
    add_executable(aclsan_multi_entry ${SANITIZER_API_TEST_DIR}/aclsan_multi_entry.cpp)
    set_target_properties(aclsan_multi_entry PROPERTIES ENABLE_EXPORTS ON)
    target_compile_options(aclsan_multi_entry PRIVATE -UNDEBUG)
    target_include_directories(aclsan_multi_entry PRIVATE ${SANITIZER_API_TEST_DIR}/dbi)
    target_link_libraries(aclsan_multi_entry PRIVATE
      acl_san_test acl_tool_injection_test acl_runtime_stub acl_prof_api_stub Threads::Threads)

    add_executable(aclsan_aclnn_runtime_hooks ${SANITIZER_API_TEST_DIR}/aclsan_aclnn_runtime_hooks.cpp)
    target_compile_features(aclsan_aclnn_runtime_hooks PRIVATE cxx_std_17)
    target_include_directories(aclsan_aclnn_runtime_hooks PRIVATE ${SANITIZER_API_TEST_DIR}/dbi)
    target_link_libraries(aclsan_aclnn_runtime_hooks PRIVATE
      acl_san_test acl_tool_injection_test acl_runtime_stub)

    add_executable(aclsan_trace_d2h_hook ${SANITIZER_API_TEST_DIR}/aclsan_trace_d2h_hook.cpp)
    target_compile_features(aclsan_trace_d2h_hook PRIVATE cxx_std_17)
    target_include_directories(aclsan_trace_d2h_hook PRIVATE
      ${SANITIZER_API_SOURCE_DIR}/../../include
      ${SANITIZER_API_SOURCE_DIR}
      ${SANITIZER_API_TEST_DIR}/dbi
    )
    target_link_libraries(aclsan_trace_d2h_hook PRIVATE acl_san_test acl_tool_injection_test acl_runtime_stub)

    add_executable(aclsan_launch_with_host_args_hook ${SANITIZER_API_TEST_DIR}/aclsan_launch_with_host_args_hook.cpp)
    target_compile_features(aclsan_launch_with_host_args_hook PRIVATE cxx_std_17)
    target_link_libraries(aclsan_launch_with_host_args_hook PRIVATE acl_san_test acl_tool_injection_test acl_runtime_stub)
    target_link_options(aclsan_launch_with_host_args_hook PRIVATE -Wl,--disable-new-dtags)
  add_test(NAME aclsan_launch_with_host_args_hook COMMAND aclsan_launch_with_host_args_hook)

    add_executable(aclsan_binary_load_dbi_hook ${SANITIZER_API_TEST_DIR}/aclsan_binary_load_dbi_hook.cpp)
    target_compile_features(aclsan_binary_load_dbi_hook PRIVATE cxx_std_17)
    target_link_libraries(aclsan_binary_load_dbi_hook PRIVATE acl_san_test acl_tool_injection_test acl_runtime_stub)
    target_link_options(aclsan_binary_load_dbi_hook PRIVATE -Wl,--disable-new-dtags)
  add_test(NAME aclsan_binary_load_dbi_hook COMMAND aclsan_binary_load_dbi_hook)

    add_executable(aclsan_full_flow_host_sample
      ${SANITIZER_API_TEST_DIR}/dbi/full_flow_host_sample.cpp)
    target_compile_features(aclsan_full_flow_host_sample PRIVATE cxx_std_17)
    target_compile_options(aclsan_full_flow_host_sample PRIVATE -Wno-error=cast-function-type)
    target_include_directories(aclsan_full_flow_host_sample PRIVATE
      ${SANITIZER_API_SOURCE_DIR}/../../include
      ${SANITIZER_API_SOURCE_DIR}
    )
    target_link_libraries(aclsan_full_flow_host_sample PRIVATE acl_san_test acl_tool_injection_test acl_runtime_stub)
    target_link_options(aclsan_full_flow_host_sample PRIVATE -Wl,--disable-new-dtags)

    add_executable(aclsan_full_flow_fake_dbi_tool
      ${SANITIZER_API_TEST_DIR}/dbi/fake_dbi_tool.cpp)
    target_compile_features(aclsan_full_flow_fake_dbi_tool PRIVATE cxx_std_17)
    target_compile_options(aclsan_full_flow_fake_dbi_tool PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(aclsan_full_flow_fake_dbi_tool PRIVATE Boost::filesystem)

    add_executable(aclsan_symbolizer_api_test ${SANITIZER_API_TEST_DIR}/aclsan_symbolizer_api_test.cpp)
    target_compile_features(aclsan_symbolizer_api_test PRIVATE cxx_std_17)
    target_compile_definitions(aclsan_symbolizer_api_test PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
    target_compile_options(aclsan_symbolizer_api_test PRIVATE
      -Wall -Wextra -Werror -Wno-error=cast-function-type)
    target_include_directories(aclsan_symbolizer_api_test PRIVATE
      ${SANITIZER_API_SOURCE_DIR}/../../include
      ${SANITIZER_API_SOURCE_DIR}
      ${SANITIZER_API_TEST_DIR}/dbi
    )
    target_link_libraries(aclsan_symbolizer_api_test PRIVATE
      acl_san_test acl_tool_injection_test acl_runtime_stub Boost::filesystem)

    foreach(target IN ITEMS aclsan_symbolizer_api_test
        aclsan_multi_entry
        aclsan_aclnn_runtime_hooks
        aclsan_trace_d2h_hook
        aclsan_full_flow_host_sample)
      add_dependencies(${target} aclsan_full_flow_fake_dbi_tool)
    endforeach()

    foreach(target IN ITEMS aclsan_symbolizer_api_test aclsan_trace_d2h_hook aclsan_aclnn_runtime_hooks)
      add_test(NAME ${target}
        COMMAND ${CMAKE_COMMAND}
          -DTEST_EXECUTABLE=$<TARGET_FILE:${target}>
          -DFAKE_TOOL=$<TARGET_FILE:aclsan_full_flow_fake_dbi_tool>
          -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/${target}-dbi
          -DRUNTIME_ROOT=${NPU_TOOLS_TEST_CANN_ROOT}
          -DINJECTION_LIBRARY_DIR=$<TARGET_FILE_DIR:acl_san_test>
          -P ${SANITIZER_API_TEST_DIR}/run_with_fake_dbi.cmake)
    endforeach()

    foreach(mode IN ITEMS data file)
      add_test(NAME aclsan.multi_entry.${mode}
        COMMAND ${CMAKE_COMMAND}
          -DTEST_EXECUTABLE=$<TARGET_FILE:aclsan_multi_entry>
          -DTEST_ARGUMENTS=$<$<STREQUAL:${mode},file>:--file>
          -DFAKE_TOOL=$<TARGET_FILE:aclsan_full_flow_fake_dbi_tool>
          -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/multi-entry-${mode}
          -DRUNTIME_ROOT=${NPU_TOOLS_TEST_CANN_ROOT}
          -DINJECTION_LIBRARY_DIR=$<TARGET_FILE_DIR:acl_san_test>
          -P ${SANITIZER_API_TEST_DIR}/run_with_fake_dbi.cmake)
    endforeach()

    add_test(NAME aclsan.full_flow_host_sample
      COMMAND ${CMAKE_COMMAND}
        -DSAMPLE=$<TARGET_FILE:aclsan_full_flow_host_sample>
        -DFAKE_TOOL=$<TARGET_FILE:aclsan_full_flow_fake_dbi_tool>
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/full-flow-host
        -DRUNTIME_ROOT=${NPU_TOOLS_TEST_CANN_ROOT}
        -P ${SANITIZER_API_TEST_DIR}/dbi/run_full_flow_host_sample.cmake)

    add_test(NAME aclsan.file_load_trace_d2h
      COMMAND ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:aclsan_trace_d2h_hook>
        -DTEST_ARGUMENTS=--file
        -DFAKE_TOOL=$<TARGET_FILE:aclsan_full_flow_fake_dbi_tool>
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/file-load-d2h
        -DRUNTIME_ROOT=${NPU_TOOLS_TEST_CANN_ROOT}
        -DINJECTION_LIBRARY_DIR=$<TARGET_FILE_DIR:acl_san_test>
        -P ${SANITIZER_API_TEST_DIR}/run_with_fake_dbi.cmake)
  endif()
endif()
if(ENABLE_TEST OR NPU_CHECK_ENABLE_REAL_DBI_FLOW OR NPU_CHECK_ENABLE_REAL_DBI_TOOLCHAIN)
  add_subdirectory("${SANITIZER_API_TEST_DIR}/dbi" "${CMAKE_CURRENT_BINARY_DIR}/tests/dbi")
endif()

if(ENABLE_TEST)
  add_subdirectory(vector_collection)
endif()
