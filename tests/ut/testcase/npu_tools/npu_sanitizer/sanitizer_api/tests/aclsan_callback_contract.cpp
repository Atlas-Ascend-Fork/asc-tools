/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_cbdata_common.h"
#include "acl_san/aclsan_cbdata_device.h"
#include "acl_san/aclsan_cbdata_launch.h"
#include "acl_san/aclsan_cbdata_resource.h"
#include "acl_san/aclsan_cbdata_synchronize.h"
#include "acl_san/aclsan_cbdata.h"
#include "acl_san/aclsan_api.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

template <typename T, typename = void>
struct HasInstrType : std::false_type {};

template <typename T>
struct HasInstrType<T, std::void_t<decltype(&T::instrType)>> : std::true_type {};

template <typename T, typename = void>
struct HasLaunchId : std::false_type {};

template <typename T>
struct HasLaunchId<T, std::void_t<decltype(&T::launchId)>> : std::true_type {};

template <typename T, typename = void>
struct HasKernelType : std::false_type {};

template <typename T>
struct HasKernelType<T, std::void_t<decltype(&T::kernelType)>> : std::true_type {};

template <typename T, typename = void>
struct HasAicRatio : std::false_type {};

template <typename T>
struct HasAicRatio<T, std::void_t<decltype(&T::aicRatio)>> : std::true_type {};

template <typename T, typename = void>
struct HasAivRatio : std::false_type {};

template <typename T>
struct HasAivRatio<T, std::void_t<decltype(&T::aivRatio)>> : std::true_type {};

template <typename T, typename = void>
struct HasKernelSchedMode : std::false_type {};

template <typename T>
struct HasKernelSchedMode<T, std::void_t<decltype(&T::kernelSchedMode)>> : std::true_type {};

static_assert(std::is_same_v<AclsanStatus, std::uint32_t>);
static_assert(std::is_same_v<AclsanCallbackId, std::uint32_t>);
static_assert(std::is_enum_v<AclsanCallbackIdResource>);
static_assert(std::is_enum_v<AclsanCallbackIdSynchronize>);
static_assert(std::is_enum_v<AclsanCallbackIdDeviceInstruction>);
static_assert(std::is_enum_v<AclsanCallbackIdLaunch>);
static_assert(std::is_same_v<decltype(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC), AclsanCallbackIdResource>);
static_assert(std::is_same_v<decltype(ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END), AclsanCallbackIdSynchronize>);
static_assert(std::is_same_v<decltype(ACLSAN_CBID_DEVICE_MEMORY_ACCESS), AclsanCallbackIdDeviceInstruction>);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) == 1);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_RESOURCE_MEMORY_FREE) == 2);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_RESOURCE_INVALID) == 0x7fffffffU);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END) == 1);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_SYNCHRONIZE_DEVICE_SYNC_END) == 2);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_SYNCHRONIZE_INVALID) == 0x7fffffffU);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_DEVICE_MEMORY_ACCESS) == 1);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_DEVICE_SYNC) == 2);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_DEVICE_INSTRUCTION_INVALID) == 0x7fffffffU);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_LAUNCH_KERNEL) == 1);
static_assert(static_cast<std::uint32_t>(ACLSAN_CBID_LAUNCH_INVALID) == 0x7fffffffU);
static_assert(std::is_standard_layout_v<AclsanCallbackCommonData>);
static_assert(std::is_standard_layout_v<AclsanResourceData>);
static_assert(std::is_standard_layout_v<AclsanSynchronizeData>);
static_assert(std::is_standard_layout_v<AclsanLaunchData>);
static_assert(!HasLaunchId<AclsanSynchronizeData>::value);
static_assert(HasLaunchId<AclsanLaunchData>::value);
static_assert(std::is_same_v<decltype(AclsanLaunchData::common), AclsanCallbackCommonData>);
static_assert(std::is_same_v<decltype(AclsanLaunchData::function), void*>);
static_assert(std::is_same_v<decltype(AclsanLaunchData::stream), void*>);
static_assert(std::is_same_v<decltype(AclsanLaunchData::functionName), const char*>);
static_assert(std::is_same_v<decltype(AclsanLaunchData::numBlocks), uint32_t>);
static_assert(!HasKernelType<AclsanLaunchData>::value);
static_assert(!HasAicRatio<AclsanLaunchData>::value);
static_assert(!HasAivRatio<AclsanLaunchData>::value);
static_assert(!HasKernelSchedMode<AclsanLaunchData>::value);
static_assert(std::is_standard_layout_v<AclsanDeviceMemoryAccessData>);
static_assert(std::is_standard_layout_v<AclsanDeviceSyncData>);
static_assert(std::is_standard_layout_v<AclsanDeviceRegisterStateData>);
static_assert(offsetof(AclsanDeviceRegisterStateData, regId) == 72);
static_assert(offsetof(AclsanDeviceRegisterStateData, regIndex) == 76);
static_assert(offsetof(AclsanDeviceRegisterStateData, value) == 80);
static_assert(sizeof(AclsanDeviceRegisterStateData) == 88);
static_assert(ACLSAN_DEVICE_PIPE_INVALID == 100);
static_assert(ACLSAN_DEVICE_PIPE_SCALAR == 0);
static_assert(ACLSAN_DEVICE_PIPE_VECTOR == 1);
static_assert(ACLSAN_DEVICE_PIPE_MTE2 == 4);
static_assert(ACLSAN_DEVICE_PIPE_MTE3 == 5);
static_assert(ACLSAN_DEVICE_PIPE_FIXPIPE == 10);
static_assert(ACLSAN_DEVICE_BLOCK_TYPE_AICORE == 0);
static_assert(ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR == 1);
static_assert(ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE == 2);
static_assert(!HasInstrType<AclsanDeviceSyncData>::value);
static_assert(std::is_same_v<decltype(AclsanDeviceSyncData::header), AclsanDeviceEventHeader>);
static_assert(offsetof(AclsanDeviceSyncData, header) == 0);
static_assert(offsetof(AclsanDeviceSyncData, syncKind) == sizeof(AclsanDeviceEventHeader));
static_assert(offsetof(AclsanDeviceSyncData, objectId) == 96);
static_assert(offsetof(AclsanDeviceSyncData, reserved) == 104);
static_assert(sizeof(AclsanDeviceSyncData) == 112);
static_assert(
    std::is_same_v<decltype(&aclsanSubscribe), AclsanStatus (*)(AclsanSubscriberHandle*, AclsanCallbackFunc, void*)>);
static_assert(std::is_same_v<decltype(&aclsanUnsubscribe), AclsanStatus (*)(AclsanSubscriberHandle)>);
static_assert(std::is_same_v<
              decltype(&aclsanEnableCallback),
              AclsanStatus (*)(uint32_t, AclsanSubscriberHandle, AclsanCallbackDomain, AclsanCallbackId)>);
static_assert(std::is_same_v<
              decltype(&aclsanEnableDomain), AclsanStatus (*)(AclsanSubscriberHandle, AclsanCallbackDomain, uint32_t)>);
static_assert(std::is_same_v<
              decltype(&aclsanGetCallbackState),
              AclsanStatus (*)(AclsanSubscriberHandle, AclsanCallbackDomain, AclsanCallbackId, uint32_t*)>);
static_assert(std::is_same_v<decltype(&aclsanGetDeviceCallStack), AclsanStatus (*)(uint64_t, AclsanDeviceCallStack*)>);

int main() { return ACLSAN_STATUS_SUCCESS; }
