/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_trace_buffer.h"

#include <cstring>

namespace aclsan {
bool ValidateTraceSegmentHeader(const AclsanTraceBufferHeader& header, uint32_t blockCount, uint64_t launchId) noexcept
{
    size_t bytes = 0;
    return blockCount != 0U && launchId != 0U && TraceBufferBytes(header.physicalCoreCount, &bytes) &&
           header.magic == ASCSAN_TRACE_BUFFER_MAGIC && header.launchId == launchId &&
           header.blockCount == blockCount && header.segmentBytes == bytes &&
           header.simdSegmentOffset == sizeof(AclsanTraceBufferHeader) &&
           header.simdSegmentBytes == bytes - sizeof(AclsanTraceBufferHeader) && header.simtSegmentOffset == 0U &&
           header.simtSegmentBytes == 0U;
}

bool ValidateTraceFifoRecord(
    const AclsanRawTraceRecord& record, uint32_t slot, uint32_t sequence, uint32_t blockCount) noexcept
{
    return slot < ASCSAN_PHYSICAL_CORE_SLOT_COUNT && sequence != UINT32_MAX && record.padding == 0U &&
           IsTraceBlockIdValid(record.blockId, IsAicPhysicalCore(slot), blockCount);
}

bool InitializeTraceBuffer(
    std::vector<uint8_t>& buffer, uint32_t physicalCoreSlotCount, uint32_t blockCount, uint64_t launchId,
    std::string& error)
{
    size_t bytes = 0;
    if (blockCount == 0U || launchId == 0U || !TraceBufferBytes(physicalCoreSlotCount, &bytes)) {
        buffer.clear();
        error = "trace buffer dimensions or launch ID are invalid";
        return false;
    }
    buffer.assign(bytes, 0U);
    AclsanTraceBufferHeader header{};
    header.magic = ASCSAN_TRACE_BUFFER_MAGIC;
    header.launchId = launchId;
    header.segmentBytes = bytes;
    header.physicalCoreCount = physicalCoreSlotCount;
    header.blockCount = blockCount;
    header.simdSegmentOffset = sizeof(header);
    header.simdSegmentBytes = bytes - sizeof(header);
    std::memcpy(buffer.data(), &header, sizeof(header));
    error.clear();
    return true;
}

TraceBufferParseResult DecodeTraceFifoRange(
    const uint8_t* records, size_t count, uint32_t slot, uint32_t firstSequence, uint32_t blockCount, uint64_t launchId,
    uint32_t deviceId)
{
    TraceBufferParseResult result;
    if ((records == nullptr && count != 0U) || slot >= ASCSAN_PHYSICAL_CORE_SLOT_COUNT || blockCount == 0U ||
        launchId == 0U || count > ASCSAN_TRACE_RECORDS_PER_CORE ||
        count > static_cast<uint64_t>(UINT32_MAX) - firstSequence) {
        result.error = "trace FIFO range is invalid";
        return result;
    }
    result.records.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        AclsanRawTraceRecord raw{};
        std::memcpy(&raw, records + index * sizeof(raw), sizeof(raw));
        const uint32_t sequence = firstSequence + static_cast<uint32_t>(index);
        if (!ValidateTraceFifoRecord(raw, slot, sequence, blockCount)) {
            result.records.clear();
            result.error = "trace FIFO record has an invalid sequence, padding, or logical block";
            return result;
        }
        ParsedTraceRecord parsed{};
        parsed.record = raw;
        parsed.instrExecId = (static_cast<uint64_t>(slot) << 32U) | (static_cast<uint64_t>(sequence) + 1U);
        parsed.launchId = launchId;
        parsed.blockId = raw.blockId;
        parsed.blockType =
            IsAicPhysicalCore(slot) ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        parsed.phyCoreId = slot;
        parsed.deviceId = deviceId;
        parsed.physicalCoreSlotCount = ASCSAN_PHYSICAL_CORE_SLOT_COUNT;
        result.records.push_back(parsed);
    }
    result.ok = true;
    return result;
}

} // namespace aclsan
