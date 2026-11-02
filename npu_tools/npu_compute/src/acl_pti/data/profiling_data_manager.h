/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_PROFILING_DATA_MANAGER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_PROFILING_DATA_MANAGER_H

#include "data_types.h"
#include "npu_compute/common.h"
#include "profiling/prof_api.h"

#include "data_processor.h"
#include <mutex>

namespace aclpti::data {

// Coordinates one active replay and one worker; accumulated results span all prepared replays.
// Normal order: Initialize -> (PrepareReplay -> RecordReplayStatus -> ReleaseReplay)* -> Shutdown.
class NPU_COMPUTE_LOCAL ProfilingDataManager final {
public:
    static ProfilingDataManager& Instance();

    // Registration persists across collection cycles; duplicate registration is rejected.
    aclptiResult RegisterProfilingDataCallback(aclptiProfilingDataCallback callback);
    aclptiResult RegisterShutdownCallback(aclptiDataModuleShutdownCallback callback, void* userData);

    ProfilingDataManager(const ProfilingDataManager&) = delete;
    ProfilingDataManager& operator=(const ProfilingDataManager&) = delete;

    // Starts a fresh collection after shutdown; a running instance is initialized only once.
    aclptiResult Initialize(aclptiProfilingDataCallback callback = {});
    MsprofRawDataCallback GetRawDataCallback();
    aclptiResult PrepareReplay(const ReplayPrepareInfo& info);
    // Closes reception, waits for accepted work, and merges collection/processing status.
    ReplayResult RecordReplayStatus(const ReplayStopInfo& info);
    // Releases only the closed reception context; aggregated data remains until Shutdown.
    aclptiResult ReleaseReplay(uint64_t replayId);
    // Requires no active replay; drains the worker and publishes the result at most once.
    aclptiResult Shutdown();

    /// Releases an active replay, drains queued data, and shuts down the module.
    aclptiResult ForceShutdown();

private:
    ProfilingDataManager() = default;
    ~ProfilingDataManager();
    enum class ManagerState { Created, Running, Stopping, Stopped };

    aclptiResult ShutdownImpl(bool force);
    aclptiResult PublishFinalResult();
    static std::int32_t RawDataThunk(MsprofRawData* raw);
    // Registered defaults persist; callback_ is the snapshot selected for this collection.
    aclptiProfilingDataCallback registeredCallback_;
    aclptiDataModuleShutdownCallback shutdownCallback_ = nullptr;
    void* shutdownUserData_ = nullptr;
    aclptiProfilingDataCallback callback_;
    std::mutex mutex_;
    std::mutex shutdownMutex_;
    ManagerState state_ = ManagerState::Created;
    aclptiResult shutdownStatus_ = ACLPTI_SUCCESS;
    DataProcessor processor_;
};

} // namespace aclpti::data

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_PROFILING_DATA_MANAGER_H
