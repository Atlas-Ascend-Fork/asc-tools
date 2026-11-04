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
#include "checker/racecheck_reports.h"
#include <algorithm>
#include <iterator>

namespace npucheck {
namespace {
class GlobalHistoryQuery {
public:
    GlobalHistoryQuery(
        MemoryAnalysis& analysis, uint64_t launch, const race::Limits& limits, std::set<std::string>& reasons)
        : analysis_(analysis), launch_(launch), limits_(limits), reasons_(reasons)
    {}

    std::vector<NpuCheckRacecheckReport> Run()
    {
        auto& state = *analysis_.state;
        auto& dependency = analysis_.Graph(launch_, limits_, reasons_);
        if (!state.globalHistoryComplete)
            reasons_.insert("global_history_incomplete");
        if (reasons_.empty())
            Collect(dependency);
        CompareHistory();
        if (!reasons_.empty()) {
            state.globalHistoryComplete = false;
            return {};
        }
        state.globalAccesses.insert(
            state.globalAccesses.end(), std::make_move_iterator(current_.begin()),
            std::make_move_iterator(current_.end()));
        state.globalSegments += segments_;
        return std::move(reports_);
    }

private:
    void CompareHistory()
    {
        using Relation = race::HappensBeforeGraph::Relation;
        std::map<uint64_t, Relation> relations;
        for (const auto& old : analysis_.state->globalAccesses) {
            const auto previous = old.data.header.launchId;
            auto relation = relations.find(previous);
            if (relation == relations.end())
                relation = relations.emplace(previous, analysis_.CompareLaunches(previous, launch_)).first;
            // A launch completion edge orders every access in its scope. Check
            // it before forming operand pairs; an ordered launch must not spend
            // the comparison allowance intended for possible conflicts.
            if (relation->second == Relation::Before || relation->second == Relation::After)
                continue;
            for (const auto& now : current_) {
                Compare(old, now, relation->second);
                if (!reasons_.empty())
                    return;
            }
        }
    }

    bool Work()
    {
        if (++work_ <= limits_.comparisons)
            return true;
        reasons_.insert("global_comparison_budget");
        return false;
    }
    void Collect(const race::DependencyGraph& dependency)
    {
        const auto& state = *analysis_.state;
        for (const auto& access : dependency.accesses) {
            if (access.data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM)
                continue;
            segments_ += access.ranges.size();
            if (segments_ + state.globalSegments > limits_.segments ||
                current_.size() + state.globalAccesses.size() >= limits_.events) {
                reasons_.insert("global_history_budget");
                return;
            }
            MemoryState::GlobalAccess record{access.data, {}};
            for (auto range : access.ranges)
                record.ranges.push_back({range.begin, range.end});
            current_.push_back(std::move(record));
        }
    }
    void Report(
        const MemoryState::GlobalAccess& old, const MemoryState::GlobalAccess& now, uint64_t begin, uint64_t end)
    {
        if (reports_.size() >= limits_.segments ||
            reports_.size() >= limits_.stateBytes / sizeof(NpuCheckRacecheckReport)) {
            reasons_.insert("global_report_budget");
            return;
        }
        const std::vector<AclsanDeviceMemoryAccessData> pair{old.data, now.data};
        auto evidence = MakeRacecheckDiagnostics({{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_GM, begin, end}}, pair);
        reports_.push_back(std::move(evidence.front()));
    }
    void CompareRanges(
        const MemoryState::GlobalAccess& old, const MemoryState::GlobalAccess& now,
        race::HappensBeforeGraph::Relation relation)
    {
        for (auto left : old.ranges) {
            for (auto right : now.ranges) {
                if (!Work())
                    return;
                const auto begin = std::max(left.begin, right.begin), end = std::min(left.end, right.end);
                if (begin >= end)
                    continue;
                if (relation == race::HappensBeforeGraph::Relation::Unknown)
                    reasons_.insert("host_graph_incomplete");
                else
                    Report(old, now, begin, end);
                if (!reasons_.empty())
                    return;
            }
        }
    }
    void Compare(
        const MemoryState::GlobalAccess& old, const MemoryState::GlobalAccess& now,
        race::HappensBeforeGraph::Relation relation)
    {
        if (!Work() || old.data.header.deviceId != now.data.header.deviceId)
            return;
        const auto a = racecheck::AccessMode(old.data), b = racecheck::AccessMode(now.data);
        if ((a == ACLSAN_DEVICE_MEMORY_ACCESS_READ && b == ACLSAN_DEVICE_MEMORY_ACCESS_READ) ||
            racecheck::CompatibleAtomicWrites(a, b))
            return;
        CompareRanges(old, now, relation);
    }
    MemoryAnalysis& analysis_;
    uint64_t launch_;
    const race::Limits& limits_;
    std::set<std::string>& reasons_;
    std::vector<MemoryState::GlobalAccess> current_;
    std::vector<NpuCheckRacecheckReport> reports_;
    size_t work_ = 0, segments_ = 0;
};
} // namespace
std::vector<NpuCheckRacecheckReport> MemoryAnalysis::CrossLaunchConflicts(
    uint64_t launchId, const race::Limits& limits, std::set<std::string>& reasons)
{
    return GlobalHistoryQuery(*this, launchId, limits, reasons).Run();
}
} // namespace npucheck
