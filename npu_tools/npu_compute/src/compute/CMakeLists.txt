# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
add_library(npu_compute SHARED
  biu/biu_pipeline.cpp
  hardware/hardware_device_api.cpp
  hardware/hardware_info_collector.cpp
  hardware/hardware_info_device.cpp
  hardware/hardware_info_host.cpp
  hardware/hardware_info_json.cpp
  hardware/hardware_info_writer.cpp
  ipc/artifact_publisher.cpp
  ipc/uds_server.cpp
  npu_compute.cpp
  pm_sampling/pm_sampling_artifacts.cpp
  pm_sampling/pm_sampling_decoder.cpp
  pm_sampling/pm_sampling_sqlite_writer.cpp
  pmu/pmu_data_consumer.cpp
  report/pmu_csv_writer.cpp
  pmu/pmu_metric_builder.cpp
  performance_hints/performance_hint_analyzer.cpp
  performance_hints/performance_hint_utils.cpp
  report/summary_jsonl_writer.cpp
  report/summary_metrics.cpp
  report/report_writer.cpp
  runtime/kernel_metadata_collector.cpp
  runtime/compute_service.cpp
  runtime/npu_compute_runtime.cpp
  runtime/section_config.cpp
)
target_include_directories(npu_compute PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
set(NPU_COMPUTE_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/libnpu_compute.map")
if(TARGET npu_compute_integration_headers)
  set(NPU_COMPUTE_LIBRARY_HEADERS npu_compute_integration_headers)
else()
  set(NPU_COMPUTE_LIBRARY_HEADERS npu_compute_headers)
endif()
target_link_libraries(npu_compute PRIVATE c_sec_static)
target_link_libraries(npu_compute
  PUBLIC ${NPU_COMPUTE_LIBRARY_HEADERS}
  PRIVATE npu_compute_plog ${CMAKE_DL_LIBS} Threads::Threads Boost::filesystem npu_compute_ipc npu_compute_sqlite_runtime
)
if(TARGET acl_pti)
  target_link_libraries(npu_compute PRIVATE acl_pti)
endif()
target_link_options(npu_compute PRIVATE
  "LINKER:--version-script=${NPU_COMPUTE_VERSION_SCRIPT}"
)
set_target_properties(npu_compute PROPERTIES
  OUTPUT_NAME npu_compute_processor
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  LINK_DEPENDS "${NPU_COMPUTE_VERSION_SCRIPT}"
)
npu_compute_set_product_output(npu_compute)
