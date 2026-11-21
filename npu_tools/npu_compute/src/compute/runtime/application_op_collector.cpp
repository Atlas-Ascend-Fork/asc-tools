/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime/application_op_collector.h"
#include "hardware/hardware_device_api.h"
#include "hardware/hardware_info_host.h"
#include "hardware/hardware_info_json.h"
#include "npu_compute_log.h"
#include <array>

namespace npucompute {
namespace {
constexpr std::array<aclptiCallbackId, 11> kObservedCallbacks = {
    ACLPTI_RUNTIME_CBID_aclrtSetDevice,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernel,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithConfig,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry,
    ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol,
    ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad};
} // namespace

bool ApplicationOpCollector::Initialize(aclptiCallbackFunc callback)
{
    if (aclptiSubscribe(&subscriber_, callback, nullptr, nullptr) != ACLPTI_SUCCESS || subscriber_ == nullptr) {
        return false;
    }
    for (const auto cbid : kObservedCallbacks) {
        if (aclptiEnableCallback(true, subscriber_, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid) != ACLPTI_SUCCESS) {
            return false;
        }
    }
    return true;
}

void ApplicationOpCollector::OnCallback(aclptiCallbackId cbid, const aclptiCallbackData& data)
{
    if (cbid == ACLPTI_RUNTIME_CBID_aclrtSetDevice) {
        if (data.callbackSite == ACLPTI_API_EXIT && data.retval == ACL_SUCCESS && data.functionParams != nullptr) {
            const auto& params = *static_cast<const aclptiAclrtSetDeviceParams*>(data.functionParams);
            std::lock_guard<std::mutex> lock(mutex_);
            (void)CollectHardware(params.deviceId);
        }
        return;
    }
    auto launch = metadata_.ObserveLaunch(cbid, data);
    if (!launch) {
        return;
    }
    auto& metadata = launch->metadata;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!metadata.deviceId) {
        ++snapshot_.unidentifiedLaunches;
        return;
    }
    const int32_t device = *metadata.deviceId;
    const auto& hardware = CollectHardware(device);
    if (!metadata.name || metadata.name->empty()) {
        ++snapshot_.unidentifiedLaunches;
        return;
    }
    ApplicationOpInfo op;
    op.taskId = launch->taskId;
    op.streamId = launch->streamId;
    op.deviceEpoch = deviceEpoch_(device);
    op.launchIndex = launchIndices_[{device, *metadata.name}]++;
    op.instanceIndex = snapshot_.ops.size();
    metadata.hardware = hardware;
    op.metadata = std::move(metadata);
    snapshot_.ops.push_back(std::move(op));
}

const SummaryHardwareInfo& ApplicationOpCollector::CollectHardware(int32_t device)
{
    auto [hardware, inserted] = hardware_.try_emplace(device);
    if (!inserted) {
        return hardware->second;
    }
    // The caller holds mutex_; query the selected device without changing the application's context.
    DynamicHardwareDeviceApi api(false);
    hardware->second = CollectSummaryHardwareInfo(api, nullptr, device);
    HardwareInfoSnapshot hardwareSnapshot;
    hardwareSnapshot.deviceId = device;
    std::string jsonl;
    std::string error;
    if (CollectHostInfo("/tmp", &hardwareSnapshot.host, nullptr) &&
        CollectDeviceInfo(
            api, &hardwareSnapshot.device, &hardwareSnapshot.cpu, &hardwareSnapshot.aiCore, &hardwareSnapshot.memory,
            nullptr, device) &&
        SerializeHardwareInfoJsonl(hardwareSnapshot, &jsonl, &error)) {
        snapshot_.hardwareJsonl += jsonl;
    } else {
        ASCTOOL_WARNING("[npu-compute] application HardwareInfo unavailable: %s", error.c_str());
    }
    return hardware->second;
}

ApplicationOpSnapshot ApplicationOpCollector::Snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // A program without a selected device still gets the conventional HardwareInfo categories.
    if (snapshot_.hardwareJsonl.empty()) {
        HardwareInfoSnapshot hardware;
        std::string error;
        (void)CollectHostInfo("/tmp", &hardware.host, nullptr);
        (void)SerializeHardwareInfoJsonl(hardware, &snapshot_.hardwareJsonl, &error);
    }
    return snapshot_;
}
} // namespace npucompute
