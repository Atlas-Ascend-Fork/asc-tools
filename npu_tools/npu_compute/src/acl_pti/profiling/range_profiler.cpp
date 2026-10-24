/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "range_profiler.h"
#include <memory>
#include "binary_registry.h"
#include "acl_pti/data/pmu_events.h"

#include "common/debug_log.h"
#include "profiling/prof_api.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <new>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace aclpti::profiling {
namespace {

void TryFlushL2Cache(std::int32_t device, std::size_t round)
{
    using FlushFunction = aclError (*)(void*);
    struct FlushApi {
        aclError status;
        FlushFunction function;
    };
    static const FlushApi api = [] {
        aclrtApiFunc original = nullptr;
        const auto status = aclrtApiInjectionGetFunc("aclrtDeviceL2CacheFlush", &original, nullptr);
        return FlushApi{status, reinterpret_cast<FlushFunction>(original)};
    }();
    if (api.status != ACL_SUCCESS || api.function == nullptr) {
        return;
    }
    const aclError status = api.function(nullptr);
    if (status != ACL_SUCCESS) {
        std::fprintf(
            stderr,
            "[WARNING] cache-control=all device=%d round=%zu api=aclrtDeviceL2CacheFlush "
            "status=%d L2 flush failed; continue profiling; cache state is not guaranteed\n",
            device, round, status);
    }
}

struct SectionDefinition {
    std::string_view name;
    const uint32_t* events;
    std::size_t eventCount;
};

constexpr uint32_t ARITHMETIC_UTILIZATION[] = {
    data::pmu::CUBE_TOTAL_INSTRUCTION_EVENT,     data::pmu::CUBE_FP_INSTRUCTION_EVENT,
    data::pmu::CUBE_INT_INSTRUCTION_EVENT,       data::pmu::CUBE_FP_ACTIVE_CYCLES_EVENT,
    data::pmu::CUBE_INT_ACTIVE_CYCLES_EVENT,     data::pmu::CUBE_ACTIVE_CYCLES_EVENT,
    data::pmu::VECTOR_ACTIVE_CYCLES_EVENT,       data::pmu::SIMD_VF_ISSUE_CYCLES_EVENT,
    data::pmu::VECTOR_SFU_ACTIVE_CYCLES_EVENT,   data::pmu::SIMT_VF_ISSUE_CYCLES_EVENT,
    data::pmu::SIMD_INSTRUCTION_ISSUE_EVENT,     data::pmu::SIMT_EXU_INSTRUCTION_ISSUE_EVENT,
    data::pmu::SIMT_DVG_INSTRUCTION_ISSUE_EVENT, data::pmu::SIMT_DCU_INSTRUCTION_ISSUE_EVENT,
};
constexpr uint32_t kPipeUtilization[] = {
    0,    1,    10,  36, 52, 53, 514, 515, data::pmu::CUBE_ACTIVE_CYCLES_EVENT, data::pmu::VECTOR_ACTIVE_CYCLES_EVENT,
    1794, 1812, 1813};
constexpr uint32_t kResourceConflictRatio[] = {11, 12, 13, 14, 15, 1344, 1366, 1376, 1377, 1378, 1379};
constexpr uint32_t kMemory[] = {512,  513,  514,  515,  516,  518,  1058, 1059, 1391, 1395, 1396, 1397,
                                1398, 1400, 1404, 1407, 1408, 1792, 1794, 1799, 1801, 1804, 1806, 1815};
constexpr uint32_t kMemoryL0[] = {772, 774, 776, 778, 1795, 1797};
constexpr uint32_t kMemoryUb[] = {1058, 1059, 1393, 1394, 1397, 1398, 1407, 1408};
constexpr uint32_t kL2Cache[] = {1060, 1061, 1062, 1063, 1064, 1065, 1066, 1067, 1068, 1069, 1070, 1071};

constexpr uint32_t kMsprofCollectionType = 8;

constexpr SectionDefinition kSectionCatalog[] = {
    {"ArithmeticUtilization", ARITHMETIC_UTILIZATION, std::size(ARITHMETIC_UTILIZATION)},
    {"PipeUtilization", kPipeUtilization, std::size(kPipeUtilization)},
    {"ResourceConflictRatio", kResourceConflictRatio, std::size(kResourceConflictRatio)},
    {"Memory", kMemory, std::size(kMemory)},
    {"MemoryL0", kMemoryL0, std::size(kMemoryL0)},
    {"MemoryUB", kMemoryUb, std::size(kMemoryUb)},
    {"L2Cache", kL2Cache, std::size(kL2Cache)},
};

const SectionDefinition* FindSection(std::string_view name)
{
    for (const auto& section : kSectionCatalog) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

} // namespace

struct RangeProfiler::ReplayRound {
    data::ReplayKind kind;
    std::size_t firstPmuEvent = 0;
    std::size_t pmuEventCount = 0;
};

struct RangeProfiler::ProfilingRoundConfig {
    MsprofConfig msprof{};
    std::array<MsprofConfigAttr, 2> attrs{};
    data::ReplayPrepareInfo prepareInfo{};
};

void LogMsprofConfig(const MsprofConfig& config, std::size_t roundId)
{
    const std::size_t dumpPathLength = ::strnlen(config.dumpPath, sizeof(config.dumpPath));
    const std::size_t sampleConfigLength = ::strnlen(config.sampleConfig, sizeof(config.sampleConfig));
    npucompute::detail::DebugLog(
        "aclpti",
        "msprof config round=%zu collectionType=%u profSwitch=0x%llx devNums=%u devId[0]=%u "
        "dumpPath=\"%.*s\" dumpPathLength=%zu sampleConfig=\"%.*s\" sampleConfigLength=%zu "
        "configInfo.attrs=%p configInfo.numAttrs=%zu",
        roundId, kMsprofCollectionType, static_cast<unsigned long long>(config.profSwitch), config.devNums,
        config.devIdList[0], static_cast<int>(dumpPathLength), config.dumpPath, dumpPathLength,
        static_cast<int>(sampleConfigLength), config.sampleConfig, sampleConfigLength,
        static_cast<const void*>(config.configInfo.attrs), config.configInfo.numAttrs);

    if (config.configInfo.attrs == nullptr) {
        return;
    }
    for (std::size_t attrIndex = 0; attrIndex < config.configInfo.numAttrs; ++attrIndex) {
        const MsprofConfigAttr& attr = config.configInfo.attrs[attrIndex];
        if (attr.id == PROF_CONFIG_ATTR_AICORE_METRICS) {
            for (std::size_t slot = 0; slot < COMPUTE_AICORE_METRICS_NUM; ++slot) {
                npucompute::detail::DebugLog(
                    "aclpti", "msprof config round=%zu attr[%zu] id=%u aicoreMetrics[%zu]=%u", roundId, attrIndex,
                    attr.id, slot, attr.value.aicoreMetrics[slot]);
            }
        } else if (attr.id == PROF_CONFIG_ATTR_INSTR) {
            npucompute::detail::DebugLog(
                "aclpti", "msprof config round=%zu attr[%zu] id=%u instrMode=%u", roundId, attrIndex, attr.id,
                attr.value.instrMode);
        } else if (attr.id == PROF_CONFIG_ATTR_TASK_BLOCK) {
            npucompute::detail::DebugLog(
                "aclpti", "msprof config round=%zu attr[%zu] id=%u taskBlockMode=%u", roundId, attrIndex, attr.id,
                attr.value.taskBlockMode);
        } else {
            npucompute::detail::DebugLog(
                "aclpti", "msprof config round=%zu attr[%zu] id=%u", roundId, attrIndex, attr.id);
        }
    }
}

aclptiResult RangeProfiler::Initialize()
{
    const aclptiResult initializeStatus = dataManager_.Initialize();
    npucompute::detail::DebugLog(
        "aclpti", "RangeProfiler data module init result=%d", static_cast<std::int32_t>(initializeStatus));
    if (initializeStatus != ACLPTI_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "RangeProfiler data module initialization failed");
        return initializeStatus;
    }
    MsprofRawDataCallback callback = dataManager_.GetRawDataCallback();
    if (callback == nullptr) {
        npucompute::detail::DebugLog("aclpti", "RangeProfiler callback registration missing callback");
        Shutdown();
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }
    const int callbackResult = acltoolUploaderInit(callback);
    npucompute::detail::DebugLog("aclpti", "RangeProfiler callback registration result=%d", callbackResult);
    if (callbackResult == 0) {
        npucompute::detail::DebugLog("aclpti", "RangeProfiler initialized");
        return ACLPTI_SUCCESS;
    }
    Shutdown();
    return ACLPTI_ERROR_INITIALIZATION_FAILED;
}

aclptiResult RangeProfiler::Shutdown()
{
    const aclptiResult status = dataManager_.ForceShutdown();
    npucompute::detail::DebugLog(
        "aclpti", "RangeProfiler data module shutdown result=%d", static_cast<std::int32_t>(status));
    if (status != ACLPTI_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "error operation=data_shutdown status=%d", static_cast<int>(status));
    }
    return status;
}

aclptiResult RangeProfiler::SetConfig(const aclptiRangeProfilerSetConfigParams* params)
{
    if (params == nullptr || params->numSections > ACLPTI_MAX_NUM_SECTIONS ||
        (params->numSections != 0 && params->sections == nullptr) ||
        (params->blockResult != ACLPTI_BLOCK_RESULT_DISABLED && params->blockResult != ACLPTI_BLOCK_RESULT_ALL &&
         params->blockResult != ACLPTI_BLOCK_RESULT_SHRINK) ||
        (params->numSections == 0 && !params->collectPipeline && !params->collectPcSampling)) {
        npucompute::detail::DebugLog("aclpti", "profiling configuration rejected: invalid parameters");
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }

    try {
        std::vector<uint32_t> events;
        std::unordered_set<uint32_t> seen;
        for (std::size_t index = 0; index < params->numSections; ++index) {
            const char* name = params->sections[index];
            if (name == nullptr) {
                npucompute::detail::DebugLog("aclpti", "section configuration rejected: null section index=%zu", index);
                return ACLPTI_ERROR_INVALID_PARAMETER;
            }
            const std::size_t length = ::strnlen(name, ACLPTI_MAX_SECTION_NAME_LENGTH + 1);
            if (length == 0 || length > ACLPTI_MAX_SECTION_NAME_LENGTH) {
                npucompute::detail::DebugLog(
                    "aclpti", "section configuration rejected: invalid section index=%zu", index);
                return ACLPTI_ERROR_INVALID_PARAMETER;
            }

            const SectionDefinition* section = FindSection(std::string_view(name, length));
            if (section == nullptr) {
                npucompute::detail::DebugLog(
                    "aclpti", "section configuration rejected: unsupported section=%.*s", static_cast<int>(length),
                    name);
                return ACLPTI_ERROR_NOT_SUPPORTED;
            }
            npucompute::detail::DebugLog("aclpti", "selected section name=%.*s", static_cast<int>(length), name);
            for (std::size_t eventIndex = 0; eventIndex < section->eventCount; ++eventIndex) {
                const uint32_t event = section->events[eventIndex];
                if (seen.insert(event).second) {
                    events.push_back(event);
                }
            }
        }
        pmuEvents_ = std::move(events);
        blockResult_ = params->blockResult;
        collectPipeline_ = params->collectPipeline;
        if (collectPipeline_) {
            npucompute::detail::DebugLog("aclpti", "selected section name=Pipeline");
        }
        collectPcSampling_ = params->collectPcSampling;
        npucompute::detail::DebugLog(
            "aclpti", "requested sections=%zu events=%zu block=%d pipeline=%d pcSampling=%d", params->numSections,
            pmuEvents_.size(), static_cast<int>(blockResult_), collectPipeline_ ? 1 : 0, collectPcSampling_ ? 1 : 0);
        return ACLPTI_SUCCESS;
    } catch (const std::bad_alloc&) {
        npucompute::detail::DebugLog("aclpti", "section configuration failed: out of memory");
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
}

aclptiResult RangeProfiler::PrepareReplayEnvironment(
    const ReplayLaunchFunction& launchFunction, aclrtStream stream, std::int32_t* deviceId,
    aclrtSynchronizeStreamFunc* synchronizeFunction) const
{
    const aclError deviceResult = aclrtGetDevice(deviceId);
    npucompute::detail::DebugLog("aclpti", "get current device result=%d device=%d", deviceResult, *deviceId);
    if (deviceResult != ACL_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "error operation=replay_get_device status=%d", deviceResult);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }

    *synchronizeFunction =
        reinterpret_cast<aclrtSynchronizeStreamFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtSynchronizeStream));
    if (!launchFunction || *synchronizeFunction == nullptr) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=original_lookup status=%d launch_available=%d synchronize_available=%d",
            ACLPTI_ERROR_PROFILING_FAILED, static_cast<int>(static_cast<bool>(launchFunction)),
            static_cast<int>(*synchronizeFunction != nullptr));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    npucompute::detail::DebugLog("aclpti", "synchronize stream before replay");
    const int initialSyncResult = (*synchronizeFunction)(stream);
    npucompute::detail::DebugLog("aclpti", "initial replay synchronization result=%d", initialSyncResult);
    if (initialSyncResult != ACL_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "error operation=replay_initial_sync status=%d", initialSyncResult);
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }

    return ACLPTI_SUCCESS;
}

std::vector<RangeProfiler::ReplayRound> RangeProfiler::BuildReplayRounds() const
{
    std::vector<ReplayRound> rounds;
    const std::size_t pmuRoundCount = (pmuEvents_.size() + COMPUTE_AICORE_METRICS_NUM - 1) / COMPUTE_AICORE_METRICS_NUM;
    rounds.reserve(
        pmuRoundCount + static_cast<std::size_t>(collectPipeline_) + static_cast<std::size_t>(collectPcSampling_));
    for (std::size_t round = 0; round < pmuRoundCount; ++round) {
        const std::size_t firstEvent = round * COMPUTE_AICORE_METRICS_NUM;
        rounds.push_back(ReplayRound{
            data::ReplayKind::Pmu,
            firstEvent,
            std::min(pmuEvents_.size() - firstEvent, static_cast<std::size_t>(COMPUTE_AICORE_METRICS_NUM)),
        });
    }
    if (collectPipeline_) {
        rounds.push_back(ReplayRound{data::ReplayKind::Pipeline});
    }
    if (collectPcSampling_) {
        rounds.push_back(ReplayRound{data::ReplayKind::PcSampling});
    }
    return rounds;
}

void RangeProfiler::ConfigureProfilingRound(
    const ReplayRound& round, std::size_t roundId, std::int32_t deviceId, ProfilingRoundConfig* config) const
{
    *config = {};
    config->prepareInfo.replayId = roundId;
    config->prepareInfo.kind = round.kind;
    config->prepareInfo.pmuEventIds.fill(data::kInvalidPmuEvent);
    MsprofConfigAttr& primaryAttr = config->attrs[0];
    if (round.kind == data::ReplayKind::Pmu) {
        config->msprof.profSwitch = PROF_TASK_TIME_MASK | PROF_AICORE_METRICS_MASK;
        primaryAttr.id = PROF_CONFIG_ATTR_AICORE_METRICS;
        std::fill_n(primaryAttr.value.aicoreMetrics, COMPUTE_AICORE_METRICS_NUM, MSPROF_INVALID_AICORE_METRIC);
        for (std::size_t index = 0; index < round.pmuEventCount; ++index) {
            const uint32_t event = pmuEvents_[round.firstPmuEvent + index];
            primaryAttr.value.aicoreMetrics[index] = event;
            config->prepareInfo.pmuEventIds[index] = event;
        }
    } else {
        config->msprof.profSwitch = PROF_TASK_TIME_MASK | PROF_INSTR_MASK;
        primaryAttr.id = PROF_CONFIG_ATTR_INSTR;
        primaryAttr.value.instrMode =
            round.kind == data::ReplayKind::Pipeline ? PROF_COMPUTE_BIU_PERF : PROF_COMPUTE_PC_SAMPLING;
    }

    std::size_t attrCount = 1;
    if (round.kind != data::ReplayKind::Pipeline && blockResult_ != ACLPTI_BLOCK_RESULT_DISABLED) {
        MsprofConfigAttr& blockAttr = config->attrs[attrCount++];
        blockAttr.id = PROF_CONFIG_ATTR_TASK_BLOCK;
        blockAttr.value.taskBlockMode =
            blockResult_ == ACLPTI_BLOCK_RESULT_ALL ? PROF_COMPUTE_ALL_BLOCK : PROF_COMPUTE_BLOCK_SHRINK;
    }
    config->msprof.devNums = 1;
    config->msprof.devIdList[0] = static_cast<std::uint32_t>(deviceId);
    config->msprof.configInfo.attrs = config->attrs.data();
    config->msprof.configInfo.numAttrs = attrCount;

    npucompute::detail::DebugLog(
        "aclpti", "prepare replay round=%zu kind=%d pmuCount=%zu attrs=%zu", roundId, static_cast<int>(round.kind),
        round.pmuEventCount, attrCount);
    for (std::size_t index = 0; index < round.pmuEventCount; ++index) {
        npucompute::detail::DebugLog(
            "aclpti", "replay pmu round=%zu slot=%zu value=%u", roundId, index, config->prepareInfo.pmuEventIds[index]);
    }
}

aclptiResult RangeProfiler::ResolveProfilingRoundStatus(
    std::size_t round, aclError launchStatus, aclError synchronizeStatus, std::int32_t stopStatus,
    const data::ReplayResult& replayResult, aclptiResult releaseStatus) const
{
    if (launchStatus != ACL_SUCCESS) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=replay_launch status=%d replay=%llu round=%zu", launchStatus,
            static_cast<unsigned long long>(round), round);
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    if (synchronizeStatus != ACL_SUCCESS) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=replay_sync status=%d replay=%llu round=%zu", synchronizeStatus,
            static_cast<unsigned long long>(round), round);
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    if (stopStatus != 0) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=prof_stop status=%d replay=%llu round=%zu", stopStatus,
            static_cast<unsigned long long>(round), round);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (replayResult.status != ACLPTI_SUCCESS) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=replay_record status=%d replay=%llu round=%zu",
            static_cast<int>(replayResult.status), static_cast<unsigned long long>(round), round);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (releaseStatus != ACLPTI_SUCCESS) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=replay_release status=%d replay=%llu round=%zu", static_cast<int>(releaseStatus),
            static_cast<unsigned long long>(round), round);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult RangeProfiler::StartProfilingRound(
    const ReplayRound& round, std::size_t roundId, std::int32_t deviceId, ProfilingRoundConfig* config)
{
    const aclptiResult prepareStatus = dataManager_.PrepareReplay(config->prepareInfo);
    if (prepareStatus != ACLPTI_SUCCESS) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=replay_prepare status=%d replay=%llu round=%zu", static_cast<int>(prepareStatus),
            static_cast<unsigned long long>(roundId), roundId);
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }

    npucompute::detail::DebugLog(
        "aclpti", "start profiling replay round=%zu device=%d pmuCount=%zu", roundId, deviceId, round.pmuEventCount);
    LogMsprofConfig(config->msprof, roundId);
    const int startResult = MsprofStart(kMsprofCollectionType, &config->msprof, sizeof(config->msprof));
    if (startResult != 0) {
        dataManager_.RecordReplayStatus({roundId, ACLPTI_ERROR_RESULT_UNRELIABLE});
        dataManager_.ReleaseReplay(roundId);
        npucompute::detail::DebugLog(
            "aclpti", "error operation=prof_start status=%d replay=%llu round=%zu", startResult,
            static_cast<unsigned long long>(roundId), roundId);
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult RangeProfiler::FinishProfilingRound(
    std::size_t round, const ProfilingRoundConfig& config, aclError launchStatus,
    aclrtSynchronizeStreamFunc synchronizeFunction, aclrtStream stream)
{
    npucompute::detail::DebugLog("aclpti", "synchronize replay kernel round=%zu", round);
    const aclError syncResult = synchronizeFunction(stream);
    npucompute::detail::DebugLog("aclpti", "synchronize replay kernel result round=%zu result=%d", round, syncResult);
    npucompute::detail::DebugLog("aclpti", "stop profiling replay round=%zu", round);
    const int stopResult = MsprofStop(kMsprofCollectionType, &config.msprof, sizeof(config.msprof));
    npucompute::detail::DebugLog("aclpti", "stop profiling replay result round=%zu result=%d", round, stopResult);

    const aclptiResult roundStatus = launchStatus != ACL_SUCCESS || syncResult != ACL_SUCCESS ?
                                         ACLPTI_ERROR_RESULT_UNRELIABLE :
                                     stopResult != 0 ? ACLPTI_ERROR_PROFILING_FAILED :
                                                       ACLPTI_SUCCESS;
    const data::ReplayStopInfo stopInfo{round, roundStatus};
    npucompute::detail::DebugLog("aclpti", "record replay status round=%zu", round);
    const data::ReplayResult replayResult = dataManager_.RecordReplayStatus(stopInfo);
    npucompute::detail::DebugLog(
        "aclpti", "record replay status result round=%zu result=%d copiedRecords=%llu copiedBytes=%llu", round,
        static_cast<int>(replayResult.status),
        static_cast<unsigned long long>(replayResult.callbackStats.copiedRecordCount),
        static_cast<unsigned long long>(replayResult.callbackStats.copiedBytes));
    npucompute::detail::DebugLog("aclpti", "release replay round=%zu", round);
    const aclptiResult releaseStatus = dataManager_.ReleaseReplay(round);
    npucompute::detail::DebugLog(
        "aclpti", "release replay result round=%zu result=%d", round, static_cast<int>(releaseStatus));

    const aclptiResult status =
        ResolveProfilingRoundStatus(round, launchStatus, syncResult, stopResult, replayResult, releaseStatus);
    if (status == ACLPTI_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "replay round=%zu complete", round);
    }
    return status;
}

aclptiResult RangeProfiler::ReplayKernel(
    const ReplayMemory& replayMemory, BinaryRegistry& binaryRegistry, aclrtFuncHandle originalFunction,
    const ReplayLaunchFunction& launchFunction, aclrtStream stream)
{
    try {
        std::int32_t deviceId = -1;
        aclrtSynchronizeStreamFunc synchronizeFunction = nullptr;
        aclptiResult status = PrepareReplayEnvironment(launchFunction, stream, &deviceId, &synchronizeFunction);
        if (status != ACLPTI_SUCCESS) {
            return status;
        }

        const std::vector<ReplayRound> rounds = BuildReplayRounds();
        npucompute::detail::DebugLog("aclpti", "replay rounds=%zu events=%zu", rounds.size(), pmuEvents_.size());

        for (std::size_t round = 0; round < rounds.size(); ++round) {
            ProfilingRoundConfig config{};
            ConfigureProfilingRound(rounds[round], round, deviceId, &config);
            const bool pcSampling = rounds[round].kind == data::ReplayKind::PcSampling;
            bool snapshotReady = true;
            bool pmSamplingMapFailed = false;
            if (pcSampling) {
                PmSamplingBinarySnapshot snapshot;
                snapshotReady = binaryRegistry.GetPmSamplingSnapshot(originalFunction, snapshot);
                if (snapshotReady) {
                    auto context = std::make_shared<aclptiPmSamplingContext>();
                    context->originalElf = {snapshot.image, &snapshot.image->originalElf};
                    context->companionElf = {snapshot.image, &snapshot.image->companionElf};
                    context->businessInstructions = {snapshot.image, &snapshot.image->instructions.business};
                    context->excludedInstructions = {snapshot.image, &snapshot.image->instructions.excluded};
                    context->instructionMapError = snapshot.error;
                    pmSamplingMapFailed = !snapshot.error.empty();
                    context->elfEntry = snapshot.elfEntry;
                    context->aicRuntimeEntry = snapshot.aicRuntimeEntry;
                    context->aivRuntimeEntry = snapshot.aivRuntimeEntry;
                    npucompute::detail::DebugLog(
                        "aclpti", "PmSampling context replay=%llu elfEntry=0x%llx aicEntry=0x%llx aivEntry=0x%llx",
                        static_cast<unsigned long long>(config.prepareInfo.replayId),
                        static_cast<unsigned long long>(context->elfEntry),
                        static_cast<unsigned long long>(context->aicRuntimeEntry),
                        static_cast<unsigned long long>(context->aivRuntimeEntry));
                    config.prepareInfo.pmSamplingContext = std::move(context);
                }
            }
            if (pmSamplingMapFailed) {
                // Do not restore device buffers for a round that will not launch:
                // the preceding successful launch must remain visible to the APP.
                status = dataManager_.PrepareReplay(config.prepareInfo);
                if (status != ACLPTI_SUCCESS) {
                    return status;
                }
                const auto failed = dataManager_.RecordReplayStatus({round, ACLPTI_ERROR_PROFILING_FAILED});
                const auto released = dataManager_.ReleaseReplay(round);
                if (failed.status != ACLPTI_ERROR_PROFILING_FAILED) {
                    return failed.status;
                }
                // The published result carries the collection failure. The original
                // kernel succeeded, so a mapping error must not fail its launch API.
                return released;
            }
            status = replayMemory.Restore();
            if (status != ACLPTI_SUCCESS) {
                npucompute::detail::DebugLog(
                    "aclpti", "error operation=replay_restore_result status=%d round=%zu", static_cast<int>(status),
                    round);
                return status;
            }
            if (cacheControl_ == ACLPTI_CACHE_CONTROL_ALL) {
                TryFlushL2Cache(deviceId, round);
            }
            status = StartProfilingRound(rounds[round], round, deviceId, &config);
            if (status != ACLPTI_SUCCESS) {
                return status;
            }

            npucompute::detail::DebugLog("aclpti", "launch replay kernel round=%zu", round);
            const bool pipeline = rounds[round].kind == data::ReplayKind::Pipeline;
            const bool instrumented = pipeline || pcSampling;
            const auto function =
                instrumented ? binaryRegistry.FindInstrumentedFunction(originalFunction) : originalFunction;
            npucompute::detail::DebugLog(
                "aclpti", "replay function round=%zu kind=%s replay_id=%llu original=%p selected=%p device=%d", round,
                pcSampling ? "PmSampling" : (pipeline ? "pipeline" : "pmu"),
                static_cast<unsigned long long>(config.prepareInfo.replayId), originalFunction, function, deviceId);
            aclError launchStatus;
            if (instrumented && (function == nullptr || !snapshotReady)) {
                // The original launch validated originalFunction, but does not guarantee an
                // instrumented companion exists. Skip launch and let FinishProfilingRound
                // below stop and release the profiling round already started above.
                npucompute::detail::DebugLog(
                    "aclpti",
                    "error operation=instrumented_function_lookup status=%d round=%zu originalFunction=%p; "
                    "enable Pipeline/PmSampling before binary load and obtain the function by name or entry",
                    ACL_ERROR_PROFILING_FAILURE, round, originalFunction);
                launchStatus = ACL_ERROR_PROFILING_FAILURE;
            } else {
                launchStatus = launchFunction(function, instrumented);
            }
            npucompute::detail::DebugLog(
                "aclpti", "launch replay kernel result round=%zu result=%d", round, launchStatus);
            status = FinishProfilingRound(round, config, launchStatus, synchronizeFunction, stream);
            if (status != ACLPTI_SUCCESS) {
                return status;
            }
        }

        return ACLPTI_SUCCESS;
    } catch (const std::bad_alloc&) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=replay_metadata_alloc status=%d", ACLPTI_ERROR_OUT_OF_MEMORY);
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
}

} // namespace aclpti::profiling
