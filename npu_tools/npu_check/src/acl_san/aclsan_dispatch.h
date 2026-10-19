/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DISPATCH_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DISPATCH_H

#include "acl_san/aclsan_api.h"

namespace aclsan {

bool InvokeCallback(AclsanCallbackDomain domain, AclsanCallbackId callbackId, const void* callbackData) noexcept;

class AclsanCallbackDispatcher final {
public:
    static void DispatchResource(AclsanCallbackId callbackId, const AclsanResourceData& callbackData) noexcept;
    static void DispatchSynchronizeEnd(const AclsanSynchronizeData& callbackData) noexcept;
    static void DispatchDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& callbackData) noexcept;
    static void DispatchDeviceState(const AclsanDeviceRegisterStateData& callbackData) noexcept;
    static void DispatchDeviceSync(const AclsanDeviceSyncData& callbackData) noexcept;
    static void DispatchDeviceBarrier(const AclsanDeviceBarrierData& callbackData) noexcept;
    static void DispatchLaunch(const AclsanLaunchData& callbackData) noexcept;

private:
    static bool IsSupportedResourceCallback(AclsanCallbackId callbackId) noexcept;
    static void Dispatch(
        AclsanCallbackDomain domain, AclsanCallbackId callbackId, const void* callbackData,
        const char* eventName) noexcept;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_DISPATCH_H
