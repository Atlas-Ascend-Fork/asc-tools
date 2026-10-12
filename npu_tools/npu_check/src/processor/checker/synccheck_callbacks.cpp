/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/synccheck.h"
#include "tool_manager/kernel_attributes.h"
#include "npu_tool_log.h"
#include <sstream>

namespace npucheck {
const std::vector<npucheck::CallbackSpec>& Synccheck::GetSubscribedID() const
{
    static const std::vector<npucheck::CallbackSpec> callbacks{
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL}};
    return callbacks;
}
bool Synccheck::OnCallback(
    AclsanCallbackDomain domain, [[maybe_unused]] AclsanCallbackId cbid, const void* data,
    npucheck::CheckerReportList& reports)
{
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION) {
        const auto* syncData = static_cast<const AclsanDeviceSyncData*>(data);
        OnDeviceSync(*syncData);
    } else if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        npucheck::AppendCheckerReports(OnSynchronization(), reports);
    } else if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto* launchData = static_cast<const AclsanLaunchData*>(data);
        const npucheck::KernelAttributes attributes = npucheck::QueryKernelAttributes(launchData->function);
        const char* functionName = launchData->functionName == nullptr ? "<unknown>" : launchData->functionName;
        ASCTOOL_DEBUG(
            "kernel attributes launch=%llu function=%p function_name=%s num_blocks=%u "
            "kernel_type=%lld kernel_type_status=%d aic_ratio=%u aiv_ratio=%u kernel_ratio_status=%d "
            "kernel_sched_mode=%lld kernel_sched_mode_status=%d launch_result=%d",
            static_cast<unsigned long long>(launchData->launchId), launchData->function, functionName,
            static_cast<unsigned>(launchData->numBlocks), static_cast<long long>(attributes.kernelType),
            attributes.kernelTypeStatus, static_cast<unsigned>(attributes.aicRatio),
            static_cast<unsigned>(attributes.aivRatio), attributes.kernelRatioStatus,
            static_cast<long long>(attributes.kernelSchedMode), attributes.kernelSchedModeStatus,
            launchData->common.result);

        const auto logFailure = [launchData, functionName](const char* attribute, aclError status) {
            if (status == ACL_SUCCESS) {
                return;
            }
            ASCTOOL_WARNING(
                "kernel attribute query failed launch=%llu function=%p function_name=%s attribute=%s result=%d",
                static_cast<unsigned long long>(launchData->launchId), launchData->function, functionName, attribute,
                status);
        };
        logFailure("ACL_FUNC_ATTR_KERNEL_TYPE", attributes.kernelTypeStatus);
        logFailure("ACL_FUNC_ATTR_KERNEL_RATIO", attributes.kernelRatioStatus);
        logFailure("ACL_FUNC_ATTR_KERNEL_SCHED_MODE", attributes.kernelSchedModeStatus);
    }
    return true;
}
bool Synccheck::HasErrors() const
{
    const auto stats = Stats();
    return stats.duplicateOpens != 0 || stats.unmatchedCloses != 0 || stats.unconsumedOpens != 0;
}
bool Synccheck::AnalysisComplete() const { return Stats().pendingOpens == 0; }
std::string Synccheck::Summary() const
{
    const auto stats = Stats();
    std::ostringstream output;
    output << "tool=synccheck sync_events=" << stats.syncEvents << " synchronizations=" << stats.synchronizationEvents
           << " matched_pairs=" << stats.matchedPairs << " duplicate_opens=" << stats.duplicateOpens
           << " unmatched_closes=" << stats.unmatchedCloses << " unconsumed_opens=" << stats.unconsumedOpens
           << " pending_opens=" << stats.pendingOpens
           << " errors=" << (stats.duplicateOpens + stats.unmatchedCloses + stats.unconsumedOpens) << " warnings=0";
    return output.str();
}
} // namespace npucheck
