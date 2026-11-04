/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "checker/memory_resources.h"
#include <algorithm>
#include <limits>

namespace npucheck {
AllocationUpdateStatus MemoryResources::Apply(
    AclsanCallbackId id, const AclsanResourceData& data, MemoryState& state, size_t node)
{
    const auto address = reinterpret_cast<uintptr_t>(data.ptr);
    const InitStateKey scope{data.deviceId, 0};
    const auto status = id == ACLSAN_CBID_RESOURCE_MEMORY_FREE ?
                            state.Release(scope, data.resourceId, address, node) :
                            state.Register(scope, data.resourceId, address, data.bytes, node);
    complete_ = complete_ && state.Allocations(scope).Complete();
    return status;
}

RangeResult MemoryResources::Resolve(
    const MemoryLocation& location, size_t entry, size_t exit, HostOrder& order, MemoryState& state)
{
    if (location.bytes == 0)
        return {RangeStatus::VALID, std::nullopt};
    if (location.bytes > std::numeric_limits<uint64_t>::max() - location.address)
        return {RangeStatus::OVERFLOW, std::nullopt};
    if (!complete_)
        return {RangeStatus::AMBIGUOUS, std::nullopt};
    const auto end = location.address + location.bytes;

    // One ledger per device: walk the recorded generations newest first. A record
    // carries both the geometry and the host-order nodes, so no second copy of the
    // allocation can disagree with it.
    auto& registry = state.Allocations(InitStateKey{location.device, 0});
    const auto& records = registry.Records();
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        // Diagnostic work counter, not a lifetime quota. The ledger's record
        // capacity bounds storage; repeated lookups do not lose recorded facts.
        if (queries_ != std::numeric_limits<size_t>::max()) {
            ++queries_;
        }
        const auto& allocation = it->allocation;
        const auto allocationEnd = allocation.base + allocation.bytes;
        if (location.address > allocationEnd || end <= allocation.base)
            continue;
        const auto allocatedBefore = order.Precedes(it->allocated, entry);
        const auto allocatedAfter = order.Precedes(exit, it->allocated);
        if (!allocatedBefore || !allocatedAfter)
            return {RangeStatus::AMBIGUOUS, allocation};
        if (*allocatedAfter)
            continue;
        if (!*allocatedBefore)
            return {RangeStatus::AMBIGUOUS, allocation};
        if (it->freed) {
            const auto freedBefore = order.Precedes(*it->freed, entry);
            const auto freedAfter = order.Precedes(exit, *it->freed);
            if (!freedBefore || !freedAfter || (!*freedBefore && !*freedAfter))
                return {RangeStatus::AMBIGUOUS, allocation};
            if (*freedBefore)
                return {RangeStatus::USE_AFTER_FREE, allocation};
        }
        const bool contained = allocation.base <= location.address && end <= allocationEnd;
        return {contained ? RangeStatus::VALID : RangeStatus::OUT_OF_BOUNDS, allocation};
    }

    // A one-device callback cannot resolve a pointer known only on another device.
    for (const auto& other : state.allocations) {
        if (other.first.deviceId == location.device)
            continue;
        for (const auto& record : other.second.Records()) {
            if (queries_ != std::numeric_limits<size_t>::max()) {
                ++queries_;
            }
            const auto& allocation = record.allocation;
            if (allocation.deviceId == location.device)
                continue;
            const auto allocationEnd = allocation.base + allocation.bytes;
            if (location.address <= allocationEnd && end > allocation.base)
                return {RangeStatus::AMBIGUOUS, std::nullopt};
        }
    }
    return {RangeStatus::UNKNOWN, std::nullopt};
}
} // namespace npucheck
