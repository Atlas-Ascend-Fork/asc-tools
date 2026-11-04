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
#include "checker/initcheck.h"
#include <chrono>

namespace npucheck {
namespace {
NpuCheckReportCommon HostReport(ReportTool tool, uint64_t id, NpuCheckReportPattern pattern)
{
    NpuCheckReportCommon common;
    common.tool = tool;
    common.reportId = common.groupId = id;
    common.pattern = pattern;
    common.severity = ReportSeverity::ERROR;
    common.timestampNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return common;
}
NpuCheckReportAllocation HostAllocation(const std::optional<Allocation>& source)
{
    NpuCheckReportAllocation allocation;
    if (!source)
        return allocation;
    allocation.allocId = source->resourceId;
    allocation.base = source->base;
    allocation.bytes = source->bytes;
    allocation.allocSerialNo = source->allocSequence;
    allocation.freeSerialNo = source->freeSequence;
    allocation.deviceId = source->deviceId;
    allocation.memorySpace = NpuCheckReportMemorySpace::GM;
    allocation.state = source->freeSequence == 0 ? 1 : 2;
    return allocation;
}
NpuCheckReportMemoryAccess HostAccess(const HostAccessEvidence& source)
{
    NpuCheckReportMemoryAccess access;
    access.memorySpace = NpuCheckReportMemorySpace::GM;
    access.accessMode = source.mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? NpuCheckReportAccessMode::READ :
                                                                          NpuCheckReportAccessMode::WRITE;
    access.address = access.rangeBegin = source.address;
    access.rangeEnd = source.bytes <= UINT64_MAX - source.address ? source.address + source.bytes : UINT64_MAX;
    access.accessBytes = static_cast<uint32_t>(source.bytes);
    return access;
}
} // namespace
void Memcheck::OnHostMemory(const AclsanCallbackCommonData& common, CheckerReportList& reports)
{
    if (common.result != 0 || !analysis_->Managed()) {
        ++stats_.incompleteHostOperations;
        return;
    }
    for (const auto& operand : analysis_->HostOperations()) {
        ++stats_.hostOperations;
        if (operand.evidence.bytes > UINT32_MAX || operand.resource.status == RangeStatus::AMBIGUOUS) {
            ++stats_.incompleteHostOperations;
            continue;
        }
        if (operand.resource.status == RangeStatus::VALID)
            continue;
        NpuCheckMemcheckReport report;
        const auto pattern = operand.resource.status == RangeStatus::USE_AFTER_FREE ?
                                 NpuCheckReportPattern::MEMCHECK_HOST_USE_AFTER_FREE :
                                 NpuCheckReportPattern::MEMCHECK_HOST_INVALID_ACCESS;
        report.common = HostReport(ReportTool::MEMCHECK, nextReportId_++, pattern);
        report.access = HostAccess(operand.evidence);
        report.allocation = HostAllocation(operand.resource.allocation);
        report.apiName = operand.evidence.apiName;
        reports.emplace_back(std::move(report));
        ++stats_.errors;
    }
}
void Initcheck::OnHostMemory(const AclsanCallbackCommonData& common, CheckerReportList& reports)
{
    if (common.result != 0) {
        ++stats_.failedHostWrites;
        return;
    }
    for (const auto& operand : analysis_->HostOperations()) {
        if (operand.evidence.mode != ACLSAN_DEVICE_MEMORY_ACCESS_READ)
            continue;
        if (operand.resource.status == RangeStatus::AMBIGUOUS) {
            ++replayFailures_["host_initialization_unknown"];
            continue;
        }
        if (operand.resource.status != RangeStatus::VALID)
            continue;
        if (!operand.initializationKnown) {
            ++replayFailures_["host_initialization_unknown"];
            continue;
        }
        if (operand.uninitializedBytes == 0)
            continue;
        NpuCheckInitcheckReport report;
        report.common =
            HostReport(ReportTool::INITCHECK, nextReportId_++, NpuCheckReportPattern::INITCHECK_API_READ_UNINITIALIZED);
        report.access = HostAccess(operand.evidence);
        report.allocation = HostAllocation(operand.resource.allocation);
        report.uninitBytes = operand.uninitializedBytes;
        report.firstUninitAddress = operand.firstUninitializedAddress;
        report.firstUninitOffset = operand.firstUninitializedAddress - report.allocation.base;
        report.initializedBytes = operand.evidence.bytes - operand.uninitializedBytes;
        reports.emplace_back(std::move(report));
        ++stats_.errors;
    }
}
} // namespace npucheck
