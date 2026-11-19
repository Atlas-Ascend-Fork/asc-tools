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
project(npu_check_cli LANGUAGES CXX)

find_package(Threads REQUIRED)

if(NOT TARGET npu_check_common)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../shared
    ${CMAKE_CURRENT_BINARY_DIR}/npu_check_common)
endif()

add_executable(npu_check_cli
  main.cpp
  options.cpp
  process_runner.cpp
  uds_client.cpp
)
target_compile_features(npu_check_cli PRIVATE cxx_std_17)
target_link_libraries(npu_check_cli PRIVATE npu_check_common npu_sanitizer_plog Threads::Threads Boost::filesystem)
target_compile_options(npu_check_cli PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(npu_check_cli PROPERTIES OUTPUT_NAME npu-check)
