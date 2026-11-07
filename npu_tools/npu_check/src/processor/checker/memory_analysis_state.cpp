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

namespace npucheck {
RangeResult MemoryAnalysis::Classify(uint64_t launchId, const MemoryLocation& location)
{
    if (!managed_)
        return state->Allocations({location.device, 0}).Classify(location.device, location.address, location.bytes);
    if (launchId == 0) {
        const auto result = resources_.Resolve(location, currentHostNode_, currentHostNode_, order_, *state);
        complete_ = complete_ && resources_.Complete();
        return result;
    }
    const auto boundary = boundaries_.find(launchId);
    if (!complete_ || boundary == boundaries_.end())
        return {RangeStatus::AMBIGUOUS, std::nullopt};
    const auto result = resources_.Resolve(location, boundary->second.first, boundary->second.second, order_, *state);
    complete_ = complete_ && resources_.Complete();
    return result;
}

RangeResult MemoryAnalysis::ClassifyAccessRange(
    const AclsanDeviceMemoryAccessData& access, uint64_t address, uint64_t bytes)
{
    const auto& header = access.header;
    const MemoryLocation range{header.deviceId, address, bytes};
    if (!managed_ || bytes == 0)
        return Classify(header.launchId, range);
    const auto origin = Classify(header.launchId, {header.deviceId, access.address, 1});
    if (origin.status != RangeStatus::VALID || !origin.allocation)
        return origin;
    // Affine fragments retain their operand's known allocation generation. A
    // stride into an unmapped gap (or a different allocation) is out of bounds,
    // not missing lifetime evidence and not a use of an unrelated tombstone.
    if (bytes > UINT64_MAX - address)
        return {RangeStatus::OVERFLOW, origin.allocation};
    const auto& allocation = *origin.allocation;
    const bool contained = address >= allocation.base && address + bytes <= allocation.base + allocation.bytes;
    return {contained ? RangeStatus::VALID : RangeStatus::OUT_OF_BOUNDS, origin.allocation};
}

bool MemoryAnalysis::HostWrite(const AclsanMemoryWriteData& write)
{
    if (write.common.size < sizeof(write))
        return false;
    const auto address = reinterpret_cast<uintptr_t>(write.destination);
    const auto resource = Classify(0, {write.deviceId, address, write.bytes});
    hostWriteRecorded_ = resource.status == RangeStatus::VALID;
    HostOperand(write.deviceId, address, write.bytes, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, write.common.apiName);
    return true;
}

void MemoryAnalysis::ObserveSynchronization(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    if (domain != ACLSAN_CB_DOMAIN_LAUNCH && domain != ACLSAN_CB_DOMAIN_SYNCHRONIZE &&
        !(domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && id == ACLSAN_CBID_DEVICE_SYNC))
        return;
    CheckerReportList reports;
    if (!synchronization_->OnCallback(domain, id, data, reports))
        complete_ = false;
    for (auto& report : reports) {
        if (auto* sync = std::get_if<NpuCheckSynccheckReport>(&report)) {
            invalidSynchronization_.insert(sync->common.exec.launchId);
            synchronizationReports_.push_back(std::move(*sync));
        }
    }
}

namespace {
bool Covered(race::Interval range, const std::vector<race::Interval>& writes)
{
    auto cursor = range.begin;
    for (auto write : writes) {
        if (write.begin > cursor)
            return false;
        if (write.end > cursor)
            cursor = write.end;
        if (cursor >= range.end)
            return true;
    }
    return false;
}
} // namespace

void MemoryAnalysis::BuildStorageIndex(uint64_t launchId, Launch& launch, const race::DependencyGraph& dependency)
{
    launch.accessValid.assign(dependency.accesses.size(), 0);
    for (uint32_t i = 0; i < dependency.accesses.size(); ++i) {
        const auto& access = dependency.accesses[i];
        launch.accessesByNode[access.node].push_back(i);
        if (!race::WritesMemory(access.data.accessMode))
            continue;
        auto& writes = launch.writesByStorage[race::StorageOf(access.data)];
        for (const auto& range : access.ranges)
            writes.ranges.push_back(WriteRange{range.begin, range.end, i});
    }
    for (auto& entry : launch.writesByStorage) {
        auto& writes = entry.second;
        std::sort(writes.ranges.begin(), writes.ranges.end(), [](const WriteRange& left, const WriteRange& right) {
            return left.begin < right.begin;
        });
        writes.prefixMaxEnd.resize(writes.ranges.size());
        uint64_t maxEnd = 0;
        for (size_t i = 0; i < writes.ranges.size(); ++i) {
            maxEnd = std::max(maxEnd, writes.ranges[i].end);
            writes.prefixMaxEnd[i] = maxEnd;
        }
        // A source is usable only when its whole footprint belongs to a live allocation
        // (GM) or a valid local range. Decide once per launch instead of per reader.
        std::vector<uint32_t> accesses;
        for (const auto& range : writes.ranges)
            accesses.push_back(range.access);
        std::sort(accesses.begin(), accesses.end());
        accesses.erase(std::unique(accesses.begin(), accesses.end()), accesses.end());
        const auto space = std::get<0>(entry.first);
        for (auto index : accesses) {
            bool valid = true;
            for (auto range : dependency.accesses[index].ranges) {
                if (space == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
                    const auto resource =
                        ClassifyAccessRange(dependency.accesses[index].data, range.begin, range.end - range.begin);
                    valid = valid && resource.status == RangeStatus::VALID;
                } else {
                    const auto capacity = InitStateTableIndex::LocalCapacity(space);
                    valid = valid && capacity != 0 && range.end <= capacity;
                }
            }
            launch.accessValid[index] = valid ? 1 : 2;
        }
    }
    launch.storageIndexReady = true;
}

void MemoryAnalysis::CollectOverlappingWrites(
    const StorageWrites& writes, uint64_t begin, uint64_t end, std::vector<uint32_t>& output)
{
    const auto limit = std::lower_bound(
        writes.ranges.begin(), writes.ranges.end(), end,
        [](const WriteRange& range, uint64_t value) { return range.begin < value; });
    size_t index = static_cast<size_t>(limit - writes.ranges.begin());
    while (index != 0) {
        --index;
        if (writes.prefixMaxEnd[index] <= begin)
            break;
        if (writes.ranges[index].end > begin)
            output.push_back(writes.ranges[index].access);
    }
}

void MemoryAnalysis::CommitWritePage(uint64_t launchId, Launch& launch, const race::Storage& storage)
{
    const auto found = launch.writesByStorage.find(storage);
    if (found == launch.writesByStorage.end())
        return;
    const auto& domain = std::get<1>(storage);
    const auto key = InitStateKey::Local(
        std::get<0>(domain), launchId, std::get<1>(domain), std::get<2>(domain), std::get<0>(storage));
    auto& page = state->initialized.GetOrCreate(key);
    for (const auto& range : found->second.ranges) {
        if (launch.accessValid[range.access] == 1)
            page.Write(range.begin, range.end - range.begin, 0);
    }
}

std::vector<race::Interval> MemoryAnalysis::PrecedingWrites(
    uint64_t launchId, size_t readerNode, const race::Storage& storage, std::set<std::string>& reasons)
{
    const race::Limits limits;
    auto& dependency = Graph(launchId, limits, reasons);
    auto& launch = Get(launchId);
    if (!launch.storageIndexReady)
        BuildStorageIndex(launchId, launch, dependency);
    std::vector<race::Interval> reads, preceding, unordered;
    const auto node = launch.accessesByNode.find(readerNode);
    if (node != launch.accessesByNode.end())
        for (auto index : node->second) {
            const auto& access = dependency.accesses[index];
            if (race::StorageOf(access.data) == storage && race::ReadsMemory(access.data.accessMode))
                reads.insert(reads.end(), access.ranges.begin(), access.ranges.end());
        }
    if (reads.empty())
        return preceding;
    const auto device = std::get<0>(std::get<1>(storage));
    if (std::get<0>(storage) == ACLSAN_DEVICE_MEMORY_SPACE_GM)
        preceding = InitializationsBefore(launchId, device, reads, reasons);

    auto& comparisons = launch.initializationQueryWork;
    const auto writes = launch.writesByStorage.find(storage);
    if (writes != launch.writesByStorage.end()) {
        std::vector<uint32_t> candidates;
        for (auto read : reads)
            CollectOverlappingWrites(writes->second, read.begin, read.end, candidates);
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (auto index : candidates) {
            const auto& access = dependency.accesses[index];
            if (access.node == readerNode)
                continue;
            if (++comparisons > limits.comparisons) {
                reasons.insert("comparison_budget");
                break;
            }
            if (preceding.size() + unordered.size() + access.ranges.size() > limits.segments) {
                reasons.insert("segment_budget");
                break;
            }
            const auto relation = dependency.graph.Compare(access.node, readerNode);
            if (launch.accessValid[index] != 1) {
                // An invalid write matters only to overlapping reads it can
                // precede. Never invent an initialization fact from that write.
                if (relation != race::HappensBeforeGraph::Relation::After) {
                    reasons.insert("invalid_initialization_source");
                }
                continue;
            }
            if (relation == race::HappensBeforeGraph::Relation::Before)
                preceding.insert(preceding.end(), access.ranges.begin(), access.ranges.end());
            else if (relation == race::HappensBeforeGraph::Relation::Unordered)
                unordered.insert(unordered.end(), access.ranges.begin(), access.ranges.end());
            else if (relation == race::HappensBeforeGraph::Relation::Unknown)
                reasons.insert("graph_query_budget");
        }
    }
    race::MergeIntervals(preceding);
    // The local page is a cache consulted only inside the proven preceding ranges. Commit
    // every usable write once per storage instead of re-committing a reader-filtered
    // subset; under-committing is what would turn later reads into false reports.
    if (std::get<0>(storage) != ACLSAN_DEVICE_MEMORY_SPACE_GM && reasons.empty() &&
        launch.pageCommitted.insert(storage).second)
        CommitWritePage(launchId, launch, storage);
    for (auto read : reads)
        if (!Covered(read, preceding))
            for (auto write : unordered)
                if (write.begin < read.end && read.begin < write.end)
                    reasons.insert("unordered_initialization");
    return preceding;
}

void MemoryAnalysis::CommitLaunch(uint64_t launchId)
{
    if (!managed_)
        return;
    // Publish the complete validated launch before checker queries begin.
    try {
        std::set<std::string> reasons;
        auto& dependency = Graph(launchId, {}, reasons);
        if (!reasons.empty()) {
            for (const auto& access : Get(launchId).memory)
                if (race::WritesMemory(access.accessMode) || access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN)
                    uncertainInitializationDevices_.insert(access.header.deviceId);
            return;
        }
        for (const auto& access : dependency.accesses) {
            if (access.data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM || !race::WritesMemory(access.data.accessMode))
                continue;
            const bool valid = std::all_of(access.ranges.begin(), access.ranges.end(), [this, &access](auto range) {
                return ClassifyAccessRange(access.data, range.begin, range.end - range.begin).status ==
                       RangeStatus::VALID;
            });
            if (!valid) {
                uncertainInitializationDevices_.insert(access.data.header.deviceId);
                continue;
            }
            for (auto range : access.ranges)
                RecordInitialization(launchId, access.data.header.deviceId, range.begin, range.end - range.begin);
        }
    } catch (...) {
        complete_ = false;
    }
}

} // namespace npucheck
