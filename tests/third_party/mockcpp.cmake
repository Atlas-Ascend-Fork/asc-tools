# 依赖蓝区二进制仓mockcpp
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
if (NOT EXISTS "${CMAKE_INSTALL_PREFIX}/mockcpp/lib/libmockcpp.a")
    set(mockcpp_SRC_DIR ${ASCENDC_TOOLS_ROOT_DIR}/tests/third_party/mockcpp_src)
    set(mockcpp_SOURCE_DIR ${mockcpp_SRC_DIR}/mockcpp)
    if (CMAKE_HOST_SYSTEM_PROCESSOR  STREQUAL "aarch64")
        set(mockcpp_CXXFLAGS "-fPIC")
    else()
        set(mockcpp_CXXFLAGS "-fPIC -std=c++11")
    endif()
    set(mockcpp_FLAGS "-fPIC")
    set(mockcpp_LINKER_FLAGS "")

    if ((NOT DEFINED ABI_ZERO) OR (ABI_ZERO STREQUAL ""))
        set(ABI_ZERO "true")
    endif()

    if (ABI_ZERO STREQUAL true)
        set(mockcpp_CXXFLAGS "${mockcpp_CXXFLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")
        set(mockcpp_FLAGS "${mockcpp_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")
    endif()

    set(BUILD_WRAPPER ${ASCENDC_TOOLS_ROOT_DIR}/tests/cmake/tools/build_ext.sh)
    set(BUILD_TYPE "DEBUG")

    if (CMAKE_GENERATOR MATCHES "Unix Makefiles")
        set(IS_MAKE True)
        set(MAKE_CMD "$(MAKE)")
    else()
        set(IS_MAKE False)
    endif()

    file(GLOB MOCKCPP_PKG
        LIST_DIRECTORIES True
        ${CANN_3RD_LIB_PATH}/mockcpp*.tar.gz
    )
    if(NOT EXISTS ${MOCKCPP_PKG})
        set(REQ_URL "https://gitcode.com/cann-src-third-party/mockcpp/releases/download/v2.7-h2/mockcpp-2.7.tar.gz")
    else()
        set(REQ_URL ${MOCKCPP_PKG})
    endif()

    set(MOCKCPP_PATCH ${CANN_3RD_LIB_PATH}/mockcpp-2.7_py3-h3.patch)
    if(NOT EXISTS "${MOCKCPP_PATCH}")
        set(MOCKCPP_PATCH ${CANN_3RD_LIB_PATH}/patch/mockcpp-2.7_py3-h3.patch)
    endif()
    set(PATCH_FILE ${third_party_TEM_DIR}/mockcpp-2.7_py3.patch)
    set(MOCKCPP_SRC_PATH "${CANN_3RD_LIB_PATH}/../llt/third_party/mockcpp_src")
    set(MOCKCPP_OPTS
        -DCMAKE_CXX_FLAGS=${mockcpp_CXXFLAGS}
        -DCMAKE_C_FLAGS=${mockcpp_FLAGS}
        -DBOOST_INCLUDE_DIRS=${BOOST_SRC_PATH}
        -DCMAKE_SHARED_LINKER_FLAGS=${mockcpp_LINKER_FLAGS}
        -DCMAKE_EXE_LINKER_FLAGS=${mockcpp_LINKER_FLAGS}
        -DBUILD_32_BIT_TARGET_BY_64_BIT_COMPILER=OFF
        -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}/mockcpp
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
    )
    include(ExternalProject)
    if(EXISTS ${MOCKCPP_SRC_PATH})
        message("Found local mockcpp source: ${MOCKCPP_SRC_PATH}")
        file(COPY ${MOCKCPP_SRC_PATH}/ DESTINATION "${mockcpp_SRC_DIR}/")
        ExternalProject_Add(mockcpp
            SOURCE_DIR ${mockcpp_SRC_DIR}
            CONFIGURE_COMMAND ${CMAKE_COMMAND} -G ${CMAKE_GENERATOR} ${MOCKCPP_OPTS} <SOURCE_DIR>
            BUILD_COMMAND ${BUILD_WRAPPER} default ${${BUILD_TYPE}} $<$<BOOL:${IS_MAKE}>:$(MAKE)>
        )
    else()
        message("No local mockcpp source, downloading from ${REQ_URL}")
        if (NOT EXISTS ${PATCH_FILE})
            if(NOT EXISTS ${MOCKCPP_PATCH})
                set(PATCH_URL "https://gitcode.com/cann-src-third-party/mockcpp/releases/download/v2.7-h3/mockcpp-2.7_py3-h3.patch")
                message("No local patch, downloading from ${PATCH_URL}")
                file(DOWNLOAD
                    ${PATCH_URL}
                    ${PATCH_FILE}
                    TIMEOUT 60
                    EXPECTED_HASH SHA256=30f78d8173d50fa9af36efbc683aee82bcd5afc7acdc4dbef7381b92a1b4c800
                )
            else()
                message("No local patch, install from ${MOCKCPP_PATCH}")
                file(COPY ${MOCKCPP_PATCH} DESTINATION ${third_party_TEM_DIR}/)
                file(RENAME ${third_party_TEM_DIR}/mockcpp-2.7_py3-h3.patch ${PATCH_FILE})
            endif()
        endif()
        ExternalProject_Add(mockcpp
            URL ${REQ_URL}
            URL_HASH SHA256=73ab0a8b6d1052361c2cebd85e022c0396f928d2e077bf132790ae3be766f603
            DEPENDS third_party_boost
            DOWNLOAD_DIR ${third_party_TEM_DIR}
            SOURCE_DIR ${mockcpp_SRC_DIR}
            TLS_VERIFY OFF
            PATCH_COMMAND git init && git apply ${PATCH_FILE} && sed -i "1i cmake_minimum_required(VERSION 3.16.0)" CMakeLists.txt && rm -rf .git
            CONFIGURE_COMMAND ${CMAKE_COMMAND} -G ${CMAKE_GENERATOR} ${MOCKCPP_OPTS} <SOURCE_DIR>
            BUILD_COMMAND ${BUILD_WRAPPER} default ${${BUILD_TYPE}} $<$<BOOL:${IS_MAKE}>:$(MAKE)>
        )
    endif()
endif()

message("cmake install prefix is ${CMAKE_INSTALL_PREFIX}")
set(MOCKCPP_DIR ${CMAKE_INSTALL_PREFIX}/mockcpp)

set(MOCKCPP_INCLUDE_ONE ${MOCKCPP_DIR}/include)

set(MOCKCPP_INCLUDE_TWO ${BOOST_SRC_PATH})

set(MOCKCPP_STATIC_LIBRARY ${MOCKCPP_DIR}/lib/libmockcpp.a)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(mockcpp
    REQUIRED_VARS MOCKCPP_INCLUDE_ONE MOCKCPP_INCLUDE_TWO MOCKCPP_STATIC_LIBRARY
)

message("mockcpp_FOUND is ${mockcpp_FOUND}")

if(mockcpp_FOUND)
    set(MOCKCPP_INCLUDE_DIR ${MOCKCPP_INCLUDE_ONE} ${MOCKCPP_INCLUDE_TWO})
    get_filename_component(MOCKCPP_LIBRARY_DIR ${MOCKCPP_STATIC_LIBRARY} DIRECTORY)

    if(NOT TARGET mockcpp_static)
        add_library(mockcpp_static STATIC IMPORTED)
        set_target_properties(mockcpp_static PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${MOCKCPP_INCLUDE_DIR}"
            IMPORTED_LOCATION "${MOCKCPP_STATIC_LIBRARY}"
            )
        add_dependencies(mockcpp_static mockcpp)
    endif()
endif()
