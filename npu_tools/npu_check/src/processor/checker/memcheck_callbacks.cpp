/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/memcheck.h"
#include "npu_tool_log.h"
#include <sstream>

namespace npucheck {
const std::vector<CallbackSpec>& Memcheck::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> callbacks{
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC},
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END}};
    return callbacks;
}

bool Memcheck::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    if (domain == ACLSAN_CB_DOMAIN_RESOURCE) {
        const auto* event = static_cast<const AclsanResourceData*>(data);
        if (cbid == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) {
            OnAllocation(*event);
        } else {
            OnFree(*event);
        }
    } else if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION) {
        const auto* event = static_cast<const AclsanDeviceMemoryAccessData*>(data);
        QueueDeviceMemoryAccess(*event);
    } else if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto* event = static_cast<const AclsanLaunchData*>(data);
        if (event->common.result != 0) {
            ++stats_.failedLaunches;
        }
    } else if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        const auto* event = static_cast<const AclsanSynchronizeData*>(data);
        if (event->common.result != 0) {
            ++stats_.failedSynchronizations;
        }
        auto completed = OnSynchronization();
        ASCTOOL_INFO("synchronization completed reports=%zu stream=%p", completed.size(), event->stream);
        AppendCheckerReports(std::move(completed), reports);
    }
    return true;
}

bool Memcheck::HasErrors() const { return Stats().errors != 0; }
bool Memcheck::AnalysisComplete() const
{
    const auto stats = Stats();
    return stats.pendingDeviceOperations == 0 && stats.droppedDeviceOperations == 0 && stats.failedLaunches == 0 &&
           stats.failedSynchronizations == 0;
}
std::string Memcheck::Summary() const
{
    const auto stats = Stats();
    std::ostringstream output;
    output << "tool=memcheck allocations=" << stats.allocations << " frees=" << stats.frees
           << " device_operations=" << stats.deviceOperations << " synchronizations=" << stats.synchronizationEvents
           << " errors=" << stats.errors << " warnings=" << stats.warnings
           << " pending_device_operations=" << stats.pendingDeviceOperations
           << " dropped_device_operations=" << stats.droppedDeviceOperations
           << " failed_launches=" << stats.failedLaunches
           << " failed_synchronizations=" << stats.failedSynchronizations;
    output << " inexact_device_operations=" << stats.inexactDeviceOperations;
    return output.str();
}
} // namespace npucheck
