/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMORY_ACCESS_LAYOUT_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMORY_ACCESS_LAYOUT_H
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace npucheck::memory_layout {
inline constexpr uint32_t kMaxAffineRank = 5;
inline constexpr uint64_t kMaxLayoutSegments = 1u << 20u;
struct AffineLayout {
    uint64_t base = 0;
    uint32_t rank = 0;
    uint64_t elementBytes = 0;
    std::array<uint64_t, kMaxAffineRank> dimensions{};
    std::array<int64_t, kMaxAffineRank> strides{};
};

struct AffineAxis {
    uint64_t count = 0;
    int64_t stride = 0;
    uint64_t absoluteStride = 0;
};

inline uint64_t AbsoluteStride(int64_t stride)
{
    return stride < 0 ? static_cast<uint64_t>(-(stride + 1)) + 1 : static_cast<uint64_t>(stride);
}

inline bool AddOffset(__int128 left, __int128 right, __int128& result)
{
    return !__builtin_add_overflow(left, right, &result);
}

inline bool AxisExtent(const AffineAxis& axis, __int128& extent)
{
    return !__builtin_mul_overflow(static_cast<__int128>(axis.count - 1), static_cast<__int128>(axis.stride), &extent);
}

inline std::optional<uint64_t> AddressWithOffset(uint64_t base, __int128 offset)
{
    if (offset < -static_cast<__int128>(base) ||
        offset > static_cast<__int128>(std::numeric_limits<uint64_t>::max() - base)) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(static_cast<__int128>(base) + offset);
}

template <typename Visitor>
bool VisitAffineSegments(const AffineLayout& layout, Visitor&& visitor)
{
    if (layout.rank == 0 || layout.rank > kMaxAffineRank || layout.elementBytes == 0) {
        return false;
    }

    std::array<AffineAxis, kMaxAffineRank> axes{};
    uint32_t axisCount = 0;
    for (uint32_t dimension = 0; dimension < layout.rank; ++dimension) {
        const uint64_t count = layout.dimensions[dimension];
        if (count == 0) {
            return false;
        }
        if (count > 1) {
            const int64_t stride = layout.strides[dimension];
            axes[axisCount++] = {count, stride, AbsoluteStride(stride)};
        }
    }
    for (uint32_t index = 1; index < axisCount; ++index) {
        const AffineAxis axis = axes[index];
        uint32_t insertion = index;
        while (insertion > 0 && axes[insertion - 1].absoluteStride > axis.absoluteStride) {
            axes[insertion] = axes[insertion - 1];
            --insertion;
        }
        axes[insertion] = axis;
    }

    __int128 contiguousBegin = 0;
    uint64_t contiguousBytes = layout.elementBytes;
    uint32_t sparseBegin = 0;
    for (; sparseBegin < axisCount; ++sparseBegin) {
        const AffineAxis& axis = axes[sparseBegin];
        if (axis.absoluteStride > contiguousBytes) {
            break;
        }

        __int128 extent = 0;
        if (!AxisExtent(axis, extent)) {
            return false;
        }
        const unsigned __int128 absoluteExtent =
            extent < 0 ? static_cast<unsigned __int128>(-(extent + 1)) + 1 : static_cast<unsigned __int128>(extent);
        const unsigned __int128 mergedBytes = static_cast<unsigned __int128>(contiguousBytes) + absoluteExtent;
        if (mergedBytes > std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        contiguousBytes = static_cast<uint64_t>(mergedBytes);
        if (extent < 0 && !AddOffset(contiguousBegin, extent, contiguousBegin)) {
            return false;
        }
    }

    uint64_t segmentCount = 1;
    __int128 minimumOffset = contiguousBegin;
    __int128 maximumOffset = contiguousBegin;
    for (uint32_t axisIndex = sparseBegin; axisIndex < axisCount; ++axisIndex) {
        const AffineAxis& axis = axes[axisIndex];
        if (axis.count > kMaxLayoutSegments / segmentCount) {
            return false;
        }
        segmentCount *= axis.count;

        __int128 extent = 0;
        if (!AxisExtent(axis, extent)) {
            return false;
        }
        if (extent < 0) {
            if (!AddOffset(minimumOffset, extent, minimumOffset)) {
                return false;
            }
        } else if (!AddOffset(maximumOffset, extent, maximumOffset)) {
            return false;
        }
    }
    if (!AddressWithOffset(layout.base, minimumOffset) || !AddressWithOffset(layout.base, maximumOffset)) {
        return false;
    }

    for (uint64_t linear = 0; linear < segmentCount; ++linear) {
        uint64_t index = linear;
        __int128 offset = contiguousBegin;
        for (uint32_t axisIndex = sparseBegin; axisIndex < axisCount; ++axisIndex) {
            const AffineAxis& axis = axes[axisIndex];
            const uint64_t coordinate = index % axis.count;
            index /= axis.count;
            __int128 term = 0;
            if (__builtin_mul_overflow(static_cast<__int128>(coordinate), static_cast<__int128>(axis.stride), &term) ||
                !AddOffset(offset, term, offset)) {
                return false;
            }
        }
        const auto address = AddressWithOffset(layout.base, offset);
        if (!address) {
            return false;
        }
        visitor(*address, contiguousBytes);
    }
    return true;
}

} // namespace npucheck::memory_layout
#endif
