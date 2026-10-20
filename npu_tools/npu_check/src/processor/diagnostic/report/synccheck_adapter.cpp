/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report/synccheck_adapter.h"

#include "acl_san/aclsan_cbdata_device.h"
#include "diagnostic/pipeline_name.h"
#include "diagnostic/report/report_catalog.h"
#include "diagnostic/report/report_fields.h"

#include <sstream>

namespace npucheck::detail {
namespace {
void PutSyncPointFields(
    const NpuCheckReportCommon& common, const NpuCheckSyncPoint& point, const std::string& prefix, ReportFields* fields)
{
    PutPrefixedExecFields(point.exec, prefix, fields);
    (*fields)[prefix + "Operation"] = OrUnknown(point.operation);
    if (point.stackRole != ReportStackRole::NONE) {
        if (const ReportFrame* frame = FirstStructuredFrame(common, point.stackRole)) {
            PutFrameLocationFields(*frame, prefix, fields);
        }
    }
}

bool ExecContextsEqual(const NpuCheckReportExecContext& lhs, const NpuCheckReportExecContext& rhs)
{
    return lhs.launchId == rhs.launchId && lhs.binaryId == rhs.binaryId && lhs.functionId == rhs.functionId &&
           lhs.instrExecId == rhs.instrExecId && lhs.serialNo == rhs.serialNo && lhs.pc == rhs.pc &&
           lhs.offset == rhs.offset && lhs.deviceId == rhs.deviceId && lhs.phyCoreId == rhs.phyCoreId &&
           lhs.blockId == rhs.blockId && lhs.blockType == rhs.blockType && lhs.pipeId == rhs.pipeId &&
           lhs.siteId == rhs.siteId && lhs.line == rhs.line && lhs.column == rhs.column &&
           lhs.function == rhs.function && lhs.file == rhs.file && lhs.pipeName == rhs.pipeName &&
           lhs.kernelName == rhs.kernelName;
}

bool HasStackRole(const NpuCheckReportCommon& common, ReportStackRole role)
{
    return FindStackByRole(common, role) != nullptr;
}

bool ValidatePointStackReference(
    const NpuCheckReportCommon& common, const NpuCheckSyncPoint& point, ReportStackRole expectedRole)
{
    const bool hasStack = HasStackRole(common, expectedRole);
    if (point.stackRole == ReportStackRole::NONE) {
        return !hasStack;
    }
    if (!point.hasExecContext || point.stackRole != expectedRole || !hasStack) {
        return false;
    }
    const ReportFrame* frame = FirstStructuredFrame(common, expectedRole);
    return frame == nullptr || frame->pc == 0 || point.exec.pc == 0 || frame->pc == point.exec.pc;
}

bool IsExpectedPoint(const NpuCheckSyncPoint& point)
{
    return !point.hasExecContext && point.stackRole == ReportStackRole::NONE && !point.operation.empty() &&
           ExecContextsEqual(point.exec, NpuCheckReportExecContext{});
}

bool IsActualPoint(const NpuCheckSyncPoint& point) { return point.hasExecContext && !point.operation.empty(); }

bool IsEmptyPoint(const NpuCheckSyncPoint& point)
{
    return !point.hasExecContext && point.operation.empty() && point.stackRole == ReportStackRole::NONE &&
           ExecContextsEqual(point.exec, NpuCheckReportExecContext{});
}

const char* PairKindName(NpuCheckSyncPairKind pairKind)
{
    switch (pairKind) {
        case NpuCheckSyncPairKind::SET_WAIT_FLAG:
            return "SET_WAIT_FLAG";
        case NpuCheckSyncPairKind::GET_RLS_BUF:
            return "GET_RLS_BUF";
        case NpuCheckSyncPairKind::UNKNOWN:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* PrimitiveKindName(NpuCheckSyncPrimitiveKind primitiveKind)
{
    switch (primitiveKind) {
        case NpuCheckSyncPrimitiveKind::BARRIER:
            return "BARRIER";
        case NpuCheckSyncPrimitiveKind::SET_WAIT_FLAG:
            return "SET_WAIT_FLAG";
        case NpuCheckSyncPrimitiveKind::GET_RLS_BUF:
            return "GET_RLS_BUF";
        case NpuCheckSyncPrimitiveKind::INSTRUCTION_SEQUENCE:
            return "INSTRUCTION_SEQUENCE";
        case NpuCheckSyncPrimitiveKind::SYNC_OBJECT:
            return "SYNC_OBJECT";
        case NpuCheckSyncPrimitiveKind::UNKNOWN:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool IsValidPrimitiveKind(NpuCheckSyncPrimitiveKind primitiveKind)
{
    const int value = static_cast<int>(primitiveKind);
    return value >= static_cast<int>(NpuCheckSyncPrimitiveKind::BARRIER) &&
           value <= static_cast<int>(NpuCheckSyncPrimitiveKind::SYNC_OBJECT);
}

const char* OpenOperation(NpuCheckSyncPairKind pairKind)
{
    return pairKind == NpuCheckSyncPairKind::SET_WAIT_FLAG ? "SET_FLAG" : "GET_BUF";
}

const char* CloseOperation(NpuCheckSyncPairKind pairKind)
{
    return pairKind == NpuCheckSyncPairKind::SET_WAIT_FLAG ? "WAIT_FLAG" : "RLS_BUF";
}

struct PairingReasonRule {
    NpuCheckSyncMismatchReason reason; // 1. 重复set 2. wait没有set 3. set没有wait
    bool triggerIsOpen;                // 是否为SET_FLAG / GET_BUF
    bool relatedIsOpen;
    bool relatedIsActual; // 关联点是否为实际执行过的操作。true表示前一次实际操作，false表示报告期望出现的操作
    const char* missingPointText; // 期望操作未出现时的补充说明。DUPLICATE_OPEN报expected xx before another xx
    bool emitExpectedCloseBeforeOpen; // 是否额外提示 再次set前应该先wait
};

const PairingReasonRule* FindPairingReasonRule(NpuCheckSyncMismatchReason reason)
{
    static constexpr PairingReasonRule rules[] = {
        {NpuCheckSyncMismatchReason::DUPLICATE_OPEN, true, true, true, "", true},
        {NpuCheckSyncMismatchReason::UNMATCHED_CLOSE, false, true, false,
         "but no matching point was found for this pair key", false},
        {NpuCheckSyncMismatchReason::UNCONSUMED_OPEN, true, false, false,
         "but no matching point was found for this pair key", false},
    };
    for (const PairingReasonRule& rule : rules) {
        if (rule.reason == reason) {
            return &rule;
        }
    }
    return nullptr;
}

bool ValidatePairingReport(const NpuCheckSynccheckReport& report, const NpuCheckSyncPairingError& detail)
{
    const PairingReasonRule* rule = FindPairingReasonRule(detail.reason);
    if (!report.hasRelatedPoint || rule == nullptr ||
        (detail.key.pairKind != NpuCheckSyncPairKind::SET_WAIT_FLAG &&
         detail.key.pairKind != NpuCheckSyncPairKind::GET_RLS_BUF)) {
        return false;
    }
    if ((detail.key.pairKind == NpuCheckSyncPairKind::SET_WAIT_FLAG && detail.key.mode != 0) ||
        (detail.key.pairKind == NpuCheckSyncPairKind::GET_RLS_BUF && detail.key.srcPipe != 0)) {
        return false;
    }
    if ((detail.key.pairKind == NpuCheckSyncPairKind::SET_WAIT_FLAG &&
         report.primitiveKind != NpuCheckSyncPrimitiveKind::SET_WAIT_FLAG) ||
        (detail.key.pairKind == NpuCheckSyncPairKind::GET_RLS_BUF &&
         report.primitiveKind != NpuCheckSyncPrimitiveKind::GET_RLS_BUF)) {
        return false;
    }

    const char* open = OpenOperation(detail.key.pairKind);
    const char* close = CloseOperation(detail.key.pairKind);
    const char* triggerOperation = rule->triggerIsOpen ? open : close;
    const char* relatedOperation = rule->relatedIsOpen ? open : close;
    const bool relatedKindMatches =
        rule->relatedIsActual ? IsActualPoint(report.relatedPoint) : IsExpectedPoint(report.relatedPoint);
    return relatedKindMatches && report.triggerPoint.operation == triggerOperation &&
           report.relatedPoint.operation == relatedOperation;
}

bool ValidateSynccheckReport(const NpuCheckSynccheckReport& report)
{
    if ((report.common.flags & kNpuCheckReportCommonHasExecContext) == 0 || !IsActualPoint(report.triggerPoint) ||
        !ExecContextsEqual(report.common.exec, report.triggerPoint.exec) ||
        !ValidatePointStackReference(report.common, report.triggerPoint, ReportStackRole::SYNC_TRIGGER) ||
        !ValidatePointStackReference(report.common, report.relatedPoint, ReportStackRole::SYNC_RELATED)) {
        return false;
    }
    if ((!report.hasRelatedPoint && !IsEmptyPoint(report.relatedPoint)) ||
        (report.hasRelatedPoint && report.relatedPoint.operation.empty())) {
        return false;
    }

    for (std::uint32_t i = 0; i < report.common.stackCount; ++i) {
        const ReportStackRole role = report.common.stacks[i].role;
        if (role == ReportStackRole::SYNC_PRODUCER || role == ReportStackRole::SYNC_CONSUMER) {
            return false;
        }
    }

    const NpuCheckReportPattern pattern = report.common.pattern;
    switch (pattern) {
        case NpuCheckReportPattern::SYNCCHECK_INTRA_CORE_DIVERGENT:
            return report.detailKind == NpuCheckSyncDetailKind::BARRIER && !report.hasRelatedPoint &&
                   report.primitiveKind == NpuCheckSyncPrimitiveKind::BARRIER &&
                   std::holds_alternative<NpuCheckSyncBarrierError>(report.detail);
        case NpuCheckReportPattern::SYNCCHECK_INTER_CORE_DIVERGENT:
        case NpuCheckReportPattern::SYNCCHECK_PARTICIPANT_MISMATCH:
            return report.detailKind == NpuCheckSyncDetailKind::BARRIER &&
                   report.primitiveKind == NpuCheckSyncPrimitiveKind::BARRIER &&
                   std::holds_alternative<NpuCheckSyncBarrierError>(report.detail) &&
                   (!report.hasRelatedPoint || IsActualPoint(report.relatedPoint) ||
                    IsExpectedPoint(report.relatedPoint));
        case NpuCheckReportPattern::SYNCCHECK_INVALID_ARGUMENT:
        case NpuCheckReportPattern::SYNCCHECK_OBJECT_NOT_INITIALIZED:
            return report.detailKind == NpuCheckSyncDetailKind::OBJECT && !report.hasRelatedPoint &&
                   IsValidPrimitiveKind(report.primitiveKind) &&
                   std::holds_alternative<NpuCheckSyncObjectError>(report.detail);
        case NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH: {
            if (report.detailKind != NpuCheckSyncDetailKind::PAIRING ||
                !std::holds_alternative<NpuCheckSyncPairingError>(report.detail)) {
                return false;
            }
            return ValidatePairingReport(report, std::get<NpuCheckSyncPairingError>(report.detail));
        }
        case NpuCheckReportPattern::SYNCCHECK_DEADLOCK:
            return report.detailKind == NpuCheckSyncDetailKind::OBJECT && IsValidPrimitiveKind(report.primitiveKind) &&
                   std::holds_alternative<NpuCheckSyncObjectError>(report.detail) &&
                   (!report.hasRelatedPoint || IsActualPoint(report.relatedPoint) ||
                    IsExpectedPoint(report.relatedPoint));
        case NpuCheckReportPattern::SYNCCHECK_INSTRUCTION_SEQUENCE_MISMATCH:
            return report.detailKind == NpuCheckSyncDetailKind::SEQUENCE && report.hasRelatedPoint &&
                   report.primitiveKind == NpuCheckSyncPrimitiveKind::INSTRUCTION_SEQUENCE &&
                   std::holds_alternative<NpuCheckSyncSequenceError>(report.detail) &&
                   (IsActualPoint(report.relatedPoint) || IsExpectedPoint(report.relatedPoint));
        default:
            return false;
    }
}

std::string FormatPairKey(const NpuCheckSyncPairKey& key)
{
    std::ostringstream os;
    if (key.pairKind == NpuCheckSyncPairKind::SET_WAIT_FLAG) {
        os << "srcPipe=" << DevicePipelineName(key.srcPipe) << ", dstPipe=" << DevicePipelineName(key.dstPipe)
           << ", id=" << key.id;
    } else {
        os << "pipe=" << DevicePipelineName(key.dstPipe) << ", id=" << key.id << ", mode=" << key.mode;
    }
    return os.str();
}

std::string SyncObjectLine(std::uint64_t objectId, std::uint64_t address)
{
    if (objectId == 0 && address == 0) {
        return "";
    }
    std::ostringstream os;
    os << "=========     sync object";
    if (objectId != 0) {
        os << " 0x" << Hex(objectId);
    }
    if (address != 0) {
        os << (objectId == 0 ? " address" : ", address") << " 0x" << Hex(address);
    }
    os << '\n';
    return os.str();
}

std::string ActualRelatedPointLine(const NpuCheckSyncPoint& point, const ReportFields& fields)
{
    std::ostringstream os;
    os << "=========     related point: " << OrUnknown(point.operation) << " by aicore ("
       << FieldOr(fields, "relatedCoreId", "<unknown>") << ") type (" << FieldOr(fields, "relatedType", "UNKNOWN")
       << ") block (" << FieldOr(fields, "relatedBlock", "0") << ") pipe ("
       << FieldOr(fields, "relatedPipe", "<unknown>") << ") at "
       << FieldOr(fields, "relatedLocation", "pc 0x0 in <unknown>") << '\n';
    return os.str();
}

ReportRecord ToReportRecord(const NpuCheckSynccheckReport& report)
{
    ReportFields fields;
    PutExecFields(report.common.exec, &fields);
    PutSyncPointFields(report.common, report.triggerPoint, "trigger", &fields);
    PutSyncPointFields(report.common, report.relatedPoint, "related", &fields);
    fields["primitiveKind"] = PrimitiveKindName(report.primitiveKind);
    fields["relatedPointLine"] = "";
    fields["expectedOperationLine"] = "";
    fields["objectLine"] = "";
    if (report.hasRelatedPoint && report.relatedPoint.hasExecContext) {
        fields["relatedPointLine"] = ActualRelatedPointLine(report.relatedPoint, fields);
    } else if (report.hasRelatedPoint) {
        fields["relatedPointLine"] = "=========     related point: expected " +
                                     OrUnknown(report.relatedPoint.operation) + ", but no runtime point was observed\n";
    }

    switch (report.detailKind) {
        case NpuCheckSyncDetailKind::BARRIER: {
            const auto& detail = std::get<NpuCheckSyncBarrierError>(report.detail);
            fields["reason"] = OrUnknown(detail.reason);
            fields["scope"] = OrUnknown(detail.scope);
            fields["activeMask"] = Hex(detail.activeMask);
            fields["expectedMask"] = Hex(detail.expectedMask);
            fields["objectLine"] = SyncObjectLine(detail.objectId, 0);
            break;
        }
        case NpuCheckSyncDetailKind::PAIRING: {
            const auto& detail = std::get<NpuCheckSyncPairingError>(report.detail);
            const PairingReasonRule& rule = *FindPairingReasonRule(detail.reason);
            fields["pairKind"] = PairKindName(detail.key.pairKind);
            fields["pairKey"] = FormatPairKey(detail.key);
            const char* open = OpenOperation(detail.key.pairKind);
            const char* close = CloseOperation(detail.key.pairKind);
            fields["reasonText"] = rule.relatedIsActual ? "duplicate " + std::string(open) :
                                                          report.triggerPoint.operation + " without matching " +
                                                              report.relatedPoint.operation;
            if (rule.relatedIsActual) {
                fields["relatedPointLine"] = "=========     related point: previous " + report.relatedPoint.operation +
                                             " at " + fields["relatedLocation"] + " is still pending\n";
            } else {
                fields["relatedPointLine"] = "=========     related point: expected " + report.relatedPoint.operation +
                                             ", " + rule.missingPointText + "\n";
            }
            if (rule.emitExpectedCloseBeforeOpen) {
                fields["expectedOperationLine"] =
                    "=========     expected " + std::string(close) + " before another " + open + "\n";
            }
            break;
        }
        case NpuCheckSyncDetailKind::SEQUENCE: {
            const auto& detail = std::get<NpuCheckSyncSequenceError>(report.detail);
            fields["reason"] = OrUnknown(detail.reason);
            fields["sequenceIndex"] = std::to_string(detail.sequenceIndex);
            fields["activeMask"] = Hex(detail.activeMask);
            break;
        }
        case NpuCheckSyncDetailKind::OBJECT: {
            const auto& detail = std::get<NpuCheckSyncObjectError>(report.detail);
            fields["reason"] = OrUnknown(detail.reason);
            fields["waitingMask"] = Hex(detail.waitingMask);
            fields["timeoutNs"] = std::to_string(detail.timeoutNs);
            fields["objectLine"] = SyncObjectLine(detail.objectId, detail.address);
            break;
        }
    }
    return ReportRecord{
        {ReportTool::SYNCCHECK, PatternName(ReportTool::SYNCCHECK, report.common.pattern)},
        report.common.severity,
        fields,
        ActiveCallStacks(report.common)};
}

} // namespace

ReportRenderStatus NormalizeSynccheckReport(const NpuCheckSynccheckReport& report, ReportRecord* out)
{
    if (out == nullptr || !ValidateSynccheckReport(report)) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    *out = ToReportRecord(report);
    return ReportRenderStatus::SUCCESS;
}

} // namespace npucheck::detail
