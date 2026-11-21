# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
add_library(acl_tool_injection_test SHARED
  ${INJECTION_SOURCE_DIR}/src/injection_hook.cpp
)
target_compile_definitions(acl_tool_injection_test PRIVATE ACL_TOOL_INJECTION_BUILD=1)
target_include_directories(acl_tool_injection_test PRIVATE "${INJECTION_SOURCE_DIR}/src")
target_link_libraries(acl_tool_injection_test
  PUBLIC injection_test_headers
  PRIVATE Threads::Threads acl_runtime_stub acl_prof_api_stub ${CMAKE_DL_LIBS}
)
target_compile_options(acl_tool_injection_test PRIVATE -Wall -Wextra -Werror)
target_link_options(acl_tool_injection_test PRIVATE -Wl,-z,defs -Wl,--disable-new-dtags)
set_target_properties(acl_tool_injection_test PROPERTIES
  OUTPUT_NAME acl_tool_injection_test
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)
injection_set_test_output(acl_tool_injection_test)

# injection hook 用例已并入 llt/npu_tools_utest（桩版 runtime 经链接期剥离真实链解决同名冲突）。

add_library(acl_tool_injection_test_unit STATIC
  ${INJECTION_SOURCE_DIR}/src/injection_hook.cpp
)
target_compile_definitions(acl_tool_injection_test_unit PRIVATE ACL_TOOL_INJECTION_BUILD=1)
target_include_directories(acl_tool_injection_test_unit PRIVATE "${INJECTION_SOURCE_DIR}/src")
target_link_libraries(acl_tool_injection_test_unit
  PUBLIC injection_test_headers
  PRIVATE Threads::Threads acl_runtime_stub_unit ${CMAKE_DL_LIBS}
)
target_compile_options(acl_tool_injection_test_unit PRIVATE -Wall -Wextra -Werror)
