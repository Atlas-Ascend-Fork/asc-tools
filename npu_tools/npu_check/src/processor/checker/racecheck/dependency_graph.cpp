/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "checker/racecheck/dependency_graph.h"

#include "acl_san/aclsan_api.h"
#include "device_instr/common/instruction_id.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace npucheck {
namespace {
using namespace race;

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
struct CorePosition {
    uint32_t die;
    uint32_t group;
    uint32_t subblock;
};
std::optional<CorePosition> Position3510(const Domain& domain)
{
    const auto type = std::get<1>(domain);
    const auto physical = std::get<3>(domain);
    const auto local = physical % 54U;
    if (physical >= 108U)
        return std::nullopt;
    if (type == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE && local < 18U)
        return CorePosition{physical / 54U, local, 0U};
    if (type == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR && local >= 18U)
        return CorePosition{physical / 54U, (local - 18U) / 2U, (local - 18U) % 2U};
    return std::nullopt;
}
bool CrossParticipant(uint32_t mode, const Domain& sender, const Domain& receiver, uint64_t receiverFlag)
{
    if (std::get<0>(sender) != std::get<0>(receiver))
        return false;
    const auto st = std::get<1>(sender), rt = std::get<1>(receiver);
    if (mode == ACLSAN_DEVICE_SYNC_MODE_GLOBAL)
        return st == rt;
    const auto sp = Position3510(sender), rp = Position3510(receiver);
    if (!sp || !rp || sp->die != rp->die || sp->group != rp->group)
        return false;
    if (mode == ACLSAN_DEVICE_SYNC_MODE_GROUP_AIV)
        return st == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR && st == rt;
    if (mode == ACLSAN_DEVICE_SYNC_MODE_GROUP_AIC_AIV)
        return st != rt;
    if (mode == ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV)
        return st != rt &&
               (rt == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR || sp->subblock == ((receiverFlag >> 4U) & 1U));
    return false;
}
bool IsSupportedMemorySpace(uint32_t space)
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
class DependencyGraphBuilder {
    using Access = DependencyGraph::Access;
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
    using Pipe = std::pair<Domain, uint32_t>;
    std::vector<AclsanDeviceMemoryAccessData>& memory;
    std::vector<AclsanDeviceSyncData>& sync;
    const std::map<Instruction, std::set<uint32_t>>& skippedFragments;
    std::set<std::string>& reasons;
    const Limits& limits_;
    HappensBeforeGraph& graph;
    std::map<Instruction, Event> events;
    std::vector<Access>& accesses;
    std::map<Instruction, size_t> instructionStarts;
    // Prefix/acquire joins do not imply completion order within a pipeline.
    std::map<Pipe, std::optional<size_t>> prefix, acquired;
    // Only S/V/M complete in issue order within one core.
    std::map<Pipe, size_t> lastOrdered;
    bool graphExhausted = false;
    size_t joinNodes = 0;

public:
    DependencyGraphBuilder(
        std::vector<AclsanDeviceMemoryAccessData>& memoryData, std::vector<AclsanDeviceSyncData>& syncData,
        const std::map<Instruction, std::set<uint32_t>>& skipped, const Limits& limits, std::set<std::string>& failures,
        DependencyGraph& output)
        : memory(memoryData),
          sync(syncData),
          skippedFragments(skipped),
          reasons(failures),
          limits_(limits),
          graph(output.graph),
          accesses(output.accesses)
    {}

    void Run()
    {
        NormalizeEvents();
        ExpandUnitFlagAccesses();
        RouteCrossCoreNotifications();
        BuildNotificationTokens();
        BuildLocalEdges();
        ResolveCrossCoreWaits();
        if (!graph.Seal(reasons.empty()))
            reasons.insert("incomplete_sync_graph");
    }

private:
    void NormalizeEvents()
    {
        // Stable identities, independent of callback interleaving, determine tie breaks only.
        std::sort(memory.begin(), memory.end(), [](const auto& a, const auto& b) {
            return std::make_pair(Key(a.header), a.accessIndex) < std::make_pair(Key(b.header), b.accessIndex);
        });
        std::sort(sync.begin(), sync.end(), [](const auto& a, const auto& b) { return Key(a.header) < Key(b.header); });
        std::map<std::pair<Instruction, uint32_t>, size_t> accessKeys;
        std::map<Instruction, std::pair<uint32_t, std::set<uint32_t>>> counts;
        size_t segments = 0;
        for (const auto& a : memory) {
            const auto key = Key(a.header);
            auto old = accessKeys.find({key, a.accessIndex});
            if (old != accessKeys.end()) {
                if (!SameMemory(accesses[old->second].data, a))
                    reasons.insert("conflicting_duplicate");
                continue;
            }
            if (!a.accessCount || a.accessIndex >= a.accessCount || !ValidPipe(a.header.pipeline) ||
                a.header.blockType > ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE) {
                reasons.insert("invalid_access_identity");
                continue;
            }
            auto& count = counts[key];
            if (count.first && count.first != a.accessCount)
                reasons.insert("access_count_mismatch");
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
                    reasons.insert("instruction_identity_mismatch");
            }
            Access access{a, node, {}};
            if (a.instructionId >= static_cast<uint32_t>(aclsan::InstructionId::StAtomicB32) &&
                a.instructionId <= static_cast<uint32_t>(aclsan::InstructionId::StiAtomicB8)) {
                reasons.insert("atomic_semantics_unknown");
            }
            // Remote/shared owner mappings are not encoded by the current callback.
            if (a.header.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR &&
                a.memorySpace >= ACLSAN_DEVICE_MEMORY_SPACE_L1) {
                reasons.insert("owner_mapping_unknown");
            }
            if (a.accessMode < ACLSAN_DEVICE_MEMORY_ACCESS_READ ||
                a.accessMode > ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD || !IsSupportedMemorySpace(a.memorySpace)) {
                reasons.insert("access_coverage_unknown");
            } else if (!Normalize(a, limits_.segments - segments, access.ranges)) {
                reasons.insert("invalid_or_excessive_layout");
            }
            segments += access.ranges.size();
            accessKeys[{key, a.accessIndex}] = accesses.size();
            accesses.push_back(std::move(access));
        }
        for (const auto& c : counts) {
            auto observed = c.second.second;
            const auto skipped = skippedFragments.find(c.first);
            if (skipped != skippedFragments.end())
                for (auto index : skipped->second)
                    if (index < c.second.first)
                        observed.insert(index);
            if (c.second.first != observed.size())
                reasons.insert("missing_access_fragment");
        }
        for (size_t i = 0; i < sync.size(); ++i) {
            const auto& s = sync[i];
            auto found = events.find(Key(s.header));
            if (found != events.end()) {
                if (!found->second.sync || !SameSync(sync[found->second.index], s))
                    reasons.insert("conflicting_duplicate");
            } else
                events.emplace(Key(s.header), Event{true, i, graph.AddNode(), s.header.pipeline});
        }
    }

    void AddEdge(size_t a, size_t b, HappensBeforeGraph::Reason reason)
    {
        if (!graph.AddEdge(a, b, reason)) {
            reasons.insert("graph_budget");
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
            reasons.insert("graph_budget");
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
            reasons.insert("unit_flag_budget");
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
                reasons.insert("invalid_unit_flag");
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
                reasons.insert("invalid_unit_flag_access");
                continue;
            }
            for (const auto& range : access.ranges) {
                for (uint64_t begin = range.begin; begin < range.end;) {
                    const auto& boundaries = partition.boundaries.at(DomainOf(a.header));
                    const auto next = boundaries.upper_bound(begin / unitBytes);
                    const uint64_t end = next == boundaries.end() || *next > UINT64_MAX / unitBytes ?
                                             range.end :
                                             std::min(range.end, *next * unitBytes);
                    const uint64_t length = end - begin;
                    AddUnitFragment(partition, access, start->second, {Interval{begin, begin + length}}, true);
                    begin += length;
                    if (graphExhausted || partition.expanded.size() >= limits_.segments) {
                        if (begin < range.end)
                            reasons.insert("unit_flag_budget");
                        break;
                    }
                }
            }
        }
    }

    void MatchUnitFlags(const UnitPartition& partition)
    {
        // Match generations independently in each physical L0C unit. Callback
        // interleaving across M and FIX is not ordering evidence. Mode 2 holds
        // ownership; only mode 3 hands the unit to the opposite pipeline.
        for (const auto& item : partition.units) {
            const auto& q = item.second;
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
                        reasons.insert("unit_flag_missing_publish");
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
                    reasons.insert("unit_flag_missing_release");
                    break;
                }
            }
            if (r < q.reads.size())
                reasons.insert("unit_flag_missing_publish");
        }
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
            const auto& s = sync[e.index];
            if (s.syncKind != ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG || s.scope == ACLSAN_DEVICE_SYNC_SCOPE_BLOCK)
                continue;
            const auto sender = DomainOf(s.header);
            const bool intra = s.mode == ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV;
            const bool cube = s.header.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
            const auto flag = s.objectId & (intra && cube ? 31U : 15U);
            if (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_CORE ||
                (s.mode != ACLSAN_DEVICE_SYNC_MODE_GLOBAL && s.mode != ACLSAN_DEVICE_SYNC_MODE_GROUP_AIV &&
                 s.mode != ACLSAN_DEVICE_SYNC_MODE_GROUP_AIC_AIV && s.mode != ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV) ||
                (s.mode != ACLSAN_DEVICE_SYNC_MODE_GLOBAL && !Position3510(sender))) {
                reasons.insert("unsupported_cross_core_topology_or_mode");
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
                    reasons.insert("graph_budget");
                    break;
                }
                auto& counter = crossCounters[{receiver, intra, receiverFlag}];
                auto [streamIt, inserted] = counter.modes.try_emplace(s.mode);
                auto& stream = streamIt->second;
                if (inserted) {
                    // A notification requires every active participant in this
                    // mode's scope. A missing SET must not shrink that scope.
                    for (const auto& peer : domains)
                        if (CrossParticipant(s.mode, peer, receiver, receiverFlag))
                            stream.senders[peer];
                }
                stream.senders[sender].push_back(e.node);
            }
            if (!delivered)
                reasons.insert("pending_cross_core_set");
            if (routedSets > limits_.edges)
                break;
        }
    }

    void BuildNotificationTokens()
    {
        for (auto& entry : crossCounters) {
            for (auto& mode : entry.second.modes) {
                auto& stream = mode.second;
                size_t complete = std::numeric_limits<size_t>::max();
                for (const auto& sender : stream.senders)
                    complete = std::min(complete, sender.second.size());
                if (stream.senders.empty())
                    continue;
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
        }
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
            const auto& s = sync[e.index];
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
                    reasons.insert("barrier_coverage_unknown");
                }
                continue;
            }
            if (s.syncKind != ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG || !ValidPipe(s.srcPipe) || !ValidPipe(s.dstPipe) ||
                (s.scope == ACLSAN_DEVICE_SYNC_SCOPE_BLOCK && s.mode != ACLSAN_DEVICE_SYNC_MODE_GLOBAL) ||
                (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_CORE && s.srcPipe == s.dstPipe) ||
                (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_BLOCK && s.mode > ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV)) {
                reasons.insert("sync_coverage_unknown");
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
                    reasons.insert("invalid_cross_core_sync_action");
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
                    reasons.insert("duplicate_set");
                    continue;
                }
                if (const auto prior = prefix[{domain, s.srcPipe}])
                    AddEdge(*prior, e.node, HappensBeforeGraph::Reason::PublishPrefix);
                flags[key] = e.node;
            } else if (s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
                auto f = flags.find(key);
                if (f == flags.end() || invalidFlags.count(key) != 0) {
                    reasons.insert("unmatched_wait");
                    continue;
                }
                AddEdge(f->second, e.node, HappensBeforeGraph::Reason::TokenMatch);
                flags.erase(f);
                MergePrefix(acquired[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::AcquireSuffix);
                MergePrefix(prefix[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::PublishPrefix);
            } else
                reasons.insert("invalid_sync_action");
        }
        if (!flags.empty())
            reasons.insert("pending_set");
    }

    void ResolveCrossCoreWaits()
    {
        // WAIT consumes one notification from its receiver counter, irrespective
        // of mode. Resolve only orders proven by SET scalar order or existing HB
        // paths. Iteration allows handshakes on another flag to establish order;
        // callback arrival and map iteration order are never matching evidence.
        bool progress = true;
        while (progress && reasons.empty()) {
            progress = false;
            for (auto& entry : crossCounters) {
                auto& counter = entry.second;
                if (counter.consumed == counter.waits.size())
                    continue;
                const auto wait = counter.waits[counter.consumed];
                std::vector<NotificationStream*> candidates;
                for (auto& mode : counter.modes) {
                    auto& stream = mode.second;
                    if (stream.consumed == stream.tokens.size())
                        continue;
                    bool future = false;
                    for (auto source : stream.tokens[stream.consumed].contributors) {
                        const auto path = graph.PrecedesDuringBuild(wait, source);
                        if (!path) {
                            reasons.insert("graph_query_budget");
                            break;
                        }
                        future |= *path;
                    }
                    if (!future)
                        candidates.push_back(&stream);
                }
                NotificationStream* selected = nullptr;
                for (auto* candidate : candidates) {
                    bool first = true;
                    for (auto* other : candidates) {
                        if (candidate == other)
                            continue;
                        for (auto source : candidate->tokens[candidate->consumed].contributors) {
                            const auto path = graph.PrecedesDuringBuild(source, other->tokens[other->consumed].node);
                            if (!path)
                                reasons.insert("graph_query_budget");
                            if (!path || !*path) {
                                first = false;
                                break;
                            }
                        }
                        if (!first)
                            break;
                    }
                    if (first) {
                        selected = candidate;
                        break;
                    }
                }
                if (selected && reasons.empty()) {
                    AddEdge(selected->tokens[selected->consumed++].node, wait, HappensBeforeGraph::Reason::TokenMatch);
                    ++counter.consumed;
                    progress = true;
                }
            }
        }
        for (const auto& entry : crossCounters) {
            const auto& counter = entry.second;
            if (counter.consumed == counter.waits.size())
                continue; // Unconsumed SET notifications are allowed to accumulate.
            size_t remaining = 0;
            for (const auto& mode : counter.modes)
                remaining += mode.second.tokens.size() - mode.second.consumed;
            reasons.insert(
                remaining < counter.waits.size() - counter.consumed ? "unmatched_cross_core_wait" :
                                                                      "ambiguous_cross_core_notification_order");
        }
    }
};
} // namespace

void race::BuildDependencyGraph(
    std::vector<AclsanDeviceMemoryAccessData>& memory, std::vector<AclsanDeviceSyncData>& sync,
    const std::map<Instruction, std::set<uint32_t>>& skippedFragments, const Limits& limits,
    std::set<std::string>& reasons, DependencyGraph& output)
{
    DependencyGraphBuilder(memory, sync, skippedFragments, limits, reasons, output).Run();
} // namespace
} // namespace npucheck
