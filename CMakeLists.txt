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
include(cmake/fetch_cann_cmake.cmake)
project(asc-tools)

init_cann_project(PREPEND_MODULE_PATH)

add_compile_options(-Werror)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
  message(STATUS "compile project asc-tools with library")
  option(BUILD_WITH_INSTALLED_DEPENDENCY_CANN_PKG "Build asc-tools with cann pkg" ON)
else()
  message(STATUS "compile project asc-tools with src")
  option(BUILD_WITH_INSTALLED_DEPENDENCY_CANN_PKG "Build asc-tools with cann source" OFF)
endif()

# BUILD_OPEN_PROJECT用于控制部分legacy包的编译，ENABLE_MOD_EXT用于控制MDC等芯片的编译
set(ENABLE_NPU_TOOLS OFF)
if(BUILD_OPEN_PROJECT AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR AND NOT ENABLE_MOD_EXT)
  set(ENABLE_NPU_TOOLS ON)
endif()

if (ENABLE_TEST)
    set(CMAKE_SKIP_RPATH FALSE)
else()
    set(CMAKE_SKIP_RPATH TRUE)
endif()

set(KIRIN_BUILD_CPU_DEBUG OFF)
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_CXX_FLAGS_RELEASE "")

include(version.cmake)

if(BUILD_OPEN_PROJECT)
    find_package(Python3 COMPONENTS Interpreter)
    set(HI_PYTHON ${Python3_EXECUTABLE})
    set(PYTHON ${Python3_EXECUTABLE})
    set(PVMODEL_PATH ${ASCEND_CANN_PACKAGE_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/simulator)
    message(STATUS "the PVMODEL_PATH is :${PVMODEL_PATH}")

    include(cmake/dependencies.cmake) # 其中会根据PVMODEL_PATH作为pvmodel相关依赖路径
    include(cmake/intf_pub_linux.cmake)
    include(cmake/package.cmake)
endif()

set(ASCEND_INSTALL_PATH "${ASCEND_CANN_PACKAGE_PATH}")

if(BUILD_OPEN_PROJECT)
  include(cmake/third_party/cpudebug.cmake)
endif()
check_cann_pkg_build_deps("asc-tools")
add_cann_version_info_targets()

add_subdirectory(cpudebug)
add_subdirectory(npuchk)
add_subdirectory(utils/msobjdump)
add_subdirectory(utils/optype_collector)
add_subdirectory(utils/templates)
if(BUILD_OPEN_PROJECT AND NOT ENABLE_TEST)
  add_subdirectory(third_party)
endif()
add_subdirectory(utils/show_kernel_debug_data)

if(ENABLE_NPU_TOOLS)
  include(cmake/third_party/npu_bit.cmake)
  add_subdirectory(npu_tools)
endif()

if(ENABLE_TEST)
  enable_testing()
  add_subdirectory(tests)
endif()
