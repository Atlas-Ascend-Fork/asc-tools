/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "aclpti/aclpti.h"

#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

struct CallbackEvent {
    aclptiCallbackDomain domain;
    aclptiCallbackId cbid;
    aclptiCallbackSite site;
    aclError retval;
};

struct CallbackState {
    std::array<CallbackEvent, 12> events{};
    std::size_t eventCount = 0;
    bool receivedExpectedUserData = true;
};

CallbackState g_callbackState;
std::size_t g_mallocSize = 0;
std::size_t g_mallocCalls = 0;
std::size_t g_freeCalls = 0;
std::size_t g_getFuncBySymbolCalls = 0;
std::size_t g_binaryUnLoadCalls = 0;
bool g_failMalloc = false;
const void* g_getFuncBySymbolArgument = nullptr;
aclrtBinHandle g_binaryUnLoadArgument = nullptr;
int g_callbackSymbol = 0;
int g_callbackBinary = 0;

aclError OriginalMalloc(void** devPtr, std::size_t size, aclrtMemMallocPolicy)
{
    ++g_mallocCalls;
    g_mallocSize = size;
    if (g_failMalloc) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *devPtr = std::malloc(size);
    return *devPtr == nullptr ? ACL_ERROR_BAD_ALLOC : ACL_SUCCESS;
}

aclError OriginalFree(void* devPtr)
{
    ++g_freeCalls;
    std::free(devPtr);
    return ACL_SUCCESS;
}

aclError OriginalGetFuncBySymbol(const void* symbol, aclrtFuncHandle* funcHandle)
{
    ++g_getFuncBySymbolCalls;
    g_getFuncBySymbolArgument = symbol;
    if (symbol == nullptr || funcHandle == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *funcHandle = const_cast<void*>(symbol);
    return 31;
}

aclError OriginalBinaryUnLoad(aclrtBinHandle binHandle)
{
    ++g_binaryUnLoadCalls;
    g_binaryUnLoadArgument = binHandle;
    return binHandle == nullptr ? ACL_ERROR_INVALID_PARAM : 32;
}

void RuntimeCallback(
    void* userData, aclptiCallbackDomain domain, aclptiCallbackId cbid, const aclptiCallbackData* callbackData)
{
    auto* state = static_cast<CallbackState*>(userData);
    if (state != &g_callbackState || callbackData == nullptr || callbackData->domain != domain ||
        callbackData->cbid != cbid) {
        g_callbackState.receivedExpectedUserData = false;
        return;
    }
    if (state->eventCount < state->events.size()) {
        state->events[state->eventCount++] = {domain, cbid, callbackData->callbackSite, callbackData->retval};
    }
    if (callbackData->callbackSite == ACLPTI_API_ENTER && cbid == ACLPTI_RUNTIME_CBID_aclrtMalloc) {
        auto* params = static_cast<aclptiAclrtMallocParams*>(callbackData->functionParams);
        params->size = 64;
    }
    if (callbackData->callbackSite == ACLPTI_API_ENTER && cbid == ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol) {
        auto* params = static_cast<aclptiAclrtGetFuncBySymbolParams*>(callbackData->functionParams);
        params->symbol = &g_callbackSymbol;
    }
    if (callbackData->callbackSite == ACLPTI_API_ENTER && cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad) {
        auto* params = static_cast<aclptiAclrtBinaryUnLoadParams*>(callbackData->functionParams);
        params->binHandle = &g_callbackBinary;
    }
}

} // namespace

std::int32_t MsprofStart(uint32_t, const void*, uint32_t) { return 0; }

std::int32_t MsprofStop(uint32_t, const void*, uint32_t) { return 0; }

std::int32_t MsprofRegisterDataCallback(uint32_t, void* function) { return function == nullptr ? -1 : 0; }

static_assert(ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol == 14);
static_assert(ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad == 15);
static_assert(ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs == 16);
static_assert(ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray == 17);
static_assert(ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray == 18);
static_assert(ACLPTI_RUNTIME_CBID_aclrtMallocAlign32 == 19);
static_assert(ACLPTI_RUNTIME_CBID_aclrtMallocWithCfg == 20);
static_assert(ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromFile == 21);
static_assert(ACLPTI_RUNTIME_CBID_SIZE == 22);

TEST(NpuComputeCallbackApi, ListsSupportedDomains)
{
    std::size_t domainCount = 0;
    ASSERT_TRUE(aclptiSupportedDomains(&domainCount, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(domainCount == 1);

    std::array<aclptiCallbackDomain, 1> insufficientDomains{};
    std::size_t insufficientCount = 0;
    ASSERT_TRUE(
        aclptiSupportedDomains(&insufficientCount, insufficientDomains.data()) == ACLPTI_ERROR_INVALID_PARAMETER);
    ASSERT_TRUE(insufficientCount == 1);

    std::array<aclptiCallbackDomain, 1> domains{};
    ASSERT_TRUE(aclptiSupportedDomains(&domainCount, domains.data()) == ACLPTI_SUCCESS);
    ASSERT_TRUE(domainCount == domains.size());
    ASSERT_TRUE(domains[0] == ACLPTI_CB_DOMAIN_RUNTIME_API);
    ASSERT_TRUE(aclptiSupportedDomains(nullptr, nullptr) == ACLPTI_ERROR_INVALID_PARAMETER);
}

TEST(NpuComputeCallbackApi, DispatchesEnabledCallbacksAndSupportsConcurrentSubscribe)
{
    g_callbackState = {};
    g_mallocSize = 0;
    g_mallocCalls = 0;
    g_freeCalls = 0;
    g_getFuncBySymbolCalls = 0;
    g_binaryUnLoadCalls = 0;
    g_failMalloc = false;
    g_getFuncBySymbolArgument = nullptr;
    g_binaryUnLoadArgument = nullptr;

    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMalloc", &OriginalMalloc) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFree", &OriginalFree) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtGetFuncBySymbol", &OriginalGetFuncBySymbol) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryUnLoad", &OriginalBinaryUnLoad) == ACL_SUCCESS);

    constexpr std::size_t subscriberCount = 8;
    std::array<aclptiSubscribeHandle, subscriberCount> subscribers{};
    std::array<aclptiResult, subscriberCount> subscribeResults{};
    std::array<std::thread, subscriberCount> subscribeThreads;
    for (std::size_t index = 0; index < subscriberCount; ++index) {
        subscribeThreads[index] = std::thread([&, index]() {
            subscribeResults[index] = aclptiSubscribe(&subscribers[index], &RuntimeCallback, &g_callbackState, nullptr);
        });
    }
    for (auto& thread : subscribeThreads) {
        thread.join();
    }
    const aclptiSubscribeHandle subscriber = subscribers[0];
    ASSERT_TRUE(subscriber != nullptr);
    for (std::size_t index = 0; index < subscriberCount; ++index) {
        ASSERT_TRUE(subscribeResults[index] == ACLPTI_SUCCESS);
        ASSERT_TRUE(subscribers[index] == subscriber);
    }
    for (aclptiCallbackId cbid = 0; cbid < ACLPTI_RUNTIME_CBID_SIZE; ++cbid) {
        ASSERT_TRUE(aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid) == ACLPTI_SUCCESS);
        ASSERT_TRUE(aclptiEnableCallback(false, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid) == ACLPTI_SUCCESS);
    }
    ASSERT_TRUE(
        aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_SUCCESS);
    ASSERT_TRUE(
        aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_INVALID, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_ERROR_NOT_SUPPORTED);
    ASSERT_TRUE(
        aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_SIZE) ==
        ACLPTI_ERROR_INVALID_PARAMETER);
    ASSERT_TRUE(
        aclptiEnableCallback(true, nullptr, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_ERROR_INVALID_SUBSCRIBER);

    void* allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 8, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    ASSERT_TRUE(allocation != nullptr);
    ASSERT_TRUE(g_callbackState.receivedExpectedUserData);
    ASSERT_TRUE(g_callbackState.eventCount == 2);
    ASSERT_TRUE(g_callbackState.events[0].domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
    ASSERT_TRUE(g_callbackState.events[0].cbid == ACLPTI_RUNTIME_CBID_aclrtMalloc);
    ASSERT_TRUE(g_callbackState.events[0].site == ACLPTI_API_ENTER);
    ASSERT_TRUE(g_callbackState.events[1].site == ACLPTI_API_EXIT);
    ASSERT_TRUE(g_callbackState.events[1].retval == ACL_SUCCESS);
    ASSERT_TRUE(g_mallocCalls == 2);
    ASSERT_TRUE(g_mallocSize == 64);

    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
    ASSERT_TRUE(g_freeCalls == 2);
    ASSERT_TRUE(g_callbackState.eventCount == 2);

    ASSERT_TRUE(
        aclptiEnableCallback(false, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_SUCCESS);
    g_failMalloc = false;
    allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 8, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    ASSERT_TRUE(g_callbackState.eventCount == 2);
    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);

    ASSERT_TRUE(
        aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_SUCCESS);
    g_failMalloc = true;
    allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 8, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_ERROR_INVALID_PARAM);
    ASSERT_TRUE(allocation == nullptr);
    ASSERT_TRUE(g_callbackState.eventCount == 4);
    ASSERT_TRUE(g_callbackState.events[2].site == ACLPTI_API_ENTER);
    ASSERT_TRUE(g_callbackState.events[3].site == ACLPTI_API_EXIT);
    ASSERT_TRUE(g_callbackState.events[3].retval == ACL_ERROR_INVALID_PARAM);

    int originalSymbol = 0;
    aclrtFuncHandle functionHandle = nullptr;
    ASSERT_TRUE(aclrtGetFuncBySymbol(&originalSymbol, &functionHandle) == 31);
    ASSERT_TRUE(functionHandle == &originalSymbol);
    ASSERT_TRUE(g_getFuncBySymbolCalls == 1);
    ASSERT_TRUE(g_getFuncBySymbolArgument == &originalSymbol);
    ASSERT_TRUE(aclrtBinaryUnLoad(&originalSymbol) == 32);
    ASSERT_TRUE(g_binaryUnLoadCalls == 1);
    ASSERT_TRUE(g_binaryUnLoadArgument == &originalSymbol);
    ASSERT_TRUE(
        aclptiEnableCallback(
            true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol) ==
        ACLPTI_SUCCESS);
    ASSERT_TRUE(
        aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad) ==
        ACLPTI_SUCCESS);
    functionHandle = nullptr;
    ASSERT_TRUE(aclrtGetFuncBySymbol(&originalSymbol, &functionHandle) == 31);
    ASSERT_TRUE(functionHandle == &g_callbackSymbol);
    ASSERT_TRUE(g_getFuncBySymbolCalls == 2);
    ASSERT_TRUE(g_getFuncBySymbolArgument == &g_callbackSymbol);
    ASSERT_TRUE(g_callbackState.eventCount == 6);
    ASSERT_TRUE(g_callbackState.events[4].cbid == ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol);
    ASSERT_TRUE(g_callbackState.events[4].site == ACLPTI_API_ENTER);
    ASSERT_TRUE(g_callbackState.events[5].cbid == ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol);
    ASSERT_TRUE(g_callbackState.events[5].site == ACLPTI_API_EXIT);
    ASSERT_TRUE(g_callbackState.events[5].retval == 31);
    ASSERT_TRUE(aclrtBinaryUnLoad(&originalSymbol) == 32);
    ASSERT_TRUE(g_binaryUnLoadCalls == 2);
    ASSERT_TRUE(g_binaryUnLoadArgument == &g_callbackBinary);
    ASSERT_TRUE(g_callbackState.eventCount == 8);
    ASSERT_TRUE(g_callbackState.events[6].cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad);
    ASSERT_TRUE(g_callbackState.events[6].site == ACLPTI_API_ENTER);
    ASSERT_TRUE(g_callbackState.events[7].cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad);
    ASSERT_TRUE(g_callbackState.events[7].site == ACLPTI_API_EXIT);
    ASSERT_TRUE(g_callbackState.events[7].retval == 32);

    ASSERT_TRUE(
        aclptiEnableCallback(
            false, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol) ==
        ACLPTI_SUCCESS);
    ASSERT_TRUE(
        aclptiEnableCallback(false, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad) ==
        ACLPTI_SUCCESS);
    functionHandle = nullptr;
    ASSERT_TRUE(aclrtGetFuncBySymbol(&originalSymbol, &functionHandle) == 31);
    ASSERT_TRUE(functionHandle == &originalSymbol);
    ASSERT_TRUE(g_getFuncBySymbolCalls == 3);
    ASSERT_TRUE(g_getFuncBySymbolArgument == &originalSymbol);
    ASSERT_TRUE(aclrtBinaryUnLoad(&originalSymbol) == 32);
    ASSERT_TRUE(g_binaryUnLoadCalls == 3);
    ASSERT_TRUE(g_binaryUnLoadArgument == &originalSymbol);
    ASSERT_TRUE(g_callbackState.eventCount == 8);

    ASSERT_TRUE(
        aclptiEnableCallback(
            true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol) ==
        ACLPTI_SUCCESS);
    aclptiSubscribeHandle activityOnlySubscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&activityOnlySubscriber, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    functionHandle = nullptr;
    ASSERT_TRUE(aclrtGetFuncBySymbol(&originalSymbol, &functionHandle) == 31);
    ASSERT_TRUE(functionHandle == &originalSymbol);
    ASSERT_TRUE(g_getFuncBySymbolCalls == 4);
    ASSERT_TRUE(g_getFuncBySymbolArgument == &originalSymbol);
    ASSERT_TRUE(g_callbackState.eventCount == 8);
    ASSERT_TRUE(
        aclptiEnableCallback(
            true, activityOnlySubscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_ERROR_INVALID_STATE);
}
