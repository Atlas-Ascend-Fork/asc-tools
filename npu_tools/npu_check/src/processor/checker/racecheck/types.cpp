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
#include "acl_san/aclsan_api.h"
#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

namespace npucheck::race {
bool ValidPipe(uint32_t pipe) { return pipe <= ACLSAN_DEVICE_PIPE_MTE3 || pipe == ACLSAN_DEVICE_PIPE_FIXPIPE; }
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
namespace {
__extension__ using Wide = __int128;
using SignedAxis = std::pair<uint64_t, int64_t>;
using ForwardAxis = std::pair<uint64_t, uint64_t>;
struct Footprint {
    uint64_t bytes = 0;
    Wide lower = 0;
    std::vector<SignedAxis> axes;
    std::vector<ForwardAxis> forward;
    std::vector<ForwardAxis> sparse;
};
bool ReadFootprint(const AclsanDeviceMemoryAccessData& access, Footprint& footprint)
{
    auto& bytes = footprint.bytes;
    auto& axes = footprint.axes;
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
    return true;
}
bool ValidateFootprint(uint64_t address, Footprint& footprint)
{
    const Wide max = std::numeric_limits<uint64_t>::max();
    Wide lower = address, upper = address;
    // Check signed extrema before folding, including INT64_MIN strides.
    for (const auto& axis : footprint.axes) {
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
        const auto step = axis.second < 0 ? static_cast<uint64_t>(-static_cast<Wide>(axis.second)) :
                                            static_cast<uint64_t>(axis.second);
        footprint.forward.emplace_back(axis.first, step);
    }
    footprint.lower = lower;
    return footprint.bytes <= max - upper;
}
bool FoldDenseAxes(Footprint& footprint)
{
    const Wide max = std::numeric_limits<uint64_t>::max();
    std::sort(footprint.forward.begin(), footprint.forward.end(), [](auto a, auto b) { return a.second < b.second; });
    for (const auto& axis : footprint.forward) {
        if (axis.first == 1)
            continue;
        if (axis.second > footprint.bytes) {
            footprint.sparse.push_back(axis);
            continue;
        }
        const Wide growth = static_cast<Wide>(axis.first - 1) * axis.second;
        if (growth > max - footprint.bytes)
            return false;
        footprint.bytes += static_cast<uint64_t>(growth);
    }
    return true;
}
NormalizationStatus MaterializeFootprint(const Footprint& footprint, size_t budget, std::vector<Interval>& result)
{
    size_t count = 1;
    for (const auto& axis : footprint.sparse) {
        if (axis.first > budget / count)
            return NormalizationStatus::SegmentBudget;
        count *= static_cast<size_t>(axis.first);
    }
    if (count > budget)
        return NormalizationStatus::SegmentBudget;
    const Wide max = std::numeric_limits<uint64_t>::max();
    std::function<bool(size_t, Wide)> visit = [&footprint, &result, &visit, max](size_t dimension, Wide offset) {
        if (dimension == footprint.sparse.size()) {
            if (offset < 0 || offset > max || footprint.bytes > max - offset)
                return false;
            result.push_back({static_cast<uint64_t>(offset), static_cast<uint64_t>(offset + footprint.bytes)});
            return true;
        }
        const auto& axis = footprint.sparse[dimension];
        for (uint64_t i = 0; i < axis.first; ++i)
            if (!visit(dimension + 1, offset + static_cast<Wide>(i) * axis.second))
                return false;
        return true;
    };
    if (!visit(0, footprint.lower)) {
        result.clear();
        return NormalizationStatus::InvalidLayout;
    }
    MergeIntervals(result);
    return NormalizationStatus::Complete;
}
NormalizationStatus PrepareFootprint(const AclsanDeviceMemoryAccessData& access, Footprint& footprint)
{
    if (!ReadFootprint(access, footprint))
        return NormalizationStatus::InvalidLayout;
    if (footprint.bytes == 0 ||
        std::any_of(footprint.axes.begin(), footprint.axes.end(), [](auto a) { return a.first == 0; })) {
        footprint.bytes = 0;
        return NormalizationStatus::Complete;
    }
    return ValidateFootprint(access.address, footprint) && FoldDenseAxes(footprint) ?
               NormalizationStatus::Complete :
               NormalizationStatus::InvalidLayout;
}
} // namespace
NormalizationStatus DescribeNormalization(
    const AclsanDeviceMemoryAccessData& access, NormalizationObservation& observation)
{
    observation = {};
    Footprint footprint;
    const auto status = PrepareFootprint(access, footprint);
    if (status != NormalizationStatus::Complete)
        return status;
    observation.empty = footprint.bytes == 0;
    if (!observation.empty) {
        observation.base = static_cast<uint64_t>(footprint.lower);
        observation.bytes = footprint.bytes;
        observation.sparseAxes = std::move(footprint.sparse);
    }
    return status;
}
NormalizationStatus NormalizeDetailed(
    const AclsanDeviceMemoryAccessData& access, size_t budget, std::vector<Interval>& result)
{
    result.clear();
    Footprint footprint;
    const auto status = PrepareFootprint(access, footprint);
    if (status != NormalizationStatus::Complete || footprint.bytes == 0)
        return status;
    return MaterializeFootprint(footprint, budget, result);
}
bool Normalize(const AclsanDeviceMemoryAccessData& access, size_t budget, std::vector<Interval>& result)
{
    return NormalizeDetailed(access, budget, result) == NormalizationStatus::Complete;
}
Storage StorageOf(const AclsanDeviceMemoryAccessData& access)
{
    return {
        access.memorySpace,
        access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ?
            Domain{access.header.deviceId, 0, 0, 0} :
            Domain{access.header.deviceId, access.memoryOwner.blockType, 0, access.memoryOwner.phyCoreId}};
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
            h.executor.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE || h.pipeline != ACLSAN_DEVICE_PIPE_SCALAR ||
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
