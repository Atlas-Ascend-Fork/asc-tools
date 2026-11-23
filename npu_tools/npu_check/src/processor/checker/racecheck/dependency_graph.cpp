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
#include "checker/racecheck/vf_subgraph.h"
#include "checker/memory_access_mode.h"
#include "checker/racecheck/analysis_phase_timer.h"
#include "npu_tool_log.h"
#include "device_instr/common/instruction_id.h"
#include "npu_tool_log.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace npucheck::race {
namespace {
constexpr uint32_t DAV3510_BLOCKS_PER_DIE = 54U;
constexpr uint32_t DAV3510_PHYSICAL_BLOCK_LIMIT = 2U * DAV3510_BLOCKS_PER_DIE;
constexpr uint32_t DAV3510_CUBE_GROUPS_PER_DIE = 18U;
constexpr uint32_t DAV3510_VECTOR_SUBBLOCKS_PER_GROUP = 2U;
constexpr uint32_t CROSS_CORE_MODE_SAME_PIPE = 0U;
constexpr uint32_t CROSS_CORE_MODE_VECTOR = 1U;
constexpr uint32_t CROSS_CORE_MODE_CROSS_TYPE = 2U;
constexpr uint32_t CROSS_CORE_MODE_RECEIVER_FLAG = 4U;
constexpr uint32_t RECEIVER_FLAG_SELECTOR_SHIFT = 4U;
constexpr uint64_t RECEIVER_FLAG_SELECTOR_MASK = 1U;
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
    return Key(a) == Key(b) && a.pc == b.pc && a.siteId == b.siteId && a.pipeline == b.pipeline && a.flags == b.flags &&
           a.serialNo == b.serialNo && a.sourceKind == b.sourceKind;
}
bool SameMemoryDescriptor(const AclsanDeviceMemoryAccessData& a, const AclsanDeviceMemoryAccessData& b)
{
    return SameHeader(a.header, b.header) && a.address == b.address && a.accessMode == b.accessMode &&
           a.memorySpace == b.memorySpace && a.accessCount == b.accessCount && a.instructionId == b.instructionId &&
           a.unitFlag == b.unitFlag && a.layoutKind == b.layoutKind && a.memoryFlags == b.memoryFlags &&
           a.dataBits == b.dataBits && a.alignSize == b.alignSize;
}

bool SameMemoryExecution(const AclsanDeviceMemoryAccessData& a, const AclsanDeviceMemoryAccessData& b)
{
    return a.vectorMask0 == b.vectorMask0 && a.vectorMask1 == b.vectorMask1 && a.predicateMask0 == b.predicateMask0 &&
           a.predicateMask1 == b.predicateMask1 && a.regDependencyMask0 == b.regDependencyMask0 &&
           a.regDependencyMask1 == b.regDependencyMask1 && a.vfInstrExecSeq == b.vfInstrExecSeq &&
           a.vfInstrValid == b.vfInstrValid && a.memoryOwner.phyCoreId == b.memoryOwner.phyCoreId &&
           a.memoryOwner.blockId == b.memoryOwner.blockId && a.memoryOwner.blockType == b.memoryOwner.blockType;
}

bool SameMemory(const AclsanDeviceMemoryAccessData& a, const AclsanDeviceMemoryAccessData& b)
{
    if (!SameMemoryDescriptor(a, b) || !SameMemoryExecution(a, b)) {
        return false;
    }
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
class DependencyGraphBuilder {
    using Access = DependencyGraph::Access;
    enum class InstructionKind {
        MEMORY,
        SYNC,
        VF,
        LOCAL_BARRIER,
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
    struct NotificationSender {
        std::optional<uint32_t> pipe;
        std::vector<size_t> completions;
    };
    struct NotificationStream {
        std::map<Domain, NotificationSender> senders;
        std::vector<Notification> tokens;
        size_t consumed = 0;
    };
    struct CrossCounter {
        std::map<uint32_t, NotificationStream> modes;
        std::vector<size_t> waits;
        std::optional<uint32_t> waitPipe;
        size_t consumed = 0;
    };
    std::map<CrossFlag, CrossCounter> crossCounters;
    std::map<size_t, size_t> crossCoreCompletions;
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
    std::vector<AclsanDeviceMemoryAccessData>& memory;
    std::vector<AclsanDeviceSyncData>& sync;
    const std::map<Instruction, std::set<uint32_t>>& skippedFragments;
    std::set<std::string>& reasons;
    const std::vector<AclsanDeviceRegisterStateData>& registers;
    const Limits& limits_;
    HappensBeforeGraph& graph;
    std::map<Instruction, Event> events;
    std::map<Instruction, VfSubgraph> vfGraphs;
    std::map<Instruction, AclsanDeviceVfData> vfMetadata;
    std::vector<AclsanDeviceVfData>& vfs;
    const std::vector<AclsanDeviceBarrierData>& localBarriers;
    uint64_t& warnings;
    std::vector<Access>& accesses;
    std::map<VfKey, DependencyGraph::VfBoundary>& vfBoundaries;
    std::map<Instruction, size_t> instructionStarts;
    UnitResetHistory unitResets;
    // Prefix/acquire joins do not imply completion order within a pipeline.
    std::map<Pipe, std::optional<size_t>> prefix, acquired;
    // Only S/V/M complete in issue order within one core.
    std::map<Pipe, size_t> lastOrdered;
    std::map<Pipe, size_t> lastCrossCoreCompletion;
    bool graphExhausted = false;
    bool budgetReported = false;
    size_t joinNodes = 0;
    size_t comparisons = 0;
    RangeStoragePool& rangeStorage_;

public:
    DependencyGraphBuilder(
        std::vector<AclsanDeviceMemoryAccessData>& memoryData, std::vector<AclsanDeviceSyncData>& syncData,
        const std::map<Instruction, std::set<uint32_t>>& skipped, const Limits& limits, std::set<std::string>& failures,
        DependencyGraph& output, const std::vector<AclsanDeviceRegisterStateData>& registerData,
        std::vector<AclsanDeviceVfData>& vfData, const std::vector<AclsanDeviceBarrierData>& barriers)
        : memory(memoryData),
          sync(syncData),
          skippedFragments(skipped),
          reasons(failures),
          registers(registerData),
          limits_(limits),
          graph(output.graph),
          vfs(vfData),
          localBarriers(barriers),
          warnings(output.warnings),
          accesses(output.accesses),
          vfBoundaries(output.vfBoundaries),
          rangeStorage_(output.rangeStorage)
    {}

    void Run()
    {
        const auto launchId = ObservedLaunchId();
        {
            AnalysisPhaseTimer phase(launchId, "normalize_events");
            NormalizeEvents();
        }
        {
            AnalysisPhaseTimer phase(launchId, "unit_flag_fragments");
            unitResets = UnitFlagResets(registers, reasons);
            ExpandUnitFlagAccesses();
            ReportRangeStorage();
        }
        {
            AnalysisPhaseTimer phase(launchId, "notification_tokens");
            RouteCrossCoreNotifications();
            BuildNotificationTokens();
        }
        {
            AnalysisPhaseTimer phase(launchId, "local_hb_edges");
            BuildLocalEdges();
            BuildPhysicalBlockEdges();
            OrderVfsOnPhysicalPipe();
        }
        {
            AnalysisPhaseTimer phase(launchId, "cross_core_waits");
            ResolveCrossCoreWaits();
        }
        {
            AnalysisPhaseTimer phase(launchId, "seal_graph");
            SealGraph();
        }
    }

private:
    uint64_t ObservedLaunchId() const
    {
        constexpr uint64_t MISSING_OBSERVED_LAUNCH_ID = 0;
        if (!memory.empty())
            return memory.front().header.launchId;
        if (!sync.empty())
            return sync.front().header.launchId;
        if (!registers.empty())
            return registers.front().header.launchId;
        return MISSING_OBSERVED_LAUNCH_ID;
    }
    void ReportRangeStorage() const
    {
        const auto launchId = ObservedLaunchId();
        const auto& stats = rangeStorage_.Stats();
        const auto& budget = *rangeStorage_.Budget();
        ASCTOOL_INFO(
            "range storage: launch=%llu calls=%zu hits=%zu materialized=%zu admitted=%zu keys=%zu "
            "live_capacity_intervals=%zu peak_capacity_intervals=%zu limit_intervals=%zu "
            "storage_rejected=%zu materialization_rejected=%zu untracked=%zu cache_disabled=%u",
            static_cast<unsigned long long>(launchId), stats.calls, stats.hits, stats.materialized, stats.admitted,
            rangeStorage_.CachedKeys(), budget.Used(), budget.Peak(), budget.Limit(), stats.storageRejected,
            stats.materializationRejected, stats.untracked, static_cast<unsigned>(stats.cacheDisabled));
    }
    void NoteBudget(const char* stage, size_t used, size_t limit, const AclsanDeviceEventHeader* header = nullptr)
    {
        if (budgetReported)
            return;
        budgetReported = true;
        if (header == nullptr && !memory.empty())
            header = &memory.front().header;
        if (header == nullptr && !sync.empty())
            header = &sync.front().header;
        const size_t minimum = limit < std::numeric_limits<size_t>::max() ? limit + 1 : limit;
        constexpr uint64_t MISSING_TRACE_LAUNCH_ID = 0;
        constexpr uint32_t MISSING_TRACE_CORE_ID = 0;
        constexpr uint64_t MISSING_TRACE_PC = 0;
        constexpr uint64_t MISSING_TRACE_EXECUTION_ID = 0;
        ASCTOOL_WARNING(
            "analysis budget: launch=%llu stage=%s used=%zu required_at_least=%zu limit=%zu core=%u pc=0x%llx "
            "instruction=%llu",
            static_cast<unsigned long long>(header ? header->launchId : MISSING_TRACE_LAUNCH_ID), stage, used, minimum,
            limit, header ? header->executor.phyCoreId : MISSING_TRACE_CORE_ID,
            static_cast<unsigned long long>(header ? header->pc : MISSING_TRACE_PC),
            static_cast<unsigned long long>(header ? header->instrExecId : MISSING_TRACE_EXECUTION_ID));
    }
    void NormalizeAccessRanges(const AclsanDeviceMemoryAccessData& access, SharedRanges& ranges)
    {
        const auto status = rangeStorage_.Normalize(access, ranges);
        if (status == RangeStorageStatus::Complete)
            return;
        if (status == RangeStorageStatus::StorageBudget) {
            reasons.insert("range_storage_budget");
            NoteBudget(
                "live_range_capacity", rangeStorage_.Budget()->Used(), rangeStorage_.Budget()->Limit(), &access.header);
        } else if (status == RangeStorageStatus::MaterializationBudget) {
            reasons.insert("invalid_or_excessive_layout");
            reasons.insert("segment_budget");
            NoteBudget(
                "single_footprint_materialization", limits_.FootprintIntervalBudget(),
                limits_.FootprintIntervalBudget(), &access.header);
        } else {
            reasons.insert("invalid_or_excessive_layout");
            if (reasons.insert("invalid_memory_layout").second)
                ASCTOOL_WARNING(
                    "analysis layout invalid: launch=%llu core=%u instruction=%llu pc=0x%llx",
                    static_cast<unsigned long long>(access.header.launchId), access.header.executor.phyCoreId,
                    static_cast<unsigned long long>(access.header.instrExecId),
                    static_cast<unsigned long long>(access.header.pc));
        }
    }
    using FragmentCounts = std::map<Instruction, std::pair<uint32_t, std::set<uint32_t>>>;
    using VfEventCounts = std::map<Instruction, size_t>;

    void ValidateInstructionIdentities()
    {
        std::map<std::pair<uint64_t, std::optional<uint64_t>>, std::pair<AclsanDeviceEventHeader, InstructionKind>>
            identities;
        const auto identity = [this, &identities](
                                  const AclsanDeviceEventHeader& h, const Instruction& key, InstructionKind kind) {
            const auto& [domain, instructionId, vfSequence, launchId] = key;
            const auto [it, inserted] =
                identities.emplace(std::make_pair(instructionId, vfSequence), std::make_pair(h, kind));
            if (h.instrExecId == 0 ||
                (!inserted && (it->second.second != kind || DomainOf(it->second.first) != DomainOf(h) ||
                               it->second.first.pc != h.pc || it->second.first.siteId != h.siteId ||
                               it->second.first.pipeline != h.pipeline))) {
                reasons.insert("instruction_identity_mismatch");
            }
        };
        for (const auto& a : memory) {
            identity(a.header, Key(a), InstructionKind::MEMORY);
        }
        for (const auto& a : sync) {
            identity(a.header, Key(a.header), InstructionKind::SYNC);
        }
        for (const auto& a : vfs) {
            identity(a.header, Key(a.header), InstructionKind::VF);
        }
        for (const auto& a : localBarriers) {
            identity(a.header, Key(a), InstructionKind::LOCAL_BARRIER);
        }
    }

    void SortEvents()
    {
        // Stable identities, independent of callback interleaving, determine tie breaks only.
        std::sort(memory.begin(), memory.end(), [](const auto& a, const auto& b) {
            return std::make_pair(Key(a), a.accessIndex) < std::make_pair(Key(b), b.accessIndex);
        });
        std::sort(sync.begin(), sync.end(), [](const auto& a, const auto& b) { return Key(a.header) < Key(b.header); });
        std::sort(vfs.begin(), vfs.end(), [](const auto& a, const auto& b) { return Key(a.header) < Key(b.header); });
    }

    void NormalizeVfMetadata()
    {
        for (const auto& vf : vfs) {
            const auto key = Key(vf.header);
            const auto old = vfMetadata.find(key);
            if (old != vfMetadata.end()) {
                if (!SameHeader(old->second.header, vf.header) || old->second.status != vf.status ||
                    old->second.accessCount != vf.accessCount || old->second.eventCount != vf.eventCount ||
                    old->second.rawEventCount != vf.rawEventCount || old->second.warningFlags != vf.warningFlags) {
                    reasons.insert("conflicting_vf_expansion");
                }
                continue;
            }
            vfMetadata.emplace(key, vf);
            EnsureVf(key);
            if ((vf.warningFlags & static_cast<uint32_t>(ACLSAN_DEVICE_VF_MASK_NOT_REFRESHED)) != 0U) {
                ++warnings;
                ASCTOOL_WARNING(
                    "racecheck: VF mask never refreshed launch=%llu instr=%llu pc=0x%llx core=%u block=%u",
                    static_cast<unsigned long long>(vf.header.launchId),
                    static_cast<unsigned long long>(vf.header.instrExecId),
                    static_cast<unsigned long long>(vf.header.pc), vf.header.executor.phyCoreId,
                    vf.header.executor.blockId);
            }
        }
    }

    size_t NormalizeVfAccessNode(const AclsanDeviceMemoryAccessData& a, bool vfMode, VfEventCounts& vfAccessCounts)
    {
        const Instruction parent = Key(a.header);
        if (!vfMode || a.vfInstrValid != 1 || a.header.pipeline != ACLSAN_DEVICE_PIPE_VECTOR ||
            a.header.executor.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR ||
            a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_UB || a.unitFlag != 0) {
            reasons.insert("invalid_vf_access");
        }
        ++vfAccessCounts[parent];
        auto& vf = EnsureVf(parent);
        const auto node = vf.AddAccess(a);
        if (a.instructionId != 0 && a.instructionId != static_cast<uint32_t>(aclsan::InstructionId::Vf)) {
            reasons.insert("invalid_vf_access");
        }
        return node;
    }

    std::optional<size_t> NormalizeAccessNode(
        const AclsanDeviceMemoryAccessData& a, const Instruction& key, VfEventCounts& vfAccessCounts)
    {
        const bool vfMode = a.accessMode >= ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ &&
                            a.accessMode <= ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE;
        if ((vfMode && a.vfInstrValid != 1) || (a.vfInstrValid == 1 && a.header.instrExecId == 0)) {
            reasons.insert("missing_vf_instruction_index");
            return std::nullopt;
        }
        if (a.vfInstrValid != 0) {
            return NormalizeVfAccessNode(a, vfMode, vfAccessCounts);
        }
        const auto event = events.find(key);
        if (event == events.end()) {
            const auto node = graph.AddNode();
            events.emplace(key, Event{false, accesses.size(), node, a.header.pipeline});
            return node;
        }
        if (vfGraphs.count(key) != 0) {
            reasons.insert("vf_group_identity_mismatch");
            return std::nullopt;
        }
        const auto& first = accesses[event->second.index].data;
        if (event->second.pipe != a.header.pipeline || first.header.pc != a.header.pc ||
            first.header.siteId != a.header.siteId || first.instructionId != a.instructionId ||
            (first.unitFlag != a.unitFlag && a.header.pipeline != ACLSAN_DEVICE_PIPE_FIXPIPE)) {
            reasons.insert("instruction_identity_mismatch");
        }
        return event->second.node;
    }

    Access NormalizeAccessCoverage(const AclsanDeviceMemoryAccessData& a, size_t node)
    {
        Access access{a, node, {}};
        if (a.instructionId == static_cast<uint32_t>(aclsan::InstructionId::Vf) && a.vfInstrValid == 0) {
            reasons.insert("missing_vf_instruction_index");
        }
        if ((a.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN)) != 0U) {
            reasons.insert("atomic_semantics_unknown");
        }
        if ((a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB &&
             a.memoryOwner.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR) ||
            (a.memorySpace >= ACLSAN_DEVICE_MEMORY_SPACE_L1 && a.memorySpace <= ACLSAN_DEVICE_MEMORY_SPACE_L0C &&
             a.memoryOwner.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE)) {
            reasons.insert("owner_mapping_unknown");
        }
        if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB && IsAtomicWrite(a.accessMode)) {
            reasons.insert("atomic_semantics_unknown");
        }
        if (!IsSupportedMemoryAccessMode(NormalizeMemoryAccessMode(a.accessMode)) ||
            a.memorySpace > ACLSAN_DEVICE_MEMORY_SPACE_FB ||
            !IsSupportedMemorySpace(static_cast<AclsanDeviceMemorySpace>(a.memorySpace))) {
            reasons.insert("access_coverage_unknown");
        } else {
            NormalizeAccessRanges(a, access.ranges);
        }
        return access;
    }

    void NormalizeMemoryOperands(FragmentCounts& counts, VfEventCounts& vfAccessCounts)
    {
        std::map<std::pair<Instruction, uint32_t>, size_t> accessKeys;
        std::map<Instruction, const AclsanDeviceMemoryAccessData*> instructionMetadata;
        for (const auto& a : memory) {
            const auto key = Key(a);
            auto old = accessKeys.find({key, a.accessIndex});
            if (old != accessKeys.end()) {
                if (!SameMemory(accesses[old->second].data, a))
                    reasons.insert("conflicting_duplicate");
                continue;
            }
            if (a.accessCount == 0U || a.accessIndex >= a.accessCount || !ValidPipe(a.header.pipeline) ||
                a.header.executor.blockType > ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE) {
                reasons.insert("invalid_access_identity");
                continue;
            }
            const auto [metadata, inserted] = instructionMetadata.emplace(key, &a);
            if (!inserted) {
                const auto& first = *metadata->second;
                if (first.vfInstrValid != a.vfInstrValid ||
                    (a.vfInstrValid != 0U &&
                     (first.accessMode != a.accessMode || first.vfInstrExecSeq != a.vfInstrExecSeq))) {
                    reasons.insert("vf_group_identity_mismatch");
                }
            }
            auto& count = counts[key];
            if (count.first != 0 && count.first != a.accessCount)
                reasons.insert("access_count_mismatch");
            count.first = a.accessCount;
            count.second.insert(a.accessIndex);
            const auto node = NormalizeAccessNode(a, key, vfAccessCounts);
            if (!node.has_value()) {
                continue;
            }
            auto access = NormalizeAccessCoverage(a, *node);
            accessKeys[{key, a.accessIndex}] = accesses.size();
            accesses.push_back(std::move(access));
        }
    }

    void ValidateAccessFragments(const FragmentCounts& counts)
    {
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
    }

    void NormalizeVfBarriers(VfEventCounts& vfBarrierCounts)
    {
        std::map<Instruction, AclsanDeviceBarrierData> uniqueBarriers;
        for (const auto& b : localBarriers) {
            const auto key = Key(b);
            const auto old = uniqueBarriers.find(key);
            if (old != uniqueBarriers.end()) {
                if (!SameHeader(old->second.header, b.header) || old->second.srcMemType != b.srcMemType ||
                    old->second.dstMemType != b.dstMemType || old->second.pipe != b.pipe ||
                    old->second.vfInstrExecSeq != b.vfInstrExecSeq) {
                    reasons.insert("conflicting_duplicate");
                }
                continue;
            }
            uniqueBarriers.emplace(key, b);
            const Instruction parent = Key(b.header);
            constexpr uint32_t all = static_cast<uint32_t>(ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL) |
                                     static_cast<uint32_t>(ACLSAN_DEVICE_LOCAL_MEM_SCALAR_ALL);
            if (b.header.instrExecId == 0 || b.barrierKind != ACLSAN_DEVICE_BARRIER_LOCAL_MEM || b.srcMemType == 0 ||
                b.dstMemType == 0 || ((b.srcMemType | b.dstMemType) & ~all) != 0 ||
                b.header.pipeline != ACLSAN_DEVICE_PIPE_VECTOR || b.pipe != ACLSAN_DEVICE_PIPE_INVALID) {
                reasons.insert("invalid_vf_barrier");
            }
            ++vfBarrierCounts[parent];
            EnsureVf(parent).AddBarrier(b);
        }
    }

    void FinalizeVfGraphs(VfEventCounts& vfAccessCounts, VfEventCounts& vfBarrierCounts)
    {
        for (auto& vf : vfGraphs) {
            const auto metadata = vfMetadata.find(vf.first);
            if (metadata == vfMetadata.end()) {
                reasons.insert("missing_vf_expansion");
                continue;
            }
            const auto& completion = metadata->second;
            if (completion.header.pipeline != ACLSAN_DEVICE_PIPE_VECTOR ||
                completion.header.executor.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR) {
                reasons.insert("invalid_vf_identity");
            }
            if (completion.status != ACLSAN_DEVICE_VF_SUCCESS) {
                reasons.insert("vf_expansion_incomplete");
            }
            if (completion.accessCount != vfAccessCounts[vf.first] ||
                completion.eventCount != vfAccessCounts[vf.first] + vfBarrierCounts[vf.first] ||
                completion.eventCount > completion.rawEventCount) {
                reasons.insert("missing_vf_event");
            }
            if (reasons.empty()) {
                vf.second.Build();
            }
        }
    }

    void NormalizeSyncEvents()
    {
        for (size_t i = 0; i < sync.size(); ++i) {
            const auto& s = sync[i];
            auto found = events.find(Key(s.header));
            if (found != events.end()) {
                if (!found->second.sync || !SameSync(sync[found->second.index], s))
                    reasons.insert("conflicting_duplicate");
            } else {
                const auto node = graph.AddNode();
                events.emplace(Key(s.header), Event{true, i, node, s.header.pipeline});
                ASCTOOL_DEBUG(
                    "racecheck DAG graph=%p sync launch=%llu instr=%llu node=%zu pipe=%u", static_cast<void*>(&graph),
                    static_cast<unsigned long long>(s.header.launchId),
                    static_cast<unsigned long long>(s.header.instrExecId), node, s.header.pipeline);
            }
        }
    }

    void NormalizeEvents()
    {
        ValidateInstructionIdentities();
        SortEvents();
        NormalizeVfMetadata();
        AnalysisPhaseTimer phase(ObservedLaunchId(), "normalize_operands");
        FragmentCounts counts;
        VfEventCounts vfAccessCounts;
        VfEventCounts vfBarrierCounts;
        NormalizeMemoryOperands(counts, vfAccessCounts);
        ValidateAccessFragments(counts);
        NormalizeVfBarriers(vfBarrierCounts);
        FinalizeVfGraphs(vfAccessCounts, vfBarrierCounts);
        NormalizeSyncEvents();
    }

    VfSubgraph& EnsureVf(const Instruction& key)
    {
        auto found = vfGraphs.find(key);
        if (found != vfGraphs.end()) {
            return found->second;
        }
        auto& vf = vfGraphs.try_emplace(key, graph, reasons).first->second;
        if (events.count(key) != 0) {
            reasons.insert("vf_group_identity_mismatch");
        }
        events[key] = Event{false, 0, vf.Completion(), ACLSAN_DEVICE_PIPE_VECTOR};
        instructionStarts[key] = vf.Entry();
        const auto& [domain, instructionId, vfSequence, launchId] = key;
        vfBoundaries.emplace(
            VfKey{domain, instructionId, launchId}, DependencyGraph::VfBoundary{vf.Entry(), vf.Completion()});
        ASCTOOL_DEBUG(
            "racecheck DAG graph=%p VF instr=%llu entry=%zu completion=%zu", static_cast<void*>(&graph),
            static_cast<unsigned long long>(instructionId), vf.Entry(), vf.Completion());
        return vf;
    }

    void OrderVfsOnPhysicalPipe()
    {
        struct PipeVf {
            uint64_t instructionId;
            uint32_t blockId;
            const VfSubgraph* subgraph;
        };
        std::map<std::pair<uint32_t, uint32_t>, std::vector<PipeVf>> pipes;
        for (const auto& [instruction, subgraph] : vfGraphs) {
            const auto& [domain, instructionId, vfSequence, launchId] = instruction;
            const auto& [deviceId, blockType, blockId, physicalCoreId] = domain;
            pipes[{deviceId, physicalCoreId}].push_back({instructionId, blockId, &subgraph});
        }
        for (auto& pipe : pipes) {
            auto& vfs = pipe.second;
            // IDs within one physical trace slice follow its instruction order.
            std::sort(vfs.begin(), vfs.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.instructionId < rhs.instructionId;
            });
            for (size_t i = 1; i < vfs.size(); ++i) {
                if (vfs[i - 1].blockId != vfs[i].blockId) {
                    AddEdge(
                        vfs[i - 1].subgraph->Completion(), vfs[i].subgraph->Entry(),
                        HappensBeforeGraph::Reason::PipelineOrder);
                }
            }
        }
    }

    void AddEdge(size_t a, size_t b, HappensBeforeGraph::Reason reason)
    {
        if (!graph.AddEdge(a, b, reason)) {
            reasons.insert("graph_budget");
            if (graph.EdgeCount() >= limits_.GraphEdgeBudget())
                NoteBudget("graph_edges", graph.EdgeCount(), limits_.GraphEdgeBudget());
            else
                reasons.insert("invalid_graph_edge");
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
        if (joinNodes >= limits_.SyntheticGraphNodeBudget()) {
            reasons.insert("graph_budget");
            NoteBudget("join_nodes", joinNodes, limits_.SyntheticGraphNodeBudget());
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
        std::map<std::tuple<Domain, uint64_t, uint64_t>, UnitQueues> units;
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
        UnitPartition& partition, const Access& access, size_t start, const SharedRanges& ranges, bool unit)
    {
        const auto& a = access.data;
        const auto key = Key(a);
        if (partition.expanded.size() >= limits_.UnitFlagFragmentBudget() || graphExhausted) {
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
            auto& q =
                partition
                    .units[{DomainOf(a.header), ranges.front().begin / unitBytes, UnitFlagEpoch(a.header, unitResets)}];
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
            SharedRanges fragment;
            if (rangeStorage_.Admit({Interval{begin, begin + length}}, fragment) != RangeStorageStatus::Complete) {
                reasons.insert("range_storage_budget");
                NoteBudget(
                    "unit_flag_range_capacity", rangeStorage_.Budget()->Used(), rangeStorage_.Budget()->Limit(),
                    &a.header);
                graphExhausted = true;
                return;
            }
            AddUnitFragment(partition, access, start, fragment, true);
            begin += length;
            if (graphExhausted || partition.expanded.size() >= limits_.UnitFlagFragmentBudget()) {
                if (begin < range.end)
                    reasons.insert("unit_flag_budget");
                break;
            }
        }
    }

    void PartitionUnitAccesses(UnitPartition& partition)
    {
        for (const auto& access : accesses) {
            const auto& a = access.data;
            if (a.unitFlag == 0) {
                if (a.header.pipeline == ACLSAN_DEVICE_PIPE_FIXPIPE) {
                    const auto key = Key(a);
                    auto start = instructionStarts.find(key);
                    if (start == instructionStarts.end()) {
                        start = instructionStarts.emplace(key, graph.AddNode()).first;
                    }
                    AddUnitFragment(partition, access, start->second, access.ranges, false);
                } else {
                    partition.expanded.push_back(access);
                }
                continue;
            }
            if ((a.unitFlag != UNIT_FLAG_MODE_START && a.unitFlag != UNIT_FLAG_MODE_END) ||
                a.header.executor.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE ||
                (a.header.pipeline != ACLSAN_DEVICE_PIPE_MATRIX && a.header.pipeline != ACLSAN_DEVICE_PIPE_FIXPIPE)) {
                reasons.insert("invalid_unit_flag");
                partition.expanded.push_back(access);
                continue;
            }
            const auto key = Key(a);
            auto start = instructionStarts.find(key);
            if (start == instructionStarts.end())
                start = instructionStarts.emplace(key, graph.AddNode()).first;
            if (a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_L0C) {
                AddUnitFragment(partition, access, start->second, access.ranges, false);
                continue;
            }
            if (a.header.pipeline == ACLSAN_DEVICE_PIPE_FIXPIPE &&
                NormalizeMemoryAccessMode(a.accessMode) != ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
                reasons.insert("invalid_unit_flag_access");
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
        {
            AnalysisPhaseTimer phase(ObservedLaunchId(), "unit_boundaries");
            CollectUnitBoundaries(partition);
        }
        {
            AnalysisPhaseTimer phase(ObservedLaunchId(), "unit_partition");
            PartitionUnitAccesses(partition);
            accesses = std::move(partition.expanded);
        }
        {
            AnalysisPhaseTimer phase(ObservedLaunchId(), "unit_queue_match");
            MatchUnitFlags(partition);
        }
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
        auto& sender = stream.senders[DomainOf(s.header)];
        if (sender.pipe.has_value() && *sender.pipe != s.srcPipe) {
            reasons.insert("ambiguous_cross_core_notification_order");
        }
        sender.pipe = s.srcPipe;
        sender.completions.push_back(node);
    }

    static bool SupportsCrossCoreNotification(const AclsanDeviceSyncData& event, const Domain& sender)
    {
        return event.scope == ACLSAN_DEVICE_SYNC_SCOPE_CORE &&
               (event.mode == CROSS_CORE_MODE_SAME_PIPE || event.mode == CROSS_CORE_MODE_VECTOR ||
                event.mode == CROSS_CORE_MODE_CROSS_TYPE || event.mode == CROSS_CORE_MODE_RECEIVER_FLAG) &&
               (event.mode == CROSS_CORE_MODE_SAME_PIPE || Position3510(sender).has_value());
    }

    std::optional<uint64_t> ResolveReceiverFlag(
        const AclsanDeviceSyncData& event, const Domain& sender, const Domain& receiver, uint64_t flag) const
    {
        if (event.mode != CROSS_CORE_MODE_RECEIVER_FLAG) {
            return flag;
        }
        const auto senderPosition = Position3510(sender);
        const auto receiverPosition = Position3510(receiver);
        if (!senderPosition.has_value() || !receiverPosition.has_value()) {
            return std::nullopt;
        }
        const bool cube = event.header.executor.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        if (cube && receiverPosition->subblock != (flag >> 4U)) {
            return std::nullopt;
        }
        return cube ? flag & 15U : flag + 16U * senderPosition->subblock;
    }

    size_t CrossCoreCompletion(size_t issue) const
    {
        const auto completion = crossCoreCompletions.find(issue);
        return completion == crossCoreCompletions.end() ? issue : completion->second;
    }

    std::optional<size_t> RegisterCrossCoreCompletion(size_t issue, uint32_t pipe)
    {
        if (pipe == ACLSAN_DEVICE_PIPE_SCALAR) {
            return issue;
        }
        if (joinNodes >= limits_.SyntheticGraphNodeBudget()) {
            reasons.insert("graph_budget");
            NoteBudget("cross_core_completion_nodes", joinNodes, limits_.SyntheticGraphNodeBudget());
            graphExhausted = true;
            return std::nullopt;
        }
        ++joinNodes;
        const size_t completion = graph.AddNode();
        crossCoreCompletions.emplace(issue, completion);
        return completion;
    }

    void RecordCrossCoreWait(const CrossFlag& key, uint32_t pipe, size_t completion)
    {
        auto& counter = crossCounters[key];
        if (counter.waitPipe.has_value() && *counter.waitPipe != pipe) {
            reasons.insert("ambiguous_cross_core_notification_order");
        }
        counter.waitPipe = pipe;
        counter.waits.push_back(completion);
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
            const bool intra = s.mode == CROSS_CORE_MODE_RECEIVER_FLAG;
            const bool cube = s.header.executor.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
            const auto flag = s.objectId & (intra && cube ? 31U : 15U);
            if (!SupportsCrossCoreNotification(s, sender)) {
                reasons.insert("unsupported_cross_core_topology_or_mode");
                continue;
            }
            const bool isWait = s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT;
            if (!isWait && s.action != ACLSAN_DEVICE_SYNC_ACTION_SET) {
                continue;
            }
            const auto completion = RegisterCrossCoreCompletion(e.node, isWait ? s.dstPipe : s.srcPipe);
            if (!completion.has_value()) {
                break;
            }
            if (isWait) {
                RecordCrossCoreWait({sender, intra, flag}, s.dstPipe, *completion);
                continue;
            }
            bool delivered = false;
            for (const auto& receiver : domains) {
                const auto receiverFlag = ResolveReceiverFlag(s, sender, receiver, flag);
                if (!receiverFlag.has_value() || !CrossParticipant(s.mode, sender, receiver, *receiverFlag)) {
                    continue;
                }
                delivered = true;
                if (++routedSets > limits_.RoutedNotificationBudget()) {
                    reasons.insert("graph_budget");
                    break;
                }
                RecordNotification(s, receiver, *receiverFlag, *completion, domains);
            }
            if (!delivered)
                reasons.insert("pending_cross_core_set");
            if (routedSets > limits_.RoutedNotificationBudget())
                break;
        }
    }

    void BuildStreamTokens(NotificationStream& stream)
    {
        size_t complete = std::numeric_limits<size_t>::max();
        for (const auto& sender : stream.senders)
            complete = std::min(complete, sender.second.completions.size());
        if (stream.senders.empty())
            return;
        for (size_t i = 0; i < complete; ++i) {
            std::optional<size_t> joined;
            std::vector<size_t> contributors;
            for (const auto& sender : stream.senders) {
                const auto node = sender.second.completions[i];
                contributors.push_back(node);
                MergePrefix(joined, node, HappensBeforeGraph::Reason::TokenMatch);
            }
            // Each sender contributes notifications in its selected pipe's order.
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

    struct PhysicalBlockSpan {
        Domain domain;
        uint64_t first;
        uint64_t last;
    };
    using PhysicalSlot = std::pair<uint32_t, uint32_t>;
    using BlockBoundaries = std::map<Domain, std::pair<size_t, size_t>>;

    std::map<PhysicalSlot, std::vector<PhysicalBlockSpan>> PhysicalBlockSpans() const
    {
        std::map<PhysicalSlot, std::vector<PhysicalBlockSpan>> slots;
        for (auto item = events.cbegin(); item != events.cend();) {
            const auto domain = std::get<0>(item->first);
            const uint64_t first = std::get<1>(item->first);
            uint64_t last = first;
            do {
                last = std::get<1>(item->first);
                ++item;
            } while (item != events.cend() && std::get<0>(item->first) == domain);
            slots[{std::get<0>(domain), std::get<3>(domain)}].push_back({domain, first, last});
        }
        return slots;
    }

    bool ValidPhysicalBlockSpans(PhysicalSlot slot, std::vector<PhysicalBlockSpan>& blocks)
    {
        constexpr uint32_t SLOT_ID_SHIFT = 32;
        std::sort(
            blocks.begin(), blocks.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto& block = blocks[i];
            if (slot.second >= DAV3510_PHYSICAL_BLOCK_LIMIT || (block.first >> SLOT_ID_SHIFT) != slot.second ||
                (block.last >> SLOT_ID_SHIFT) != slot.second || static_cast<uint32_t>(block.first) == 0 ||
                std::get<1>(block.domain) != std::get<1>(blocks.front().domain) ||
                (i != 0 && blocks[i - 1].last >= block.first)) {
                reasons.insert("physical_block_order_unknown");
                return false;
            }
        }
        return true;
    }

    void AddPhysicalBlockBoundaries(const std::vector<PhysicalBlockSpan>& blocks, BlockBoundaries& boundaries)
    {
        std::optional<size_t> previous;
        for (const auto& block : blocks) {
            if (graphExhausted)
                return;
            constexpr size_t BOUNDARY_NODES = 2;
            if (joinNodes > limits_.SyntheticGraphNodeBudget() ||
                limits_.SyntheticGraphNodeBudget() - joinNodes < BOUNDARY_NODES) {
                reasons.insert("graph_budget");
                graphExhausted = true;
                return;
            }
            const size_t begin = graph.AddNode();
            const size_t end = graph.AddNode();
            joinNodes += BOUNDARY_NODES;
            boundaries.emplace(block.domain, std::make_pair(begin, end));
            if (previous)
                AddEdge(*previous, begin, HappensBeforeGraph::Reason::PhysicalBlockOrder);
            previous = end;
        }
    }

    void BuildPhysicalBlockEdges()
    {
        auto slots = PhysicalBlockSpans();
        BlockBoundaries boundaries;
        for (auto& entry : slots) {
            if (entry.second.size() > 1 && ValidPhysicalBlockSpans(entry.first, entry.second))
                AddPhysicalBlockBoundaries(entry.second, boundaries);
        }
        for (const auto& item : events) {
            if (graphExhausted)
                break;
            const auto boundary = boundaries.find(std::get<0>(item.first));
            if (boundary == boundaries.end())
                continue;
            const auto start = instructionStarts.find(item.first);
            const size_t begin =
                !item.second.sync && start != instructionStarts.end() ? start->second : item.second.node;
            AddEdge(boundary->second.first, begin, HappensBeforeGraph::Reason::PhysicalBlockOrder);
            AddEdge(
                CrossCoreCompletion(item.second.node), boundary->second.second,
                HappensBeforeGraph::Reason::PhysicalBlockOrder);
        }
    }

    using Flag = std::tuple<Domain, uint32_t, uint32_t, uint32_t, uint64_t>;

    void BuildMemoryEdges(const Domain& domain, const Event& e, size_t begin)
    {
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
    }

    void BuildBarrierEdges(const Domain& domain, const Event& e, const AclsanDeviceSyncData& s)
    {
        if (s.dstPipe == ACLSAN_DEVICE_PIPE_ALL) {
            const uint32_t pipes[] = {ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MATRIX,
                                      ACLSAN_DEVICE_PIPE_MTE1,   ACLSAN_DEVICE_PIPE_MTE2,   ACLSAN_DEVICE_PIPE_MTE3,
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
    }

    void BuildCrossCorePipeEdges(const Pipe& pipe, size_t issue, size_t completion, bool isSet)
    {
        if (issue != completion) {
            AddEdge(issue, completion, HappensBeforeGraph::Reason::PipelineOrder);
            const auto previous = lastCrossCoreCompletion.find(pipe);
            if (previous != lastCrossCoreCompletion.end()) {
                AddEdge(previous->second, completion, HappensBeforeGraph::Reason::PipelineOrder);
            }
        }
        lastCrossCoreCompletion[pipe] = completion;
        if (isSet) {
            if (const auto prior = prefix[pipe]; prior.has_value() && *prior != completion) {
                AddEdge(*prior, completion, HappensBeforeGraph::Reason::PublishPrefix);
            }
        } else {
            MergePrefix(acquired[pipe], completion, HappensBeforeGraph::Reason::AcquireSuffix);
        }
        MergePrefix(prefix[pipe], completion, HappensBeforeGraph::Reason::PublishPrefix);
    }

    void BuildLocalCrossCoreEdges(
        const Domain& domain, const Event& e, const AclsanDeviceSyncData& s, std::map<Domain, size_t>& scalarWaitGate)
    {
        // Issue is ordered by scalar; execution on a selected pipe must not
        // publish that pipe's work back onto the issuing scalar path.
        const Pipe scalar{domain, ACLSAN_DEVICE_PIPE_SCALAR};
        if (const auto prior = prefix[scalar])
            AddEdge(*prior, e.node, HappensBeforeGraph::Reason::PipelineOrder);
        lastOrdered[scalar] = e.node;
        const bool isSet = s.action == ACLSAN_DEVICE_SYNC_ACTION_SET;
        if (!isSet && s.action != ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
            reasons.insert("invalid_cross_core_sync_action");
            return;
        }
        const uint32_t pipe = isSet ? s.srcPipe : s.dstPipe;
        const size_t completion = CrossCoreCompletion(e.node);
        if (pipe != ACLSAN_DEVICE_PIPE_SCALAR) {
            MergePrefix(prefix[scalar], e.node, HappensBeforeGraph::Reason::PublishPrefix);
        } else if (!isSet) {
            // A scalar wait delays later issue, not already queued work.
            scalarWaitGate[domain] = completion;
        }
        BuildCrossCorePipeEdges({domain, pipe}, e.node, completion, isSet);
    }

    void BuildBlockFlagEdges(
        const Domain& domain, const Event& e, const AclsanDeviceSyncData& s, std::map<Flag, size_t>& flags,
        std::set<Flag>& invalidFlags)
    {
        const Flag key{domain, s.scope, s.srcPipe, s.dstPipe, s.objectId};
        if (invalidFlags.count(key) != 0) {
            if (s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
                flags.erase(key);
                invalidFlags.erase(key);
            }
            return;
        }
        if (s.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
            if (flags.count(key) != 0) {
                invalidFlags.insert(key);
                reasons.insert("duplicate_set");
                return;
            }
            if (const auto prior = prefix[{domain, s.srcPipe}])
                AddEdge(*prior, e.node, HappensBeforeGraph::Reason::PublishPrefix);
            flags[key] = e.node;
        } else if (s.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
            auto f = flags.find(key);
            if (f == flags.end() || invalidFlags.count(key) != 0) {
                reasons.insert("unmatched_wait");
                return;
            }
            AddEdge(f->second, e.node, HappensBeforeGraph::Reason::TokenMatch);
            flags.erase(f);
            MergePrefix(acquired[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::AcquireSuffix);
            MergePrefix(prefix[{domain, s.dstPipe}], e.node, HappensBeforeGraph::Reason::PublishPrefix);
        } else
            reasons.insert("invalid_sync_action");
    }

    static bool IsSupportedLocalFlag(const AclsanDeviceSyncData& sync)
    {
        return sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG && ValidPipe(sync.srcPipe) &&
               ValidPipe(sync.dstPipe) &&
               (sync.scope != ACLSAN_DEVICE_SYNC_SCOPE_BLOCK || sync.mode == CROSS_CORE_MODE_SAME_PIPE) &&
               (sync.scope == ACLSAN_DEVICE_SYNC_SCOPE_CORE || sync.srcPipe != sync.dstPipe) &&
               (sync.scope == ACLSAN_DEVICE_SYNC_SCOPE_BLOCK || sync.mode <= CROSS_CORE_MODE_RECEIVER_FLAG);
    }

    void BuildLocalEdges()
    {
        std::map<Flag, size_t> flags;
        std::set<Flag> invalidFlags;
        std::map<Domain, size_t> scalarWaitGate;
        for (const auto& pair : events) {
            if (graphExhausted)
                break;
            const Domain domain = std::get<0>(pair.first);
            const auto& e = pair.second;
            const size_t begin =
                !e.sync && instructionStarts.count(pair.first) != 0 ? instructionStarts.at(pair.first) : e.node;
            if (const auto wait = scalarWaitGate.find(domain); wait != scalarWaitGate.end())
                AddEdge(wait->second, begin, HappensBeforeGraph::Reason::AcquireSuffix);
            if (!e.sync) {
                BuildMemoryEdges(domain, e, begin);
                continue;
            }
            const auto& s = sync[e.index];
            if (s.syncKind == ACLSAN_DEVICE_SYNC_KIND_UNKNOWN && s.action == ACLSAN_DEVICE_SYNC_ACTION_UNKNOWN) {
                BuildBarrierEdges(domain, e, s);
                continue;
            }
            if (s.syncKind == ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF) {
                BuildMutexEdges(domain, e, s);
                continue;
            }
            if (!IsSupportedLocalFlag(s)) {
                reasons.insert("sync_coverage_unknown");
                continue;
            }
            if (s.scope != ACLSAN_DEVICE_SYNC_SCOPE_BLOCK) {
                BuildLocalCrossCoreEdges(domain, e, s, scalarWaitGate);
                continue;
            }
            BuildBlockFlagEdges(domain, e, s, flags, invalidFlags);
        }
        if (!flags.empty())
            reasons.insert("pending_set");
        for (const auto& entry : mutexes)
            if (entry.second.get)
                reasons.insert("pending_get_buf");
    }

    void BuildMutexEdges(const Domain& domain, const Event& event, const AclsanDeviceSyncData& sync)
    {
        if (sync.scope != ACLSAN_DEVICE_SYNC_SCOPE_PIPE || !ValidPipe(sync.dstPipe) || sync.mode > 1U ||
            sync.objectId > 31U) {
            reasons.insert("sync_coverage_unknown");
            return;
        }
        auto& state = mutexes[{domain, sync.objectId}];
        const uint32_t pipe = sync.dstPipe;
        if (sync.action == ACLSAN_DEVICE_SYNC_ACTION_GET) {
            if (state.get) {
                reasons.insert("duplicate_get_buf");
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
            reasons.insert("invalid_sync_action");
            return;
        }
        if (!state.get || state.get->pipe != pipe || state.get->mode != sync.mode) {
            reasons.insert("unmatched_release_buf");
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
                    reasons.insert("graph_query_budget");
                    break;
                }
                future = future || *path;
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
                reasons.insert("graph_query_budget");
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
        if (selected != nullptr && reasons.empty()) {
            AddEdge(selected->tokens[selected->consumed++].node, wait, HappensBeforeGraph::Reason::TokenMatch);
            ++counter.consumed;
            return true;
        }
        return false;
    }

    void ResolveCrossCoreWaits()
    {
        // WAIT consumes one notification from its receiver counter, irrespective
        // of mode. Resolve using the current same-pipe prefix model and existing HB
        // paths. Iteration allows handshakes on another flag to establish order;
        // mixed-pipe issue order and callback arrival are not matching evidence.
        bool progress = true;
        while (progress && reasons.empty()) {
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
            reasons.insert(
                remaining < counter.waits.size() - counter.consumed ? "unmatched_cross_core_wait" :
                                                                      "ambiguous_cross_core_notification_order");
        }
    }

    bool SealGraph()
    {
        const bool graphComplete = graph.Seal(reasons.empty());
        if (!graphComplete) {
            reasons.insert("incomplete_sync_graph");
            return false;
        }

        return true;
    }
};

void BuildDependencyGraph(
    std::vector<AclsanDeviceMemoryAccessData>& memory, std::vector<AclsanDeviceSyncData>& sync,
    const std::map<Instruction, std::set<uint32_t>>& skippedFragments, const Limits& limits,
    std::set<std::string>& reasons, DependencyGraph& output,
    const std::vector<AclsanDeviceRegisterStateData>& registers, std::vector<AclsanDeviceVfData>& vfs,
    const std::vector<AclsanDeviceBarrierData>& localBarriers)
{
    output.graph.BeginAnalysis(limits.ReachabilityPhaseWorkBudget());
    DependencyGraphBuilder(memory, sync, skippedFragments, limits, reasons, output, registers, vfs, localBarriers)
        .Run();
}
} // namespace npucheck::race
