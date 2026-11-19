# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
add_library(npu_compute_launcher_core STATIC
  ipc/artifact_receiver.cpp
  ipc/uds_client.cpp
  config/config.cpp
  config/option_registry.cpp
  import/import_output_directory.cpp
  import/imported_profile_results.cpp
  launch/injection_path.cpp
  launch/collection_session.cpp
  launch/launcher.cpp
  launch/process_launcher.cpp
  launch/staging_directory.cpp
  report/pm_sampling_file_validator.cpp

  launch/disk_output_directory.cpp
  report/collection_processor.cpp
  report/collection_file_validator.cpp
  report/csv_reader.cpp
  report/csv_sqlite_writer.cpp
  report/summary_sqlite_finalizer.cpp
  report/pipe_trace_finalizer.cpp
  report/rep_decoder.cpp
  report/rep_encoder.cpp
  report/rep_report_writer.cpp
  report/report_name.cpp
)
target_include_directories(npu_compute_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(npu_compute_launcher_core PRIVATE c_sec_static)
target_link_libraries(npu_compute_launcher_core PUBLIC
  npu_compute_headers
  npu_compute_ipc
  Boost::filesystem
  Threads::Threads
  npu_compute_sqlite_runtime
)

add_executable(npu_compute_cli main.cpp)
target_link_libraries(npu_compute_cli PRIVATE npu_compute_launcher_core)
set_target_properties(npu_compute_cli PROPERTIES OUTPUT_NAME npu-compute)
npu_compute_set_product_output(npu_compute_cli)
