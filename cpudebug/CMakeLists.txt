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
project(ascendc_cpulib)

if(BUILD_OPEN_PROJECT)
  set(ASCENDC_FRAMEWORK_DIR ${CMAKE_CURRENT_SOURCE_DIR})

  add_library(asc_tools_headers INTERFACE)
  target_include_directories(asc_tools_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/utils>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/utils/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/utils/include/utils>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/utils/include/basic_api>

    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/ascendc>
    $<INSTALL_INTERFACE:include/ascendc/include>
    $<INSTALL_INTERFACE:include/ascendc/include/basic_api>
    $<INSTALL_INTERFACE:include/ascendc/include/basic_api/impl>
    $<INSTALL_INTERFACE:include/ascendc/include/basic_api/interface>
  )

endif()

set(PRODUCT_TYPE_LIST ascend910 ascend310p ascend910B1 ascend310B1 ascend950pr_9599)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  list(APPEND PRODUCT_TYPE_LIST kirinx90 kirin9030)
endif()

set(CPULIB_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

include(cmake/fun.cmake)

add_subdirectory(src/acl_stub)

file(GLOB ASCENDC_CHECK_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api_check/*.cpp
)
file(GLOB ASCENDC_REGFWK_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/regfwk/kernel_print_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/regfwk/stub_backtrace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/regfwk/stub_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/regfwk/stub_reg.cpp
)

foreach(product_type ${PRODUCT_TYPE_LIST})
  execute_process(
    COMMAND ${CMAKE_AR} -x libcpudebug_model.a
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/libraries/lib/${product_type}
    RESULT_VARIABLE result
  )
  file(GLOB CPUDEBUG_FILES
    ${CMAKE_SOURCE_DIR}/libraries/lib/${product_type}/*.o
  )
  add_library(cpudebug_obj_${product_type} OBJECT IMPORTED)
  set_target_properties(cpudebug_obj_${product_type} PROPERTIES
    IMPORTED_OBJECTS "${CPUDEBUG_FILES}"
  )

  add_library(cpudebug_${product_type} SHARED
    ${ASCENDC_CHECK_SRC}
    ${ASCENDC_REGFWK_SRC}
    $<TARGET_OBJECTS:cpudebug_obj_${product_type}>
  )
  target_compile_definitions(cpudebug_${product_type} PRIVATE
    __CCE_KT_TEST__=1
    ASCENDC_CPU_DEBUG=1
    ASCENDC_DEBUG=1
    $<$<STREQUAL:${product_type},ascend910>:__CCE_AICORE__=100;__DAV_C100__;__NPU_ARCH__=1001>
    $<$<STREQUAL:${product_type},ascend310p>:__CCE_AICORE__=200;__DAV_M200__;__NPU_ARCH__=2002>
    $<$<STREQUAL:${product_type},ascend910B1>:__CCE_AICORE__=220;__DAV_C220__;__NPU_ARCH__=2201>
    $<$<STREQUAL:${product_type},ascend310B1>:__CCE_AICORE__=300;__DAV_M300__;__NPU_ARCH__=3002>
    $<$<STREQUAL:${product_type},ascend950pr_9599>:__CCE_AICORE__=310;__DAV_C310__;__NPU_ARCH__=3510>
    $<$<STREQUAL:${product_type},kirinx90>:__CCE_AICORE__=300;__DAV_L300__;__NPU_ARCH__=3003>
    $<$<STREQUAL:${product_type},kirin9030>:__CCE_AICORE__=311;__DAV_L311__;__NPU_ARCH__=3113>
  )
  target_compile_options(cpudebug_${product_type} PRIVATE
    -DCMAKE_C_COMPILER_LAUNCHER=${CCACHE_PROGRAM}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CCACHE_PROGRAM}
  )
  target_include_directories(cpudebug_${product_type} PRIVATE
    ${CMAKE_SOURCE_DIR}/libraries/lib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api_check
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api_check/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/include/utils
  )
  target_compile_definitions(cpudebug_${product_type} PRIVATE
    $<$<STREQUAL:${product_type},ascend910B1>:NO_COSIM>
    $<$<STREQUAL:${product_type},ascend310B1>:NO_COSIM>
    $<$<STREQUAL:${product_type},kirinx90>:NO_COSIM>
    $<$<STREQUAL:${product_type},kirin9030>:NO_COSIM>
    $<$<STREQUAL:${product_type},ascend950pr_9599>:NO_COSIM>
  )
  target_link_libraries(cpudebug_${product_type} PRIVATE
    $<BUILD_INTERFACE:intf_pub>
    $<BUILD_INTERFACE:asc_tools_headers>
    $<BUILD_INTERFACE:mmpa_headers>
    c_sec
    dl
    -Wl,--no-as-needed
    mmpa
    unified_dlog
    pthread
    -Wl,--as-needed
    $<$<CONFIG:Release>:-s>
  )
  set_target_properties(cpudebug_${product_type} PROPERTIES
    OUTPUT_NAME cpudebug
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${product_type}
  )
  product_dir(${product_type} Product_cap)
  if(BUILD_OPEN_PROJECT)
    install(TARGETS cpudebug_${product_type}
      LIBRARY DESTINATION tools/cpudebug/lib64/${Product_cap} ${INSTALL_OPTIONAL}
      PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
      COMPONENT asc-tools
    )
    install(CODE
      "execute_process(
          COMMAND ${CMAKE_COMMAND} -E create_symlink
          libcpudebug.so
          libtikcpp_debug.so
          WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/tools/cpudebug/lib64/${Product_cap}\"
      )"
      COMPONENT asc-tools
  )
  else()
    install(TARGETS cpudebug_${product_type}
      LIBRARY DESTINATION ${INSTALL_LIBRARY_DIR}/${product_type} ${INSTALL_OPTIONAL}
      COMPONENT asc-tools
    )
  endif()
endforeach()

if(BUILD_OPEN_PROJECT)
  set(_install_path tools/cpudebug/lib64)
else()
  set(_install_path ${INSTALL_LIBRARY_DIR})
endif()

install(FILES
  ${CMAKE_SOURCE_DIR}/libraries/lib/include/stub_fun.h
  include/intri_fun.h
  include/intri_fmt.h
  include/stub_def.h
  include/stub_reg.h
  include/kernel_fp16.h
  include/kernel_vectorized.h
  include/kernel_bf16.h
  include/kernel_fp8_e4m3.h
  include/kernel_fp8_e5m2.h
  include/kernel_fp8_e8m0.h
  include/kernel_fp8_e6m2.h
  include/kernel_fp4_e2m1.h
  include/kernel_fp4_e1m2.h
  include/kernel_hif4.h
  include/kernel_hif8.h
  include/kernel_print_lock.h
  include/kern_fwk.h
  include/tikicpulib.h
  include/kernel_raise_signal.h
  include/simt_stub.h
  include/kernel_simt_cpu.h
  include/cpu_debug_launch.h
  include/kernel_elf_parser.h
  DESTINATION tools/cpudebug/include ${INSTALL_OPTIONAL}
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
  COMPONENT asc-tools
)

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/tikicpulib-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/cpudebug-config.cmake
  INSTALL_DESTINATION ${INSTALL_LIBRARY_DIR}/cmake
)

if(BUILD_OPEN_PROJECT)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/cpudebug-config.cmake
  DESTINATION tools/cpudebug/cmake ${INSTALL_OPTIONAL}
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
  COMPONENT asc-tools
)

install(DIRECTORY
  ${CMAKE_SOURCE_DIR}/libraries/lib/cmake/
  DESTINATION tools/cpudebug/cmake ${INSTALL_OPTIONAL}
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
  COMPONENT asc-tools
)

install(CODE
  "execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
      cpudebug-config.cmake
      tikicpulib-config.cmake
      WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/tools/cpudebug/cmake/\"
  )"
  COMPONENT asc-tools
)

install(CODE
  "execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
      targets-cpudebug.cmake
      targets-tikicpulib.cmake
      WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/tools/cpudebug/cmake/\"
  )"
  COMPONENT asc-tools
)

install(CODE
  "execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
      targets-cpudebug-release.cmake
      targets-tikicpulib-release.cmake
      WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/tools/cpudebug/cmake/\"
  )"
  COMPONENT asc-tools
)

else()
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/cpudebug-config.cmake
  DESTINATION ${_install_path}/cmake ${INSTALL_OPTIONAL}
  COMPONENT asc-tools
)

install(DIRECTORY
  ${CMAKE_SOURCE_DIR}/libraries/lib/cmake/
  DESTINATION ${_install_path}/cmake ${INSTALL_OPTIONAL}
  COMPONENT asc-tools
)
endif()

install(FILES
  ${CMAKE_SOURCE_DIR}/libraries/lib/libcpudebug_cceprint.so
  ${CMAKE_SOURCE_DIR}/libraries/lib/libcpudebug_npuchk.so
  ${CMAKE_SOURCE_DIR}/libraries/lib/libcpudebug_stubreg.so
  DESTINATION ${_install_path} ${INSTALL_OPTIONAL}
  PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
  COMPONENT asc-tools
)

install(CODE
  "execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
      libcpudebug_cceprint.so
      libtikicpulib_cceprint.so
      WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/${_install_path}\"
  )"
  COMPONENT asc-tools
)

install(CODE
  "execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
      libcpudebug_npuchk.so
      libtikicpulib_npuchk.so
      WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/${_install_path}\"
  )"
  COMPONENT asc-tools
)

install(CODE
  "execute_process(
      COMMAND ${CMAKE_COMMAND} -E create_symlink
      libcpudebug_stubreg.so
      libtikicpulib_stubreg.so
      WORKING_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/${_install_path}\"
  )"
  COMPONENT asc-tools
)
