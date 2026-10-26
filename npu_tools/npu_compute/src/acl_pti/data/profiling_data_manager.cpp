/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "profiling_data_manager.h"

#include "npu_compute_log.h"
#include <cstdlib>
#include <memory>
#include <utility>
#include <stdexcept>

namespace aclpti::data {
namespace {
// Validate replay configuration independently of processor state.
bool IsValidReplayInfo(const ReplayPrepareInfo& info)
{
    if (info.kind != ReplayKind::Pmu && info.kind != ReplayKind::Pipeline && info.kind != ReplayKind::PcSampling) {
        return false;
    }
    bool unused = false;
    for (auto event : info.pmuEventIds) {
        if (event == kInvalidPmuEvent) {
            unused = true;
        } else if (unused) {
            return false;
        }
    }
    return true;
}

} // namespace

ProfilingDataManager& ProfilingDataManager::Instance()
{
    static ProfilingDataManager manager;
    return manager;
}
ProfilingDataManager::~ProfilingDataManager() { (void)ForceShutdown(); }

aclptiResult ProfilingDataManager::Initialize(aclptiProfilingDataCallback callback)
{
#if defined(NPU_COMPUTE_ENABLE_TEST_CONTROLS)
    const char* failure = std::getenv("NPU_COMPUTE_TEST_PTI_INITIALIZE_FAILURE");
    if (failure != nullptr && failure[0] != '\0') {
        return ACLPTI_ERROR_INTERNAL;
    }
#endif
    // Serialize restart with the whole shutdown, including result publication.
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ManagerState::Running) {
        return ACLPTI_SUCCESS;
    }
    if (state_ != ManagerState::Created && state_ != ManagerState::Stopped) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    callback_ = std::move(callback);
    shutdownStatus_ = ACLPTI_SUCCESS;
    if (!callback_) {
        callback_ = registeredCallback_;
    }
    if (!callback_) {
        callback_ = [](std::shared_ptr<const aclptiProfilingDataResult>) { return ACLPTI_SUCCESS; };
    }
    try {
        processor_.Start();
    } catch (...) {
        state_ = ManagerState::Stopped;
        shutdownStatus_ = ACLPTI_ERROR_INTERNAL;
        return shutdownStatus_;
    }
    state_ = ManagerState::Running;
    ASCTOOL_DEBUG("[aclpti-data] initialize complete: worker=1");
    return ACLPTI_SUCCESS;
}

MsprofRawDataCallback ProfilingDataManager::GetRawDataCallback()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ManagerState::Running ? &RawDataThunk : nullptr;
}

aclptiResult ProfilingDataManager::PrepareReplay(const ReplayPrepareInfo& info)
{
    if (!IsValidReplayInfo(info)) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ManagerState::Running ? processor_.PrepareReplay(info) : ACLPTI_ERROR_NOT_INITIALIZED;
}

ReplayResult ProfilingDataManager::RecordReplayStatus(const ReplayStopInfo& info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ManagerState::Running ? processor_.RecordReplayStatus(info) :
                                             ReplayResult{info.replayId, ACLPTI_ERROR_NOT_INITIALIZED, {}};
}

aclptiResult ProfilingDataManager::ReleaseReplay(uint64_t replayId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ManagerState::Running ? processor_.ReleaseReplay(replayId) : ACLPTI_ERROR_NOT_INITIALIZED;
}

aclptiResult ProfilingDataManager::Shutdown() { return ShutdownImpl(false); }
aclptiResult ProfilingDataManager::ForceShutdown() { return ShutdownImpl(true); }

aclptiResult ProfilingDataManager::ShutdownImpl(bool force)
{
    // Serializes shutdown and restart; result callbacks run without the instance lock.
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ManagerState::Created || state_ == ManagerState::Stopped) {
            return shutdownStatus_;
        }
        const auto status = processor_.CloseReplay(force);
        if (status != ACLPTI_SUCCESS) {
            return status;
        }
        state_ = ManagerState::Stopping;
    }
    // Stopping rejects callbacks before draining; the singleton itself remains alive.
    aclptiResult callbackStatus = ACLPTI_SUCCESS;
    aclptiResult publicationStatus = ACLPTI_SUCCESS;
    try {
        auto processedResult = processor_.StopAndTakeResult();
#if defined(NPU_COMPUTE_ENABLE_TEST_CONTROLS)
        const char* failure = std::getenv("NPU_COMPUTE_TEST_RESULT_OOM");
        if (failure != nullptr && failure[0] != '\0') {
            throw std::bad_alloc();
        }
#endif
        // Publish after the worker exits: the consumer owns a stable snapshot, not live state.
        if (processedResult) {
            auto result = std::make_shared<aclptiProfilingDataResult>(std::move(*processedResult));
            callbackStatus = callback_(std::move(result));
        }
    } catch (const std::bad_alloc&) {
        publicationStatus = ACLPTI_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        callbackStatus = ACLPTI_ERROR_CALLBACK;
    }
    aclptiDataModuleShutdownCallback shutdownCallback;
    void* userData;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdownCallback = shutdownCallback_;
        userData = shutdownUserData_;
    }
    // Consumer failures are reported through the registered drain callback, as before.
    ASCTOOL_DEBUG("[aclpti-data] result callback status=%d", static_cast<int>(callbackStatus));
    aclptiResult status = ACLPTI_SUCCESS;
    if (shutdownCallback) {
        try {
            status = static_cast<aclptiResult>(shutdownCallback(userData));
        } catch (...) {
            status = ACLPTI_ERROR_INTERNAL;
        }
    }
    if (publicationStatus != ACLPTI_SUCCESS) {
        status = publicationStatus;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdownStatus_ = status;
        state_ = ManagerState::Stopped;
    }
    return status;
}

// The process-wide callback always targets the same object; state gates reception.
std::int32_t ProfilingDataManager::RawDataThunk(MsprofRawData* raw)
{
    auto& manager = Instance();
    std::lock_guard<std::mutex> lock(manager.mutex_);
    if (manager.state_ != ManagerState::Running) {
        return static_cast<std::int32_t>(ACLPTI_ERROR_NOT_INITIALIZED);
    }
    return static_cast<std::int32_t>(manager.processor_.ReceiveRawData(raw));
}

aclptiResult ProfilingDataManager::RegisterProfilingDataCallback(aclptiProfilingDataCallback callback)
{
    if (!callback) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (registeredCallback_) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    registeredCallback_ = std::move(callback);
    return ACLPTI_SUCCESS;
}

aclptiResult ProfilingDataManager::RegisterShutdownCallback(aclptiDataModuleShutdownCallback callback, void* userData)
{
    if (!callback) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdownCallback_) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    shutdownCallback_ = callback;
    shutdownUserData_ = userData;
    return ACLPTI_SUCCESS;
}

} // namespace aclpti::data
