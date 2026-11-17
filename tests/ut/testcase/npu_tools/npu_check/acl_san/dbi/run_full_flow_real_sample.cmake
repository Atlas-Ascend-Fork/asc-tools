# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").

foreach(required IN ITEMS SAMPLE KERNEL DEVICE_ID ARCH TOOLCHAIN_ROOT TEST_ROOT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()
if(NOT EXISTS "${KERNEL}")
  message(FATAL_ERROR "real full-flow kernel does not exist: ${KERNEL}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/work" "${TEST_ROOT}/cache")
if(NOT DEFINED MODE)
  set(MODE multi)
endif()
set(launch_api host-args)
if(ARGS_ARRAY)
  set(launch_api args-array)
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "${SAMPLE}" "${KERNEL}" "${MODE}" "${DEVICE_ID}" "${launch_api}"
  RESULT_VARIABLE sample_result
  OUTPUT_VARIABLE sample_stdout
  ERROR_VARIABLE sample_stderr
)
set(sample_output "${sample_stdout}${sample_stderr}")
file(WRITE "${TEST_ROOT}/sample.log" "${sample_output}")
if(NOT sample_result EQUAL 0)
  message(FATAL_ERROR "real full-flow sample failed (${sample_result}):\n${sample_output}")
endif()

set(required_output
  "[intercept] binary_load"
  "[dbi] patched=yes backend=real"
  "[hook] function instrumented=yes"
  "[hook] launch trace_buffer_injected=yes"
  "[device] records="
  "[verify] kernel_result=pass trace_records=pass resources=balanced"
  "FULL_FLOW_SAMPLE_PASS"
)
if(MODE STREQUAL "zero")
  list(APPEND required_output "[d2h] copies=0")
else()
  list(APPEND required_output "[d2h] copies=1")
endif()
if(MODE STREQUAL "multi")
  list(APPEND required_output "[callback] records=32 launches=3")
else()
  list(APPEND required_output "[callback] records=8 launches=1")
endif()
if(MODE STREQUAL "zero")
  list(APPEND required_output
    "[metadata] kernel=ZeroArgumentKernel count=1 result=0"
    "[metadata] kernel=ZeroArgumentKernel index=0 offset=8 size=8 result=0")
else()
  list(APPEND required_output
    "[metadata] kernel=ZeroArgumentKernel count=1 result=0"
    "[metadata] kernel=ZeroArgumentKernel index=0 offset=24 size=8 result=0"
    "[metadata] kernel=OneArgumentKernel count=2 result=0"
    "[metadata] kernel=OneArgumentKernel index=1 offset=24 size=8 result=0"
    "[metadata] kernel=FullFlowKernel count=4 result=0"
    "[metadata] kernel=FullFlowKernel index=3 offset=24 size=8 result=0")
endif()
foreach(fragment IN LISTS required_output)
  string(FIND "${sample_output}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "missing real sample output fragment '${fragment}':\n${sample_output}")
  endif()
endforeach()
string(REGEX MATCH "\\[callback\\] records=[1-9][0-9]*" callback_count "${sample_output}")
if(callback_count STREQUAL "")
  message(FATAL_ERROR "real sample did not report a positive callback count:\n${sample_output}")
endif()

message(STATUS "real NPU full-flow output verified")
