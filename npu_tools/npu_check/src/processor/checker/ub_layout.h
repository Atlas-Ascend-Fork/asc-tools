/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_UB_LAYOUT_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_UB_LAYOUT_H

#include "acl_san/aclsan_cbdata_launch.h"
#include "dbi/ctrlbin/ctrlbin_bindings.h"
#include "dbi/ub_layout_bounds.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace npucheck {

inline bool IsSimtMemoryInstruction(uint32_t id)
{
    const auto in = [id](InstrType first, InstrType last) {
        return id >= static_cast<uint32_t>(first) && id <= static_cast<uint32_t>(last);
    };
    return in(InstrType::SIMT_LD_U8, InstrType::SIMT_STK_B32) || in(InstrType::SIMT_LDS_U8, InstrType::SIMT_STS_B128) ||
           in(InstrType::RED_S_U32, InstrType::ATOM_MAX_G_S32);
}
constexpr uint64_t UB_CAPACITY = aclsan::DAV3510_UB_CAPACITY_BYTES;

enum class UbRegionKind { USER, STATIC, DYNAMIC, RESERVED, SIMD_FORBIDDEN };

struct UbRegion {
    uint64_t begin;
    uint64_t end;
    UbRegionKind kind;
    bool AllowsSimdAccess() const
    {
        return kind == UbRegionKind::USER || kind == UbRegionKind::STATIC || kind == UbRegionKind::DYNAMIC ||
               kind == UbRegionKind::RESERVED;
    }
    bool Contains(uint64_t address, uint64_t bytes) const
    {
        return address >= begin && address <= end && bytes <= end - address;
    }
};

struct UbLayout {
    bool valid = false;
    std::vector<UbRegion> regions;
    uint32_t kernelVfType = ACLSAN_KERNEL_VF_TYPE_UNKNOWN;
    const char* unavailableReason = "launch_metadata_unavailable";
    std::optional<UbRegion> Find(uint64_t address) const
    {
        for (const auto& region : regions) {
            if (address >= region.begin && address < region.end) {
                return region;
            }
        }
        return std::nullopt;
    }
};

inline UbLayout BuildUbLayout(const AclsanLaunchData& input)
{
    if (input.ubMetadataValid != 1) {
        return {};
    }
    const auto bounds =
        aclsan::ResolveDav3510UbLayoutBounds(input.kernelVfType, input.compilerAllocUbBytes, input.dynamicUbBytes);
    if (!bounds.valid) {
        UbLayout result;
        result.kernelVfType = input.kernelVfType;
        result.unavailableReason = "invalid_layout_inputs";
        return result;
    }
    const uint64_t shared = bounds.sharedEnd;
    const uint64_t userEnd = bounds.userEnd;
    const uint64_t reservedBegin = shared - aclsan::DAV3510_UB_DEFAULT_RESERVED_BYTES;
    if (input.kernelVfType == ACLSAN_KERNEL_VF_TYPE_NO_VF) {
        return {
            true,
            {{0, userEnd, UbRegionKind::USER}, {reservedBegin, shared, UbRegionKind::RESERVED}},
            input.kernelVfType,
            nullptr};
    }
    const uint64_t staticEnd = bounds.staticEnd;
    UbLayout result{
        true,
        {{0, staticEnd, UbRegionKind::STATIC}, {staticEnd, userEnd, UbRegionKind::DYNAMIC}},
        input.kernelVfType,
        nullptr};
    result.regions.push_back({reservedBegin, shared, UbRegionKind::RESERVED});
    if (shared < UB_CAPACITY) {
        result.regions.push_back({shared, UB_CAPACITY, UbRegionKind::SIMD_FORBIDDEN});
    }
    return result;
}

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_UB_LAYOUT_H
