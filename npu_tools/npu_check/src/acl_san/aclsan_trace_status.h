/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_STATUS_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_STATUS_H

#include <atomic>
#include <cstdint>
#include <limits>

namespace aclsan {
struct TraceSessionStatus {
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> collectionFailures{0};
    std::atomic<uint64_t> launched{0};
    std::atomic<uint64_t> failedLaunches{0};
    std::atomic<uint64_t> completed{0};
};

inline TraceSessionStatus& TraceStatus() noexcept
{
    static TraceSessionStatus status;
    return status;
}

inline void TraceStatusAdd(std::atomic<uint64_t>& value, uint64_t delta) noexcept
{
    uint64_t old = value.load(std::memory_order_relaxed);
    do {
        const uint64_t next =
            delta > std::numeric_limits<uint64_t>::max() - old ? std::numeric_limits<uint64_t>::max() : old + delta;
        if (value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {
            return;
        }
    } while (true);
}

inline void TraceIntegrityMark(uint64_t launchId, uint64_t dropped, bool failure) noexcept
{
    TraceSessionStatus& status = TraceStatus();
    TraceStatusAdd(status.dropped, dropped);
    if (failure) {
        TraceStatusAdd(status.collectionFailures, 1U);
        if (launchId != 0U) {
            TraceStatusAdd(status.failedLaunches, 1U);
        }
    }
}

inline void TraceIntegrityRegisterLaunch(uint64_t) noexcept { TraceStatusAdd(TraceStatus().launched, 1U); }

inline void TraceStatusCompleteLaunch() noexcept { TraceStatusAdd(TraceStatus().completed, 1U); }

inline void ResetTraceSessionStatus() noexcept
{
    TraceSessionStatus& status = TraceStatus();
    status.dropped.store(0U, std::memory_order_relaxed);
    status.collectionFailures.store(0U, std::memory_order_relaxed);
    status.launched.store(0U, std::memory_order_relaxed);
    status.failedLaunches.store(0U, std::memory_order_relaxed);
    status.completed.store(0U, std::memory_order_relaxed);
}
} // namespace aclsan
#endif
