# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
set(GENERATE_CPP_COV ${ASCENDC_TOOLS_ROOT_DIR}/tests/cmake/tools/generate_cpp_cov.sh)

set(third_party_TEM_DIR ${ASCENDC_TOOLS_ROOT_DIR}/build/tmp)

add_cann_third_party(gtest)
find_package(Threads REQUIRED)
target_link_libraries(GTest::gtest INTERFACE Threads::Threads)
add_cann_third_party(boost)
include(${ASCENDC_TOOLS_ROOT_DIR}/tests/third_party/mockcpp.cmake)
