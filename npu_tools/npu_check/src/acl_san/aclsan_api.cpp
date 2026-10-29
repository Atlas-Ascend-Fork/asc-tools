/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include "securec.h"
#include "device_runtime/device_symbolizer.h"
#include "aclsan_active_probe_plan.h"
#include "aclsan_dispatch.h"
#include "aclsan_device_call_stack.h"
#include "npu_tool_log.h"
#include "aclsan_runtime_hook.h"
#include "injection/injection_hook.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

#define ACLSAN_CHECK_ACTIVE_SUBSCRIBER(apiName, subscriber)                \
    do {                                                                   \
        if (!IsActive((subscriber))) {                                     \
            ASCTOOL_ERROR("%s: subscriber is not initialized", (apiName)); \
            return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;                  \
        }                                                                  \
    } while (false)

namespace {

template <size_t BufferBytes>
bool CopyText(const std::string& text, char (&buffer)[BufferBytes])
{
    const size_t copyBytes = std::min(text.size(), BufferBytes - 1);
    if (memcpy_s(buffer, BufferBytes, text.data(), copyBytes) != EOK) {
        buffer[0] = '\0';
        return false;
    }
    buffer[copyBytes] = '\0';
    return copyBytes == text.size();
}

AclsanStatus UnavailableStatus(const std::string& error)
{
    if (error == "invalid_state" || error == "invalid_configuration") {
        return ACLSAN_STATUS_ERROR_INVALID_STATE;
    }
    if (error == "invalid_symbolizer_output") {
        return ACLSAN_STATUS_ERROR_INTERNAL;
    }
    return ACLSAN_STATUS_ERROR_RUNTIME;
}

[[noreturn]] void AbortConfigurationException(const char* operation, const char* reason) noexcept
{
    ASCTOOL_ERROR("[FATAL] npucheck internal failure: operation=%s reason=%s", operation, reason);
    std::abort();
}

struct CallbackKey {
    AclsanCallbackDomain domain;
    AclsanCallbackId id;

    bool operator<(const CallbackKey& other) const noexcept
    {
        return std::tie(domain, id) < std::tie(other.domain, other.id);
    }
};

// 记录每组 domain + callback id 需要开启 Hook 的 Runtime API。
const std::map<CallbackKey, std::vector<aclrtApiId>> CALLBACK_ROUTES = {
    {{ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END}, {ACL_RT_API_aclrtMemset, ACL_RT_API_aclrtMemcpy}},
    {{ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC},
     {ACL_RT_API_aclrtMalloc, ACL_RT_API_aclrtMallocAlign32, ACL_RT_API_aclrtMallocWithCfg}},
    {{ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE}, {ACL_RT_API_aclrtFree}},
    {{ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
     {ACL_RT_API_aclrtBinaryLoadFromData, ACL_RT_API_aclrtBinaryGetFunction, ACL_RT_API_aclrtBinaryGetFunctionByEntry,
      ACL_RT_API_aclrtLaunchKernelWithHostArgs, ACL_RT_API_aclrtLaunchKernelWithArgsArray,
      ACL_RT_API_aclrtSynchronizeStream, ACL_RT_API_aclrtSynchronizeStreamWithTimeout, ACL_RT_API_aclrtGetFuncBySymbol,
      ACL_RT_API_aclrtBinaryUnLoad, ACL_RT_API_aclrtResetDevice, ACL_RT_API_aclrtMalloc, ACL_RT_API_aclrtMallocWithCfg,
      ACL_RT_API_aclrtMallocAlign32, ACL_RT_API_aclrtBinaryLoadFromFile}},
    {{ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
     {ACL_RT_API_aclrtBinaryLoadFromData, ACL_RT_API_aclrtBinaryGetFunction, ACL_RT_API_aclrtBinaryGetFunctionByEntry,
      ACL_RT_API_aclrtLaunchKernelWithHostArgs, ACL_RT_API_aclrtLaunchKernelWithArgsArray,
      ACL_RT_API_aclrtSynchronizeStream, ACL_RT_API_aclrtSynchronizeStreamWithTimeout, ACL_RT_API_aclrtGetFuncBySymbol,
      ACL_RT_API_aclrtBinaryUnLoad, ACL_RT_API_aclrtResetDevice, ACL_RT_API_aclrtBinaryLoadFromFile}},
    {{ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE},
     {ACL_RT_API_aclrtBinaryLoadFromData, ACL_RT_API_aclrtBinaryGetFunction, ACL_RT_API_aclrtBinaryGetFunctionByEntry,
      ACL_RT_API_aclrtLaunchKernelWithHostArgs, ACL_RT_API_aclrtLaunchKernelWithArgsArray,
      ACL_RT_API_aclrtSynchronizeStream, ACL_RT_API_aclrtSynchronizeStreamWithTimeout, ACL_RT_API_aclrtGetFuncBySymbol,
      ACL_RT_API_aclrtBinaryUnLoad, ACL_RT_API_aclrtResetDevice, ACL_RT_API_aclrtBinaryLoadFromFile}},
    {{ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER},
     {ACL_RT_API_aclrtBinaryLoadFromData, ACL_RT_API_aclrtBinaryGetFunction, ACL_RT_API_aclrtBinaryGetFunctionByEntry,
      ACL_RT_API_aclrtLaunchKernelWithHostArgs, ACL_RT_API_aclrtLaunchKernelWithArgsArray,
      ACL_RT_API_aclrtSynchronizeStream, ACL_RT_API_aclrtSynchronizeStreamWithTimeout, ACL_RT_API_aclrtGetFuncBySymbol,
      ACL_RT_API_aclrtBinaryUnLoad, ACL_RT_API_aclrtResetDevice, ACL_RT_API_aclrtBinaryLoadFromFile}},
    {{ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END},
     {ACL_RT_API_aclrtSynchronizeStream, ACL_RT_API_aclrtSynchronizeStreamWithTimeout}},
    {{ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
     {ACL_RT_API_aclrtBinaryLoadFromData, ACL_RT_API_aclrtBinaryGetFunction, ACL_RT_API_aclrtBinaryGetFunctionByEntry,
      ACL_RT_API_aclrtGetFuncBySymbol, ACL_RT_API_aclrtLaunchKernelWithHostArgs,
      ACL_RT_API_aclrtLaunchKernelWithArgsArray, ACL_RT_API_aclrtBinaryUnLoad, ACL_RT_API_aclrtResetDevice,
      ACL_RT_API_aclrtBinaryLoadFromFile}},
    {{ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL_BEFORE},
     {ACL_RT_API_aclrtLaunchKernelWithHostArgs, ACL_RT_API_aclrtLaunchKernelWithArgsArray}},
};

} // namespace

struct AclsanSubscriber final {
public:
    static AclsanSubscriber& Instance() noexcept;

    AclsanStatus Subscribe(AclsanSubscriberHandle* subscriber, AclsanCallbackFunc callback, void* userdata);
    AclsanStatus EnableCallback(
        uint32_t enable, AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, AclsanCallbackId id);
    AclsanStatus Unsubscribe(AclsanSubscriberHandle subscriber) noexcept;
    AclsanStatus EnableDomain(AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, uint32_t enable);
    AclsanStatus GetCallbackState(
        AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, uint32_t cbid,
        uint32_t* enabled) const noexcept;
    bool IsCallbackEnabled(AclsanCallbackDomain domain, AclsanCallbackId id) const noexcept;
    bool InvokeCallback(AclsanCallbackDomain domain, AclsanCallbackId id, const void* callbackData) const noexcept;

private:
    AclsanSubscriber() = default;
    ~AclsanSubscriber() = default;
    AclsanSubscriber(const AclsanSubscriber&) = delete;
    AclsanSubscriber& operator=(const AclsanSubscriber&) = delete;
    AclsanSubscriber(AclsanSubscriber&&) = delete;
    AclsanSubscriber& operator=(AclsanSubscriber&&) = delete;

    static bool IsValidCallbackId(AclsanCallbackDomain domain, AclsanCallbackId callbackId) noexcept;
    static bool IsValidCallbackDomain(AclsanCallbackDomain domain) noexcept;
    static std::set<aclrtApiId> ComputeRequiredHooks(const std::set<CallbackKey>& enabledCallbacks);
    static uint32_t ComputeRequiredProbeGroups(const std::set<CallbackKey>& enabledCallbacks) noexcept;

    bool IsActive(AclsanSubscriberHandle subscriber) const noexcept;
    void LogConfigurationState(const char* operation, const char* stage) const noexcept;
    AclsanStatus ApplyCallbackConfiguration(std::set<CallbackKey>& candidateCallbacks, const char* operation);
    AclsanStatus UpdateCallbackConfiguration(const CallbackKey& key, bool shouldEnable);

    AclsanSubscriberHandle activeHandle_ = nullptr;
    AclsanCallbackFunc callback_ = nullptr;
    void* userdata_ = nullptr;
    std::set<CallbackKey> enabledCallbacks_;
    std::set<aclrtApiId> requiredHooks_;
};

// 获取进程内唯一的AclsanSubscriber对象
AclsanSubscriber& AclsanSubscriber::Instance() noexcept
{
    static AclsanSubscriber subscriber;
    return subscriber;
}

bool AclsanSubscriber::IsValidCallbackId(AclsanCallbackDomain domain, AclsanCallbackId callbackId) noexcept
{
    switch (domain) {
        case ACLSAN_CB_DOMAIN_MEMORY:
            return callbackId == ACLSAN_CBID_MEMORY_WRITE_END;
        case ACLSAN_CB_DOMAIN_RESOURCE:
            return callbackId >= ACLSAN_CBID_RESOURCE_MEMORY_ALLOC && callbackId <= ACLSAN_CBID_RESOURCE_MEMORY_FREE;
        case ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION:
            return (callbackId >= ACLSAN_CBID_DEVICE_MEMORY_ACCESS && callbackId <= ACLSAN_CBID_DEVICE_STATE) ||
                   callbackId == ACLSAN_CBID_DEVICE_PIPE_BARRIER;
        case ACLSAN_CB_DOMAIN_SYNCHRONIZE:
            return callbackId == ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END;
        case ACLSAN_CB_DOMAIN_LAUNCH:
            return callbackId == ACLSAN_CBID_LAUNCH_KERNEL || callbackId == ACLSAN_CBID_LAUNCH_KERNEL_BEFORE;
        default:
            return false;
    }
}

bool AclsanSubscriber::IsValidCallbackDomain(AclsanCallbackDomain domain) noexcept
{
    return domain == ACLSAN_CB_DOMAIN_RESOURCE || domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION ||
           domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE || domain == ACLSAN_CB_DOMAIN_LAUNCH ||
           domain == ACLSAN_CB_DOMAIN_MEMORY;
}

// 根据现在订阅的domain + id，确认哪些aclrt函数需要被hook
std::set<aclrtApiId> AclsanSubscriber::ComputeRequiredHooks(const std::set<CallbackKey>& enabledCallbacks)
{
    std::set<aclrtApiId> requiredHooks;
    for (const CallbackKey& key : enabledCallbacks) {
        const auto route = CALLBACK_ROUTES.find(key);
        if (route == CALLBACK_ROUTES.end()) {
            ASCTOOL_ERROR(
                "ComputeRequiredHooks: callback route not found for domain=%u id=%u", static_cast<uint32_t>(key.domain),
                static_cast<uint32_t>(key.id));
            continue;
        }
        for (aclrtApiId apiId : route->second) {
            requiredHooks.insert(apiId);
        }
    }
    return requiredHooks;
}

uint32_t AclsanSubscriber::ComputeRequiredProbeGroups(const std::set<CallbackKey>& enabledCallbacks) noexcept
{
    uint32_t probeGroupMask = 0;
    for (const CallbackKey& key : enabledCallbacks) {
        probeGroupMask |= aclsan::ProbeGroupMaskForCallback(key.domain, key.id);
    }
    return probeGroupMask;
}

bool AclsanSubscriber::IsActive(AclsanSubscriberHandle subscriber) const noexcept
{
    return activeHandle_ != nullptr && subscriber == activeHandle_;
}

void AclsanSubscriber::LogConfigurationState(const char* operation, const char* stage) const noexcept
{
    ASCTOOL_DEBUG(
        "operation=%s stage=%s enabled_callbacks=%zu required_hooks=%zu", operation, stage, enabledCallbacks_.size(),
        requiredHooks_.size());
    for (const CallbackKey& key : enabledCallbacks_) {
        ASCTOOL_DEBUG(
            "operation=%s callback_domain=%u callback_id=%u", operation, static_cast<uint32_t>(key.domain),
            static_cast<uint32_t>(key.id));
    }
    for (aclrtApiId apiId : requiredHooks_) {
        ASCTOOL_DEBUG("operation=%s required_hook_api_id=%u", operation, static_cast<uint32_t>(apiId));
    }
}

AclsanStatus AclsanSubscriber::ApplyCallbackConfiguration(
    std::set<CallbackKey>& candidateCallbacks, const char* operation)
{
    std::set<aclrtApiId> candidateRequiredHooks = ComputeRequiredHooks(candidateCallbacks);
    aclsan::CommitActiveProbePlan(ComputeRequiredProbeGroups(candidateCallbacks));
    aclsan::ApplyRuntimeHooks(candidateRequiredHooks);
    enabledCallbacks_.swap(candidateCallbacks);
    requiredHooks_.swap(candidateRequiredHooks);
    LogConfigurationState(operation, "committed");
    return ACLSAN_STATUS_SUCCESS;
}

// 根据domain + id + enable更新subscriber状态，记录已启用的callback，计算哪些aclrt接口需要被hook
AclsanStatus AclsanSubscriber::UpdateCallbackConfiguration(const CallbackKey& key, bool shouldEnable)
{
    try {
        std::unique_lock<std::shared_mutex> planLock(aclsan::ActiveProbePlanMutex());
        // 基于当前已启用的 domain + id 构建本次候选状态。
        std::set<CallbackKey> candidateCallbacks = enabledCallbacks_;
        if (shouldEnable) {
            candidateCallbacks.insert(key);
        } else {
            candidateCallbacks.erase(key);
        }

        return ApplyCallbackConfiguration(candidateCallbacks, "aclsanEnableCallback");
    } catch (const std::bad_alloc&) {
        AbortConfigurationException("aclsanEnableCallback", "out of memory");
    } catch (...) {
        AbortConfigurationException("aclsanEnableCallback", "unexpected C++ exception");
    }
}

AclsanStatus AclsanSubscriber::Subscribe(
    AclsanSubscriberHandle* subscriber, AclsanCallbackFunc callback, void* userdata)
{
    if (subscriber == nullptr) {
        ASCTOOL_ERROR("%s: %s is nullptr", "aclsanSubscribe", "subscriber");
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (callback == nullptr) {
        ASCTOOL_ERROR("%s: %s is nullptr", "aclsanSubscribe", "callback");
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (activeHandle_ != nullptr) {
        ASCTOOL_ERROR("aclsanSubscribe: subscriber already exists");
        return ACLSAN_STATUS_ERROR_ALREADY_SUBSCRIBED;
    }
    if (acltoolHookInit() != 0) {
        ASCTOOL_ERROR("aclsanSubscribe: acltoolHookInit failed");
        return ACLSAN_STATUS_ERROR_INJECTION_FAILED;
    }

    activeHandle_ = this;
    callback_ = callback;
    userdata_ = userdata;
    *subscriber = activeHandle_;
    ASCTOOL_INFO("aclsanSubscribe succeed");
    return ACLSAN_STATUS_SUCCESS;
}

AclsanStatus AclsanSubscriber::EnableCallback(
    uint32_t enable, AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, AclsanCallbackId id)
{
    ACLSAN_CHECK_ACTIVE_SUBSCRIBER("aclsanEnableCallback", subscriber);
    ASCTOOL_DEBUG(
        "aclsanEnableCallback start: enable=%u domain=%u id=%u", enable, static_cast<uint32_t>(domain),
        static_cast<uint32_t>(id));
    if (!IsValidCallbackId(domain, id)) {
        ASCTOOL_ERROR(
            "aclsanEnableCallback: invalid domain + id combination, enable=%u domain=%u id=%u", enable,
            static_cast<uint32_t>(domain), static_cast<uint32_t>(id));
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }

    return UpdateCallbackConfiguration(CallbackKey{domain, id}, enable != 0);
}

AclsanStatus AclsanSubscriber::Unsubscribe(AclsanSubscriberHandle subscriber) noexcept
{
    ACLSAN_CHECK_ACTIVE_SUBSCRIBER("aclsanUnsubscribe", subscriber);

    std::unique_lock<std::shared_mutex> planLock(aclsan::ActiveProbePlanMutex());
    LogConfigurationState("aclsanUnsubscribe", "before-reset");
    aclsan::ApplyRuntimeHooks({});
    activeHandle_ = nullptr;
    callback_ = nullptr;
    userdata_ = nullptr;
    enabledCallbacks_.clear();
    requiredHooks_.clear();
    aclsan::CommitActiveProbePlan(0);
    LogConfigurationState("aclsanUnsubscribe", "after-reset");
    ASCTOOL_INFO("aclsanUnsubscribe succeed");
    return ACLSAN_STATUS_SUCCESS;
}

AclsanStatus AclsanSubscriber::EnableDomain(
    AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, uint32_t enable)
{
    ACLSAN_CHECK_ACTIVE_SUBSCRIBER("aclsanEnableDomain", subscriber);
    if (!IsValidCallbackDomain(domain)) {
        ASCTOOL_ERROR("aclsanEnableDomain: invalid domain=%u", static_cast<uint32_t>(domain));
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }

    try {
        std::unique_lock<std::shared_mutex> planLock(aclsan::ActiveProbePlanMutex());
        std::set<CallbackKey> candidateCallbacks = enabledCallbacks_;
        for (const auto& route : CALLBACK_ROUTES) {
            if (route.first.domain != domain) {
                continue;
            }
            if (enable != 0) {
                candidateCallbacks.insert(route.first);
            } else {
                candidateCallbacks.erase(route.first);
            }
        }
        return ApplyCallbackConfiguration(candidateCallbacks, "aclsanEnableDomain");
    } catch (const std::bad_alloc&) {
        AbortConfigurationException("aclsanEnableDomain", "out of memory");
    } catch (...) {
        AbortConfigurationException("aclsanEnableDomain", "unexpected C++ exception");
    }
}

AclsanStatus AclsanSubscriber::GetCallbackState(
    AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, uint32_t cbid, uint32_t* enabled) const noexcept
{
    if (enabled == nullptr) {
        ASCTOOL_ERROR("%s: %s is nullptr", "aclsanGetCallbackState", "enabled");
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }
    ACLSAN_CHECK_ACTIVE_SUBSCRIBER("aclsanGetCallbackState", subscriber);

    const AclsanCallbackId id = static_cast<AclsanCallbackId>(cbid);
    if (!IsValidCallbackId(domain, id)) {
        ASCTOOL_ERROR(
            "aclsanGetCallbackState: invalid domain + id combination, domain=%u id=%u", static_cast<uint32_t>(domain),
            cbid);
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }
    *enabled = IsCallbackEnabled(domain, id) ? 1 : 0;
    return ACLSAN_STATUS_SUCCESS;
}

bool AclsanSubscriber::IsCallbackEnabled(AclsanCallbackDomain domain, AclsanCallbackId id) const noexcept
{
    return activeHandle_ != nullptr && enabledCallbacks_.count(CallbackKey{domain, id}) != 0;
}

bool AclsanSubscriber::InvokeCallback(
    AclsanCallbackDomain domain, AclsanCallbackId id, const void* callbackData) const noexcept
{
    if (callbackData == nullptr) {
        ASCTOOL_ERROR("InvokeCallback: callback data is nullptr");
        return false;
    }

    if (callback_ == nullptr) {
        ASCTOOL_ERROR("InvokeCallback: callback is nullptr");
        return false;
    }

    // 如果对应domain和id用户没有主动订阅，那么就不传回cbdata
    if (!IsCallbackEnabled(domain, id)) {
        ASCTOOL_DEBUG(
            "InvokeCallback: domain=%u id=%u is not enabled. No call for callback func", static_cast<uint32_t>(domain),
            static_cast<uint32_t>(id));
        return true;
    }

    try {
        callback_(userdata_, domain, id, callbackData);
    } catch (...) {
        ASCTOOL_ERROR(
            "InvokeCallback: domain=%u id=%u failed", static_cast<uint32_t>(domain), static_cast<uint32_t>(id));
        return false;
    }
    return true;
}

// 创建唯一subscriber，更新callback函数。该步骤还会刷新aclrt的hook表
extern "C" AclsanStatus aclsanSubscribe(AclsanSubscriberHandle* subscriber, AclsanCallbackFunc callback, void* userdata)
{
    return AclsanSubscriber::Instance().Subscribe(subscriber, callback, userdata);
}

// 更新显式callback route，并开启/关闭相关aclrt接口的hook
extern "C" AclsanStatus aclsanEnableCallback(
    uint32_t enable, AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, AclsanCallbackId cbId)
{
    return AclsanSubscriber::Instance().EnableCallback(enable, subscriber, domain, cbId);
}

extern "C" AclsanStatus aclsanUnsubscribe(AclsanSubscriberHandle subscriber)
{
    return AclsanSubscriber::Instance().Unsubscribe(subscriber);
}

extern "C" AclsanStatus aclsanEnableDomain(
    AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, uint32_t enable)
{
    return AclsanSubscriber::Instance().EnableDomain(subscriber, domain, enable);
}

extern "C" AclsanStatus aclsanGetCallbackState(
    AclsanSubscriberHandle subscriber, AclsanCallbackDomain domain, AclsanCallbackId cbid, uint32_t* enabled)
{
    return AclsanSubscriber::Instance().GetCallbackState(subscriber, domain, cbid, enabled);
}

extern "C" AclsanStatus aclsanGetDeviceCallStack(uint64_t pc, AclsanDeviceCallStack* result)
{
    if (result == nullptr) {
        ASCTOOL_ERROR("%s: %s is nullptr", "aclsanGetDeviceCallStack", "result");
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }
    *result = {};
    result->pc = pc;

    aclsan::device_runtime::CallStackResult resolved;
    const AclsanStatus resolveStatus = aclsan::ResolveActiveDeviceCallStack(pc, &resolved);
    result->binaryId = resolved.binaryId;
    if (resolveStatus != ACLSAN_STATUS_SUCCESS) {
        return resolveStatus;
    }
    if (!resolved.available) {
        return UnavailableStatus(resolved.error);
    }

    const size_t frameCount = std::min(resolved.frames.size(), static_cast<size_t>(ACLSAN_CALL_STACK_MAX_DEPTH));
    result->depth = static_cast<uint32_t>(frameCount);
    bool complete = frameCount == resolved.frames.size();
    for (size_t index = 0; index < frameCount; ++index) {
        const aclsan::device_runtime::CallStackFrame& source = resolved.frames[index];
        AclsanDeviceCallStackFrame& destination = result->frames[index];
        destination.line = source.line;
        destination.column = source.column;
        destination.inlineDepth = source.inlineDepth;
        complete = CopyText(source.functionName, destination.functionName) && complete;
        complete = CopyText(source.fileName, destination.fileName) && complete;
    }
    if (!complete) {
        result->flags |= static_cast<uint32_t>(ACLSAN_CALL_STACK_FLAG_TRUNCATED);
        return ACLSAN_STATUS_ERROR_MAX_LIMIT_REACHED;
    }
    return ACLSAN_STATUS_SUCCESS;
}

namespace aclsan {

// 在 libsanitizer_api 内部通过 subscriber 保存的 callback 和 userdata 派发已启用键。
bool InvokeCallback(AclsanCallbackDomain domain, AclsanCallbackId cbId, const void* cbData) noexcept
{
    return AclsanSubscriber::Instance().InvokeCallback(domain, cbId, cbData);
}

} // namespace aclsan
