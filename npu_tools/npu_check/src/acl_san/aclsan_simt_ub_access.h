/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_SIMT_UB_ACCESS_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_SIMT_UB_ACCESS_H

#include "acl_san/aclsan_api.h"
#include "acl_san/aclsan_cbdata_device.h"
#include "acl_san/aclsan_ub_layout.h"

// Private payload for memcheck on the opt-in SIMT callback. The shared
// AclsanDeviceMemoryAccessData schema and other checker inputs remain unchanged.
struct AclsanSimtUbMemoryAccessData : AclsanDeviceMemoryAccessData {
    AclsanUbLayout ubLayout{};
};

inline bool IsSimtUbMemcheckCallback(AclsanCallbackDomain domain, AclsanCallbackId id, const void* raw)
{
    if (domain != ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION || id != ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS || !raw) {
        return false;
    }
    const auto& header = *static_cast<const AclsanDeviceEventHeader*>(raw);
    return (header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_SIMT_UB)) != 0U;
}
#endif
