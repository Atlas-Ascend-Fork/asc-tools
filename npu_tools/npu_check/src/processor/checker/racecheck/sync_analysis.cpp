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
constexpr uint32_t DAV3510_BLOCKS_PER_DIE = 54U;
constexpr uint32_t DAV3510_PHYSICAL_BLOCK_LIMIT = 2U * DAV3510_BLOCKS_PER_DIE;
constexpr uint32_t DAV3510_CUBE_GROUPS_PER_DIE = 18U;
constexpr uint32_t DAV3510_VECTOR_SUBBLOCKS_PER_GROUP = 2U;
constexpr uint32_t CROSS_CORE_MODE_SAME_PIPE = 0U;
constexpr uint32_t CROSS_CORE_MODE_VECTOR = 1U;
constexpr uint32_t CROSS_CORE_MODE_CROSS_TYPE = 2U;
constexpr uint32_t CROSS_CORE_MODE_RECEIVER_FLAG = 4U;
constexpr uint32_t RECEIVER_FLAG_SELECTOR_SHIFT = 4U;
constexpr uint32_t RECEIVER_FLAG_SELECTOR_MASK = 1U;
bool IsSupportedMemorySpace(AclsanDeviceMemorySpace space)
{
    switch (space) {
        case ACLSAN_DEVICE_MEMORY_SPACE_GM:
        case ACLSAN_DEVICE_MEMORY_SPACE_UB:
        case ACLSAN_DEVICE_MEMORY_SPACE_L1:
        case ACLSAN_DEVICE_MEMORY_SPACE_L0A:
        case ACLSAN_DEVICE_MEMORY_SPACE_L0B:
        case ACLSAN_DEVICE_MEMORY_SPACE_L0C:
        case ACLSAN_DEVICE_MEMORY_SPACE_BT:
        case ACLSAN_DEVICE_MEMORY_SPACE_FB:
            return true;
        default:
            return false;
    }
}
bool SameHeader(const AclsanDeviceEventHeader& a, const AclsanDeviceEventHeader& b)
{
    return Key(a) == Key(b) && a.launchId == b.launchId && a.pc == b.pc && a.siteId == b.siteId &&
           a.pipeline == b.pipeline && a.flags == b.flags && a.serialNo == b.serialNo && a.sourceKind == b.sourceKind;
}
bool SameMemory(const AclsanDeviceMemoryAccessData& a, const AclsanDeviceMemoryAccessData& b)
{
    if (!SameHeader(a.header, b.header) || a.address != b.address || a.accessMode != b.accessMode ||
        a.memorySpace != b.memorySpace || a.accessCount != b.accessCount || a.instructionId != b.instructionId ||
        a.unitFlag != b.unitFlag || a.layoutKind != b.layoutKind || a.memoryFlags != b.memoryFlags ||
        a.dataBits != b.dataBits || a.alignSize != b.alignSize || a.vectorMask0 != b.vectorMask0 ||
        a.vectorMask1 != b.vectorMask1 || a.predicateMask0 != b.predicateMask0 ||
        a.predicateMask1 != b.predicateMask1 || a.regDependencyMask0 != b.regDependencyMask0 ||
        a.regDependencyMask1 != b.regDependencyMask1)
        return false;
    switch (a.layoutKind) {
        case ACLSAN_MEM_LAYOUT_SCALAR:
            return a.layout.scalar.bytes == b.layout.scalar.bytes;
        case ACLSAN_MEM_LAYOUT_RANGE:
            return a.layout.range.bytes == b.layout.range.bytes;
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT:
            return std::tie(
                       a.layout.blockRepeat.blockNum, a.layout.blockRepeat.blockSize, a.layout.blockRepeat.blockStride,
                       a.layout.blockRepeat.repeatTimes, a.layout.blockRepeat.repeatStride) ==
                   std::tie(
                       b.layout.blockRepeat.blockNum, b.layout.blockRepeat.blockSize, b.layout.blockRepeat.blockStride,
                       b.layout.blockRepeat.repeatTimes, b.layout.blockRepeat.repeatStride);
        case ACLSAN_MEM_LAYOUT_ND_AFFINE:
            return a.layout.ndAffine.rank == b.layout.ndAffine.rank &&
                   a.layout.ndAffine.elementBytes == b.layout.ndAffine.elementBytes &&
                   std::equal(
                       std::begin(a.layout.ndAffine.dims), std::end(a.layout.ndAffine.dims), b.layout.ndAffine.dims) &&
                   std::equal(
                       std::begin(a.layout.ndAffine.strides), std::end(a.layout.ndAffine.strides),
                       b.layout.ndAffine.strides);
        default:
            return false;
    }
}
bool SameSync(const AclsanDeviceSyncData& a, const AclsanDeviceSyncData& b)
{
    return SameHeader(a.header, b.header) &&
           std::tie(a.syncKind, a.action, a.scope, a.srcPipe, a.dstPipe, a.mode, a.objectId) ==
               std::tie(b.syncKind, b.action, b.scope, b.srcPipe, b.dstPipe, b.mode, b.objectId);
}
// Physical topology of the supported dav-3510 target. Logical blockId is a
// launch scheduling identity and must not be used as a physical group index.
struct CorePosition {
    uint32_t die;
    uint32_t group;
    uint32_t subblock;
};
std::optional<CorePosition> Position3510(const Domain& domain)
{
    const auto type = std::get<1>(domain);
    const auto physical = std::get<3>(domain);
    const auto local = physical % DAV3510_BLOCKS_PER_DIE;
    if (physical >= DAV3510_PHYSICAL_BLOCK_LIMIT)
        return std::nullopt;
    if (type == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE && local < DAV3510_CUBE_GROUPS_PER_DIE)
        return CorePosition{physical / DAV3510_BLOCKS_PER_DIE, local, 0U};
    if (type == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR && local >= DAV3510_CUBE_GROUPS_PER_DIE)
        return CorePosition{
            physical / DAV3510_BLOCKS_PER_DIE,
            (local - DAV3510_CUBE_GROUPS_PER_DIE) / DAV3510_VECTOR_SUBBLOCKS_PER_GROUP,
            (local - DAV3510_CUBE_GROUPS_PER_DIE) % DAV3510_VECTOR_SUBBLOCKS_PER_GROUP};
    return std::nullopt;
}
// Whether this sender contributes to a receiver's hardware counter. Mode 4
// uses the receiver's flag: C flags 16..31 select V1; V flags are always 0..15.
bool CrossParticipant(uint32_t mode, const Domain& sender, const Domain& receiver, uint64_t receiverFlag)
{
    if (std::get<0>(sender) != std::get<0>(receiver))
        return false;
    const auto st = std::get<1>(sender), rt = std::get<1>(receiver);
    if (mode == CROSS_CORE_MODE_SAME_PIPE)
        return st == rt;
    const auto sp = Position3510(sender), rp = Position3510(receiver);
    if (!sp || !rp || sp->die != rp->die || sp->group != rp->group)
        return false;
    if (mode == CROSS_CORE_MODE_VECTOR)
        return st == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR && st == rt;
    if (mode == CROSS_CORE_MODE_CROSS_TYPE)
        return st != rt;
    if (mode == CROSS_CORE_MODE_RECEIVER_FLAG)
        return st != rt &&
               (rt == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR ||
                sp->subblock == ((receiverFlag >> RECEIVER_FLAG_SELECTOR_SHIFT) & RECEIVER_FLAG_SELECTOR_MASK));
    return false;
}
} // namespace

Racechecker::Racechecker(race::Limits limits) : limits_(limits) {}
const std::vector<CallbackSpec>& Racechecker::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> ids{
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
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
        auto& state = launches_[l.launchId];
        if (state.registered)
            return malformed();
        state.stream = l.stream;
        state.registered = true;
        if (l.functionName)
            state.name = l.functionName;
        return true;
    }
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    const size_t size = cbid == ACLSAN_CBID_DEVICE_SYNC         ? sizeof(AclsanDeviceSyncData) :
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
    auto& state = launches_[h.launchId];
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
    if (state.memory.size() + state.sync.size() >= limits_.events) {
        state.reasons.insert("event_budget");
        return true;
    }
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
    struct Access {
        const AclsanDeviceMemoryAccessData& data;
        size_t node;
        std::vector<Interval> ranges;
    };
    struct Event {
        bool sync;
        size_t index;
        size_t node;
        uint32_t pipe;
    };
    // FFTS (modes 0/1/2) and intra-block (mode 4) are separate counter banks.
    // Each key identifies one receiver, not a launch-wide flag broadcast.
    using CrossFlag = std::tuple<Domain, bool, uint64_t>;
    struct Notification {
        size_t node;
        std::vector<size_t> contributors;
    };
    struct NotificationStream {
        std::map<Domain, std::vector<size_t>> senders;
        std::vector<Notification> tokens;
        size_t consumed = 0;
    };
    struct CrossCounter {
        std::map<uint32_t, NotificationStream> modes;
        std::vector<size_t> waits;
        size_t consumed = 0;
    };
    std::map<CrossFlag, CrossCounter> crossCounters;
    // GET/RLS BUF is the device-level representation of a Mutex. A mutex is
    // shared by all supported pipes in one execution domain. Only one GET may
    // be active for a mutex, while GET/RLS itself must pair on the same pipe,
    // buffer id and mode.
    using MutexKey = std::pair<Domain, uint64_t>;
    struct MutexGet {
        size_t node;
        uint32_t pipe;
        uint32_t mode;
    };
    struct MutexState {
        std::map<uint32_t, size_t> releases;
        std::optional<MutexGet> get;
    };
    std::map<MutexKey, MutexState> mutexes;
    using Pipe = std::pair<Domain, uint32_t>;
    Racechecker& checker;
    Launch& launch;
    CheckerReportList& reports;
    const Limits& limits_;
    HappensBeforeGraph graph;
    std::map<Instruction, Event> events;
    std::vector<Access> accesses;
    std::map<Instruction, size_t> instructionStarts;
    // Prefix/acquire joins do not imply completion order within a pipeline.
    std::map<Pipe, std::optional<size_t>> prefix, acquired;
    // Only S/V/M complete in issue order within one core.
    std::map<Pipe, size_t> lastOrdered;
    bool graphExhausted = false;
    size_t joinNodes = 0;
    size_t comparisons = 0;

public:
    LaunchProcessor(Racechecker& owner, Launch& data, CheckerReportList& output)
        : checker(owner),
          launch(data),
          reports(output),
          limits_(owner.limits_),
          graph(limits_.edges, limits_.queryWork, limits_.queryCache)
    {}

    void Run()
    {
        NormalizeEvents();
        ExpandUnitFlagAccesses();
        RouteCrossCoreNotifications();
        BuildNotificationTokens();
        BuildLocalEdges();
        ResolveCrossCoreWaits();
        if (SealGraph())
            ReplayMemory();
    }

private:
    void NormalizeEvents()
    {
        // Stable identities, independent of callback interleaving, determine tie breaks only.
        std::sort(launch.memory.begin(), launch.memory.end(), [](const auto& a, const auto& b) {
            return std::make_pair(Key(a.header), a.accessIndex) < std::make_pair(Key(b.header), b.accessIndex);
        });
        std::sort(launch.sync.begin(), launch.sync.end(), [](const auto& a, const auto& b) {
            return Key(a.header) < Key(b.header);
        });
        std::map<std::pair<Instruction, uint32_t>, size_t> accessKeys;
        std::map<Instruction, std::pair<uint32_t, std::set<uint32_t>>> counts;
        size_t segments = 0;
        for (const auto& a : launch.memory) {
            const auto key = Key(a.header);
            auto old = accessKeys.find({key, a.accessIndex});
            if (old != accessKeys.end()) {
                if (!SameMemory(accesses[old->second].data, a))
                    launch.reasons.insert("conflicting_duplicate");
                continue;
            }
            if (!a.accessCount || a.accessIndex >= a.accessCount || !ValidPipe(a.header.pipeline) ||
                a.header.blockType > ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE) {
                launch.reasons.insert("invalid_access_identity");
                continue;
            }
            auto& count = counts[key];
            if (count.first != 0 && count.first != a.accessCount)
                launch.reasons.insert("access_count_mismatch");
            count.first = a.accessCount;
            count.second.insert(a.accessIndex);
            auto event = events.find(key);
            size_t node;
            if (event == events.end()) {
                node = graph.AddNode();
                events.emplace(key, Event{false, accesses.size(), node, a.header.pipeline});
            } else {
                node = event->second.node;
                const auto& first = accesses[event->second.index].data;
                if (event->second.pipe != a.header.pipeline || first.header.pc != a.header.pc ||
                    first.header.siteId != a.header.siteId || first.instructionId != a.instructionId ||
                    first.unitFlag != a.unitFlag)
                    launch.reasons.insert("instruction_identity_mismatch");
            }
            Access access{a, node, {}};
            if (a.instructionId >= static_cast<uint32_t>(aclsan::InstructionId::StAtomicB32) &&
                a.instructionId <= static_cast<uint32_t>(aclsan::InstructionId::StiAtomicB8)) {
                launch.reasons.insert("atomic_semantics_unknown");
            }
            // Remote/shared owner mappings are not encoded by the current callback.
            if (a.header.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR &&
                a.memorySpace >= ACLSAN_DEVICE_MEMORY_SPACE_L1) {
                launch.reasons.insert("owner_mapping_unknown");
            }
            if (a.accessMode < ACLSAN_DEVICE_MEMORY_ACCESS_READ ||
                a.accessMode > ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX ||
                a.memorySpace > ACLSAN_DEVICE_MEMORY_SPACE_FB ||
                !IsSupportedMemorySpace(static_cast<AclsanDeviceMemorySpace>(a.memorySpace))) {
                launch.reasons.insert("access_coverage_unknown");
            } else if (!Normalize(a, limits_.segments - segments, access.ranges)) {
                launch.reasons.insert("invalid_or_excessive_layout");
            }
            segments += access.ranges.size();
            accessKeys[{key, a.accessIndex}] = accesses.size();
            accesses.push_back(std::move(access));
        }
        for (const auto& c : counts) {
            auto observed = c.second.second;
            const auto skipped = launch.skippedFragments.find(c.first);
            if (skipped != launch.skippedFragments.end())
                for (auto index : skipped->second)
                    if (index < c.second.first)
                        observed.insert(index);
            if (c.second.first != observed.size())
                launch.reasons.insert("missing_access_fragment");
        }
        for (size_t i = 0; i < launch.sync.size(); ++i) {
            const auto& s = launch.sync[i];
            auto found = events.find(Key(s.header));
            if (found != events.end()) {
                if (!found->second.sync || !SameSync(launch.sync[found->second.index], s))
                    launch.reasons.insert("conflicting_duplicate");
            } else
                events.emplace(Key(s.header), Event{true, i, graph.AddNode(), s.header.pipeline});
        }
    }

    void AddEdge(size_t a, size_t b, HappensBeforeGraph::Reason reason)
    {
        if (!graph.AddEdge(a, b, reason)) {
            launch.reasons.insert("graph_budget");
            graphExhausted = true;
        }
    }

    // A join collects both contributors with two incoming edges. It is not
    // a completion-order frontier for later accesses on the same pipeline.
    void MergePrefix(std::optional<size_t>& aggregate, size_t node, HappensBeforeGraph::Reason reason)
    {
        if (!aggregate) {
            aggregate = node;
            return;
        }
        if (*aggregate == node || graphExhausted)
            return;
        // Each join needs two edges. Bound extra nodes as well as edges so a
        // failed edge insertion cannot leave an unbounded synthetic-node list.
        if (joinNodes >= limits_.edges) {
            launch.reasons.insert("graph_budget");
            graphExhausted = true;
            return;
        }
        const size_t joined = graph.AddNode();
        ++joinNodes;
        AddEdge(*aggregate, joined, reason);
        if (!graphExhausted)
            AddEdge(node, joined, reason);
        aggregate = joined;
    }

    static constexpr uint64_t unitBytes = 512;
    struct UnitAccess {
        size_t node;
        uint32_t mode;
    };
    struct UnitQueues {
        std::vector<UnitAccess> writes;
        std::vector<UnitAccess> reads;
    };
    struct UnitPartition {
        std::map<std::pair<Domain, uint64_t>, UnitQueues> units;
        std::map<Domain, std::set<uint64_t>> boundaries;
        std::map<std::pair<Instruction, uint64_t>, size_t> nodes;
        std::vector<Access> expanded;
    };

    void CollectUnitBoundaries(UnitPartition& partition)
    {
        // Adjacent hardware units with identical access membership have identical
        // protocol histories. Partition at every observed unit boundary, retaining
        // byte-precise footprints while avoiding one node per 512 bytes of a tile.
        for (const auto& access : accesses) {
            if (access.data.unitFlag == 0 || access.data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_L0C)
                continue;
            auto& boundaries = partition.boundaries[DomainOf(access.data.header)];
            for (const auto& range : access.ranges) {
                if (range.begin < range.end) {
                    boundaries.insert(range.begin / unitBytes);
                    boundaries.insert((range.end - 1) / unitBytes + 1);
                }
            }
        }
    }

    void AddUnitFragment(
        UnitPartition& partition, const Access& access, size_t start, const std::vector<Interval>& ranges, bool unit)
    {
        const auto& a = access.data;
        const auto key = Key(a.header);
        if (partition.expanded.size() >= limits_.segments || graphExhausted) {
            launch.reasons.insert("unit_flag_budget");
            return;
        }
        const auto unitKey = std::make_pair(key, ranges.empty() ? 0 : ranges.front().begin / unitBytes);
        const auto existing = unit ? partition.nodes.find(unitKey) : partition.nodes.end();
        const bool fresh = existing == partition.nodes.end();
        const size_t node = fresh ? graph.AddNode() : existing->second;
        if (unit && fresh)
            partition.nodes.emplace(unitKey, node);
        if (fresh) {
            AddEdge(start, node, HappensBeforeGraph::Reason::InstructionBoundary);
            AddEdge(node, access.node, HappensBeforeGraph::Reason::InstructionBoundary);
        }
        partition.expanded.push_back(Access{a, node, ranges});
        if (unit && fresh) {
            auto& q = partition.units[{DomainOf(a.header), ranges.front().begin / unitBytes}];
            auto& queue = a.header.pipeline == ACLSAN_DEVICE_PIPE_MATRIX ? q.writes : q.reads;
            queue.push_back({node, a.unitFlag});
        }
    }

    void PartitionUnitRange(UnitPartition& partition, const Access& access, size_t start, const Interval& range)
    {
        const auto& a = access.data;
        for (uint64_t begin = range.begin; begin < range.end;) {
            const auto& boundaries = partition.boundaries.at(DomainOf(a.header));
            const auto next = boundaries.upper_bound(begin / unitBytes);
            const uint64_t end = next == boundaries.end() || *next > UINT64_MAX / unitBytes ?
                                     range.end :
                                     std::min(range.end, *next * unitBytes);
            const uint64_t length = end - begin;
            AddUnitFragment(partition, access, start, {Interval{begin, begin + length}}, true);
            begin += length;
            if (graphExhausted || partition.expanded.size() >= limits_.segments) {
                if (begin < range.end)
                    launch.reasons.insert("unit_flag_budget");
                break;
            }
        }
    }

    void PartitionUnitAccesses(UnitPartition& partition)
    {
        for (const auto& access : accesses) {
            const auto& a = access.data;
            if (a.unitFlag == 0) {
                partition.expanded.push_back(access);
                continue;
            }
            if ((a.unitFlag != UNIT_FLAG_MODE_START && a.unitFlag != UNIT_FLAG_MODE_END) ||
                a.header.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE ||
                (a.header.pipeline != ACLSAN_DEVICE_PIPE_MATRIX && a.header.pipeline != ACLSAN_DEVICE_PIPE_FIXPIPE)) {
                launch.reasons.insert("invalid_unit_flag");
                partition.expanded.push_back(access);
                continue;
            }
            const auto key = Key(a.header);
            auto start = instructionStarts.find(key);
            if (start == instructionStarts.end())
                start = instructionStarts.emplace(key, graph.AddNode()).first;
            if (a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_L0C) {
                AddUnitFragment(partition, access, start->second, access.ranges, false);
                continue;
            }
            if (a.header.pipeline == ACLSAN_DEVICE_PIPE_FIXPIPE && a.accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
                launch.reasons.insert("invalid_unit_flag_access");
                continue;
            }
            for (const auto& range : access.ranges) {
                PartitionUnitRange(partition, access, start->second, range);
            }
        }
    }

    void MatchUnitQueue(const UnitQueues& q)
    {
        size_t w = 0, r = 0;
        std::optional<size_t> released;
        while (w < q.writes.size()) {
            std::optional<size_t> published;
            while (w < q.writes.size()) {
                const auto current = q.writes[w++];
                if (released)
                    AddEdge(*released, current.node, HappensBeforeGraph::Reason::UnitFlag);
                if (current.mode == UNIT_FLAG_MODE_END) {
                    published = current.node;
                    break;
                }
            }
            if (!published) {
                if (r < q.reads.size())
                    launch.reasons.insert("unit_flag_missing_publish");
                break;
            }
            released.reset();
            std::optional<size_t> previousRead;
            while (r < q.reads.size()) {
                const auto current = q.reads[r++];
                AddEdge(*published, current.node, HappensBeforeGraph::Reason::UnitFlag);
                // Hardware completes the preceding L0C read/flag action before
                // the next FIX instruction's unit action. This does not order GM writes.
                if (previousRead)
                    AddEdge(*previousRead, current.node, HappensBeforeGraph::Reason::UnitFlag);
                previousRead = current.node;
                if (current.mode == UNIT_FLAG_MODE_END) {
                    released = current.node;
                    break;
                }
            }
            if (!released && w < q.writes.size()) {
                launch.reasons.insert("unit_flag_missing_release");
                break;
            }
        }
        if (r < q.reads.size())
            launch.reasons.insert("unit_flag_missing_publish");
    }

    void MatchUnitFlags(const UnitPartition& partition)
    {
        // Match generations independently in each physical L0C unit. Callback
        // interleaving across M and FIX is not ordering evidence. Mode 2 holds
        // ownership; only mode 3 hands the unit to the opposite pipeline.
        for (const auto& item : partition.units)
            MatchUnitQueue(item.second);
    }

    void ExpandUnitFlagAccesses()
    {
        // UnitFlag synchronizes L0C fragments, never whole FIX/M pipelines.
        // Keep scratch state local to this stage; instruction boundaries remain in the graph.
        UnitPartition partition;
        CollectUnitBoundaries(partition);
        PartitionUnitAccesses(partition);
        accesses = std::move(partition.expanded);
        MatchUnitFlags(partition);
    }

    void RecordNotification(
        const AclsanDeviceSyncData& s, const Domain& receiver, uint64_t receiverFlag, size_t node,
        const std::set<Domain>& domains)
    {
        auto& counter = crossCounters[{receiver, s.mode == CROSS_CORE_MODE_RECEIVER_FLAG, receiverFlag}];
        auto [streamIt, inserted] = counter.modes.try_emplace(s.mode);
        auto& stream = streamIt->second;
        if (inserted) {
            // A notification requires every active participant in this
            // mode's scope. A missing SET must not shrink that scope.
            for (const auto& peer : domains)
                if (CrossParticipant(s.mode, peer, receiver, receiverFlag))
                    stream.senders[peer];
        }
        stream.senders[DomainOf(s.header)].push_back(node);
    }

    void RouteCrossCoreNotifications()
    {
        // Route notifications to actual receiver counters before adding HB edges.
        // Callback arrival order is irrelevant; each sender/receiver is traversed
        // in its own instruction order. Reused flags consume one wave per WAIT.
        std::set<Domain> domains;
        for (const auto& pair : events)
            domains.insert(std::get<0>(pair.first));
        size_t routedSets = 0;
        for (const auto& pair : events) {
            const auto& e = pair.second;
            if (!e.sync)
                continue;
            const auto& s = launch.sync[e.index];
            if (s.syncKind != ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG || s.scope == ACLSAN_DEVICE_SYNC_SCOPE_BLOCK)
                continue;
            const auto sender = DomainOf(s.header);
            const bool intra = s.mode == CROSS_CORE_MODE_RECEIVER_FLAG;
            const bool cube = s.header.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
            const auto flag = s.objectId & (intra && cube ? 31U : 15U);
            if (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_CORE ||
                (s.mode != CROSS_CORE_MODE_SAME_PIPE && s.mode != CROSS_CORE_MODE_VECTOR &&
                 s.mode != CROSS_CORE_MODE_CROSS_TYPE && s.mode != CROSS_CORE_MODE_RECEIVER_FLAG) ||
                (s.mode != CROSS_CORE_MODE_SAME_PIPE && !Position3510(sender))) {
                launch.reasons.insert("unsupported_cross_core_topology_or_mode");
                continue;
            }
            if (s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
                crossCounters[{sender, intra, flag}].waits.push_back(e.node);
                continue;
            }
            if (s.action != ACLSAN_DEVICE_SYNC_ACTION_SET)
                continue;
            bool delivered = false;
            for (const auto& receiver : domains) {
                uint64_t receiverFlag = flag;
                if (intra) {
                    const auto sp = Position3510(sender), rp = Position3510(receiver);
                    if (!sp || !rp)
                        continue;
                    if (cube && rp->subblock != (flag >> 4U))
                        continue;
                    receiverFlag = cube ? flag & 15U : flag + 16U * sp->subblock;
                }
                if (!CrossParticipant(s.mode, sender, receiver, receiverFlag))
                    continue;
                delivered = true;
                if (++routedSets > limits_.edges) {
                    launch.reasons.insert("graph_budget");
                    break;
                }
                RecordNotification(s, receiver, receiverFlag, e.node, domains);
            }
            if (!delivered)
                launch.reasons.insert("pending_cross_core_set");
            if (routedSets > limits_.edges)
                break;
        }
    }

    void BuildStreamTokens(NotificationStream& stream)
    {
        size_t complete = std::numeric_limits<size_t>::max();
        for (const auto& sender : stream.senders)
            complete = std::min(complete, sender.second.size());
        if (stream.senders.empty())
            return;
        for (size_t i = 0; i < complete; ++i) {
            std::optional<size_t> joined;
            std::vector<size_t> contributors;
            for (const auto& sender : stream.senders) {
                const auto node = sender.second[i];
                contributors.push_back(node);
                MergePrefix(joined, node, HappensBeforeGraph::Reason::TokenMatch);
            }
            // Per-core scalar SET order makes successive complete
            // notifications in one participant stream ordered as well.
            if (!stream.tokens.empty())
                AddEdge(stream.tokens.back().node, *joined, HappensBeforeGraph::Reason::TokenMatch);
            stream.tokens.push_back({*joined, std::move(contributors)});
        }
    }

    void BuildNotificationTokens()
    {
        for (auto& entry : crossCounters)
            for (auto& mode : entry.second.modes)
                BuildStreamTokens(mode.second);
    }

    void BuildLocalEdges()
    {
        using Flag = std::tuple<Domain, uint32_t, uint32_t, uint32_t, uint64_t>;
        std::map<Flag, size_t> flags;
        std::set<Flag> invalidFlags;
        for (const auto& pair : events) {
            if (graphExhausted)
                break;
            const Domain domain = std::get<0>(pair.first);
            const auto& e = pair.second;
            if (!e.sync) {
                const size_t begin =
                    instructionStarts.count(pair.first) != 0 ? instructionStarts.at(pair.first) : e.node;
                if (e.pipe == ACLSAN_DEVICE_PIPE_VECTOR || e.pipe == ACLSAN_DEVICE_PIPE_SCALAR ||
                    e.pipe == ACLSAN_DEVICE_PIPE_MATRIX) {
                    const auto previous = lastOrdered.find({domain, e.pipe});
                    if (previous != lastOrdered.end())
                        AddEdge(previous->second, begin, HappensBeforeGraph::Reason::PipelineOrder);
                    lastOrdered[{domain, e.pipe}] = e.node;
                }
                if (const auto token = acquired[{domain, e.pipe}])
                    AddEdge(*token, begin, HappensBeforeGraph::Reason::AcquireSuffix);
                MergePrefix(prefix[{domain, e.pipe}], e.node, HappensBeforeGraph::Reason::PublishPrefix);
                continue;
            }
            const auto& s = launch.sync[e.index];
            if (s.syncKind == ACLSAN_DEVICE_SYNC_KIND_UNKNOWN && s.action == ACLSAN_DEVICE_SYNC_ACTION_UNKNOWN) {
                if (s.dstPipe == ACLSAN_DEVICE_PIPE_ALL) {
                    const uint32_t pipes[] = {ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_VECTOR,
                                              ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_PIPE_MTE1,
                                              ACLSAN_DEVICE_PIPE_MTE2,   ACLSAN_DEVICE_PIPE_MTE3,
                                              ACLSAN_DEVICE_PIPE_FIXPIPE};
                    for (const auto p : pipes) {
                        if (const auto prior = prefix[{domain, p}])
                            AddEdge(*prior, e.node, HappensBeforeGraph::Reason::BarrierPrefix);
                        acquired[{domain, p}] = e.node;
                        prefix[{domain, p}] = e.node;
                    }
                } else if (ValidPipe(s.dstPipe)) {
                    if (const auto prior = prefix[{domain, s.dstPipe}])
                        AddEdge(*prior, e.node, HappensBeforeGraph::Reason::BarrierPrefix);
                    acquired[{domain, s.dstPipe}] = e.node;
                    prefix[{domain, s.dstPipe}] = e.node;
                } else {
                    launch.reasons.insert("barrier_coverage_unknown");
                }
                continue;
            }
            if (s.syncKind == ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF) {
                BuildMutexEdges(domain, e, s);
                continue;
            }
            if (s.syncKind != ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG || !ValidPipe(s.srcPipe) || !ValidPipe(s.dstPipe) ||
                (s.scope == ACLSAN_DEVICE_SYNC_SCOPE_BLOCK && s.mode != CROSS_CORE_MODE_SAME_PIPE) ||
                (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_CORE && s.srcPipe == s.dstPipe) ||
                (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_BLOCK && s.mode > CROSS_CORE_MODE_RECEIVER_FLAG)) {
                launch.reasons.insert("sync_coverage_unknown");
                continue;
            }
            if (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_BLOCK) {
                // Cross-core SET/WAIT execute on scalar in core order, regardless
                // of their selected source/destination pipeline or flag ID. This
                // orders notifications, not all ordinary memory completions.
                auto scalar = std::make_pair(domain, uint32_t{ACLSAN_DEVICE_PIPE_SCALAR});
                if (const auto prior = prefix[scalar])
                    AddEdge(*prior, e.node, HappensBeforeGraph::Reason::PipelineOrder);
                lastOrdered[scalar] = e.node;
                MergePrefix(prefix[scalar], e.node, HappensBeforeGraph::Reason::PublishPrefix);
                if (s.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
                    if (s.srcPipe != ACLSAN_DEVICE_PIPE_SCALAR)
                        if (const auto prior = prefix[{domain, s.srcPipe}])
                            AddEdge(*prior, e.node, HappensBeforeGraph::Reason::PublishPrefix);
                } else if (s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
                    // Notification edges are resolved after local graph building.
                    if (ValidPipe(s.dstPipe)) {
                        MergePrefix(acquired[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::AcquireSuffix);
                        MergePrefix(prefix[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::PublishPrefix);
                    }
                } else if (s.action != ACLSAN_DEVICE_SYNC_ACTION_SET) {
                    launch.reasons.insert("invalid_cross_core_sync_action");
                }
                continue;
            }
            const Flag key{domain, s.scope, s.srcPipe, s.dstPipe, s.objectId};
            if (invalidFlags.count(key) != 0) {
                if (s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
                    flags.erase(key);
                    invalidFlags.erase(key);
                }
                continue;
            }
            if (s.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
                if (flags.count(key) != 0) {
                    invalidFlags.insert(key);
                    launch.reasons.insert("duplicate_set");
                    continue;
                }
                if (const auto prior = prefix[{domain, s.srcPipe}])
                    AddEdge(*prior, e.node, HappensBeforeGraph::Reason::PublishPrefix);
                flags[key] = e.node;
            } else if (s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
                auto f = flags.find(key);
                if (f == flags.end() || invalidFlags.count(key) != 0) {
                    launch.reasons.insert("unmatched_wait");
                    continue;
                }
                AddEdge(f->second, e.node, HappensBeforeGraph::Reason::TokenMatch);
                flags.erase(f);
                MergePrefix(acquired[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::AcquireSuffix);
                MergePrefix(prefix[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::PublishPrefix);
            } else
                launch.reasons.insert("invalid_sync_action");
        }
        if (!flags.empty())
            launch.reasons.insert("pending_set");
        for (const auto& entry : mutexes)
            if (entry.second.get)
                launch.reasons.insert("pending_get_buf");
    }

    void BuildMutexEdges(const Domain& domain, const Event& event, const AclsanDeviceSyncData& sync)
    {
        if (sync.scope != ACLSAN_DEVICE_SYNC_SCOPE_PIPE || !ValidPipe(sync.dstPipe) || sync.mode > 1U ||
            sync.objectId > 31U) {
            launch.reasons.insert("sync_coverage_unknown");
            return;
        }
        auto& state = mutexes[{domain, sync.objectId}];
        const uint32_t pipe = sync.dstPipe;
        if (sync.action == ACLSAN_DEVICE_SYNC_ACTION_GET) {
            if (state.get) {
                launch.reasons.insert("duplicate_get_buf");
                return;
            }
            // BLOCK mode waits for every pipe's preceding release. NON_BLOCK
            // deliberately contributes no acquire edge because the API does
            // not block the selected pipe in that mode.
            if (sync.mode == 0) {
                for (const auto& release : state.releases)
                    AddEdge(release.second, event.node, HappensBeforeGraph::Reason::TokenMatch);
            }
            if (sync.mode == 0)
                MergePrefix(acquired[{domain, pipe}], event.node, HappensBeforeGraph::Reason::AcquireSuffix);
            state.get = MutexGet{event.node, pipe, sync.mode};
            return;
        }
        if (sync.action != ACLSAN_DEVICE_SYNC_ACTION_RELEASE) {
            launch.reasons.insert("invalid_sync_action");
            return;
        }
        if (!state.get || state.get->pipe != pipe || state.get->mode != sync.mode) {
            launch.reasons.insert("unmatched_release_buf");
            return;
        }
        if (const auto prior = prefix[{domain, pipe}])
            AddEdge(*prior, event.node, HappensBeforeGraph::Reason::PublishPrefix);
        if (sync.mode == 1) {
            for (const auto& release : state.releases)
                if (release.first != pipe)
                    AddEdge(release.second, event.node, HappensBeforeGraph::Reason::TokenMatch);
        }
        AddEdge(state.get->node, event.node, HappensBeforeGraph::Reason::TokenMatch);
        state.get.reset();
        state.releases[pipe] = event.node;
        // The release waits for the pipe prefix (and, in NON_BLOCK mode,
        // preceding releases), so its suffix must observe that completion.
        acquired[{domain, pipe}] = event.node;
        prefix[{domain, pipe}] = event.node;
    }

    std::vector<NotificationStream*> PendingWaitCandidates(CrossCounter& counter, size_t wait)
    {
        std::vector<NotificationStream*> candidates;
        for (auto& mode : counter.modes) {
            auto& stream = mode.second;
            if (stream.consumed == stream.tokens.size())
                continue;
            bool future = false;
            for (auto source : stream.tokens[stream.consumed].contributors) {
                const auto path = graph.PrecedesDuringBuild(wait, source);
                if (!path) {
                    launch.reasons.insert("graph_query_budget");
                    break;
                }
                future |= *path;
            }
            if (!future)
                candidates.push_back(&stream);
        }
        return candidates;
    }

    bool NotificationPrecedes(const Notification& notification, size_t other)
    {
        for (auto source : notification.contributors) {
            const auto path = graph.PrecedesDuringBuild(source, other);
            if (!path)
                launch.reasons.insert("graph_query_budget");
            if (!path || !*path)
                return false;
        }
        return true;
    }

    NotificationStream* SelectFirstNotification(const std::vector<NotificationStream*>& candidates)
    {
        for (auto* candidate : candidates) {
            bool first = true;
            for (auto* other : candidates) {
                if (candidate == other)
                    continue;
                if (!NotificationPrecedes(
                        candidate->tokens[candidate->consumed], other->tokens[other->consumed].node)) {
                    first = false;
                    break;
                }
            }
            if (first)
                return candidate;
        }
        return nullptr;
    }

    bool ResolveCounterWait(CrossCounter& counter)
    {
        if (counter.consumed == counter.waits.size())
            return false;
        const auto wait = counter.waits[counter.consumed];
        const auto candidates = PendingWaitCandidates(counter, wait);
        auto* selected = SelectFirstNotification(candidates);
        if (selected && launch.reasons.empty()) {
            AddEdge(selected->tokens[selected->consumed++].node, wait, HappensBeforeGraph::Reason::TokenMatch);
            ++counter.consumed;
            return true;
        }
        return false;
    }

    void ResolveCrossCoreWaits()
    {
        // WAIT consumes one notification from its receiver counter, irrespective
        // of mode. Resolve only orders proven by SET scalar order or existing HB
        // paths. Iteration allows handshakes on another flag to establish order;
        // callback arrival and map iteration order are never matching evidence.
        bool progress = true;
        while (progress && launch.reasons.empty()) {
            progress = false;
            for (auto& entry : crossCounters)
                if (ResolveCounterWait(entry.second))
                    progress = true;
        }
        for (const auto& entry : crossCounters) {
            const auto& counter = entry.second;
            if (counter.consumed == counter.waits.size())
                continue; // Unconsumed SET notifications are allowed to accumulate.
            size_t remaining = 0;
            for (const auto& mode : counter.modes)
                remaining += mode.second.tokens.size() - mode.second.consumed;
            launch.reasons.insert(
                remaining < counter.waits.size() - counter.consumed ? "unmatched_cross_core_wait" :
                                                                      "ambiguous_cross_core_notification_order");
        }
    }

    bool SealGraph()
    {
        const bool graphComplete = graph.Seal(launch.reasons.empty());
        checker.graphNodesPeak_ = std::max(checker.graphNodesPeak_, graph.Order().size());
        checker.graphEdgesPeak_ = std::max(checker.graphEdgesPeak_, graph.EdgeCount());
        if (!graphComplete) {
            launch.reasons.insert("incomplete_sync_graph");
            return false;
        }

        return true;
    }

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
        std::vector<AclsanDeviceMemoryAccessData> records;
        records.reserve(accesses.size());
        std::map<size_t, std::vector<size_t>> instructions;
        for (size_t i = 0; i < accesses.size(); ++i) {
            records.push_back(accesses[i].data);
            instructions[accesses[i].node].push_back(i);
        }
        MemStateTracker memory(records);
        MemoryReplay replay{*this, records, memory};
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
}

bool Racechecker::HasErrors() const { return raw_ != 0 || war_ != 0 || waw_ != 0 || atomic_ != 0; }
bool Racechecker::AnalysisComplete() const { return !incomplete_ && launches_.empty() && launchesCount_ != 0; }
std::string Racechecker::Summary() const
{
    std::ostringstream s;
    s << "tool=racecheck launches=" << launchesCount_ << " memory_events=" << memoryEvents_
      << " sync_events=" << syncEvents_ << " raw=" << raw_ << " skipped_memory_events=" << skippedMemoryEvents_
      << " skipped_sync_events=" << skippedSyncEvents_ << " war=" << war_ << " waw=" << waw_
      << " atomic_semantic=" << atomic_ << " graph_nodes_peak=" << graphNodesPeak_
      << " graph_edges_peak=" << graphEdgesPeak_ << " graph_query_work_peak=" << graphQueryWorkPeak_
      << " state_accounted_bytes_peak=" << stateBytesPeak_ << " state_segments_peak=" << stateSegmentsPeak_
      << " pending_launches=" << launches_.size() << " analysis_complete=" << (AnalysisComplete() ? "true" : "false")
      << " errors=" << (raw_ + war_ + waw_ + atomic_) << " warnings=0";
    for (const auto& reason : reasons_)
        s << " " << reason.first << "=" << reason.second;
    return s.str();
}
} // namespace npucheck
