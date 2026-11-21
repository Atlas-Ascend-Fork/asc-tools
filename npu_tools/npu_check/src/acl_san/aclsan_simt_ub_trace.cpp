/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "aclsan_simt_ub_trace.h"
#include "acl_san/aclsan_api.h"
#include "dbi/ctrlbin/ctrlbin_bindings.h"
#include <limits>

namespace aclsan {
namespace {
uint32_t WriteBytes(InstrType instruction)
{
    constexpr uint32_t byteStoreBytes = sizeof(uint8_t);
    constexpr uint32_t halfwordStoreBytes = sizeof(uint16_t);
    constexpr uint32_t wordStoreBytes = sizeof(uint32_t);
    constexpr uint32_t doublewordStoreBytes = sizeof(uint64_t);
    constexpr uint32_t quadwordStoreBytes = 2U * sizeof(uint64_t);
    constexpr uint32_t unsupportedWriteBytes = 0U;
    switch (instruction) {
        case InstrType::SIMT_STS_B8:
        case InstrType::SIMT_ST_B8:
            return byteStoreBytes;
        case InstrType::SIMT_STS_B16:
        case InstrType::SIMT_ST_B16:
        case InstrType::RED_S_BF16:
        case InstrType::RED_S_FP16:
            return halfwordStoreBytes;
        case InstrType::SIMT_STS_B64:
            return doublewordStoreBytes;
        case InstrType::SIMT_STS_B128:
            return quadwordStoreBytes;
        case InstrType::SIMT_STS_B32:
        case InstrType::SIMT_ST_B32:
        case InstrType::RED_S_U32:
        case InstrType::RED_S_S32:
        case InstrType::RED_S_FP32:
        case InstrType::ATOM_MAX_S_S32:
        case InstrType::ATOM_MIN_S_S32:
        case InstrType::ATOM_ADD_S_S32:
        case InstrType::ATOM_ADD_S_U32:
        case InstrType::ATOM_EXCH_S_U32:
        case InstrType::ATOM_MIN_S_U32:
        case InstrType::ATOM_CAS_S_U32:
        case InstrType::ATOM_MAX_S_U32:
            return wordStoreBytes;
        default:
            return unsupportedWriteBytes;
    }
}
uint32_t WriteMode(InstrType instruction)
{
    switch (instruction) {
        case InstrType::ATOM_ADD_S_S32:
        case InstrType::ATOM_ADD_S_U32:
            return ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
        case InstrType::ATOM_MIN_S_S32:
        case InstrType::ATOM_MIN_S_U32:
            return ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN;
        case InstrType::ATOM_MAX_S_S32:
        case InstrType::ATOM_MAX_S_U32:
            return ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX;
        default:
            return ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    }
}
void SetSimtUbExecutionContext(const ParsedSimtTraceRecord& parsed, uint32_t bytes, AclsanSimtUbMemoryAccessData& data)
{
    constexpr uint32_t invocationIdBits = std::numeric_limits<uint32_t>::digits;
    const auto& raw = parsed.record;
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.launchId = parsed.launchId;
    data.header.pc = raw.pc;
    data.header.instrExecId = parsed.instrExecId;
    data.header.deviceId = parsed.deviceId;
    data.header.sourceKind = ACLSAN_DEVICE_SOURCE_VECTOR;
    data.header.executor.phyCoreId = parsed.phyCoreId;
    data.header.executor.blockId = raw.blockId;
    data.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT | ACLSAN_DEVICE_EVENT_FLAG_SIMT_UB;
    data.simt.invocationId = (static_cast<uint64_t>(parsed.phyCoreId) << invocationIdBits) | raw.invocationId;
    data.simt.scalarPc = raw.scalarPc;
    data.simt.threadX = raw.threadX;
    data.simt.threadY = raw.threadY;
    data.simt.threadZ = raw.threadZ;
    data.simt.blockDimX = raw.blockDimX;
    data.simt.blockDimY = raw.blockDimY;
    data.simt.blockDimZ = raw.blockDimZ;
    data.simt.base = raw.base;
    data.simt.offset = raw.offset;
    data.simt.elementBytes = bytes;
    data.simt.validFlags = ACLSAN_SIMT_CONTEXT_INVOCATION | ACLSAN_SIMT_CONTEXT_THREAD | ACLSAN_SIMT_CONTEXT_BLOCK_DIM |
                           ACLSAN_SIMT_CONTEXT_ADDRESS;
}
} // namespace

bool IsSimtUbTraceRecord(const ParsedSimtTraceRecord& parsed) noexcept
{
    const auto instruction = static_cast<InstrType>(parsed.record.instructionId);
    return WriteBytes(instruction) != 0 && instruction != InstrType::SIMT_ST_B8 &&
           instruction != InstrType::SIMT_ST_B16 && instruction != InstrType::SIMT_ST_B32;
}

std::optional<AclsanSimtUbMemoryAccessData> TranslateSimtUbTraceRecord(
    const ParsedSimtTraceRecord& parsed, const AclsanUbLayout& layout) noexcept
{
    const auto& raw = parsed.record;
    const auto instruction = static_cast<InstrType>(raw.instructionId);
    const uint32_t bytes = WriteBytes(instruction);
    if (bytes == 0 || raw.reserved != 0 || raw.blockDimX == 0 || raw.blockDimY == 0 || raw.blockDimZ == 0 ||
        raw.threadX >= raw.blockDimX || raw.threadY >= raw.blockDimY || raw.threadZ >= raw.blockDimZ) {
        return std::nullopt;
    }
    // Evaluate the addressing expression before narrowing, so overflowing a
    // 64-bit base cannot wrap an invalid UB address back into a valid region.
    const __int128 address = static_cast<__int128>(raw.base) + static_cast<__int128>(raw.offset) * bytes;
    const uint64_t ubAddress = address < 0 || address > std::numeric_limits<uint64_t>::max() ?
                                   std::numeric_limits<uint64_t>::max() :
                                   static_cast<uint64_t>(address);
    AclsanSimtUbMemoryAccessData data{};
    SetSimtUbExecutionContext(parsed, bytes, data);
    data.address = ubAddress;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    data.memoryOwner = data.header.executor;
    data.accessMode = WriteMode(instruction);
    data.accessCount = 1;
    data.dataBits = bytes * std::numeric_limits<uint8_t>::digits;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = bytes;
    data.instructionId = raw.instructionId;
    data.ubLayout = layout;
    return data;
}
} // namespace aclsan
