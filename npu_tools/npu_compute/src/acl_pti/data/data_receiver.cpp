/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "data_processor.h"
#include "securec.h"
#include "data_types.h"
#include "raw_data_decoder.h"
#include <cstring>
#include <limits>
#include <optional>

namespace aclpti::data {
namespace {
constexpr int MAX_BIU_GROUP_ID = 5;

// SDK compatibility values; remove when the build SDK provides these enumerators.
constexpr int BIU_PERF_AIV0_DATA_TYPE = 10;
constexpr int BIU_PERF_AIV1_DATA_TYPE = 11;

constexpr size_t MAX_BIU_REPLAY_BYTES = 64U * 1024U * 1024U;

// Core identity comes from RawDataType, never from blockId or BIU word contents.
std::optional<aclptiBiuCoreKind> BiuCoreKind(RawDataType type)
{
    if (type == BIU_PERF_DATA_TYPE) {
        return aclptiBiuCoreKind::Aic;
    }
    if (type == BIU_PERF_AIV0_DATA_TYPE) {
        return aclptiBiuCoreKind::Aiv0;
    }
    if (type == BIU_PERF_AIV1_DATA_TYPE) {
        return aclptiBiuCoreKind::Aiv1;
    }
    return std::nullopt;
}

size_t RawRecordSize(const MsprofRawData* raw, std::optional<aclptiBiuCoreKind> core)
{
    if (raw == nullptr) {
        return 0;
    }
    switch (raw->type) {
        case LOG_DATA_TYPE:
            return TASK_LOG_BYTES;
        case PMU_DATA_TYPE:
            return PMU_RECORD_BYTES;
        default:
            return core ? sizeof(uint32_t) : 1;
    }
}

bool IsPrimaryPayload(ReplayKind kind, RawDataType type, bool biu)
{
    switch (kind) {
        case ReplayKind::Pipeline:
            return biu;
        case ReplayKind::Pmu:
            return type == PMU_DATA_TYPE;
        case ReplayKind::PcSampling:
            return type == PC_SAMPLING_DATA_TYPE;
        default:
            return false;
    }
}

} // namespace

bool DataProcessor::ActiveReplay::HasOpenPacket() const
{
    for (const auto& entry : biuChannels) {
        if (entry.second.packetOpen) {
            return true;
        }
    }
    for (const auto& entry : pcChannels) {
        if (entry.second.packetOpen) {
            return true;
        }
    }
    return false;
}

aclptiResult DataProcessor::ReceiveDataChunk(const MsprofRawData& raw, std::optional<aclptiBiuCoreKind> core)
{
    // ReceiveRawData already validates payload type, size/alignment and device identity.
    auto& replay = *active_;
    aclptiPipelineKey key{};
    ReceiveState* state = nullptr;
    if (core) {
        if (raw.chunkModule < 0 || raw.chunkModule > MAX_BIU_GROUP_ID || raw.offset % sizeof(uint32_t) != 0) {
            return ACLPTI_ERROR_INVALID_RAW_DATA;
        }
        if (replay.stats.acceptedBytes > MAX_BIU_REPLAY_BYTES - raw.chunkSize) {
            return ACLPTI_ERROR_OUT_OF_MEMORY;
        }
        key = {static_cast<uint8_t>(raw.chunkModule), *core};
        state = &replay.biuChannels[key];
    } else {
        state = &replay.pcChannels[raw.chunkModule];
    }
    // BIU continuity is tracked independently for each (groupId, coreKind).
    const auto expected = state->packetOpen ? state->expectedOffset : 0;
    if (raw.offset != expected || raw.offset > std::numeric_limits<size_t>::max() - raw.chunkSize) {
        return ACLPTI_ERROR_INVALID_RAW_DATA;
    }
    DataItem item;
    item.replayId = replay.info.replayId;
    const auto* bytes = reinterpret_cast<const uint8_t*>(raw.chunk);
    if (core) {
        item.payload = BiuChunk{key, std::vector<uint8_t>(bytes, bytes + raw.chunkSize)};
    } else {
        item.payload = aclptiRawDataChunk{replay.info.replayId, raw.deviceId,    raw.chunkModule,
                                          raw.offset,           raw.isLastChunk, {bytes, bytes + raw.chunkSize}};
    }
    if (!Submit(std::move(item))) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    // Commit reception state only after the payload has been accepted by the queue.
    replay.deviceId = raw.deviceId;
    state->expectedOffset = raw.isLastChunk ? 0 : raw.offset + raw.chunkSize;
    state->packetOpen = !raw.isLastChunk;
    ++replay.callbackStats.copiedRecordCount;
    replay.callbackStats.copiedBytes += raw.chunkSize;
    replay.stats.acceptedBytes += raw.chunkSize;
    return ACLPTI_SUCCESS;
}

aclptiResult DataProcessor::ReportRawDataFailure(
    const MsprofRawData* raw, size_t recordSize, aclptiResult status, bool primary)
{
    auto& replay = *active_;
    ++replay.stats.failedRecordCount;
    if (primary) {
        ++replay.stats.rejectedChunkCount;
    }
    ASCTOOL_ERROR(
        "[aclpti-data] raw callback invalid chunk: replay=%llu kind=%d status=%d primary=%d type=%d device=%d "
        "module=%d offset=%zu chunk_size=%zu last=%d expected_record_size=%zu expected_device=%d"
        " code=%u",
        static_cast<unsigned long long>(replay.info.replayId), static_cast<int>(replay.info.kind),
        static_cast<int>(status), static_cast<int>(primary), raw == nullptr ? -1 : static_cast<int>(raw->type),
        raw == nullptr ? -1 : raw->deviceId, raw == nullptr ? -1 : raw->chunkModule, raw == nullptr ? 0 : raw->offset,
        raw == nullptr ? 0 : raw->chunkSize, static_cast<int>(raw != nullptr && raw->isLastChunk), recordSize,
        replay.deviceId, static_cast<unsigned>(static_cast<uint32_t>(status)));
    return status;
}

aclptiResult DataProcessor::ValidateRawData(const MsprofRawData* raw, size_t recordSize, bool& primary)
{
    if (raw == nullptr) {
        return ACLPTI_ERROR_INVALID_RAW_DATA;
    }
    auto& replay = *active_;
    const bool log = raw->type == LOG_DATA_TYPE;
    if (!primary && !log) {
        primary = true;
        return ACLPTI_ERROR_INVALID_RAW_DATA;
    }
    if (raw->chunkSize == 0 || raw->chunkSize > sizeof(raw->chunk) || raw->chunkSize % recordSize != 0) {
        return ACLPTI_ERROR_INVALID_RAW_DATA;
    }
    if (primary) {
        replay.stats.receivedBytes += raw->chunkSize;
    }
    if (raw->deviceId < 0 || (replay.deviceId >= 0 && replay.deviceId != raw->deviceId)) {
        return ACLPTI_ERROR_INVALID_RAW_DATA;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult DataProcessor::ReceiveFixedRecords(const MsprofRawData& raw, size_t recordSize, bool primary)
{
    auto& replay = *active_;
    // Each complete fixed-size record is independent; partial acceptance remains in stats.
    for (size_t offset = 0; offset < raw.chunkSize; offset += recordSize) {
        RawRecord record;
        record.size = recordSize;
        record.recordIndex = replay.nextRecordIndex;
        record.events = replay.info.pmuEventIds;
        if (memcpy_s(record.bytes.data(), record.bytes.size(), raw.chunk + offset, recordSize) != EOK) {
            return ACLPTI_ERROR_INVALID_RAW_DATA;
        }
        if (!Submit(DataItem{replay.info.replayId, std::move(record)})) {
            return ACLPTI_ERROR_INVALID_STATE;
        }
        replay.deviceId = raw.deviceId;
        ++replay.nextRecordIndex;
        ++replay.callbackStats.copiedRecordCount;
        replay.callbackStats.copiedBytes += recordSize;
        if (primary) {
            replay.stats.acceptedBytes += recordSize;
        }
    }
    return ACLPTI_SUCCESS;
}

aclptiResult DataProcessor::ReceiveRawData(const MsprofRawData* raw)
{
    if (!active_) {
        return ACLPTI_ERROR_NO_ACTIVE_REPLAY;
    }
    if (active_->closed) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    auto& replay = *active_;
    const auto core = raw == nullptr ? std::nullopt : BiuCoreKind(raw->type);
    const size_t recordSize = RawRecordSize(raw, core);
    bool primary = raw != nullptr && IsPrimaryPayload(replay.info.kind, raw->type, core.has_value());
    const auto validationStatus = ValidateRawData(raw, recordSize, primary);
    if (validationStatus != ACLPTI_SUCCESS) {
        return ReportRawDataFailure(raw, recordSize, validationStatus, primary);
    }
    try {
        const auto status = core || raw->type == PC_SAMPLING_DATA_TYPE ? ReceiveDataChunk(*raw, core) :
                                                                         ReceiveFixedRecords(*raw, recordSize, primary);
        if (status != ACLPTI_SUCCESS) {
            return ReportRawDataFailure(raw, recordSize, status, primary);
        }
    } catch (const std::bad_alloc&) {
        return ReportRawDataFailure(raw, recordSize, ACLPTI_ERROR_OUT_OF_MEMORY, primary);
    }
    replay.callbackStats.receivedBytes += raw->chunkSize;
    return ACLPTI_SUCCESS;
}

} // namespace aclpti::data
