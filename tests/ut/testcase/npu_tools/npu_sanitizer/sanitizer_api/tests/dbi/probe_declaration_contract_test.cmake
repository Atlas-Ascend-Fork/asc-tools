foreach(required IN ITEMS DBI_ROOT PROBE_ARCH)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(probe_sources
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/fixpipe.cpp"
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/matrix.cpp"
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/mte1.cpp"
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/mte2.cpp"
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/mte3.cpp"
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/scalar.cpp"
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/sync.cpp"
  "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/vector.cpp"
)

set(probe_content "")
foreach(probe_source IN LISTS probe_sources)
  if(NOT EXISTS "${probe_source}")
    message(FATAL_ERROR "missing probe source: ${probe_source}")
  endif()
  file(READ "${probe_source}" source_content)
  string(APPEND probe_content "\n${source_content}")
endforeach()
file(READ "${DBI_ROOT}/dbi/trace_record.h" trace_record_content)
file(READ "${DBI_ROOT}/dbi/trace_buffer_abi.h" trace_buffer_abi_content)
file(READ "${DBI_ROOT}/dbi/probes/${PROBE_ARCH}/sync.cpp" sync_probe_content)

foreach(trace_type IN ITEMS AclsanTraceBufferHeader AclsanTraceSliceHeader AclsanRawTraceRecord)
  string(FIND "${trace_buffer_abi_content}" "struct ${trace_type}" type_offset)
  if(type_offset EQUAL -1)
    message(FATAL_ERROR "trace_buffer_abi.h must define ${trace_type}")
  endif()
endforeach()
string(CONCAT legacy_trace_prefix "Asc" "san")
if(trace_buffer_abi_content MATCHES "${legacy_trace_prefix}[A-Za-z0-9_]*" OR
    trace_record_content MATCHES "${legacy_trace_prefix}[A-Za-z0-9_]*")
  message(FATAL_ERROR "DBI trace ABI must not expose legacy-prefixed type names")
endif()

foreach(required_abi_token IN ITEMS
    "ASCSAN_TRACE_BUFFER_MAGIC"
    "ASCSAN_PHYSICAL_CORE_PART_COUNT"
    "ASCSAN_AIC_CORE_RATIO_DENOMINATOR"
    "ASCSAN_PHYSICAL_CORE_TOPOLOGY_UNIT"
    "physicalCoreCount"
    "segmentBytes")
  string(FIND "${trace_buffer_abi_content}" "${required_abi_token}" token_offset)
  if(token_offset EQUAL -1)
    message(FATAL_ERROR "trace_buffer_abi.h must contain ${required_abi_token}")
  endif()
endforeach()

foreach(required_record_token IN ITEMS
    "AscendC::GetBlockIdx()"
    "get_coreid()"
    "record->category = category"
    "record->blockId = blockId"
    "static_cast<uint64_t>(phyCoreId) * sliceBytes")
  string(FIND "${trace_record_content}" "${required_record_token}" token_offset)
  if(token_offset EQUAL -1)
    message(FATAL_ERROR "trace_record.h must contain ${required_record_token}")
  endif()
endforeach()
string(REGEX MATCHALL "DeviceInstructionCategory::MemoryAccess" memory_access_categories "${probe_content}")
list(LENGTH memory_access_categories memory_access_category_count)
if(NOT memory_access_category_count EQUAL 93)
  message(FATAL_ERROR "expected 93 memory-access probe categories, found ${memory_access_category_count}")
endif()
string(REGEX MATCHALL "DeviceInstructionCategory::Synchronization" synchronization_categories "${probe_content}")
list(LENGTH synchronization_categories synchronization_category_count)
if(NOT synchronization_category_count EQUAL 29)
  message(FATAL_ERROR "expected 29 synchronization probe categories, found ${synchronization_category_count}")
endif()
string(REGEX MATCHALL "DeviceInstructionCategory::RegisterState" register_state_categories "${probe_content}")
list(LENGTH register_state_categories register_state_category_count)
if(NOT register_state_category_count EQUAL 33)
  message(FATAL_ERROR "expected 33 register-state probe categories, found ${register_state_category_count}")
endif()
foreach(expected_pipeline IN ITEMS PIPE_S PIPE_M PIPE_V PIPE_MTE1 PIPE_MTE2 PIPE_MTE3 PIPE_FIX)
  if(NOT probe_content MATCHES "static_cast<uint16_t>\\(${expected_pipeline}\\)")
    message(FATAL_ERROR "probe sources must record ${expected_pipeline}")
  endif()
endforeach()
if(trace_record_content MATCHES "PIPELINE_SET_WAIT_FLAG|PIPELINE_GET_RLS_BUF|PIPELINE_MTE[123]|PIPELINE_FIXPIPE")
  message(FATAL_ERROR "trace_record.h must not mix instruction categories with execution pipelines")
endif()
if(trace_record_content MATCHES "get_subblockdim\\(\\)|get_subblockid\\(\\)")
  message(FATAL_ERROR "trace_record.h must derive blockId directly from AscendC::GetBlockIdx()")
endif()
if(trace_record_content MATCHES "ASCSAN_TRACE_BUFFER_MAGIC_V[0-9]+")
  message(FATAL_ERROR "trace_record.h must use the single unversioned trace buffer magic")
endif()
if(trace_buffer_abi_content MATCHES "ASCSAN_TRACE_SLICES_PER_BLOCK|TraceSliceCount|recordsPerBlock" OR
    trace_record_content MATCHES "ASCSAN_TRACE_SLICES_PER_BLOCK|TraceSliceCount|recordsPerBlock")
  message(FATAL_ERROR "DBI trace ABI must use fixed physical-core slices")
endif()

if(probe_content MATCHES "ASCSAN_PROBE" OR trace_record_content MATCHES "ASCSAN_PROBE")
  message(FATAL_ERROR "ASCSAN_PROBE must not appear in the DBI probe header or sources")
endif()

string(REGEX MATCHALL
  "__sanitizer_report_[A-Za-z0-9_]+[ \t\r\n]*\\("
  probe_definitions "${probe_content}")
list(LENGTH probe_definitions probe_count)
if(NOT probe_count EQUAL 155)
  message(FATAL_ERROR "expected 155 probe definitions, found ${probe_count}")
endif()

string(REGEX MATCHALL "__sanitizer_report_[A-Za-z0-9_]+" probe_symbols "${probe_content}")
list(REMOVE_DUPLICATES probe_symbols)
list(SORT probe_symbols)
file(READ "${DBI_ROOT}/dbi/dynamic_bind.cpp" binding_content)
string(REGEX MATCHALL "__sanitizer_report_[A-Za-z0-9_]+" binding_symbols "${binding_content}")
list(REMOVE_DUPLICATES binding_symbols)
list(SORT binding_symbols)
if(NOT probe_symbols STREQUAL binding_symbols)
  message(FATAL_ERROR "Probe definitions and ctrl.bin binding symbols differ")
endif()

string(CONCAT explicit_declaration
  "extern[ \t\r\n]+__attribute__\\(\\(noinline\\)\\)[ \t\r\n]+"
  "__attribute__\\(\\(weak\\)\\)[ \t\r\n]+__aicore__[ \t\r\n]+void[ \t\r\n]+"
  "__sanitizer_report_[A-Za-z0-9_]+[ \t\r\n]*\\(")
string(REGEX MATCHALL "${explicit_declaration}" explicit_probes "${probe_content}")
list(LENGTH explicit_probes explicit_probe_count)
if(NOT explicit_probe_count EQUAL 155)
  message(FATAL_ERROR "expected 155 explicit probe declarations, found ${explicit_probe_count}")
endif()

# Vector synchronization instructions omit PIPE_V from their explicit operands.
string(REGEX MATCHALL
  "static_cast<uint64_t>\\(PIPE_V\\)"
  vector_sync_pipe_arguments "${sync_probe_content}")
list(LENGTH vector_sync_pipe_arguments vector_sync_pipe_argument_count)
if(NOT vector_sync_pipe_argument_count EQUAL 8)
  message(FATAL_ERROR
    "expected 8 implicit PIPE_V arguments in sync probes, found ${vector_sync_pipe_argument_count}")
endif()
