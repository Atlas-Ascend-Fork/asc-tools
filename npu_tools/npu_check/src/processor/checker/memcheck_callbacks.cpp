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
#include "securec.h"
#include <cstddef>
#include <sstream>

namespace npucheck {
const std::vector<CallbackSpec>& Memcheck::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> callbacks{
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_TRANSFER_END},
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC},
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END}};
    return callbacks;
}

bool Memcheck::OnDeviceMemoryCallback(AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    const auto* header = static_cast<const AclsanDeviceEventHeader*>(data);
    const bool simt = cbid == ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS;
    const size_t required = simt ? sizeof(AclsanDeviceMemoryAccessData) : offsetof(AclsanDeviceMemoryAccessData, simt);
    if (header == nullptr || header->version != ACLSAN_API_VERSION || header->size < required) {
        ++stats_.droppedDeviceOperations;
        return false;
    }
    ++stats_.deviceOperations;
    constexpr uint32_t incompleteFlags = static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) |
                                         static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED) |
                                         static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR);
    if (simt && (header->flags & incompleteFlags) != 0U) {
        ++stats_.droppedDeviceOperations;
        return true;
    }
    AclsanDeviceMemoryAccessData event{};
    if (memcpy_s(&event, sizeof(event), data, required) != EOK) {
        ++stats_.droppedDeviceOperations;
        return false;
    }
    event.header.size = sizeof(event);
    auto immediate = QueueDeviceMemoryAccess(event);
    AppendCheckerReports(std::move(immediate), reports);
    return true;
}

bool Memcheck::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    if (IsSimtUbMemcheckCallback(domain, cbid, data)) {
        const auto* access = static_cast<const AclsanDeviceMemoryAccessData*>(data);
        if (access == nullptr || access->header.version != ACLSAN_API_VERSION ||
            access->header.size < sizeof(AclsanSimtUbMemoryAccessData)) {
            ++stats_.droppedDeviceOperations;
            return false;
        }
        QueueSimtUbMemoryAccess(*static_cast<const AclsanSimtUbMemoryAccessData*>(access));
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_MEMORY) {
        const auto& common = *static_cast<const AclsanCallbackCommonData*>(data);
        if (!analysis_->IsLegacyMemcpy(cbid, common))
            OnHostMemory(common, reports);
    } else if (domain == ACLSAN_CB_DOMAIN_RESOURCE) {
        const auto* event = static_cast<const AclsanResourceData*>(data);
        if (cbid == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) {
            OnAllocation(*event);
        } else {
            OnFree(*event);
        }
    } else if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_VF) {
        const auto* completion = static_cast<const AclsanDeviceVfData*>(data);
        if (!completion || completion->header.version != ACLSAN_API_VERSION ||
            completion->header.size < sizeof(*completion)) {
            ++stats_.droppedDeviceOperations;
            return false;
        }
        if (completion->status != ACLSAN_DEVICE_VF_SUCCESS ||
            completion->header.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT ||
            completion->accessCount > completion->eventCount || completion->eventCount > completion->rawEventCount) {
            ++stats_.droppedDeviceOperations;
        }
    } else if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION) {
        return OnDeviceMemoryCallback(cbid, data, reports);
    } else if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto* event = static_cast<const AclsanLaunchData*>(data);
        if (!analysis_->Managed()) {
            analysis_->RegisterUbLayout(*event);
        }
        if (event->common.result != 0) {
            ++stats_.failedLaunches;
        } else if (analysis_->Managed() && event->launchId != 0) {
            observedLaunches_.insert(event->launchId);
        } else if (!analysis_->Managed() && event->launchId != 0) {
            constexpr size_t MAX_PENDING_LAUNCH_SNAPSHOTS = 1024;
            if (launchAllocations_.size() >= MAX_PENDING_LAUNCH_SNAPSHOTS) {
                ++stats_.droppedDeviceOperations;
            } else {
                launchAllocations_.try_emplace(
                    event->launchId, LaunchAllocationSnapshot{memoryState_->allocations, event->stream});
            }
        }
    } else if (domain == ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION) {
        const auto* event = static_cast<const AclsanOperatorExecutionData*>(data);
        if (event == nullptr || event->common.version != ACLSAN_API_VERSION || event->common.size < sizeof(*event) ||
            event->launchId == 0) {
            unfinishedLaunches_ = true;
            return false;
        }
        if (event->common.result != 0 || event->flags != ACLSAN_OPERATOR_EXECUTION_FLAG_NONE)
            unfinishedLaunches_ = true;
        auto completed = CollectCompletedLaunch(event->launchId);
        AppendCheckerReports(std::move(completed), reports);
    } else if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        const auto* event = static_cast<const AclsanSynchronizeData*>(data);
        ++stats_.synchronizationEvents;
        if (event->common.result != 0) {
            ++stats_.failedSynchronizations;
        }
    }
    return true;
}

bool Memcheck::HasErrors() const { return Stats().errors != 0; }
bool Memcheck::AnalysisComplete() const
{
    const auto stats = Stats();
    return !unfinishedLaunches_ && launchAllocations_.empty() && stats.pendingDeviceOperations == 0 &&
           stats.droppedDeviceOperations == 0 && stats.failedLaunches == 0 && stats.failedSynchronizations == 0 &&
           stats.incompleteHostOperations == 0;
}
std::string Memcheck::Summary() const
{
    const auto stats = Stats();
    std::ostringstream output;
    output << "tool=memcheck device_operations=" << stats.deviceOperations
           << " memory_events=" << stats.deviceOperations << " sync_events=" << stats.synchronizationEvents
           << " errors=" << stats.errors << " warnings=" << stats.warnings
           << " analysis_complete=" << (AnalysisComplete() ? "true" : "false") << " allocations=" << stats.allocations
           << " frees=" << stats.frees << " pending_device_operations=" << stats.pendingDeviceOperations
           << " dropped_device_operations=" << stats.droppedDeviceOperations
           << " unverified_lifetime_accesses=" << stats.unverifiedLifetimeAccesses
           << " failed_launches=" << stats.failedLaunches << " failed_synchronizations=" << stats.failedSynchronizations
           << " inexact_device_operations=" << stats.inexactDeviceOperations
           << " host_operations=" << stats.hostOperations
           << " incomplete_host_operations=" << stats.incompleteHostOperations
           << " unconfirmed_launches=" << (unfinishedLaunches_ ? "true" : "false");
    return output.str();
}
} // namespace npucheck
