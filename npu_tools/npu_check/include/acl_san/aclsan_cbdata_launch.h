/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_LAUNCH_H
#define NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_LAUNCH_H

#include "acl_san/aclsan_cbdata_common.h"

#include <stdint.h>

// Kernel types from ELF metadata. UNKNOWN is used when metadata is unavailable.
typedef enum AclsanKernelVfType {
    ACLSAN_KERNEL_VF_TYPE_UNKNOWN = 0,
    ACLSAN_KERNEL_VF_TYPE_NO_VF = 1,            // Ordinary SIMD without VF.
    ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY = 2,     // SIMD VF, optionally with ordinary SIMD.
    ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY = 3,     // SIMT VF without SIMD VF; ordinary SIMD is allowed.
    ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF = 4, // Both SIMD VF and SIMT VF.
} AclsanKernelVfType;

typedef struct AclsanLaunchData {
    AclsanCallbackCommonData common;
    uint64_t launchId;
    void* function; // aclrtFuncHandle
    void* stream;   // aclrtStream
    // 仅在 callback 调用期间有效；未通过 aclrtBinaryGetFunction 获取时为 NULL。
    const char* functionName;
    uint32_t numBlocks; // launch API 传入的原始 block 数量。
    // Optional UB layout inputs. Zero validity means unavailable, not pure SIMD.
    uint32_t kernelVfType; // AclsanKernelVfType value.
    uint32_t ubMetadataValid;
    uint64_t compilerAllocUbBytes;
    uint64_t dynamicUbBytes;
} AclsanLaunchData;

#endif // NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_LAUNCH_H
