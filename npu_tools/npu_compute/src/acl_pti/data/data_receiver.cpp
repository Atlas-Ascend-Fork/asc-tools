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
#include <array>
#include "data_types.h"
#include "raw_data_decoder.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

namespace aclpti::data {
namespace {
constexpr size_t HEX_DUMP_BYTES_PER_LINE = 16;
constexpr size_t HEX_DUMP_CHARS_PER_BYTE = 3; // Two hexadecimal digits followed by a space.
constexpr int MAX_BIU_GROUP_ID = 5;

// Temporary SDK compatibility values; remove when the build SDK provides these enumerators.
constexpr int BIU_PERF_AIV0_DATA_TYPE = 10;
constexpr int BIU_PERF_AIV1_DATA_TYPE = 11;
constexpr char HEX_ALPHABET[] = "0123456789abcdef";
constexpr unsigned HEX_DIGIT_BITS = 4;
constexpr unsigned HEX_DIGIT_MASK = (1U << HEX_DIGIT_BITS) - 1;

void LogRawData(uint64_t replayId, const MsprofRawData& raw, bool biu, uint64_t acceptedBytes)
{
    if (!ASCTOOL_DEBUG_ENABLED()) {
        return;
    }
    ASCTOOL_DEBUG(
        "[aclpti-data] [DEBUG-rawdata] replay=%llu type=%d device=%d module=%d offset=%zu last=%d "
        "validBytes=%zu validWords=%zu acceptedBytes=%llu",
        static_cast<unsigned long long>(replayId), static_cast<int>(raw.type), raw.deviceId, raw.chunkModule,
        raw.offset, raw.isLastChunk ? 1 : 0, raw.chunkSize, raw.chunkSize / sizeof(uint32_t),
        static_cast<unsigned long long>(acceptedBytes));
    const auto* bytes = reinterpret_cast<const uint8_t*>(raw.chunk);
    // Print every BIU word, including timestamp headers, to diagnose framing without filtering.
    if (biu) {
        for (size_t offset = 0; offset < raw.chunkSize; offset += sizeof(uint32_t)) {
            ASCTOOL_DEBUG(
                "[aclpti-data] [DEBUG-rawdata] BIU replay=%llu offset=%zu word=0x%08x",
                static_cast<unsigned long long>(replayId), raw.offset + offset, ReadLittleEndianWord(bytes + offset));
        }
        return;
    }
    for (size_t offset = 0; offset < raw.chunkSize; offset += HEX_DUMP_BYTES_PER_LINE) {
        std::array<char, HEX_DUMP_BYTES_PER_LINE * HEX_DUMP_CHARS_PER_BYTE + 1> hex{};
        size_t position = 0;
        for (size_t index = offset; index < raw.chunkSize && index < offset + HEX_DUMP_BYTES_PER_LINE; ++index) {
            hex[position++] = HEX_ALPHABET[bytes[index] >> HEX_DIGIT_BITS];
            hex[position++] = HEX_ALPHABET[bytes[index] & HEX_DIGIT_MASK];
            hex[position++] = ' ';
        }
        ASCTOOL_DEBUG(
            "[aclpti-data] [DEBUG-rawdata] payload replay=%llu offset=%zu bytes=%s",
            static_cast<unsigned long long>(replayId), offset, hex.data());
    }
}

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
    const size_t recordSize = raw == nullptr             ? 0 :
                              raw->type == LOG_DATA_TYPE ? TASK_LOG_BYTES :
                              raw->type == PMU_DATA_TYPE ? PMU_RECORD_BYTES :
                              core                       ? sizeof(uint32_t) :
                                                           1;
    const auto fail = [&](aclptiResult status, bool primary) {
        ++replay.stats.failedRecordCount;
        if (primary) {
            ++replay.stats.rejectedChunkCount;
        }
        ASCTOOL_ERROR(
            "[aclpti-data] raw callback invalid chunk: replay=%llu kind=%d status=%d primary=%d type=%d device=%d "
            "module=%d offset=%zu chunk_size=%zu last=%d expected_record_size=%zu expected_device=%d"
            " code=%u",
            static_cast<unsigned long long>(replay.info.replayId), static_cast<int>(replay.info.kind),
            static_cast<int>(status), primary ? 1 : 0, raw == nullptr ? -1 : static_cast<int>(raw->type),
            raw == nullptr ? -1 : raw->deviceId, raw == nullptr ? -1 : raw->chunkModule,
            raw == nullptr ? 0 : raw->offset, raw == nullptr ? 0 : raw->chunkSize,
            raw != nullptr && raw->isLastChunk ? 1 : 0, recordSize, replay.deviceId,
            static_cast<unsigned>(static_cast<uint32_t>(status)));
        return status;
    };
    if (raw == nullptr) {
        return fail(ACLPTI_ERROR_INVALID_RAW_DATA, false);
    }
    // Task logs may accompany every replay kind, but primary payloads must match that kind.
    const bool primary = (replay.info.kind == ReplayKind::Pipeline && core.has_value()) ||
                         (replay.info.kind == ReplayKind::Pmu && raw->type == PMU_DATA_TYPE) ||
                         (replay.info.kind == ReplayKind::PcSampling && raw->type == PC_SAMPLING_DATA_TYPE);
    const bool log = raw->type == LOG_DATA_TYPE;
    if (!primary && !log) {
        return fail(ACLPTI_ERROR_INVALID_RAW_DATA, true);
    }
    if (raw->chunkSize == 0 || raw->chunkSize > sizeof(raw->chunk)) {
        return fail(ACLPTI_ERROR_INVALID_RAW_DATA, primary);
    }
    if (raw->chunkSize % recordSize != 0) {
        return fail(ACLPTI_ERROR_INVALID_RAW_DATA, primary);
    }
    if (primary) {
        replay.stats.receivedBytes += raw->chunkSize;
    }
    if (raw->deviceId < 0) {
        return fail(ACLPTI_ERROR_INVALID_RAW_DATA, primary);
    }
    if (replay.deviceId >= 0 && replay.deviceId != raw->deviceId) {
        return fail(ACLPTI_ERROR_INVALID_RAW_DATA, primary);
    }
    try {
        if (core || raw->type == PC_SAMPLING_DATA_TYPE) {
            const auto status = ReceiveDataChunk(*raw, core);
            if (status != ACLPTI_SUCCESS) {
                return fail(status, primary);
            }
        } else {
            // Each complete fixed-size record is independent; partial acceptance remains in stats.
            for (size_t offset = 0; offset < raw->chunkSize; offset += recordSize) {
                RawRecord record;
                record.size = recordSize;
                record.recordIndex = replay.nextRecordIndex;
                record.events = replay.info.pmuEventIds;
                if (memcpy_s(record.bytes.data(), record.bytes.size(), raw->chunk + offset, recordSize) != EOK) {
                    return fail(ACLPTI_ERROR_INVALID_RAW_DATA, primary);
                }
                if (!Submit(DataItem{replay.info.replayId, std::move(record)})) {
                    return fail(ACLPTI_ERROR_INVALID_STATE, primary);
                }
                replay.deviceId = raw->deviceId;
                ++replay.nextRecordIndex;
                ++replay.callbackStats.copiedRecordCount;
                replay.callbackStats.copiedBytes += recordSize;
                if (primary) {
                    replay.stats.acceptedBytes += recordSize;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(ACLPTI_ERROR_OUT_OF_MEMORY, primary);
    }
    replay.callbackStats.receivedBytes += raw->chunkSize;
    LogRawData(replay.info.replayId, *raw, core.has_value(), replay.stats.acceptedBytes);
    return ACLPTI_SUCCESS;
}
} // namespace aclpti::data
