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
 * @file replay_runtime.h
 * @brief Owns replay memory and range-profiling state used by runtime API handlers.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_REPLAY_RUNTIME_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_REPLAY_RUNTIME_H

#include "range_profiler.h"
#include "replay_memory.h"
#include "binary_registry.h"

#include <atomic>

namespace aclpti::profiling {

class ReplayRuntime {
public:
    /// Initializes the range profiler once while preserving its failure result.
    aclptiResult Initialize();

    /// Applies the requested profiling collection configuration.
    aclptiResult SetConfig(const aclptiRangeProfilerSetConfigParams* params);

    bool CollectPipeline() const { return rangeProfiler_.CollectPipeline(); }
    bool CollectPcSampling() const { return rangeProfiler_.CollectPcSampling(); }
    bool GetPmSamplingSnapshot(aclrtFuncHandle function, PmSamplingBinarySnapshot& snapshot)
    {
        return binaryRegistry_.GetPmSamplingSnapshot(function, snapshot);
    }
    bool GetInstrumentedTraceArgumentOffset(aclrtFuncHandle function, std::uint32_t& offset)
    {
        return binaryRegistry_.GetInstrumentedTraceArgumentOffset(function, offset);
    }

    aclptiResult RegisterBinary(
        const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary);
    aclptiResult RegisterBinaryFromFile(const char* path, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary);
    aclptiResult RegisterBinaryFunction(aclrtBinHandle binary, const char* name, aclrtFuncHandle function);
    aclptiResult RegisterBinaryFunction(aclrtBinHandle binary, std::uint64_t entry, aclrtFuncHandle function);
    aclptiResult RegisterSymbolFunction(aclrtFuncHandle function);
    aclptiResult PrepareBinaryUnload(aclrtBinHandle binary, BinaryRegistry::UnloadContext& context);
    aclptiResult CompleteBinaryUnload(BinaryRegistry::UnloadContext& context);

    /// Stops profiling once; callers preserve the error that triggered shutdown.
    aclptiResult StopProfiling();

    /// Allocates replay shadow memory for a successful device allocation.
    aclptiResult MirrorMalloc(
        void** devPtr, std::size_t size, aclrtMemMallocPolicy policy, aclrtApiId api, aclrtMallocConfig* cfg = nullptr);

    /// Releases replay shadow memory associated with a device allocation.
    aclptiResult MirrorFree(void* devPtr);

    /// Mirrors a supported device memory copy for replay.
    aclptiResult MirrorMemcpy(
        void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind kind);

    /// Mirrors a device memset for replay.
    aclptiResult MirrorMemset(void* devPtr, std::size_t maxCount, std::int32_t value, std::size_t count);

    /// Delegates replay rounds to RangeProfiler and shuts down profiling afterwards.
    /// Called after the handler has successfully submitted the original launch.
    /// The callback invokes the supplied function using the original launch arguments.
    /// Invoked synchronously and never retained; captured arguments must live until return.
    aclptiResult ReplayKernel(
        aclrtFuncHandle originalFunction, const ReplayLaunchFunction& launchFunction, aclrtStream stream);

private:
    /// Reports whether new profiling work may be started.
    bool ProfilingAvailable() const;

    /// Stops profiling after a failed optional operation and preserves its root status.
    aclptiResult HandleProfilingResult(aclptiResult status);

    bool initialized_ = false;
    std::atomic<bool> profilingAvailable_{true};
    ReplayMemory replayMemory_;
    BinaryRegistry binaryRegistry_;
    RangeProfiler rangeProfiler_;
};

/// Returns the process-wide replay runtime.
ReplayRuntime& GetReplayRuntime();

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_REPLAY_RUNTIME_H
