# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
add_library(acl_runtime_stub SHARED
  runtime_stub.cpp
)
target_compile_definitions(acl_runtime_stub PRIVATE ACL_RUNTIME_STUB_SHARED)
target_include_directories(acl_runtime_stub
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${INJECTION_SOURCE_DIR}/src"
)
target_link_libraries(acl_runtime_stub
  PUBLIC injection_test_headers
  PRIVATE acl_prof_api_stub ${CMAKE_DL_LIBS}
)
set_target_properties(acl_runtime_stub PROPERTIES
  OUTPUT_NAME runtime
  ARCHIVE_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_CANN_RUNTIME_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_CANN_RUNTIME_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${NPU_TOOLS_TEST_CANN_RUNTIME_DIR}"
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN"
  SKIP_BUILD_RPATH FALSE
  BUILD_WITH_INSTALL_RPATH FALSE
)

# 静态孪生：供纯逻辑单测直链（源码编入测试二进制，无 .so、无同名抢占）。
# SHARED 版仅保留给以 DSO 边界/部署布局为测试对象的用例与探针。
add_library(acl_runtime_stub_unit STATIC
  runtime_stub.cpp
)
target_compile_definitions(acl_runtime_stub_unit PRIVATE ACL_RUNTIME_STUB_STATIC=1)
target_include_directories(acl_runtime_stub_unit
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${INJECTION_SOURCE_DIR}/src"
)
target_link_libraries(acl_runtime_stub_unit
  PUBLIC injection_test_headers
  PRIVATE ${CMAKE_DL_LIBS}
)
