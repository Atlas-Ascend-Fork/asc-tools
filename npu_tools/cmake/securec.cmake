# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
# Use the same Secure C target as asc-devkit's host tools.
if(TARGET c_sec_static)
  return()
endif()
if(COMMAND find_cann_package)
  find_cann_package(securec REQUIRED)
  return()
endif()

# Standalone npu_compute builds do not load the CANN CMake package helpers.
find_path(NPU_TOOLS_SECUREC_INCLUDE_DIR securec.h
  HINTS
    "${NPUCOMPUTE_CANN_ROOT}/include"
    "${NPUCOMPUTE_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
    "${ASCEND_CANN_PACKAGE_PATH}/include"
    "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
    "$ENV{ASCEND_HOME_PATH}/include"
  NO_CMAKE_FIND_ROOT_PATH)
find_library(NPU_TOOLS_SECUREC_LIBRARY NAMES libc_sec.a c_sec securec boundscheck
  HINTS
    "${NPUCOMPUTE_CANN_ROOT}/lib64"
    "${NPUCOMPUTE_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
    "${ASCEND_CANN_PACKAGE_PATH}/lib64"
    "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
    "$ENV{ASCEND_HOME_PATH}/lib64"
  NO_CMAKE_FIND_ROOT_PATH)
if(NOT NPU_TOOLS_SECUREC_INCLUDE_DIR OR NOT NPU_TOOLS_SECUREC_LIBRARY)
  message(FATAL_ERROR "Secure C was not found. Set NPU_TOOLS_SECUREC_INCLUDE_DIR and NPU_TOOLS_SECUREC_LIBRARY.")
endif()
add_library(c_sec_static UNKNOWN IMPORTED GLOBAL)
set_target_properties(c_sec_static PROPERTIES
  IMPORTED_LOCATION "${NPU_TOOLS_SECUREC_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${NPU_TOOLS_SECUREC_INCLUDE_DIR}")
