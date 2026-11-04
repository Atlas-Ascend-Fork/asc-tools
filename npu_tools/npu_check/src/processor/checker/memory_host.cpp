/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "checker/memory_analysis.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace npucheck {
bool MemoryAnalysis::IsLegacyMemcpy(AclsanCallbackId id, const AclsanCallbackCommonData& common) const
{
    return managed_ && id == ACLSAN_CBID_MEMORY_WRITE_END && common.apiName &&
           std::strcmp(common.apiName, "aclrtMemcpy") == 0;
}

race::HappensBeforeGraph::Relation MemoryAnalysis::CompareHosts(size_t first, size_t second)
{
    using Relation = race::HappensBeforeGraph::Relation;
    if (!complete_)
        return Relation::Unknown;
    const auto forward = order_.Precedes(first, second);
    const auto backward = order_.Precedes(second, first);
    if (!forward || !backward)
        return Relation::Unknown;
    return *forward ? Relation::Before : *backward ? Relation::After : Relation::Unordered;
}

race::HappensBeforeGraph::Relation MemoryAnalysis::CompareHostToLaunch(size_t node, uint64_t launchId)
{
    using Relation = race::HappensBeforeGraph::Relation;
    const auto found = boundaries_.find(launchId);
    if (!complete_ || found == boundaries_.end())
        return Relation::Unknown;
    const auto before = order_.Precedes(node, found->second.first);
    const auto after = order_.Precedes(found->second.second, node);
    if (!before || !after)
        return Relation::Unknown;
    return *before ? Relation::Before : *after ? Relation::After : Relation::Unordered;
}

void MemoryAnalysis::QueryHostInitialization(HostAccess& access)
{
    if (!complete_) {
        access.initializationKnown = false;
        return;
    }
    const auto& operand = access.evidence;
    std::vector<race::Interval> initialized;
    bool unordered = false;
    const uint64_t end = operand.address + operand.bytes;
    if (!access.resource.allocation) {
        access.initializationKnown = false;
        return;
    }
    const auto sources = OverlappingInitializations(
        operand.deviceId, access.resource.allocation->allocSequence, {{operand.address, end}});
    for (const auto* write : sources) {
        const auto relation = CompareHosts(write->node, operand.node);
        if (relation == race::HappensBeforeGraph::Relation::Before) {
            initialized.push_back({std::max(write->range.begin, operand.address), std::min(write->range.end, end)});
        } else if (relation != race::HappensBeforeGraph::Relation::After) {
            unordered = true;
        }
    }
    race::MergeIntervals(initialized);
    auto cursor = operand.address;
    for (auto range : initialized) {
        if (range.begin > cursor) {
            if (access.uninitializedBytes == 0)
                access.firstUninitializedAddress = cursor;
            access.uninitializedBytes += range.begin - cursor;
        }
        cursor = std::max(cursor, range.end);
    }
    if (cursor < end) {
        if (access.uninitializedBytes == 0)
            access.firstUninitializedAddress = cursor;
        access.uninitializedBytes += end - cursor;
    }
    if (access.uninitializedBytes != 0 &&
        (unordered || !launches_.empty() || uncertainInitializationDevices_.count(operand.deviceId)))
        access.initializationKnown = false;
}

void MemoryAnalysis::HostOperand(uint32_t device, uint64_t address, uint64_t bytes, uint32_t mode, const char* apiName)
{
    constexpr size_t MAX_HOST_OPERANDS = 100000;
    HostAccess access{
        {device, address, bytes, mode, currentHostNode_, apiName ? apiName : "host memory operation"},
        Classify(0, {device, address, bytes})};
    // Runtime may infer memcpy direction from the pointers and ignore kind.
    // An untracked operand is not proof that a host pointer is invalid GM.
    if (access.resource.status == RangeStatus::UNKNOWN)
        access.resource.status = RangeStatus::AMBIGUOUS;
    if (state->hostAccesses.size() >= MAX_HOST_OPERANDS || bytes > std::numeric_limits<uint32_t>::max() ||
        access.resource.status == RangeStatus::AMBIGUOUS) {
        complete_ = false;
        access.initializationKnown = false;
    }
    if (bytes != 0 && access.resource.status == RangeStatus::VALID && access.initializationKnown) {
        if (mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ)
            QueryHostInitialization(access);
        else if (access.resource.allocation)
            AddInitialization(
                device, {address, address + bytes}, currentHostNode_, access.resource.allocation->allocSequence);
    }
    hostOperations_.push_back(access);
    if (state->hostAccesses.size() < MAX_HOST_OPERANDS && bytes <= UINT64_MAX - address)
        state->hostAccesses.push_back(std::move(access.evidence));
}

bool MemoryAnalysis::HostTransfer(const AclsanMemoryTransferData& transfer)
{
    if (transfer.common.size < sizeof(transfer) || transfer.kind < ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE ||
        transfer.kind > ACLSAN_MEMORY_TRANSFER_DEVICE_TO_DEVICE)
        return false;
    if (transfer.kind != ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE)
        HostOperand(
            transfer.deviceId, reinterpret_cast<uintptr_t>(transfer.source), transfer.bytes,
            ACLSAN_DEVICE_MEMORY_ACCESS_READ, transfer.common.apiName);
    if (transfer.kind != ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST)
        HostOperand(
            transfer.deviceId, reinterpret_cast<uintptr_t>(transfer.destination), transfer.bytes,
            ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, transfer.common.apiName);
    return true;
}
} // namespace npucheck
