/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_dispatch.h"
#include "npu_tool_log.h"

#include <cstdint>

namespace aclsan {

bool AclsanCallbackDispatcher::IsSupportedResourceCallback(AclsanCallbackId cbid) noexcept
{
    return cbid == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC || cbid == ACLSAN_CBID_RESOURCE_MEMORY_FREE;
}

// 对着指定domain + id 发送cbdata
void AclsanCallbackDispatcher::Dispatch(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata, const char* eventName) noexcept
{
    if (!InvokeCallback(domain, cbid, cbdata)) {
        ASCTOOL_ERROR(
            "InvokeCallback in AclsanCallbackDispatcher Dispatch failed: event=%s domain=%u id=%u", eventName,
            static_cast<uint32_t>(domain), static_cast<uint32_t>(cbid));
    }
}

void AclsanCallbackDispatcher::DispatchResource(AclsanCallbackId cbid, const AclsanResourceData& cbdata) noexcept
{
    if (!IsSupportedResourceCallback(cbid)) {
        ASCTOOL_ERROR(
            "AclsanCallbackDispatcher DispatchResource failed due to invalid cbid=%u", static_cast<uint32_t>(cbid));
        return;
    }
    Dispatch(ACLSAN_CB_DOMAIN_RESOURCE, cbid, &cbdata, cbdata.common.apiName);
}

void AclsanCallbackDispatcher::DispatchSynchronizeEnd(const AclsanSynchronizeData& cbdata) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &cbdata, cbdata.common.apiName);
}

void AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& cbdata) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &cbdata, "DEVICE_MEMORY_ACCESS");
}

void AclsanCallbackDispatcher::DispatchDeviceState(const AclsanDeviceRegisterStateData& cbdata) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE, &cbdata, "DEVICE_STATE");
}

void AclsanCallbackDispatcher::DispatchDeviceSync(const AclsanDeviceSyncData& cbdata) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &cbdata, "DEVICE_SYNC");
}

void AclsanCallbackDispatcher::DispatchLaunch(const AclsanLaunchData& cbdata) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &cbdata, cbdata.common.apiName);
}

void AclsanCallbackDispatcher::DispatchDeviceBarrier(const AclsanDeviceBarrierData& cbdata) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_BARRIER, ACLSAN_CBID_DEVICE_PIPE_BARRIER, &cbdata, "DEVICE_PIPE_BARRIER");
}

} // namespace aclsan
