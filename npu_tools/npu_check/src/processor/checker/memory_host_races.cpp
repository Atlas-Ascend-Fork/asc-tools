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
namespace {
class HostRaceQuery {
public:
    HostRaceQuery(MemoryAnalysis& analysis, const race::Limits& limits, std::set<std::string>& reasons)
        : analysis_(analysis), limits_(limits), reasons_(reasons)
    {}
    void Compare(
        const HostAccessEvidence& host, const AclsanDeviceMemoryAccessData& device, race::Interval range,
        bool hostCurrent)
    {
        if (host.deviceId != device.header.deviceId)
            return;
        const auto relation = analysis_.CompareHostToLaunch(host.node, device.header.launchId);
        if (relation == race::HappensBeforeGraph::Relation::Before ||
            relation == race::HappensBeforeGraph::Relation::After)
            return;
        auto report = Prepare(host, device.accessMode, range, relation);
        if (!report)
            return;
        report->launchId = device.header.launchId;
        report->previousRange = hostCurrent ? NpuCheckRacecheckRange{range.begin, range.end} :
                                              NpuCheckRacecheckRange{host.address, host.address + host.bytes};
        report->currentRange = hostCurrent ? NpuCheckRacecheckRange{host.address, host.address + host.bytes} :
                                             NpuCheckRacecheckRange{range.begin, range.end};
        if (hostCurrent) {
            report->previous = device;
            report->currentHost = host;
        } else {
            report->previousHost = host;
            report->current = device;
        }
        report->kind = Kind(hostCurrent ? device.accessMode : host.mode, hostCurrent ? host.mode : device.accessMode);
        reports_.push_back(std::move(*report));
    }
    void Compare(const HostAccessEvidence& previous, const HostAccessEvidence& current)
    {
        if (previous.deviceId != current.deviceId || previous.node == current.node)
            return;
        const auto relation = analysis_.CompareHosts(previous.node, current.node);
        if (relation == race::HappensBeforeGraph::Relation::Before ||
            relation == race::HappensBeforeGraph::Relation::After)
            return;
        auto report = Prepare(current, previous.mode, {previous.address, previous.address + previous.bytes}, relation);
        if (!report)
            return;
        report->previousHost = previous;
        report->currentHost = current;
        report->previousRange = {previous.address, previous.address + previous.bytes};
        report->currentRange = {current.address, current.address + current.bytes};
        report->kind = Kind(previous.mode, current.mode);
        reports_.push_back(std::move(*report));
    }
    std::vector<NpuCheckRacecheckReport> Take()
    {
        return reasons_.empty() ? std::move(reports_) : std::vector<NpuCheckRacecheckReport>{};
    }
    bool Work()
    {
        if (++work_ <= limits_.comparisons && reports_.size() < limits_.segments &&
            reports_.size() < limits_.stateBytes / sizeof(NpuCheckRacecheckReport))
            return true;
        reasons_.insert("host_race_budget");
        return false;
    }

private:
    static ConflictKind Kind(uint32_t before, uint32_t after)
    {
        return before == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? ConflictKind::WAR :
               after == ACLSAN_DEVICE_MEMORY_ACCESS_READ  ? ConflictKind::RAW :
                                                            ConflictKind::WAW;
    }
    std::optional<NpuCheckRacecheckReport> Prepare(
        const HostAccessEvidence& host, uint32_t otherMode, race::Interval range,
        race::HappensBeforeGraph::Relation relation)
    {
        if (host.mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ && otherMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ)
            return std::nullopt;
        const auto begin = std::max(host.address, range.begin), end = std::min(host.address + host.bytes, range.end);
        if (begin >= end)
            return std::nullopt;
        if (relation == race::HappensBeforeGraph::Relation::Unknown) {
            reasons_.insert("host_graph_incomplete");
            return std::nullopt;
        }
        NpuCheckRacecheckReport report;
        report.key = {host.deviceId, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0};
        report.checkRange = {begin, end};
        return report;
    }
    MemoryAnalysis& analysis_;
    const race::Limits& limits_;
    std::set<std::string>& reasons_;
    std::vector<NpuCheckRacecheckReport> reports_;
    size_t work_ = 0;
};
} // namespace
std::vector<NpuCheckRacecheckReport> MemoryAnalysis::HostConflicts(
    const race::Limits& limits, std::set<std::string>& reasons)
{
    HostRaceQuery query(*this, limits, reasons);
    if (!state->globalHistoryComplete)
        reasons.insert("global_history_incomplete");
    for (const auto& operation : hostOperations_) {
        const auto& host = operation.evidence;
        if (host.bytes > UINT64_MAX - host.address)
            continue;
        for (const auto& access : state->globalAccesses)
            for (auto range : access.ranges) {
                if (!query.Work())
                    return {};
                query.Compare(host, access.data, {range.begin, range.end}, true);
            }
        for (const auto& previous : state->hostAccesses) {
            if (!query.Work())
                return {};
            query.Compare(previous, host);
        }
    }
    return query.Take();
}
std::vector<NpuCheckRacecheckReport> MemoryAnalysis::CrossLaunchHostConflicts(
    uint64_t launchId, const race::Limits& limits, std::set<std::string>& reasons)
{
    auto& dependency = Graph(launchId, limits, reasons);
    if (!reasons.empty())
        return {};
    HostRaceQuery query(*this, limits, reasons);
    for (const auto& access : dependency.accesses) {
        if (access.data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM)
            continue;
        for (const auto& host : state->hostAccesses)
            for (auto range : access.ranges) {
                if (!query.Work())
                    return {};
                query.Compare(host, access.data, range, false);
            }
    }
    return query.Take();
}
} // namespace npucheck
