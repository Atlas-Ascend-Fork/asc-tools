/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
/**
 * @file range_profiler.h
 * @brief Configures PMU sections and coordinates profiled kernel replay rounds.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_RANGE_PROFILER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_RANGE_PROFILER_H

#include "aclpti/aclpti_range_profiler.h"
#include "acl_pti/data/profiling_data_manager.h"
#include "injection/injection_hook.h"
#include "replay_memory.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct MsprofConfig;
struct MsprofConfigAttr;

namespace aclpti::profiling {

class BinaryRegistry;

using ReplayLaunchFunction = std::function<aclError(aclrtFuncHandle, bool)>;

class RangeProfiler {
public:
    /// Initializes raw profiling data collection and uploader callbacks.
    aclptiResult Initialize();

    /// Validates and stores the requested PMU and instruction collection configuration.
    aclptiResult SetConfig(const aclptiRangeProfilerSetConfigParams* params);

    bool CollectPipeline() const { return collectPipeline_; }
    bool CollectPcSampling() const { return collectPcSampling_; }

    /// Replays a kernel and distinguishes profiling-only failures from unreliable device results.
    aclptiResult ReplayKernel(
        const ReplayMemory& replayMemory, BinaryRegistry& binaryRegistry, aclrtFuncHandle originalFunction,
        const ReplayLaunchFunction& launchFunction, aclrtStream stream);

    /// Drains raw profiling data and stops the data module.
    aclptiResult Shutdown();

private:
    struct ReplayRound;
    struct ProfilingRoundConfig;

    /// Resolves replay dependencies and synchronizes the stream before profiling starts.
    aclptiResult PrepareReplayEnvironment(
        const ReplayLaunchFunction& launchFunction, aclrtStream stream, std::int32_t* deviceId,
        aclrtSynchronizeStreamFunc* synchronizeFunction) const;

    /// Expands the logical collection request into deterministic PMU and instruction rounds.
    std::vector<ReplayRound> BuildReplayRounds() const;

    /// Builds the Msprof and data-module configuration owned by one replay round.
    void ConfigureProfilingRound(
        const ReplayRound& round, std::size_t roundId, std::int32_t deviceId, ProfilingRoundConfig* config) const;

    /// Prepares raw-data collection and starts Msprof for one profiling round.
    aclptiResult StartProfilingRound(
        const ReplayRound& round, std::size_t roundId, std::int32_t deviceId, ProfilingRoundConfig* config);

    /// Synchronizes the replayed kernel, stops Msprof, records its data, and releases the round.
    aclptiResult FinishProfilingRound(
        std::size_t round, const ProfilingRoundConfig& config, aclError launchStatus,
        aclrtSynchronizeStreamFunc synchronizeFunction, aclrtStream stream);

    /// Maps launch, synchronization, collection, recording, and release results to an ACL PTI status.
    aclptiResult ResolveProfilingRoundStatus(
        std::size_t round, aclError launchStatus, aclError synchronizeStatus, std::int32_t stopStatus,
        const data::ReplayResult& replayResult, aclptiResult releaseStatus) const;

    std::vector<uint32_t> pmuEvents_;
    aclptiBlockResultMode blockResult_ = ACLPTI_BLOCK_RESULT_DISABLED;
    bool collectPipeline_ = false;
    bool collectPcSampling_ = false;
    data::ProfilingDataManager& dataManager_ = data::ProfilingDataManager::Instance();
};

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_RANGE_PROFILER_H
