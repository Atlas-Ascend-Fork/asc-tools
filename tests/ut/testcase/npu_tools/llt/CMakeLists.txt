# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

# npu_tools 迁移模块的统一 LLT 大用例：一个 GTest 二进制收拢全部单元/组件测试，
# 通过 run_llt_test 注册 POST_BUILD 钩子，`build.sh --test` 构建即执行。

set(NPU_TOOLS_LLT_CHECK_TESTS_DIR "${NPU_TOOLS_TEST_ROOT}/npu_sanitizer/npu_check/tests")
set(NPU_TOOLS_LLT_CLI_TESTS_DIR "${NPU_TOOLS_TEST_ROOT}/npu_sanitizer/npu_check_cli/tests")
set(NPU_TOOLS_LLT_COMMON_TESTS_DIR "${NPU_TOOLS_TEST_ROOT}/npu_sanitizer/common/tests")
set(NPU_TOOLS_LLT_SANITIZER_TESTS_DIR "${NPU_TOOLS_TEST_ROOT}/npu_sanitizer/sanitizer_api/tests")
set(NPU_TOOLS_LLT_COMPUTE_TESTS_DIR "${NPU_TOOLS_TEST_ROOT}/npu_compute/tests")
set(NPU_TOOLS_LLT_COMPUTE_DIR "${CMAKE_SOURCE_DIR}/npu_tools/npu_compute")
set(NPU_TOOLS_LLT_COMPUTE_CLI_DIR "${NPU_TOOLS_LLT_COMPUTE_DIR}/src/cli")
set(NPU_TOOLS_LLT_COMPUTE_SRC_DIR "${NPU_TOOLS_LLT_COMPUTE_DIR}/src/compute")

set(NPU_TOOLS_LLT_PROCESSOR_DIR "${CMAKE_SOURCE_DIR}/npu_tools/npu_check/src/processor")
set(NPU_TOOLS_LLT_CLI_DIR "${CMAKE_SOURCE_DIR}/npu_tools/npu_check/src/cli")
set(NPU_TOOLS_LLT_ACL_SAN_DIR "${CMAKE_SOURCE_DIR}/npu_tools/npu_check/src/acl_san")


set(NPU_TOOLS_LLT_ACL_SAN_INTERNAL_SOURCES
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_api.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_active_probe_plan.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_trace_buffer.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_trace_runtime.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_memory_cbdata.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_register_dependency.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_device_data_log.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/aclsan_translate_device_data.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/device_runtime/device_binary_registry.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/device_runtime/device_symbolizer.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/device_instr/decoder_registry.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/device_instr/arch/dav_3510/decoder.cpp"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/device_instr/arch/dav_3510/register_state_manager.cpp"
)

if(NOT TARGET GTest::gtest)
  find_package(GTest REQUIRED)
endif()

add_executable(npu_tools_utest
  gtest_main.cpp
  # npu_check processor / tool manager / renderer
  "${NPU_TOOLS_LLT_CHECK_TESTS_DIR}/mem_state_tracker_test.cpp"
  "${NPU_TOOLS_LLT_CHECK_TESTS_DIR}/racecheck_report_test.cpp"
  # cli
  "${NPU_TOOLS_LLT_CLI_TESTS_DIR}/options_test.cpp"
  "${NPU_TOOLS_LLT_CLI_TESTS_DIR}/process_runner_test.cpp"
  # sanitizer api（含原"桩家族"用例：分歧桩已收敛到 aclsan_boundary_stub 可安装边界）
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_log_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_memory_cbdata_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/ub_memory_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_callback_invoke_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_probe_plan_callback_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_hook_cbdata_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_trace_buffer.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_translate_device_data_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/cce_instr_header_contract_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/cce_instruction_pipeline_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/device_instr_decoder_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/register_state_manager_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/device_symbolizer_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/device_binary_registry_test.cpp"
  # common
  "${NPU_TOOLS_LLT_COMMON_TESTS_DIR}/aclsan_log_test.cpp"
  # injection（依赖桩版 runtime/profapi，真实 CANN 链已按需剥离）
  "${NPU_TOOLS_TEST_ROOT}/injection/tests/injection_hook_test.cpp"
  # npu_compute 纯逻辑用例（.so 边界/进程类用例保留在 npu_compute/tests 独立目标）
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/test_process_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/ipc_protocol_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/uds_artifact_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/ipc/artifact_publisher.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/cli_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/option_registry_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/collection_file_validator_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/import_output_directory_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/rep_test_decoder.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/rep_decoder_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/rep_encoder_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/rep_directory_packer_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/rep_report_writer_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/report_name_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/staging_directory_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/debug_log_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/hardware_info_json_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/hardware_info_host_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/hardware_info_device_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/hardware_device_api_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/hardware_info_writer_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/hardware_info_collector_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/pmu_csv_writer_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/pmu_data_consumer_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/pmu_report_writer_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/kernel_metadata_collector_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/summary_metadata_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/summary_metrics_test.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_TESTS_DIR}/pipe_trace_finalizer_test.cpp"
  # 直接编译的产品源（与原单测目标保持一致）
    "${NPU_TOOLS_LLT_CLI_DIR}/options.cpp"
  "${NPU_TOOLS_LLT_CLI_DIR}/uds_client.cpp"
  "${NPU_TOOLS_LLT_CLI_DIR}/process_runner.cpp"
  # 统一可安装边界桩层（aclsan C API / 注入钩子 / InvokeCallback）
  aclsan_boundary_stub.cpp
  # 注入/运行时/工具桩直接源码编入（替代 .so 桩链接，消除同名库抢占一类问题）：
  #   injection_hook.cpp —— 注入库产品源（真实注册面/hook 表）
  #   runtime_stub.cpp   —— runtime 桩（ACL API 分发表）
  #   prof_api_stub.cpp  —— profapi 桩
  "${INJECTION_SOURCE_DIR}/src/injection_hook.cpp"
  "${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/runtime/runtime_stub.cpp"
  "${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/prof_api/prof_api_stub.cpp"
  ${NPU_TOOLS_LLT_ACL_SAN_INTERNAL_SOURCES}
  # npu_compute 产品源（launcher 核心 + hardware + pmu）
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/config/config.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/config/option_registry.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/ipc/artifact_receiver.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/ipc/uds_client.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/import/import_output_directory.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/import/imported_profile_results.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/launch/injection_path.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/launch/collection_session.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/launch/launcher.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/launch/process_launcher.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/launch/staging_directory.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/report/collection_file_validator.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/report/rep_decoder.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/report/rep_directory_packer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/report/rep_encoder.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/report/rep_report_writer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/report/report_name.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}/report/pipe_trace_finalizer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/hardware/hardware_device_api.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/hardware/hardware_info_collector.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/hardware/hardware_info_device.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/hardware/hardware_info_host.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/hardware/hardware_info_json.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/hardware/hardware_info_writer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/pmu/pmu_data_consumer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/pmu/pmu_metric_builder.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/report/pmu_csv_writer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/report/summary_jsonl_writer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/report/summary_metrics.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/report/report_writer.cpp"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}/runtime/kernel_metadata_collector.cpp"
)

set_source_files_properties("${INJECTION_SOURCE_DIR}/src/injection_hook.cpp" PROPERTIES
  COMPILE_DEFINITIONS "ACL_TOOL_INJECTION_BUILD=1"
)

target_compile_features(npu_tools_utest PRIVATE cxx_std_17)
target_compile_definitions(npu_tools_utest PRIVATE
  _GLIBCXX_USE_CXX11_ABI=0
  NPU_CHECK_REPORT_PATTERN_SNAPSHOT="${NPU_TOOLS_LLT_CHECK_TESTS_DIR}/report_renderer_patterns.golden"
)
target_compile_options(npu_tools_utest PRIVATE -UNDEBUG)

# 原单测目标显式开启 -Wall -Wextra -Werror 的源文件按原样保留，其余沿用全局选项。
set(NPU_TOOLS_LLT_WERROR_SOURCES
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_log_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_callback_invoke_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_probe_plan_callback_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_hook_cbdata_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_memory_cbdata_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_trace_buffer.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/aclsan_translate_device_data_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/cce_instr_header_contract_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/cce_instruction_pipeline_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/device_instr_decoder_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/register_state_manager_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/device_symbolizer_test.cpp"
  "${NPU_TOOLS_LLT_SANITIZER_TESTS_DIR}/device_binary_registry_test.cpp"
)
set_source_files_properties(${NPU_TOOLS_LLT_WERROR_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wextra;-Werror"
)

target_include_directories(npu_tools_utest PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${INJECTION_SOURCE_DIR}/src"
  "${NPU_TOOLS_LLT_COMPUTE_DIR}/src"
  "${NPU_TOOLS_LLT_COMPUTE_CLI_DIR}"
  "${NPU_TOOLS_LLT_COMPUTE_SRC_DIR}"
  "${NPU_TOOLS_LLT_PROCESSOR_DIR}"
  "${NPU_TOOLS_LLT_PROCESSOR_DIR}/../include"
  "${NPU_TOOLS_LLT_CLI_DIR}"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/../../include"
  "${NPU_TOOLS_LLT_ACL_SAN_DIR}/device_runtime"
)

target_link_libraries(npu_tools_utest PRIVATE
  npu_compute_test_process
  npu_compute_headers
  npu_compute_ipc
  npu_check_core
  npu_check_common
  acl_san
  npu_sanitizer_plog
  plog_test_library
  npu_check_dbi_engine
  injection_headers
  injection_test_headers
  # Some CANN packages expose this imported headers target only in the injection directory.
  # injection_headers already propagates its include directories in that case.
  "$<TARGET_NAME_IF_EXISTS:acl_rt_headers>"
  Threads::Threads
  GTest::gtest
  Boost::filesystem
  ${CMAKE_DL_LIBS}
)

target_compile_definitions(npu_tools_utest PRIVATE
  NPU_COMPUTE_PROCESS_CHILD_PATH="$<TARGET_FILE:npu_compute_test_process_child>")
add_dependencies(npu_tools_utest npu_compute_test_process_child)

set(NPU_TOOLS_GTEST_XML "${CMAKE_BINARY_DIR}/npu_compute-reports/$<CONFIG>/gtest.xml")
add_test(NAME npu_tools_utest COMMAND npu_tools_utest
  "--gtest_output=xml:${NPU_TOOLS_GTEST_XML}")
set_tests_properties(npu_tools_utest PROPERTIES LABELS "npu_compute_ci_host" TIMEOUT 300)

# 桩库（unified_dlog/runtime/profapi）与 CANN 真实库同名；用 DT_RPATH（优先于
# LD_LIBRARY_PATH）让构建树里的桩目录稳定抢占，POST_BUILD/ctest 环境无关。
# aclsan C API 与 aclsan::InvokeCallback 的真实实现在 aclsan_api.cpp（本二进制内），
# 经 --wrap 重定向到 aclsan_boundary_stub 的可安装边界；桩版 libruntime(fake-cann)
# 与产品库闭包里的真实 libacl_rt 同名互斥，--as-needed 使未被引用的 libacl_san.so
# 及其真实 runtime 链整体剥离；--allow-shlib-undefined 放宽剩余共享库闭包校验。
target_link_options(npu_tools_utest PRIVATE
  -Wl,--export-dynamic-symbol=rtFunctionGetMetaInfo
  -Wl,--export-dynamic-symbol=rtFunctionGetMetaInfoSize
  -Wl,--disable-new-dtags
  -Wl,--allow-shlib-undefined
  -Wl,--as-needed
  -Wl,--wrap=acltoolHookInit
  -Wl,--wrap=acltoolGetOriginalRuntimeApi
  -Wl,--wrap=acltoolClearCallback
  -Wl,--wrap=acltoolRegisterAclrtMallocCallbacks
  -Wl,--wrap=acltoolRegisterAclrtFreeCallbacks
  -Wl,--wrap=acltoolRegisterAclrtSynchronizeStreamCallbacks
  -Wl,--wrap=acltoolRegisterAclrtSynchronizeStreamWithTimeoutCallbacks
  -Wl,--wrap=acltoolRegisterAclrtGetFuncBySymbolCallbacks
  -Wl,--wrap=acltoolRegisterAclrtBinaryUnLoadCallbacks
  -Wl,--wrap=acltoolRegisterAclrtResetDeviceCallbacks
  -Wl,--wrap=acltoolRegisterAclrtBinaryLoadFromDataCallbacks
  -Wl,--wrap=acltoolRegisterAclrtBinaryGetFunctionCallbacks
  -Wl,--wrap=acltoolRegisterAclrtBinaryGetFunctionByEntryCallbacks
  -Wl,--wrap=acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks
  -Wl,--wrap=acltoolRegisterAclrtLaunchKernelWithArgsArrayCallbacks
  -Wl,--wrap=acltoolRegisterAclrtMallocWithCfgCallbacks
  -Wl,--wrap=acltoolRegisterAclrtBinaryLoadFromFileCallbacks
  -Wl,--wrap=aclsanSubscribe
  -Wl,--wrap=aclsanUnsubscribe
  -Wl,--wrap=aclsanEnableCallback
  -Wl,--wrap=aclsanGetDeviceCallStack
  -Wl,--wrap=_ZN6aclsan14InvokeCallbackE20AclsanCallbackDomainjPKv
  -Wl,--wrap=_ZN6aclsan17ApplyRuntimeHooksERKSt3setI10aclrtApiIdSt4lessIS1_ESaIS1_EE
  -Wl,--wrap=_ZN6aclsan28ResolveActiveDeviceCallStackEmPNS_14device_runtime15CallStackResultE
)

option(NPU_TOOLS_LLT_RUN_POST_BUILD "Run npu_tools_utest after linking" ON)
if(NPU_TOOLS_LLT_RUN_POST_BUILD)
  run_llt_test(TARGET npu_tools_utest TASK_NUM 1)
endif()
