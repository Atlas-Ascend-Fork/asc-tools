/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_REPORT_FIELDS_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_REPORT_FIELDS_H

#include "diagnostic/report_renderer.h"

namespace npucheck::detail {

std::string FieldOr(const ReportFields& fields, const std::string& key, const char* fallback);
const char* MemorySpaceName(NpuCheckReportMemorySpace space);
const char* AccessModeName(NpuCheckReportAccessMode mode);
const char* AccessModeName(AclsanDeviceMemoryAccessMode mode);
std::string Hex(std::uint64_t value);
std::string HexWithPrefix(std::uint64_t value);
std::string OrUnknown(const std::string& value);
std::string FormatCoreId(std::uint32_t coreId);
std::string FormatBlockType(std::uint32_t blockType);
std::string FormatLaunchId(std::uint64_t launchId);
std::string FormatLocation(const NpuCheckReportExecContext& exec, bool includeAt);
std::string FormatLocation(const ReportFrame& frame, bool includeAt);

void PutExecFields(const NpuCheckReportExecContext& exec, ReportFields* fields);
void PutPrefixedExecFields(const NpuCheckReportExecContext& exec, const std::string& prefix, ReportFields* fields);
void PutAccessFields(const NpuCheckReportMemoryAccess& access, ReportFields* fields);
void PutAllocationFields(const NpuCheckReportAllocation& allocation, ReportFields* fields);
void PutDefaultHostFields(ReportFields* fields);
void PutFrameLocationFields(const ReportFrame& frame, const std::string& prefix, ReportFields* fields);
void PutFaultLocationFields(const NpuCheckReportCommon& common, ReportFields* fields);

std::vector<ReportCallStack> ActiveCallStacks(const NpuCheckReportCommon& common);
const ReportCallStack* FindStackByRole(const NpuCheckReportCommon& common, ReportStackRole role);
const ReportFrame* FirstStructuredFrame(const NpuCheckReportCommon& common, ReportStackRole role);

} // namespace npucheck::detail

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_REPORT_FIELDS_H
