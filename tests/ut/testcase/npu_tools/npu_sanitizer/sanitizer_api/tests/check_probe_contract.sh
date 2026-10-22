#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../../.." && pwd)
test_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
api_dir="${repo_root}/npu_tools/npu_check/src/acl_san"
dbi_include_dir="${api_dir}/dbi"
dbi_source_dir="${api_dir}/dbi"

Fail()
{
    printf 'probe contract check failed: %s\n' "$1" >&2
    exit 1
}

[[ ! -e "${api_dir}/probe" ]] || Fail 'sanitizer_api/src/probe must not exist'
[[ ! -e "${api_dir}/../dbi" ]] || Fail 'top-level npu_sanitizer/dbi must not exist'
[[ ! -e "${api_dir}/dbi/msbit" ]] || Fail 'legacy msbit implementation directory must not remain'
[[ -f "${api_dir}/dbi/ctrlbin/ctrlbin_writer.cpp" ]] || Fail 'ctrlbin writer implementation is missing'
[[ -f "${api_dir}/dbi/ctrlbin/ctrlbin_bindings.h" ]] || Fail 'ctrlbin binding declarations are missing'
if rg -n -i '\bmsbit\b|src/dbi/msbit' "${api_dir}" "${api_dir}/CMakeLists.txt"; then
    Fail 'production DBI code must not retain MSBit naming'
fi
grep -Fq 'dbi/ctrlbin/ctrlbin_writer.cpp' "${api_dir}/CMakeLists.txt" || \
    Fail 'ctrlbin writer is not registered with the build'

required_files=(
    "${api_dir}/device_runtime/device_symbolizer.h"
    "${api_dir}/device_runtime/device_symbolizer.cpp"
    "${api_dir}/aclsan_trace_buffer.cpp"
    "${api_dir}/aclsan_trace_runtime.cpp"
    "${api_dir}/device_instr/soc_version.h"
    "${api_dir}/aclsan_active_probe_plan.h"
    "${api_dir}/aclsan_trace_buffer.h"
    "${api_dir}/aclsan_trace_runtime.h"
    "${dbi_include_dir}/trace_buffer_abi.h"
    "${dbi_include_dir}/dbi_pipeline.h"
    "${dbi_source_dir}/dbi_pipeline.cpp"
    "${dbi_source_dir}/binary_instrumenter.cpp"
    "${dbi_source_dir}/probe_source_generator.cpp"
)
for required_file in "${required_files[@]}"; do
    [[ -f "${required_file}" ]] || Fail "required file is missing: ${required_file}"
done

[[ ! -e "${api_dir}/device_instr/common/raw_data_struct.h" ]] || \
    Fail 'duplicate raw_data_struct.h must not exist'
if rg -n 'raw_data_struct\.h' \
    "${api_dir}/../../include" "${api_dir}" "${test_dir}" --glob '!check_probe_contract.sh'; then
    Fail 'sanitizer_api still references the removed raw trace header or type'
fi

legacy_namespace='sani''tizer'
legacy_trace_prefix='Asc''san'
if rg -n "\\bnamespace ${legacy_namespace}\\b|\\b${legacy_namespace}::" \
    "${api_dir}/../../include" "${api_dir}" "${test_dir}" --glob '!check_probe_contract.sh'; then
    Fail 'sanitizer_api must use the aclsan namespace'
fi
if rg -n "\\b${legacy_trace_prefix}[A-Za-z0-9_]*\\b" \
    "${api_dir}"; then
    Fail 'sanitizer_api and dbi must use Aclsan trace ABI type names'
fi

cmake_file="${api_dir}/CMakeLists.txt"
grep -Fq 'npu_check_dbi_engine' "${cmake_file}" || Fail 'acl_san does not link the DBI engine'
grep -Fq 'aclsan_trace_buffer.cpp' "${cmake_file}" || Fail 'trace buffer adapter is not built'
grep -Fq 'aclsan_trace_runtime.cpp' "${cmake_file}" || Fail 'trace runtime is not built'
grep -Fq 'device_runtime/device_symbolizer.cpp' "${cmake_file}" || Fail 'device symbolizer is not built'

soc_version_header="${api_dir}/device_instr/soc_version.h"
grep -Fq 'using SocVersionMappings = std::unordered_map<std::string, SocVersion>;' "${soc_version_header}" || \
    Fail 'SoC version mappings must use unordered_map<string, SocVersion>'
grep -Fq 'DAV_3510' "${soc_version_header}" || Fail 'DAV_3510 SoC version is missing'

trace_buffer="${api_dir}/aclsan_trace_buffer.cpp"
trace_buffer_abi="${dbi_include_dir}/trace_buffer_abi.h"
for trace_type in AclsanTraceBufferHeader AclsanTraceSliceHeader AclsanRawTraceRecord; do
    grep -Fq "struct ${trace_type}" "${trace_buffer_abi}" || Fail "trace buffer ABI does not define ${trace_type}"
done
grep -Fq 'ASCSAN_TRACE_BUFFER_MAGIC' "${trace_buffer_abi}" || \
    Fail 'trace buffer ABI does not define its magic'
grep -Fq 'uint64_t segmentBytes;' "${trace_buffer_abi}" || \
    Fail 'trace buffer ABI does not define launch segment boundaries'
grep -Fq 'using AclsanTraceSegmentHeader = AclsanTraceBufferHeader;' "${trace_buffer_abi}" || \
    Fail 'trace buffer ABI does not expose launch segment terminology'
if grep -Eq 'ASCSAN_TRACE_BUFFER_MAGIC_V[0-9]+' "${trace_buffer_abi}"; then
    Fail 'trace buffer ABI must expose only one unversioned magic'
fi
grep -Fq 'parsed.record.pc = wire.pc;' "${trace_buffer}" || Fail 'wire pc is not converted explicitly'
grep -Fq 'parsed.record.instrId = wire.instrId;' "${trace_buffer}" || Fail 'wire instrId is not converted explicitly'
grep -Fq 'std::memcpy(parsed.record.args, wire.args, sizeof(wire.args));' "${trace_buffer}" || \
    Fail 'wire arguments are not converted explicitly'
grep -Fq 'const TraceBlockKey blockKey{blockType, wire.blockId};' "${trace_buffer}" || \
    Fail 'instruction execution identity is not keyed by block type and logical block ID'
grep -Fq 'parsed.instrExecId = ++instructionCounts[blockKey];' "${trace_buffer}" || \
    Fail 'instruction execution ID is not counted per logical block'
if grep -Eq 'seenLogicalBlocks|sliceBlockId' "${trace_buffer}"; then
    Fail 'Host parser must not validate logical block placement'
fi
grep -Fq 'parsed.blockId = wire.blockId;' "${trace_buffer}" || Fail 'block ID is not read from the raw record'
grep -Fq 'parsed.blockType = blockType;' "${trace_buffer}" || Fail 'block type is not derived from the DBI slice'
grep -Fq 'parsed.phyCoreId = expectedPhyCoreId;' "${trace_buffer}" || \
    Fail 'physical core ID is not derived from the physical slice'
grep -Fq 'parsed.launchId = header.launchId;' "${trace_buffer}" || \
    Fail 'launch ID is not propagated from the validated trace buffer header'
grep -Fq 'header.segmentBytes != requiredBytes' "${trace_buffer}" || \
    Fail 'trace parser does not validate the launch segment boundary'
grep -Fq 'parsed.deviceId = deviceId;' "${trace_buffer}" || \
    Fail 'launch-owned device ID is not propagated to parsed records'
if rg -n 'ASCSAN_TRACE_SLICES_PER_BLOCK|TraceSliceCount' "${trace_buffer_abi}" "${trace_buffer}"; then
    Fail 'trace buffer layout must not size physical slices from the logical block count'
fi

trace_runtime="${api_dir}/aclsan_trace_runtime.cpp"
grep -Fq 'const char* socName = getSocName();' "${trace_runtime}" || \
    Fail 'trace runtime does not query the runtime SoC name'
grep -Fq 'aclsan::ResolveSocVersion(socName)' "${trace_runtime}" || \
    Fail 'trace runtime does not resolve the runtime SoC version'
grep -Fq 'aclsan::FindDeviceInstructionDecoder(*socVersion)' "${trace_runtime}" || \
    Fail 'trace runtime does not select the decoder by SoC version'
trace_translator="${api_dir}/aclsan_translate_device_data.cpp"
device_data_header="${api_dir}/aclsan_device_data.h"
grep -Fq 'using DeviceMemoryAccessDataList = std::vector<AclsanDeviceMemoryAccessData>;' "${device_data_header}" || \
    Fail 'device memory access translator result is not a variable-length vector'
grep -Fq 'std::get_if<DeviceMemoryAccessDataList>' "${trace_runtime}" || \
    Fail 'trace runtime does not consume the variable-length memory access list'
if rg -n '\bDeviceMemoryAccessDataArray\b' \
    "${api_dir}/../../include" "${api_dir}" "${test_dir}" --glob '!check_probe_contract.sh'; then
    Fail 'fixed-length DeviceMemoryAccessDataArray must not remain'
fi
for identity_field in launchId instrExecId deviceId phyCoreId blockId blockType; do
    grep -Fq "parsed.${identity_field}" "${trace_translator}" || \
        Fail "callback translation does not use ParsedTraceRecord::${identity_field}"
done
if rg -n '\bTraceCallbackContext\b' \
    "${api_dir}/../../include" "${api_dir}" "${test_dir}" --glob '!check_probe_contract.sh'; then
    Fail 'TraceCallbackContext must be merged into ParsedTraceRecord'
fi
grep -Fq 'std::get_if<SetPaddingParamField>' "${trace_runtime}" || \
    Fail 'trace runtime does not recognize decoded SET_PADDING state'
grep -Fq 'registerState.Update(key, *value);' "${trace_runtime}" || \
    Fail 'trace runtime does not update SET_PADDING state by block type and block ID'

binding_source="${dbi_source_dir}/dynamic_bind.cpp"

scalar_dev_id=64
for scalar_dev_op in st ld; do
    for scalar_dev_bits in 64 32 16 8; do
        scalar_dev_type="${scalar_dev_op^^}_DEV_B${scalar_dev_bits}"
        scalar_dev_symbol="__sanitizer_report_${scalar_dev_op}_dev_b${scalar_dev_bits}"
        grep -Fq "{InstrType::${scalar_dev_type}, ${scalar_dev_id}, \"${scalar_dev_symbol}\", {1, 2}}" \
            "${binding_source}" || Fail "incorrect address/offset binding for ${scalar_dev_symbol}"
        scalar_dev_id=$((scalar_dev_id + 1))
    done
done

scalar_atomic_id=56
for scalar_atomic_spec in 'ST_ATOMIC_B32:st_atomic_b32' 'ST_ATOMIC_B16:st_atomic_b16' 'ST_ATOMIC_B8:st_atomic_b8' \
    'STI_ATOMIC_B32:sti_atomic_b32' 'STI_ATOMIC_B16:sti_atomic_b16' 'STI_ATOMIC_B8:sti_atomic_b8'; do
    scalar_atomic_type="${scalar_atomic_spec%%:*}"
    scalar_atomic_symbol="__sanitizer_report_${scalar_atomic_spec##*:}"
    grep -Fq "{InstrType::${scalar_atomic_type}, ${scalar_atomic_id}, \"${scalar_atomic_symbol}\", {1, 2, 3}}" \
        "${binding_source}" || Fail "incorrect address/offset/post binding for ${scalar_atomic_symbol}"
    scalar_atomic_id=$((scalar_atomic_id + 1))
done

probe_generator="${dbi_source_dir}/probe_source_generator.cpp"
ctrlbin_bindings="${dbi_source_dir}/ctrlbin/ctrlbin_bindings.h"
binding_catalog=$(tr '\n\r\t' ' ' < "${binding_source}" | tr -s ' ')
probe_catalog=$(tr '\n\r\t' ' ' < "${probe_generator}" | tr -s ' ')
grep -Fq 'enum class InstrType' "${ctrlbin_bindings}" || Fail 'ctrlbin instruction type is not named InstrType'
if rg -n '\bCtrlbinInstructionType\b' "${binding_source}" "${ctrlbin_bindings}"; then
    Fail 'CtrlbinInstructionType alias remains in the binding path'
fi
grep -Fq '{InstrType::SET_PADDING, 392, "__sanitizer_report_set_padding", {0}}' \
    <<< "${binding_catalog}" || \
    Fail 'DBI does not bind SET_PADDING argument 0 to instruction ID 392'
grep -Fq 'return ProbeGroup::Matrix;' "${binding_source}" || Fail 'PIPE_M instruction IDs are not assigned to the Matrix group'
grep -Fq 'return ProbeGroup::Vector;' "${binding_source}" || Fail 'PIPE_V instruction IDs are not assigned to the Vector group'
grep -Fq '{392, ProbeGroup::Scalar, "__sanitizer_report_set_padding"' "${probe_generator}" || \
    Fail 'Scalar SET_PADDING ProbeDefinition is missing'
grep -Fq '"RegisterState", "PIPE_S"' <<< "${probe_catalog}" || \
    Fail 'SET_PADDING ProbeDefinition does not record PIPE_S'
grep -Fq 'R"ARGS(value, 0UL, 0UL, 0UL, 0UL)ARGS"' <<< "${probe_catalog}" || \
    Fail 'SET_PADDING ProbeDefinition does not preserve value in raw argument 0'
grep -Fq '{149, ProbeGroup::Mte2, "__sanitizer_report_set_l1_2d_b16"' "${probe_generator}" || \
    Fail 'MTE2 SET_L1_2D.b16 ProbeDefinition is missing'
grep -Fq '{150, ProbeGroup::Mte2, "__sanitizer_report_set_l1_2d_b32"' "${probe_generator}" || \
    Fail 'MTE2 SET_L1_2D.b32 ProbeDefinition is missing'
required_scalar_probes=(
    'LOOP3_PARA|90|__sanitizer_report_set_loop3_para'
    'LOOP_SIZE_UBTOOUT|125|__sanitizer_report_set_loop_size_ubtoout'
    'LOOP1_STRIDE_UBTOOUT|126|__sanitizer_report_set_loop1_stride_ubtoout'
    'LOOP2_STRIDE_UBTOOUT|127|__sanitizer_report_set_loop2_stride_ubtoout'
    'LOOP_SIZE_OUTTOUB|128|__sanitizer_report_set_loop_size_outtoub'
    'LOOP1_STRIDE_OUTTOUB|129|__sanitizer_report_set_loop1_stride_outtoub'
    'LOOP2_STRIDE_OUTTOUB|130|__sanitizer_report_set_loop2_stride_outtoub'
    'PAD_CNT_NDDMA|131|__sanitizer_report_set_pad_cnt_nddma'
    'SET_LOOP_SIZE_OUTTOL1|394|__sanitizer_report_set_loop_size_outtol1'
    'SET_LOOP1_STRIDE_OUTTOL1|395|__sanitizer_report_set_loop1_stride_outtol1'
    'SET_LOOP2_STRIDE_OUTTOL1|396|__sanitizer_report_set_loop2_stride_outtol1'
)
for specification in "${required_scalar_probes[@]}"; do
    IFS='|' read -r instruction_type instruction_id probe_symbol <<< "${specification}"
    grep -Fq "{InstrType::${instruction_type}, ${instruction_id}, \"${probe_symbol}\", {0}}" \
        <<< "${binding_catalog}" || \
        Fail "DBI binding is missing for instruction ID ${instruction_id}"
    grep -Fq "{${instruction_id}, ProbeGroup::Scalar, \"${probe_symbol}\"" "${probe_generator}" || \
        Fail "Scalar ProbeDefinition is missing for instruction ID ${instruction_id}"
done
grep -Fq '{InstrType::MAD_S8, 400, "__sanitizer_report_mad_s8", {0, 1, 2, 3}}' \
    <<< "${binding_catalog}" || \
    Fail 'PIPE_M MAD binding is missing or has the wrong argument mask'
grep -Fq '{InstrType::SCATTER_VNCHWCONV_B8, 177, "__sanitizer_report_scatter_vnchwconv_b8", {0, 1, 2, 3, 4}}' \
    <<< "${binding_catalog}" || Fail 'PIPE_V VNCHWCONV binding is missing or has the wrong argument mask'
grep -Fq '{InstrType::ST_ATOMIC_B8, 58, "__sanitizer_report_st_atomic_b8", {1, 2, 3}}' \
    <<< "${binding_catalog}" || Fail 'Scalar ST_ATOMIC binding is missing or has the wrong argument mask'
grep -Fq '{InstrType::PIPE_BARRIER, 439, "__sanitizer_report_pipe_barrier", {0}}' \
    <<< "${binding_catalog}" || \
    Fail 'Sync PIPE_BARRIER binding is missing or has the wrong argument mask'

hook_source="${api_dir}/aclsan_hook_aclrt.cpp"
grep -Fq 'InstrumentRuntimeBinary' "${hook_source}" || \
    Fail 'binary-load hook does not use the DBI engine'
grep -Fq 'SnapshotActiveProbePlan()' "${hook_source}" || Fail 'binary-load hook does not snapshot the active DBI plan'
grep -Fq 'PrepareTraceLaunch(' "${hook_source}" || Fail 'launch hook does not prepare a DBI trace buffer'
grep -Fq 'CompleteTraceLaunch(' "${hook_source}" || Fail 'launch hook does not retain DBI trace ownership'
grep -Fq 'CollectTraceStream(' "${hook_source}" || Fail 'synchronize hook does not collect DBI records'

if rg -n \
    'ACLSAN_PROBE_|ACLSAN_BUILD_DEVICE_PROBE_RESOURCES|sanitizer_api/src/probe|src/probe/|ProbeRuntime|ProbeParseResult|DispatchProbeRecords' \
    "${api_dir}/CMakeLists.txt" "${api_dir}/../../include" "${api_dir}" "${test_dir}" \
    --glob '!check_probe_contract.sh' --glob '!aclsan_binary_load_dbi_hook.cpp'; then
    Fail 'legacy sanitizer_api probe implementation is still referenced'
fi

printf 'probe contract check passed: DBI is integrated under sanitizer_api and has no legacy src/probe\n'
