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

#include "npu_compute/acl_pti_callback_stub.h"

#include <cstdio>
#include <vector>

namespace {

struct CallbackEvent {
    aclptiCallbackDomain domain;
    aclptiCallbackId cbid;
    aclptiCallbackSite site;
    aclError retval;
    void* functionParams;
};

struct CallbackState {
    std::vector<CallbackEvent> events;
};

void CaptureCallback(
    void* userData, aclptiCallbackDomain domain, aclptiCallbackId cbid, const aclptiCallbackData* callbackData)
{
    auto* state = static_cast<CallbackState*>(userData);
    if (state == nullptr || callbackData == nullptr) {
        return;
    }
    state->events.push_back(
        {domain, cbid, callbackData->callbackSite, callbackData->retval, callbackData->functionParams});
}

} // namespace

TEST(NpuComputeCallbackStub, RecordsEnableOrderAndDispatchesOnlyEnabledCallbacks)
{
    npucompute::test::ResetAclPtiCallbackStub();

    CallbackState state;
    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&subscriber, &CaptureCallback, &state, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(subscriber != nullptr);

    int functionParams = 7;
    ASSERT_TRUE(!npucompute::test::InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc, ACLPTI_API_ENTER, ACL_SUCCESS, &functionParams));
    ASSERT_TRUE(state.events.empty());

    ASSERT_TRUE(
        aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_SUCCESS);
    ASSERT_TRUE(npucompute::test::AclPtiEnableCount() == 1);

    const std::vector<npucompute::test::AclPtiEnableCall> enableCalls = npucompute::test::CapturedAclPtiEnableCalls();
    ASSERT_TRUE(enableCalls.size() == 1);
    ASSERT_TRUE(enableCalls[0].sequence > npucompute::test::AclPtiSubscribeSequence());
    ASSERT_TRUE(enableCalls[0].enable);
    ASSERT_TRUE(enableCalls[0].subscriber == subscriber);
    ASSERT_TRUE(enableCalls[0].domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
    ASSERT_TRUE(enableCalls[0].cbid == ACLPTI_RUNTIME_CBID_aclrtMalloc);

    ASSERT_TRUE(npucompute::test::InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc, ACLPTI_API_ENTER, ACL_SUCCESS, &functionParams));
    ASSERT_TRUE(npucompute::test::InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc, ACLPTI_API_EXIT, ACL_ERROR_INVALID_PARAM,
        &functionParams));
    ASSERT_TRUE(npucompute::test::InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc, ACLPTI_API_EXIT, ACL_SUCCESS, &functionParams));
    ASSERT_TRUE(state.events.size() == 3);
    ASSERT_TRUE(state.events[0].domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
    ASSERT_TRUE(state.events[0].cbid == ACLPTI_RUNTIME_CBID_aclrtMalloc);
    ASSERT_TRUE(state.events[0].site == ACLPTI_API_ENTER);
    ASSERT_TRUE(state.events[0].functionParams == &functionParams);
    ASSERT_TRUE(state.events[1].site == ACLPTI_API_EXIT);
    ASSERT_TRUE(state.events[1].retval == ACL_ERROR_INVALID_PARAM);
    ASSERT_TRUE(state.events[2].site == ACLPTI_API_EXIT);
    ASSERT_TRUE(state.events[2].retval == ACL_SUCCESS);

    ASSERT_TRUE(!npucompute::test::InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtSetDevice, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr));
    ASSERT_TRUE(state.events.size() == 3);

    ASSERT_TRUE(
        aclptiEnableCallback(false, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc) ==
        ACLPTI_SUCCESS);
    ASSERT_TRUE(!npucompute::test::InvokeAclPtiCallback(
        ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMalloc, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr));
    ASSERT_TRUE(state.events.size() == 3);
}
