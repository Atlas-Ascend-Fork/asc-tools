/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "initialization.h"

#include "acl_pti/profiling/replay_runtime.h"
#include "acl_pti/handler/runtime_api_handlers.h"
#include "injection/injection_hook.h"
#include "acl/acl_rt.h"

#include "acl_pti/callback/dispatcher.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <utility>

namespace aclpti::initialization {
namespace {
enum class SessionMode { None, StandaloneKernel, StandaloneApplication, Kernel, Application };
enum class SessionState { Idle, Prepared, Active, Closing, Stopped };

constexpr uint64_t kOpen = uint64_t{1} << 63;
constexpr uint64_t kConfigurable = uint64_t{1} << 62;
constexpr uint64_t kRawUpload = uint64_t{1} << 31;
constexpr uint64_t kRuntimeInFlight = kRawUpload - 1;
constexpr uint64_t kInFlight = kConfigurable - 1;
std::atomic<uint64_t> admission{0};
pthread_mutex_t admissionMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t admissionChanged = PTHREAD_COND_INITIALIZER;
thread_local uint32_t callbackDepth = 0;
thread_local std::optional<aclptiResult> pendingKernelCompletion;
std::mutex controlMutex;
std::condition_variable controlChanged;
SessionMode mode = SessionMode::None;
SessionState state = SessionState::Idle;
aclptiResult shutdownStatus = ACLPTI_SUCCESS;

struct KernelSession {
    data::ProfilingDataManager data;
    profiling::ReplayRuntime runtime{data};
    KernelCompleteCallback onComplete = nullptr;
};

std::unique_ptr<callback::Dispatcher> processorDispatcher;
std::unique_ptr<KernelSession> kernelSession;
std::unique_ptr<profiling::ApplicationProfiler> applicationSession;
MsprofRawDataCallback applicationUploader = nullptr;
bool legacyFinalizeRegistered = false;

enum class LifecycleRegistration { Idle, Registering, Registered };
std::atomic<LifecycleRegistration> lifecycleRegistration{LifecycleRegistration::Idle};
std::atomic<ApplicationLifecycleCallback> applicationInit{nullptr};
std::atomic<ApplicationLifecycleCallback> applicationFinalize{nullptr};

aclError ApplicationInitLifecycle(const char*, size_t, void*) noexcept
{
    if (const auto init = applicationInit.load(std::memory_order_acquire)) {
        init();
    }
    return ACL_SUCCESS;
}

aclError ApplicationFinalizeLifecycle(void*) noexcept
{
    if (const auto finalize = applicationFinalize.load(std::memory_order_acquire)) {
        finalize();
    }
    return ACL_SUCCESS;
}

class CallbackFrame {
public:
    CallbackFrame() noexcept { ++callbackDepth; }
    ~CallbackFrame() { --callbackDepth; }
};

void CloseAdmission() noexcept
{
    pthread_mutex_lock(&admissionMutex);
    admission.fetch_and(kInFlight, std::memory_order_acq_rel);
    while ((admission.load(std::memory_order_acquire) & kInFlight) != 0) {
        pthread_cond_wait(&admissionChanged, &admissionMutex);
    }
    pthread_mutex_unlock(&admissionMutex);
}

aclError ApplicationFinalized(void*) noexcept
{
    const CallbackAccess access;
    if (access) {
        // Runtime holds lifecycle locks here. Device reconciliation belongs to the decoder.
        profiling::GetApplicationProfiler().NoteProfilingFinalized();
    }
    return ACL_SUCCESS;
}

int32_t ReceiveApplication(MsprofRawData* raw) noexcept
{
    const CallbackAccess access(CallbackAccess::Kind::Raw);
    return access ? applicationUploader(raw) : ACLPTI_ERROR_NOT_INITIALIZED;
}

int32_t ReceiveKernel(MsprofRawData* raw) noexcept
{
    const CallbackAccess access(CallbackAccess::Kind::Raw);
    return access ? kernelSession->data.ReceiveRawData(raw) : ACLPTI_ERROR_NOT_INITIALIZED;
}

int32_t RegisterApplicationUploader(MsprofRawDataCallback callback)
{
    applicationUploader = callback;
    return acltoolUploaderInit(&ReceiveApplication);
}

aclptiResult PrepareApplication(
    profiling::ApplicationProfiler& profiler, profiling::ApplicationConsumer consumer,
    profiling::ApplicationDrainedCallback onDrained, bool registerFinalizeCallback,
    profiling::ApplicationDeviceStartCallback beforeDeviceStart = nullptr)
{
    if (acltoolHookInit() != 0 || !handler::RegisterRuntimeApiHandlers(handler::RuntimeHandlerMode::Application)) {
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }
    const auto status = profiler.Initialize(
        [consumer = std::move(consumer)](const auto& record) {
            const CallbackFrame frame;
            consumer(record);
        },
        [onDrained = std::move(onDrained)](const auto& summary) {
            const CallbackFrame frame;
            onDrained(summary);
        },
        &RegisterApplicationUploader, beforeDeviceStart);
    if (status != ACLPTI_SUCCESS) {
        return status;
    }
    if (registerFinalizeCallback) {
        if (aclFinalizeCallbackRegister(ACL_REG_TYPE_OTHER, ApplicationFinalized, nullptr) != ACL_SUCCESS) {
            (void)profiler.Shutdown(profiling::ApplicationEndReason::StartupRollback);
            return ACLPTI_ERROR_INITIALIZATION_FAILED;
        }
        legacyFinalizeRegistered = true;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult PrepareKernel(
    profiling::ReplayRuntime& runtime, KernelResultCallback onData, MsprofRawDataCallback uploader)
{
    if (acltoolHookInit() != 0 || !handler::RegisterRuntimeApiHandlers()) {
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }
    return runtime.Initialize(onData, uploader);
}

void ClearDispatcher() noexcept
{
    if (processorDispatcher) {
        callback::BindDispatcher(nullptr);
        processorDispatcher.reset();
    } else {
        callback::GetDispatcher().Clear();
    }
}

void RecordProcessorDispatcher(std::unique_ptr<callback::Dispatcher> dispatcher) noexcept
{
    processorDispatcher = std::move(dispatcher);
    callback::BindDispatcher(processorDispatcher.get());
}

void RecordPrepared() noexcept
{
    state = SessionState::Prepared;
    admission.store(kConfigurable, std::memory_order_release);
}

void RecordActive() noexcept
{
    state = SessionState::Active;
    admission.fetch_or(kOpen, std::memory_order_release);
}
} // namespace

CallbackAccess::CallbackAccess(Kind kind) noexcept
{
    const uint64_t permittedFlag = kind == Kind::Configure ? kConfigurable : kOpen;
    const uint64_t increment = kind == Kind::Raw ? kRawUpload : 1;
    auto value = admission.load(std::memory_order_acquire);
    while ((value & permittedFlag) != 0) {
        if (admission.compare_exchange_weak(
                value, value + increment, std::memory_order_acquire, std::memory_order_relaxed)) {
            ++callbackDepth;
            admitted_ = increment;
            return;
        }
    }
}

CallbackAccess::~CallbackAccess()
{
    if (admitted_ == 0U) {
        return;
    }
    --callbackDepth;
    if (admission.fetch_sub(admitted_, std::memory_order_acq_rel) == admitted_) {
        pthread_mutex_lock(&admissionMutex);
        pthread_cond_broadcast(&admissionChanged);
        pthread_mutex_unlock(&admissionMutex);
    }
}

bool InProcessorCallback() noexcept { return callbackDepth != 0; }

bool CollectionOpen() noexcept { return (admission.load(std::memory_order_acquire) & kOpen) != 0; }

void NoteKernelCompletion(aclptiResult status) noexcept
{
    if (kernelSession != nullptr && InProcessorCallback()) {
        pendingKernelCompletion = status;
    }
}

void DeliverKernelCompletion() noexcept
{
    if (!pendingKernelCompletion || callbackDepth != 1) {
        return;
    }
    const auto status = *pendingKernelCompletion;
    pendingKernelCompletion.reset();
    try {
        kernelSession->onComplete(status);
    } catch (...) {
        ASCTOOL_ERROR("[aclpti] processor completion callback threw code=%u", NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
    }
}

aclptiResult RegisterApplicationLifecycle(ApplicationLifecycleCallback onInit, ApplicationLifecycleCallback onFinalize)
{
    if (onInit == nullptr || onFinalize == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    auto expected = LifecycleRegistration::Idle;
    if (!lifecycleRegistration.compare_exchange_strong(expected, LifecycleRegistration::Registering)) {
        return ACLPTI_SUCCESS;
    }

    applicationInit.store(onInit, std::memory_order_release);
    applicationFinalize.store(onFinalize, std::memory_order_release);
    // Init registration can invoke the callback immediately when ACL is already initialized.
    if (aclFinalizeCallbackRegister(ACL_REG_TYPE_OTHER, ApplicationFinalizeLifecycle, nullptr) == ACL_SUCCESS) {
        if (aclInitCallbackRegister(ACL_REG_TYPE_OTHER, ApplicationInitLifecycle, nullptr) == ACL_SUCCESS) {
            lifecycleRegistration.store(LifecycleRegistration::Registered, std::memory_order_release);
            return ACLPTI_SUCCESS;
        }
        (void)aclFinalizeCallbackUnRegister(ACL_REG_TYPE_OTHER, ApplicationFinalizeLifecycle);
    }
    applicationInit.store(nullptr, std::memory_order_release);
    applicationFinalize.store(nullptr, std::memory_order_release);
    lifecycleRegistration.store(LifecycleRegistration::Idle, std::memory_order_release);
    return ACLPTI_ERROR_INITIALIZATION_FAILED;
}

bool CloseApplicationAdmissionForFinalize() noexcept
{
    const auto previous = admission.fetch_and(kInFlight, std::memory_order_acq_rel);
    return (previous & kRuntimeInFlight) == 0;
}

aclptiResult PrepareApplicationSession(
    ApplicationRecordCallback consumer, ApplicationSummaryCallback onDrained, bool registerFinalizeCallback,
    profiling::ApplicationDeviceStartCallback beforeDeviceStart)
{
    if (consumer == nullptr || onDrained == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(controlMutex);
    if (state != SessionState::Idle) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    try {
        auto dispatcher = std::make_unique<callback::Dispatcher>();
        auto profiler = std::make_unique<profiling::ApplicationProfiler>();
        const auto status =
            PrepareApplication(*profiler, consumer, onDrained, registerFinalizeCallback, beforeDeviceStart);
        if (status != ACLPTI_SUCCESS) {
            handler::ClearRuntimeApiHandlers();
            applicationUploader = nullptr;
            return status;
        }
        applicationSession = std::move(profiler);
        profiling::BindApplicationProfiler(applicationSession.get());
        RecordProcessorDispatcher(std::move(dispatcher));
        mode = SessionMode::Application;
        RecordPrepared();
        return ACLPTI_SUCCESS;
    } catch (...) {
        handler::ClearRuntimeApiHandlers();
        applicationUploader = nullptr;
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }
}

aclptiResult PrepareKernelSession(KernelResultCallback onData, KernelCompleteCallback onComplete)
{
    if (onData == nullptr || onComplete == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(controlMutex);
    if (state != SessionState::Idle) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    try {
        auto dispatcher = std::make_unique<callback::Dispatcher>();
        auto session = std::make_unique<KernelSession>();
        const auto status = PrepareKernel(session->runtime, onData, &ReceiveKernel);
        if (status != ACLPTI_SUCCESS) {
            handler::ClearRuntimeApiHandlers();
            return status;
        }
        session->onComplete = onComplete;
        kernelSession = std::move(session);
        profiling::BindReplayRuntime(&kernelSession->runtime);
        RecordProcessorDispatcher(std::move(dispatcher));
        mode = SessionMode::Kernel;
        RecordPrepared();
        return ACLPTI_SUCCESS;
    } catch (...) {
        handler::ClearRuntimeApiHandlers();
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }
}

void ActivateProcessorSession() noexcept
{
    std::lock_guard<std::mutex> lock(controlMutex);
    if (state == SessionState::Prepared) {
        RecordActive();
    }
}

aclptiResult ShutdownProcessorSession(profiling::ApplicationEndReason reason) noexcept
{
    if (InProcessorCallback()) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    bool rollback = false;
    {
        std::unique_lock<std::mutex> lock(controlMutex);
        controlChanged.wait(lock, [] { return state != SessionState::Closing; });
        if (state == SessionState::Idle || state == SessionState::Stopped) {
            return shutdownStatus;
        }
        rollback = state == SessionState::Prepared && reason == profiling::ApplicationEndReason::StartupRollback;
        state = SessionState::Closing;
    }
    CloseAdmission();
    handler::ClearRuntimeApiHandlers();
    aclptiResult status = ACLPTI_SUCCESS;
    try {
        if (applicationSession != nullptr || mode == SessionMode::StandaloneApplication) {
            if (rollback && legacyFinalizeRegistered) {
                (void)aclFinalizeCallbackUnRegister(ACL_REG_TYPE_OTHER, ApplicationFinalized);
                legacyFinalizeRegistered = false;
            }
            status = profiling::GetApplicationProfiler().Shutdown(reason);
            applicationUploader = nullptr;
            profiling::BindApplicationProfiler(nullptr);
            applicationSession.reset();
        } else if (kernelSession != nullptr) {
            const CallbackFrame frame;
            status = kernelSession->runtime.ShutdownLocal();
            profiling::BindReplayRuntime(nullptr);
            kernelSession.reset();
        }
        ClearDispatcher();
        ASCTOOL_INFO("[aclpti] collection session released");
    } catch (...) {
        status = ACLPTI_ERROR_INTERNAL;
    }
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        shutdownStatus = status;
        state = SessionState::Stopped;
    }
    controlChanged.notify_all();
    return status;
}

aclptiResult InitializeApplication(
    profiling::ApplicationConsumer consumer, profiling::ApplicationDrainedCallback onDrained)
{
    if (!consumer || !onDrained) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(controlMutex);
    if (state != SessionState::Idle) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    const auto status =
        PrepareApplication(profiling::GetApplicationProfiler(), std::move(consumer), std::move(onDrained), true);
    if (status == ACLPTI_SUCCESS) {
        mode = SessionMode::StandaloneApplication;
        RecordPrepared();
        RecordActive();
    } else {
        handler::ClearRuntimeApiHandlers();
        applicationUploader = nullptr;
    }
    return status;
}

aclptiResult ShutdownApplication(profiling::ApplicationEndReason reason) noexcept
{
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        if (mode != SessionMode::StandaloneApplication && mode != SessionMode::Application) {
            return ACLPTI_ERROR_INVALID_STATE;
        }
    }
    return ShutdownProcessorSession(reason);
}

uint64_t ApplicationDeviceEpoch(int32_t deviceId)
{
    const CallbackAccess access;
    if (!access || (mode != SessionMode::StandaloneApplication && mode != SessionMode::Application)) {
        return 0;
    }
    return profiling::GetApplicationProfiler().DeviceEpoch(deviceId);
}

aclptiResult SetRangeConfig(const aclptiRangeProfilerSetConfigParams* params)
{
    if (params == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(controlMutex);
    if (state == SessionState::Closing || state == SessionState::Stopped) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    if (mode == SessionMode::Application || mode == SessionMode::StandaloneApplication) {
        return profiling::GetApplicationProfiler().SetConfig(params);
    }
    return profiling::GetReplayRuntime().SetConfig(params);
}

aclptiResult InitializeDependencies()
{
    std::lock_guard<std::mutex> lock(controlMutex);
    if (state == SessionState::Prepared || state == SessionState::Active) {
        return ACLPTI_SUCCESS;
    }
    if (state != SessionState::Idle) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    const auto status = PrepareKernel(profiling::GetReplayRuntime(), nullptr, nullptr);
    if (status != ACLPTI_SUCCESS) {
        handler::ClearRuntimeApiHandlers();
        return status;
    }
    mode = SessionMode::StandaloneKernel;
    RecordPrepared();
    RecordActive();
    return ACLPTI_SUCCESS;
}

} // namespace aclpti::initialization
