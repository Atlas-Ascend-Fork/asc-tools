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
#include "checker/racecheck/analysis_phase_timer.h"
#include "checker/racecheck_reports.h"
#include <optional>
#include "checker/racecheck/happens_before_graph.h"
#include "checker/mem_state_tracker.h"
#include "checker/memory_access_mode.h"
#include "npu_tool_log.h"
#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace npucheck {
namespace {
using namespace race;
} // namespace

Racechecker::Racechecker(race::Limits limits, std::shared_ptr<MemoryAnalysis> analysis)
    : analysis_(analysis ? std::move(analysis) : std::make_shared<MemoryAnalysis>(false, limits)),
      limits_(analysis_->Managed() ? analysis_->AnalysisLimits() : limits)
{}
const std::vector<CallbackSpec>& Racechecker::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> ids{
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC},
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_TRANSFER_END},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END},
        {ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE}};
    return ids;
}
bool Racechecker::MarkMalformedCallback()
{
    incomplete_ = true;
    ++reasons_["malformed_callback"];
    for (auto& launch : launches_)
        launch.second.reasons.insert("malformed_callback");
    return false;
}

bool Racechecker::AcceptLaunch(uint64_t launchId)
{
    if (launches_.count(launchId) != 0 ||
        !ExceedsBudget(limits_.TrackedLaunchBudget(), {launches_.size(), finished_.size(), 1}))
        return true;
    incomplete_ = true;
    ++reasons_["launch_budget"];
    return false;
}

bool Racechecker::HandleHostMemory(AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
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

bool Racechecker::HandleStreamSynchronization(const void* data)
{
    const auto& s = *static_cast<const AclsanSynchronizeData*>(data);
    if (s.common.version != ACLSAN_API_VERSION || s.common.size < sizeof(s))
        return MarkMalformedCallback();
    if (s.common.result != 0) {
        incomplete_ = true;
        ++reasons_["synchronization_failed"];
        for (auto& launch : launches_)
            if (launch.second.registered && launch.second.stream == s.stream)
                launch.second.reasons.insert("synchronization_failed");
    }
    return true;
}

bool Racechecker::HandleOperatorCompletion(const void* data, CheckerReportList& reports)
{
    // This launch's device cbdata has been fully dispatched; analyze it
    // without waiting for the stream synchronization host boundary.
    const auto& e = *static_cast<const AclsanOperatorExecutionData*>(data);
    if (e.common.version != ACLSAN_API_VERSION || e.common.size < sizeof(e))
        return MarkMalformedCallback();
    const auto it = launches_.find(e.launchId);
    if (it == launches_.end()) {
        if (finished_.count(e.launchId) != 0)
            return true;
        incomplete_ = true;
        ++reasons_["unknown_operator_execution"];
        return true;
    }
    Launch& launch = it->second;
    if ((e.flags & static_cast<uint32_t>(ACLSAN_OPERATOR_EXECUTION_FLAG_INCOMPLETE)) != 0U)
        launch.reasons.insert("operator_execution_incomplete");
    if ((e.flags & static_cast<uint32_t>(ACLSAN_OPERATOR_EXECUTION_FLAG_DROPPED)) != 0U)
        launch.reasons.insert("trace_dropped");
    Finish(e.launchId, launch, reports);
    finished_.insert(e.launchId);
    launches_.erase(it);
    return true;
}

bool Racechecker::HandleLaunch(const void* data)
{
    const auto& l = *static_cast<const AclsanLaunchData*>(data);
    if (l.common.version != ACLSAN_API_VERSION || l.common.size < sizeof(l))
        return MarkMalformedCallback();
    if (l.common.result != 0)
        return true;
    if (l.launchId == 0 || finished_.count(l.launchId) != 0)
        return MarkMalformedCallback();
    if (!AcceptLaunch(l.launchId))
        return true;
    auto& state = GetLaunch(l.launchId);
    if (state.registered)
        return MarkMalformedCallback();
    state.stream = l.stream;
    state.registered = true;
    if (l.functionName != nullptr)
        state.name = l.functionName;
    return true;
}

bool Racechecker::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    if (domain == ACLSAN_CB_DOMAIN_RESOURCE)
        return true;
    const auto& subscriptions = GetSubscribedID();
    if (std::find(subscriptions.begin(), subscriptions.end(), CallbackSpec{domain, cbid}) == subscriptions.end())
        return true;
    if (data == nullptr)
        return MarkMalformedCallback();
    analysis_->ObserveStandaloneSynchronization(domain, cbid, data);
    if (domain == ACLSAN_CB_DOMAIN_MEMORY) {
        return HandleHostMemory(cbid, data, reports);
    }
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        return HandleStreamSynchronization(data);
    }
    if (domain == ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION) {
        return HandleOperatorCompletion(data, reports);
    }
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        return HandleLaunch(data);
    }
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    const size_t size = cbid == ACLSAN_CBID_DEVICE_STATE        ? sizeof(AclsanDeviceRegisterStateData) :
                        cbid == ACLSAN_CBID_DEVICE_VF           ? sizeof(AclsanDeviceVfData) :
                        cbid == ACLSAN_CBID_DEVICE_SYNC         ? sizeof(AclsanDeviceSyncData) :
                        cbid == ACLSAN_CBID_DEVICE_PIPE_BARRIER ? sizeof(AclsanDeviceBarrierData) :
                                                                  sizeof(AclsanDeviceMemoryAccessData);
    if (h.version != ACLSAN_API_VERSION || h.size < size || h.launchId == 0 || h.instrExecId == 0 ||
        finished_.count(h.launchId) != 0)
        return MarkMalformedCallback();
    if (!AcceptLaunch(h.launchId))
        return true;
    return StoreDeviceEvent(domain, cbid, data);
}

bool Racechecker::EventBudgetExceeded(Launch& launch, const AclsanDeviceEventHeader& header)
{
    constexpr size_t MANAGED_EVENT_INCREMENT = 0;
    constexpr size_t STANDALONE_EVENT_INCREMENT = 1;
    const size_t incoming = analysis_->Managed() ? MANAGED_EVENT_INCREMENT : STANDALONE_EVENT_INCREMENT;
    if (!race::ExceedsBudget(
            limits_.LaunchEventBudget(), {launch.memory.size(), launch.sync.size(), launch.registers.size(), incoming}))
        return false;
    if (launch.reasons.insert("event_budget").second)
        ASCTOOL_WARNING(
            "analysis budget: launch=%llu stage=events memory=%zu sync=%zu registers=%zu limit=%zu",
            static_cast<unsigned long long>(header.launchId), launch.memory.size(), launch.sync.size(),
            launch.registers.size(), limits_.LaunchEventBudget());
    return true;
}
bool Racechecker::FullDeviceEventBudget(const Launch& state) const
{
    return state.memory.size() + state.sync.size() + state.registers.size() + state.localBarriers.size() +
               state.vfs.size() >=
           limits_.LaunchEventBudget();
}

void Racechecker::StoreVfEvent(Launch& state, const AclsanDeviceVfData& vf)
{
    if (FullDeviceEventBudget(state)) {
        state.reasons.insert("event_budget");
        return;
    }
    if (!analysis_->Managed()) {
        state.vfs.push_back(vf);
    }
    if (vf.status != ACLSAN_DEVICE_VF_SUCCESS || vf.header.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT) {
        state.reasons.insert("vf_expansion_incomplete");
    }
    constexpr auto knownWarnings = static_cast<uint32_t>(ACLSAN_DEVICE_VF_MASK_NOT_REFRESHED);
    if ((vf.warningFlags & ~knownWarnings) != 0U) {
        state.reasons.insert("unknown_vf_warning");
    }
}

bool Racechecker::AcceptMemoryEvent(Launch& state, const AclsanDeviceMemoryAccessData& a)
{
    ++memoryEvents_;
    const auto& h = a.header;
    constexpr auto paddingFlag = static_cast<uint32_t>(ACLSAN_DEVICE_MEMORY_FLAG_HARDWARE_PADDING);
    const bool exact = h.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT && a.vectorMask0 == 0U && a.vectorMask1 == 0U &&
                       a.predicateMask0 == 0U && a.predicateMask1 == 0U && (a.memoryFlags & ~paddingFlag) == 0U;
    if ((h.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN)) != 0U)
        state.reasons.insert("atomic_configuration_unknown");
    if (!exact) {
        // A synthetic conversion-failure marker denotes a missing footprint.
        if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN)
            state.reasons.insert("incomplete_memory_observation");
        ++skippedMemoryEvents_;
        ++state.skippedMemory;
        if (a.accessCount != 0 && a.accessIndex < a.accessCount && state.skippedMemory <= limits_.LaunchEventBudget())
            state.skippedFragments[race::Key(a)].insert(a.accessIndex);
        return false;
    }
    return true;
}

bool Racechecker::AcceptBarrierEvent(Launch& state, const AclsanDeviceBarrierData& barrier)
{
    ++syncEvents_;
    const auto& h = barrier.header;
    if (barrier.barrierKind == ACLSAN_DEVICE_BARRIER_LOCAL_MEM && h.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT &&
        h.size >= sizeof(AclsanDeviceBarrierData)) {
        if (FullDeviceEventBudget(state)) {
            state.reasons.insert("event_budget");
        } else if (!analysis_->Managed()) {
            state.localBarriers.push_back(barrier);
        }
        return false;
    }
    if (barrier.barrierKind != ACLSAN_DEVICE_BARRIER_PIPE ||
        (h.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE && h.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT)) {
        ++skippedSyncEvents_;
        state.reasons.insert("sync_coverage_unknown");
        return false;
    }
    return true;
}

bool Racechecker::AcceptSyncEvent(Launch& state, const AclsanDeviceSyncData& s)
{
    ++syncEvents_;
    const auto& h = s.header;
    if (s.scope == ACLSAN_DEVICE_SYNC_SCOPE_PIPE && s.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG &&
        s.srcPipe == ACLSAN_DEVICE_PIPE_MATRIX && s.dstPipe == ACLSAN_DEVICE_PIPE_FIXPIPE &&
        (s.mode == UNIT_FLAG_MODE_START || s.mode == UNIT_FLAG_MODE_END)) {
        // unitFlag is a per-fragment, bidirectional hardware handshake.
        // The current whole-instruction graph cannot prove those edges.
        // Do not treat it as a cross-core flag or report an unproven race.
        ++skippedSyncEvents_;
        state.reasons.insert("unit_flag_semantics_unknown");
        return false;
    }
    if (h.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE && h.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT) {
        ++skippedSyncEvents_;
        state.reasons.insert("sync_coverage_unknown");
        return false;
    }
    return true;
}

bool Racechecker::StoreDeviceEvent(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    auto& state = GetLaunch(h.launchId);
    if (id == ACLSAN_CBID_DEVICE_STATE) {
        const auto& reg = *static_cast<const AclsanDeviceRegisterStateData*>(data);
        if (reg.regId != ACLSAN_DEVICE_REGISTER_FPC)
            return true;
        if (!EventBudgetExceeded(state, h) && !analysis_->Managed())
            state.registers.push_back(reg);
        return true;
    }
    if (id == ACLSAN_CBID_DEVICE_VF) {
        StoreVfEvent(state, *static_cast<const AclsanDeviceVfData*>(data));
        return true;
    }
    const bool isMemory = domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS;
    bool accepted;
    if (isMemory) {
        accepted = AcceptMemoryEvent(state, *static_cast<const AclsanDeviceMemoryAccessData*>(data));
    } else if (id == ACLSAN_CBID_DEVICE_PIPE_BARRIER) {
        accepted = AcceptBarrierEvent(state, *static_cast<const AclsanDeviceBarrierData*>(data));
    } else {
        accepted = AcceptSyncEvent(state, *static_cast<const AclsanDeviceSyncData*>(data));
    }
    if (!accepted) {
        return true;
    }
    if (EventBudgetExceeded(state, h))
        return true;
    if (analysis_->Managed())
        return true;
    if (isMemory)
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
            l.reasons.insert("missing_operator_execution");
        Finish(it->first, l, reports);
        finished_.insert(it->first);
        it = launches_.erase(it);
    }
}
void Racechecker::Finalize(CheckerReportList& reports) { Collect(nullptr, true, reports); }
void Racechecker::Finish(uint64_t id, Launch& launch, CheckerReportList& reports)
{
    ++launchesCount_;
    if (!launch.registered)
        launch.reasons.insert("missing_launch");
    if (analysis_->HasInvalidSynchronization(id))
        launch.reasons.insert("invalid_synchronization");
    constexpr uint64_t NO_UNSUPPORTED_UB_EVENTS = 0;
    const auto unsupported = analysis_->Managed() ? analysis_->Get(id).unsupportedUbEvents : NO_UNSUPPORTED_UB_EVENTS;
    const auto maximum = std::numeric_limits<uint64_t>::max();
    unsupportedUbEvents_ = unsupported <= maximum - unsupportedUbEvents_ ? unsupportedUbEvents_ + unsupported : maximum;
    std::optional<uint32_t> device;
    const auto checkHeader = [&device, &launch](const AclsanDeviceEventHeader& h) {
        if (device && *device != h.deviceId)
            launch.reasons.insert("device_mismatch");
        device = h.deviceId;
    };
    for (const auto& a : launch.memory)
        checkHeader(a.header);
    for (const auto& a : launch.sync)
        checkHeader(a.header);
    for (const auto& a : launch.vfs) {
        checkHeader(a.header);
    }
    for (const auto& a : launch.localBarriers) {
        checkHeader(a.header);
    }
    try {
        Analyze(id, launch, reports);
    } catch (const RacecheckError& error) {
        launch.reasons.insert(
            error.Kind() == RacecheckErrorKind::MemoryAllocationFailed ? "allocation_failure" : "invalid_memory_state");
        ASCTOOL_ERROR("racecheck launch=%llu stopped: %s", static_cast<unsigned long long>(id), error.what());
    } catch (const std::bad_alloc&) {
        launch.reasons.insert("allocation_failure");
    }
    for (const auto& reason : launch.reasons)
        ++reasons_[reason];
    incomplete_ = incomplete_ || !launch.reasons.empty();
}

// Per-launch analysis state. It is discarded after replay and never shared
// across launches; the graph is sealed before any memory-state comparison.
class Racechecker::LaunchProcessor {
    using Access = DependencyGraph::Access;
    Racechecker& checker;
    Launch& launch;
    CheckerReportList& published;
    CheckerReportList reports;
    const Limits& limits_;
    std::unique_ptr<DependencyGraph> ownedDependency;
    DependencyGraph& dependency;
    HappensBeforeGraph& graph;
    std::vector<Access>& accesses;
    size_t comparisons = 0;

public:
    LaunchProcessor(Racechecker& owner, Launch& data, CheckerReportList& output)
        : checker(owner),
          launch(data),
          published(output),
          limits_(owner.limits_),
          ownedDependency(!owner.analysis_->Managed() ? std::make_unique<DependencyGraph>(limits_) : nullptr),
          dependency(ownedDependency ? *ownedDependency : owner.analysis_->Graph(data.id, limits_, data.reasons)),
          graph(dependency.graph),
          accesses(dependency.accesses)
    {}

    void Run()
    {
        if (ownedDependency) {
            BuildDependencyGraph(
                launch.memory, launch.sync, launch.skippedFragments, limits_, launch.reasons, dependency,
                launch.registers, launch.vfs, launch.localBarriers);
        }
        checker.warnings_ += dependency.warnings;
        checker.graphNodesPeak_ = std::max(checker.graphNodesPeak_, graph.Order().size());
        checker.graphEdgesPeak_ = std::max(checker.graphEdgesPeak_, graph.EdgeCount());
        graph.BeginAnalysis(limits_.ReachabilityPhaseWorkBudget());
        if (!launch.reasons.empty()) {
            return;
        }
        race::AnalysisPhaseTimer phase(launch.id, "memory_replay");
        struct Publication {
            Racechecker& checker;
            uint64_t raw, war, waw, atomic;
            bool committed = false;
            ~Publication()
            {
                if (!committed) {
                    checker.raw_ = raw;
                    checker.war_ = war;
                    checker.waw_ = waw;
                    checker.atomic_ = atomic;
                }
            }
        } publication{checker, checker.raw_, checker.war_, checker.waw_, checker.atomic_};
        // A FIX execution can span topological positions due to L0C unit release.
        // Keep every launch result private until all endpoints have replayed successfully.
        if (!ReplayMemory()) {
            return;
        }
        if (reports.size() > published.max_size() - published.size()) {
            throw RacecheckError(RacecheckErrorKind::CapacityExceeded, "race report capacity exceeded");
        }
        published.reserve(published.size() + reports.size());
        static_assert(std::is_nothrow_move_constructible_v<CheckerReport>);
        for (auto& report : reports) {
            published.emplace_back(std::move(report));
        }
        publication.committed = true;
    }

private:
    HappensBeforeGraph::Relation CompareNodes(size_t previous, size_t current)
    {
        using Relation = HappensBeforeGraph::Relation;
        const auto& lhs = accesses[previous];
        const auto& rhs = accesses[current];
        const auto a = race::DynamicVfKey(lhs.data);
        const auto b = race::DynamicVfKey(rhs.data);
        if (a == b)
            return graph.Compare(lhs.node, rhs.node);

        const auto lhsRank = graph.TraversalRank(lhs.node);
        const auto rhsRank = graph.TraversalRank(rhs.node);
        if (!lhsRank || !rhsRank || *lhsRank == *rhsRank)
            return graph.Compare(lhs.node, rhs.node);
        const bool forward = *lhsRank < *rhsRank;
        const auto& source = forward ? a : b;
        const auto& target = forward ? b : a;
        // External edges leave a VF through completion and enter through entry.
        // Reuse that exact path query for all access pairs across the boundary.
        const auto from = source ? dependency.vfBoundaries.at(*source).completion : (forward ? lhs.node : rhs.node);
        const auto to = target ? dependency.vfBoundaries.at(*target).entry : (forward ? rhs.node : lhs.node);
        if (graph.TraversalRank(from) >= graph.TraversalRank(to))
            return Relation::Unordered;
        const auto relation = graph.Compare(from, to);
        if (relation == Relation::Before && !forward)
            return Relation::After;
        return relation;
    }

    HappensBeforeGraph::Relation CompareAccesses(size_t previous, size_t current)
    {
        if (!TryConsumeBudget(limits_.CandidateComparisonBudget(), comparisons)) {
            launch.reasons.insert("comparison_budget");
            return HappensBeforeGraph::Relation::Unknown;
        }
        const auto relation = CompareNodes(previous, current);
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
        MemStateTracker vfMemory;
        bool vfEnabled;
        size_t stateBytes = 0;
        size_t stateSegments = 0;
        std::optional<size_t> vfOrderTarget;
        std::set<race::VfKey> orderedVfGroups;
        std::map<size_t, size_t> orderedChainPrefixes;

        void SetVfOrderTarget(size_t target)
        {
            if (vfOrderTarget != target) {
                vfOrderTarget = target;
                orderedVfGroups.clear();
                orderedChainPrefixes.clear();
            }
        }

        HappensBeforeGraph::Relation CompareCandidate(size_t previous, size_t current, bool pairwise)
        {
            using Relation = HappensBeforeGraph::Relation;
            if (!pairwise || !race::DynamicVfKey(records[current]) || race::DynamicVfKey(records[previous])) {
                return processor.CompareAccesses(previous, current);
            }
            const auto source = processor.accesses[previous].node;
            const auto chain = processor.graph.ChainOf(source);
            const auto rank = processor.graph.TraversalRank(source);
            if (!chain || !rank) {
                return processor.CompareAccesses(previous, current);
            }
            const auto prefix = orderedChainPrefixes.find(*chain);
            // Actual chain edges link every earlier member to the proven predecessor.
            if (prefix != orderedChainPrefixes.end() && *rank <= prefix->second) {
                return Relation::Before;
            }
            const auto relation = processor.CompareAccesses(previous, current);
            if (relation == Relation::Before) {
                if (prefix != orderedChainPrefixes.end()) {
                    prefix->second = *rank;
                } else if (orderedChainPrefixes.size() < processor.limits_.ReplayOrderCacheEntryBudget()) {
                    orderedChainPrefixes.emplace(*chain, *rank);
                }
            }
            return relation;
        }

        bool VfGroupPrecedes(const race::VfKey& key, size_t target)
        {
            if (orderedVfGroups.count(key) != 0)
                return true;
            const auto source = processor.dependency.vfBoundaries.at(key).completion;
            if (processor.comparisons >= processor.limits_.CandidateComparisonBudget())
                return false;
            ++processor.comparisons;
            const auto relation = processor.graph.Compare(source, target);
            processor.checker.graphQueryWorkPeak_ =
                std::max(processor.checker.graphQueryWorkPeak_, processor.graph.QueryWork());
            if (relation != HappensBeforeGraph::Relation::Before)
                return false;
            if (orderedVfGroups.size() < processor.limits_.ReplayOrderCacheEntryBudget())
                orderedVfGroups.insert(key);
            return true;
        }

        MemoryReplay(
            LaunchProcessor& owner, const std::vector<AclsanDeviceMemoryAccessData>& input, MemStateTracker& ordinary,
            bool enableVf)
            : processor(owner),
              records(input),
              memory(ordinary),
              vfMemory(input, racecheck::MemStateTrackerMode::VfExternal),
              vfEnabled(enableVf)
        {}

        struct ConflictBatch {
            std::vector<AccessPair> confirmed;
            std::vector<NpuCheckRacecheckReport> reports;
            std::set<std::pair<uint64_t, uint64_t>> ranges;
            std::set<std::tuple<CbIndex, CbIndex, uint64_t, uint64_t>> pairs;
        };

        using VfSummary = std::map<size_t, racecheck::PreparedAccess>;
        using VfSummaries = std::map<size_t, VfSummary>;

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
            size_t groupBytes = 0;
            for (const auto& writers : state.atomicWriters)
                history += writers.capacity();
            if (state.vfExternal) {
                const auto& external = *state.vfExternal;
                groupBytes += sizeof(VfExternalState) + SEGMENT_NODE_ALLOWANCE;
                history += external.readers.capacity() + external.writers.capacity();
                for (const auto& writers : external.atomicWriters)
                    history += writers.capacity();
                for (const auto& group : external.groups) {
                    groupBytes += sizeof(decltype(external.groups)::value_type) + SEGMENT_NODE_ALLOWANCE;
                    groupBytes += group.second.atomicWriters.capacity() *
                                  sizeof(decltype(group.second.atomicWriters)::value_type);
                }
            }
            return sizeof(MemState) + SEGMENT_NODE_ALLOWANCE + groupBytes + history * sizeof(CbIndex);
        }

        // Account only touched intervals and their merge neighbours. This avoids a
        // full-table scan on every access while retaining history-capacity costs.
        std::pair<size_t, size_t> MeasureUsage(
            const MemStateTracker& tracker, const PhysicalSpaceKey& key, const std::vector<Interval>& windows) const
        {
            std::pair<size_t, size_t> result{};
            const auto table = tracker.Tables().find(key);
            if (table == tracker.Tables().end())
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

        void PublishConflicts(ConflictBatch& batch)
        {
            static_assert(std::is_nothrow_move_constructible_v<CheckerReport>);
            static_assert(std::is_nothrow_constructible_v<CheckerReport, NpuCheckRacecheckReport&&>);
            if (batch.confirmed.empty())
                return;
            auto& output = processor.reports;
            for (auto& evidence : batch.reports) {
                evidence.reportId = processor.checker.raw_ + processor.checker.war_ + processor.checker.waw_ +
                                    processor.checker.atomic_ + 1;
                const auto kind = evidence.kind;
                output.emplace_back(std::move(evidence));
                if (kind == ConflictKind::RAW) {
                    ++processor.checker.raw_;
                } else if (kind == ConflictKind::WAR) {
                    ++processor.checker.war_;
                } else if (kind == ConflictKind::WAW) {
                    ++processor.checker.waw_;
                } else {
                    ++processor.checker.atomic_;
                }
            }
        }

        bool CheckAccess(
            MemStateTracker& tracker, size_t current, const racecheck::PreparedAccess& prepared, ConflictBatch& batch,
            bool pairwise, const MemStateTracker::VfGroupPrecedes* vfGroupPrecedes = nullptr)
        {
            const auto candidates = vfGroupPrecedes != nullptr ?
                                        tracker.CheckPotentialRace(current, prepared, *vfGroupPrecedes) :
                                        tracker.CheckPotentialRace(current, prepared);
            std::optional<racecheck::PreparedAccess> reportAccess;
            for (const auto& candidate : candidates) {
                const auto relation = CompareCandidate(candidate.prevIndex, current, pairwise);
                if (relation == HappensBeforeGraph::Relation::Unordered) {
                    batch.confirmed.push_back(candidate);
                    const bool newReport =
                        pairwise ?
                            batch.pairs.emplace(candidate.prevIndex, candidate.curIndex, candidate.begin, candidate.end)
                                .second :
                            batch.ranges.emplace(candidate.begin, candidate.end).second;
                    if (newReport) {
                        if (race::DynamicVfKey(records[current]) && !reportAccess)
                            reportAccess.emplace(PrepareAccess(current));
                        auto evidence = MakeRacecheckDiagnostics(
                            {candidate}, records, current, reportAccess ? *reportAccess : prepared);
                        batch.reports.push_back(std::move(evidence.front()));
                    }
                    continue;
                }
                if (relation != HappensBeforeGraph::Relation::Before) {
                    return false;
                }
            }
            return true;
        }

        bool CheckVfAccess(size_t index, const racecheck::PreparedAccess& prepared, ConflictBatch& batch)
        {
            const auto key = race::DynamicVfKey(records[index]);
            const auto target =
                key.has_value() ? processor.dependency.vfBoundaries.at(*key).entry : processor.accesses[index].node;
            SetVfOrderTarget(target);
            const MemStateTracker::VfGroupPrecedes precedes = [this, target](const race::VfKey& source) {
                return VfGroupPrecedes(source, target);
            };
            return CheckAccess(vfMemory, index, prepared, batch, true, &precedes);
        }

        void ReserveConflictReports(size_t reportCount)
        {
            auto& output = processor.reports;
            if (reportCount > output.max_size() - output.size()) {
                throw RacecheckError(RacecheckErrorKind::CapacityExceeded, "race report capacity exceeded");
            }
            const size_t required = output.size() + reportCount;
            if (required > output.capacity()) {
                const size_t grown =
                    output.capacity() > output.max_size() / 2 ? output.max_size() : output.capacity() * 2;
                output.reserve(std::max(required, grown));
            }
        }

        bool CheckInstruction(
            const std::vector<size_t>& indices, const std::map<size_t, racecheck::PreparedAccess>* summary = nullptr)
        {
            std::vector<ConflictBatch> ordinaryBatches(indices.size());
            std::vector<ConflictBatch> vfBatches(indices.size());
            std::vector<racecheck::PreparedAccess> prepared;
            prepared.reserve(indices.size());
            for (const auto index : indices) {
                prepared.push_back(summary != nullptr ? summary->at(index) : PrepareAccess(index));
            }
            size_t reportCount = 0;
            for (size_t i = 0; i < indices.size(); ++i) {
                const auto index = indices[i];
                const bool vfAccess = race::DynamicVfKey(records[index]).has_value();
                const bool vfMemoryAccess = vfEnabled && records[index].memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB;
                if (!vfAccess && !CheckAccess(memory, index, prepared[i], ordinaryBatches[i], false)) {
                    return false;
                }
                if (vfMemoryAccess && !CheckVfAccess(index, prepared[i], vfBatches[i])) {
                    return false;
                }
                reportCount += ordinaryBatches[i].reports.size() + vfBatches[i].reports.size();
            }
            ReserveConflictReports(reportCount);
            std::vector<size_t> order(indices.size());
            for (size_t i = 0; i < order.size(); ++i) {
                order[i] = i;
            }
            std::stable_sort(order.begin(), order.end(), [this, &indices](size_t a, size_t b) {
                return NormalizeMemoryAccessMode(records[indices[a]].accessMode) == ACLSAN_DEVICE_MEMORY_ACCESS_READ &&
                       NormalizeMemoryAccessMode(records[indices[b]].accessMode) != ACLSAN_DEVICE_MEMORY_ACCESS_READ;
            });
            const auto savedBytes = stateBytes, savedSegments = stateSegments;
            for (auto i : order) {
                const auto index = indices[i];
                const bool vfAccess = race::DynamicVfKey(records[index]).has_value();
                const bool vfMemoryAccess = vfEnabled && records[index].memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB;
                if (!vfAccess && !UpdateAccess(memory, index, ordinaryBatches[i].confirmed, prepared[i])) {
                    stateBytes = savedBytes;
                    stateSegments = savedSegments;
                    return false;
                }
                if (vfMemoryAccess && !UpdateAccess(vfMemory, index, vfBatches[i].confirmed, prepared[i])) {
                    stateBytes = savedBytes;
                    stateSegments = savedSegments;
                    return false;
                }
            }
            for (auto& batch : ordinaryBatches) {
                PublishConflicts(batch);
            }
            for (auto& batch : vfBatches) {
                PublishConflicts(batch);
            }
            return true;
        }

        std::vector<Interval> AffectedWindows(
            const MemStateTracker& tracker, const racecheck::PreparedAccess& prepared) const
        {
            std::vector<Interval> windows;
            const auto table = tracker.Tables().find(prepared.key);
            for (const auto range : prepared.ranges) {
                Interval window{range.begin, range.end};
                if (table != tracker.Tables().end() && !table->second.empty()) {
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

        bool UpdateAccess(
            MemStateTracker& tracker, size_t current, const std::vector<AccessPair>& conflicts,
            const racecheck::PreparedAccess& prepared)
        {
            if (prepared.ranges.empty())
                return true;
            if (processor.limits_.ReplayStateByteBudget() < sizeof(MemState) + SEGMENT_NODE_ALLOWANCE) {
                return RejectStateBudget(current, "plan", sizeof(MemState) + SEGMENT_NODE_ALLOWANCE);
            }
            const auto windows = AffectedWindows(tracker, prepared);
            const auto before = MeasureUsage(tracker, prepared.key, windows);
            tracker.UpdateMemState(current, conflicts, prepared);
            const auto after = MeasureUsage(tracker, prepared.key, windows);
            stateSegments = stateSegments - before.first + after.first;
            stateBytes = stateBytes - before.second + after.second;
            return CheckStateBudget(current);
        }

        bool CheckStateBudget(size_t current)
        {
            processor.checker.stateBytesPeak_ = std::max(processor.checker.stateBytesPeak_, stateBytes);
            processor.checker.stateSegmentsPeak_ = std::max(processor.checker.stateSegmentsPeak_, stateSegments);
            if (stateSegments > processor.limits_.ReplayStateSegmentBudget() ||
                stateBytes > processor.limits_.ReplayStateByteBudget()) {
                return RejectStateBudget(current, "replay", stateBytes);
            }
            return true;
        }

        bool RejectStateBudget(size_t current, const char* phase, size_t requiredBytes)
        {
            processor.launch.reasons.insert("memory_state_budget");
            const auto& header = records[current].header;
            ASCTOOL_WARNING(
                "analysis budget: launch=%llu stage=memory_state phase=%s access_index=%zu total_accesses=%zu "
                "core=%u pc=0x%llx instruction=%llu state_segments=%zu segment_limit=%zu "
                "state_bytes=%zu byte_limit=%zu required_bytes=%zu",
                static_cast<unsigned long long>(header.launchId), phase, current, records.size(),
                header.executor.phyCoreId, static_cast<unsigned long long>(header.pc),
                static_cast<unsigned long long>(header.instrExecId), stateSegments,
                processor.limits_.ReplayStateSegmentBudget(), stateBytes, processor.limits_.ReplayStateByteBudget(),
                requiredBytes);
            return false;
        }

        using PendingVfAccesses = std::map<size_t, std::vector<size_t>>;
        static constexpr size_t PENDING_VF_NODE_BYTES = sizeof(PendingVfAccesses::value_type) + SEGMENT_NODE_ALLOWANCE;

        bool CollectVfAccesses(PendingVfAccesses& pending)
        {
            for (size_t index = 0; index < records.size(); ++index) {
                const auto key = race::DynamicVfKey(records[index]);
                if (!key)
                    continue;
                auto [group, inserted] = pending.try_emplace(processor.dependency.vfBoundaries.at(*key).entry);
                auto& indices = group->second;
                const auto oldCapacity = indices.capacity();
                indices.push_back(index);
                stateBytes +=
                    (inserted ? PENDING_VF_NODE_BYTES : 0) + (indices.capacity() - oldCapacity) * sizeof(size_t);
                if (!CheckStateBudget(index))
                    return false;
            }
            return true;
        }

        bool AppendVfSummary(
            VfSummaries& summaries, size_t entry, size_t index, const PhysicalSpaceKey& key, const MemState& segment)
        {
            auto [group, newGroup] = summaries.try_emplace(entry);
            auto [access, newAccess] = group->second.try_emplace(
                index, racecheck::PreparedAccess{key, racecheck::AccessMode(records[index]), {}});
            auto& ranges = access->second.ranges;
            const auto oldCapacity = ranges.capacity();
            if (!ranges.empty() && ranges.back().end == segment.begin) {
                ranges.back().end = segment.end;
            } else {
                // Summary ranges are vector elements, not live tracker nodes.
                // Their allocated capacity is charged to the byte budget below.
                ranges.push_back({segment.begin, segment.end});
            }
            stateBytes += (newGroup ? sizeof(VfSummaries::value_type) + SEGMENT_NODE_ALLOWANCE : 0) +
                          (newAccess ? sizeof(VfSummary::value_type) + SEGMENT_NODE_ALLOWANCE : 0) +
                          (ranges.capacity() - oldCapacity) * sizeof(racecheck::ByteRange);
            return CheckStateBudget(index);
        }

        bool BuildVfSummary(VfSummaries& summaries, size_t entry, const std::vector<size_t>& indices)
        {
            const auto baseBytes = stateBytes, baseSegments = stateSegments;
            MemStateTracker tracker(records, racecheck::MemStateTrackerMode::VfExternal);
            for (const auto index : indices) {
                if (!UpdateAccess(tracker, index, {}, PrepareAccess(index)))
                    return false;
            }
            const auto trackerBytes = stateBytes - baseBytes, trackerSegments = stateSegments - baseSegments;
            for (const auto& [key, table] : tracker.Tables()) {
                for (const auto& item : table) {
                    const auto& segment = item.second;
                    if (!segment.vfExternal || segment.vfExternal->groups.empty())
                        continue;
                    // Only this entry's VF was fed to the tracker.
                    const auto& history = segment.vfExternal->groups.begin()->second;
                    if (history.witness && !AppendVfSummary(summaries, entry, *history.witness, key, segment))
                        return false;
                }
            }
            stateBytes -= trackerBytes;
            stateSegments -= trackerSegments;
            return true;
        }

        bool BuildVfSummaries(VfSummaries& summaries)
        {
            // External ordering is shared within a dynamic VF, not across VFs.
            // Isolate interval construction so unrelated VF boundaries cannot copy its history.
            PendingVfAccesses pending;
            if (!CollectVfAccesses(pending))
                return false;
            while (!pending.empty()) {
                const auto group = pending.begin();
                if (!BuildVfSummary(summaries, group->first, group->second))
                    return false;
                stateBytes -= PENDING_VF_NODE_BYTES + group->second.capacity() * sizeof(size_t);
                pending.erase(group);
            }
            return true;
        }

        void ReleaseVfSummary(const VfSummary& summary)
        {
            stateBytes -= sizeof(VfSummaries::value_type) + SEGMENT_NODE_ALLOWANCE;
            for (const auto& access : summary) {
                stateBytes -= sizeof(VfSummary::value_type) + SEGMENT_NODE_ALLOWANCE +
                              access.second.ranges.capacity() * sizeof(racecheck::ByteRange);
            }
        }
    };

    bool ReplayMemory()
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
            if (!race::DynamicVfKey(records.back()))
                instructions[accesses[i].node].push_back(i);
        }
        const bool hasVf = std::any_of(records.begin(), records.end(), [](const auto& record) {
            return record.vfInstrValid != 0 && record.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB;
        });
        MemoryReplay replay{*this, records, history->tracker, hasVf};
        MemoryReplay::VfSummaries summaries;
        if (hasVf && !replay.BuildVfSummaries(summaries))
            return false;
        for (auto node : graph.Order()) {
            auto summary = summaries.find(node);
            if (summary != summaries.end()) {
                std::vector<size_t> indices;
                indices.reserve(summary->second.size());
                for (const auto& entry : summary->second)
                    indices.push_back(entry.first);
                if (!replay.CheckInstruction(indices, &summary->second))
                    return false;
                replay.ReleaseVfSummary(summary->second);
                summaries.erase(summary);
                continue;
            }
            auto instruction = instructions.find(node);
            if (instruction == instructions.end())
                continue;
            if (!replay.CheckInstruction(instruction->second))
                return false;
        }
        return true;
    }
};

void Racechecker::Analyze([[maybe_unused]] uint64_t id, Launch& launch, CheckerReportList& reports)
{
    race::AnalysisPhaseTimer total(id, "racecheck_launch");
    LaunchProcessor processor(*this, launch, reports);
    processor.Run();
    if (analysis_->Managed() && launch.reasons.empty()) {
        race::AnalysisPhaseTimer phase(id, "cross_launch_history");
        PublishConflicts(analysis_->CrossLaunchConflicts(id, limits_, launch.reasons), reports);
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
bool Racechecker::AnalysisComplete() const { return !incomplete_ && launches_.empty() && analysis_->Complete(); }
std::string Racechecker::Summary() const
{
    std::ostringstream s;
    s << "tool=racecheck launches=" << launchesCount_ << " memory_events=" << memoryEvents_
      << " sync_events=" << syncEvents_ << " host_operations=" << hostEvents_ << " raw=" << raw_
      << " skipped_memory_events=" << skippedMemoryEvents_ << " skipped_sync_events=" << skippedSyncEvents_
      << " unsupported_ub_events=" << unsupportedUbEvents_ << " war=" << war_ << " waw=" << waw_
      << " atomic_semantic=" << atomic_ << " graph_nodes_peak=" << graphNodesPeak_
      << " graph_edges_peak=" << graphEdgesPeak_ << " graph_query_work_peak=" << graphQueryWorkPeak_
      << " state_accounted_bytes_peak=" << stateBytesPeak_ << " state_segments_peak=" << stateSegmentsPeak_
      << " pending_launches=" << launches_.size() << " analysis_complete=" << (AnalysisComplete() ? "true" : "false")
      << " warnings=" << warnings_ << " errors=" << (raw_ + war_ + waw_ + atomic_);
    for (const auto& reason : reasons_)
        s << " " << reason.first << "=" << reason.second;
    return s.str();
}
} // namespace npucheck
