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
#include "checker/racecheck/analysis_phase_timer.h"
#include "npu_tool_log.h"
#include <algorithm>
#include <limits>

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

void MemoryAnalysis::RegisterUbLayout(const AclsanLaunchData& launch)
{
    if (launch.common.size < sizeof(launch) || launch.common.result != 0 || launch.launchId == 0) {
        return;
    }
    Get(launch.launchId).ubLayout = BuildUbLayout(launch);
}

const UbLayout* MemoryAnalysis::FindUbLayout(uint64_t launchId) const
{
    const auto launch = launches_.find(launchId);
    return launch != launches_.end() && launch->second.ubLayout.valid ? &launch->second.ubLayout : nullptr;
}

RangeResult MemoryAnalysis::ClassifyUbRange(
    const AclsanDeviceMemoryAccessData& access, uint64_t address, uint64_t bytes)
{
    // Physical-capacity violations are certain even when finer layout inputs are missing.
    if (bytes == 0) {
        return {RangeStatus::VALID, std::nullopt};
    }
    if (access.address >= UB_CAPACITY || address >= UB_CAPACITY || bytes > UB_CAPACITY - address) {
        return {RangeStatus::OUT_OF_BOUNDS, Allocation{0, 0, UB_CAPACITY, access.header.deviceId, 0, 0}};
    }
    auto& launch = Get(access.header.launchId);
    if (!launch.ubLayout.valid) {
        if (!launch.ubLayoutWarningLogged) {
            ASCTOOL_WARNING(
                "memcheck: UB layout unavailable device=%u launch=%llu reason=%s; "
                "UB partition boundary checks are incomplete; other memory accesses continue to be checked",
                access.header.deviceId, static_cast<unsigned long long>(access.header.launchId),
                launch.ubLayout.unavailableReason);
            launch.ubLayoutWarningLogged = true;
        }
        return {RangeStatus::AMBIGUOUS, std::nullopt};
    }
    const auto region = launch.ubLayout.Find(access.address);
    if (!region.has_value()) {
        return {RangeStatus::OUT_OF_BOUNDS, Allocation{0, 0, UB_CAPACITY, access.header.deviceId, 0, 0}};
    }
    const Allocation allocation{0, region->begin, region->end - region->begin, access.header.deviceId, 0, 0};
    if (!region->AllowsSimdAccess() || !region->Contains(address, bytes)) {
        return {RangeStatus::OUT_OF_BOUNDS, allocation};
    }
    return {RangeStatus::VALID, allocation};
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
        !(domain == ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION && id == ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE) &&
        !(domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && id == ACLSAN_CBID_DEVICE_SYNC))
        return;
    CheckerReportList reports;
    if (!synchronization_->OnCallback(domain, id, data, reports) || !synchronization_->ObservationComplete())
        complete_ = false;
    for (auto& report : reports) {
        if (auto* sync = std::get_if<NpuCheckSynccheckReport>(&report)) {
            invalidSynchronization_.insert(sync->common.exec.launchId);
            synchronizationReports_.push_back(std::move(*sync));
        }
    }
}

namespace {
constexpr uint8_t UNKNOWN_INITIALIZATION_SOURCE = 0;
constexpr uint8_t USABLE_INITIALIZATION_SOURCE = 1;
constexpr uint8_t INVALID_INITIALIZATION_SOURCE = 2;
constexpr size_t MISSING_INITIALIZATION_RANK = 0;
constexpr uint8_t INITIALIZATION_CACHE_PIPELINE = 0;
constexpr size_t MAX_OBSERVED_PAGE_COMMITS = 8;
constexpr size_t FIRST_INITIALIZATION_QUERY = 1;
constexpr size_t UNINDEXED_SOURCE_COUNT = 0;

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
bool ReadsCovered(const std::vector<race::Interval>& reads, const std::vector<race::Interval>& writes)
{
    // Both inputs are canonical unions; never bridge a gap between intervals.
    auto write = writes.begin();
    for (const auto& read : reads) {
        while (write != writes.end() && write->end <= read.begin)
            ++write;
        if (write == writes.end() || write->begin > read.begin || write->end < read.end)
            return false;
    }
    return true;
}

bool InitializationRangesOverlap(const std::vector<race::Interval>& reads, const std::vector<race::Interval>& writes)
{
    auto read = reads.begin();
    auto write = writes.begin();
    while (read != reads.end() && write != writes.end()) {
        if (read->begin < write->end && write->begin < read->end)
            return true;
        if (read->end <= write->begin)
            ++read;
        else
            ++write;
    }
    return false;
}

void IndexInitializationSources(
    const MemoryAnalysis::Launch& launch, const race::DependencyGraph& dependency, const std::vector<uint32_t>& sources,
    MemoryAnalysis::StorageWrites& writes)
{
    for (auto index : sources) {
        if (launch.accessValid[index] == USABLE_INITIALIZATION_SOURCE)
            writes.validSources.push_back(index);
        else
            writes.invalidSources.push_back(index);
    }
    writes.ranked = std::all_of(writes.validSources.begin(), writes.validSources.end(), [&dependency](uint32_t index) {
        return dependency.graph.TraversalRank(dependency.accesses[index].node).has_value();
    });
    if (!writes.ranked)
        return;
    std::sort(writes.validSources.begin(), writes.validSources.end(), [&dependency](uint32_t left, uint32_t right) {
        const auto first = *dependency.graph.TraversalRank(dependency.accesses[left].node);
        const auto second = *dependency.graph.TraversalRank(dependency.accesses[right].node);
        return first != second ? first < second : left < right;
    });
}

void OrderInitializationSources(
    const MemoryAnalysis::Launch& launch, const race::DependencyGraph& dependency, size_t readerNode,
    std::vector<uint32_t>& candidates)
{
    const auto readerRank = dependency.graph.TraversalRank(readerNode);
    std::sort(candidates.begin(), candidates.end(), [&launch, &dependency, &readerRank](uint32_t left, uint32_t right) {
        const bool leftUsable = launch.accessValid[left] == USABLE_INITIALIZATION_SOURCE;
        const bool rightUsable = launch.accessValid[right] == USABLE_INITIALIZATION_SOURCE;
        if (leftUsable != rightUsable)
            return !leftUsable; // Invalid sources must not be hidden by sufficient valid evidence.
        if (!leftUsable)
            return left < right;
        const auto leftRank = dependency.graph.TraversalRank(dependency.accesses[left].node);
        const auto rightRank = dependency.graph.TraversalRank(dependency.accesses[right].node);
        const bool leftPreferred = readerRank && leftRank && *leftRank < *readerRank;
        const bool rightPreferred = readerRank && rightRank && *rightRank < *readerRank;
        if (leftPreferred != rightPreferred)
            return leftPreferred;
        const auto first = leftRank.value_or(MISSING_INITIALIZATION_RANK);
        const auto second = rightRank.value_or(MISSING_INITIALIZATION_RANK);
        return first != second ? first > second : left < right;
    });
}

void AddPrunedInitializationSources(size_t count, size_t& total)
{
    const auto maximum = std::numeric_limits<size_t>::max();
    total = count > maximum - total ? maximum : total + count;
}
} // namespace

void MemoryAnalysis::BuildStorageIndex(uint64_t launchId, Launch& launch, const race::DependencyGraph& dependency)
{
    race::AnalysisPhaseTimer timing(launchId, "initialization_index");
    launch.accessValid.assign(dependency.accesses.size(), UNKNOWN_INITIALIZATION_SOURCE);
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
            launch.accessValid[index] = valid ? USABLE_INITIALIZATION_SOURCE : INVALID_INITIALIZATION_SOURCE;
        }
        IndexInitializationSources(launch, dependency, accesses, writes);
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
    // Managed initialization pages follow the physical storage key used for write evidence.
    const auto key = InitStateKey::PhysicalLocal(
        std::get<0>(domain), launchId, std::get<1>(domain), std::get<3>(domain), std::get<0>(storage));
    auto& page = state->initialized.GetOrCreate(key);
    std::optional<race::AnalysisPhaseTimer> timing;
    if (launch.initializationPageTimings < MAX_OBSERVED_PAGE_COMMITS) {
        ++launch.initializationPageTimings;
        timing.emplace(launchId, "initialization_page_commit");
    }
    // The index is sorted by begin. All writes use the same cache pipeline, so
    // their exact union is equivalent to repeatedly setting every valid byte.
    std::optional<race::Interval> pending;
    for (const auto& range : found->second.ranges) {
        if (launch.accessValid[range.access] != USABLE_INITIALIZATION_SOURCE)
            continue;
        if (pending && range.begin <= pending->end) {
            pending->end = std::max(pending->end, range.end);
        } else {
            if (pending)
                page.Write(pending->begin, pending->end - pending->begin, INITIALIZATION_CACHE_PIPELINE);
            pending = race::Interval{range.begin, range.end};
        }
    }
    if (pending)
        page.Write(pending->begin, pending->end - pending->begin, INITIALIZATION_CACHE_PIPELINE);
}

void MemoryAnalysis::PrepareInitializationQuery(
    uint64_t launchId, InitializationQuery& query, std::set<std::string>& reasons)
{
    auto& launch = query.launch;
    const auto node = launch.accessesByNode.find(query.readerNode);
    if (node != launch.accessesByNode.end()) {
        for (auto index : node->second) {
            const auto& access = query.dependency.accesses[index];
            if (race::StorageOf(access.data) == query.storage && race::ReadsMemory(access.data.accessMode))
                query.reads.insert(query.reads.end(), access.ranges.begin(), access.ranges.end());
        }
    }
    race::MergeIntervals(query.reads);
    if (query.reads.empty())
        return;
    if (std::get<0>(query.storage) == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
        const auto device = std::get<0>(std::get<1>(query.storage));
        query.preceding = InitializationsBefore(launchId, device, query.reads, reasons);
    }
    const auto writes = launch.writesByStorage.find(query.storage);
    if (writes == launch.writesByStorage.end())
        return;
    const auto rank = query.dependency.graph.TraversalRank(query.readerNode);
    if (writes->second.ranked && rank) {
        query.indexed = &writes->second;
        const auto& sources = query.indexed->validSources;
        const auto split =
            std::lower_bound(sources.begin(), sources.end(), *rank, [&query](uint32_t index, size_t value) {
                return *query.dependency.graph.TraversalRank(query.dependency.accesses[index].node) < value;
            });
        query.rankSplit = static_cast<size_t>(split - sources.begin());
        for (auto index : query.indexed->invalidSources) {
            if (InitializationRangesOverlap(query.reads, query.dependency.accesses[index].ranges))
                query.candidates.push_back(index);
        }
        return;
    }
    for (auto read : query.reads)
        CollectOverlappingWrites(writes->second, read.begin, read.end, query.candidates);
    std::sort(query.candidates.begin(), query.candidates.end());
    query.candidates.erase(std::unique(query.candidates.begin(), query.candidates.end()), query.candidates.end());
    OrderInitializationSources(launch, query.dependency, query.readerNode, query.candidates);
}

bool MemoryAnalysis::VisitInitializationSource(
    InitializationQuery& query, uint32_t index, std::set<std::string>& reasons)
{
    const auto& access = query.dependency.accesses[index];
    if (access.node == query.readerNode)
        return true;
    auto& comparisons = query.launch.initializationQueryWork;
    if (comparisons >= limits_.CandidateComparisonBudget()) {
        reasons.insert("comparison_budget");
        return false;
    }
    ++comparisons;
    if (race::ExceedsBudget(
            limits_.InitializationIntervalBudget(),
            {query.preceding.size(), query.unordered.size(), access.ranges.size()})) {
        reasons.insert("segment_budget");
        return false;
    }
    const auto relation = query.dependency.graph.Compare(access.node, query.readerNode);
    if (query.launch.accessValid[index] != USABLE_INITIALIZATION_SOURCE) {
        if (relation != race::HappensBeforeGraph::Relation::After)
            reasons.insert("invalid_initialization_source");
        return true;
    }
    if (relation == race::HappensBeforeGraph::Relation::Before) {
        query.preceding.insert(query.preceding.end(), access.ranges.begin(), access.ranges.end());
        race::MergeIntervals(query.preceding);
    } else if (relation == race::HappensBeforeGraph::Relation::Unordered) {
        query.unordered.insert(query.unordered.end(), access.ranges.begin(), access.ranges.end());
    } else if (relation == race::HappensBeforeGraph::Relation::Unknown) {
        reasons.insert("graph_query_budget");
    }
    return true;
}

bool MemoryAnalysis::CollectRankedInitializationRange(
    InitializationQuery& query, size_t begin, size_t end, bool reverse, std::set<std::string>& reasons)
{
    constexpr size_t LAST_SOURCE_OFFSET = 1;
    constexpr size_t SOURCE_VISIT_INCREMENT = 1;
    const auto count = end - begin; // Both boundaries come from this immutable source vector.
    for (size_t offset = 0; offset < count; ++offset) {
        if (reasons.empty() && ReadsCovered(query.reads, query.preceding)) {
            AddPrunedInitializationSources(count - offset, query.launch.initializationSourcesPruned);
            return false;
        }
        const auto position = reverse ? end - LAST_SOURCE_OFFSET - offset : begin + offset;
        const auto index = query.indexed->validSources[position];
        AddPrunedInitializationSources(SOURCE_VISIT_INCREMENT, query.launch.initializationSourcesVisited);
        if (!InitializationRangesOverlap(query.reads, query.dependency.accesses[index].ranges))
            continue;
        if (!VisitInitializationSource(query, index, reasons))
            return false;
    }
    return true;
}

void MemoryAnalysis::CollectInitializationEvidence(InitializationQuery& query, std::set<std::string>& reasons)
{
    for (size_t position = 0; position < query.candidates.size(); ++position) {
        const auto index = query.candidates[position];
        const bool usable = query.launch.accessValid[index] == USABLE_INITIALIZATION_SOURCE;
        if (usable && reasons.empty() && ReadsCovered(query.reads, query.preceding)) {
            AddPrunedInitializationSources(
                query.candidates.size() - position, query.launch.initializationSourcesPruned);
            return;
        }
        if (!VisitInitializationSource(query, index, reasons))
            return;
    }
    if (query.indexed == nullptr)
        return;
    if (!CollectRankedInitializationRange(query, 0, query.rankSplit, true, reasons)) {
        // Deferred counts describe storage sources, not proven spatially-overlapping HB queries.
        AddPrunedInitializationSources(
            query.indexed->validSources.size() - query.rankSplit, query.launch.initializationSourcesPruned);
        return;
    }
    CollectRankedInitializationRange(query, query.rankSplit, query.indexed->validSources.size(), false, reasons);
}

std::vector<race::Interval> MemoryAnalysis::PrecedingWrites(
    uint64_t launchId, size_t readerNode, const race::Storage& storage, std::set<std::string>& reasons)
{
    auto& dependency = Graph(launchId, limits_, reasons);
    auto& launch = Get(launchId);
    if (!launch.storageIndexReady)
        BuildStorageIndex(launchId, launch, dependency);
    InitializationQuery query(launch, dependency, storage, readerNode);
    if (launch.initializationQueryBatches < std::numeric_limits<size_t>::max())
        ++launch.initializationQueryBatches;
    const auto batch = launch.initializationQueryBatches;
    const bool observed = batch != 0 && (batch & (batch - FIRST_INITIALIZATION_QUERY)) == 0;
    std::optional<race::AnalysisPhaseTimer> timing;
    if (observed)
        timing.emplace(launchId, "initialization_query");
    PrepareInitializationQuery(launchId, query, reasons);
    if (observed)
        ASCTOOL_INFO(
            "initialization query: launch=%llu batch=%zu reads=%zu eager_candidates=%zu indexed_sources=%zu "
            "hb_queries=%zu visited_sources=%zu deferred_storage_sources=%zu",
            static_cast<unsigned long long>(launchId), batch, query.reads.size(), query.candidates.size(),
            query.indexed == nullptr ? UNINDEXED_SOURCE_COUNT : query.indexed->validSources.size(),
            launch.initializationQueryWork, launch.initializationSourcesVisited, launch.initializationSourcesPruned);
    if (query.reads.empty())
        return {};
    CollectInitializationEvidence(query, reasons);
    race::MergeIntervals(query.preceding);
    // The cache must include all valid writes, not just sufficient evidence for this reader.
    if (std::get<0>(storage) != ACLSAN_DEVICE_MEMORY_SPACE_GM && reasons.empty() &&
        launch.pageCommitted.insert(storage).second)
        CommitWritePage(launchId, launch, storage);
    for (auto read : query.reads) {
        if (Covered(read, query.preceding))
            continue;
        if (std::any_of(query.unordered.begin(), query.unordered.end(), [read](auto write) {
                return write.begin < read.end && read.begin < write.end;
            }))
            reasons.insert("unordered_initialization");
    }
    return std::move(query.preceding);
}

void MemoryAnalysis::CommitLaunch(uint64_t launchId)
{
    if (!managed_)
        return;
    race::AnalysisPhaseTimer timing(launchId, "initialization_publish");
    // Publish the complete validated launch before checker queries begin.
    try {
        std::set<std::string> reasons;
        auto& dependency = Graph(launchId, limits_, reasons);
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
