/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_BUFFER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_BUFFER_H

#include "acl_san/aclsan_cbdata_device.h"
#include "dbi/trace_buffer_abi.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace aclsan {

struct ParsedTraceRecord {
    AclsanRawTraceRecord record{};
    uint64_t instrExecId = 0;
    uint64_t launchId = 0;
    uint32_t blockId = 0;
    uint32_t blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE;
    uint32_t phyCoreId = 0;
    uint32_t deviceId = 0;
    uint64_t parameterBytes = 0;
    uint32_t physicalCoreSlotCount = 0;
};

struct TraceBufferParseResult {
    bool ok = false;
    std::vector<ParsedTraceRecord> records;
    std::string error;
};

struct ParsedSimtTraceRecord {
    AclsanSimtRawTraceRecord record{};
    uint64_t instrExecId = 0;
    uint64_t launchId = 0;
    uint32_t phyCoreId = 0;
    uint32_t deviceId = 0;
};

struct SimtTraceParseResult {
    bool ok = false;
    uint64_t droppedRecords = 0;
    std::vector<ParsedSimtTraceRecord> records;
    std::string error;
};

class SimtFifoBoundaries {
public:
    bool Observe(const AclsanSimtRawTraceRecord& record, uint32_t core, uint32_t slot, uint32_t blocks);
    bool Complete(uint32_t expectedBlocks = 0U) const;

private:
    struct Invocation {
        std::bitset<ASCSAN_SIMT_THREADS_PER_CORE> begun;
        std::bitset<ASCSAN_SIMT_THREADS_PER_CORE> ended;
        uint32_t lanes;
        uint16_t dimX;
        uint16_t dimY;
        uint64_t scalarPc;
    };
    std::map<std::tuple<uint32_t, uint32_t, uint64_t>, Invocation> invocations_;
};

bool InitializeTraceBuffer(
    std::vector<uint8_t>& buffer, uint32_t physicalCoreSlotCount, uint32_t blockCount, uint64_t launchId,
    std::string& error, bool enableSimt = false, bool onlineSimt = false);

SimtTraceParseResult ParseSimtTraceBuffer(
    const uint8_t* buffer, size_t bytes, uint32_t physicalCoreSlotCount, uint32_t blockCount, uint64_t launchId,
    uint32_t deviceId, bool executionComplete);

SimtTraceParseResult ParseSimtTraceSegment(
    const uint8_t* buffer, size_t bytes, uint32_t physicalCoreCount, uint32_t blockCount, uint64_t launchId,
    uint32_t deviceId, bool executionComplete);

bool ValidateTraceSegmentHeader(const AclsanTraceBufferHeader& header, uint32_t blockCount, uint64_t launchId) noexcept;
bool ValidateTraceFifoRecord(
    const AclsanRawTraceRecord& record, uint32_t slot, uint32_t sequence, uint32_t blockCount) noexcept;

// records is an already-copied contiguous batch, in increasing sequence order.
// The byte pointer need not be aligned for AclsanRawTraceRecord.
TraceBufferParseResult DecodeTraceFifoRange(
    const uint8_t* records, size_t count, uint32_t slot, uint32_t firstSequence, uint32_t blockCount, uint64_t launchId,
    uint32_t deviceId);

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_BUFFER_H
