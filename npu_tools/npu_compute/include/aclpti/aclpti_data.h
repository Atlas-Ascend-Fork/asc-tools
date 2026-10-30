/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_DATA_H
#define NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_DATA_H

#include "aclpti/aclpti_export.h"
#include "aclpti/aclpti_types.h"

#ifdef __cplusplus

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <set>
#include <string>
#include <vector>

enum aclptiCoreType : uint8_t {
    ACLPTI_CORE_TYPE_AIC = 0,
    ACLPTI_CORE_TYPE_AIV = 1,
};

struct aclptiTaskPmuCoreData {
    double totalCycles = 0;
    bool overflow = false;
    std::map<uint32_t, double> values;
};

// One original Task, with independent AIC/AIV counters merged across successful replays.
struct aclptiTaskPmuData {
    std::optional<aclptiTaskPmuCoreData> aic;
    std::optional<aclptiTaskPmuCoreData> aiv;
};

struct aclptiPmuDataRow {
    struct SystemCounter {
        uint64_t taskStartSystemCounter;
        uint64_t taskEndSystemCounter;
    };

    struct CoreInfo {
        aclptiCoreType coreType;
        uint8_t coreId;
        uint64_t count;
    };

    struct CoreData {
        aclptiCoreType coreType;
        uint8_t coreId;
        uint64_t sampleCount = 0;
        double totalCycles = 0.0;
        bool overflow = false;
        std::map<uint32_t, double> values;
        std::map<uint32_t, uint64_t> valueCounts;
        std::vector<SystemCounter> systemCounters;
    };

    uint16_t blockId;
    uint16_t subBlockId;
    aclptiCoreType coreType;
    uint8_t coreId;
    std::vector<CoreInfo> coreInfos;
    double totalCycles = 0.0;
    bool overflow = false;
    std::map<uint32_t, double> values;
    std::vector<CoreData> coreData;
    std::vector<SystemCounter> systemCounters;
};

struct aclptiBlockKey {
    uint16_t blockId = 0;
    uint16_t subBlockId = 0;
    aclptiCoreType coreType = ACLPTI_CORE_TYPE_AIC;
    uint8_t coreId = 0;

    bool operator<(const aclptiBlockKey& other) const
    {
        if (blockId != other.blockId) {
            return blockId < other.blockId;
        }
        if (subBlockId != other.subBlockId) {
            return subBlockId < other.subBlockId;
        }
        if (coreType != other.coreType) {
            return coreType < other.coreType;
        }
        return coreId < other.coreId;
    }

    bool operator==(const aclptiBlockKey& other) const
    {
        return blockId == other.blockId && subBlockId == other.subBlockId && coreType == other.coreType &&
               coreId == other.coreId;
    }
};

struct aclptiTaskLogRow {
    uint64_t replayId;
    uint8_t funcType;
    uint16_t taskId;
    uint16_t rtStreamId;
    uint64_t systemCounter;
    uint16_t blockId;
    uint16_t subBlockId;
    aclptiCoreType coreType;
    uint8_t coreTypeId;
};

struct aclptiRawDataChunk {
    uint64_t replayId;
    int32_t deviceId;
    int32_t chunkModule;
    size_t offset;
    bool isLastChunk;
    std::vector<uint8_t> bytes;
};

enum class aclptiBiuCoreKind : uint8_t { Aic, Aiv0, Aiv1 };
enum class aclptiBiuFormat : uint8_t { Unknown = 0, Chip6 = 1 };

struct aclptiPipelineKey {
    uint8_t groupId = 0;
    aclptiBiuCoreKind coreKind = aclptiBiuCoreKind::Aic;

    bool operator<(const aclptiPipelineKey& other) const
    {
        return std::tie(groupId, coreKind) < std::tie(other.groupId, other.coreKind);
    }
};

struct aclptiReplayStats {
    uint64_t receivedBytes = 0;
    uint64_t acceptedBytes = 0;
    uint64_t rejectedChunkCount = 0;
    uint64_t failedRecordCount = 0;
};

// One BIU capture stream; replay ID is the key in the complete profiling result.
struct aclptiPipelineData {
    int32_t deviceId = -1;
    aclptiResult status = ACLPTI_SUCCESS;
    aclptiReplayStats stats;
    aclptiBiuFormat format = aclptiBiuFormat::Unknown;
    std::map<aclptiPipelineKey, std::vector<uint32_t>> channels;
};

// The probe displacement is filled after stream synchronization, before result delivery.
// Aliasing shared pointers retain ELF bytes and original instruction tables after binary unload.
struct aclptiPmSamplingContext {
    std::shared_ptr<const std::vector<char>> originalElf;
    std::shared_ptr<const std::vector<char>> companionElf;
    std::shared_ptr<const std::map<uint64_t, uint64_t>> businessInstructions;
    std::shared_ptr<const std::set<uint64_t>> simtInstructions;
    std::string instructionMapError;
    std::string kernelName;
    int32_t deviceId = -1;
    uint64_t elfEntry = 0; // Companion ELF address of the launched function.
    uint64_t aicRuntimeEntry = 0;
    uint64_t aivRuntimeEntry = 0;
    uint64_t originalEntry = 0;
    uint64_t pcOffset = 0; // Probe-reported displacement from sampled code to the original kernel.
};

// Complete collection for one original launch, including all of its profiling replays.
// PMU events are merged before delivery; distinct original launches must never share a result.
struct aclptiProfilingDataResult {
    aclptiResult status = ACLPTI_SUCCESS;
    std::map<uint16_t, std::vector<aclptiTaskLogRow>> taskLogs;
    std::map<aclptiBlockKey, std::vector<aclptiTaskLogRow>> blockLogs;
    std::map<aclptiBlockKey, aclptiPmuDataRow> pmuLogs;
    aclptiTaskPmuData taskPmuData;
    std::map<uint64_t, aclptiPipelineData> pipelineData;
    std::vector<aclptiRawDataChunk> pcSamplingData;
    std::map<uint64_t, std::shared_ptr<const aclptiPmSamplingContext>> pmSamplingContexts;
    aclptiReplayStats stats; // Totals across the complete profiling collection.
    struct ErrorStats {
        uint64_t failedRecordCount = 0;
        std::map<uint64_t, uint64_t> failedRecordCountByReplay;
    } errorStats;
};

using aclptiProfilingDataCallback = std::function<aclptiResult(std::shared_ptr<const aclptiProfilingDataResult>)>;

ACLPTI_EXPORT aclptiResult aclptiRegisterProfilingDataCallback(aclptiProfilingDataCallback callback);

using aclptiDataModuleShutdownCallback = aclptiResult (*)(void* userData);

extern "C" ACLPTI_EXPORT aclptiResult
aclptiRegisterDataModuleShutdownCallback(aclptiDataModuleShutdownCallback callback, void* userData);

#endif // __cplusplus

#endif // NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_DATA_H
