/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_H
#define NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_H

#include "acl_san/aclsan_cbdata_common.h"
#include "acl_san/aclsan_cbdata_device.h"
#include "acl_san/aclsan_cbdata_launch.h"
#include "acl_san/aclsan_cbdata_resource.h"
#include "acl_san/aclsan_cbdata_synchronize.h"

// Completed synchronous host-to-device writes only.
typedef struct AclsanMemoryWriteData {
    AclsanCallbackCommonData common; // Callback metadata and result.
    const void* destination;         // Device destination address, recorded without modifying its contents.
    uint64_t bytes;                  // Number of bytes written.
    uint32_t deviceId;               // Target device ID.
    uint32_t reserved;               // Reserved for future use.
} AclsanMemoryWriteData;

// Synchronous aclrtMemcpy completion. Failed operations carry their result and
// do not establish an initialized destination. Both device operands use deviceId;
// cross-device copies and asynchronous copies require a different protocol.
typedef enum AclsanMemoryTransferKind {
    ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE = 1,
    ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST = 2,
    ACLSAN_MEMORY_TRANSFER_DEVICE_TO_DEVICE = 3
} AclsanMemoryTransferKind;

typedef struct AclsanMemoryTransferData {
    AclsanCallbackCommonData common;
    const void* source;
    void* destination;
    uint64_t bytes;
    uint32_t deviceId;
    AclsanMemoryTransferKind kind;
} AclsanMemoryTransferData;

#endif // NPU_TOOLS_NPU_CHECK_INCLUDE_ACL_SAN_ACLSAN_CBDATA_H
