/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "dispatcher.h"

#include "aclpti/aclpti_runtime_api.h"

namespace aclpti::callback {

aclptiSubscribeHandle Dispatcher::SubscriberHandle() { return &subscriber_; }

bool Dispatcher::IsValidSubscriber(aclptiSubscribeHandle subscriber) const { return subscriber == &subscriber_; }

std::uint64_t Dispatcher::MakeCallbackKey(aclptiCallbackDomain domain, aclptiCallbackId cbid)
{
    return (static_cast<std::uint64_t>(domain) << 32U) | static_cast<std::uint64_t>(cbid);
}

void Dispatcher::Configure(aclptiCallbackFunc callback, void* userData)
{
    std::lock_guard<std::mutex> lock(subscriber_.callbackMutex);
    subscriber_.callback = callback;
    subscriber_.userData = userData;
    subscriber_.enabledCallbacks.clear();
}

aclptiResult Dispatcher::Enable(
    bool enable, aclptiSubscribeHandle subscriber, aclptiCallbackDomain domain, aclptiCallbackId cbid)
{
    if (!IsValidSubscriber(subscriber)) {
        return ACLPTI_ERROR_INVALID_SUBSCRIBER;
    }
    if (domain != ACLPTI_CB_DOMAIN_RUNTIME_API) {
        return ACLPTI_ERROR_NOT_SUPPORTED;
    }
    if (cbid >= ACLPTI_RUNTIME_CBID_SIZE) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }

    std::lock_guard<std::mutex> lock(subscriber_.callbackMutex);
    if (enable && subscriber_.callback == nullptr) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    const auto key = MakeCallbackKey(domain, cbid);
    if (enable) {
        subscriber_.enabledCallbacks.insert(key);
    } else {
        subscriber_.enabledCallbacks.erase(key);
    }
    return ACLPTI_SUCCESS;
}

aclptiResult Dispatcher::SupportedDomains(std::size_t* domainCount, aclptiCallbackDomain* domains) const
{
    if (domainCount == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    if (domains == nullptr) {
        *domainCount = 1;
        return ACLPTI_SUCCESS;
    }
    if (*domainCount < 1) {
        *domainCount = 1;
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    domains[0] = ACLPTI_CB_DOMAIN_RUNTIME_API;
    *domainCount = 1;
    return ACLPTI_SUCCESS;
}

void Dispatcher::Dispatch(
    aclptiCallbackDomain domain, aclptiCallbackId cbid, aclptiCallbackSite site, void* const functionParams,
    aclError retval)
{
    aclptiCallbackFunc callback = nullptr;
    void* userData = nullptr;
    {
        std::lock_guard<std::mutex> lock(subscriber_.callbackMutex);
        if (subscriber_.enabledCallbacks.find(MakeCallbackKey(domain, cbid)) == subscriber_.enabledCallbacks.end()) {
            return;
        }
        callback = subscriber_.callback;
        userData = subscriber_.userData;
    }
    if (callback == nullptr) {
        return;
    }

    const aclptiCallbackData callbackData{domain, cbid, site, functionParams, retval};
    callback(userData, domain, cbid, &callbackData);
}

void Dispatcher::SetActivityEnabled(aclptiSubscribeHandle subscriber, bool enabled)
{
    if (IsValidSubscriber(subscriber)) {
        std::lock_guard<std::mutex> lock(subscriber_.callbackMutex);
        subscriber_.activityEnabled = enabled;
    }
}

Dispatcher& GetDispatcher()
{
    static Dispatcher dispatcher;
    return dispatcher;
}

} // namespace aclpti::callback
