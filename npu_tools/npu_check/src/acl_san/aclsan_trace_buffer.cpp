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
#include "securec.h"

#include "dbi/trace_buffer_abi.h"

#include <cstring>

namespace aclsan {

bool InitializeTraceBuffer(
    std::vector<uint8_t>& buffer, uint32_t physicalCoreCount, uint32_t blockCount, uint64_t launchId,
    std::string& error)
{
    size_t bytes = 0;
    if (blockCount == 0U || !aclsan::TraceBufferBytes(physicalCoreCount, &bytes)) {
        buffer.clear();
        error = "trace buffer dimensions are zero or overflow the layout";
        return false;
    }

    buffer.assign(bytes, 0);
    aclsan::AclsanTraceBufferHeader header{};
    header.magic = aclsan::ASCSAN_TRACE_BUFFER_MAGIC;
    header.launchId = launchId;
    header.segmentBytes = static_cast<uint64_t>(bytes);
    header.blockCount = blockCount;
    header.physicalCoreCount = physicalCoreCount;
    if (memcpy_s(buffer.data(), buffer.size(), &header, sizeof(header)) != EOK) {
        buffer.clear();
        error = "cannot initialize trace segment header";
        return false;
    }

    const aclsan::AclsanTraceSliceHeader emptySlice{};
    for (uint32_t sliceIndex = 0; sliceIndex < physicalCoreCount; ++sliceIndex) {
        const size_t sliceOffset =
            sizeof(header) + static_cast<size_t>(sliceIndex) * aclsan::ASCSAN_TRACE_BYTES_PER_CORE;
        if (memcpy_s(buffer.data() + sliceOffset, buffer.size() - sliceOffset, &emptySlice, sizeof(emptySlice)) !=
            EOK) {
            buffer.clear();
            error = "cannot initialize trace slice header";
            return false;
        }
    }
    error.clear();
    return true;
}

TraceBufferParseResult ParseTraceBuffer(
    const uint8_t* buffer, size_t bytes, uint32_t expectedPhysicalCoreCount, uint32_t expectedBlockCount,
    uint64_t expectedLaunchId, uint32_t deviceId)
{
    TraceBufferParseResult result;
    size_t requiredBytes = 0;
    if (buffer == nullptr || expectedBlockCount == 0U ||
        !aclsan::TraceBufferBytes(expectedPhysicalCoreCount, &requiredBytes) || bytes < requiredBytes) {
        result.error = "trace buffer is null, truncated, or has invalid expected dimensions";
        return result;
    }

    aclsan::AclsanTraceBufferHeader header{};
    if (memcpy_s(&header, sizeof(header), buffer, sizeof(header)) != EOK) {
        result.records.clear();
        result.error = "cannot copy trace record or header";
        return result;
    }
    if (header.magic != aclsan::ASCSAN_TRACE_BUFFER_MAGIC || header.launchId != expectedLaunchId ||
        header.segmentBytes != requiredBytes || header.blockCount != expectedBlockCount ||
        header.physicalCoreCount != expectedPhysicalCoreCount) {
        result.error = "trace segment header does not match the launch-owned layout";
        return result;
    }

    size_t recordCount = 0;
    for (uint32_t sliceIndex = 0; sliceIndex < expectedPhysicalCoreCount; ++sliceIndex) {
        const size_t sliceOffset =
            sizeof(header) + static_cast<size_t>(sliceIndex) * aclsan::ASCSAN_TRACE_BYTES_PER_CORE;
        aclsan::AclsanTraceSliceHeader slice{};
        if (memcpy_s(&slice, sizeof(slice), buffer + sliceOffset, sizeof(slice)) != EOK) {
            result.records.clear();
            result.error = "cannot copy trace record or header";
            return result;
        }
        if (slice.recordCount > aclsan::ASCSAN_TRACE_RECORDS_PER_CORE) {
            result.error = "trace slice record count exceeds its launch-owned capacity";
            return result;
        }
        result.overflowCount += slice.overflowCount;
        recordCount += slice.recordCount;
        if (slice.recordCount == 0) {
            continue;
        }

        const uint32_t expectedPhyCoreId = sliceIndex;
        if (slice.phyCoreId != expectedPhyCoreId) {
            result.error = "trace slice physical core ID does not match its layout index";
            return result;
        }
    }

    result.records.reserve(recordCount);
    for (uint32_t sliceIndex = 0; sliceIndex < expectedPhysicalCoreCount; ++sliceIndex) {
        const size_t sliceOffset =
            sizeof(header) + static_cast<size_t>(sliceIndex) * aclsan::ASCSAN_TRACE_BYTES_PER_CORE;
        aclsan::AclsanTraceSliceHeader slice{};
        if (memcpy_s(&slice, sizeof(slice), buffer + sliceOffset, sizeof(slice)) != EOK) {
            result.records.clear();
            result.error = "cannot copy trace record or header";
            return result;
        }
        if (slice.recordCount == 0) {
            continue;
        }

        const bool isAic = aclsan::IsAicPhysicalCore(sliceIndex, expectedPhysicalCoreCount);
        const uint32_t expectedPhyCoreId = sliceIndex;
        const uint32_t blockType =
            isAic ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        for (uint32_t index = 0; index < slice.recordCount; ++index) {
            const size_t recordOffset =
                sliceOffset + sizeof(slice) + static_cast<size_t>(index) * sizeof(aclsan::AclsanRawTraceRecord);
            aclsan::AclsanRawTraceRecord wire{};
            if (memcpy_s(&wire, sizeof(wire), buffer + recordOffset, sizeof(wire)) != EOK) {
                result.records.clear();
                result.error = "cannot copy trace record or header";
                return result;
            }

            ParsedTraceRecord parsed{};
            parsed.record.pc = wire.pc;
            parsed.record.instrId = wire.instrId;
            if (memcpy_s(parsed.record.args, sizeof(parsed.record.args), wire.args, sizeof(wire.args)) != EOK) {
                result.records.clear();
                result.error = "cannot copy trace record or header";
                return result;
            }
            parsed.record.siteId = wire.siteId;
            parsed.record.category = wire.category;
            parsed.record.pipeline = wire.pipeline;
            parsed.record.blockId = wire.blockId;
            parsed.record.reserved = wire.reserved;
            parsed.record.parameterBase = wire.parameterBase;
            if (!aclsan::IsTraceBlockIdValid(wire.blockId, isAic, expectedBlockCount)) {
                result.records.clear();
                result.error = "raw trace logical block ID exceeds the launch-owned range";
                return result;
            }
            // Endpoint owner mapping can place instructions from different
            // issuing cores under the same callback header. Use the fixed
            // trace slot, not a per-block counter, to keep IDs launch-unique
            // and stable when other slices grow between trace snapshots.
            parsed.instrExecId = uint64_t(sliceIndex) * aclsan::ASCSAN_TRACE_RECORDS_PER_CORE + index + 1U;
            parsed.launchId = header.launchId;
            parsed.blockId = wire.blockId;
            parsed.blockType = blockType;
            parsed.phyCoreId = expectedPhyCoreId;
            parsed.deviceId = deviceId;
            parsed.physicalCoreCount = expectedPhysicalCoreCount;
            result.records.push_back(parsed);
        }
    }
    result.ok = true;
    return result;
}

} // namespace aclsan
