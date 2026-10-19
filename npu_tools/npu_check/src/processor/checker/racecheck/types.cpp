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
#include <algorithm>
#include <functional>
#include <limits>

namespace npucheck::race {
bool ValidPipe(uint32_t p) { return p <= ACLSAN_DEVICE_PIPE_MTE3 || p == ACLSAN_DEVICE_PIPE_FIXPIPE; }
void MergeIntervals(std::vector<Interval>& ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.begin < b.begin; });
    size_t n = 0;
    for (auto r : ranges) {
        if (n && r.begin <= ranges[n - 1].end)
            ranges[n - 1].end = std::max(ranges[n - 1].end, r.end);
        else
            ranges[n++] = r;
    }
    ranges.resize(n);
}
bool Normalize(const AclsanDeviceMemoryAccessData& a, size_t budget, std::vector<Interval>& result)
{
    result.clear();
    __extension__ using Wide = __int128;
    std::vector<std::pair<uint64_t, int64_t>> axes;
    uint64_t bytes = 0;
    switch (a.layoutKind) {
        case ACLSAN_MEM_LAYOUT_SCALAR:
            bytes = a.layout.scalar.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_RANGE:
            bytes = a.layout.range.bytes;
            break;
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT:
            bytes = a.layout.blockRepeat.blockSize;
            axes = {
                {a.layout.blockRepeat.repeatTimes, a.layout.blockRepeat.repeatStride},
                {a.layout.blockRepeat.blockNum, a.layout.blockRepeat.blockStride}};
            break;
        case ACLSAN_MEM_LAYOUT_ND_AFFINE:
            if (!a.layout.ndAffine.rank || a.layout.ndAffine.rank > 5)
                return false;
            bytes = a.layout.ndAffine.elementBytes;
            for (uint32_t i = 0; i < a.layout.ndAffine.rank; ++i)
                axes.emplace_back(a.layout.ndAffine.dims[i], a.layout.ndAffine.strides[i]);
            break;
        default:
            return false;
    }
    if (!bytes || std::any_of(axes.begin(), axes.end(), [](auto axis) { return axis.first == 0; }))
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
    Wide lower = a.address, upper = a.address;
    std::vector<std::pair<uint64_t, uint64_t>> forwardAxes;
    for (const auto axis : axes) {
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
    for (const auto axis : forwardAxes) {
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
Storage StorageOf(const AclsanDeviceMemoryAccessData& a)
{
    return {
        a.memorySpace,
        a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ? Domain{a.header.deviceId, 0, 0, 0} : DomainOf(a.header)};
}
} // namespace npucheck::race
