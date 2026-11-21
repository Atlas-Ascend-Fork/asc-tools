/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_UB_LAYOUT_H
#define NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_UB_LAYOUT_H

#include <stdint.h>

typedef enum AclsanUbRegionIndex {
    ACLSAN_UB_REGION_STATIC,
    ACLSAN_UB_REGION_DYNAMIC,
    ACLSAN_UB_REGION_RESERVED,
    ACLSAN_UB_REGION_DATA_CACHE,
    ACLSAN_UB_REGION_COUNT
} AclsanUbRegionIndex;

typedef enum AclsanUbLayoutStatus { ACLSAN_UB_LAYOUT_INVALID = 0, ACLSAN_UB_LAYOUT_VALID = 1 } AclsanUbLayoutStatus;

// Half-open byte offsets, in order: user static, launch dynamic, reserved, Data Cache.
// Empty regions consume no bytes. Gaps between regions are unallocated.
typedef struct AclsanUbRegion {
    uint64_t begin;
    uint64_t end;
} AclsanUbRegion;

typedef struct AclsanUbLayout {
    uint64_t physicalBytes;
    AclsanUbRegion regions[ACLSAN_UB_REGION_COUNT];
    uint32_t valid; // 1 only when all bounds describe the device layout for this launch.
    uint32_t reserved;
} AclsanUbLayout;

// This describes host-observable facts, not a guessed device layout.
// compilerAllocatedBytes includes the compiler's reserved UB when present.
typedef enum AclsanUbMetadataFlags {
    ACLSAN_UB_METADATA_COMPILER = 1u << 0u,
    ACLSAN_UB_METADATA_STATIC = 1u << 1u,
    ACLSAN_UB_METADATA_UNAVAILABLE = 1u << 2u
} AclsanUbMetadataFlags;

typedef struct AclsanUbLaunchMetadata {
    uint64_t compilerAllocatedBytes;
    uint64_t userStaticBytes;
    uint64_t dynamicBytes; // Exact value of this launch's dynUBufSize, before Runtime alignment.
    uint32_t kernelVfType; // Runtime ELF tag 12: 3=SIMT VF, 4=SIMD/SIMT VF.
    uint32_t flags;
} AclsanUbLaunchMetadata;

#endif // NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_UB_LAYOUT_H
