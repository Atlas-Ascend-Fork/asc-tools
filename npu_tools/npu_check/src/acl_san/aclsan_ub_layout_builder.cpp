/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "aclsan_ub_layout_builder.h"
#include "acl_san/aclsan_cbdata_launch.h"
#include "dbi/trace_buffer_abi.h"

#include <algorithm>

namespace aclsan {

bool BuildSimtUbLayout(SocVersion version, const AclsanUbLaunchMetadata& metadata, AclsanUbLayout& layout) noexcept
{
    layout = {};
    constexpr uint64_t SHARED_ALIGN = 128U;
    constexpr uint64_t MAX_SHARED_BYTES = DAV3510_UB_CAPACITY_BYTES - SIMT_UB_MIN_CACHE_BYTES;
    constexpr uint32_t REQUIRED = ACLSAN_UB_METADATA_COMPILER | ACLSAN_UB_METADATA_STATIC;
    if (version != SocVersion::DAV_3510 || (metadata.flags & REQUIRED) != REQUIRED ||
        (metadata.flags & ACLSAN_UB_METADATA_UNAVAILABLE) != 0 ||
        (metadata.kernelVfType != ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY &&
         metadata.kernelVfType != ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF) ||
        metadata.userStaticBytes > MAX_SHARED_BYTES - DAV3510_UB_DEFAULT_RESERVED_BYTES ||
        metadata.compilerAllocatedBytes != metadata.userStaticBytes + DAV3510_UB_DEFAULT_RESERVED_BYTES ||
        metadata.dynamicBytes > MAX_SHARED_BYTES - metadata.compilerAllocatedBytes) {
        return false;
    }

    // Runtime CheckAndGetTotalShareMemorySize uses align128(tag7 + launch dynUBufSize).
    // All additions are bounded above by MAX_SHARED_BYTES (or MAX_SHARED_BYTES + 127).
    const uint64_t sharedBytes =
        (metadata.compilerAllocatedBytes + metadata.dynamicBytes + SHARED_ALIGN - 1U) / SHARED_ALIGN * SHARED_ALIGN;
    if (sharedBytes > MAX_SHARED_BYTES) {
        return false;
    }
    // The default reserve ends at the aligned shared-memory endpoint.
    // Data Cache occupies the highest UB addresses and is capped at 128 KiB.
    layout.physicalBytes = DAV3510_UB_CAPACITY_BYTES;
    layout.regions[ACLSAN_UB_REGION_STATIC] = {0U, metadata.userStaticBytes};
    layout.regions[ACLSAN_UB_REGION_DYNAMIC] = {
        metadata.userStaticBytes, sharedBytes - DAV3510_UB_DEFAULT_RESERVED_BYTES};
    layout.regions[ACLSAN_UB_REGION_RESERVED] = {sharedBytes - DAV3510_UB_DEFAULT_RESERVED_BYTES, sharedBytes};
    const uint64_t cacheBytes = std::min<uint64_t>(DAV3510_UB_CAPACITY_BYTES - sharedBytes, SIMT_UB_MAX_CACHE_BYTES);
    layout.regions[ACLSAN_UB_REGION_DATA_CACHE] = {DAV3510_UB_CAPACITY_BYTES - cacheBytes, DAV3510_UB_CAPACITY_BYTES};
    layout.valid = ACLSAN_UB_LAYOUT_VALID;
    return true;
}

} // namespace aclsan
