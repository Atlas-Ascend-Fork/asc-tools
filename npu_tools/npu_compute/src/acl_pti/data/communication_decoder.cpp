/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "communication_decoder.h"
#include "communication_protocol.h"
#include "raw_data_decoder.h"
#include "securec.h"
#include <algorithm>
#include <array>
#include <limits>
namespace aclpti::communication {
namespace {
using namespace protocol;
bool DecodeHccl(const uint8_t* p, Record& r)
{
    constexpr size_t itemIdOffset = 0, cclTagOffset = 8;
    constexpr size_t groupNameOffset = 16, localRankOffset = 24;
    constexpr size_t remoteRankOffset = 28, rankSizeOffset = 32;
    constexpr size_t stageOffset = 36, notifyIdOffset = 40;
    constexpr size_t startTimestampOffset = 48, durationOffset = 56;
    constexpr size_t srcAddrOffset = 64, dstAddrOffset = 72;
    constexpr size_t dataSizeOffset = 80, taskIdOffset = 88;
    constexpr size_t sqIdOffset = 96, planeIdOffset = 98;
    constexpr size_t opTypeOffset = 100, dataTypeOffset = 101;
    constexpr size_t linkTypeOffset = 102, transportTypeOffset = 103;
    constexpr size_t rdmaTypeOffset = 104, roleOffset = 105;
    constexpr size_t workflowOffset = 106;
    constexpr size_t reserveOffset = 92, reservesOffset = 107;

    r.kind = RecordKind::HcclTask;
    r.itemId = Read<uint64_t>(p, itemIdOffset);
    r.cclTag = Read<uint64_t>(p, cclTagOffset);
    r.groupName = Read<uint64_t>(p, groupNameOffset);
    r.rankId = Read<uint32_t>(p, localRankOffset);
    r.remoteRank = Read<uint32_t>(p, remoteRankOffset);
    r.rankSize = Read<uint32_t>(p, rankSizeOffset);
    r.stage = Read<uint32_t>(p, stageOffset);
    r.notifyId = Read<uint64_t>(p, notifyIdOffset);
    r.times[Record::kHcclStartTimestamp] = Read<uint64_t>(p, startTimestampOffset);
    const uint64_t durationBits = Read<uint64_t>(p, durationOffset);
    static_assert(sizeof(double) == sizeof(durationBits));
    if (memcpy_s(&r.durationEstimated, sizeof(r.durationEstimated), &durationBits, sizeof(durationBits)) != EOK) {
        return false;
    }
    r.srcAddr = Read<uint64_t>(p, srcAddrOffset);
    r.dstAddr = Read<uint64_t>(p, dstAddrOffset);
    r.dataLen = Read<uint64_t>(p, dataSizeOffset);
    r.taskId = Read<uint32_t>(p, taskIdOffset);
    r.reserve = Read<uint32_t>(p, reserveOffset);
    r.streamId = Read<uint16_t>(p, sqIdOffset);
    r.planeId = Read<uint16_t>(p, planeIdOffset);
    r.opType = p[opTypeOffset];
    r.dataType = p[dataTypeOffset];
    r.linkType = p[linkTypeOffset];
    r.transportType = p[transportTypeOffset];
    r.rdmaType = p[rdmaTypeOffset];
    r.role = p[roleOffset];
    r.workFlowMode = p[workflowOffset];
    auto& reserves = r.reserves.emplace();
    std::copy_n(p + reservesOffset, reserves.size(), reserves.begin());
    return true;
}
bool DecodeLegacyHccl(const uint8_t* p, Record& r)
{
    constexpr size_t itemIdOffset = 0, cclTagOffset = 8, groupNameOffset = 16;
    constexpr size_t localRankOffset = 24, remoteRankOffset = 28, rankSizeOffset = 32;
    constexpr size_t workflowOffset = 36, planeIdOffset = 40, contextIdOffset = 44;
    constexpr size_t notifyIdOffset = 48, stageOffset = 56, roleOffset = 60, durationOffset = 64;
    constexpr size_t srcAddrOffset = 72, dstAddrOffset = 80, dataSizeOffset = 88;
    constexpr size_t opTypeOffset = 96, dataTypeOffset = 100, linkTypeOffset = 104;
    constexpr size_t transportTypeOffset = 108, rdmaTypeOffset = 112, taskIdOffset = 116, sqIdOffset = 120;

    r.kind = RecordKind::HcclTask;
    r.itemId = Read<uint64_t>(p, itemIdOffset);
    r.cclTag = Read<uint64_t>(p, cclTagOffset);
    r.groupName = Read<uint64_t>(p, groupNameOffset);
    r.rankId = Read<uint32_t>(p, localRankOffset);
    r.remoteRank = Read<uint32_t>(p, remoteRankOffset);
    r.rankSize = Read<uint32_t>(p, rankSizeOffset);
    r.workFlowMode = Read<uint32_t>(p, workflowOffset);
    r.planeId = Read<uint32_t>(p, planeIdOffset);
    r.ctxId = Read<uint32_t>(p, contextIdOffset);
    r.notifyId = Read<uint64_t>(p, notifyIdOffset);
    r.stage = Read<uint32_t>(p, stageOffset);
    r.role = Read<uint32_t>(p, roleOffset);
    const uint64_t durationBits = Read<uint64_t>(p, durationOffset);
    if (memcpy_s(&r.durationEstimated, sizeof(r.durationEstimated), &durationBits, sizeof(durationBits)) != EOK) {
        return false;
    }
    r.srcAddr = Read<uint64_t>(p, srcAddrOffset);
    r.dstAddr = Read<uint64_t>(p, dstAddrOffset);
    r.dataLen = Read<uint64_t>(p, dataSizeOffset);
    r.opType = Read<uint32_t>(p, opTypeOffset);
    r.dataType = Read<uint32_t>(p, dataTypeOffset);
    r.linkType = Read<uint32_t>(p, linkTypeOffset);
    r.transportType = Read<uint32_t>(p, transportTypeOffset);
    r.rdmaType = Read<uint32_t>(p, rdmaTypeOffset);
    r.taskId = Read<uint32_t>(p, taskIdOffset);
    r.streamId = Read<uint16_t>(p, sqIdOffset);
    return true;
}
void DecodeNode(const uint8_t* p, Record& r)
{
    constexpr size_t streamIdOffset = 0, taskIdOffset = 2, timestampsOffset = 8;
    constexpr size_t threadIdOffset = 64, deviceIdOffset = 68, nodeTicksOffset = 72;
    constexpr size_t kernelTypeOffset = 104, dispatchTimeOffset = 108, totalTimeOffset = 112;
    constexpr size_t fftsThreadIdOffset = 116, versionOffset = 118;

    r.kind = RecordKind::Node;
    r.streamId = Read<uint16_t>(p, streamIdOffset);
    r.taskId = Read<uint16_t>(p, taskIdOffset);
    for (size_t i = 0; i < r.times.size(); ++i) {
        r.times[i] = Read<uint64_t>(p, timestampsOffset + i * sizeof(uint64_t));
    }
    r.threadId = Read<uint32_t>(p, threadIdOffset);
    r.payloadDeviceId = Read<uint32_t>(p, deviceIdOffset);
    for (size_t i = 0; i < r.nodeTicks.size(); ++i) {
        r.nodeTicks[i] = Read<uint64_t>(p, nodeTicksOffset + i * sizeof(uint64_t));
    }
    r.kernelType = Read<uint32_t>(p, kernelTypeOffset);
    r.dispatchTime = Read<uint32_t>(p, dispatchTimeOffset);
    r.totalTime = Read<uint32_t>(p, totalTimeOffset);
    r.fftsThreadId = Read<uint16_t>(p, fftsThreadIdOffset);
    r.version = p[versionOffset];
}
void DecodeCcuInfo(const uint8_t* p, Record& r)
{
    constexpr size_t versionOffset = 0, itemIdOffset = 8, groupNameOffset = 16;
    constexpr size_t rankIdOffset = 24, rankSizeOffset = 28, taskIdOffset = 36, dieIdOffset = 40, instrIdOffset = 42;
    constexpr size_t taskWorkflowOffset = 1, taskStreamOffset = 32, taskMissionOffset = 41;
    constexpr size_t instrWorkflowOffset = 32, instrStreamOffset = 34, instrMissionOffset = 44;
    constexpr size_t reduceOpOffset = 45, inputTypeOffset = 46, outputTypeOffset = 47, dataSizeOffset = 48;
    constexpr size_t ckeIdOffset = 48, maskOffset = 52, channelsOffset = 56, remoteRanksOffset = 88;

    r.version = p[versionOffset];
    r.itemId = Read<uint64_t>(p, itemIdOffset);
    r.groupName = Read<uint64_t>(p, groupNameOffset);
    r.rankId = Read<uint32_t>(p, rankIdOffset);
    r.rankSize = Read<uint32_t>(p, rankSizeOffset);
    r.taskId = Read<uint32_t>(p, taskIdOffset);
    r.dieId = p[dieIdOffset];
    r.instrId = Read<uint16_t>(p, instrIdOffset);
    if (r.type == kCcuTaskInfo) {
        r.kind = RecordKind::CcuTask;
        r.workFlowMode = p[taskWorkflowOffset];
        r.streamId = Read<uint16_t>(p, taskStreamOffset);
        r.missionId = p[taskMissionOffset];
        return;
    }
    r.workFlowMode = p[instrWorkflowOffset];
    r.streamId = Read<uint16_t>(p, instrStreamOffset);
    r.missionId = p[instrMissionOffset];
    if (r.type == kCcuGroupInfo) {
        r.kind = RecordKind::CcuGroup;
        r.opType = p[reduceOpOffset];
        r.dataType = p[inputTypeOffset];
        r.outputDataType = p[outputTypeOffset];
        r.dataLen = Read<uint64_t>(p, dataSizeOffset);
    } else {
        r.kind = RecordKind::CcuWait;
        r.ckeId = Read<uint32_t>(p, ckeIdOffset);
        r.mask = Read<uint32_t>(p, maskOffset);
    }
    static_assert(std::tuple_size_v<decltype(r.channelIds)> == std::tuple_size_v<decltype(r.remoteRanks)>);
    for (size_t i = 0; i < r.channelIds.size(); ++i) {
        r.channelIds[i] = Read<uint16_t>(p, channelsOffset + i * sizeof(uint16_t));
        r.remoteRanks[i] = Read<uint32_t>(p, remoteRanksOffset + i * sizeof(uint32_t));
    }
}
bool DecodeCcuChannels(const uint8_t* bytes, size_t size, const MsprofRawData& raw, std::vector<Record>& records)
{
    constexpr uint16_t firstTailChannel = 120, paddedTailChannel = 124, channelsPerGroup = 12;
    constexpr size_t channelWords = 5, lastChannelWords = 9, tailPaddingWords = 6;
    constexpr size_t wideTailChannelWords = 11, timestampOffset = 0;
    constexpr size_t averageOffset = sizeof(uint64_t);
    constexpr std::array<size_t, kCcuChannelCount - firstTailChannel> tailWidths{
        wideTailChannelWords, channelWords, channelWords, wideTailChannelWords,
        wideTailChannelWords, channelWords, channelWords, wideTailChannelWords};
    if (bytes == nullptr || size != kCcuChannelBytes) {
        return false;
    }
    size_t wordOffset = 0;
    for (uint16_t channel = 0; channel < kCcuChannelCount; ++channel) {
        // The v6.1 statistics packet stores 32-bit words with gaps between channel groups.
        const bool padded = channel == firstTailChannel || channel == paddedTailChannel;
        const size_t offset = (wordOffset + (padded ? tailPaddingWords : 0)) * sizeof(uint32_t);
        if (offset > size || averageOffset + sizeof(uint32_t) > size - offset) {
            return false;
        }
        const auto* data = bytes + offset;
        const auto timestamp = Read<uint64_t>(data, timestampOffset);
        const auto average = Read<uint32_t>(data, averageOffset);
        if (timestamp != 0 && average != 0) {
            Record record;
            record.kind = RecordKind::CcuChannel;
            record.deviceId = raw.deviceId;
            record.rawType = kCcuChannel;
            record.dieId = static_cast<uint32_t>(raw.chunkModule);
            record.channelId = channel;
            record.timestamp = timestamp;
            record.channelAverage = average;
            records.push_back(record);
        }
        if (channel < firstTailChannel) {
            const bool lastInGroup = (channel + 1) % channelsPerGroup == 0;
            wordOffset += lastInGroup ? lastChannelWords : channelWords;
        } else {
            wordOffset += tailWidths[channel - firstTailChannel];
        }
    }
    return true;
}

void DecodeMission(const uint8_t* p, Record& r)
{
    // Packed CCU Mission offsets, including unaligned loop timestamps.
    constexpr size_t relativeCountersOffset = 0, waitStartOffset = 32, setckeInstrIdOffset = 40;
    constexpr size_t loopEndOffset = 42, loopStartOffset = 50, loopInstrIdOffset = 58, taskIdOffset = 60;
    using Counter = Record::CcuCounterIndex;

    r.kind = RecordKind::CcuMission;
    r.streamId = kUnknownStreamId;
    r.times[Counter::loopStart] = Read<uint64_t>(p, loopStartOffset);
    r.times[Counter::loopEnd] = Read<uint64_t>(p, loopEndOffset);
    r.times[Counter::waitStart] = Read<uint64_t>(p, waitStartOffset);
    r.instrId = Read<uint16_t>(p, loopInstrIdOffset);
    r.setckeInstrId = Read<uint16_t>(p, setckeInstrIdOffset);
    r.taskId = Read<uint32_t>(p, taskIdOffset);
    for (size_t i = 0; i < r.relative.size(); ++i) {
        r.relative[i] = Read<uint16_t>(p, relativeCountersOffset + i * sizeof(uint16_t));
    }
}
size_t PayloadSize(int32_t rawType, uint32_t level, uint32_t type)
{
    constexpr size_t aicpuNodeBytes = 128, legacyHcclTaskBytes = 128;
    constexpr size_t ccuTaskInfoBytes = 48, ccuInstructionInfoBytes = 152;
    if (rawType == kAicpu && level == kAicpuLevel) {
        switch (type) {
            case kAicpuNode:
                return aicpuNodeBytes;
            case kAicpuLegacyHcclTask:
                return legacyHcclTaskBytes;
            case kAicpuHcclTask:
                return kHcclTaskBytes;
            default:
                return 0;
        }
    }
    if (rawType == kAdditional && level == kHcclLevel) {
        if (type == kCcuTaskInfo) {
            return ccuTaskInfoBytes;
        }
        if (type == kCcuWaitSignalInfo || type == kCcuGroupInfo) {
            return ccuInstructionInfoBytes;
        }
    }
    return 0;
}
} // namespace
bool Decoder::Fail(const char* message)
{
    error_ = message;
    return false;
}
bool Decoder::Decode(
    const uint8_t* bytes, size_t size, int32_t rawType, const MsprofRawData& raw, std::vector<Record>& records)
{
    Record r{};
    r.deviceId = raw.deviceId;
    r.rawType = static_cast<uint32_t>(rawType);
    if (rawType == kCcu) {
        r.dieId = static_cast<uint32_t>(raw.chunkModule);
        DecodeMission(bytes, r);
    } else if (rawType == kCcuChannel) {
        if (!DecodeCcuChannels(bytes, size, raw, records)) {
            return Fail("Invalid CCU channel statistics record");
        }
        return true;
    } else if (rawType == kTaskLog) {
        data::TaskLog32 task{};
        // Borrow the byte representation for synchronous decoding only; no aligned struct cast.
        if (data::DecodeTaskLog(reinterpret_cast<const std::byte*>(bytes), size, task) != ACLPTI_SUCCESS) {
            return Fail("Invalid task log magic");
        }
        if (!task.IsTaskStart() && !task.IsTaskEnd()) {
            return true;
        }
        r.kind = task.IsTaskStart() ? RecordKind::TaskStart : RecordKind::TaskEnd;
        r.functionType = task.funcType;
        r.taskType = task.sqeType;
        // Chip6 carries a complete task identity here, not an independent stream field.
        r.streamId = kUnknownStreamId;
        r.taskId = task.fullTaskId;
        r.times[Record::kTaskSystemCounter] = task.systemCounter;
        r.fusionType = task.fusionType;
        r.missionId = task.missionId;
    } else if (rawType == kCompact) {
        return DecodeCompactRecord(bytes, size, r, records);
    } else {
        return DecodeAdditionalRecord(bytes, size, r, records);
    }
    records.push_back(r);
    return true;
}

bool Decoder::DecodeCompactRecord(const uint8_t* bytes, size_t size, Record r, std::vector<Record>& records)
{
    if (!IsRuntimeTaskTrack(bytes, size)) {
        return true;
    }
    RuntimeTaskTrack track{};
    if (!DecodeRuntimeTaskTrack(bytes, size, track)) {
        return Fail("Invalid Runtime TaskTrack header");
    }
    if (track.taskType != kRuntimeCcuLaunch) {
        return true;
    }
    if (!HasRuntimeTaskIdentity(track) || (r.deviceId != kHostDeviceId && r.deviceId != track.deviceId)) {
        return Fail("Invalid CCU Runtime TaskTrack identity");
    }
    r.level = kRuntimeLevel;
    r.type = kRuntimeTaskTrackType;
    r.deviceId = track.deviceId;
    r.streamId = track.streamId;
    r.taskId = track.taskId;
    r.threadId = Read<uint32_t>(bytes, kThreadIdOffset);
    r.timestamp = Read<uint64_t>(bytes, kTimestampOffset);
    r.taskType = kRuntimeCcuLaunch;
    r.kind = RecordKind::RuntimeTaskTrack;
    records.push_back(r);
    return true;
}

bool Decoder::DecodeAdditionalRecord(const uint8_t* bytes, size_t size, Record r, std::vector<Record>& records)
{
    if (bytes == nullptr || size != kAdditionalBytes) {
        return Fail("Incomplete AdditionalInfo record");
    }
    r.level = Read<uint16_t>(bytes, kLevelOffset);
    r.type = Read<uint32_t>(bytes, kTypeOffset);
    const size_t required = PayloadSize(static_cast<int32_t>(r.rawType), r.level, r.type);
    if (required == 0) {
        return true;
    }
    if (protocol::Read<uint16_t>(bytes, kMagicOffset) != protocol::kMagic) {
        return Fail("Invalid AdditionalInfo magic");
    }
    r.threadId = protocol::Read<uint32_t>(bytes, kThreadIdOffset);
    r.timestamp = protocol::Read<uint64_t>(bytes, kTimestampOffset);
    const auto length = Read<uint32_t>(bytes, kPayloadLengthOffset);
    if (length > protocol::kPayloadBytes) {
        return Fail("AdditionalInfo payload exceeds record");
    }
    const bool batch = r.level == protocol::kAicpuLevel && r.type == kAicpuHcclTask;
    if (length == 0 || (batch ? length % required != 0 : length != required)) {
        return Fail("Invalid communication payload length");
    }
    const uint8_t* payload = bytes + protocol::kHeaderBytes;
    if (r.level == kHcclLevel) {
        DecodeCcuInfo(payload, r);
    } else if (r.type == kAicpuNode) {
        DecodeNode(payload, r);
    } else if (r.type == kAicpuLegacyHcclTask) {
        if (!DecodeLegacyHccl(payload, r)) {
            return Fail("Cannot copy legacy HCCL duration");
        }
    } else {
        for (size_t offset = 0; offset < length; offset += required) {
            if (!DecodeHccl(payload + offset, r)) {
                return Fail("Cannot copy HCCL duration");
            }
            records.push_back(r);
        }
        return true;
    }
    records.push_back(r);
    return true;
}

bool Decoder::DecodeAdditionalPackage(const Stream& stream, const MsprofRawData& raw, std::vector<Record>& records)
{
    using namespace protocol;
    for (size_t at = 0; at < stream.bytes.size(); at += kAdditionalBytes) {
        const size_t remaining = stream.bytes.size() - at;
        const auto* bytes = stream.bytes.data() + at;
        // A shorter payload cannot identify a whitelisted level/type.
        if (remaining < kAdditionalTypeEnd) {
            return true;
        }
        const bool wanted = IsCcuAdditional(Read<uint16_t>(bytes, kLevelOffset), Read<uint32_t>(bytes, kTypeOffset));
        if (!wanted) {
            // Dictionary packages are text, not an array of 256-byte records.
            if (Read<uint16_t>(bytes, kMagicOffset) != kMagic) {
                return true;
            }
            continue;
        }
        if (remaining < kAdditionalBytes) {
            return Fail("Incomplete CCU AdditionalInfo record");
        }
        if (!Decode(bytes, kAdditionalBytes, kAdditional, raw, records)) {
            return false;
        }
    }
    return true;
}

bool Decoder::ReadRawType(const MsprofRawData& raw, int32_t& type)
{
    // An older SDK may lack some wire IDs. Copy the representation without reading an out-of-range enum.
    static_assert(sizeof(raw.type) == sizeof(type));
    if (memcpy_s(&type, sizeof(type), &raw.type, sizeof(raw.type)) != EOK) {
        return Fail("Cannot copy communication raw type");
    }
    return true;
}

Decoder::Stream* Decoder::PrepareStream(const MsprofRawData& raw, int32_t type)
{
    if (raw.deviceId < 0 || raw.chunkModule < 0) {
        Fail("Invalid raw device or module");
        return nullptr;
    }
    const auto key = std::make_tuple(raw.deviceId, type, raw.chunkModule);
    if (streams_.find(key) == streams_.end() && streams_.size() >= kMaxStreams) {
        Fail("Too many raw streams");
        return nullptr;
    }
    return &streams_[key];
}

bool Decoder::BufferRaw(Stream& stream, const MsprofRawData& raw)
{
    if (raw.offset != stream.offset)
        return Fail("Discontinuous raw package offset");
    if (raw.chunkSize > std::numeric_limits<size_t>::max() - raw.offset)
        return Fail("Raw package offset overflow");
    if (raw.chunkSize > kMaxBufferedBytes - stream.bytes.size())
        return Fail("Raw buffering limit exceeded");
    for (size_t i = 0; i < raw.chunkSize; ++i)
        stream.bytes.push_back(static_cast<uint8_t>(raw.chunk[i]));
    stream.offset = raw.isLastChunk ? 0 : raw.offset + raw.chunkSize;
    stream.open = !raw.isLastChunk;
    return true;
}

bool Decoder::DecodeBuffered(Stream& stream, int32_t type, const MsprofRawData& raw, std::vector<Record>& records)
{
    using namespace protocol;
    size_t width = kAdditionalBytes;
    switch (type) {
        case kTaskLog:
            width = kTaskLogBytes;
            break;
        case kCcu:
            width = kMissionBytes;
            break;
        case kCompact:
            width = kCompactBytes;
            break;
        case kCcuChannel:
            width = kCcuChannelBytes;
            break;
        default:
            break;
    }
    size_t consumed = 0;
    while (stream.bytes.size() - consumed >= width) {
        if (!Decode(stream.bytes.data() + consumed, width, type, raw, records))
            return false;
        consumed += width;
    }
    stream.bytes.erase(stream.bytes.begin(), stream.bytes.begin() + consumed);
    return true;
}

bool Decoder::Feed(const MsprofRawData& raw, std::vector<Record>& records)
{
    using namespace protocol;
    if (!error_.empty())
        return false;
    int32_t type;
    if (!ReadRawType(raw, type))
        return false;
    if (!IsSupportedRawType(type))
        return true;
    if (raw.chunkSize > sizeof(raw.chunk))
        return Fail("Raw chunk exceeds callback storage");
    auto* stream = PrepareStream(raw, type);
    if (stream == nullptr)
        return false;
    if (!BufferRaw(*stream, raw))
        return false;
    if (type == kAdditional)
        return FeedAdditional(*stream, raw, records);
    return DecodeBuffered(*stream, type, raw, records);
}
bool Decoder::FeedAdditional(Stream& stream, const MsprofRawData& raw, std::vector<Record>& records)
{
    // Keep the package boundary: text dictionaries and binary AdditionalInfo
    // use the same raw type, and text lines can cross callback slices.
    if (!stream.ignoredAdditionalPackage && stream.bytes.size() >= kAdditionalTypeEnd &&
        Read<uint16_t>(stream.bytes.data(), kMagicOffset) != kMagic &&
        !IsCcuAdditional(
            Read<uint16_t>(stream.bytes.data(), kLevelOffset), Read<uint32_t>(stream.bytes.data(), kTypeOffset))) {
        stream.ignoredAdditionalPackage = true;
    }
    if (stream.ignoredAdditionalPackage) {
        stream.bytes.clear();
        stream.open = false;
    }
    if (raw.isLastChunk) {
        if (!stream.ignoredAdditionalPackage && !DecodeAdditionalPackage(stream, raw, records)) {
            return false;
        }
        stream.bytes.clear();
        stream.ignoredAdditionalPackage = false;
    }
    return true;
}
bool Decoder::Finish()
{
    if (!error_.empty())
        return false;
    for (const auto& entry : streams_) {
        if (entry.second.open || !entry.second.bytes.empty())
            return Fail("Incomplete raw stream at session end");
    }
    return true;
}
} // namespace aclpti::communication
