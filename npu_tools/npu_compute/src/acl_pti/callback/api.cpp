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
#include "aclpti/aclpti_callback.h"

#include "acl_pti/initialization.h"
#include "dispatcher.h"

extern "C" ACLPTI_EXPORT aclptiResult aclptiSubscribe(
    aclptiSubscribeHandle* subscriber, aclptiCallbackFunc callback, void* userData, aclptiSubscribeParams* pParams)
{
    if (subscriber == nullptr || (pParams != nullptr && pParams->reserved != 0) ||
        (callback == nullptr && userData != nullptr)) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }

    *subscriber = nullptr;
    const aclptiResult result = aclpti::initialization::InitializeDependencies();
    if (result == ACLPTI_SUCCESS) {
        auto& dispatcher = aclpti::callback::GetDispatcher();
        dispatcher.Configure(callback, userData);
        *subscriber = dispatcher.SubscriberHandle();
    }
    ASCTOOL_DEBUG("[aclpti] subscribe result=%d handle=%p", static_cast<int>(result), static_cast<void*>(*subscriber));
    if (result != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] callback operation failed: result=%d"
            " code=%u",
            static_cast<int>(result), static_cast<unsigned>(static_cast<uint32_t>(result)));
    }
    return result;
}

extern "C" ACLPTI_EXPORT aclptiResult
aclptiEnableCallback(bool enable, aclptiSubscribeHandle subscriber, aclptiCallbackDomain domain, aclptiCallbackId cbid)
{
    auto& dispatcher = aclpti::callback::GetDispatcher();
    const aclptiResult result = dispatcher.Enable(enable, subscriber, domain, cbid);
    ASCTOOL_DEBUG(
        "[aclpti] callback enable=%d domain=%d cbid=%u result=%d", static_cast<int>(enable), static_cast<int>(domain),
        static_cast<unsigned int>(cbid), static_cast<int>(result));
    if (result != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] callback operation failed: result=%d"
            " code=%u",
            static_cast<int>(result), static_cast<unsigned>(static_cast<uint32_t>(result)));
    }
    return result;
}

extern "C" ACLPTI_EXPORT aclptiResult aclptiSupportedDomains(std::size_t* domainCount, aclptiCallbackDomain* domains)
{
    const aclptiResult supportedResult = aclpti::callback::GetDispatcher().SupportedDomains(domainCount, domains);
    ASCTOOL_DEBUG(
        "[aclpti] supported domains result=%d count=%zu", static_cast<int>(supportedResult),
        domainCount == nullptr ? 0U : *domainCount);
    if (supportedResult != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] supported domains failed: result=%d"
            " code=%u",
            static_cast<int>(supportedResult), static_cast<unsigned>(static_cast<uint32_t>(supportedResult)));
    }
    return supportedResult;
}
