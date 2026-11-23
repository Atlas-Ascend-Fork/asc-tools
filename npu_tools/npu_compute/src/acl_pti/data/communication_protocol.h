/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef ACLPTI_COMMUNICATION_PROTOCOL_H
#define ACLPTI_COMMUNICATION_PROTOCOL_H
#include "raw_data_decoder.h"
#include "profiling/prof_common.h"
#include <cstddef>
#include <cstdint>
namespace aclpti::communication::protocol {
// runtime/pkg_inc/profiling/{aprof_pub,prof_common}.h; numeric IDs support older CANN SDKs.
inline constexpr int32_t kTaskLog = 7, kAicpu = 12, kCcu = 13, kCcuChannel = 14, kCompact = 16, kAdditional = 17;
inline constexpr int32_t kHostDeviceId = 64;
inline constexpr uint32_t kCcuTaskInfo = 14, kCcuWaitSignalInfo = 15, kCcuGroupInfo = 16;
inline constexpr uint32_t kAicpuNode = 0, kAicpuLegacyHcclTask = 6, kAicpuHcclTask = 13;
inline constexpr size_t kMagicOffset = 0, kLevelOffset = 2, kTypeOffset = 4;
inline constexpr size_t kThreadIdOffset = 8, kPayloadLengthOffset = 12, kTimestampOffset = 16;
inline constexpr size_t kAdditionalTypeEnd = kTypeOffset + sizeof(uint32_t);
inline constexpr uint16_t kMagic = 0x5a5a, kAicpuLevel = 6000, kHcclLevel = 5500;
inline constexpr uint16_t kAicoreLevel = 3000;
inline constexpr uint32_t kAicoreTimestampType = 0;
inline bool IsCcuAdditional(uint32_t level, uint32_t type)
{
    return level == kHcclLevel && (type == kCcuTaskInfo || type == kCcuWaitSignalInfo || type == kCcuGroupInfo);
}
inline bool IsAicoreTimestamp(uint32_t level, uint32_t type)
{
    return level == kAicoreLevel && type == kAicoreTimestampType;
}
inline bool IsCommunicationAdditional(uint32_t level, uint32_t type)
{
    return IsCcuAdditional(level, type) || IsAicoreTimestamp(level, type);
}
inline constexpr size_t kAdditionalBytes = 256, kHeaderBytes = 24, kPayloadBytes = 232;
inline constexpr size_t kCompactBytes = 64, kRuntimeTrackBytes = 40;
inline constexpr uint32_t kRuntimeLevel = 5000, kRuntimeTaskTrackType = 802, kRuntimeCcuLaunch = 30;
inline constexpr size_t kCcuChannelBytes = 2816;
inline constexpr size_t kMissionBytes = 64, kTaskLogBytes = data::TASK_LOG_BYTES, kHcclTaskBytes = 116;
inline constexpr size_t kMaxStreams = 4096, kMaxBufferedBytes = 4 * 1024 * 1024;
inline constexpr uint16_t kCcuChannelCount = 128;
inline constexpr uint32_t kRelativeScale = 4;
inline bool IsSupportedRawType(int32_t type)
{
    return type == kTaskLog || type == kAicpu || type == kCcu || type == kCcuChannel || type == kCompact ||
           type == kAdditional;
}
// Offsets are explicit little-endian wire offsets, including natural alignment padding.
template <class T>
T Read(const uint8_t* bytes, size_t offset)
{
    return data::ReadLittleEndian<T>(bytes + offset);
}
struct RuntimeTaskTrack {
    uint16_t deviceId = 0;
    uint16_t streamId = UINT16_MAX;
    uint32_t taskId = 0;
    uint64_t taskType = 0;
    uint64_t kernelNameId = 0;
};

inline bool IsRuntimeTaskTrack(const uint8_t* bytes, size_t size)
{
    return bytes != nullptr && size >= kCompactBytes && Read<uint16_t>(bytes, kLevelOffset) == kRuntimeLevel &&
           Read<uint32_t>(bytes, kTypeOffset) == kRuntimeTaskTrackType;
}

// Callers pass a complete compact record and check the envelope's device independently.
inline bool DecodeRuntimeTaskTrack(const uint8_t* bytes, size_t size, RuntimeTaskTrack& track)
{
    constexpr size_t deviceOffset = 0, streamOffset = 2, taskOffset = 4;
    constexpr size_t taskTypeOffset = 8, kernelNameOffset = 16;
    if (bytes == nullptr || size < kCompactBytes || Read<uint16_t>(bytes, kMagicOffset) != kMagic ||
        Read<uint32_t>(bytes, kPayloadLengthOffset) != kRuntimeTrackBytes) {
        return false;
    }
    const auto* payload = bytes + kHeaderBytes;
    track = {
        Read<uint16_t>(payload, deviceOffset), Read<uint16_t>(payload, streamOffset),
        Read<uint32_t>(payload, taskOffset), Read<uint64_t>(payload, taskTypeOffset),
        Read<uint64_t>(payload, kernelNameOffset)};
    return true;
}

inline bool HasRuntimeTaskIdentity(const RuntimeTaskTrack& track)
{
    return track.deviceId < kHostDeviceId && track.streamId != UINT16_MAX;
}
} // namespace aclpti::communication::protocol
#endif
