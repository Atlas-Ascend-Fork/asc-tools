/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_APPLICATION_PROFILER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_APPLICATION_PROFILER_H

#include "acl/acl_base.h"
#include "aclpti/aclpti_range_profiler.h"
#include "acl_pti/data/data_types.h"
#include "npu_compute/common.h"
#include "profiling/prof_api.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace aclpti::profiling {

enum class ApplicationDeviceState { Inactive, Starting, Active, Stopping, Unknown };
enum class ApplicationEndReason {
    Unconfirmed,
    AclFinalize,
    ProcessExit,
    LocalShutdown,
    StartupRollback,
    FinalizeWhileBusy
};
enum class ApplicationDeviceEndReason { Unconfirmed, ResetStop, RuntimeFinalize };

struct ApplicationDeviceSummary {
    ApplicationDeviceState state = ApplicationDeviceState::Inactive;
    uint64_t deviceEpoch = 0;
    int32_t startStatus = 0;
    int32_t stopStatus = 0;
    bool startAttempted = false;
    bool stopAttempted = false;
    ApplicationDeviceEndReason endReason = ApplicationDeviceEndReason::Unconfirmed;
};

struct ApplicationRawRecord {
    int32_t deviceId = 0;
    // Zero means unassigned (including Host and ambiguous data after a device restart).
    uint64_t deviceEpoch = 0;
    int32_t type = 0;
    int32_t chunkModule = 0;
    size_t offset = 0;
    bool isLastChunk = false;
    std::vector<uint8_t> bytes;
    std::vector<data::DecodedRecord> decoded;
    uint64_t decodeErrors = 0;
};

struct ApplicationSummary {
    std::map<int32_t, ApplicationDeviceSummary> devices;
    uint64_t receivedRecords = 0;
    uint64_t processedRecords = 0;
    uint64_t droppedRecords = 0;
    uint64_t invalidRecords = 0;
    uint64_t processingErrors = 0;
    // Local stream/control health only; overall coverage additionally requires tailConfirmed.
    bool complete = false;
    // Stop success and an empty local queue do not prove Runtime callback completion.
    bool tailConfirmed = false;
    ApplicationEndReason endReason = ApplicationEndReason::Unconfirmed;
};

using ApplicationConsumer = std::function<void(const ApplicationRawRecord&)>;
using ApplicationDrainedCallback = std::function<void(const ApplicationSummary&)>;
using ApplicationDeviceStartCallback = void (*)(int32_t) noexcept;

class NPU_COMPUTE_LOCAL ApplicationProfiler final {
public:
    ApplicationProfiler();
    ~ApplicationProfiler();
    ApplicationProfiler(const ApplicationProfiler&) = delete;
    ApplicationProfiler& operator=(const ApplicationProfiler&) = delete;

    using UploaderRegistration = int32_t (*)(MsprofRawDataCallback);
    aclptiResult Initialize(
        ApplicationConsumer consumer, ApplicationDrainedCallback onDrained,
        UploaderRegistration registerUploader = nullptr, ApplicationDeviceStartCallback beforeDeviceStart = nullptr);
    aclError SetDevice(aclError (*original)(int32_t), int32_t deviceId);
    aclError ResetDevice(aclError (*original)(int32_t), int32_t deviceId, bool force);
    aclptiResult SetConfig(const aclptiRangeProfilerSetConfigParams* params);
    uint64_t DeviceEpoch(int32_t deviceId) const;
    // Closes raw reception and records Runtime's tail under the reception lock.
    // Shutdown requests the final handoff after the session's metadata admission closes.
    void NoteProfilingFinalized() noexcept;
    // Local drain/join only. Without a prior finalize notification active devices fail shutdown.
    // Invokes onDrained on the consumer worker before joining it. Consumer callbacks must not
    // synchronously request Shutdown; that would join their own worker and is rejected.
    aclptiResult Shutdown(ApplicationEndReason reason = ApplicationEndReason::LocalShutdown) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

NPU_COMPUTE_LOCAL ApplicationProfiler& GetApplicationProfiler();
void BindApplicationProfiler(ApplicationProfiler* profiler) noexcept;

} // namespace aclpti::profiling

#endif
