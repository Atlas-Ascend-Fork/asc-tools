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
 * @file dispatcher.h
 * @brief Stores subscriber state and dispatches enabled runtime API callbacks.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_CALLBACK_DISPATCHER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_CALLBACK_DISPATCHER_H

#include "aclpti/aclpti_callback.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

struct aclptiSubscriber_st {
    bool activityEnabled = false;
    aclptiCallbackFunc callback = nullptr;
    void* userData = nullptr;
    std::unordered_set<std::uint64_t> enabledCallbacks;
    std::mutex callbackMutex;
};

namespace aclpti::callback {

class Dispatcher {
public:
    /// Returns the handle of the process-wide subscriber state.
    aclptiSubscribeHandle SubscriberHandle();

    /// Reports whether a handle identifies the process-wide subscriber.
    bool IsValidSubscriber(aclptiSubscribeHandle subscriber) const;

    /// Replaces the subscriber callback and user data and clears enabled callbacks.
    void Configure(aclptiCallbackFunc callback, void* userData);

    /// Enables or disables delivery for one callback domain and callback ID.
    aclptiResult Enable(
        bool enable, aclptiSubscribeHandle subscriber, aclptiCallbackDomain domain, aclptiCallbackId cbid);

    /// Writes the callback domains supported by this dispatcher.
    aclptiResult SupportedDomains(std::size_t* domainCount, aclptiCallbackDomain* domains) const;

    /// Delivers one runtime API callback event when its callback ID is enabled.
    void Dispatch(
        aclptiCallbackDomain domain, aclptiCallbackId cbid, aclptiCallbackSite site, void* const functionParams,
        aclError retval);

    /// Updates activity collection state for a valid subscriber.
    void SetActivityEnabled(aclptiSubscribeHandle subscriber, bool enabled);

private:
    /// Combines a callback domain and callback ID into an enabled-set key.
    static std::uint64_t MakeCallbackKey(aclptiCallbackDomain domain, aclptiCallbackId cbid);

    aclptiSubscriber_st subscriber_;
};

/// Returns the process-wide callback dispatcher.
Dispatcher& GetDispatcher();

} // namespace aclpti::callback

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_CALLBACK_DISPATCHER_H
