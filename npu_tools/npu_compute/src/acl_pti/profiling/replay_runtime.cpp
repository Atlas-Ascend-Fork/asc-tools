/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "replay_runtime.h"

#include "common/debug_log.h"

#include <new>

namespace aclpti::profiling {

aclptiResult ReplayRuntime::Initialize()
{
    if (initialized_) {
        npucompute::detail::DebugLog("aclpti", "replay runtime already initialized");
        return ACLPTI_SUCCESS;
    }
    const aclptiResult result = rangeProfiler_.Initialize();
    if (result != ACLPTI_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "replay runtime initialization failed");
        return result;
    }
    initialized_ = true;
    npucompute::detail::DebugLog("aclpti", "replay runtime initialized");
    return ACLPTI_SUCCESS;
}

aclptiResult ReplayRuntime::SetConfig(const aclptiRangeProfilerSetConfigParams* params)
{
    if (params == nullptr ||
        (params->cacheControl != ACLPTI_CACHE_CONTROL_ALL && params->cacheControl != ACLPTI_CACHE_CONTROL_NONE)) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    const aclptiResult status = rangeProfiler_.SetConfig(params);
    if (status != ACLPTI_SUCCESS) {
        return status;
    }
    rangeProfiler_.SetCacheControl(params->cacheControl);
    return ACLPTI_SUCCESS;
}

aclptiResult ReplayRuntime::RegisterBinary(
    const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary)
{
    if ((!CollectPipeline() && !CollectPcSampling()) || !ProfilingAvailable()) {
        return ACLPTI_SUCCESS;
    }
    return HandleProfilingResult(binaryRegistry_.RegisterBinary(data, size, options, binary, CollectPcSampling()));
}

aclptiResult ReplayRuntime::RegisterBinaryFromFile(
    const char* path, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary)
{
    if ((!CollectPipeline() && !CollectPcSampling()) || !ProfilingAvailable()) {
        return ACLPTI_SUCCESS;
    }
    return HandleProfilingResult(binaryRegistry_.RegisterBinaryFromFile(path, options, binary, CollectPcSampling()));
}

aclptiResult ReplayRuntime::RegisterBinaryFunction(aclrtBinHandle binary, const char* name, aclrtFuncHandle function)
{
    if ((!CollectPipeline() && !CollectPcSampling()) || !ProfilingAvailable()) {
        return ACLPTI_SUCCESS;
    }
    return HandleProfilingResult(binaryRegistry_.RegisterBinaryFunction(binary, name, function));
}

aclptiResult ReplayRuntime::RegisterBinaryFunction(aclrtBinHandle binary, std::uint64_t entry, aclrtFuncHandle function)
{
    if ((!CollectPipeline() && !CollectPcSampling()) || !ProfilingAvailable()) {
        return ACLPTI_SUCCESS;
    }
    return HandleProfilingResult(binaryRegistry_.RegisterBinaryFunction(binary, entry, function));
}

aclptiResult ReplayRuntime::RegisterSymbolFunction(aclrtFuncHandle function)
{
    if ((!CollectPipeline() && !CollectPcSampling()) || !ProfilingAvailable()) {
        return ACLPTI_SUCCESS;
    }
    return HandleProfilingResult(binaryRegistry_.RegisterSymbolFunction(function, CollectPcSampling()));
}

aclptiResult ReplayRuntime::PrepareBinaryUnload(aclrtBinHandle binary, BinaryRegistry::UnloadContext& context)
{
    // Cleanup must remain available after profiling has stopped.
    return HandleProfilingResult(binaryRegistry_.PrepareBinaryUnload(binary, context));
}

aclptiResult ReplayRuntime::CompleteBinaryUnload(BinaryRegistry::UnloadContext& context)
{
    return HandleProfilingResult(binaryRegistry_.CompleteBinaryUnload(context));
}

aclptiResult ReplayRuntime::MirrorMalloc(
    void** devPtr, std::size_t size, aclrtMemMallocPolicy policy, aclrtApiId api, aclrtMallocConfig* cfg)
{
    if (!ProfilingAvailable()) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    return HandleProfilingResult(replayMemory_.MirrorMalloc(devPtr, size, policy, api, cfg));
}

aclptiResult ReplayRuntime::MirrorFree(void* devPtr) { return HandleProfilingResult(replayMemory_.MirrorFree(devPtr)); }

aclptiResult ReplayRuntime::MirrorMemcpy(
    void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind kind)
{
    if (count == 0 || (kind != ACL_MEMCPY_DEVICE_TO_DEVICE && kind != ACL_MEMCPY_HOST_TO_DEVICE)) {
        return ACLPTI_SUCCESS;
    }
    if (!ProfilingAvailable()) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    return HandleProfilingResult(replayMemory_.MirrorMemcpy(destination, destinationSize, source, count, kind));
}

aclptiResult ReplayRuntime::MirrorMemset(void* devPtr, std::size_t maxCount, std::int32_t value, std::size_t count)
{
    if (count == 0) {
        return ACLPTI_SUCCESS;
    }
    if (!ProfilingAvailable()) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    return HandleProfilingResult(replayMemory_.MirrorMemset(devPtr, maxCount, value, count));
}

aclptiResult ReplayRuntime::ReplayKernel(
    aclrtFuncHandle originalFunction, const ReplayLaunchFunction& launchFunction, aclrtStream stream)
{
    if (!ProfilingAvailable()) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const aclptiResult status =
        rangeProfiler_.ReplayKernel(replayMemory_, binaryRegistry_, originalFunction, launchFunction, stream);
    // One original launch owns all rounds above and exactly one complete result publication.
    const aclptiResult shutdownStatus = StopProfiling();
    npucompute::detail::DebugLog(
        "aclpti", "replay runtime result original=%p replay_status=%d shutdown_status=%d", originalFunction,
        static_cast<int>(status), static_cast<int>(shutdownStatus));
    if (status == ACLPTI_SUCCESS) {
        return shutdownStatus;
    }
    return status;
}

bool ReplayRuntime::ProfilingAvailable() const { return profilingAvailable_.load(std::memory_order_acquire); }

aclptiResult ReplayRuntime::HandleProfilingResult(aclptiResult status)
{
    if (status != ACLPTI_SUCCESS) {
        (void)StopProfiling();
    }
    return status;
}

aclptiResult ReplayRuntime::StopProfiling()
{
    bool expected = true;
    if (!profilingAvailable_.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return ACLPTI_SUCCESS;
    }
    try {
        return rangeProfiler_.Shutdown();
    } catch (const std::bad_alloc&) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=profiling_shutdown status=%d", ACLPTI_ERROR_OUT_OF_MEMORY);
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
}

ReplayRuntime& GetReplayRuntime()
{
    static ReplayRuntime runtime;
    return runtime;
}

} // namespace aclpti::profiling
