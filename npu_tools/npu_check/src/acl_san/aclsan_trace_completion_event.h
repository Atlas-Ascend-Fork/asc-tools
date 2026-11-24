/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_COMPLETION_EVENT_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_COMPLETION_EVENT_H

#include "acl/acl_rt.h"
#include <mutex>

namespace aclsan {

// Resolved runtime functions are injected here so unavailable APIs can fall back
// to stream synchronization without inventing a completion boundary.
struct TraceCompletionEventApi {
    aclError (*create)(aclrtEvent*) = nullptr;
    aclError (*record)(aclrtEvent, aclrtStream) = nullptr;
    aclError (*query)(aclrtEvent, aclrtEventRecordedStatus*) = nullptr;
    aclError (*destroy)(aclrtEvent) = nullptr;

    bool Available() const noexcept
    {
        return create != nullptr && record != nullptr && query != nullptr && destroy != nullptr;
    }
};

class TraceCompletionEvent final {
public:
    TraceCompletionEvent() = default;
    TraceCompletionEvent(const TraceCompletionEvent&) = delete;
    TraceCompletionEvent& operator=(const TraceCompletionEvent&) = delete;

    bool Record(const TraceCompletionEventApi& api, aclrtStream stream) noexcept
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        if (!api.Available() || event_ != nullptr) {
            return false;
        }
        const aclError created = api.create(&event_);
        if (created != ACL_SUCCESS || event_ == nullptr) {
            uncertain_ = event_ != nullptr;
            return false;
        }
        // A failing enqueue may still have touched the event. Retain ownership
        // until stream completion is known or device reset invalidates it.
        uncertain_ = true;
        if (api.record(event_, stream) != ACL_SUCCESS) {
            return false;
        }
        recorded_ = true;
        return true;
    }

    bool Poll(const TraceCompletionEventApi& api) noexcept
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        if (complete_ || queryFailed_ || !recorded_ || event_ == nullptr || api.query == nullptr) {
            return complete_;
        }
        aclrtEventRecordedStatus status = ACL_EVENT_RECORDED_STATUS_NOT_READY;
        if (api.query(event_, &status) != ACL_SUCCESS) {
            queryFailed_ = true;
        } else if (status == ACL_EVENT_RECORDED_STATUS_COMPLETE) {
            complete_ = true;
        } else if (status != ACL_EVENT_RECORDED_STATUS_NOT_READY) {
            queryFailed_ = true;
        }
        return complete_;
    }

    bool Release(const TraceCompletionEventApi& api, bool streamComplete) noexcept
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        if (event_ == nullptr) {
            return true;
        }
        if ((!complete_ && uncertain_ && !streamComplete) || api.destroy == nullptr ||
            api.destroy(event_) != ACL_SUCCESS) {
            return false;
        }
        event_ = nullptr;
        return true;
    }

    bool ObserveBeforeRelease(const TraceCompletionEventApi& api) const noexcept
    {
        std::unique_lock<std::mutex> lock(eventMutex_, std::try_to_lock);
        if (!lock.owns_lock() || !recorded_) {
            return false;
        }
        aclrtEventRecordedStatus status = ACL_EVENT_RECORDED_STATUS_NOT_READY;
        return complete_ || (event_ != nullptr && api.query != nullptr && api.query(event_, &status) == ACL_SUCCESS &&
                             status == ACL_EVENT_RECORDED_STATUS_COMPLETE);
    }

    bool Complete() const noexcept { return complete_; }
    bool CanConfirm() const noexcept { return recorded_ && !queryFailed_; }
    bool QueryFailed() const noexcept { return queryFailed_; }
    bool HasEvent() const noexcept { return event_ != nullptr; }

private:
    mutable std::mutex eventMutex_;
    aclrtEvent event_ = nullptr;
    bool uncertain_ = false;
    bool recorded_ = false;
    bool complete_ = false;
    bool queryFailed_ = false;
};

} // namespace aclsan
#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_COMPLETION_EVENT_H
