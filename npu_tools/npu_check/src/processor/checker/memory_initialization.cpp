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
#include <limits>

namespace npucheck {
void MemoryAnalysis::AddInitialization(uint32_t device, race::Interval range, size_t node, uint64_t allocationSequence)
{
    constexpr size_t MAX_INITIALIZATION_RANGES = 100000;
    if (initializationCount_ >= MAX_INITIALIZATION_RANGES) {
        // This is lost evidence, unlike a computational work counter. It stays
        // incomplete until the session ends; starting a new launch cannot repair it.
        complete_ = false;
        incompleteReasons_.insert("initialization_history_capacity");
        return;
    }
    auto& index = initializations_[{device, allocationSequence}];
    index.writes.push_back({range, node});
    index.sorted = false;
    ++initializationCount_;
}

void MemoryAnalysis::RecordInitialization(uint64_t launchId, uint32_t device, uint64_t address, uint64_t bytes)
{
    if (!managed_ || bytes == 0) {
        return;
    }
    const auto boundary = boundaries_.find(launchId);
    if (bytes > UINT64_MAX - address || (launchId != 0 && boundary == boundaries_.end())) {
        complete_ = false;
        return;
    }
    const auto resource = Classify(launchId, {device, address, bytes});
    if (resource.status != RangeStatus::VALID || !resource.allocation) {
        return;
    }
    const auto node = launchId == 0 ? currentHostNode_ : boundary->second.second;
    AddInitialization(device, {address, address + bytes}, node, resource.allocation->allocSequence);
}

std::vector<const MemoryAnalysis::Initialization*> MemoryAnalysis::OverlappingInitializations(
    uint32_t device, uint64_t allocationSequence, const std::vector<race::Interval>& reads)
{
    std::vector<const Initialization*> output;
    const auto found = initializations_.find({device, allocationSequence});
    if (found == initializations_.end()) {
        return output;
    }
    auto& index = found->second;
    if (!index.sorted) {
        std::sort(
            index.writes.begin(), index.writes.end(), [](const Initialization& left, const Initialization& right) {
                return left.range.begin < right.range.begin;
            });
        index.prefixMaxEnd.resize(index.writes.size());
        uint64_t maxEnd = 0;
        for (size_t i = 0; i < index.writes.size(); ++i) {
            maxEnd = std::max(maxEnd, index.writes[i].range.end);
            index.prefixMaxEnd[i] = maxEnd;
        }
        index.sorted = true;
    }
    for (const auto read : reads) {
        const auto limit = std::lower_bound(
            index.writes.begin(), index.writes.end(), read.end,
            [](const Initialization& write, uint64_t end) { return write.range.begin < end; });
        auto i = static_cast<size_t>(limit - index.writes.begin());
        while (i != 0) {
            --i;
            if (index.prefixMaxEnd[i] <= read.begin) {
                break;
            }
            if (index.writes[i].range.end > read.begin) {
                output.push_back(&index.writes[i]);
            }
        }
    }
    std::sort(output.begin(), output.end()); // All pointers belong to this bucket's vector.
    output.erase(std::unique(output.begin(), output.end()), output.end());
    const auto available = std::numeric_limits<size_t>::max() - initializationCandidates_;
    initializationCandidates_ += std::min(available, output.size());
    return output;
}

std::vector<race::Interval> MemoryAnalysis::InitializationsBefore(
    uint64_t launchId, uint32_t device, const std::vector<race::Interval>& reads, std::set<std::string>& reasons)
{
    std::vector<race::Interval> output;
    const auto launch = boundaries_.find(launchId);
    if (!complete_ || launch == boundaries_.end()) {
        reasons.insert("host_graph_incomplete");
        return output;
    }
    for (const auto read : reads) {
        // Validate the reader's generation once. Source ranges were validated at
        // insertion; matching the generation prevents freed/reused bytes leaking in.
        const auto resource = Classify(launchId, {device, read.begin, read.end - read.begin});
        if (resource.status != RangeStatus::VALID || !resource.allocation) {
            reasons.insert("ambiguous_initialization_lifetime");
            break;
        }
        const auto sources = OverlappingInitializations(device, resource.allocation->allocSequence, {read});
        for (const auto* write : sources) {
            const auto precedes = order_.Precedes(write->node, launch->second.first);
            const auto follows = order_.Precedes(launch->second.second, write->node);
            if (!precedes || !follows) {
                reasons.insert("host_order_incomplete");
                break;
            }
            if (*precedes) {
                if (output.size() >= race::Limits{}.segments) {
                    reasons.insert("initialization_segment_budget");
                    return {};
                }
                output.push_back({std::max(read.begin, write->range.begin), std::min(read.end, write->range.end)});
            } else if (!*follows) {
                reasons.insert("unordered_initialization");
            }
        }
    }
    race::MergeIntervals(output);
    return output;
}
} // namespace npucheck
