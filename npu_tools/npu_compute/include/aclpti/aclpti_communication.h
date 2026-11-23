/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef ACLPTI_COMMUNICATION_H
#define ACLPTI_COMMUNICATION_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace aclpti::communication {
inline constexpr uint32_t kUnknownStreamId = UINT32_MAX;

// Each mapping must be supplied from a real clock calibration, never callback arrival time.
// hostNsAnchor uses the same host monotonic clock for all devices and sources.
struct ClockMapping {
    uint64_t counterAnchor = 0;
    uint64_t hostNsAnchor = 0;
    uint64_t frequencyHz = 0;
    bool calibrated = false;
};
struct DeviceClocks {
    static constexpr int32_t kInvalidDeviceId = -1;
    int32_t deviceId = kInvalidDeviceId;
    ClockMapping aicpu, task, ccu;
};
enum class RecordKind {
    ComputeTurn,
    CommTurn,
    HcclTask,
    CcuMission,
    CcuTask,
    CcuGroup,
    CcuWait,
    Node,
    TaskStart,
    TaskEnd,
    LegacyTaskInfo, // Reserved for TaskInfo in older staging artifacts.
    RuntimeTaskTrack,
    CcuChannel,
    BlockStart,
    BlockEnd,
    AicoreTimestamp
};
// Owned decoded values. Times retain source units; no host/device clock conversion is implied.
struct Record {
    static constexpr std::size_t kHcclStartTimestamp = 0;
    static constexpr std::size_t kTaskSystemCounter = 0;
    struct CcuCounterIndex {
        static constexpr std::size_t loopStart = 0;
        static constexpr std::size_t loopEnd = 1;
        static constexpr std::size_t waitStart = 2;
    };
    static constexpr std::size_t kTimeSlots = 7;
    static constexpr std::size_t kCcuInstructionChannelSlots = 16;
    static constexpr std::size_t kNodeTickSlots = 4;
    static constexpr std::size_t kHcclReservedBytes = 9;

    RecordKind kind = RecordKind::Node;
    int32_t deviceId = 0;
    uint32_t streamId = 0, taskId = 0, dieId = 0, planeId = 0;
    uint32_t version = 0, turn = 0, totalTurns = 0, instrId = 0;
    // Turns: source field order. Mission: loop start/end, setcke start. Node: source seven times.
    std::array<uint64_t, kTimeSlots> times{};
    uint64_t dataLen = 0, itemId = 0, groupName = 0;
    uint32_t rankId = 0, remoteRank = 0, rankSize = 0;
    // Raw CCU relative counters; interpretation and clock conversion belong to the finalizer.
    std::array<uint16_t, kCcuInstructionChannelSlots> relative{};
    std::array<uint16_t, kCcuInstructionChannelSlots> channelIds{};
    std::array<uint32_t, kCcuInstructionChannelSlots> remoteRanks{};
    uint32_t missionId = 0, workFlowMode = 0, setckeInstrId = 0, ckeId = 0, mask = 0;
    uint32_t opType = 0, dataType = 0, outputDataType = 0, linkType = 0;
    uint32_t transportType = 0, rdmaType = 0, role = 0, stage = 0, ctxId = 0;
    uint64_t timestamp = 0, cclTag = 0, notifyId = 0, srcAddr = 0, dstAddr = 0;
    double durationEstimated = 0;
    uint32_t threadId = 0, payloadDeviceId = 0, rawType = 0, level = 0, type = 0;
    std::array<uint64_t, kNodeTickSlots> nodeTicks{};
    // Node records retain their source kernelType value.
    uint32_t kernelType = UINT32_MAX, dispatchTime = 0, totalTime = 0, fftsThreadId = 0;
    uint32_t taskType = UINT32_MAX, functionType = UINT32_MAX, fusionType = 0;
    // Timestamp records carry a core-local logical index in blockId and no task identity.
    uint32_t blockId = 0, subBlockId = 0, coreType = UINT32_MAX, coreId = 0, descId = 0;
    uint64_t curPc = 0;
    std::optional<uint32_t> reserve;
    std::optional<std::array<uint8_t, kHcclReservedBytes>> reserves;
    uint16_t channelId = UINT16_MAX;
    uint32_t channelAverage = 0;
};
} // namespace aclpti::communication
#endif
