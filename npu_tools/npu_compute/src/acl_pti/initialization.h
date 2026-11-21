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
 * @file initialization.h
 * @brief Coordinates one-time initialization of shared ACL PTI dependencies.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_INITIALIZATION_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_INITIALIZATION_H

#include "aclpti/aclpti_types.h"
#include "aclpti/aclpti_data.h"
#include "acl_pti/profiling/application_profiler.h"

namespace aclpti::initialization {

/// Initializes runtime hooks, handlers, and profiling dependencies once.
aclptiResult InitializeDependencies();

// Internal processor entry: mode selection is immutable for this process.
ACLPTI_EXPORT aclptiResult
InitializeApplication(profiling::ApplicationConsumer consumer, profiling::ApplicationDrainedCallback onDrained);
ACLPTI_EXPORT aclptiResult
ShutdownApplication(profiling::ApplicationEndReason reason = profiling::ApplicationEndReason::LocalShutdown) noexcept;
ACLPTI_EXPORT uint64_t ApplicationDeviceEpoch(int32_t deviceId);
using ApplicationRecordCallback = void (*)(const profiling::ApplicationRawRecord&);
using ApplicationSummaryCallback = void (*)(const profiling::ApplicationSummary&);
using KernelResultCallback = void (*)(const aclptiProfilingDataResult&);
using KernelCompleteCallback = void (*)(aclptiResult);
using ApplicationLifecycleCallback = void (*)() noexcept;

// Processor-private entry points. Callback arguments are borrowed until return.
ACLPTI_EXPORT aclptiResult
RegisterApplicationLifecycle(ApplicationLifecycleCallback onInit, ApplicationLifecycleCallback onFinalize);
ACLPTI_EXPORT aclptiResult PrepareApplicationSession(
    ApplicationRecordCallback consumer, ApplicationSummaryCallback onDrained, bool registerFinalizeCallback = true,
    profiling::ApplicationDeviceStartCallback beforeDeviceStart = nullptr);
// The finalize callback holds Runtime lifecycle locks. Close admission and report whether all
// remaining visits are local raw uploads, which can be drained without calling Runtime.
ACLPTI_EXPORT bool CloseApplicationAdmissionForFinalize() noexcept;
ACLPTI_EXPORT aclptiResult PrepareKernelSession(KernelResultCallback onData, KernelCompleteCallback onComplete);
ACLPTI_EXPORT void ActivateProcessorSession() noexcept;
ACLPTI_EXPORT aclptiResult ShutdownProcessorSession(profiling::ApplicationEndReason reason) noexcept;
ACLPTI_EXPORT bool InProcessorCallback() noexcept;

aclptiResult SetRangeConfig(const aclptiRangeProfilerSetConfigParams* params);
bool CollectionOpen() noexcept;
void NoteKernelCompletion(aclptiResult status) noexcept;
void DeliverKernelCompletion() noexcept;

// Admission and the in-flight count are changed together. The owner can release
// its session once the last successful admission is returned.
class CallbackAccess {
public:
    enum class Kind { Runtime, Configure, Raw };
    explicit CallbackAccess(Kind kind = Kind::Runtime) noexcept;
    ~CallbackAccess();
    CallbackAccess(const CallbackAccess&) = delete;
    CallbackAccess& operator=(const CallbackAccess&) = delete;
    explicit operator bool() const noexcept { return admitted_; }

private:
    uint64_t admitted_ = 0;
};

} // namespace aclpti::initialization

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_INITIALIZATION_H
