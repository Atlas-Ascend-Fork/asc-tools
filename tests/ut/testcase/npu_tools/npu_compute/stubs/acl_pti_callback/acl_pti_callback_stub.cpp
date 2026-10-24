/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute/acl_pti_callback_stub.h"
#include "aclpti/aclpti_data.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct aclptiSubscriber_st {
    bool activityEnabled = false;
};

namespace {

std::mutex g_mutex;
aclptiSubscriber_st g_subscriber;
aclptiResult g_subscribeResult = ACLPTI_SUCCESS;
aclptiResult g_rangeConfigResult = ACLPTI_SUCCESS;
std::size_t g_subscribeCount = 0;
aclptiCallbackFunc g_callback = nullptr;
void* g_userData = nullptr;
aclptiSubscribeHandle g_capturedSubscriber = nullptr;
std::vector<npucompute::test::AclPtiEnableCall> g_enableCalls;
std::unordered_map<aclptiCallbackId, aclptiResult> g_enableResults;
std::unordered_set<uint64_t> g_enabledCallbacks;
std::size_t g_callSequence = 0;
std::size_t g_subscribeSequence = 0;
std::size_t g_rangeConfigCount = 0;
std::size_t g_rangeConfigSequence = 0;
std::vector<std::string> g_sections;
aclptiBlockResultMode g_blockResult = ACLPTI_BLOCK_RESULT_DISABLED;
bool g_collectPipeline = false;
bool g_collectPcSampling = false;
aclptiDataModuleShutdownCallback g_shutdownCallback = nullptr;
void* g_shutdownUserData = nullptr;

uint64_t CallbackKey(aclptiCallbackDomain domain, aclptiCallbackId cbid)
{
    return (static_cast<uint64_t>(domain) << 32U) | static_cast<uint64_t>(cbid);
}

} // namespace

extern "C" aclptiResult aclptiRegisterDataModuleShutdownCallback(
    aclptiDataModuleShutdownCallback callback, void* userData)
{
    if (callback == nullptr || g_shutdownCallback != nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    g_shutdownCallback = callback;
    g_shutdownUserData = userData;
    return std::atexit([] { g_shutdownCallback(g_shutdownUserData); }) == 0 ? ACLPTI_SUCCESS : ACLPTI_ERROR_INTERNAL;
}

namespace npucompute::test {

void ResetAclPtiCallbackStub()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_subscribeResult = ACLPTI_SUCCESS;
    g_rangeConfigResult = ACLPTI_SUCCESS;
    g_subscribeCount = 0;
    g_callback = nullptr;
    g_userData = nullptr;
    g_capturedSubscriber = nullptr;
    g_enableCalls.clear();
    g_enableResults.clear();
    g_enabledCallbacks.clear();
    g_callSequence = 0;
    g_subscribeSequence = 0;
    g_rangeConfigCount = 0;
    g_rangeConfigSequence = 0;
    g_sections.clear();
    g_blockResult = ACLPTI_BLOCK_RESULT_DISABLED;
    g_collectPipeline = false;
    g_collectPcSampling = false;
}

void SetAclPtiSubscribeResult(aclptiResult result)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_subscribeResult = result;
}

void SetAclPtiEnableResult(aclptiCallbackId cbid, aclptiResult result)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enableResults[cbid] = result;
}

void SetAclPtiRangeConfigResult(aclptiResult result)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rangeConfigResult = result;
}

std::size_t AclPtiSubscribeCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_subscribeCount;
}

std::size_t AclPtiEnableCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enableCalls.size();
}

std::size_t AclPtiSubscribeSequence()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_subscribeSequence;
}

std::size_t AclPtiRangeConfigCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_rangeConfigCount;
}

std::size_t AclPtiRangeConfigSequence()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_rangeConfigSequence;
}

aclptiCallbackFunc CapturedAclPtiCallback()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_callback;
}

void* CapturedAclPtiUserData()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_userData;
}

aclptiSubscribeHandle CapturedAclPtiSubscriber()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capturedSubscriber;
}

std::vector<AclPtiEnableCall> CapturedAclPtiEnableCalls()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enableCalls;
}

std::vector<std::string> CapturedAclPtiSections()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sections;
}

aclptiBlockResultMode CapturedAclPtiBlockResult()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_blockResult;
}

bool CapturedAclPtiCollectPipeline()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_collectPipeline;
}

bool CapturedAclPtiCollectPcSampling()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_collectPcSampling;
}

bool InvokeAclPtiCallback(
    aclptiCallbackDomain domain, aclptiCallbackId cbid, aclptiCallbackSite site, aclError retval, void* functionParams)
{
    aclptiCallbackFunc callback = nullptr;
    void* userData = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_enabledCallbacks.count(CallbackKey(domain, cbid)) == 0) {
            return false;
        }
        callback = g_callback;
        userData = g_userData;
    }
    if (callback == nullptr) {
        return false;
    }
    const aclptiCallbackData callbackData{domain, cbid, site, functionParams, retval};
    callback(userData, domain, cbid, &callbackData);
    return true;
}

bool InvokeAclPtiRuntimeReady()
{
    return InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr);
}

} // namespace npucompute::test

extern "C" int AclPtiCallbackStubEmitRuntimeEvent(uint32_t cbid, uint32_t site, std::int32_t retval)
{
    if (cbid >= ACLPTI_RUNTIME_CBID_SIZE || site > ACLPTI_API_EXIT) {
        return 0;
    }
    const bool dispatched = npucompute::test::InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, static_cast<aclptiCallbackSite>(site), static_cast<aclError>(retval),
        nullptr);
    std::fprintf(
        stderr, "[acl_pti_callback_stub] event domain=%d cbid=%u site=%u retval=%d dispatched=%d\n",
        static_cast<int>(ACLPTI_CB_DOMAIN_RUNTIME_API), cbid, site, retval, dispatched ? 1 : 0);
    return dispatched ? 1 : 0;
}

extern "C" aclptiResult aclptiSubscribe(
    aclptiSubscribeHandle* subscriber, aclptiCallbackFunc callback, void* userData, aclptiSubscribeParams* params)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_subscribeCount;
    g_subscribeSequence = ++g_callSequence;
    std::fprintf(
        stderr, "[acl_pti_callback_stub] subscribe count=%zu callback=%s userData=%p\n", g_subscribeCount,
        callback == nullptr ? "null" : "set", userData);
    g_capturedSubscriber = nullptr;
    if (subscriber == nullptr || (callback == nullptr && userData != nullptr) ||
        (params != nullptr && params->reserved != 0)) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    *subscriber = nullptr;
    if (g_subscribeResult == ACLPTI_SUCCESS) {
        g_callback = callback;
        g_userData = userData;
        g_enabledCallbacks.clear();
        *subscriber = &g_subscriber;
        g_capturedSubscriber = *subscriber;
    }
    return g_subscribeResult;
}

extern "C" aclptiResult aclptiEnableCallback(
    bool enable, aclptiSubscribeHandle subscriber, aclptiCallbackDomain domain, aclptiCallbackId cbid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    aclptiResult result = ACLPTI_SUCCESS;
    if (subscriber != &g_subscriber) {
        result = ACLPTI_ERROR_INVALID_SUBSCRIBER;
    } else if (domain != ACLPTI_CB_DOMAIN_RUNTIME_API) {
        result = ACLPTI_ERROR_NOT_SUPPORTED;
    } else if (cbid >= ACLPTI_RUNTIME_CBID_SIZE) {
        result = ACLPTI_ERROR_INVALID_PARAMETER;
    } else if (enable && g_callback == nullptr) {
        result = ACLPTI_ERROR_INVALID_STATE;
    } else {
        const auto configuredResult = g_enableResults.find(cbid);
        if (configuredResult != g_enableResults.end()) {
            result = configuredResult->second;
        }
    }

    g_enableCalls.push_back({++g_callSequence, enable, subscriber, domain, cbid, result});
    if (result == ACLPTI_SUCCESS) {
        const uint64_t key = CallbackKey(domain, cbid);
        if (enable) {
            g_enabledCallbacks.insert(key);
        } else {
            g_enabledCallbacks.erase(key);
        }
    }
    std::fprintf(
        stderr, "[acl_pti_callback_stub] enable=%d domain=%d cbid=%u result=%d\n", enable ? 1 : 0,
        static_cast<int>(domain), cbid, static_cast<int>(result));
    return result;
}

extern "C" aclptiResult aclptiRangeProfilerSetConfig(aclptiRangeProfilerSetConfigParams* params)
{
    if (params != nullptr && params->cacheControl != ACLPTI_CACHE_CONTROL_ALL &&
        params->cacheControl != ACLPTI_CACHE_CONTROL_NONE) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_rangeConfigCount;
    g_rangeConfigSequence = ++g_callSequence;
    g_sections.clear();
    g_blockResult = params == nullptr ? ACLPTI_BLOCK_RESULT_DISABLED : params->blockResult;
    g_collectPipeline = params != nullptr && params->collectPipeline;
    g_collectPcSampling = params != nullptr && params->collectPcSampling;
    if (params != nullptr && params->sections != nullptr) {
        for (std::size_t index = 0; index < params->numSections; ++index) {
            if (params->sections[index] != nullptr) {
                g_sections.emplace_back(params->sections[index]);
            }
        }
    }
    std::fprintf(
        stderr, "[acl_pti_callback_stub] range config count=%zu result=%d\n", g_rangeConfigCount,
        static_cast<int>(g_rangeConfigResult));
    return g_rangeConfigResult;
}
