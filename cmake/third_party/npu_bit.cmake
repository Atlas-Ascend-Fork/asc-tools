# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

if (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(NPU_BIT_PKG_ARCH x86_64)
    set(NPU_BIT_RAR_ARCH x86)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(NPU_BIT_PKG_ARCH aarch64)
    set(NPU_BIT_RAR_ARCH aarch64)
else ()
    message(FATAL_ERROR "Unsupported npu_bit architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

set(NPU_BIT_PKG_BASE "cann-asc-tools-deps_${ASC_TOOLS_VERSION}_linux-${NPU_BIT_PKG_ARCH}")
set(NPU_BIT_PKG_NAME "${NPU_BIT_PKG_BASE}.tar.gz")
set(NPU_BIT_RAR_NAME "cann_asc_tools_deps_${NPU_BIT_RAR_ARCH}_pkg_compile.rar")
set(NPU_BIT_LIB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../libraries/npu_bit")
set(NPU_BIT_LIB_PATH "${NPU_BIT_LIB_ROOT}/${NPU_BIT_PKG_BASE}")

if (NOT EXISTS "${NPU_BIT_LIB_PATH}/include/npubit.h"
    OR NOT EXISTS "${NPU_BIT_LIB_PATH}/lib/libnpu_bit.a"
    OR NOT EXISTS "${NPU_BIT_LIB_PATH}/lib/libnpu_bit.so")
    if(NOT CANN_3RD_LIB_PATH)
        set(CANN_3RD_LIB_PATH "${CMAKE_CURRENT_LIST_DIR}/../../third_party")
    endif()
    file(MAKE_DIRECTORY "${CANN_3RD_LIB_PATH}" "${NPU_BIT_LIB_ROOT}")
    set(NPU_BIT_PKG "${CANN_3RD_LIB_PATH}/${NPU_BIT_PKG_NAME}")

    if(NOT EXISTS "${NPU_BIT_PKG}")
        set(NPU_BIT_RAR "${CANN_3RD_LIB_PATH}/${NPU_BIT_RAR_NAME}")
        if(NOT EXISTS "${NPU_BIT_RAR}" AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../${NPU_BIT_RAR_NAME}")
            set(NPU_BIT_RAR "${CMAKE_CURRENT_LIST_DIR}/../../${NPU_BIT_RAR_NAME}")
        endif()
        if(NOT EXISTS "${NPU_BIT_RAR}")
            if(NOT NPU_BIT_PKG_URL)
                set(NPU_BIT_PKG_URL "https://ascend-cann.obs.cn-north-4.myhuaweicloud.com/CANN/20261109_newest/${NPU_BIT_RAR_NAME}")
            endif()
            message(STATUS "Downloading npu_bit dependencies from ${NPU_BIT_PKG_URL}")
            include(FetchContent)
            FetchContent_Declare(
                npu_bit_deps
                URL "${NPU_BIT_PKG_URL}"
                DOWNLOAD_DIR "${CANN_3RD_LIB_PATH}"
                DOWNLOAD_NAME "${NPU_BIT_RAR_NAME}"
                DOWNLOAD_NO_EXTRACT TRUE
            )
            FetchContent_MakeAvailable(npu_bit_deps)
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xf "${NPU_BIT_RAR}" "${NPU_BIT_PKG_NAME}"
            WORKING_DIRECTORY "${CANN_3RD_LIB_PATH}"
            RESULT_VARIABLE NPU_BIT_EXTRACT_RESULT
        )
        if(NOT NPU_BIT_EXTRACT_RESULT STREQUAL "0")
            message(FATAL_ERROR "Failed to extract ${NPU_BIT_PKG_NAME} from ${NPU_BIT_RAR}")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${NPU_BIT_PKG}"
        WORKING_DIRECTORY "${NPU_BIT_LIB_ROOT}"
        RESULT_VARIABLE NPU_BIT_EXTRACT_RESULT
    )
    if(NOT NPU_BIT_EXTRACT_RESULT STREQUAL "0")
        message(FATAL_ERROR "Failed to extract npu_bit dependencies: ${NPU_BIT_PKG}")
    endif()
    foreach(NPU_BIT_FILE IN ITEMS include/npubit.h lib/libnpu_bit.a lib/libnpu_bit.so)
        if(NOT EXISTS "${NPU_BIT_LIB_PATH}/${NPU_BIT_FILE}")
            message(FATAL_ERROR "npu_bit dependencies are missing ${NPU_BIT_FILE}: ${NPU_BIT_PKG}")
        endif()
    endforeach()
endif()

add_library(npu_bit STATIC IMPORTED)
set_target_properties(npu_bit PROPERTIES
    IMPORTED_LOCATION "${NPU_BIT_LIB_PATH}/lib/libnpu_bit.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NPU_BIT_LIB_PATH}/include"
)

install(FILES "${NPU_BIT_LIB_PATH}/lib/libnpu_bit.so"
    DESTINATION tools/npu_bit/lib64
    PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    COMPONENT asc-tools
)
install(FILES "${NPU_BIT_LIB_PATH}/lib/libnpu_bit.a"
    DESTINATION tools/npu_bit/lib64
    COMPONENT asc-tools
)
install(DIRECTORY "${NPU_BIT_LIB_PATH}/include/"
    DESTINATION tools/npu_bit/include
    COMPONENT asc-tools
    FILES_MATCHING PATTERN "*.h"
)
