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

#include "acl_san/aclsan_api.h"
#include "aclsan_dispatch.h"
#include "aclsan_device_call_stack.h"
#include "aclsan_runtime_hook.h"

#include <cassert>
#include <cstdint>
#include <set>

namespace {

uint32_t g_callbackCalls = 0;

void CaptureCallback(void*, AclsanCallbackDomain, AclsanCallbackId, const void*) { ++g_callbackCalls; }

} // namespace

// acltoolHookInit / ApplyRuntimeHooks / ResolveActiveDeviceCallStack 的最小桩
// 由 llt/aclsan_boundary_stub 提供默认实现，行为与原桩一致。
TEST(AclsanCallbackInvoke, Main)
{
    AclsanSubscriberHandle subscriber = nullptr;
    assert(aclsanSubscribe(&subscriber, CaptureCallback, nullptr) == ACLSAN_STATUS_SUCCESS);

    uint32_t enabled = 1;
    assert(
        aclsanGetCallbackState(subscriber, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &enabled) ==
        ACLSAN_STATUS_SUCCESS);
    assert(enabled == 0);

    assert(!aclsan::InvokeCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, nullptr));
    assert(g_callbackCalls == 0);

    const AclsanResourceData callbackData{};
    assert(aclsan::InvokeCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &callbackData));
    assert(g_callbackCalls == 0);

    assert(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) ==
        ACLSAN_STATUS_SUCCESS);
    assert(
        aclsanGetCallbackState(subscriber, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &enabled) ==
        ACLSAN_STATUS_SUCCESS);
    assert(enabled == 1);
    assert(aclsan::InvokeCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &callbackData));
    assert(g_callbackCalls == 1);

    assert(
        aclsanEnableCallback(0, subscriber, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) ==
        ACLSAN_STATUS_SUCCESS);
    assert(
        aclsanGetCallbackState(subscriber, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &enabled) ==
        ACLSAN_STATUS_SUCCESS);
    assert(enabled == 0);

    const AclsanLaunchData launchData{};
    assert(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL) ==
        ACLSAN_STATUS_SUCCESS);
    assert(
        aclsanGetCallbackState(subscriber, ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &enabled) ==
        ACLSAN_STATUS_SUCCESS);
    assert(enabled == 1);
    assert(aclsan::InvokeCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launchData));
    assert(g_callbackCalls == 2);

    assert(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
}

TEST(AclsanCallbackInvoke, CompletedHostWriteDomainCanBeEnabledAndDispatched)
{
    AclsanSubscriberHandle subscriber = nullptr;
    uint64_t observedBytes = 0;
    const auto capture = [](void* userdata, AclsanCallbackDomain domain, AclsanCallbackId id, const void* data) {
        EXPECT_EQ(domain, ACLSAN_CB_DOMAIN_MEMORY);
        EXPECT_EQ(id, ACLSAN_CBID_MEMORY_WRITE_END);
        *static_cast<uint64_t*>(userdata) = static_cast<const AclsanMemoryWriteData*>(data)->bytes;
    };
    ASSERT_EQ(aclsanSubscribe(&subscriber, capture, &observedBytes), ACLSAN_STATUS_SUCCESS);
    EXPECT_EQ(aclsanEnableDomain(subscriber, ACLSAN_CB_DOMAIN_MEMORY, 1), ACLSAN_STATUS_SUCCESS);
    uint32_t enabled = 0;
    EXPECT_EQ(
        aclsanGetCallbackState(subscriber, ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, &enabled),
        ACLSAN_STATUS_SUCCESS);
    EXPECT_EQ(enabled, 1U);
    AclsanMemoryWriteData write{};
    write.common.version = ACLSAN_API_VERSION;
    write.common.size = sizeof(write);
    write.bytes = 64;
    EXPECT_TRUE(aclsan::InvokeCallback(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, &write));
    EXPECT_EQ(observedBytes, 64U);
    EXPECT_EQ(aclsanEnableDomain(subscriber, ACLSAN_CB_DOMAIN_MEMORY, 0), ACLSAN_STATUS_SUCCESS);
    write.bytes = 128;
    EXPECT_TRUE(aclsan::InvokeCallback(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, &write));
    EXPECT_EQ(observedBytes, 64U);
    EXPECT_EQ(aclsanUnsubscribe(subscriber), ACLSAN_STATUS_SUCCESS);
}
