/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_simt_trace.h"
#include "acl_san/aclsan_api.h"
#include "dbi/simt_instruction_widths.h"
#include "simt_memory_record.h"

#include <climits>

namespace aclsan {
namespace {

uint32_t AccessMode(uint32_t instructionId)
{
    switch (static_cast<InstrType>(instructionId)) {
        case InstrType::ATOM_ADD_G_U64:
        case InstrType::ATOM_ADD_G_S64:
        case InstrType::ATOM_ADD_G_U32:
        case InstrType::ATOM_ADD_G_S32:
            return ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
        case InstrType::ATOM_MIN_G_U64:
        case InstrType::ATOM_MIN_G_S64:
        case InstrType::ATOM_MIN_G_U32:
        case InstrType::ATOM_MIN_G_S32:
            return ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN;
        case InstrType::ATOM_MAX_G_U64:
        case InstrType::ATOM_MAX_G_S64:
        case InstrType::ATOM_MAX_G_U32:
        case InstrType::ATOM_MAX_G_S32:
            return ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX;
        case InstrType::SIMT_ST_B8:
        case InstrType::SIMT_ST_B16:
        case InstrType::SIMT_ST_B32:
        case InstrType::SIMT_STG_B8:
        case InstrType::SIMT_STG_B16:
        case InstrType::SIMT_STG_B32:
        case InstrType::SIMT_STG_B64:
        case InstrType::SIMT_STG_B128:
        case InstrType::RED_G_BF16:
        case InstrType::RED_G_U32:
        case InstrType::RED_G_F16X2:
        case InstrType::RED_G_S32:
        case InstrType::RED_G_FP32:
        case InstrType::RED_G_BF16X2:
        case InstrType::RED_G_FP16:
        case InstrType::ATOM_EXCH_G_U64:
        case InstrType::ATOM_CAS_G_U64:
        case InstrType::ATOM_EXCH_G_U32:
        case InstrType::ATOM_CAS_G_U32:
            return ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        default:
            return ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    }
}

bool HasUnknownAtomicConfiguration(uint32_t instructionId)
{
    const auto instruction = static_cast<InstrType>(instructionId);
    return instruction >= InstrType::RED_G_BF16 && instruction <= InstrType::ATOM_MAX_G_S32;
}

} // namespace

std::optional<AclsanDeviceMemoryAccessData> TranslateSimtTraceRecord(const ParsedSimtTraceRecord& parsed) noexcept
{
    const auto& raw = parsed.record;
    const uint32_t width = SimtInstructionElementBytes(raw.instructionId);
    if (width == 0) {
        return std::nullopt;
    }
    npucheck::SimtMemoryAccessRecord address{};
    address.base = raw.base;
    address.offset = raw.offset;
    address.elementBytes = width;
    address.accessBytes = width;
    const auto range = npucheck::ResolveSimtAccessRange(address);

    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.launchId = parsed.launchId;
    data.header.pc = raw.pc;
    data.header.sourceKind = ACLSAN_DEVICE_SOURCE_VECTOR;
    data.header.instrExecId = parsed.instrExecId;
    data.header.deviceId = parsed.deviceId;
    data.header.executor.phyCoreId = parsed.phyCoreId;
    data.header.executor.blockId = raw.blockId;
    data.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    data.header.flags = range.has_value() ? ACLSAN_DEVICE_EVENT_FLAG_EXACT :
                                            (ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED | ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED);
    data.address = range.has_value() ? range->first : 0;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    data.accessMode = AccessMode(raw.instructionId);
    if (HasUnknownAtomicConfiguration(raw.instructionId)) {
        data.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
    }
    data.accessCount = 1U;
    data.dataBits = width * CHAR_BIT;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = width;
    data.instructionId = raw.instructionId;
    data.simt.invocationId = (static_cast<uint64_t>(parsed.phyCoreId) << 32U) | raw.invocationId;
    data.simt.scalarPc = raw.scalarPc;
    data.simt.threadX = raw.threadX;
    data.simt.threadY = raw.threadY;
    data.simt.threadZ = raw.threadZ;
    data.simt.blockDimX = raw.blockDimX;
    data.simt.blockDimY = raw.blockDimY;
    data.simt.blockDimZ = raw.blockDimZ;
    data.simt.base = raw.base;
    data.simt.offset = raw.offset;
    data.simt.elementBytes = width;
    data.simt.validFlags = ACLSAN_SIMT_CONTEXT_INVOCATION | ACLSAN_SIMT_CONTEXT_THREAD | ACLSAN_SIMT_CONTEXT_BLOCK_DIM |
                           (range.has_value() ? static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_ADDRESS) : 0U);
    return data;
}

} // namespace aclsan
