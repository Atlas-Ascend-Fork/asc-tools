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
  project(npu_compute LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NPU_COMPUTE_INSTALL_ARCH_DIR "${CMAKE_SYSTEM_PROCESSOR}-linux")
set(NPU_COMPUTE_INSTALL_BINDIR "${NPU_COMPUTE_INSTALL_ARCH_DIR}/bin")
set(NPU_COMPUTE_INSTALL_INTERNAL_DIR "tools/npu_tools")
set(NPU_COMPUTE_INSTALL_INTERNAL_BINDIR "${NPU_COMPUTE_INSTALL_INTERNAL_DIR}/bin")
set(NPU_COMPUTE_INSTALL_LIBDIR "${NPU_COMPUTE_INSTALL_INTERNAL_DIR}/lib64")

find_package(Threads REQUIRED)
set(CMAKE_SKIP_RPATH FALSE)

set(NPUCOMPUTE_CANN_ROOT "" CACHE PATH
  "CANN architecture package root used by NPU Compute"
)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/securec.cmake")
# 与 cpudebug/injection 的 CANN 依赖机制保持一致：
# 框架流程（外部开源顶层构建、内部联编）下依赖统一由 find_cann_package
# （dependencies.cmake 已注册 acl_rt/msprof）解析为框架目标；
# 无框架（独立编译本目录）时回退为 CANN 包路径查找。
if(COMMAND find_cann_package)
  find_cann_package(acl_rt REQUIRED)
  find_cann_package(msprof REQUIRED)
  set(NPUCOMPUTE_CANN_TARGETS ON)
  set(NPUCOMPUTE_ACL_INCLUDE_DIR "${_CANN_ACL_RT_INCLUDE_DIR}" CACHE INTERNAL
    "CANN ACL include dir resolved by find_cann_package(acl_rt)")
  set(NPUCOMPUTE_PROFILING_INCLUDE_DIR "${_CANN_MSPROF_INCLUDE_DIR}" CACHE INTERNAL
    "CANN profiling include dir resolved by find_cann_package(msprof)")
  message(STATUS "NPU Compute CANN headers resolved by find_cann_package")
else()
  message(STATUS "NPU Compute CANN headers resolved from package paths")
endif()

if(NOT NPUCOMPUTE_CANN_TARGETS)
find_path(NPUCOMPUTE_ACL_INCLUDE_DIR NAMES acl/acl.h HINTS
  "${NPUCOMPUTE_CANN_ROOT}/include"
  "${NPUCOMPUTE_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
  "${TOP_DIR}/inc/external"
  "${ASCEND_CANN_PACKAGE_PATH}/include"
  "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
  "$ENV{ASCEND_HOME_PATH}/include"
  "$ENV{ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/include"
  NO_CMAKE_FIND_ROOT_PATH)
find_path(NPUCOMPUTE_PROFILING_INCLUDE_DIR NAMES profiling/prof_common.h HINTS
  "${NPUCOMPUTE_CANN_ROOT}/pkg_inc"
  "${NPUCOMPUTE_CANN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc"
  "${TOP_DIR}/pkg_inc"
  "${ASCEND_CANN_PACKAGE_PATH}/pkg_inc"
  "${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc"
  "$ENV{ASCEND_HOME_PATH}/pkg_inc"
  "$ENV{ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc"
  NO_CMAKE_FIND_ROOT_PATH)
if(NOT NPUCOMPUTE_ACL_INCLUDE_DIR OR NOT NPUCOMPUTE_PROFILING_INCLUDE_DIR)
  message(FATAL_ERROR "NPU Compute CANN headers were not found. Set NPUCOMPUTE_CANN_ROOT. "
    "(NPUCOMPUTE_CANN_ROOT=${NPUCOMPUTE_CANN_ROOT}, "
    "ASCEND_CANN_PACKAGE_PATH=${ASCEND_CANN_PACKAGE_PATH}, "
    "ASCEND_HOME_PATH=$ENV{ASCEND_HOME_PATH}, TOP_DIR=${TOP_DIR})")
endif()
endif()

if(DEFINED NPU_TOOLS_LIBRARY_OUTPUT_DIR AND DEFINED NPU_TOOLS_RUNTIME_OUTPUT_DIR)
  set(NPU_COMPUTE_LIBRARY_OUTPUT_DIR "${NPU_TOOLS_LIBRARY_OUTPUT_DIR}")
  set(NPU_COMPUTE_RUNTIME_OUTPUT_DIR "${NPU_TOOLS_RUNTIME_OUTPUT_DIR}")
else()
  set(NPU_COMPUTE_LIBRARY_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib64")
  set(NPU_COMPUTE_RUNTIME_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()

function(npu_compute_set_product_output target)
  set_target_properties(${target} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${NPU_COMPUTE_LIBRARY_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${NPU_COMPUTE_LIBRARY_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${NPU_COMPUTE_RUNTIME_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
  )
endfunction()

# SO logging is owned by npu-compute and depends directly on the CANN backend.
add_library(npu_compute_plog_headers INTERFACE)
target_include_directories(npu_compute_plog_headers INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/common>"
)
target_link_libraries(npu_compute_plog_headers INTERFACE unified_dlog_headers)
add_library(npu_compute_plog INTERFACE)
target_link_libraries(npu_compute_plog INTERFACE npu_compute_plog_headers unified_dlog)

add_library(npu_compute_headers INTERFACE)
target_compile_definitions(npu_compute_headers INTERFACE _GLIBCXX_USE_CXX11_ABI=0)
target_include_directories(npu_compute_headers INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
)
if(NOT NPUCOMPUTE_CANN_TARGETS)
  target_include_directories(npu_compute_headers INTERFACE
    "$<BUILD_INTERFACE:${NPUCOMPUTE_ACL_INCLUDE_DIR}>"
    "$<BUILD_INTERFACE:${NPUCOMPUTE_PROFILING_INCLUDE_DIR}>"
    "$<BUILD_INTERFACE:${NPUCOMPUTE_PROFILING_INCLUDE_DIR}/profiling>"
  )
else()
  target_link_libraries(npu_compute_headers INTERFACE acl_rt_headers msprof_headers)
endif()

add_library(npu_compute_backend_headers INTERFACE)
target_link_libraries(npu_compute_backend_headers INTERFACE npu_compute_headers injection_headers)

add_library(npu_compute_integration_headers INTERFACE)
target_link_libraries(npu_compute_integration_headers INTERFACE npu_compute_backend_headers)

add_library(acl_runtime_backend INTERFACE)
add_library(acl_prof_api_backend INTERFACE)
target_link_libraries(acl_runtime_backend INTERFACE injection_runtime_backend)
target_link_libraries(acl_prof_api_backend INTERFACE injection_prof_api_backend)

add_subdirectory(src/acl_pti)
add_subdirectory(src/ipc)

# Private adapter only; SQLite itself is supplied by the runtime environment.
add_library(npu_compute_sqlite_runtime STATIC src/shared/sqlite_runtime_loader.cpp)
target_include_directories(npu_compute_sqlite_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(npu_compute_sqlite_runtime PUBLIC cxx_std_17)
target_compile_definitions(npu_compute_sqlite_runtime PUBLIC _GLIBCXX_USE_CXX11_ABI=0)
target_link_libraries(npu_compute_sqlite_runtime PUBLIC ${CMAKE_DL_LIBS} Threads::Threads Boost::filesystem)
set_target_properties(npu_compute_sqlite_runtime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

add_subdirectory(src/compute)
add_subdirectory(src/cli)

install(TARGETS acl_pti npu_compute
  LIBRARY DESTINATION "${NPU_COMPUTE_INSTALL_LIBDIR}" ${INSTALL_OPTIONAL}
  COMPONENT asc-tools
)

install(TARGETS npu_compute_cli
  RUNTIME DESTINATION "${NPU_COMPUTE_INSTALL_INTERNAL_BINDIR}" ${INSTALL_OPTIONAL}
  COMPONENT asc-tools
)

install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/npu_compute.sh"
  DESTINATION "${NPU_COMPUTE_INSTALL_BINDIR}" ${INSTALL_OPTIONAL}
  PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
  RENAME npu-compute
  COMPONENT asc-tools
)
