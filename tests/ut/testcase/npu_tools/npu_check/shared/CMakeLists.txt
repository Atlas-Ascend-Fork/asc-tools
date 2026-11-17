# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
# Test-only CANN library providing the directly linked dlog entry points.
# Keep this library in the test build tree; do not install it.
add_library(plog_test_library STATIC plog_test_library.cpp)
target_compile_features(plog_test_library PUBLIC cxx_std_17)
target_include_directories(plog_test_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(plog_test_library PRIVATE Threads::Threads)
set_target_properties(plog_test_library PROPERTIES
  CXX_VISIBILITY_PRESET default
)

# aclsan_log 用例已并入 llt/npu_tools_utest（GTest 化）。
