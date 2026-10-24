# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16.0)
project(aclsan_sanitizer_api)

# TODO: 需要洲杰把这段逻辑从cmake移除
set(NPU_CHECK_EMBED_PROBE_RESOURCES_SCRIPT_CONTENT [=[# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.

cmake_policy(SET CMP0054 NEW)

foreach(required IN ITEMS
    TRACE_RECORD TRACE_BUFFER_ABI CTRLBIN_BINDING_SOURCE CTRLBIN_PUBLIC_HEADER CTRLBIN_WRITER_SOURCE
    CTRLBIN_BINDINGS_HEADER CTRLBIN_FILE_IO_HEADER CTRLBIN_SERIALIZE_HEADER CTRLBIN_SINGLETON_HEADER
    CTRLBIN_STRING_UTILS_HEADER OUTPUT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
  if(NOT "${required}" STREQUAL "OUTPUT" AND NOT EXISTS "${${required}}")
    message(FATAL_ERROR "missing Probe resource: ${${required}}")
  endif()
endforeach()

file(READ "${TRACE_RECORD}" trace_record_hex HEX)
file(READ "${TRACE_BUFFER_ABI}" trace_buffer_abi_hex HEX)
file(SHA256 "${TRACE_RECORD}" trace_record_sha)
file(SHA256 "${TRACE_BUFFER_ABI}" trace_buffer_abi_sha)
string(SHA256 resource_identity "trace_record.h:${trace_record_sha}\ntrace_buffer_abi.h:${trace_buffer_abi_sha}\n")
set(ctrlbin_identity_input "")
foreach(ctrlbin_input IN ITEMS
    CTRLBIN_BINDING_SOURCE CTRLBIN_PUBLIC_HEADER CTRLBIN_WRITER_SOURCE CTRLBIN_BINDINGS_HEADER
    CTRLBIN_FILE_IO_HEADER CTRLBIN_SERIALIZE_HEADER CTRLBIN_SINGLETON_HEADER CTRLBIN_STRING_UTILS_HEADER)
  file(SHA256 "${${ctrlbin_input}}" ctrlbin_input_sha)
  get_filename_component(ctrlbin_input_name "${${ctrlbin_input}}" NAME)
  string(APPEND ctrlbin_identity_input "${ctrlbin_input_name}:${ctrlbin_input_sha}\n")
endforeach()
string(SHA256 ctrlbin_implementation_identity "${ctrlbin_identity_input}")

string(LENGTH "${trace_record_hex}" trace_record_hex_length)
string(LENGTH "${trace_buffer_abi_hex}" trace_buffer_abi_hex_length)
math(EXPR trace_record_size "${trace_record_hex_length} / 2")
math(EXPR trace_buffer_abi_size "${trace_buffer_abi_hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," trace_record_bytes "${trace_record_hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," trace_buffer_abi_bytes "${trace_buffer_abi_hex}")

set(content "// Generated file. Do not edit.\n")
string(APPEND content "#include \"dbi/embedded_probe_resources.h\"\n\n")
string(APPEND content "#include <cstddef>\n\n")
string(APPEND content "namespace aclsan {\nnamespace {\n")
string(APPEND content "const unsigned char TRACE_RECORD[] = {${trace_record_bytes}};\n")
string(APPEND content "const unsigned char TRACE_BUFFER_ABI[] = {${trace_buffer_abi_bytes}};\n")
string(APPEND content "constexpr std::size_t TRACE_RECORD_SIZE = ${trace_record_size}U;\n")
string(APPEND content "constexpr std::size_t TRACE_BUFFER_ABI_SIZE = ${trace_buffer_abi_size}U;\n")
string(APPEND content "constexpr std::string_view RESOURCE_IDENTITY = \"${resource_identity}\";\n")
string(APPEND content
  "constexpr std::string_view CTRL_BIN_IMPLEMENTATION_IDENTITY = \"${ctrlbin_implementation_identity}\";\n")
string(APPEND content "} // namespace\n\n")
string(APPEND content "std::string_view EmbeddedTraceRecordHeader()\n{\n")
string(APPEND content "    return {reinterpret_cast<const char*>(TRACE_RECORD), TRACE_RECORD_SIZE};\n}\n\n")
string(APPEND content "std::string_view EmbeddedTraceBufferAbiHeader()\n{\n")
string(APPEND content "    return {reinterpret_cast<const char*>(TRACE_BUFFER_ABI), TRACE_BUFFER_ABI_SIZE};\n}\n\n")
string(APPEND content "std::string_view EmbeddedProbeResourceIdentity() { return RESOURCE_IDENTITY; }\n\n")
string(APPEND content
  "std::string_view EmbeddedCtrlBinImplementationIdentity() { return CTRL_BIN_IMPLEMENTATION_IDENTITY; }\n\n")
string(APPEND content "} // namespace aclsan\n")

file(WRITE "${OUTPUT}" "${content}")
]=])
set(NPU_CHECK_EMBED_PROBE_RESOURCES_SCRIPT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embed_probe_resources.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GENERATE
  OUTPUT "${NPU_CHECK_EMBED_PROBE_RESOURCES_SCRIPT}"
  CONTENT "${NPU_CHECK_EMBED_PROBE_RESOURCES_SCRIPT_CONTENT}")


find_package(Threads REQUIRED)

add_library(npu_check_dbi_probe_group STATIC dbi/probe_group.cpp)
target_compile_features(npu_check_dbi_probe_group PUBLIC cxx_std_17)
target_compile_definitions(npu_check_dbi_probe_group PUBLIC
  _GLIBCXX_USE_CXX11_ABI=0
)
target_include_directories(npu_check_dbi_probe_group PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(npu_check_dbi_probe_group PRIVATE -Wall -Wextra -Werror)
set_target_properties(npu_check_dbi_probe_group PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

# TODO: 需要洲杰把这段逻辑从cmake移除
set(NPU_CHECK_PROBE_RESOURCE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_probe_resources.cpp")
add_custom_command(
  OUTPUT "${NPU_CHECK_PROBE_RESOURCE_SOURCE}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${CMAKE_COMMAND}"
    "-DTRACE_RECORD=${CMAKE_CURRENT_SOURCE_DIR}/dbi/trace_record.h"
    "-DTRACE_BUFFER_ABI=${CMAKE_CURRENT_SOURCE_DIR}/dbi/trace_buffer_abi.h"
    "-DCTRLBIN_BINDING_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dbi/dynamic_bind.cpp"
    "-DCTRLBIN_PUBLIC_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin_generator.h"
    "-DCTRLBIN_WRITER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_writer.cpp"
    "-DCTRLBIN_BINDINGS_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_bindings.h"
    "-DCTRLBIN_FILE_IO_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_file_io.h"
    "-DCTRLBIN_SERIALIZE_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_serialize.h"
    "-DCTRLBIN_SINGLETON_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_singleton.h"
    "-DCTRLBIN_STRING_UTILS_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_string_utils.h"
    "-DOUTPUT=${NPU_CHECK_PROBE_RESOURCE_SOURCE}"
    -P "${NPU_CHECK_EMBED_PROBE_RESOURCES_SCRIPT}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/trace_record.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/trace_buffer_abi.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/dynamic_bind.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin_generator.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_writer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_bindings.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_file_io.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_serialize.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_singleton.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin/ctrlbin_string_utils.h"
    "${NPU_CHECK_EMBED_PROBE_RESOURCES_SCRIPT}"
  VERBATIM)

add_library(npu_check_dbi_probe_runtime STATIC
  dbi/probe_source_generator.cpp
  "${NPU_CHECK_PROBE_RESOURCE_SOURCE}"
)
target_compile_features(npu_check_dbi_probe_runtime PUBLIC cxx_std_17)
target_include_directories(npu_check_dbi_probe_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(npu_check_dbi_probe_runtime PRIVATE -Wall -Wextra -Werror)
target_link_libraries(npu_check_dbi_probe_runtime PUBLIC npu_check_dbi_probe_group)
set_target_properties(npu_check_dbi_probe_runtime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

add_library(npu_check_dbi_ctrlbin STATIC
  dbi/dynamic_bind.cpp
  dbi/ctrlbin/ctrlbin_writer.cpp
)
target_compile_features(npu_check_dbi_ctrlbin PUBLIC cxx_std_17)
target_include_directories(npu_check_dbi_ctrlbin
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dbi/ctrlbin
)
target_compile_options(npu_check_dbi_ctrlbin PRIVATE -Wall -Wextra -Werror)
target_link_libraries(npu_check_dbi_ctrlbin PUBLIC npu_check_dbi_probe_group)
set_source_files_properties(dbi/ctrlbin/ctrlbin_writer.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
set_target_properties(npu_check_dbi_ctrlbin PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

add_library(npu_check_dbi_engine STATIC
  dbi/binary_instrumenter.cpp
  dbi/dbi_pipeline.cpp
  dbi/kernel_param_metadata.cpp
  dbi/tool_runner.cpp
)
target_compile_features(npu_check_dbi_engine PUBLIC cxx_std_17)
target_include_directories(npu_check_dbi_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(npu_check_dbi_engine PRIVATE -Wall -Wextra -Werror)
target_link_libraries(npu_check_dbi_engine PRIVATE c_sec_static)
target_link_libraries(npu_check_dbi_engine
  PUBLIC npu_check_dbi_ctrlbin
  PRIVATE npu_check_dbi_probe_runtime npu_sanitizer_plog ${CMAKE_DL_LIBS} Threads::Threads Boost::filesystem
)
set_target_properties(npu_check_dbi_engine PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

# libacl_san.so 依赖 libacl_tool_injection.so提供的hook能力
if(NOT TARGET acl_tool_injection)
  message(FATAL_ERROR "sanitizer_api requires Runtime injection target: acl_tool_injection")
endif()

add_library(acl_san SHARED
  device_runtime/device_binary_registry.cpp
  device_runtime/device_symbolizer.cpp
  device_instr/decoder_registry.cpp
  device_instr/arch/dav_3510/decoder.cpp
  device_instr/arch/dav_3510/register_state_manager.cpp
  aclsan_active_probe_plan.cpp
  aclsan_memory_cbdata.cpp
  aclsan_register_dependency.cpp
  aclsan_trace_buffer.cpp
  aclsan_trace_runtime.cpp
  aclsan_api.cpp
  aclsan_dispatch.cpp
  aclsan_hook_aclrt.cpp
  aclsan_device_data_log.cpp
  aclsan_translate_device_data.cpp
)

target_compile_features(acl_san PUBLIC cxx_std_17)
target_include_directories(acl_san
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(acl_san PRIVATE c_sec_static)
target_link_libraries(acl_san
  PRIVATE
    acl_tool_injection
    npu_check_dbi_engine
    npu_sanitizer_plog
    dl
    pthread
    Boost::filesystem
)
target_link_options(acl_san PRIVATE "LINKER:--exclude-libs,ALL")

set(ACLSAN_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/acl_san.map")
target_link_options(acl_san PRIVATE
  "LINKER:--version-script=${ACLSAN_VERSION_SCRIPT}"
)
set_property(TARGET acl_san APPEND PROPERTY
  LINK_DEPENDS "${ACLSAN_VERSION_SCRIPT}"
)

set_target_properties(acl_san PROPERTIES
  OUTPUT_NAME acl_san
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)
