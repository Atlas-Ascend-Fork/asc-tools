# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16.0)
  project(acl_tool_injection LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(INJECTION_CANN_ROOT "" CACHE PATH "CANN architecture package root used by Runtime injection")
set(INJECTION_INSTALL_LIBDIR "tools/npu_tools/lib64")
if(NOT DEFINED NPU_TOOLS_LIBRARY_OUTPUT_DIR)
  set(NPU_TOOLS_LIBRARY_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib64")
endif()
if(NOT DEFINED NPU_TOOLS_RUNTIME_OUTPUT_DIR)
  set(NPU_TOOLS_RUNTIME_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()

# 与 cpudebug/dependencies.cmake 的 CANN 依赖机制保持一致：
# - 框架流程（外部开源顶层构建、内部联编，prepare.cmake 已加载）：依赖统一由
#   find_cann_package（dependencies.cmake 已注册 acl_rt/msprof）解析为框架目标，
#   内部联编时框架将 runtime 以二进制组件或同工程目标提供并统一处理搜索路径；
# - 无框架（独立编译本目录）时回退为 CANN 包路径查找。
if(COMMAND find_cann_package)
  find_cann_package(acl_rt REQUIRED)
  find_cann_package(msprof REQUIRED)
  set(INJECTION_CANN_TARGETS ON)
  set(INJECTION_ACL_INCLUDE_DIR "${_CANN_ACL_RT_INCLUDE_DIR}" CACHE INTERNAL
    "CANN ACL include dir resolved by find_cann_package(acl_rt)")
  set(INJECTION_PROFILING_INCLUDE_DIR "${_CANN_MSPROF_INCLUDE_DIR}" CACHE INTERNAL
    "CANN profiling include dir resolved by find_cann_package(msprof)")
  message(STATUS "Runtime injection CANN dependencies resolved by find_cann_package")
else()
  message(STATUS "Runtime injection CANN dependencies resolved from package paths")
endif()

if(NOT INJECTION_CANN_TARGETS)
find_path(INJECTION_ACL_INCLUDE_DIR
  NAMES acl/acl.h
  HINTS
    "${INJECTION_CANN_ROOT}/include"
    "${INJECTION_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
    "${TOP_DIR}/inc/external"
    "${ASCEND_CANN_PACKAGE_PATH}/include"
    "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
    "$ENV{ASCEND_HOME_PATH}/include"
    "$ENV{ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
  NO_CMAKE_FIND_ROOT_PATH
)
if(NOT INJECTION_ACL_INCLUDE_DIR)
  message(FATAL_ERROR "Runtime injection CANN ACL headers were not found. Set INJECTION_CANN_ROOT. "
    "(INJECTION_CANN_ROOT=${INJECTION_CANN_ROOT}, "
    "ASCEND_CANN_PACKAGE_PATH=${ASCEND_CANN_PACKAGE_PATH}, "
    "ASCEND_HOME_PATH=$ENV{ASCEND_HOME_PATH}, TOP_DIR=${TOP_DIR})")
endif()

find_path(INJECTION_PROFILING_INCLUDE_DIR
  NAMES profiling/prof_common.h
  HINTS
    "${INJECTION_CANN_ROOT}/pkg_inc"
    "${INJECTION_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc"
    "${TOP_DIR}/pkg_inc"
    "${ASCEND_CANN_PACKAGE_PATH}/pkg_inc"
    "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc"
    "$ENV{ASCEND_HOME_PATH}/pkg_inc"
    "$ENV{ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc"
  NO_CMAKE_FIND_ROOT_PATH
)
if(NOT INJECTION_PROFILING_INCLUDE_DIR)
  message(FATAL_ERROR "Runtime injection CANN profiling headers were not found. Set INJECTION_CANN_ROOT. "
    "(INJECTION_CANN_ROOT=${INJECTION_CANN_ROOT}, "
    "ASCEND_CANN_PACKAGE_PATH=${ASCEND_CANN_PACKAGE_PATH}, "
    "ASCEND_HOME_PATH=$ENV{ASCEND_HOME_PATH}, TOP_DIR=${TOP_DIR})")
endif()
endif()

add_library(injection_headers INTERFACE)
target_include_directories(injection_headers INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
)
if(NOT INJECTION_CANN_TARGETS)
  target_include_directories(injection_headers INTERFACE
    "$<BUILD_INTERFACE:${INJECTION_ACL_INCLUDE_DIR}>"
    "$<BUILD_INTERFACE:${INJECTION_PROFILING_INCLUDE_DIR}>"
    "$<BUILD_INTERFACE:${INJECTION_PROFILING_INCLUDE_DIR}/profiling>"
  )
else()
  target_link_libraries(injection_headers INTERFACE acl_rt_headers msprof_headers)
endif()
add_library(injection_runtime_backend INTERFACE)
add_library(injection_prof_api_backend INTERFACE)
if(NOT INJECTION_CANN_TARGETS)
get_filename_component(INJECTION_ACL_INCLUDE_REALPATH "${INJECTION_ACL_INCLUDE_DIR}" REALPATH)
get_filename_component(INJECTION_CANN_ARCH_ROOT "${INJECTION_ACL_INCLUDE_REALPATH}" DIRECTORY)
find_library(INJECTION_ACL_RT_LIBRARY NAMES acl_rt HINTS
  "${INJECTION_CANN_ROOT}/lib64"
  "${INJECTION_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
  "${INJECTION_CANN_ARCH_ROOT}/lib64"
  "${TOP_DIR}/lib64"
  "${ASCEND_CANN_PACKAGE_PATH}/lib64"
  "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
  "$ENV{ASCEND_HOME_PATH}/lib64"
  "$ENV{ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(INJECTION_PROFAPI_LIBRARY NAMES profapi HINTS
  "${INJECTION_CANN_ROOT}/lib64"
  "${INJECTION_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
  "${INJECTION_CANN_ARCH_ROOT}/lib64"
  "${TOP_DIR}/lib64"
  "${ASCEND_CANN_PACKAGE_PATH}/lib64"
  "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
  "$ENV{ASCEND_HOME_PATH}/lib64"
  "$ENV{ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/lib64"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT INJECTION_ACL_RT_LIBRARY OR NOT INJECTION_PROFAPI_LIBRARY)
  message(FATAL_ERROR "Runtime injection CANN libraries were not found. Set INJECTION_CANN_ROOT.")
endif()
add_library(Injection::acl_rt SHARED IMPORTED GLOBAL)
set_target_properties(Injection::acl_rt PROPERTIES IMPORTED_LOCATION "${INJECTION_ACL_RT_LIBRARY}")
add_library(Injection::profapi SHARED IMPORTED GLOBAL)
set_target_properties(Injection::profapi PROPERTIES IMPORTED_LOCATION "${INJECTION_PROFAPI_LIBRARY}")
target_link_libraries(injection_runtime_backend INTERFACE Injection::acl_rt)
target_link_libraries(injection_prof_api_backend INTERFACE Injection::profapi)
else()
target_link_libraries(injection_runtime_backend INTERFACE acl_rt)
target_link_libraries(injection_prof_api_backend INTERFACE profapi)
add_library(Injection::acl_rt INTERFACE IMPORTED GLOBAL)
target_link_libraries(Injection::acl_rt INTERFACE acl_rt)
add_library(Injection::profapi INTERFACE IMPORTED GLOBAL)
target_link_libraries(Injection::profapi INTERFACE profapi)
endif()

find_package(Threads REQUIRED)
add_library(acl_tool_injection SHARED src/injection_hook.cpp)
target_compile_definitions(acl_tool_injection PRIVATE ACL_TOOL_INJECTION_BUILD=1)
target_include_directories(acl_tool_injection PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_library(injection_plog_headers INTERFACE)
target_include_directories(injection_plog_headers INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(injection_plog_headers INTERFACE unified_dlog_headers)
add_library(injection_plog INTERFACE)
target_link_libraries(injection_plog INTERFACE injection_plog_headers unified_dlog)

target_link_libraries(acl_tool_injection PUBLIC injection_headers PRIVATE
  injection_plog Threads::Threads injection_runtime_backend injection_prof_api_backend)
target_compile_options(acl_tool_injection PRIVATE -Wall -Wextra -Werror)
target_link_options(acl_tool_injection PRIVATE -Wl,-z,defs)
set_target_properties(acl_tool_injection PROPERTIES
  OUTPUT_NAME acl_tool_injection
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  ARCHIVE_OUTPUT_DIRECTORY "${NPU_TOOLS_LIBRARY_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${NPU_TOOLS_LIBRARY_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${NPU_TOOLS_RUNTIME_OUTPUT_DIR}"
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN"
  SKIP_BUILD_RPATH FALSE
  BUILD_WITH_INSTALL_RPATH FALSE
)

install(TARGETS acl_tool_injection
  LIBRARY DESTINATION "${INJECTION_INSTALL_LIBDIR}" ${INSTALL_OPTIONAL}
  COMPONENT asc-tools)
