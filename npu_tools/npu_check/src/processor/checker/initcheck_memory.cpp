/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/initcheck_memory.h"
#include "checker/initcheck_report.h"
#include "checker/racecheck/happens_before_graph.h"
#include "checker/racecheck/types.h"
#include <memory>
#include <optional>
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <limits>
#include <set>

namespace npucheck {
namespace {
// Capacity of each L0A or L0B space in bytes, not their combined capacity.
constexpr size_t kLocalBytes = 64 * 1024;
// Maximum number of queued memory, synchronization and barrier records.
constexpr size_t kMaxPendingEvents = 100000;
// Byte set within one L0A/L0B space: bit n represents byte offset n.
// The access record selects L0A or L0B; an operand's set bits describe the
// bytes it accesses, not whether those bytes have been initialized.
using Footprint = std::bitset<kLocalBytes>;
// Match set/wait flags by (source pipeline, destination pipeline, flag ID).
using FlagKey = std::tuple<uint32_t, uint32_t, uint64_t>;

bool IsLocal(uint32_t space)
{
    // L0A and L0B are the two local-memory spaces tracked by this checker.
    return space == ACLSAN_DEVICE_MEMORY_SPACE_L0A || space == ACLSAN_DEVICE_MEMORY_SPACE_L0B;
}
// Only L0A/L0B operands reach replay: map their spaces to the two state entries.
size_t LocalMemoryIndex(uint32_t space)
{
    assert(IsLocal(space));
    return space == ACLSAN_DEVICE_MEMORY_SPACE_L0A ? 0 : 1;
}
bool Exact(const AclsanDeviceEventHeader& header) { return header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT; }

bool HasValidFragmentMetadata(
    const AclsanDeviceMemoryAccessData& data, const AclsanDeviceMemoryAccessData& first, size_t fragmentCount)
{
    return Exact(data.header) && data.accessCount == fragmentCount && data.accessCount == first.accessCount &&
           data.accessIndex < data.accessCount;
}

bool HasMatchingInstructionContext(const AclsanDeviceMemoryAccessData& data, const AclsanDeviceMemoryAccessData& first)
{
    return data.header.pipeline == first.header.pipeline && data.header.pc == first.header.pc &&
           data.header.siteId == first.header.siteId && data.instructionId == first.instructionId &&
           data.header.phyCoreId == first.header.phyCoreId;
}

bool HasSupportedMemoryAccess(const AclsanDeviceMemoryAccessData& data)
{
    return data.memorySpace > ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN && data.memorySpace <= ACLSAN_DEVICE_MEMORY_SPACE_FB &&
           data.header.blockType <= ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE && data.accessMode >= 1 &&
           data.accessMode <= 3 && data.unitFlag == 0 && data.memoryFlags == 0 && data.vectorMask0 == 0 &&
           data.vectorMask1 == 0 && data.predicateMask0 == 0 && data.predicateMask1 == 0;
}

// Validate the entire affine descriptor before committing any of its bytes. Signed
// 128-bit arithmetic prevents wrapped negative strides from initializing unrelated bytes.
bool DecodeFootprint(const AclsanDeviceMemoryAccessData& data, Footprint& bytes)
{
    // Replay validates unsupported vector masks before calling this helper;
    // L0A/L0B byte ranges are expanded from the layout below.
    if (data.memoryFlags != 0 || data.predicateMask0 != 0 || data.predicateMask1 != 0) {
        return false;
    }
    // Contiguous bytes per element/block; each axis is (count, byte stride).
    uint64_t width = 0;
    std::vector<std::pair<uint64_t, int64_t>> axes;
    switch (data.layoutKind) {
        case ACLSAN_MEM_LAYOUT_SCALAR:
            width = data.layout.scalar.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_RANGE:
            width = data.layout.range.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT: {
            const auto& d = data.layout.blockRepeat;
            width = d.blockSize;
            axes = {{d.blockNum, d.blockStride}, {d.repeatTimes, d.repeatStride}};
            break;
        }
        case ACLSAN_MEM_LAYOUT_ND_AFFINE: {
            const auto& d = data.layout.ndAffine;
            if (d.rank == 0 || d.rank > 5) {
                return false;
            }
            width = d.elementBytes;
            for (uint32_t i = 0; i < d.rank; ++i) {
                axes.emplace_back(d.dims[i], d.strides[i]);
            }
            break;
        }
        default:
            return false;
    }
    if (width == 0) {
        return true;
    }
    for (const auto& axis : axes) {
        if (axis.first == 0) {
            return true;
        }
    }
    if (width > kLocalBytes || data.address >= kLocalBytes) {
        return false;
    }
    __int128 low = data.address;
    __int128 high = data.address;
    for (const auto& axis : axes) {
        const __int128 displacement = static_cast<__int128>(axis.first - 1) * axis.second;
        if (displacement < -static_cast<__int128>(kLocalBytes) || displacement > kLocalBytes) {
            return false;
        }
        if (displacement < 0) {
            low += displacement;
        } else {
            high += displacement;
        }
    }
    if (low < 0 || high + width > kLocalBytes) {
        return false;
    }
    // Expand the union by doubling runs of affine offsets. This bounds work by
    // the local-memory bitmap size even when billions of segments overlap.
    Footprint starts;
    starts.set(static_cast<size_t>(data.address));
    const auto expand = [](Footprint value, uint64_t count, int64_t stride) {
        uint64_t covered = 1;
        while (covered < count && stride != 0) {
            const uint64_t extra = std::min(covered, count - covered);
            const __int128 offset = static_cast<__int128>(extra) * stride;
            if (offset < 0) {
                value |= value >> static_cast<size_t>(-offset);
            } else {
                value |= value << static_cast<size_t>(offset);
            }
            covered += extra;
        }
        return value;
    };
    for (const auto& axis : axes) {
        starts = expand(starts, axis.first, axis.second);
    }
    bytes = expand(starts, width, 1);
    return true;
}

// Only accepts L0A/L0B accesses, already validated when replay builds operands.
NpuCheckInitcheckReport BuildL0ReadReport(
    const AclsanDeviceMemoryAccessData& data, const Footprint& bytes, uint64_t first, uint64_t missing)
{
    assert(IsLocal(data.memorySpace));
    auto report = BuildInitcheckMemoryReportBase(data, kLocalBytes, data.header.deviceId);
    report.common.exec.offset = data.header.pc;
    report.access.accessMode = static_cast<NpuCheckReportAccessMode>(data.accessMode);
    report.common.pattern = missing == bytes.count() ? NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ :
                                                       NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ;
    report.access.accessBytes = static_cast<uint32_t>(bytes.count());
    size_t begin = 0;
    size_t end = kLocalBytes;
    while (begin < end && !bytes[begin]) {
        ++begin;
    }
    while (end > begin && !bytes[end - 1]) {
        --end;
    }
    report.access.rangeBegin = begin;
    report.access.rangeEnd = end;
    report.firstUninitAddress = first;
    report.firstUninitOffset = first;
    report.uninitBytes = missing;
    report.initializedBytes = bytes.count() - missing;
    return report;
}

// One lazy 64-KB leaf per L0 space on dav-3510. Visibility evidence is kept
// separately in the dependency graph: a topological rank alone proves nothing.
class MemStatus {
public:
    static constexpr uint8_t kDefinedBit = 1U << 4;
    static constexpr uint8_t kPipelineMask = 0x0FU;

    uint8_t Get(size_t offset) const { return leaf_ ? (*leaf_)[offset] : 0; }
    void Write(const Footprint& bytes, uint32_t pipe)
    {
        assert((pipe & ~kPipelineMask) == 0);
        if (bytes.none()) {
            return;
        }
        if (!leaf_) {
            leaf_ = std::make_unique<std::array<uint8_t, kLocalBytes>>();
        }
        for (size_t i = 0; i < kLocalBytes; ++i) {
            if (bytes[i]) {
                (*leaf_)[i] = static_cast<uint8_t>(kDefinedBit | pipe);
            }
        }
    }

private:
    // Each entry describes one L0 byte. Bits 3..0 store AclsanDevicePipeline:
    // 0=S, 1=V, 2=M, 3=MTE1, 4=MTE2, 5=MTE3, 10=FIXPIPE.
    // Bit 4 is set after a committed write; zero means uninitialized.
    // Bits 7..5 are unused. Visibility is proven by the dependency graph.
    std::unique_ptr<std::array<uint8_t, kLocalBytes>> leaf_;
};
} // namespace

void InitcheckMemory::Launch(const AclsanLaunchData& data)
{
    if (data.common.result == 0) {
        streams_[data.launchId] = data.stream;
    }
}
void InitcheckMemory::Queue(const AclsanDeviceEventHeader& h, Event event)
{
    if (queued_ >= kMaxPendingEvents) {
        Invalidate();
        return;
    }
    pending_[{h.deviceId, h.launchId, h.blockType, h.blockId}].push_back(std::move(event));
    ++queued_;
}
void InitcheckMemory::Access(const AclsanDeviceMemoryAccessData& data)
{
    // Older register-dependency-only callbacks have no memory endpoints.
    if (data.accessCount == 0 && !IsLocal(data.memorySpace)) {
        return;
    }
    Queue(data.header, data);
    if (IsLocal(data.memorySpace)) {
        ++operations_;
    }
}
void InitcheckMemory::Sync(const AclsanDeviceSyncData& data) { Queue(data.header, race::NormalizeSync(data)); }
void InitcheckMemory::Barrier(const AclsanDeviceBarrierData& data) { Queue(data.header, data); }
void InitcheckMemory::Finalize()
{
    if (!pending_.empty()) {
        ++incomplete_;
    }
    pending_.clear();
    streams_.clear();
    queued_ = 0;
}
void InitcheckMemory::Collect(void* stream, CheckerReportList& reports)
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        const auto launch = std::get<1>(it->first);
        const auto owner = streams_.find(launch);
        if (owner != streams_.end() && owner->second != stream) {
            ++it;
            continue;
        }
        // Synchronization-only blocks belong to the GM/L1/L0C replay. Without
        // an L0 instruction, they do not require an L0 launch context.
        const bool hasMemory = std::any_of(it->second.begin(), it->second.end(), [](const auto& event) {
            return std::holds_alternative<AclsanDeviceMemoryAccessData>(event);
        });
        if (hasMemory && (invalid_ || owner == streams_.end())) {
            ++incomplete_;
        } else if (hasMemory) {
            try {
                Replay(it->second, reports);
            } catch (const std::bad_alloc&) {
                ++incomplete_;
            }
        }
        queued_ -= it->second.size();
        it = pending_.erase(it);
    }
    for (auto it = streams_.begin(); it != streams_.end();) {
        if (it->second == stream) {
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
}

void InitcheckMemory::Replay(const std::vector<Event>& records, CheckerReportList& reports)
{
    using Graph = race::HappensBeforeGraph;
    const race::Limits limits;
    struct Operand {
        const AclsanDeviceMemoryAccessData* data;
        Footprint bytes;
        size_t node;
    };
    struct Instruction {
        size_t node;
        std::vector<const Event*> fragments;
        std::vector<size_t> operands;
    };
    Graph graph(limits.edges, limits.queryWork, limits.queryCache);
    std::map<uint64_t, Instruction> instructions;
    std::vector<Operand> operands;
    // Match the existing replay rule: a transfer rejected by Memcheck must
    // neither report another read error nor initialize its L0 destination.
    using InvalidInstruction =
        std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t, uint32_t>;
    std::set<InvalidInstruction> invalidInstructions;
    for (const auto& report : reports) {
        const auto* bounds = std::get_if<NpuCheckMemcheckReport>(&report);
        if (!bounds || bounds->common.tool != ReportTool::MEMCHECK ||
            !(bounds->common.flags & kNpuCheckReportCommonHasExecContext) ||
            (bounds->common.pattern != NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS &&
             bounds->common.pattern != NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE)) {
            continue;
        }
        const auto& e = bounds->common.exec;
        invalidInstructions.emplace(
            e.deviceId, e.launchId, e.blockType, e.blockId, e.phyCoreId, e.instrExecId, e.pipeId, e.pc, e.siteId);
    }
    for (const auto& record : records) {
        const auto id = std::visit([](const auto& value) { return value.header.instrExecId; }, record);
        auto [entry, inserted] = instructions.try_emplace(id);
        if (inserted) {
            entry->second.node = graph.AddNode();
        }
        entry->second.fragments.push_back(&record);
    }

    // Validate complete instruction groups before emitting any errors. An unknown
    // footprint anywhere in this block may hide an initialization source.
    for (auto& [id, instruction] : instructions) {
        if (!id) {
            ++incomplete_;
            return;
        }
        const auto* first = std::get_if<AclsanDeviceMemoryAccessData>(instruction.fragments.front());
        if (!first) {
            if (instruction.fragments.size() != 1) {
                ++incomplete_;
                return;
            }
            continue;
        }
        const auto& h = first->header;
        const bool excluded = invalidInstructions.count(
                                  {h.deviceId, h.launchId, h.blockType, h.blockId, h.phyCoreId, h.instrExecId,
                                   h.pipeline, h.pc, h.siteId}) != 0;
        bool valid = true;
        std::set<uint32_t> indices;
        for (const auto* record : instruction.fragments) {
            const auto* data = std::get_if<AclsanDeviceMemoryAccessData>(record);
            if (!data) {
                valid = false;
                continue;
            }
            bool validFragment = HasValidFragmentMetadata(*data, *first, instruction.fragments.size());
            if (validFragment) {
                validFragment = indices.insert(data->accessIndex).second;
            }
            valid &= validFragment && HasMatchingInstructionContext(*data, *first) && HasSupportedMemoryAccess(*data);
            // Coverage is validated even for an instruction rejected by Memcheck.
            if (!IsLocal(data->memorySpace) || excluded) {
                continue;
            }
            if (operands.size() >= limits.stateBytes / sizeof(Operand)) {
                ++incomplete_;
                return;
            }
            operands.push_back({data, {}, instruction.node});
            instruction.operands.push_back(operands.size() - 1);
            valid &= race::ValidPipe(data->header.pipeline) && data->accessMode >= 1 && data->accessMode <= 3 &&
                     data->header.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR &&
                     DecodeFootprint(*data, operands.back().bytes);
        }
        if (!valid) {
            ++incomplete_;
            return;
        }
    }
    if (operands.empty()) {
        return;
    }

    // Per-pipeline graph nodes: prefix collects issued operations, acquired
    // records waits/barriers, and ordered tracks the last S/V/M operation.
    std::map<uint32_t, std::optional<size_t>> prefix, acquired, ordered;
    std::map<FlagKey, size_t> flags;
    bool valid = true;
    const auto edge = [&](size_t from, size_t to, Graph::Reason reason) { valid &= graph.AddEdge(from, to, reason); };
    const auto merge = [&](std::optional<size_t>& aggregate, size_t node) {
        if (!aggregate) {
            aggregate = node;
        } else if (*aggregate != node && valid) {
            const size_t join = graph.AddNode();
            edge(*aggregate, join, Graph::Reason::PublishPrefix);
            edge(node, join, Graph::Reason::PublishPrefix);
            aggregate = join;
        }
    };
    for (const auto& [id, instruction] : instructions) {
        (void)id;
        const auto node = instruction.node;
        const auto* record = instruction.fragments.front();
        if (const auto* data = std::get_if<AclsanDeviceMemoryAccessData>(record)) {
            const auto pipe = data->header.pipeline;
            if (!race::ValidPipe(pipe)) {
                valid = false;
                break;
            }
            // S/V/M complete in order; DMA/FIX issue order does not prove completion.
            if (pipe <= ACLSAN_DEVICE_PIPE_MATRIX) {
                if (ordered[pipe]) {
                    edge(*ordered[pipe], node, Graph::Reason::PipelineOrder);
                }
                ordered[pipe] = node;
            }
            if (acquired[pipe]) {
                edge(*acquired[pipe], node, Graph::Reason::AcquireSuffix);
            }
            merge(prefix[pipe], node);
        } else if (const auto* barrier = std::get_if<AclsanDeviceBarrierData>(record)) {
            if ((barrier->header.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE && !Exact(barrier->header)) ||
                (barrier->pipe != ACLSAN_DEVICE_PIPE_ALL && !race::ValidPipe(barrier->pipe))) {
                valid = false;
                break;
            }
            for (uint32_t pipe : {0U, 1U, 2U, 3U, 4U, 5U, 10U}) {
                if (barrier->pipe != ACLSAN_DEVICE_PIPE_ALL && barrier->pipe != pipe) {
                    continue;
                }
                if (prefix[pipe]) {
                    edge(*prefix[pipe], node, Graph::Reason::BarrierPrefix);
                }
                acquired[pipe] = node;
                prefix[pipe] = node;
            }
        } else {
            const auto& sync = std::get<AclsanDeviceSyncData>(*record);
            if (sync.header.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE && !Exact(sync.header)) {
                valid = false;
                break;
            }
            if (sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF) {
                continue; // This protocol does not publish L0A/L0B data.
            }
            // Inter-core records retain their observed execution order. L0
            // state is block-local, so no cross-core notification graph is needed.
            if (sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG && sync.scope >= ACLSAN_DEVICE_SYNC_SCOPE_CORE &&
                sync.scope <= ACLSAN_DEVICE_SYNC_SCOPE_SOC) {
                continue;
            }
            if (sync.syncKind != ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG ||
                sync.scope != ACLSAN_DEVICE_SYNC_SCOPE_BLOCK || sync.mode != 0 || !race::ValidPipe(sync.srcPipe) ||
                !race::ValidPipe(sync.dstPipe)) {
                valid = false;
                break;
            }
            const FlagKey key{sync.srcPipe, sync.dstPipe, sync.objectId};
            if (sync.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
                if (!flags.emplace(key, node).second) {
                    valid = false;
                    break;
                }
                if (prefix[sync.srcPipe]) {
                    edge(*prefix[sync.srcPipe], node, Graph::Reason::PublishPrefix);
                }
            } else if (sync.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
                const auto found = flags.find(key);
                if (found == flags.end()) {
                    valid = false;
                    break;
                }
                edge(found->second, node, Graph::Reason::TokenMatch);
                flags.erase(found);
                merge(acquired[sync.dstPipe], node);
                merge(prefix[sync.dstPipe], node);
            } else {
                valid = false;
                break;
            }
        }
    }
    if (!graph.Seal(valid)) {
        ++incomplete_;
        return;
    }

    std::map<size_t, const Instruction*> nodes;
    for (const auto& item : instructions) {
        nodes[item.second.node] = &item.second;
    }
    // memory[0] tracks L0A and memory[1] tracks L0B; LocalMemoryIndex keeps
    // this mapping explicit instead of relying on enum values.
    std::array<MemStatus, 2> memory;
    // Indices into operands: accepted writes and writes with uncertain validity.
    std::set<size_t> committed, uncertainWrites;
    size_t comparisons = 0;
    for (const auto node : graph.Order()) {
        const auto found = nodes.find(node);
        if (found == nodes.end()) {
            continue;
        }
        const auto& instruction = *found->second;
        bool passed = true, uncertainInstruction = false;
        for (const auto index : instruction.operands) {
            const auto& read = operands[index];
            const auto& data = *read.data;
            if ((data.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_READ) == 0 || read.bytes.none()) {
                continue;
            }
            // Bytes covered by proven preceding writes, or by uncertain writes.
            Footprint defined, uncertain;
            for (size_t w = 0; w < operands.size(); ++w) {
                const auto& write = operands[w];
                if (++comparisons > limits.comparisons) {
                    ++incomplete_;
                    return;
                }
                if (write.node == node || write.data->memorySpace != data.memorySpace ||
                    (write.data->accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) == 0 ||
                    (write.bytes & read.bytes).none()) {
                    continue;
                }
                const auto relation = graph.Compare(write.node, node);
                if (relation == Graph::Relation::Before && committed.count(w)) {
                    defined |= write.bytes;
                } else if (relation == Graph::Relation::Before && uncertainWrites.count(w)) {
                    uncertain |= write.bytes;
                } else if (relation == Graph::Relation::Unordered || relation == Graph::Relation::Unknown) {
                    uncertain |= write.bytes;
                }
            }
            const Footprint missing = read.bytes & ~defined;
            if ((missing & uncertain).any()) {
                ++incomplete_;
                passed = false;
                uncertainInstruction = true;
                continue;
            }
            const auto space = LocalMemoryIndex(data.memorySpace);
            size_t firstMissing = kLocalBytes;
            uint64_t missingCount = 0;
            for (size_t i = 0; i < kLocalBytes; ++i) {
                if (!read.bytes[i]) {
                    continue;
                }
                if ((memory[space].Get(i) & MemStatus::kDefinedBit) == 0 || missing[i]) {
                    ++missingCount;
                    firstMissing = std::min(firstMissing, i);
                }
            }
            if (missingCount) {
                reports.emplace_back(BuildL0ReadReport(data, read.bytes, firstMissing, missingCount));
                passed = false;
            }
        }
        if (!passed) {
            if (uncertainInstruction) {
                for (const auto index : instruction.operands) {
                    if ((operands[index].data->accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) != 0) {
                        uncertainWrites.insert(index);
                    }
                }
            }
            continue;
        }
        for (const auto index : instruction.operands) {
            const auto& write = operands[index];
            if ((write.data->accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) != 0) {
                memory[LocalMemoryIndex(write.data->memorySpace)].Write(write.bytes, write.data->header.pipeline);
                committed.insert(index);
            }
        }
    }
}
} // namespace npucheck
