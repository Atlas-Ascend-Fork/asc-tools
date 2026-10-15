# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
# 单测族按测试点主次分两类：
# 1) 纯逻辑单测（多数）：并入 llt/npu_tools_utest（GTest 化，编译即运行）。
# 2) DSO 边界用例：以 .so 链接为场景机制（exe 侧符号抢占 / 动态装载），保留独立目标。
set(NPU_COMPUTE_ACLPTI_UNIT_BACKENDS
  acl_tool_injection_test_unit
  acl_runtime_stub_unit
  prof_api_min_stub
)
set(NPU_COMPUTE_ACLPTI_DSO_BACKENDS
  acl_tool_injection_test
  acl_prof_api_stub
  acl_runtime_stub
)

if(NOT TARGET npu_compute_ipc)
  add_subdirectory(${NPU_COMPUTE_SOURCE_DIR}/src/ipc ${CMAKE_CURRENT_BINARY_DIR}/ipc)
endif()
add_executable(npu_compute_ipc_protocol_test ipc_protocol_test.cpp)
target_link_libraries(npu_compute_ipc_protocol_test PRIVATE npu_compute_ipc Threads::Threads)
add_test(NAME npu_compute.ipc_protocol COMMAND npu_compute_ipc_protocol_test)
add_executable(npu_compute_uds_artifact_test
  uds_artifact_test.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/ipc/artifact_receiver.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/ipc/artifact_publisher.cpp)
target_link_libraries(npu_compute_uds_artifact_test
  PRIVATE npu_compute_ipc Threads::Threads Boost::filesystem)
target_include_directories(npu_compute_uds_artifact_test PRIVATE ${NPU_COMPUTE_SOURCE_DIR}/src/compute)
add_test(NAME npu_compute.uds_artifact COMMAND npu_compute_uds_artifact_test)
set_tests_properties(npu_compute.uds_artifact PROPERTIES TIMEOUT 60)

# 最小 profapi 桩（"仅仅能跑"）：Msprof 场景函数由各用例 mock 定义，
# 这里只补装载器入口；真实装载/场景由 DSO 集成用例覆盖。
add_library(prof_api_min_stub STATIC prof_api_min_stub.cpp)
target_link_libraries(prof_api_min_stub PRIVATE injection_test_headers)

# acl_pti 静态孪生：供 DSO 族以外的 .so 链接型用例直链（无同名抢占）。
add_library(acl_pti_unit STATIC
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/initialization.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/activity/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/activity/control.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/callback/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/callback/dispatcher.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/profiling_data_manager.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/data_processor.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/data_receiver.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/replay_data_aggregation.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/raw_data_decoder.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/range_profiler.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/replay_memory.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/replay_runtime.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/binary_registry.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/handler/runtime_api_handlers.cpp
  binary_instrumenter_stub.cpp
)
target_compile_definitions(acl_pti_unit PRIVATE NPU_COMPUTE_ENABLE_TEST_CONTROLS=1)
target_include_directories(acl_pti_unit PRIVATE "${NPU_COMPUTE_SOURCE_DIR}/src")
target_link_libraries(acl_pti_unit
  PUBLIC npu_compute_test_headers
  PRIVATE acl_tool_injection_test_unit Threads::Threads Boost::filesystem ${CMAKE_DL_LIBS}
)
set_target_properties(acl_pti_unit PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)


find_package(Threads REQUIRED)

# 桩库（libruntime.so/libprofapi.so 等）与 CANN 真实库同名；DSO 族统一转 DT_RPATH
# （优先于 LD_LIBRARY_PATH），使构建树桩目录在任意环境下稳定抢占。
foreach(target IN ITEMS
    npu_compute_binary_instrumenter_test
    npu_compute_injection_path_test
    npu_compute_imported_profile_results_test
    npu_compute_launcher_test
    npu_compute_rep_fixture_app
    npu_compute_stub_demo_app
    acl_pti_public_api_test
    acl_pti_callback_api_test
    acl_pti_replay_test
    acl_pti_simt_replay_test
    acl_pti_args_array_replay_test
    acl_pti_shadow_memory_test
    acl_pti_launch_without_config_test
    acl_pti_initialize_failure_test
    acl_pti_runtime_failure_short_circuit_test
    acl_pti_hook_failure_domain_test
    acl_pti_callback_stub_test
    acl_prof_api_loader_test
    acl_prof_api_stub_test
    npu_compute_runtime_callback_test
    npu_compute_biu_pipeline_test
  )
  if(TARGET ${target})
    target_link_options(${target} PRIVATE -Wl,--disable-new-dtags)
  endif()
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# 边界型用例：自带 runtime 边界 mock（acltoolGetOriginalRuntimeApi / aclrt* 桩），
# 引用 acl_pti 产品类，属静态族独立目标（不入 llt 大用例）。
add_executable(npu_compute_binary_registry_test binary_registry_test.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/binary_registry.cpp)
target_include_directories(npu_compute_binary_registry_test PRIVATE "${NPU_COMPUTE_SOURCE_DIR}/src")
target_link_libraries(npu_compute_binary_registry_test PRIVATE
  acl_pti_unit ${NPU_COMPUTE_ACLPTI_UNIT_BACKENDS})
add_test(NAME npu_compute.binary_registry COMMAND npu_compute_binary_registry_test)

add_executable(npu_compute_binary_instrumenter_test binary_instrumenter_test.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/binary_instrumenter.cpp)
target_compile_definitions(npu_compute_binary_instrumenter_test PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_include_directories(npu_compute_binary_instrumenter_test PRIVATE "${NPU_COMPUTE_SOURCE_DIR}/src")
target_link_libraries(npu_compute_binary_instrumenter_test PRIVATE Boost::filesystem Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME npu_compute.binary_instrumenter
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/binary_instrumenter_test.py
    $<TARGET_FILE:npu_compute_binary_instrumenter_test>)


add_library(acl_pti_test SHARED
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/initialization.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/activity/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/activity/control.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/callback/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/callback/dispatcher.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/profiling_data_manager.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/data_processor.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/data_receiver.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/replay_data_aggregation.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/raw_data_decoder.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/range_profiler.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/replay_memory.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/replay_runtime.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/profiling/binary_registry.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/handler/runtime_api_handlers.cpp
  binary_instrumenter_stub.cpp
)
target_compile_definitions(acl_pti_test PRIVATE
  NPU_COMPUTE_ENABLE_TEST_CONTROLS=1
)
target_include_directories(acl_pti_test PRIVATE "${NPU_COMPUTE_SOURCE_DIR}/src")
target_link_libraries(acl_pti_test
  PUBLIC npu_compute_test_headers
  PRIVATE acl_tool_injection_test acl_prof_api_stub Threads::Threads
)
set_target_properties(acl_pti_test PROPERTIES
  OUTPUT_NAME acl_pti_test
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)
npu_compute_set_test_output(acl_pti_test)

set(NPU_COMPUTE_TEST_RUNTIME_SOURCES
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/biu/biu_pipeline.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_device_api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_collector.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_device.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_host.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_json.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/ipc/artifact_publisher.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/ipc/uds_server.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/npu_compute.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/runtime/npu_compute_runtime.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/pmu/pmu_data_consumer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/pmu_csv_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/pmu/pmu_metric_builder.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/summary_jsonl_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/summary_metrics.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/report_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/runtime/compute_service.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/runtime/kernel_metadata_collector.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/runtime/section_config.cpp
)
add_library(npu_compute_test SHARED ${NPU_COMPUTE_TEST_RUNTIME_SOURCES})
target_include_directories(npu_compute_test PRIVATE
  "${NPU_COMPUTE_SOURCE_DIR}/src"
  "${NPU_COMPUTE_SOURCE_DIR}/src/compute"
)
target_link_libraries(npu_compute_test
  PUBLIC npu_compute_test_headers
  PRIVATE acl_pti_test ${CMAKE_DL_LIBS} Threads::Threads Boost::filesystem npu_compute_ipc
)
set(NPU_COMPUTE_TEST_VERSION_SCRIPT
  "${NPU_COMPUTE_SOURCE_DIR}/src/compute/libnpu_compute.map")
target_link_options(npu_compute_test PRIVATE
  "LINKER:--version-script=${NPU_COMPUTE_TEST_VERSION_SCRIPT}")
set_target_properties(npu_compute_test PROPERTIES
  OUTPUT_NAME npu-compute-test
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  LINK_DEPENDS "${NPU_COMPUTE_TEST_VERSION_SCRIPT}"
)
npu_compute_set_test_output(npu_compute_test)

# ResolveInjectionLibraryPath 在可执行文件旁查找 libnpu_compute_processor.so；
# 测试版库输出名为 libnpu-compute-test.so（避免与产品库同名冲突），
# 这里复制一份以产品名就位，为 launcher/CLI 类用例创造运行条件（不引入真实产品依赖）。
add_custom_command(TARGET npu_compute_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:npu_compute_test>"
          "${NPU_TOOLS_TEST_RUNTIME_OUTPUT_DIR}/libnpu_compute_processor.so"
  COMMENT "Stage libnpu_compute_processor.so for CLI/launcher tests")

add_library(npu_compute_launcher_core_test STATIC
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/ipc/artifact_receiver.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/ipc/uds_client.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/report/collection_file_validator.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/report/pipe_trace_finalizer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/config/config.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/config/option_registry.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/import/import_output_directory.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/import/imported_profile_results.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/launch/injection_path.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/launch/collection_session.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/launch/launcher.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/launch/process_launcher.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/report/rep_decoder.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/report/rep_directory_packer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/report/rep_encoder.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/report/rep_report_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/report/report_name.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/launch/staging_directory.cpp
)
target_include_directories(npu_compute_launcher_core_test PUBLIC
  "${NPU_COMPUTE_SOURCE_DIR}/src/cli")
target_link_libraries(npu_compute_launcher_core_test
  PUBLIC npu_compute_test_headers Boost::filesystem)
target_link_libraries(npu_compute_launcher_core_test PRIVATE npu_compute_ipc Threads::Threads)

add_executable(npu_compute_cli_test_bin
  ${NPU_COMPUTE_SOURCE_DIR}/src/cli/main.cpp
)
target_link_libraries(npu_compute_cli_test_bin PRIVATE npu_compute_launcher_core_test)
set_target_properties(npu_compute_cli_test_bin PROPERTIES OUTPUT_NAME npu-compute)
npu_compute_set_test_output(npu_compute_cli_test_bin)








if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/injection_path_test.cpp")
  add_executable(npu_compute_injection_path_test
    injection_path_test.cpp
    ${NPU_COMPUTE_SOURCE_DIR}/src/cli/launch/injection_path.cpp
  )
  target_include_directories(npu_compute_injection_path_test PRIVATE
    ${NPU_COMPUTE_SOURCE_DIR}/src/cli
  )
  target_link_libraries(npu_compute_injection_path_test PRIVATE Boost::filesystem)
  set_target_properties(npu_compute_injection_path_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/injection_path_layout/${CMAKE_SYSTEM_PROCESSOR}-linux/bin"
  )
  add_test(NAME npu_compute.injection_path COMMAND npu_compute_injection_path_test)
endif()


add_executable(npu_compute_imported_profile_results_test imported_profile_results_test.cpp)
target_link_libraries(npu_compute_imported_profile_results_test
  PRIVATE npu_compute_launcher_core_test Boost::filesystem)
add_test(NAME npu_compute.imported_profile_results
  COMMAND npu_compute_imported_profile_results_test $<TARGET_FILE:npu_compute_cli_test_bin>
)

add_executable(npu_compute_launcher_test launcher_test.cpp)
target_link_libraries(npu_compute_launcher_test
  PRIVATE npu_compute_launcher_core_test Boost::filesystem)
add_dependencies(npu_compute_launcher_test npu_compute_test)
npu_compute_set_test_output(npu_compute_launcher_test)
add_test(NAME npu_compute.launcher COMMAND npu_compute_launcher_test)


add_executable(npu_compute_rep_fixture_app
  rep_fixture_app.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/ipc/artifact_publisher.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/ipc/uds_server.cpp
)
target_include_directories(npu_compute_rep_fixture_app PRIVATE ${NPU_COMPUTE_SOURCE_DIR}/src/compute)
target_link_libraries(npu_compute_rep_fixture_app PRIVATE npu_compute_ipc Threads::Threads)
add_dependencies(npu_compute_rep_fixture_app npu_compute_cli_test_bin npu_compute_test)
npu_compute_set_test_output(npu_compute_rep_fixture_app)
target_compile_definitions(npu_compute_launcher_test PRIVATE
  NPU_COMPUTE_REP_FIXTURE_PATH="$<TARGET_FILE:npu_compute_rep_fixture_app>")
add_dependencies(npu_compute_launcher_test npu_compute_rep_fixture_app)
set_tests_properties(npu_compute.launcher PROPERTIES TIMEOUT 60)

add_executable(npu_compute_stub_demo_app stub_demo_app.cpp)
target_link_libraries(npu_compute_stub_demo_app PRIVATE acl_runtime_stub)
add_dependencies(npu_compute_stub_demo_app npu_compute_test acl_pti_test)
npu_compute_set_test_output(npu_compute_stub_demo_app)

add_executable(acl_pti_public_api_test public_api_test.cpp)
target_link_libraries(acl_pti_public_api_test
  PRIVATE acl_pti_unit acl_tool_injection_test_unit acl_runtime_stub_unit
)
# 该用例的 Msprof mock 未包含 prof_api.h（C++ mangled，DSO 模式下即为死代码），
# 库内引用由全量桩的 extern "C" 定义满足，语义与 .so 布局一致。
target_link_libraries(acl_pti_public_api_test PRIVATE acl_prof_api_stub_unit)
set_target_properties(acl_pti_public_api_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.public_api COMMAND acl_pti_public_api_test)

add_executable(acl_pti_callback_api_test callback_api_test.cpp)
target_link_libraries(acl_pti_callback_api_test
  PRIVATE acl_pti_unit acl_tool_injection_test_unit acl_runtime_stub_unit
)
# 该用例的 Msprof mock 未包含 prof_api.h（C++ mangled，DSO 模式下即为死代码），
# 库内引用由全量桩的 extern "C" 定义满足，语义与 .so 布局一致。
target_link_libraries(acl_pti_callback_api_test PRIVATE acl_prof_api_stub_unit)
set_target_properties(acl_pti_callback_api_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.callback_api COMMAND acl_pti_callback_api_test)

add_executable(acl_pti_replay_test replay_test.cpp)
target_link_libraries(acl_pti_replay_test
  PRIVATE acl_pti_test ${NPU_COMPUTE_ACLPTI_DSO_BACKENDS}
)
set_target_properties(acl_pti_replay_test PROPERTIES ENABLE_EXPORTS YES)
set_target_properties(acl_pti_replay_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.replay COMMAND acl_pti_replay_test)
add_test(NAME npu_compute.replay_symbol COMMAND acl_pti_replay_test symbol)
add_test(NAME npu_compute.replay_debug COMMAND acl_pti_replay_test debug)
add_test(NAME npu_compute.replay_shrink_block COMMAND acl_pti_replay_test shrink)
add_test(NAME npu_compute.replay_pipeline_host_args COMMAND acl_pti_replay_test host)

add_executable(acl_pti_simt_replay_test simt_replay_test.cpp)
target_link_libraries(acl_pti_simt_replay_test
  PRIVATE acl_pti_unit ${NPU_COMPUTE_ACLPTI_UNIT_BACKENDS}
)
set_target_properties(acl_pti_simt_replay_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.simt_replay COMMAND acl_pti_simt_replay_test)

add_executable(acl_pti_args_array_replay_test args_array_replay_test.cpp)
target_link_libraries(acl_pti_args_array_replay_test
  PRIVATE acl_pti_unit ${NPU_COMPUTE_ACLPTI_UNIT_BACKENDS}
)
set_target_properties(acl_pti_args_array_replay_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.args_array_replay
  COMMAND acl_pti_args_array_replay_test normal
)
add_test(NAME npu_compute.simt_args_array_replay
  COMMAND acl_pti_args_array_replay_test simt
)

add_executable(acl_pti_shadow_memory_test shadow_memory_test.cpp)
target_link_libraries(acl_pti_shadow_memory_test
  PRIVATE acl_pti_unit ${NPU_COMPUTE_ACLPTI_UNIT_BACKENDS}
)
set_target_properties(acl_pti_shadow_memory_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.shadow_memory COMMAND acl_pti_shadow_memory_test)

add_executable(acl_pti_launch_without_config_test launch_without_config_test.cpp)
target_link_libraries(acl_pti_launch_without_config_test
  PRIVATE acl_pti_unit ${NPU_COMPUTE_ACLPTI_UNIT_BACKENDS}
)
set_target_properties(acl_pti_launch_without_config_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.launch_without_config
  COMMAND acl_pti_launch_without_config_test
)

add_executable(acl_pti_initialize_failure_test pti_initialize_failure_test.cpp)
target_link_libraries(acl_pti_initialize_failure_test
  PRIVATE acl_pti_unit ${NPU_COMPUTE_ACLPTI_UNIT_BACKENDS}
)
set_target_properties(acl_pti_initialize_failure_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.pti_initialize_failure
  COMMAND acl_pti_initialize_failure_test
)

add_executable(acl_pti_runtime_failure_short_circuit_test
  runtime_failure_short_circuit_test.cpp
)
target_link_libraries(acl_pti_runtime_failure_short_circuit_test
  PRIVATE acl_pti_test ${NPU_COMPUTE_ACLPTI_DSO_BACKENDS}
)
set_target_properties(acl_pti_runtime_failure_short_circuit_test PROPERTIES ENABLE_EXPORTS YES)
set_target_properties(acl_pti_runtime_failure_short_circuit_test
  PROPERTIES ENABLE_EXPORTS YES
)
add_test(NAME npu_compute.runtime_failure_short_circuit
  COMMAND acl_pti_runtime_failure_short_circuit_test
)
foreach(scenario malloc free memcpy memset launch missing shutdown-failure binary-disabled
    binary-load binary-companion-load binary-name binary-entry binary-companion-name
    binary-companion-entry binary-unload binary-companion-unload)
  add_test(NAME npu_compute.runtime_failure.${scenario}
    COMMAND acl_pti_runtime_failure_short_circuit_test ${scenario})
endforeach()

add_executable(acl_pti_hook_failure_domain_test hook_failure_domain_test.cpp)
target_link_libraries(acl_pti_hook_failure_domain_test
  PRIVATE acl_pti_test ${NPU_COMPUTE_ACLPTI_DSO_BACKENDS}
)
set_target_properties(acl_pti_hook_failure_domain_test PROPERTIES ENABLE_EXPORTS YES)
add_test(NAME npu_compute.hook_failure.shadow_malloc_stop
  COMMAND acl_pti_hook_failure_domain_test shadow-malloc-stop
)
add_test(NAME npu_compute.hook_failure.replay_sync
  COMMAND acl_pti_hook_failure_domain_test replay-sync
)
add_test(NAME npu_compute.hook_failure.replay_initial_sync_failure
  COMMAND acl_pti_hook_failure_domain_test replay-initial-sync-failure
)
add_test(NAME npu_compute.hook_failure.replay_initial_sync
  COMMAND acl_pti_hook_failure_domain_test replay-initial-sync
)
add_test(NAME npu_compute.hook_failure.profiling_flow
  COMMAND acl_pti_hook_failure_domain_test profiling-flow
)
add_test(NAME npu_compute.hook_failure.shadow_free
  COMMAND acl_pti_hook_failure_domain_test shadow-free
)
add_test(NAME npu_compute.hook_failure.null_free
  COMMAND acl_pti_hook_failure_domain_test null-free
)
add_test(NAME npu_compute.hook_failure.shadow_memcpy_stop
  COMMAND acl_pti_hook_failure_domain_test shadow-memcpy-stop
)
add_test(NAME npu_compute.hook_failure.missing_memcpy_mirror
  COMMAND acl_pti_hook_failure_domain_test missing-memcpy-mirror
)
add_test(NAME npu_compute.hook_failure.missing_memset_mirror
  COMMAND acl_pti_hook_failure_domain_test missing-memset-mirror
)
add_test(NAME npu_compute.hook_failure.malloc_missing_free
  COMMAND acl_pti_hook_failure_domain_test malloc-missing-free
)
add_test(NAME npu_compute.hook_failure.shadow_metadata_oom
  COMMAND acl_pti_hook_failure_domain_test shadow-metadata-oom
)

add_library(acl_prof_api_loader_test_plugin SHARED loader_test_plugin.cpp)
set_target_properties(acl_prof_api_loader_test_plugin PROPERTIES
  OUTPUT_NAME loader_test_plugin
  PREFIX ""
)

add_executable(acl_prof_api_loader_test prof_api_loader_test.cpp)
target_link_libraries(acl_prof_api_loader_test PRIVATE acl_prof_api_stub)
add_test(NAME npu_compute.prof_api_loader
  COMMAND acl_prof_api_loader_test $<TARGET_FILE:acl_prof_api_loader_test_plugin>
)

add_executable(acl_prof_api_stub_test prof_api_stub_test.cpp)
target_link_libraries(acl_prof_api_stub_test PRIVATE acl_prof_api_stub)
add_test(NAME npu_compute.prof_api_stub COMMAND acl_prof_api_stub_test)

add_executable(acl_pti_data_module_test pti_data_module_impl_test.cpp)
target_compile_definitions(acl_pti_data_module_test PRIVATE
  NPU_COMPUTE_ENABLE_TEST_CONTROLS=1
)
target_sources(acl_pti_data_module_test PRIVATE
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/profiling_data_manager.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/data_processor.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/data_receiver.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/replay_data_aggregation.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/acl_pti/data/raw_data_decoder.cpp
)
target_include_directories(acl_pti_data_module_test BEFORE PRIVATE
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute
  ${NPU_COMPUTE_SOURCE_DIR}/src
  ${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/runtime/include
  ${NPU_TOOLS_TEST_ROOT}/injection/tests/stubs/prof_api/include
)
target_link_libraries(acl_pti_data_module_test
  PRIVATE npu_compute_test_headers Threads::Threads
)
add_test(NAME npu_compute.acl_pti_data_module
  COMMAND acl_pti_data_module_test
)




add_executable(npu_compute_biu_pipeline_test
  biu_pipeline_test.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/biu/biu_pipeline.cpp
)
target_include_directories(npu_compute_biu_pipeline_test PRIVATE
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute
)
target_link_libraries(npu_compute_biu_pipeline_test PRIVATE
  acl_pti_test Boost::filesystem
)
add_test(NAME npu_compute.biu_pipeline COMMAND npu_compute_biu_pipeline_test)









add_executable(acl_pti_callback_stub_test
  acl_pti_callback_stub_test.cpp
)
target_link_libraries(acl_pti_callback_stub_test PRIVATE
  acl_pti_callback_stub
)
add_test(NAME npu_compute.acl_pti_callback_stub
  COMMAND acl_pti_callback_stub_test
)

set(NPU_COMPUTE_RUNTIME_IMPLEMENTATION_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime_test_entry.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/biu/biu_pipeline.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_device_api.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_collector.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_device.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_host.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_json.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/hardware/hardware_info_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/runtime/npu_compute_runtime.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/pmu/pmu_data_consumer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/pmu_csv_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/pmu/pmu_metric_builder.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/summary_jsonl_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/summary_metrics.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/report/report_writer.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/runtime/kernel_metadata_collector.cpp
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute/runtime/section_config.cpp
)

add_executable(npu_compute_runtime_callback_test
  npu_compute_runtime_callback_test.cpp
  ${NPU_COMPUTE_RUNTIME_IMPLEMENTATION_SOURCES}
)
target_include_directories(npu_compute_runtime_callback_test PRIVATE
  ${NPU_COMPUTE_SOURCE_DIR}/src
  ${NPU_COMPUTE_SOURCE_DIR}/src/compute
)
target_link_libraries(npu_compute_runtime_callback_test PRIVATE
  acl_pti_callback_stub
  acl_runtime_stub
  npu_compute_test_headers
  acl_pti_test
  ${CMAKE_DL_LIBS}
  Threads::Threads
  Boost::filesystem
)
set_target_properties(npu_compute_runtime_callback_test PROPERTIES
  ENABLE_EXPORTS YES
)
add_test(NAME npu_compute.runtime_callback
  COMMAND ${CMAKE_COMMAND} -E env
    "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:acl_runtime_stub>:$ENV{LD_LIBRARY_PATH}"
    $<TARGET_FILE:npu_compute_runtime_callback_test>
)

if(NPU_COMPUTE_BUILD_REAL_HARDWARE_TESTS)
  add_library(npu_compute_real_hardware_test_library SHARED
    ${NPU_COMPUTE_RUNTIME_IMPLEMENTATION_SOURCES}
  )
  target_include_directories(
    npu_compute_real_hardware_test_library PRIVATE
    ${NPU_COMPUTE_SOURCE_DIR}/src
    ${NPU_COMPUTE_SOURCE_DIR}/src/compute
  )
  target_link_libraries(npu_compute_real_hardware_test_library PRIVATE
    acl_pti_callback_stub
    npu_compute_test_headers
    acl_pti_test
    ${CMAKE_DL_LIBS}
    Threads::Threads
    Boost::filesystem
  )
  set(NPU_COMPUTE_REAL_HARDWARE_VERSION_SCRIPT
    "${CMAKE_CURRENT_BINARY_DIR}/runtime_test.map"
  )
  file(WRITE "${NPU_COMPUTE_REAL_HARDWARE_VERSION_SCRIPT}"
    "{ global: acltoolInitialize; acltoolShutdown; NpuComputeTestConfigure; local: *; };\n")
  target_link_options(npu_compute_real_hardware_test_library PRIVATE
    "LINKER:--version-script=${NPU_COMPUTE_REAL_HARDWARE_VERSION_SCRIPT}"
  )
  set_target_properties(
    npu_compute_real_hardware_test_library PROPERTIES
    OUTPUT_NAME npu-compute-real-hardware-test
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    LINK_DEPENDS "${NPU_COMPUTE_REAL_HARDWARE_VERSION_SCRIPT}"
  )
  npu_compute_set_test_output(npu_compute_real_hardware_test_library)

  find_library(NPU_COMPUTE_REAL_ASCENDCL_LIBRARY
    NAMES ascendcl
    HINTS
      "${NPUCOMPUTE_CANN_ROOT}/lib64"
      "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
      "$ENV{ASCEND_HOME_PATH}/lib64"
    NO_DEFAULT_PATH
  )
  if(NOT NPU_COMPUTE_REAL_ASCENDCL_LIBRARY)
    message(FATAL_ERROR
      "Real HardwareInfo tests require libascendcl.so from the CANN installation")
  endif()

  add_executable(npu_compute_real_hardware_callback_app
    real_hardware_callback_app.cpp
  )
  target_link_libraries(
    npu_compute_real_hardware_callback_app PRIVATE
    acl_pti_callback_stub
    npu_compute_test_headers
    "${NPU_COMPUTE_REAL_ASCENDCL_LIBRARY}"
    ${CMAKE_DL_LIBS}
    Boost::filesystem
  )
  set_target_properties(
    npu_compute_real_hardware_callback_app PROPERTIES
    OUTPUT_NAME npu_compute_real_hardware_callback_app
  )
  npu_compute_set_test_output(npu_compute_real_hardware_callback_app)
endif()

# 桩库（libruntime.so/libprofapi.so 等）与 CANN 真实库同名；DSO 族统一转 DT_RPATH
# （优先于 LD_LIBRARY_PATH），使构建树桩目录在任意环境下稳定抢占。
foreach(target IN ITEMS
    npu_compute_binary_instrumenter_test
    npu_compute_injection_path_test
    npu_compute_imported_profile_results_test
    npu_compute_launcher_test
    npu_compute_rep_fixture_app
    npu_compute_stub_demo_app
    acl_pti_public_api_test
    acl_pti_callback_api_test
    acl_pti_replay_test
    acl_pti_simt_replay_test
    acl_pti_args_array_replay_test
    acl_pti_shadow_memory_test
    acl_pti_launch_without_config_test
    acl_pti_initialize_failure_test
    acl_pti_runtime_failure_short_circuit_test
    acl_pti_hook_failure_domain_test
    acl_pti_callback_stub_test
    acl_prof_api_loader_test
    acl_prof_api_stub_test
    npu_compute_runtime_callback_test
    npu_compute_biu_pipeline_test
  )
  if(TARGET ${target})
    target_link_options(${target} PRIVATE -Wl,--disable-new-dtags)
  endif()
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME npu_compute.rep_packaging_integration
  COMMAND ${CMAKE_COMMAND} -E env
    "NPU_COMPUTE_TEST_CLI=$<TARGET_FILE:npu_compute_cli_test_bin>"
    "NPU_COMPUTE_REP_FIXTURE_APP=$<TARGET_FILE:npu_compute_rep_fixture_app>"
    ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
    ${NPU_COMPUTE_PY_TEST_DIR}/test_rep_packaging_integration.py
)

if(NPU_COMPUTE_BUILD_REAL_HARDWARE_TESTS)
  add_test(
    NAME npu_compute.real_hardware_with_callback_stub
    COMMAND ${CMAKE_COMMAND} -E env
      "NPU_COMPUTE_REAL_HW_APP=$<TARGET_FILE:npu_compute_real_hardware_callback_app>"
      "NPU_COMPUTE_REAL_HW_LIBRARY=$<TARGET_FILE:npu_compute_real_hardware_test_library>"
      ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
      ${NPU_COMPUTE_PY_TEST_DIR}/test_real_hardware_collection.py
  )
  set_tests_properties(
    npu_compute.real_hardware_with_callback_stub PROPERTIES
    LABELS "requires_npu"
  )
endif()

add_test(
  NAME npu_compute.hardware_info_integration
  COMMAND ${CMAKE_COMMAND} -E env
    "NPU_COMPUTE_TEST_BIN_DIR=$<TARGET_FILE_DIR:npu_compute_cli_test_bin>"
    "NPU_COMPUTE_CALLBACK_STUB=$<TARGET_FILE:acl_pti_callback_stub>"
    "NPU_COMPUTE_HARDWARE_API_STUB=$<TARGET_FILE:npu_compute_hardware_api_stub>"
    ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
    ${NPU_COMPUTE_PY_TEST_DIR}/test_hardware_info_integration.py
)

add_test(
  NAME npu_compute.subscribe_lifecycle
  COMMAND ${CMAKE_COMMAND} -E env
    "NPU_COMPUTE_TEST_RUNTIME_DIR=${NPU_TOOLS_TEST_CANN_RUNTIME_DIR}"
    "NPU_COMPUTE_TEST_BIN_DIR=$<TARGET_FILE_DIR:npu_compute_cli_test_bin>"
    ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
    ${NPU_COMPUTE_PY_TEST_DIR}/test_subscriber_lifecycle.py
)

add_test(
  NAME npu_compute.integration_stubs
  COMMAND ${CMAKE_COMMAND} -E env
    "NPU_COMPUTE_TEST_RUNTIME_DIR=${NPU_TOOLS_TEST_CANN_RUNTIME_DIR}"
    "NPU_COMPUTE_TEST_BIN_DIR=$<TARGET_FILE_DIR:npu_compute_cli_test_bin>"
    ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
    ${NPU_COMPUTE_PY_TEST_DIR}/test_integration_stubs.py
)

add_test(
  NAME npu_compute.connected_integration
  COMMAND ${CMAKE_COMMAND} -E env
    "NPU_COMPUTE_TEST_BIN_DIR=$<TARGET_FILE_DIR:npu_compute_cli_test_bin>"
    "LD_PRELOAD=$<TARGET_FILE:npu_compute_hardware_api_stub>:$ENV{LD_PRELOAD}"
    ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
    ${CMAKE_SOURCE_DIR}/tests/py_ut/testcase/npu_compute/test_npu_compute_integration.py
)

add_test(
  NAME npu_compute.install_layout
  COMMAND ${CMAKE_COMMAND} -E env
    "NPU_COMPUTE_TEST_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "NPU_COMPUTE_TEST_BIN_DIR=$<TARGET_FILE_DIR:npu_compute_cli_test_bin>"
    "NPU_COMPUTE_HARDWARE_API_STUB=$<TARGET_FILE:npu_compute_hardware_api_stub>"
    ${Python3_EXECUTABLE} -m pytest -q -p no:cacheprovider
    ${CMAKE_SOURCE_DIR}/tests/py_ut/testcase/npu_compute/test_npu_compute_install.py
)
