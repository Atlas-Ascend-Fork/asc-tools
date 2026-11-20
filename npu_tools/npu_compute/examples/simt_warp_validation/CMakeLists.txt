# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
cmake_minimum_required(VERSION 3.16)
project(simt_warp_10_cases LANGUAGES CXX)
find_program(BISHENG_EXECUTABLE NAMES bisheng)
if(NOT BISHENG_EXECUTABLE)
  message(FATAL_ERROR "bisheng not found; source the actual CANN set_env.sh first")
endif()
set(NPU_ARCH "dav-3510" CACHE STRING "Target Ascend device architecture")
foreach(case_number RANGE 1 10)
  if(case_number LESS 10)
    set(case_name "simt_case_0${case_number}")
  else()
    set(case_name "simt_case_${case_number}")
  endif()
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${case_name}"
    COMMAND "${BISHENG_EXECUTABLE}" -xasc "${CMAKE_CURRENT_SOURCE_DIR}/simt_cases.asc"
            "-DCASE_ID=${case_number}" "--npu-arch=${NPU_ARCH}" -lm -o "${CMAKE_CURRENT_BINARY_DIR}/${case_name}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/simt_cases.asc" VERBATIM)
  add_custom_target("build_${case_name}" ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${case_name}")
endforeach()
