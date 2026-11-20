/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_UB_LAYOUT_BOUNDS_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_UB_LAYOUT_BOUNDS_H

#include "acl_san/aclsan_cbdata_launch.h"
#include "trace_buffer_abi.h"

namespace aclsan {

struct Dav3510UbLayoutBounds {
    uint32_t staticEnd = 0;
    uint32_t userEnd = 0;
    uint32_t sharedEnd = 0;
    bool valid = false;
};

inline Dav3510UbLayoutBounds ResolveDav3510UbLayoutBounds(
    uint32_t kernelVfType, uint64_t compilerBytes, uint64_t dynamicBytes) noexcept
{
    constexpr uint32_t capacity = DAV3510_UB_CAPACITY_BYTES;
    constexpr uint32_t reserved = DAV3510_UB_DEFAULT_RESERVED_BYTES;
    constexpr uint32_t staticAlignment = 32U;
    constexpr uint32_t sharedAlignment = 128U;
    constexpr uint32_t minSimtCache = 32U * 1024U;
    if (kernelVfType == ACLSAN_KERNEL_VF_TYPE_NO_VF) {
        return {0, capacity - reserved, capacity, true};
    }
    if (kernelVfType < ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY || kernelVfType > ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF ||
        compilerBytes < reserved || compilerBytes > capacity || dynamicBytes > capacity - compilerBytes) {
        return {};
    }
    // ELF compiler allocation includes the reserved area and the aligned static UB.
    const auto staticEnd = static_cast<uint32_t>(compilerBytes - reserved);
    if (staticEnd % staticAlignment != 0U) {
        return {};
    }
    const auto allocated = static_cast<uint32_t>(compilerBytes + dynamicBytes);
    const bool simt =
        kernelVfType == ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY || kernelVfType == ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF;
    const uint32_t sharedEnd = simt ? (allocated + sharedAlignment - 1U) / sharedAlignment * sharedAlignment : capacity;
    if (simt && sharedEnd > capacity - minSimtCache) {
        return {};
    }
    // Type 2 exposes all UB below the fixed tail reservation. The host's
    // dynamic request is validated above, but does not limit device accesses.
    // SIMT kernels instead use their aligned shared-memory configuration.
    return {staticEnd, sharedEnd - reserved, sharedEnd, true};
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_UB_LAYOUT_BOUNDS_H
