/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/racecheck/types.h"
#include "device_instr/common/instruction_id.h"
#include "acl_san/aclsan_api.h"
#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

namespace npucheck::race {
bool ValidPipe(uint32_t p) { return p <= ACLSAN_DEVICE_PIPE_MTE3 || p == ACLSAN_DEVICE_PIPE_FIXPIPE; }
AclsanDeviceSyncData NormalizeSync(const AclsanDeviceSyncData& data)
{
    auto sync = data;
    if (sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE || sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK) {
        const bool intraBlock = sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK;
        const uint32_t pipe = sync.action == ACLSAN_DEVICE_SYNC_ACTION_SET ? sync.srcPipe : sync.dstPipe;
        sync.header.sourceKind = ACLSAN_DEVICE_SOURCE_SET_WAIT_FLAG;
        sync.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
        sync.scope = ACLSAN_DEVICE_SYNC_SCOPE_CORE;
        // SET/WAIT expose only their selected pipe; INTRA_BLOCK WAIT also omits mode.
        sync.srcPipe = pipe;
        sync.dstPipe = pipe;
        if (intraBlock)
            sync.mode = ACLSAN_DEVICE_SYNC_MODE_SINGLE_AIV;
        else if (sync.action == ACLSAN_DEVICE_SYNC_ACTION_WAIT)
            sync.objectId &= 0x1FU;
    }
    return sync;
}

void MergeIntervals(std::vector<Interval>& ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.begin < b.begin; });
    size_t n = 0;
    for (auto r : ranges) {
        if (n != 0 && r.begin <= ranges[n - 1].end)
            ranges[n - 1].end = std::max(ranges[n - 1].end, r.end);
        else
            ranges[n++] = r;
    }
    ranges.resize(n);
}
bool Normalize(const AclsanDeviceMemoryAccessData& access, size_t budget, std::vector<Interval>& result)
{
    result.clear();
    __extension__ using Wide = __int128;
    std::vector<std::pair<uint64_t, int64_t>> axes;
    uint64_t bytes = 0;
    switch (access.layoutKind) {
        case ACLSAN_MEM_LAYOUT_SCALAR:
            bytes = access.layout.scalar.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_RANGE:
            bytes = access.layout.range.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT:
            bytes = access.layout.blockRepeat.blockSize;
            axes = {
                {access.layout.blockRepeat.repeatTimes, access.layout.blockRepeat.repeatStride},
                {access.layout.blockRepeat.blockNum, access.layout.blockRepeat.blockStride}};
            break;
        case ACLSAN_MEM_LAYOUT_ND_AFFINE:
            if (access.layout.ndAffine.rank == 0 ||
                access.layout.ndAffine.rank > std::size(access.layout.ndAffine.dims))
                return false;
            bytes = access.layout.ndAffine.elementBytes;
            for (uint32_t i = 0; i < access.layout.ndAffine.rank; ++i)
                axes.emplace_back(access.layout.ndAffine.dims[i], access.layout.ndAffine.strides[i]);
            break;
        default:
            return false;
    }
    if (bytes == 0 || std::any_of(axes.begin(), axes.end(), [](auto axis) { return axis.first == 0; }))
        return true;
    size_t count = 1;
    for (auto axis : axes) {
        if (axis.first > budget / count)
            return false;
        count *= axis.first;
    }
    if (count > budget)
        return false;
    const Wide max = std::numeric_limits<uint64_t>::max();
    // Validate the full Cartesian footprint before coalescing. Checking each
    // signed extremum independently avoids cancellation hiding an invalid
    // address, and keeps the running bounds within uint64_t even for INT64_MIN.
    Wide lower = access.address, upper = access.address;
    std::vector<std::pair<uint64_t, uint64_t>> forwardAxes;
    for (const auto& axis : axes) {
        const Wide extent = static_cast<Wide>(axis.first - 1) * axis.second;
        if (extent < 0) {
            if (-extent > lower)
                return false;
            lower += extent;
        } else {
            if (extent > max - upper)
                return false;
            upper += extent;
        }
        const uint64_t step = axis.second < 0 ? static_cast<uint64_t>(-static_cast<Wide>(axis.second)) :
                                                static_cast<uint64_t>(axis.second);
        forwardAxes.emplace_back(axis.first, step);
    }
    if (bytes > max - upper)
        return false;
    // Negative axes now start at their lowest address. Axis order does not
    // change a Cartesian footprint; smallest steps expose dense/overlapping
    // runs first. An axis is folded only when its translated intervals touch
    // or overlap, so gaps in a sparse matrix never become accessed bytes.
    std::sort(forwardAxes.begin(), forwardAxes.end(), [](auto x, auto y) { return x.second < y.second; });
    std::vector<std::pair<uint64_t, uint64_t>> sparseAxes;
    for (const auto& axis : forwardAxes) {
        if (axis.first == 1)
            continue;
        if (axis.second <= bytes) {
            const Wide growth = static_cast<Wide>(axis.first - 1) * axis.second;
            if (growth > max - bytes)
                return false;
            bytes += static_cast<uint64_t>(growth);
        } else {
            sparseAxes.push_back(axis);
        }
    }
    std::function<bool(size_t, Wide)> visit = [&](size_t d, Wide offset) {
        if (d == sparseAxes.size()) {
            if (offset < 0 || offset > max || bytes > max - offset)
                return false;
            result.push_back({static_cast<uint64_t>(offset), static_cast<uint64_t>(offset + bytes)});
            return true;
        }
        for (uint64_t i = 0; i < sparseAxes[d].first; ++i)
            if (!visit(d + 1, offset + static_cast<Wide>(i) * sparseAxes[d].second))
                return false;
        return true;
    };
    if (!visit(0, lower)) {
        result.clear();
        return false;
    }
    MergeIntervals(result);
    return true;
}
Storage StorageOf(const AclsanDeviceMemoryAccessData& access)
{
    return {
        access.memorySpace, access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ?
                                Domain{access.header.deviceId, 0, 0, 0} :
                                DomainOf(access.header)};
}

std::map<Instruction, Instruction> RemoteFixpipeOrigins(
    const std::vector<AclsanDeviceMemoryAccessData>& memory, std::set<std::string>& reasons)
{
    using Identity = std::tuple<uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, uint32_t, uint32_t>;
    const auto identity = [](const AclsanDeviceMemoryAccessData& access) {
        const auto& h = access.header;
        const uint32_t aicBlock = h.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR ? h.blockId / 2U : h.blockId;
        return Identity{h.launchId, h.deviceId, aicBlock, h.instrExecId, h.pc, h.siteId, access.instructionId};
    };
    const auto isFixToUb = [](const AclsanDeviceMemoryAccessData& access) {
        return access.header.pipeline == ACLSAN_DEVICE_PIPE_FIXPIPE &&
               (access.instructionId == static_cast<uint32_t>(aclsan::InstructionId::FixL0cToUbufF32) ||
                access.instructionId == static_cast<uint32_t>(aclsan::InstructionId::FixL0cToUbufS32));
    };
    std::map<Identity, Instruction> origins;
    std::set<Identity> ambiguous;
    for (const auto& access : memory) {
        if (!isFixToUb(access) || access.header.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE ||
            access.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_L0C ||
            access.accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
            continue;
        }
        const auto inserted = origins.emplace(identity(access), Key(access.header));
        if (!inserted.second && inserted.first->second != Key(access.header)) {
            ambiguous.insert(identity(access));
        }
    }
    std::map<Instruction, Instruction> result;
    for (const auto& access : memory) {
        if (!isFixToUb(access) || access.header.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR ||
            access.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_UB) {
            continue;
        }
        const auto origin = origins.find(identity(access));
        if (origin == origins.end() || ambiguous.count(identity(access)) != 0 ||
            access.header.blockId / 2U != std::get<2>(std::get<0>(origin->second)) ||
            access.accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_WRITE || access.unitFlag != 0) {
            reasons.insert("missing_fixpipe_origin");
            continue;
        }
        result.emplace(Key(access.header), origin->second);
    }
    return result;
}
UnitResetHistory UnitFlagResets(
    const std::vector<AclsanDeviceRegisterStateData>& registers, std::set<std::string>& reasons)
{
    UnitResetHistory resets;
    std::map<Instruction, uint64_t> values;
    for (const auto& state : registers) {
        if (state.regId != ACLSAN_DEVICE_REGISTER_FPC)
            continue;
        const auto& h = state.header;
        const auto inserted = values.emplace(Key(h), state.value);
        if ((!inserted.second && inserted.first->second != state.value) || state.regIndex != 0 ||
            h.version != ACLSAN_API_VERSION || h.size < sizeof(state) || h.launchId == 0 || h.instrExecId == 0 ||
            h.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE || h.pipeline != ACLSAN_DEVICE_PIPE_SCALAR ||
            (h.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT && h.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE)) {
            reasons.insert("invalid_unit_flag_reset");
            continue;
        }
        constexpr uint64_t RESET_UNIT_FLAG = UINT64_C(1) << 63U;
        if ((state.value & RESET_UNIT_FLAG) != 0)
            resets[DomainOf(h)].insert(h.instrExecId);
    }
    return resets;
}

uint64_t UnitFlagEpoch(const AclsanDeviceEventHeader& header, const UnitResetHistory& resets)
{
    const auto found = resets.find(DomainOf(header));
    if (found == resets.end())
        return 0;
    const auto next = found->second.lower_bound(header.instrExecId);
    return next == found->second.begin() ? 0 : *std::prev(next);
}
} // namespace npucheck::race
