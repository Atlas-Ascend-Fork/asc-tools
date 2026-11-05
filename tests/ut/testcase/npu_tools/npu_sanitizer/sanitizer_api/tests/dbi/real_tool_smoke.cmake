cmake_policy(SET CMP0054 NEW)

if(NOT EXISTS "${INPUT_KERNEL}")
  message(FATAL_ERROR "real DBI smoke input does not exist: ${INPUT_KERNEL}")
endif()
if(NOT DEFINED EXPECTED_OFFSET OR "${EXPECTED_OFFSET}" STREQUAL "")
  message(FATAL_ERROR "EXPECTED_OFFSET is required")
endif()

file(REMOVE_RECURSE "${WORK_DIRECTORY}" "${CACHE_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}" "${CACHE_DIRECTORY}")
set(collection_args)
if(UB_SCALAR_COLLECTION)
  list(APPEND collection_args ub-scalar)
endif()
execute_process(
  COMMAND "${SMOKE_EXECUTABLE}"
    "${INPUT_KERNEL}"
    "${OUTPUT_KERNEL}"
    "${ARCH}"
    "${TOOLCHAIN_ROOT}"
    "${WORK_DIRECTORY}"
    "${CACHE_DIRECTORY}"
    ${collection_args}
  RESULT_VARIABLE smoke_status
  OUTPUT_VARIABLE smoke_output
  ERROR_VARIABLE smoke_error
)
if(NOT smoke_status EQUAL 0)
  message(FATAL_ERROR "real DBI smoke failed (${smoke_status}):\n${smoke_output}${smoke_error}")
endif()
string(FIND "${smoke_output}" "trace_argument_offset=${EXPECTED_OFFSET}" offset_position)
if(offset_position EQUAL -1)
  message(FATAL_ERROR "real DBI smoke reported an unexpected trace argument offset:\n${smoke_output}")
endif()

if(NOT EXISTS "${OUTPUT_KERNEL}")
  message(FATAL_ERROR "real DBI smoke did not create ${OUTPUT_KERNEL}")
endif()
file(SIZE "${OUTPUT_KERNEL}" output_size)
if(output_size EQUAL 0)
  message(FATAL_ERROR "real DBI smoke created an empty output")
endif()
file(GLOB probe_objects "${CACHE_DIRECTORY}/aggregates/*/probe.o")
file(GLOB control_files "${CACHE_DIRECTORY}/aggregates/*/ctrl.bin")
list(LENGTH probe_objects probe_count)
list(LENGTH control_files control_count)
if(NOT probe_count EQUAL 1 OR NOT control_count EQUAL 1)
  message(FATAL_ERROR "real DBI smoke did not publish exactly one probe.o and ctrl.bin")
endif()
file(SIZE "${probe_objects}" probe_size)
file(SIZE "${control_files}" control_size)
if(probe_size EQUAL 0 OR control_size EQUAL 0)
  message(FATAL_ERROR "real DBI smoke published an empty probe.o or ctrl.bin")
endif()
message(STATUS "real DBI smoke output: ${OUTPUT_KERNEL} (${output_size} bytes)")

if(UB_SCALAR_COLLECTION)
  # dav-3510 calls and probe trampolines materialize a PC-relative ADRP/ADDI target.
  function(adrp_target code reg output)
    string(REGEX MATCH
      "([0-9a-f]+):[^\n]*ADRP[ \t]+${reg}, #(-?[0-9]+)[^\n]*\n[^\n]*ADDI[ \t]+${reg}, ${reg}, #(-?[0-9]+)"
      address "${code}")
    if(NOT address)
      message(FATAL_ERROR "cannot resolve dav-3510 target: ${code}")
    endif()
    math(EXPR target "(0x${CMAKE_MATCH_1} & ~4095) + ${CMAKE_MATCH_2} + ${CMAKE_MATCH_3}")
    set(${output} "${target}" PARENT_SCOPE)
  endfunction()

  set(objdump "${TOOLCHAIN_ROOT}/tools/bisheng_compiler/bin/llvm-objdump")
  foreach(kernel IN ITEMS INPUT_KERNEL OUTPUT_KERNEL)
    execute_process(COMMAND "${objdump}" -d "${${kernel}}"
      RESULT_VARIABLE disassembly_status OUTPUT_VARIABLE disassembly ERROR_VARIABLE disassembly_error)
    if(NOT disassembly_status EQUAL 0)
      message(FATAL_ERROR "cannot disassemble ${${kernel}}: ${disassembly_error}")
    endif()
    file(WRITE "${${kernel}}.disasm" "${disassembly}")
    if("${kernel}" STREQUAL "INPUT_KERNEL")
      foreach(instruction IN ITEMS LD.b8 LD_IO.b16 LD_IO.b32 LD_IO.b64
          ST.b8 ST_IO.b16 ST_IO.b32 ST_IO.b64)
        string(FIND "${disassembly}" "${instruction}" position)
        if(position EQUAL -1)
          message(FATAL_ERROR "UB scalar fixture did not generate ${instruction}")
        endif()
      endforeach()
    else()
      # Probe definitions are linked even without instrumentation. Inspect calls in the
      # rewritten vector kernel and resolve their targets against the named trampolines.
      string(FIND "${disassembly}" "Disassembly of section .text:" text_position)
      if(text_position EQUAL -1)
        message(FATAL_ERROR "UB scalar output has no rewritten .text section")
      endif()
      string(SUBSTRING "${disassembly}" ${text_position} -1 rewritten)
      string(REGEX MATCH "<UbScalarCollectionKernel_mix_aiv[^>]*>:\n([^\n]+\n)+" kernel_body "${rewritten}")
      string(REGEX MATCHALL
        "[0-9a-f]+:[^\n]*ADRP[ \t]+X28,[^\n]*\n[^\n]*ADDI[^\n]*\n[^\n]*SHRI[^\n]*\n[^\n]*CALLR[ \t]+X28"
        calls "${kernel_body}")
      set(call_targets)
      foreach(call IN LISTS calls)
        adrp_target("${call}" X28 target)
        list(APPEND call_targets "${target}")
      endforeach()
      foreach(probe IN ITEMS ld_b8_reg ld_b16_imm ld_b32_imm ld_b64_imm
          st_b8_reg st_b16_imm st_b32_imm st_b64_imm)
        string(REGEX MATCH
          "__sanitizer_report_${probe}[^>]*[.]vector[.]org[.]0>:\n[^\n]*\n[^\n]*"
          trampoline "${disassembly}")
        adrp_target("${trampoline}" X0 target)
        list(FIND call_targets "${target}" call_position)
        if(call_position EQUAL -1)
          message(FATAL_ERROR "bisheng-tune did not insert a call to __sanitizer_report_${probe}")
        endif()
      endforeach()
    endif()
  endforeach()
endif()
