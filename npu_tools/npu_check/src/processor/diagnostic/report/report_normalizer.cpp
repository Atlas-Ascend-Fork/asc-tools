/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report/report_normalizer.h"

#include "diagnostic/report/report_catalog.h"
#include "diagnostic/report/report_fields.h"
#include "diagnostic/report/synccheck_adapter.h"

#include <type_traits>

namespace npucheck::detail {
namespace {

const char* MemcheckPatternName(NpuCheckReportPattern pattern) { return PatternName(ReportTool::MEMCHECK, pattern); }

const char* InitcheckPatternName(NpuCheckReportPattern pattern) { return PatternName(ReportTool::INITCHECK, pattern); }

const char* RacecheckPatternName(NpuCheckReportPattern pattern) { return PatternName(ReportTool::RACECHECK, pattern); }

const char* SoccheckPatternName(NpuCheckReportPattern pattern) { return PatternName(ReportTool::SOCCHECK, pattern); }

bool ValidateReportCommon(
    const NpuCheckReportCommon& common, ReportTool expectedTool, NpuCheckReportPattern envelopePattern)
{
    if (common.tool != expectedTool || common.pattern != envelopePattern ||
        FindPatternDescriptor(expectedTool, common.pattern) == nullptr || common.stackCount > kNpuCheckReportStackMax) {
        return false;
    }

    for (std::uint32_t i = 0; i < common.stackCount; ++i) {
        const int role = static_cast<int>(common.stacks[i].role);
        const int format = static_cast<int>(common.stacks[i].format);
        if (role < static_cast<int>(ReportStackRole::FAULT_DEVICE) ||
            role > static_cast<int>(ReportStackRole::HOST_API_CALL) ||
            format < static_cast<int>(ReportStackFormat::NONE) ||
            format > static_cast<int>(ReportStackFormat::FRAMES) ||
            common.stacks[i].frames.size() > kNpuCheckReportFrameMax) {
            return false;
        }
        for (std::uint32_t j = i + 1; j < common.stackCount; ++j) {
            if (common.stacks[i].role == common.stacks[j].role) {
                return false;
            }
        }
    }
    return true;
}

ReportRecord ToReportRecord(const NpuCheckMemcheckReport& report)
{
    ReportFields fields;
    PutExecFields(report.common.exec, &fields);
    PutFaultLocationFields(report.common, &fields);
    PutAccessFields(report.access, &fields);
    PutAllocationFields(report.allocation, &fields);
    PutDefaultHostFields(&fields);
    fields["capacityContext"] = "";
    const auto space = report.access.memorySpace;
    if (space == NpuCheckReportMemorySpace::L1 || space == NpuCheckReportMemorySpace::L0_A ||
        space == NpuCheckReportMemorySpace::L0_B || space == NpuCheckReportMemorySpace::L0_C) {
        fields["capacityContext"] = " (" + std::string(MemorySpaceName(space)) + " capacity " +
                                    std::to_string(report.allocation.bytes) + " bytes, valid offsets [0, " +
                                    std::to_string(report.allocation.bytes) + "))";
    }
    fields["apiName"] = report.apiName;
    fields["apiErrorName"] = report.apiErrorName;
    fields["apiErrorCode"] = std::to_string(report.apiErrorCode);
    fields["apiErrorMessage"] = report.apiErrorMessage;
    if (report.common.pattern == NpuCheckReportPattern::MEMCHECK_LEAK) {
        fields["space"] = MemorySpaceName(report.allocation.memorySpace);
    }
    return ReportRecord{
        {ReportTool::MEMCHECK, MemcheckPatternName(report.common.pattern)},
        report.common.severity,
        fields,
        ActiveCallStacks(report.common)};
}

ReportRecord ToReportRecord(const NpuCheckInitcheckReport& report)
{
    ReportFields fields;
    PutExecFields(report.common.exec, &fields);
    PutFaultLocationFields(report.common, &fields);
    PutAccessFields(report.access, &fields);
    PutAllocationFields(report.allocation, &fields);
    PutDefaultHostFields(&fields);
    fields["firstUninitAddress"] = Hex(report.firstUninitAddress);
    fields["firstUninitOffset"] = Hex(report.firstUninitOffset);
    fields["uninitBytes"] = std::to_string(report.uninitBytes);
    fields["initializedBytes"] = std::to_string(report.initializedBytes);
    fields["unusedBytes"] = std::to_string(report.unusedBytes);
    fields["unusedPercent"] = std::to_string(report.unusedPercent);
    fields["registerId"] = std::to_string(report.registerId);
    fields["registerName"] = report.registerName;
    fields["setterInstruction"] = report.setterInstruction;
    fields["consumerInstruction"] = report.consumerInstruction;
    if (report.common.pattern == NpuCheckReportPattern::INITCHECK_UNUSED_MEMORY) {
        fields["space"] = MemorySpaceName(report.allocation.memorySpace);
    }
    return ReportRecord{
        {ReportTool::INITCHECK, InitcheckPatternName(report.common.pattern)},
        report.common.severity,
        fields,
        ActiveCallStacks(report.common)};
}

ReportRecord ToReportRecord(const NpuCheckRacecheckReport& report)
{
    ReportFields fields;
    PutExecFields(report.common.exec, &fields);
    PutFaultLocationFields(report.common, &fields);
    PutAccessFields(report.first.access, &fields);
    fields["blockId"] = std::to_string(report.first.exec.blockId);
    fields["firstAccess"] = AccessModeName(report.first.access.accessMode);
    fields["secondAccess"] = AccessModeName(report.second.access.accessMode);
    fields["firstCoreId"] = FormatCoreId(report.first.exec.phyCoreId);
    fields["firstType"] = FormatBlockType(report.first.exec.blockType);
    fields["firstBlock"] = std::to_string(report.first.exec.blockId);
    fields["firstPipe"] = OrUnknown(report.first.exec.pipeName);
    fields["firstFunction"] = OrUnknown(report.first.exec.function);
    fields["firstOffset"] = Hex(report.first.exec.offset);
    fields["firstFile"] = OrUnknown(report.first.exec.file);
    fields["firstLine"] = std::to_string(report.first.exec.line);
    fields["firstLocation"] = FormatLocation(report.first.exec, false);
    fields["firstLaunchId"] = FormatLaunchId(report.first.exec.launchId);
    fields["secondCoreId"] = FormatCoreId(report.second.exec.phyCoreId);
    fields["secondType"] = FormatBlockType(report.second.exec.blockType);
    fields["secondBlock"] = std::to_string(report.second.exec.blockId);
    fields["secondPipe"] = OrUnknown(report.second.exec.pipeName);
    fields["secondFunction"] = OrUnknown(report.second.exec.function);
    fields["secondOffset"] = Hex(report.second.exec.offset);
    fields["secondFile"] = OrUnknown(report.second.exec.file);
    fields["secondLine"] = std::to_string(report.second.exec.line);
    fields["secondLocation"] = FormatLocation(report.second.exec, false);
    fields["secondLaunchId"] = FormatLaunchId(report.second.exec.launchId);
    if (const ReportFrame* frame = FirstStructuredFrame(report.common, ReportStackRole::RELATED_ACCESS_A)) {
        PutFrameLocationFields(*frame, "first", &fields);
    }
    if (const ReportFrame* frame = FirstStructuredFrame(report.common, ReportStackRole::RELATED_ACCESS_B)) {
        PutFrameLocationFields(*frame, "second", &fields);
    }
    if (report.common.pattern == NpuCheckReportPattern::RACECHECK_CROSS_PIPE_RACE) {
        fields["coreId"] = FormatCoreId(report.first.exec.phyCoreId);
    } else if (report.common.pattern == NpuCheckReportPattern::RACECHECK_INVALID_REMOTE_ACCESS) {
        fields["location"] = "at " + fields["firstLocation"];
        fields["coreId"] = FormatCoreId(report.first.exec.phyCoreId);
        fields["blockType"] = FormatBlockType(report.first.exec.blockType);
        fields["blockId"] = std::to_string(report.first.exec.blockId);
        fields["pipeName"] = OrUnknown(report.first.exec.pipeName);
        fields["launchId"] = FormatLaunchId(report.first.exec.launchId);
    }
    fields["hazardCount"] = std::to_string(report.hazardCount);
    fields["overlapBytes"] = std::to_string(report.overlapBytes);
    fields["ordering"] = report.ordering;
    fields["coverage"] = report.coverage;
    fields["unorderedPeers"] = report.unorderedPeers ? "true" : "false";
    fields["raceDevice"] = std::to_string(report.first.exec.deviceId);
    fields["firstInstruction"] = std::to_string(report.first.exec.instrExecId);
    fields["secondInstruction"] = std::to_string(report.second.exec.instrExecId);
    std::string ranges;
    for (const auto& range : report.overlapRanges) {
        if (!ranges.empty())
            ranges += ", ";
        ranges += "[0x" + Hex(range.first) + ", 0x" + Hex(range.second) + ")";
    }
    fields["overlapRanges"] = ranges.empty() ? "unknown" : ranges;
    fields["currentValue"] = HexWithPrefix(report.currentValue);
    fields["incomingValue"] = HexWithPrefix(report.incomingValue);
    fields["scope"] = std::to_string(report.scope);
    fields["remoteState"] = "is unavailable";
    return ReportRecord{
        {ReportTool::RACECHECK, RacecheckPatternName(report.common.pattern)},
        report.common.severity,
        fields,
        ActiveCallStacks(report.common)};
}

ReportRecord ToReportRecord(const NpuCheckSoccheckReport& report)
{
    ReportFields fields;
    PutExecFields(report.common.exec, &fields);
    PutFaultLocationFields(report.common, &fields);
    PutPrefixedExecFields(report.producer, "producer", &fields);
    PutPrefixedExecFields(report.consumer, "consumer", &fields);
    fields["stateKind"] = std::to_string(report.state.stateKind);
    fields["scope"] = std::to_string(report.state.scope);
    fields["registerId"] = std::to_string(report.state.registerId);
    fields["ownerCoreId"] = FormatCoreId(report.state.ownerCoreId);
    fields["stateId"] = std::to_string(report.state.stateId);
    fields["oldValue"] = Hex(report.state.oldValue);
    fields["newValue"] = Hex(report.state.newValue);
    fields["expectedValue"] = Hex(report.state.expectedValue);
    fields["observedValue"] = Hex(report.state.observedValue);
    return ReportRecord{
        {ReportTool::SOCCHECK, SoccheckPatternName(report.common.pattern)},
        report.common.severity,
        fields,
        ActiveCallStacks(report.common)};
}

template <typename Report>
inline constexpr ReportTool kReportTool = ReportTool::MEMCHECK;

template <>
inline constexpr ReportTool kReportTool<NpuCheckInitcheckReport> = ReportTool::INITCHECK;

template <>
inline constexpr ReportTool kReportTool<NpuCheckRacecheckReport> = ReportTool::RACECHECK;

template <>
inline constexpr ReportTool kReportTool<NpuCheckSynccheckReport> = ReportTool::SYNCCHECK;

template <>
inline constexpr ReportTool kReportTool<NpuCheckSoccheckReport> = ReportTool::SOCCHECK;

template <typename Report>
bool ValidateToolSpecific(const Report&)
{
    return true;
}

bool ValidateToolSpecific(const NpuCheckRacecheckReport& report)
{
    return report.common.pattern != NpuCheckReportPattern::RACECHECK_CROSS_PIPE_RACE ||
           report.first.exec.phyCoreId == report.second.exec.phyCoreId;
}

template <typename Report>
ReportRenderStatus NormalizeTypedReport(const Report& report, ReportRecord* out)
{
    *out = ToReportRecord(report);
    return ReportRenderStatus::SUCCESS;
}

ReportRenderStatus NormalizeTypedReport(const NpuCheckSynccheckReport& report, ReportRecord* out)
{
    return NormalizeSynccheckReport(report, out);
}

} // namespace

ReportRenderStatus NormalizeReport(const NpuCheckReportRecord& record, ReportRecord* out)
{
    if (out == nullptr) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    const int tool = static_cast<int>(record.tool);
    if (tool < static_cast<int>(ReportTool::MEMCHECK) || tool > static_cast<int>(ReportTool::SOCCHECK)) {
        return ReportRenderStatus::UNKNOWN_TEMPLATE;
    }
    return std::visit(
        [&record, out](const auto& payload) -> ReportRenderStatus {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, std::monostate>) {
                return ReportRenderStatus::INVALID_ARGUMENT;
            } else {
                using Report = std::remove_const_t<std::remove_pointer_t<Payload>>;
                if (payload == nullptr || record.tool != kReportTool<Report> ||
                    !ValidateReportCommon(payload->common, kReportTool<Report>, record.pattern) ||
                    !ValidateToolSpecific(*payload)) {
                    return ReportRenderStatus::INVALID_ARGUMENT;
                }
                return NormalizeTypedReport(*payload, out);
            }
        },
        record.GetPayload());
}

} // namespace npucheck::detail
