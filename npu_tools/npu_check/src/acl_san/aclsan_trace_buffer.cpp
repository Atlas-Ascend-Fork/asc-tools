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

#include <algorithm>
#include <bitset>
#include <cstring>

namespace aclsan {
bool ValidateTraceSegmentHeader(const AclsanTraceBufferHeader& header, uint32_t blockCount, uint64_t launchId) noexcept
{
    size_t bytes = 0;
    return blockCount != 0U && launchId != 0U && TraceBufferBytes(header.physicalCoreCount, &bytes) &&
           header.magic == ASCSAN_TRACE_BUFFER_MAGIC && header.launchId == launchId &&
           header.blockCount == blockCount && header.simdSegmentOffset == sizeof(AclsanTraceBufferHeader) &&
           header.simdSegmentBytes == bytes - sizeof(AclsanTraceBufferHeader) &&
           TraceSegmentSizesMatch(header.segmentBytes, header.simtSegmentOffset, header.simtSegmentBytes, bytes);
}

bool ValidateTraceFifoRecord(
    const AclsanRawTraceRecord& record, uint32_t slot, uint32_t sequence, uint32_t blockCount) noexcept
{
    return slot < ASCSAN_PHYSICAL_CORE_SLOT_COUNT && sequence != UINT32_MAX && record.padding == 0U &&
           IsTraceBlockIdValid(record.blockId, IsAicPhysicalCore(slot), blockCount);
}

bool InitializeTraceBuffer(
    std::vector<uint8_t>& buffer, uint32_t physicalCoreSlotCount, uint32_t blockCount, uint64_t launchId,
    std::string& error, bool enableSimt)
{
    size_t bytes = 0;
    if (blockCount == 0U || launchId == 0U || !TraceBufferBytes(physicalCoreSlotCount, &bytes)) {
        buffer.clear();
        error = "trace buffer dimensions or launch ID are invalid";
        return false;
    }
    AclsanTraceBufferHeader header{};
    header.magic = ASCSAN_TRACE_BUFFER_MAGIC;
    header.launchId = launchId;
    header.segmentBytes = bytes;
    header.physicalCoreCount = physicalCoreSlotCount;
    header.blockCount = blockCount;
    header.simdSegmentOffset = sizeof(header);
    header.simdSegmentBytes = bytes - sizeof(header);
    if (enableSimt) {
        header.simtSegmentOffset = SimtTraceOffset(bytes);
        header.simtSegmentBytes = SimtTraceBytes(physicalCoreSlotCount);
        header.segmentBytes = header.simtSegmentOffset + header.simtSegmentBytes;
    }
    buffer.assign(static_cast<size_t>(header.segmentBytes), 0U);
    if (memcpy_s(buffer.data(), buffer.size(), &header, sizeof(header)) != EOK) {
        buffer.clear();
        error = "cannot initialize trace buffer header";
        return false;
    }
    if (enableSimt) {
        const AclsanSimtTraceHeader simt{
            ASCSAN_SIMT_TRACE_MAGIC,
            header.simtSegmentBytes,
            ASCSAN_SIMT_TRACE_VERSION,
            physicalCoreSlotCount,
            ASCSAN_SIMT_THREADS_PER_CORE,
            ASCSAN_SIMT_RECORDS_PER_THREAD,
            {}};
        if (memcpy_s(buffer.data() + header.simtSegmentOffset, header.simtSegmentBytes, &simt, sizeof(simt)) != EOK) {
            buffer.clear();
            error = "cannot initialize SIMT trace header";
            return false;
        }
    }
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

SimtTraceParseResult ParseSimtTraceBuffer(
    const uint8_t* buffer, size_t bytes, uint32_t physicalCoreSlotCount, uint32_t blockCount, uint64_t launchId,
    uint32_t deviceId, bool executionComplete)
{
    SimtTraceParseResult result{};
    AclsanTraceBufferHeader trace{};
    if (buffer == nullptr || bytes < sizeof(trace)) {
        result.error = "SIMT buffer has invalid dimensions";
        return result;
    }
    if (memcpy_s(&trace, sizeof(trace), buffer, sizeof(trace)) != EOK) {
        result.error = "cannot copy SIMT trace buffer header";
        return result;
    }
    if (trace.physicalCoreCount != physicalCoreSlotCount || trace.segmentBytes != bytes ||
        !ValidateTraceSegmentHeader(trace, blockCount, launchId)) {
        result.error = "SIMT buffer does not match the owning launch";
        return result;
    }
    if (trace.simtSegmentBytes == 0U) {
        result.ok = true;
        return result;
    }
    return ParseSimtTraceSegment(
        buffer + trace.simtSegmentOffset, trace.simtSegmentBytes, physicalCoreSlotCount, blockCount, launchId, deviceId,
        executionComplete);
}

namespace {

struct SimtLaunchContext {
    uint32_t blockCount;
    uint64_t launchId;
    uint32_t deviceId;
    bool executionComplete;
};

struct SimtCoreCoverage {
    uint64_t invocationId;
    uint64_t completionMask;
    uint32_t coreId;
    uint32_t expectedLaneCount = 0;
    bool observedCurrentInvocation = false;
    std::bitset<ASCSAN_SIMT_THREADS_PER_CORE> completedLanes{};
    std::bitset<ASCSAN_SIMT_THREADS_PER_CORE> incompleteLanes{};
};

uint64_t SimtCompletionMask(uint64_t invocationId)
{
    if (invocationId == 0U || invocationId > ASCSAN_SIMT_COMPLETION_BITS) {
        return 0U;
    }
    return invocationId == ASCSAN_SIMT_COMPLETION_BITS ? UINT64_MAX : (UINT64_C(1) << invocationId) - 1U;
}

void TrackSimtLaneCoverage(
    SimtCoreCoverage& coverage, const AclsanSimtThreadHeader& lane, uint32_t laneId, bool executionComplete,
    SimtTraceParseResult& result)
{
    const bool current = coverage.invocationId != 0U && lane.lastInvocation == coverage.invocationId;
    coverage.observedCurrentInvocation = coverage.observedCurrentInvocation || current;
    if (executionComplete && current && lane.completedInvocation == coverage.invocationId &&
        coverage.invocationId <= ASCSAN_SIMT_COMPLETION_BITS &&
        (lane.completedInvocations & coverage.completionMask) == coverage.completionMask) {
        coverage.completedLanes.set(laneId);
    }
    result.droppedRecords += lane.overflowCount;
    if (lane.invalidContext != 0U || lane.recordCount > ASCSAN_SIMT_RECORDS_PER_THREAD ||
        (executionComplete && lane.lastInvocation > lane.completedInvocation)) {
        ++result.droppedRecords;
        coverage.incompleteLanes.set(laneId);
    }
}

bool IsValidSimtRecord(
    const AclsanSimtRawTraceRecord& record, const SimtCoreCoverage& coverage, uint32_t laneId, uint32_t blockCount)
{
    if (IsAicPhysicalCore(coverage.coreId) || record.invocationId == 0U ||
        record.invocationId > coverage.invocationId || record.invocationId > UINT32_MAX ||
        !IsTraceBlockIdValid(record.blockId, false, blockCount) || record.blockDimX == 0U || record.blockDimY == 0U ||
        record.blockDimZ == 0U || record.threadX >= record.blockDimX || record.threadY >= record.blockDimY ||
        record.threadZ >= record.blockDimZ) {
        return false;
    }
    const uint64_t linearThread =
        record.threadX + static_cast<uint64_t>(record.blockDimX) *
                             (record.threadY + static_cast<uint64_t>(record.blockDimY) * record.threadZ);
    return linearThread == laneId;
}

void TrackSimtInvocationLanes(
    SimtCoreCoverage& coverage, const AclsanSimtRawTraceRecord& record, SimtTraceParseResult& result)
{
    if (record.invocationId != coverage.invocationId) {
        return;
    }
    const uint64_t laneCount = static_cast<uint64_t>(record.blockDimX) * record.blockDimY * record.blockDimZ;
    if (laneCount > ASCSAN_SIMT_THREADS_PER_CORE) {
        ++result.droppedRecords;
        return;
    }
    coverage.expectedLaneCount = std::max(coverage.expectedLaneCount, static_cast<uint32_t>(laneCount));
}

bool DecodeSimtLane(
    const uint8_t* laneData, uint32_t laneId, const SimtLaunchContext& launch, SimtCoreCoverage& coverage,
    SimtTraceParseResult& result)
{
    AclsanSimtThreadHeader lane{};
    if (memcpy_s(&lane, sizeof(lane), laneData, sizeof(lane)) != EOK) {
        return false;
    }
    TrackSimtLaneCoverage(coverage, lane, laneId, launch.executionComplete, result);
    const uint32_t count = std::min(lane.recordCount, ASCSAN_SIMT_RECORDS_PER_THREAD);
    // FIFO IDs reserve the low bits for the sequence and the high bits for the physical slot.
    constexpr uint32_t FIFO_SEQUENCE_BITS = 32U;
    const uint64_t firstId = (static_cast<uint64_t>(ASCSAN_PHYSICAL_CORE_SLOT_COUNT) << FIFO_SEQUENCE_BITS) +
                             (static_cast<uint64_t>(coverage.coreId) * ASCSAN_SIMT_THREADS_PER_CORE + laneId) *
                                 ASCSAN_SIMT_RECORDS_PER_THREAD;
    for (uint32_t index = 0; index < count; ++index) {
        ParsedSimtTraceRecord parsed{};
        const size_t offset = sizeof(lane) + index * sizeof(parsed.record);
        if (memcpy_s(&parsed.record, sizeof(parsed.record), laneData + offset, sizeof(parsed.record)) != EOK) {
            return false;
        }
        if (!IsValidSimtRecord(parsed.record, coverage, laneId, launch.blockCount)) {
            ++result.droppedRecords;
            continue;
        }
        TrackSimtInvocationLanes(coverage, parsed.record, result);
        parsed.instrExecId = firstId + index + 1U;
        parsed.launchId = launch.launchId;
        parsed.phyCoreId = coverage.coreId;
        parsed.deviceId = launch.deviceId;
        result.records.push_back(parsed);
    }
    return true;
}

void FinishSimtCoreCoverage(const SimtCoreCoverage& coverage, SimtTraceParseResult& result)
{
    if (coverage.invocationId == 0U) {
        return;
    }
    if (!coverage.observedCurrentInvocation) {
        ++result.droppedRecords;
    }
    for (uint32_t laneId = 0; laneId < coverage.expectedLaneCount; ++laneId) {
        if (!coverage.completedLanes.test(laneId) && !coverage.incompleteLanes.test(laneId)) {
            ++result.droppedRecords;
        }
    }
}

bool DecodeSimtCore(
    const uint8_t* coreData, uint32_t coreId, const SimtLaunchContext& launch, SimtTraceParseResult& result)
{
    AclsanSimtCoreHeader core{};
    if (memcpy_s(&core, sizeof(core), coreData, sizeof(core)) != EOK) {
        return false;
    }
    if (core.flags != 0U) {
        ++result.droppedRecords;
    }
    SimtCoreCoverage coverage{core.invocationId, SimtCompletionMask(core.invocationId), coreId};
    for (uint32_t laneId = 0; laneId < ASCSAN_SIMT_THREADS_PER_CORE; ++laneId) {
        // The caller validated the fixed sidecar size and each slot keeps its records within its partition.
        const size_t offset = sizeof(core) + laneId * ASCSAN_SIMT_THREAD_BYTES;
        if (!DecodeSimtLane(coreData + offset, laneId, launch, coverage, result)) {
            return false;
        }
    }
    if (launch.executionComplete) {
        FinishSimtCoreCoverage(coverage, result);
    }
    return true;
}

} // namespace

SimtTraceParseResult ParseSimtTraceSegment(
    const uint8_t* buffer, size_t bytes, uint32_t physicalCoreCount, uint32_t blockCount, uint64_t launchId,
    uint32_t deviceId, bool executionComplete)
{
    SimtTraceParseResult result{};
    if (buffer == nullptr || physicalCoreCount != ASCSAN_PHYSICAL_CORE_SLOT_COUNT || blockCount == 0U ||
        launchId == 0U || bytes != ASCSAN_SIMT_SEGMENT_BYTES) {
        result.error = "SIMT sidecar has invalid dimensions";
        return result;
    }
    AclsanSimtTraceHeader header{};
    if (memcpy_s(&header, sizeof(header), buffer, sizeof(header)) != EOK) {
        result.error = "cannot copy SIMT trace record or header";
        return result;
    }
    if (header.magic != ASCSAN_SIMT_TRACE_MAGIC || header.version != ASCSAN_SIMT_TRACE_VERSION ||
        header.bytes != bytes || header.physicalCoreCount != physicalCoreCount ||
        header.threadsPerCore != ASCSAN_SIMT_THREADS_PER_CORE ||
        header.recordsPerThread != ASCSAN_SIMT_RECORDS_PER_THREAD) {
        result.error = "SIMT sidecar header is incompatible";
        return result;
    }
    const SimtLaunchContext launch{blockCount, launchId, deviceId, executionComplete};
    for (uint32_t coreId = 0; coreId < physicalCoreCount; ++coreId) {
        const size_t coreOffset = sizeof(header) + coreId * ASCSAN_SIMT_CORE_BYTES;
        if (!DecodeSimtCore(buffer + coreOffset, coreId, launch, result)) {
            result.records.clear();
            result.error = "cannot copy SIMT trace record or header";
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace aclsan
