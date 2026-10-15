# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

set(PKG_NAME "cpudebug_deps")
string(TOLOWER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_LOWER)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    message(STATUS "Detected architecture: x86_64")
    set(TAR_ARCH x86)
    set(CPUDEBUG_PKG_ARCH x86_64)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|arm")
    message(STATUS "Detected architecture: ARM64")
    set(TAR_ARCH aarch64)
    set(CPUDEBUG_PKG_ARCH aarch64)
else ()
    message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

if (IS_DIRECTORY ${CMAKE_SOURCE_DIR}/libraries/lib AND NOT EXISTS ${CMAKE_SOURCE_DIR}/libraries/lib/cmake/targets-tikicpulib-${BUILD_TYPE_LOWER}.cmake)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_SOURCE_DIR}/libraries/lib
        COMMENT "Removing ${CMAKE_SOURCE_DIR}/libraries/lib directory..."
    )
endif()

if (NOT EXISTS ${CMAKE_SOURCE_DIR}/libraries/lib/include/stub_fun.h)
    set(CPUDEBUG_PKG_NAME cann-asc-tools-cpudebug-deps-lib_${BUILD_TYPE_LOWER}_${ASC_TOOLS_VERSION}_linux-${CMAKE_SYSTEM_PROCESSOR}.tar.gz)
    file(GLOB CPUDEBUG_PKG
        LIST_DIRECTORIES True
        ${CANN_3RD_LIB_PATH}/${CPUDEBUG_PKG_NAME}
    )

    if(NOT EXISTS ${CPUDEBUG_PKG})
        set(CPUDEBUG_PKG_URL "https://ascend-ci.obs.cn-north-4.myhuaweicloud.com/asc-toolkit-dev/package/176/cann-asc-toolkit-dev_linux-${CPUDEBUG_PKG_ARCH}.tar.gz")
        message(STATUS "cpudebug pkg not found in ${CANN_3RD_LIB_PATH}, downloading from ${CPUDEBUG_PKG_URL}")
    else()
        set(CPUDEBUG_PKG_URL ${CPUDEBUG_PKG})
    endif()

    set(CPUDEBUG_PKG ${CANN_3RD_LIB_PATH}/${CPUDEBUG_PKG_NAME})
    include(FetchContent)
    FetchContent_Declare(
        ${PKG_NAME}
        URL ${CPUDEBUG_PKG_URL}
        TLS_VERIFY FALSE
        DOWNLOAD_DIR ${CANN_3RD_LIB_PATH}
        DOWNLOAD_NAME ${CPUDEBUG_PKG_NAME}
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(${PKG_NAME})

    execute_process(
        COMMAND tar -xf ${CPUDEBUG_PKG} -C ${CMAKE_SOURCE_DIR}/libraries --strip-components 1
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/libraries
        RESULT_VARIABLE result
    )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/libraries/lib/cmake/targets-tikicpulib.cmake)
    execute_process(
        COMMAND mv targets-tikicpulib.cmake targets-cpudebug.cmake
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/libraries/lib/cmake
    )
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/libraries/lib/cmake/targets-tikicpulib-release.cmake)
    execute_process(
        COMMAND mv targets-tikicpulib-release.cmake targets-cpudebug-release.cmake
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/libraries/lib/cmake
    )
endif()

add_library(cpudebug_stubreg SHARED IMPORTED)
set_target_properties(cpudebug_stubreg PROPERTIES
    IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/libraries/lib/libcpudebug_stubreg.so"
)
