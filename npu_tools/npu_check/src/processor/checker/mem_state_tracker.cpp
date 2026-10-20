/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include "checker/mem_state_tracker.h"
#include <algorithm>
#include <array>
#include <limits>
#include <iterator>
#include <new>
#include <set>
#include <cstdio>
#include <cinttypes>

namespace npucheck::racecheck {
namespace {
[[noreturn]] void Invalid(const char* message) { throw RacecheckError(RacecheckErrorKind::InvalidEvent, message); }
[[noreturn]] void Unsupported(const char* message)
{
    throw RacecheckError(RacecheckErrorKind::UnsupportedEvent, message);
}
struct Axis {
    uint64_t count;
    int64_t stride;
};
uint64_t Magnitude(int64_t value)
{
    return value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);
}
__int128 Add(__int128 lhs, __int128 rhs)
{
    __int128 result;
    if (__builtin_add_overflow(lhs, rhs, &result)) {
        Invalid("layout offset overflow");
    }
    return result;
}
uint64_t Address(uint64_t base, __int128 offset)
{
    if (offset < -static_cast<__int128>(base) ||
        offset > static_cast<__int128>(std::numeric_limits<uint64_t>::max() - base)) {
        Invalid("layout address overflow or underflow");
    }
    return static_cast<uint64_t>(static_cast<__int128>(base) + offset);
}
uint64_t End(uint64_t begin, uint64_t bytes)
{
    if (bytes > std::numeric_limits<uint64_t>::max() - begin) {
        Invalid("byte range end overflow");
    }
    return begin + bytes;
}
} // namespace

PhysicalSpaceKey MakeKey(const AclsanDeviceMemoryAccessData& data)
{
    if (data.memorySpace < ACLSAN_DEVICE_MEMORY_SPACE_GM || data.memorySpace > ACLSAN_DEVICE_MEMORY_SPACE_FB) {
        Unsupported("unknown physical memory space");
    }
    const auto space = static_cast<AclsanDeviceMemorySpace>(data.memorySpace);
    if ((space == ACLSAN_DEVICE_MEMORY_SPACE_UB && data.header.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR) ||
        (space == ACLSAN_DEVICE_MEMORY_SPACE_L1 && data.header.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE)) {
        Invalid("UB requires the actual AIV core; L1 requires the actual AIC core");
    }
    return {data.header.deviceId, space, space == ACLSAN_DEVICE_MEMORY_SPACE_GM ? 0u : data.header.phyCoreId};
}

AclsanDeviceMemoryAccessMode AccessMode(const AclsanDeviceMemoryAccessData& data)
{
    switch (data.accessMode) {
        case ACLSAN_DEVICE_MEMORY_ACCESS_READ:
            return ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        case ACLSAN_DEVICE_MEMORY_ACCESS_WRITE:
            return ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD:
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN:
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX:
            return static_cast<AclsanDeviceMemoryAccessMode>(data.accessMode);
        default:
            Unsupported("unknown memory access mode");
    }
}

namespace {
std::vector<ByteRange> ExpandRanges(const AclsanDeviceMemoryAccessData& data)
{
    constexpr uint32_t incomplete = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED | ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED |
                                    ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR |
                                    ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
    if ((data.header.flags & incomplete) != 0 || (data.header.flags & ACLSAN_DEVICE_EVENT_FLAG_EXACT) == 0) {
        throw RacecheckError(
            RacecheckErrorKind::IncompleteTrace, "racecheck requires an exact, complete memory access");
    }
    constexpr uint32_t knownFlags = ACLSAN_DEVICE_EVENT_FLAG_EXACT | incomplete | ACLSAN_DEVICE_EVENT_FLAG_PREDICATED;
    if ((data.header.flags & ~knownFlags) != 0 || data.memoryFlags != 0) {
        Unsupported("unknown memory flags");
    }
    if (data.vectorMask0 != 0 || data.vectorMask1 != 0) {
        Unsupported("vector mask must be resolved upstream");
    }
    if ((data.header.flags & ACLSAN_DEVICE_EVENT_FLAG_PREDICATED) != 0 && data.predicateMask0 == 0 &&
        data.predicateMask1 == 0) {
        return {};
    }
    if (data.predicateMask0 != 0 || data.predicateMask1 != 0) {
        Unsupported("predicate mask must be resolved upstream");
    }
    if (data.accessCount > 1 && data.header.instrExecId == 0) {
        throw RacecheckError(RacecheckErrorKind::IncompleteTrace, "grouped access lacks dynamic instruction identity");
    }

    uint64_t elementBytes = 0;
    uint32_t rank = 0;
    std::array<Axis, 5> axes{};
    switch (data.layoutKind) {
        case ACLSAN_MEM_LAYOUT_SCALAR:
            elementBytes = data.layout.scalar.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_RANGE:
            elementBytes = data.layout.range.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT:
            elementBytes = data.layout.blockRepeat.blockSize;
            rank = 2;
            axes[0] = {data.layout.blockRepeat.blockNum, data.layout.blockRepeat.blockStride};
            axes[1] = {data.layout.blockRepeat.repeatTimes, data.layout.blockRepeat.repeatStride};
            break;
        case ACLSAN_MEM_LAYOUT_ND_AFFINE:
            elementBytes = data.layout.ndAffine.elementBytes;
            rank = data.layout.ndAffine.rank;
            if (rank == 0 || rank > axes.size()) {
                Invalid("affine rank must be between 1 and 5");
            }
            for (uint32_t i = 0; i < rank; ++i) {
                axes[i] = {data.layout.ndAffine.dims[i], data.layout.ndAffine.strides[i]};
            }
            break;
        default:
            Unsupported("unknown memory layout");
    }
    if (elementBytes == 0) {
        Invalid("zero-length memory element");
    }
    for (uint32_t i = 0; i < rank; ++i) {
        if (axes[i].count == 0) {
            Invalid("zero layout dimension");
        }
    }
    std::sort(axes.begin(), axes.begin() + rank, [](const Axis& lhs, const Axis& rhs) {
        return Magnitude(lhs.stride) < Magnitude(rhs.stride);
    });
    __int128 contiguousOffset = 0;
    uint32_t sparse = 0;
    for (; sparse < rank; ++sparse) {
        const auto axis = axes[sparse];
        if (axis.count == 1) {
            continue;
        }
        if (Magnitude(axis.stride) > elementBytes) {
            break;
        }
        const __int128 extent = static_cast<__int128>(axis.count - 1) * axis.stride;
        const auto magnitude =
            extent < 0 ? static_cast<unsigned __int128>(-(extent + 1)) + 1 : static_cast<unsigned __int128>(extent);
        if (magnitude > std::numeric_limits<uint64_t>::max() - elementBytes) {
            Invalid("contiguous layout size overflow");
        }
        elementBytes += static_cast<uint64_t>(magnitude);
        if (extent < 0) {
            contiguousOffset = Add(contiguousOffset, extent);
        }
    }
    size_t count = 1;
    __int128 minimum = contiguousOffset;
    __int128 maximum = contiguousOffset;
    for (uint32_t i = sparse; i < rank; ++i) {
        if (axes[i].count > std::numeric_limits<size_t>::max() / count) {
            throw RacecheckError(RacecheckErrorKind::CapacityExceeded, "layout range count overflow");
        }
        count *= axes[i].count;
        const __int128 extent = static_cast<__int128>(axes[i].count - 1) * axes[i].stride;
        if (extent < 0) {
            minimum = Add(minimum, extent);
        } else {
            maximum = Add(maximum, extent);
        }
    }
    Address(data.address, minimum);
    End(Address(data.address, maximum), elementBytes);
    std::vector<ByteRange> ranges;
    ranges.reserve(count);
    for (size_t linear = 0; linear < count; ++linear) {
        size_t remainder = linear;
        __int128 offset = contiguousOffset;
        for (uint32_t i = sparse; i < rank; ++i) {
            offset = Add(offset, static_cast<__int128>(remainder % axes[i].count) * axes[i].stride);
            remainder /= axes[i].count;
        }
        const uint64_t begin = Address(data.address, offset);
        ranges.push_back({begin, End(begin, elementBytes)});
    }
    std::sort(
        ranges.begin(), ranges.end(), [](const ByteRange& lhs, const ByteRange& rhs) { return lhs.begin < rhs.begin; });
    size_t output = 0;
    for (const auto range : ranges) {
        if (output != 0 && range.begin <= ranges[output - 1].end) {
            ranges[output - 1].end = std::max(ranges[output - 1].end, range.end);
        } else {
            ranges[output++] = range;
        }
    }
    ranges.resize(output);
    return ranges;
}
} // namespace

PreparedAccess PrepareAccess(const AclsanDeviceMemoryAccessData& data)
{
    if (data.header.size != sizeof(data)) {
        Unsupported("unsupported cbdata size");
    }
    const auto key = MakeKey(data);
    const auto mode = AccessMode(data);
    return {key, mode, ExpandRanges(data)};
}

std::vector<ByteRange> ExpandAccess(const AclsanDeviceMemoryAccessData& data) { return PrepareAccess(data).ranges; }

bool SameInstruction(const AclsanDeviceMemoryAccessData& lhs, const AclsanDeviceMemoryAccessData& rhs)
{
    if (lhs.header.instrExecId == 0 || rhs.header.instrExecId == 0) {
        return false;
    }
    return std::tie(
               lhs.header.deviceId, lhs.header.launchId, lhs.header.phyCoreId, lhs.header.blockId, lhs.header.blockType,
               lhs.header.instrExecId) ==
           std::tie(
               rhs.header.deviceId, rhs.header.launchId, rhs.header.phyCoreId, rhs.header.blockId, rhs.header.blockType,
               rhs.header.instrExecId);
}
std::vector<FailedRange> FailureRanges(const std::vector<AccessPair>& conflicts)
{
    // 生产候选按地址生成，同一历史节点的候选区间完全相同；外部判序仅筛选。
    // 顺序整理这些区间，保留该区间首次出现的原始 evidence 下标。
    std::vector<FailedRange> result;
    bool ordered = true;
    for (size_t i = 0; i < conflicts.size(); ++i) {
        const auto& conflict = conflicts[i];
        if (conflict.begin >= conflict.end) {
            ordered = false;
            break;
        }
        if (!result.empty()) {
            const auto& last = result.back();
            if (conflict.begin == last.begin && conflict.end == last.end) {
                continue;
            }
            if (conflict.begin < last.end) {
                ordered = false;
                break;
            }
        }
        result.push_back({conflict.begin, conflict.end, i});
    }
    if (ordered) {
        return result;
    }

    // 独立调用仍支持交叠、乱序输入。丢弃顺序扫描的前缀，完整重算首次证据。
    result.clear();
    struct Boundary {
        uint64_t address;
        size_t index;
        bool starts;
    };
    std::vector<Boundary> boundaries;
    for (size_t i = 0; i < conflicts.size(); ++i) {
        boundaries.push_back({conflicts[i].begin, i, true});
        boundaries.push_back({conflicts[i].end, i, false});
    }
    std::sort(boundaries.begin(), boundaries.end(), [](const Boundary& lhs, const Boundary& rhs) {
        return lhs.address < rhs.address;
    });
    std::set<size_t> active;
    for (size_t i = 0; i < boundaries.size();) {
        const uint64_t begin = boundaries[i].address;
        do {
            const auto& boundary = boundaries[i++];
            if (boundary.starts) {
                active.insert(boundary.index);
            } else {
                active.erase(boundary.index);
            }
        } while (i < boundaries.size() && boundaries[i].address == begin);
        if (i < boundaries.size() && !active.empty()) {
            result.push_back({begin, boundaries[i].address, *active.begin()});
        }
    }
    return result;
}
} // namespace npucheck::racecheck

namespace npucheck {
namespace {
[[noreturn]] void Invalid(const char* message) { throw RacecheckError(RacecheckErrorKind::InvalidEvent, message); }

// 源读先于同一动态指令的普通目标覆盖写入（例如 Add(A, B, A)。
// 这不是对相同指令 ID、原子目标或写先于读组合的通用豁免。
// 这条说明已合并到上一行。
bool IsInstructionReadBeforeWrite(
    const AclsanDeviceMemoryAccessData& previous, const AclsanDeviceMemoryAccessData& current)
{
    return previous.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ &&
           current.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE && racecheck::SameInstruction(previous, current);
}

// 状态相关的选择遵循 V2 第 2.5 节，而不是直接套用读写矩阵：
// 之前接受的访问可能覆盖普通历史记录。
template <class Visitor>
void VisitCandidates(const MemState& segment, AclsanDeviceMemoryAccessMode current, Visitor&& visitor)
{
    if (segment.state == MemOpState::Race) {
        return;
    }
    const bool atomic = racecheck::IsAtomicWrite(current);
    const bool readersCoverHistory =
        (current == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE && segment.state == MemOpState::Read) ||
        (atomic && !segment.readers.empty());
    if (readersCoverHistory) {
        for (CbIndex index : segment.readers) {
            visitor(index);
        }
        if (!atomic) {
            return;
        }
    }
    // reader 只覆盖普通 writer，不能遮蔽不兼容的原子 writer。
    const bool skipOrdinary =
        readersCoverHistory || (current == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE &&
                                segment.state == MemOpState::AtomicWrite && !segment.readers.empty());
    if (!skipOrdinary) {
        if (segment.writer) {
            visitor(*segment.writer);
        }
    }
    for (size_t group = 0; group < segment.atomicWriters.size(); ++group) {
        if (atomic && group == static_cast<size_t>(current - ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD)) {
            continue;
        }
        for (CbIndex index : segment.atomicWriters[group]) {
            visitor(index);
        }
    }
}

bool SameState(const MemState& lhs, const MemState& rhs)
{
    // tracker 不保存报告身份：保留每个 Race 边界，
    // 不合并可能具有不同首次证据的子区间。
    return lhs.state != MemOpState::Race && lhs.end == rhs.begin && lhs.state == rhs.state &&
           lhs.writer == rhs.writer && lhs.atomicWriters == rhs.atomicWriters && lhs.readers == rhs.readers;
}

bool NeedsHistory(
    uint64_t begin, uint64_t end, const std::vector<racecheck::ByteRange>& ranges,
    const std::vector<racecheck::FailedRange>& failures)
{
    const auto range = std::upper_bound(
        ranges.begin(), ranges.end(), begin,
        [](uint64_t address, const racecheck::ByteRange& candidate) { return address < candidate.begin; });
    if (range == ranges.begin() || std::prev(range)->end < end) {
        return true;
    }
    const auto failure = std::lower_bound(
        failures.begin(), failures.end(), begin,
        [](const racecheck::FailedRange& candidate, uint64_t address) { return candidate.end <= address; });
    return failure != failures.end() && failure->begin < end;
}

void DiscardReadAndAtomicHistory(MemState& segment) noexcept
{
    std::vector<CbIndex>().swap(segment.readers);
    for (auto& group : segment.atomicWriters) {
        std::vector<CbIndex>().swap(group);
    }
}

bool Split(
    SegmentTable& segments, uint64_t address, const std::vector<racecheck::ByteRange>* overwrites,
    const std::vector<racecheck::FailedRange>& failures)
{
    auto it = segments.upper_bound(address);
    if (it == segments.begin()) {
        return false;
    }
    --it;
    if (it->second.state == MemOpState::Race || address <= it->second.begin || address >= it->second.end) {
        return false;
    }
    const bool keepLeft = !overwrites || NeedsHistory(it->second.begin, address, *overwrites, failures);
    const bool keepRight = !overwrites || NeedsHistory(address, it->second.end, *overwrites, failures);
    // 两侧都保留历史时才复制；仅右侧需要时在节点分配成功后转移历史。
    MemState right = keepLeft && keepRight ? it->second : MemState{address, it->second.end, it->second.state, {}, {}};
    right.begin = address;
    auto& inserted = segments.emplace(address, std::move(right)).first->second;
    if (!keepLeft) {
        if (keepRight) {
            inserted.writer = it->second.writer;
            it->second.writer.reset();
            inserted.readers = std::move(it->second.readers);
            inserted.atomicWriters = std::move(it->second.atomicWriters);
        } else {
            it->second.writer.reset();
            DiscardReadAndAtomicHistory(it->second);
        }
    }
    it->second.end = address;
    return true;
}

void AppendUnique(std::vector<CbIndex>& history, CbIndex index, bool newIndex)
{
    // 比所有已成功更新的下标都大时，该记录不可能已存在于任何历史。
    // 旧下标重放或乱序提交仍执行完整查重，保留原插入顺序。
    if (newIndex || std::find(history.begin(), history.end(), index) == history.end()) {
        history.push_back(index);
    }
}

void Apply(MemState& segment, AclsanDeviceMemoryAccessMode mode, CbIndex index, bool newIndex)
{
    switch (mode) {
        case ACLSAN_DEVICE_MEMORY_ACCESS_READ:
            segment.state = MemOpState::Read;
            AppendUnique(segment.readers, index, newIndex);
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_WRITE:
            segment.state = MemOpState::Write;
            segment.writer = index;
            DiscardReadAndAtomicHistory(segment);
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD:
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN:
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX:
            segment.state = MemOpState::AtomicWrite;
            AppendUnique(segment.atomicWriters[mode - ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD], index, newIndex);
            break;
    }
}

} // namespace

MemStateTracker::MemStateTracker(
    const std::vector<AclsanDeviceMemoryAccessData>& records, RacecheckDiagnostics diagnostics)
    : records_(records), diagnostics_(diagnostics)
{}

void MemStateTracker::Trace(
    CbIndex index, const char* event, uint64_t begin, uint64_t end, CbIndex previous, const char* detail,
    bool error) const noexcept
{
    if (!diagnostics_.emit && !error) {
        return;
    }
    char message[1536];
    if (index < records_.size()) {
        const auto& d = records_[index];
        std::snprintf(
            message, sizeof(message),
            "racecheck event=%s level=%s phase=%s launch=%" PRIu64 " tracker_launch=%" PRIu64
            " cb=%zu device=%u space=%u table_core=%u core=%u block=%u block_type=%u pipe=%u"
            " pc=0x%" PRIx64 " instr=%" PRIu64 " mode=%u layout=%u address=0x%" PRIx64 " range=[%" PRIu64 ",%" PRIu64
            ") prev=%zu tables=%zu detail=%s\n",
            event, error ? "ERROR" : "DEBUG", error ? "failed" : "trace", d.header.launchId,
            launchId_.value_or(d.header.launchId), index, d.header.deviceId, d.memorySpace,
            d.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ? 0u : d.header.phyCoreId, d.header.phyCoreId,
            d.header.blockId, d.header.blockType, d.header.pipeline, d.header.pc, d.header.instrExecId, d.accessMode,
            d.layoutKind, d.address, begin, end, previous, tables_.size(), detail);
    } else {
        std::snprintf(
            message, sizeof(message),
            "racecheck event=%s level=%s cb=%zu tracker_bound=%u tracker_launch=%" PRIu64
            " records=%zu tables=%zu detail=%s\n",
            event, error ? "ERROR" : "DEBUG", index, launchId_.has_value() ? 1u : 0u, launchId_.value_or(0),
            records_.size(), tables_.size(), detail);
    }
    try {
        if (error && diagnostics_.emitError) {
            diagnostics_.emitError(diagnostics_.context, message);
        } else if (diagnostics_.emit) {
            diagnostics_.emit(diagnostics_.context, message);
        } else {
            std::fputs(message, stderr);
        }
    } catch (...) {
        // 诊断只用于观察，包括不会抛异常的提交完成之后。
    }
}

void MemStateTracker::TraceState(
    CbIndex index, const char* event, const MemState& state, const char* writerEvent,
    const char* readerEvent) const noexcept
{
    if (!diagnostics_.emit) {
        return;
    }
    char detail[128];
    const char* name = state.state == MemOpState::Write       ? "Write" :
                       state.state == MemOpState::Read        ? "Read" :
                       state.state == MemOpState::AtomicWrite ? "AtomicWrite" :
                                                                "Race";
    const size_t ordinaryCount = state.writer.has_value() ? 1 : 0;
    const size_t writerCount =
        ordinaryCount + state.atomicWriters[0].size() + state.atomicWriters[1].size() + state.atomicWriters[2].size();
    std::snprintf(
        detail, sizeof(detail), "state=%s writers=%zu readers=%zu ordinary=%zu add=%zu min=%zu max=%zu", name,
        writerCount, state.readers.size(), ordinaryCount, state.atomicWriters[0].size(), state.atomicWriters[1].size(),
        state.atomicWriters[2].size());
    Trace(index, event, state.begin, state.end, static_cast<CbIndex>(-1), detail);
    if (state.writer) {
        Trace(index, writerEvent, state.begin, state.end, *state.writer);
    }
    for (const auto& group : state.atomicWriters) {
        for (auto writer : group) {
            Trace(index, writerEvent, state.begin, state.end, writer);
        }
    }
    for (auto reader : state.readers) {
        Trace(index, readerEvent, state.begin, state.end, reader);
    }
}

void MemStateTracker::TraceTable(CbIndex index, const SegmentTable& table) const noexcept
{
    if (!diagnostics_.emit) {
        return;
    }
    const auto space = records_[index].memorySpace;
    constexpr std::array<const char*, 9> names = {"UNKNOWN", "GM", "UB", "L1", "L0A", "L0B", "L0C", "BT", "FB"};
    const char* name = space < names.size() ? names[space] : "UNKNOWN";
    char detail[128];
    std::snprintf(detail, sizeof(detail), "memory=%s table_segments=%zu", name, table.size());
    Trace(index, "memstate.begin", 0, 0, static_cast<CbIndex>(-1), detail);
    for (const auto& entry : table) {
        TraceState(index, "memstate.segment", entry.second, "memstate.writer", "memstate.reader");
    }
    Trace(index, "memstate.end", 0, 0, static_cast<CbIndex>(-1), detail);
}

const AclsanDeviceMemoryAccessData& MemStateTracker::Record(CbIndex index) const
{
    if (failed_) {
        throw RacecheckError(RacecheckErrorKind::IncompleteTrace, "tracker stopped after failed in-place update");
    }
    if (index >= records_.size()) {
        Invalid("cbdata index out of range");
    }
    const auto& data = records_[index];
    if (launchId_ && data.header.launchId != *launchId_) {
        Invalid("record belongs to another launch");
    }
    // 同一个 launch 中，一个物理 ID 不能同时标识 AIC 和 AIV。
    const auto opposite = data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB ? ACLSAN_DEVICE_MEMORY_SPACE_L1 :
                          data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L1 ? ACLSAN_DEVICE_MEMORY_SPACE_UB :
                                                                              ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN;
    if (opposite != ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN &&
        tables_.find({data.header.deviceId, opposite, data.header.phyCoreId}) != tables_.end()) {
        Invalid("physical core cannot own both UB and L1");
    }
    return data;
}

std::vector<AccessPair> MemStateTracker::CheckPotentialRace(CbIndex cbIndex) const
{
    return CheckPotentialRaceImpl(cbIndex, nullptr);
}

std::vector<AccessPair> MemStateTracker::CheckPotentialRace(
    CbIndex cbIndex, const racecheck::PreparedAccess& prepared) const
{
    return CheckPotentialRaceImpl(cbIndex, &prepared);
}

std::vector<AccessPair> MemStateTracker::CheckPotentialRaceImpl(
    CbIndex cbIndex, const racecheck::PreparedAccess* prepared) const
{
    Trace(cbIndex, "check.begin");
    try {
        const auto& current = Record(cbIndex);
        std::optional<racecheck::PreparedAccess> local;
        if (!prepared) {
            local.emplace(racecheck::PrepareAccess(current));
            prepared = &*local;
        }
        const auto& ranges = prepared->ranges;
        const auto& key = prepared->key;
        const auto mode = prepared->mode;
        std::vector<AccessPair> candidates;
        const auto table = tables_.find(key);
        if (table == tables_.end()) {
            Trace(cbIndex, "check.empty_table");
        }
        for (const auto range : ranges) {
            Trace(cbIndex, "check.range", range.begin, range.end);
            if (table == tables_.end()) {
                Trace(cbIndex, "check.hole", range.begin, range.end);
                continue;
            }
            uint64_t cursor = range.begin;
            const auto& segments = table->second;
            auto it = segments.lower_bound(range.begin);
            // 当前范围可能从历史节点（包括冻结 Race 节点）的内部开始。
            if (it != segments.begin() && std::prev(it)->second.end > range.begin) {
                --it;
            }
            for (; it != segments.end() && it->second.begin < range.end; ++it) {
                const auto* segment = &it->second;
                if (cursor < segment->begin) {
                    Trace(cbIndex, "check.hole", cursor, segment->begin);
                }
                cursor = std::min(range.end, segment->end);
                TraceState(cbIndex, "check.segment", *segment);
                if (segment->state == MemOpState::Race) {
                    Trace(cbIndex, "check.skip_race", std::max(range.begin, segment->begin), cursor);
                    continue;
                }
                VisitCandidates(*segment, mode, [&](CbIndex previous) {
                    if (previous == cbIndex) {
                        return;
                    }
                    if (IsInstructionReadBeforeWrite(records_[previous], current)) {
                        Trace(
                            cbIndex, "check.skip_instruction_read_write", std::max(range.begin, segment->begin),
                            std::min(range.end, segment->end), previous,
                            "WAR same_instruction=1 ordering=instruction_read_before_write");
                        return;
                    }
                    candidates.push_back(
                        {previous, cbIndex, key.memorySpace, std::max(range.begin, segment->begin),
                         std::min(range.end, segment->end)});
                    if (diagnostics_.emit) {
                        char detail[96];
                        std::snprintf(
                            detail, sizeof(detail), "same_instruction=%u ordering=external",
                            racecheck::SameInstruction(records_[previous], current) ? 1u : 0u);
                        Trace(
                            cbIndex, "check.candidate", candidates.back().begin, candidates.back().end, previous,
                            detail);
                    }
                    Trace(previous, "check.previous", candidates.back().begin, candidates.back().end, cbIndex);
                });
            }
            if (cursor < range.end) {
                Trace(cbIndex, "check.hole", cursor, range.end);
            }
        }
        char summary[64];
        std::snprintf(summary, sizeof(summary), "candidates=%zu ranges=%zu", candidates.size(), ranges.size());
        Trace(cbIndex, "check.end", 0, 0, static_cast<CbIndex>(-1), summary);
        return candidates;
    } catch (const RacecheckError& error) {
        Trace(cbIndex, "check.failed", 0, 0, static_cast<CbIndex>(-1), error.what(), true);
        throw;
    } catch (const std::bad_alloc&) {
        Trace(
            cbIndex, "check.failed", 0, 0, static_cast<CbIndex>(-1),
            "insufficient host memory; memory allocation failed; table unchanged", true);
        throw RacecheckError(
            RacecheckErrorKind::MemoryAllocationFailed, "insufficient host memory while checking potential races");
    } catch (const std::length_error&) {
        Trace(
            cbIndex, "check.failed", 0, 0, static_cast<CbIndex>(-1),
            "container maximum capacity exceeded; table unchanged", true);
        throw RacecheckError(
            RacecheckErrorKind::CapacityExceeded, "container maximum capacity exceeded while checking potential races");
    }
}

void MemStateTracker::UpdateMemState(CbIndex cbIndex, const std::vector<AccessPair>& conflicts)
{
    UpdateMemStateImpl(cbIndex, conflicts, nullptr);
}

void MemStateTracker::UpdateMemState(
    CbIndex cbIndex, const std::vector<AccessPair>& conflicts, const racecheck::PreparedAccess& prepared)
{
    UpdateMemStateImpl(cbIndex, conflicts, &prepared);
}

void MemStateTracker::UpdateMemStateImpl(
    CbIndex cbIndex, const std::vector<AccessPair>& conflicts, const racecheck::PreparedAccess* prepared)
{
    Trace(cbIndex, "update.begin");
    bool mutationStarted = false;
    auto fail = [&](const char* reason, bool discard) {
        if (discard) {
            tables_.clear();
            launchId_.reset();
            largestUpdatedIndex_.reset();
            failed_ = true;
        }
        char detail[512];
        std::snprintf(
            detail, sizeof(detail), "%s; %s", reason,
            failed_ ? "state discarded; tracker stopped; discard this launch analysis state" : "table unchanged");
        Trace(cbIndex, "update.failed", 0, 0, static_cast<CbIndex>(-1), detail, true);
    };
    try {
        const auto& current = Record(cbIndex);
        std::optional<racecheck::PreparedAccess> local;
        if (!prepared) {
            local.emplace(racecheck::PrepareAccess(current));
            prepared = &*local;
        }
        const auto& key = prepared->key;
        const auto mode = prepared->mode;
        const auto& ranges = prepared->ranges;
        const bool newIndex = !largestUpdatedIndex_ || cbIndex > *largestUpdatedIndex_;
        // conflicts 是本次候选经外部判序得到的不变子集；表尚未改变。
        // 保留身份和范围检查，不再逐条查询并扫描历史成员。
        for (const auto& pair : conflicts) {
            Trace(cbIndex, "update.conflict", pair.begin, pair.end, pair.prevIndex);
            const auto& previous = Record(pair.prevIndex);
            const auto previousKey = racecheck::MakeKey(previous);
            if (pair.curIndex != cbIndex || pair.prevIndex == cbIndex || pair.memorySpace != key.memorySpace ||
                key < previousKey || previousKey < key || pair.begin >= pair.end) {
                Invalid("conflict identity, space or range mismatch");
            }
            if (IsInstructionReadBeforeWrite(previous, current)) {
                Invalid("same-instruction source read and destination write are not conflict candidates");
            }
            auto range = std::upper_bound(
                ranges.begin(), ranges.end(), pair.begin,
                [](uint64_t begin, const racecheck::ByteRange& candidate) { return begin < candidate.begin; });
            if (range == ranges.begin() || std::prev(range)->end < pair.end) {
                Invalid("conflict is not contained in an actual current access range");
            }
        }
        if (ranges.empty()) {
            Trace(cbIndex, "update.no_access");
            return;
        }

        // 所有模式直接复用原表。失败后停止并丢弃分析状态，不保留回滚副本。
        const auto failures = racecheck::FailureRanges(conflicts);
        mutationStarted = true;
        const auto inserted = tables_.try_emplace(key);
        auto& working = inserted.first->second;
        if (inserted.second) {
            Trace(cbIndex, "update.create_table");
        }
        const auto* overwrites = mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE ? &ranges : nullptr;
        for (const auto range : ranges) {
            if (Split(working, range.begin, overwrites, failures)) {
                Trace(cbIndex, "update.split", range.begin, range.begin);
            }
            if (Split(working, range.end, overwrites, failures)) {
                Trace(cbIndex, "update.split", range.end, range.end);
            }
        }
        for (const auto& failure : failures) {
            if (Split(working, failure.begin, overwrites, failures)) {
                Trace(cbIndex, "update.split_conflict", failure.begin, failure.begin);
            }
            if (Split(working, failure.end, overwrites, failures)) {
                Trace(cbIndex, "update.split_conflict", failure.end, failure.end);
            }
        }

        size_t failureIndex = 0;
        for (const auto range : ranges) {
            uint64_t cursor = range.begin;
            auto it = working.lower_bound(cursor);
            // 已有 Race 节点保留原始边界，且可能从当前区间之前开始。
            // 跳过时要把这个前驱节点纳入考虑。
            if (it != working.begin() && std::prev(it)->second.end > cursor) {
                --it;
            }
            while (cursor < range.end) {
                if (it == working.end() || it->second.begin > cursor) {
                    const uint64_t end = it == working.end() ? range.end : std::min(range.end, it->second.begin);
                    Trace(cbIndex, "update.hole", cursor, end);
                    it = working.emplace(cursor, MemState{cursor, end, MemOpState::Read, {}, {}}).first;
                }
                auto& segment = it->second;
                while (failureIndex < failures.size() && failures[failureIndex].end <= cursor) {
                    ++failureIndex;
                }
                if (segment.state != MemOpState::Race) {
                    if (failureIndex < failures.size() && failures[failureIndex].begin <= cursor &&
                        cursor < failures[failureIndex].end) {
                        segment.state = MemOpState::Race;
                        const auto& evidence = conflicts[failures[failureIndex].evidence];
                        char detail[96];
                        std::snprintf(detail, sizeof(detail), "evidence_order=%zu", failures[failureIndex].evidence);
                        Trace(cbIndex, "race.first", segment.begin, segment.end, evidence.prevIndex, detail);
                        TraceState(cbIndex, "race.frozen", segment);
                    } else {
                        if (mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) {
                            // 普通写会替换全部历史：保留首节点，直接删除其余安全覆盖节点，
                            // 最后只应用一次。当前访问内的未记录空洞也由该节点覆盖。
                            // Split 已切开非 Race 节点；新冲突和已有 Race 都不能跨越。
                            uint64_t end = range.end;
                            if (failureIndex < failures.size()) {
                                end = std::min(end, failures[failureIndex].begin);
                            }
                            auto next = std::next(it);
                            while (next != working.end() && next->second.begin < end &&
                                   next->second.state != MemOpState::Race) {
                                next = working.erase(next);
                            }
                            if (next != working.end()) {
                                end = std::min(end, next->second.begin);
                            }
                            segment.end = end;
                        }
                        Apply(segment, mode, cbIndex, newIndex);
                        TraceState(cbIndex, "update.applied", segment);
                    }
                } else {
                    Trace(cbIndex, "update.skip_race", cursor, std::min(range.end, segment.end));
                }
                cursor = segment.end;
                ++it;
            }
        }
        // 仅合并本次触及的节点及两侧邻居；Race 状态保持原始边界。
        for (const auto range : ranges) {
            auto it = working.lower_bound(range.begin);
            if (it != working.begin()) {
                --it;
            }
            while (it != working.end()) {
                auto next = std::next(it);
                if (next == working.end() || next->second.begin > range.end) {
                    break;
                }
                if (SameState(it->second, next->second)) {
                    Trace(cbIndex, "update.merge", it->second.begin, next->second.end);
                    it->second.end = next->second.end;
                    working.erase(next);
                } else {
                    ++it;
                }
            }
        }
        launchId_ = current.header.launchId;
        if (newIndex) {
            largestUpdatedIndex_ = cbIndex;
        }
        Trace(cbIndex, "update.commit");
        TraceTable(cbIndex, tables_.at(key));
    } catch (const RacecheckError& error) {
        fail(error.what(), mutationStarted);
        throw;
    } catch (const std::bad_alloc&) {
        fail("insufficient host memory; memory allocation failed", true);
        throw RacecheckError(
            RacecheckErrorKind::MemoryAllocationFailed, "insufficient host memory while updating memory state");
    } catch (const std::length_error&) {
        fail("container maximum capacity exceeded", true);
        throw RacecheckError(
            RacecheckErrorKind::CapacityExceeded, "container maximum capacity exceeded while updating memory state");
    } catch (...) {
        fail("unexpected failure during memory state update", mutationStarted);
        throw;
    }
}
} // namespace npucheck
