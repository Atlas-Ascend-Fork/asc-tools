/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/racecheck.h"
#include "checker/racecheck_reports.h"
#include <optional>
#include "checker/racecheck/happens_before_graph.h"
#include "checker/mem_state_tracker.h"
#include "npu_tool_log.h"
#include "device_instr/common/instruction_id.h"
#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace npucheck {
namespace {
using namespace race;
} // namespace

Racechecker::Racechecker(race::Limits limits, std::shared_ptr<MemoryAnalysis> analysis)
    : analysis_(analysis ? std::move(analysis) : std::make_shared<MemoryAnalysis>()), limits_(limits)
{}
const std::vector<CallbackSpec>& Racechecker::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> ids{
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_TRANSFER_END},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END}};
    return ids;
}
bool Racechecker::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    const auto malformed = [this] {
        incomplete_ = true;
        ++reasons_["malformed_callback"];
        for (auto& launch : launches_)
            launch.second.reasons.insert("malformed_callback");
        return false;
    };
    const auto acceptLaunch = [this](uint64_t launchId) {
        if (launches_.count(launchId) != 0 || launches_.size() + finished_.size() < limits_.events)
            return true;
        incomplete_ = true;
        ++reasons_["launch_budget"];
        return false;
    };
    const auto& subscriptions = GetSubscribedID();
    if (std::find(subscriptions.begin(), subscriptions.end(), CallbackSpec{domain, cbid}) == subscriptions.end())
        return true;
    if (!data)
        return malformed();
    if (domain == ACLSAN_CB_DOMAIN_MEMORY) {
        const auto& common = *static_cast<const AclsanCallbackCommonData*>(data);
        if (analysis_->IsLegacyMemcpy(cbid, common))
            return true;
        if (!analysis_->Managed() || common.result != 0) {
            incomplete_ = true;
            ++reasons_["host_operation_unavailable"];
            return true;
        }
        hostEvents_ += analysis_->HostOperations().size();
        std::set<std::string> reasons;
        PublishConflicts(analysis_->HostConflicts(limits_, reasons), reports);
        for (const auto& reason : reasons) {
            incomplete_ = true;
            ++reasons_[reason];
        }
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        const auto& s = *static_cast<const AclsanSynchronizeData*>(data);
        if (s.common.version != ACLSAN_API_VERSION || s.common.size < sizeof(s))
            return malformed();
        if (s.common.result != 0) {
            for (auto& launch : launches_)
                if (launch.second.registered && launch.second.stream == s.stream)
                    launch.second.reasons.insert("synchronization_failed");
        } else {
            // This closes the observed callback batch; it does not certify raw coverage.
            Collect(s.stream, false, reports);
        }
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto& l = *static_cast<const AclsanLaunchData*>(data);
        if (l.common.version != ACLSAN_API_VERSION || l.common.size < sizeof(l))
            return malformed();
        if (l.common.result != 0)
            return true;
        if (l.launchId == 0 || finished_.count(l.launchId) != 0)
            return malformed();
        if (!acceptLaunch(l.launchId))
            return true;
        auto& state = GetLaunch(l.launchId);
        if (state.registered)
            return malformed();
        state.stream = l.stream;
        state.registered = true;
        if (l.functionName)
            state.name = l.functionName;
        return true;
    }
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    const size_t size = cbid == ACLSAN_CBID_DEVICE_STATE        ? sizeof(AclsanDeviceRegisterStateData) :
                        cbid == ACLSAN_CBID_DEVICE_SYNC         ? sizeof(AclsanDeviceSyncData) :
                        cbid == ACLSAN_CBID_DEVICE_PIPE_BARRIER ? sizeof(AclsanDeviceBarrierData) :
                                                                  sizeof(AclsanDeviceMemoryAccessData);
    if (h.version != ACLSAN_API_VERSION || h.size < size || h.launchId == 0 || h.instrExecId == 0 ||
        finished_.count(h.launchId) != 0)
        return malformed();
    if (!acceptLaunch(h.launchId))
        return true;
    return StoreDeviceEvent(domain, cbid, data);
}

bool Racechecker::StoreDeviceEvent(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    auto& state = GetLaunch(h.launchId);
    if (id == ACLSAN_CBID_DEVICE_STATE) {
        const auto& reg = *static_cast<const AclsanDeviceRegisterStateData*>(data);
        if (reg.regId != ACLSAN_DEVICE_REGISTER_FPC)
            return true;
        if (state.memory.size() + state.sync.size() + state.registers.size() >= limits_.events)
            state.reasons.insert("event_budget");
        else if (!analysis_->Managed())
            state.registers.push_back(reg);
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        ++memoryEvents_;
        const auto& a = *static_cast<const AclsanDeviceMemoryAccessData*>(data);
        const bool exact = h.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT && !a.vectorMask0 && !a.vectorMask1 &&
                           !a.predicateMask0 && !a.predicateMask1 && !a.memoryFlags;
        if ((h.flags & ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN) != 0)
            state.reasons.insert("atomic_configuration_unknown");
        if (!exact) {
            // A synthetic conversion-failure marker denotes a missing footprint.
            if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN)
                state.reasons.insert("incomplete_memory_observation");
            ++skippedMemoryEvents_;
            ++state.skippedMemory;
            if (a.accessCount != 0 && a.accessIndex < a.accessCount && state.skippedMemory <= limits_.events)
                state.skippedFragments[race::Key(h)].insert(a.accessIndex);
            return true;
        }
    } else if (id == ACLSAN_CBID_DEVICE_PIPE_BARRIER) {
        ++syncEvents_;
        if (h.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE && h.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT) {
            ++skippedSyncEvents_;
            state.reasons.insert("sync_coverage_unknown");
            return true;
        }
    } else {
        ++syncEvents_;
        const auto& s = *static_cast<const AclsanDeviceSyncData*>(data);
        if (s.scope == ACLSAN_DEVICE_SYNC_SCOPE_PIPE && s.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG &&
            s.srcPipe == ACLSAN_DEVICE_PIPE_MATRIX && s.dstPipe == ACLSAN_DEVICE_PIPE_FIXPIPE &&
            (s.mode == UNIT_FLAG_MODE_START || s.mode == UNIT_FLAG_MODE_END)) {
            // unitFlag is a per-fragment, bidirectional hardware handshake.
            // The current whole-instruction graph cannot prove those edges.
            // Do not treat it as a cross-core flag or report an unproven race.
            ++skippedSyncEvents_;
            state.reasons.insert("unit_flag_semantics_unknown");
            return true;
        }
        if (h.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE && h.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT) {
            ++skippedSyncEvents_;
            state.reasons.insert("sync_coverage_unknown");
            return true;
        }
    }
    if (state.memory.size() + state.sync.size() + state.registers.size() >= limits_.events) {
        state.reasons.insert("event_budget");
        return true;
    }
    if (analysis_->Managed())
        return true;
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS)
        state.memory.push_back(*static_cast<const AclsanDeviceMemoryAccessData*>(data));
    else if (id == ACLSAN_CBID_DEVICE_SYNC) {
        state.sync.push_back(race::NormalizeSync(*static_cast<const AclsanDeviceSyncData*>(data)));
    } else {
        const auto& b = *static_cast<const AclsanDeviceBarrierData*>(data);
        // Keep barrier transport separate at the ACLSan boundary, but normalize
        // it to a sync event inside racecheck. Barriers have no event ID.
        AclsanDeviceSyncData s{};
        s.header = b.header;
        s.syncKind = ACLSAN_DEVICE_SYNC_KIND_UNKNOWN;
        s.action = ACLSAN_DEVICE_SYNC_ACTION_UNKNOWN;
        s.scope = ACLSAN_DEVICE_SYNC_SCOPE_PIPE;
        s.srcPipe = b.header.pipeline;
        s.dstPipe = b.pipe;
        s.mode = 0;
        s.objectId = 0;
        state.sync.push_back(s);
    }
    return true;
}
void Racechecker::Collect(void* stream, bool final, CheckerReportList& reports)
{
    for (auto it = launches_.begin(); it != launches_.end();) {
        auto& l = it->second;
        if (!final && (!l.registered || l.stream != stream)) {
            ++it;
            continue;
        }
        if (final)
            l.reasons.insert("missing_sync_end");
        Finish(it->first, l, reports);
        finished_.insert(it->first);
        it = launches_.erase(it);
    }
}
void Racechecker::Finalize(CheckerReportList& reports) { Collect(nullptr, true, reports); }
void Racechecker::Finish(uint64_t id, Launch& l, CheckerReportList& reports)
{
    ++launchesCount_;
    if (!l.registered)
        l.reasons.insert("missing_launch");
    if (l.memory.empty() && !l.skippedMemory)
        l.reasons.insert("empty_memory_observation");
    std::optional<uint32_t> device;
    const auto checkHeader = [&](const AclsanDeviceEventHeader& h) {
        if (device && *device != h.deviceId)
            l.reasons.insert("device_mismatch");
        device = h.deviceId;
    };
    for (const auto& a : l.memory)
        checkHeader(a.header);
    for (const auto& a : l.sync)
        checkHeader(a.header);
    try {
        Analyze(id, l, reports);
    } catch (const RacecheckError& error) {
        l.reasons.insert(
            error.Kind() == RacecheckErrorKind::MemoryAllocationFailed ? "allocation_failure" : "invalid_memory_state");
        ASCTOOL_ERROR("racecheck launch=%llu stopped: %s", static_cast<unsigned long long>(id), error.what());
    } catch (const std::bad_alloc&) {
        l.reasons.insert("allocation_failure");
    }
    for (const auto& reason : l.reasons)
        ++reasons_[reason];
    incomplete_ |= !l.reasons.empty();
}

// Per-launch analysis state. It is discarded after replay and never shared
// across launches; the graph is sealed before any memory-state comparison.
class Racechecker::LaunchProcessor {
    Racechecker& checker;
    Launch& launch;
    CheckerReportList& reports;
    const Limits& limits_;
    std::unique_ptr<DependencyGraph> ownedDependency;
    DependencyGraph& dependency;
    HappensBeforeGraph& graph;
    std::vector<DependencyGraph::Access>& accesses;
    size_t comparisons = 0;

public:
    LaunchProcessor(Racechecker& owner, Launch& data, CheckerReportList& output)
        : checker(owner),
          launch(data),
          reports(output),
          limits_(owner.limits_),
          ownedDependency(owner.analysis_->Managed() ? nullptr : std::make_unique<DependencyGraph>(limits_)),
          dependency(ownedDependency ? *ownedDependency : owner.analysis_->Graph(data.id, limits_, data.reasons)),
          graph(dependency.graph),
          accesses(dependency.accesses)
    {}
    void Run()
    {
        if (ownedDependency)
            BuildDependencyGraph(
                launch.memory, launch.sync, launch.skippedFragments, limits_, launch.reasons, dependency,
                launch.registers);
        checker.graphNodesPeak_ = std::max(checker.graphNodesPeak_, graph.Order().size());
        checker.graphEdgesPeak_ = std::max(checker.graphEdgesPeak_, graph.EdgeCount());
        checker.graphChainsPeak_ = std::max(checker.graphChainsPeak_, graph.ChainCount());
        graph.BeginAnalysis(limits_.analysisWork);
        if (launch.reasons.empty())
            ReplayMemory();
    }

private:
    HappensBeforeGraph::Relation CompareAccesses(size_t previous, size_t current)
    {
        if (++comparisons > limits_.comparisons) {
            launch.reasons.insert("comparison_budget");
            return HappensBeforeGraph::Relation::Unknown;
        }
        const auto relation = graph.Compare(accesses[previous].node, accesses[current].node);
        checker.graphQueryWorkPeak_ = std::max(checker.graphQueryWorkPeak_, graph.QueryWork());
        if (relation == HappensBeforeGraph::Relation::Unknown)
            launch.reasons.insert("graph_query_budget");
        if (relation == HappensBeforeGraph::Relation::After)
            launch.reasons.insert("invalid_analysis_order");
        return relation;
    }

    // Checker-owned replay state. The graph is already sealed; this stage
    // queries ordering and drives memstate without changing either module's role.
    struct MemoryReplay {
        // Conservative allowance for a segment map node and allocator bookkeeping.
        static constexpr size_t SEGMENT_NODE_ALLOWANCE = 4 * sizeof(void*);
        LaunchProcessor& processor;
        const std::vector<AclsanDeviceMemoryAccessData>& records;
        MemStateTracker& memory;
        size_t stateBytes = 0;
        size_t stateSegments = 0;

        struct ConflictBatch {
            std::vector<AccessPair> confirmed;
            std::vector<NpuCheckRacecheckReport> reports;
            std::set<std::pair<uint64_t, uint64_t>> ranges;
        };

        racecheck::PreparedAccess PrepareAccess(size_t index) const
        {
            racecheck::PreparedAccess prepared{
                racecheck::MakeKey(records[index]), racecheck::AccessMode(records[index]), {}};
            // unitFlag graph nodes may cover only a fragment of the raw operand.
            for (const auto range : processor.accesses[index].ranges)
                prepared.ranges.push_back({range.begin, range.end});
            return prepared;
        }

        static size_t StateStorageBytes(const MemState& state)
        {
            size_t history = state.readers.capacity();
            for (const auto& writers : state.atomicWriters)
                history += writers.capacity();
            return sizeof(MemState) + SEGMENT_NODE_ALLOWANCE + history * sizeof(CbIndex);
        }

        // Account only touched intervals and their merge neighbours. This avoids a
        // full-table scan on every access while retaining history-capacity costs.
        std::pair<size_t, size_t> MeasureUsage(const PhysicalSpaceKey& key, const std::vector<Interval>& windows) const
        {
            std::pair<size_t, size_t> result{};
            const auto table = memory.Tables().find(key);
            if (table == memory.Tables().end())
                return result;
            for (const auto window : windows) {
                auto it = table->second.lower_bound(window.begin);
                if (it != table->second.begin() && std::prev(it)->second.end > window.begin)
                    --it;
                for (; it != table->second.end() && it->first < window.end; ++it) {
                    ++result.first;
                    result.second += StateStorageBytes(it->second);
                }
            }
            return result;
        }

        void CommitConflicts(size_t current, const racecheck::PreparedAccess& prepared, ConflictBatch& batch)
        {
            if (batch.confirmed.empty())
                return;
            // Allocate the complete batch before mutating MemStateTracker. If
            // report construction fails, neither reports nor memory state change.
            auto pending = processor.reports;
            for (auto& evidence : batch.reports)
                pending.emplace_back(std::move(evidence));
            memory.UpdateMemState(current, batch.confirmed, prepared);
            processor.reports.swap(pending);
            for (const auto& evidence : batch.reports) {
                if (evidence.kind == ConflictKind::RAW)
                    ++processor.checker.raw_;
                else if (evidence.kind == ConflictKind::WAR)
                    ++processor.checker.war_;
                else if (evidence.kind == ConflictKind::WAW)
                    ++processor.checker.waw_;
                else
                    ++processor.checker.atomic_;
            }
        }

        bool CheckAccess(size_t current)
        {
            const auto prepared = PrepareAccess(current);
            const auto candidates = memory.CheckPotentialRace(current, prepared);
            ConflictBatch batch;
            for (const auto& candidate : candidates) {
                const auto relation = processor.CompareAccesses(candidate.prevIndex, current);
                if (relation == HappensBeforeGraph::Relation::Unordered) {
                    batch.confirmed.push_back(candidate);
                    // A single current access can have multiple historical
                    // candidates for the same range. Keep one user-facing
                    // report for that memory range, while submitting every
                    // candidate to the tracker so the range is frozen safely.
                    if (batch.ranges.emplace(candidate.begin, candidate.end).second) {
                        auto evidence = MakeRacecheckDiagnostics({candidate}, records, current, prepared);
                        evidence.front().reportId = processor.checker.raw_ + processor.checker.war_ +
                                                    processor.checker.waw_ + processor.checker.atomic_ +
                                                    batch.reports.size() + 1;
                        batch.reports.push_back(std::move(evidence.front()));
                    }
                    continue;
                }
                if (relation != HappensBeforeGraph::Relation::Before) {
                    CommitConflicts(current, prepared, batch);
                    return false;
                }
            }
            CommitConflicts(current, prepared, batch);
            return true;
        }

        bool CheckInstruction(const std::vector<size_t>& indices)
        {
            // Check every operand against the prior instruction state before any
            // operand updates it. Overlapping source/destination fragments cannot
            // hide an earlier inter-instruction hazard or create a self-race.
            for (auto current : indices)
                if (!CheckAccess(current))
                    return false;
            return true;
        }

        std::vector<Interval> AffectedWindows(const racecheck::PreparedAccess& prepared) const
        {
            std::vector<Interval> windows;
            const auto table = memory.Tables().find(prepared.key);
            for (const auto range : prepared.ranges) {
                Interval window{range.begin, range.end};
                if (table != memory.Tables().end() && !table->second.empty()) {
                    auto first = table->second.lower_bound(range.begin);
                    if (first != table->second.begin())
                        window.begin = std::prev(first)->first;
                    auto last = table->second.upper_bound(range.end);
                    if (last != table->second.end())
                        window.end = std::max(window.end, last->second.end);
                    else
                        window.end = std::max(window.end, table->second.rbegin()->second.end);
                }
                windows.push_back(window);
            }
            MergeIntervals(windows);
            return windows;
        }

        bool UpdateAccess(size_t current)
        {
            const auto prepared = PrepareAccess(current);
            if (prepared.ranges.empty())
                return true;
            if (processor.limits_.stateBytes < sizeof(MemState) + SEGMENT_NODE_ALLOWANCE) {
                processor.launch.reasons.insert("state_plan_budget");
                return false;
            }
            const auto windows = AffectedWindows(prepared);
            const auto before = MeasureUsage(prepared.key, windows);
            memory.UpdateMemState(current, {}, prepared);
            const auto after = MeasureUsage(prepared.key, windows);
            stateSegments = stateSegments - before.first + after.first;
            stateBytes = stateBytes - before.second + after.second;
            processor.checker.stateBytesPeak_ = std::max(processor.checker.stateBytesPeak_, stateBytes);
            processor.checker.stateSegmentsPeak_ = std::max(processor.checker.stateSegmentsPeak_, stateSegments);
            if (stateSegments > processor.limits_.stateSegments || stateBytes > processor.limits_.stateBytes) {
                processor.launch.reasons.insert("memory_state_budget");
                return false;
            }
            return true;
        }

        bool UpdateInstruction(const std::vector<size_t>& indices)
        {
            // Source reads commit before ordinary destination writes. This retains
            // the destination writer when an in-place instruction has both operands.
            auto updates = indices;
            std::stable_sort(updates.begin(), updates.end(), [this](size_t a, size_t b) {
                return records[a].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ &&
                       records[b].accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_READ;
            });
            for (auto current : updates)
                if (!UpdateAccess(current))
                    return false;
            return true;
        }
    };

    void ReplayMemory()
    {
        // Freeze before querying: a missing path is meaningful only after all
        // supported synchronization edges have been collected and validated.
        // The graph owns ordering; memstate owns physical interval history. Records
        // remain immutable while topological replay supplies the causal commit order.
        auto history = checker.analysis_->state->NewAccessHistory();
        auto& records = history->records;
        records.reserve(accesses.size());
        std::map<size_t, std::vector<size_t>> instructions;
        for (size_t i = 0; i < accesses.size(); ++i) {
            records.push_back(accesses[i].data);
            instructions[accesses[i].node].push_back(i);
        }
        MemoryReplay replay{*this, records, history->tracker};
        for (auto node : graph.Order()) {
            auto instruction = instructions.find(node);
            if (instruction == instructions.end())
                continue;
            if (!replay.CheckInstruction(instruction->second))
                return;
            if (!replay.UpdateInstruction(instruction->second))
                return;
        }
    }
};

void Racechecker::Analyze([[maybe_unused]] uint64_t id, Launch& launch, CheckerReportList& reports)
{
    LaunchProcessor processor(*this, launch, reports);
    processor.Run();
    if (analysis_->Managed()) {
        PublishConflicts(analysis_->CrossLaunchConflicts(id, limits_, launch.reasons), reports);
        if (launch.reasons.empty())
            PublishConflicts(analysis_->CrossLaunchHostConflicts(id, limits_, launch.reasons), reports);
    }
}

void Racechecker::PublishConflicts(std::vector<NpuCheckRacecheckReport> conflicts, CheckerReportList& reports)
{
    for (auto& report : conflicts) {
        report.reportId = raw_ + war_ + waw_ + atomic_ + 1;
        if (report.kind == ConflictKind::RAW)
            ++raw_;
        else if (report.kind == ConflictKind::WAR)
            ++war_;
        else if (report.kind == ConflictKind::WAW)
            ++waw_;
        else
            ++atomic_;
        reports.emplace_back(std::move(report));
    }
}

bool Racechecker::HasErrors() const { return raw_ != 0 || war_ != 0 || waw_ != 0 || atomic_ != 0; }
bool Racechecker::AnalysisComplete() const
{
    return !incomplete_ && launches_.empty() && (launchesCount_ != 0 || hostEvents_ != 0);
}
std::string Racechecker::Summary() const
{
    std::ostringstream s;
    s << "tool=racecheck launches=" << launchesCount_ << " memory_events=" << memoryEvents_
      << " sync_events=" << syncEvents_ << " host_operations=" << hostEvents_ << " raw=" << raw_
      << " skipped_memory_events=" << skippedMemoryEvents_ << " skipped_sync_events=" << skippedSyncEvents_
      << " war=" << war_ << " waw=" << waw_ << " atomic_semantic=" << atomic_ << " graph_nodes_peak=" << graphNodesPeak_
      << " graph_edges_peak=" << graphEdgesPeak_ << " graph_chains_peak=" << graphChainsPeak_
      << " graph_query_work_peak=" << graphQueryWorkPeak_ << " state_accounted_bytes_peak=" << stateBytesPeak_
      << " state_segments_peak=" << stateSegmentsPeak_ << " pending_launches=" << launches_.size()
      << " analysis_complete=" << (AnalysisComplete() ? "true" : "false")
      << " errors=" << (raw_ + war_ + waw_ + atomic_) << " warnings=0";
    for (const auto& reason : reasons_)
        s << " " << reason.first << "=" << reason.second;
    return s.str();
}
} // namespace npucheck
