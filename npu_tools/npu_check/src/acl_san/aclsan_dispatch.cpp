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

bool AclsanCallbackDispatcher::IsSupportedResourceCallback(AclsanCallbackId cbId) noexcept
{
    return cbId == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC || cbId == ACLSAN_CBID_RESOURCE_MEMORY_FREE;
}

// 对着指定domain + id 发送cbData
void AclsanCallbackDispatcher::Dispatch(
    AclsanCallbackDomain domain, AclsanCallbackId cbId, const void* cbData, const char* eventName) noexcept
{
    if (!InvokeCallback(domain, cbId, cbData)) {
        ASCTOOL_ERROR(
            "InvokeCallback in AclsanCallbackDispatcher Dispatch failed: event=%s domain=%u id=%u", eventName,
            static_cast<uint32_t>(domain), static_cast<uint32_t>(cbId));
    }
}

void AclsanCallbackDispatcher::DispatchResource(AclsanCallbackId cbId, const AclsanResourceData& cbData) noexcept
{
    if (!IsSupportedResourceCallback(cbId)) {
        ASCTOOL_ERROR(
            "AclsanCallbackDispatcher DispatchResource failed due to invalid cbid=%u", static_cast<uint32_t>(cbId));
        return;
    }
    Dispatch(ACLSAN_CB_DOMAIN_RESOURCE, cbId, &cbData, cbData.common.apiName);
}

void AclsanCallbackDispatcher::DispatchMemoryWrite(const AclsanMemoryWriteData& cbData) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, &cbData, cbData.common.apiName);
}

void AclsanCallbackDispatcher::DispatchSynchronizeEnd(const AclsanSynchronizeData& cbData) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &cbData, cbData.common.apiName);
}

void AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& cbData) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &cbData, "DEVICE_MEMORY_ACCESS");
}

void AclsanCallbackDispatcher::DispatchDeviceState(const AclsanDeviceRegisterStateData& cbData) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE, &cbData, "DEVICE_STATE");
}

void AclsanCallbackDispatcher::DispatchDeviceSync(const AclsanDeviceSyncData& cbData) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &cbData, "DEVICE_SYNC");
}

void AclsanCallbackDispatcher::DispatchLaunch(const AclsanLaunchData& cbData) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &cbData, cbData.common.apiName);
}

void AclsanCallbackDispatcher::DispatchDeviceBarrier(const AclsanDeviceBarrierData& cbData) noexcept
{
    Dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER, &cbData, "DEVICE_PIPE_BARRIER");
}

} // namespace aclsan
