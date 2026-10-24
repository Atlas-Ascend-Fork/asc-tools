# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
add_library(npu_compute_ipc STATIC wire_protocol.cpp uds_transport.cpp)
target_include_directories(npu_compute_ipc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(npu_compute_ipc PRIVATE c_sec_static)
target_compile_features(npu_compute_ipc PUBLIC cxx_std_17)
target_compile_definitions(npu_compute_ipc PUBLIC _GLIBCXX_USE_CXX11_ABI=0)
set_target_properties(npu_compute_ipc PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
