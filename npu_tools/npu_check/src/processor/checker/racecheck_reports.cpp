/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/racecheck_reports.h"
#include "diagnostic/pipeline_name.h"
#include "diagnostic/report/report_fields.h"
#include <algorithm>
#include <new>

namespace npucheck {
namespace {
bool SameKey(const PhysicalSpaceKey& lhs, const PhysicalSpaceKey& rhs) { return !(lhs < rhs) && !(rhs < lhs); }

// 只在生成本批报告时缓存布局，报告本身不引用这些记录或展开结果。
using AccessRangeCache = std::map<CbIndex, std::vector<racecheck::ByteRange>>;

racecheck::ByteRange AccessRange(
    CbIndex index, const AccessPair& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records,
    AccessRangeCache& cache, const racecheck::PreparedAccess* current)
{
    const std::vector<racecheck::ByteRange>* ranges = nullptr;
    if (current && index == evidence.curIndex) {
        ranges = &current->ranges;
    } else {
        auto found = cache.find(index);
        if (found == cache.end()) {
            found = cache.emplace(index, racecheck::ExpandAccess(records[index])).first;
        }
        ranges = &found->second;
    }
    const auto containing = std::lower_bound(
        ranges->begin(), ranges->end(), evidence.begin,
        [](const auto& range, uint64_t begin) { return range.end <= begin; });
    if (containing == ranges->end() || containing->begin > evidence.begin || evidence.end > containing->end) {
        throw RacecheckError(RacecheckErrorKind::InvalidEvent, "report overlap crosses a hole or access boundary");
    }
    return *containing;
}

PhysicalSpaceKey ValidateAccesses(
    const AclsanDeviceMemoryAccessData& previous, const AclsanDeviceMemoryAccessData& current,
    ConflictKind* inferredKind, const racecheck::PreparedAccess* prepared = nullptr)
{
    const auto key = prepared ? prepared->key : racecheck::MakeKey(current);
    if (previous.header.launchId != current.header.launchId || !SameKey(key, racecheck::MakeKey(previous))) {
        throw RacecheckError(
            RacecheckErrorKind::InvalidEvent, "race report belongs to another launch or physical table");
    }
    const auto priorMode = racecheck::AccessMode(previous);
    const auto currentMode = prepared ? prepared->mode : racecheck::AccessMode(current);
    const bool priorRead = priorMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    const bool currentRead = currentMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    const bool bothAtomic = racecheck::IsAtomicWrite(priorMode) && racecheck::IsAtomicWrite(currentMode);
    const auto kind = priorRead   ? ConflictKind::WAR :
                      currentRead ? ConflictKind::RAW :
                      bothAtomic  ? ConflictKind::ATOMIC_SEMANTIC :
                                    ConflictKind::WAW;
    if ((priorRead && currentRead) || racecheck::CompatibleAtomicWrites(priorMode, currentMode) ||
        (priorRead && currentMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE &&
         racecheck::SameInstruction(previous, current))) {
        throw RacecheckError(
            RacecheckErrorKind::InvalidEvent, "race report has no matching inter-instruction conflict");
    }
    if (inferredKind) {
        *inferredKind = kind;
    }
    return key;
}

NpuCheckRacecheckReport MakeDiagnostic(
    const AccessPair& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records, AccessRangeCache& cache,
    const racecheck::PreparedAccess* prepared)
{
    if (evidence.prevIndex >= records.size() || evidence.curIndex >= records.size() ||
        evidence.prevIndex == evidence.curIndex || evidence.begin >= evidence.end) {
        throw RacecheckError(RacecheckErrorKind::InvalidEvent, "invalid race report indices or range");
    }
    ConflictKind kind;
    const auto& previous = records[evidence.prevIndex];
    const auto& current = records[evidence.curIndex];
    const auto key = ValidateAccesses(previous, current, &kind, prepared);
    if (evidence.memorySpace != key.memorySpace) {
        throw RacecheckError(RacecheckErrorKind::InvalidEvent, "race report belongs to another memory space");
    }
    const auto previousRange = AccessRange(evidence.prevIndex, evidence, records, cache, prepared);
    const auto currentRange = AccessRange(evidence.curIndex, evidence, records, cache, prepared);
    NpuCheckRacecheckReport report;
    report.launchId = current.header.launchId;
    report.key = key;
    report.previous = previous;
    report.current = current;
    report.previousRange = {previousRange.begin, previousRange.end};
    report.currentRange = {currentRange.begin, currentRange.end};
    report.checkRange = {evidence.begin, evidence.end};
    report.kind = kind;
    return report;
}

NpuCheckReportExecContext ExecContext(const AclsanDeviceMemoryAccessData& data)
{
    NpuCheckReportExecContext exec;
    exec.launchId = data.header.launchId;
    exec.instrExecId = data.header.instrExecId;
    exec.serialNo = data.header.serialNo;
    exec.pc = data.header.pc;
    exec.deviceId = data.header.deviceId;
    exec.phyCoreId = data.header.phyCoreId;
    exec.blockId = data.header.blockId;
    exec.blockType = data.header.blockType;
    exec.pipeId = data.header.pipeline;
    exec.siteId = data.header.siteId;
    exec.pipeName = detail::DevicePipelineName(data.header.pipeline);
    return exec;
}

void PutAccess(
    const AclsanDeviceMemoryAccessData& data, const NpuCheckRacecheckRange& range, const char* prefix,
    const char* accessPrefix, ReportFields& fields)
{
    using namespace detail;
    const auto exec = ExecContext(data);
    PutPrefixedExecFields(exec, prefix, &fields);
    const auto mode = racecheck::AccessMode(data);
    const char* name = "UNKNOWN";
    switch (mode) {
        case ACLSAN_DEVICE_MEMORY_ACCESS_READ:
            name = "READ";
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_WRITE:
            name = "WRITE";
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD:
            name = "ATOMIC_WRITE_ADD";
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN:
            name = "ATOMIC_WRITE_MIN";
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX:
            name = "ATOMIC_WRITE_MAX";
            break;
    }
    fields[std::string(accessPrefix) + "Access"] = name;
    fields[std::string(accessPrefix) + "RangeBegin"] = Hex(range.begin);
    fields[std::string(accessPrefix) + "RangeEnd"] = Hex(range.end);
}

ReportCallStack FallbackStack(const AclsanDeviceMemoryAccessData& data, ReportStackRole role)
{
    ReportCallStack stack;
    stack.role = role;
    stack.format = ReportStackFormat::RAW_TEXT;
    stack.rawText = "pc " + detail::HexWithPrefix(data.header.pc) + "\nCall stack unavailable.\n";
    return stack;
}
std::vector<NpuCheckRacecheckReport> MakeDiagnostics(
    const std::vector<AccessPair>& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records,
    CbIndex currentIndex, const racecheck::PreparedAccess* current)
{
    try {
        AccessRangeCache cache;
        std::vector<NpuCheckRacecheckReport> reports;
        reports.reserve(evidence.size());
        for (const auto& item : evidence) {
            if (current && item.curIndex != currentIndex) {
                throw RacecheckError(
                    RacecheckErrorKind::InvalidEvent, "report evidence does not match prepared current index");
            }
            reports.push_back(MakeDiagnostic(item, records, cache, current));
        }
        return reports;
    } catch (const std::bad_alloc&) {
        throw RacecheckError(
            RacecheckErrorKind::MemoryAllocationFailed, "insufficient host memory while preparing race reports");
    } catch (const std::length_error&) {
        throw RacecheckError(
            RacecheckErrorKind::CapacityExceeded, "container maximum capacity exceeded while preparing race reports");
    }
}
} // namespace

std::vector<NpuCheckRacecheckReport> MakeRacecheckDiagnostics(
    const std::vector<AccessPair>& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records)
{
    return MakeDiagnostics(evidence, records, 0, nullptr);
}

std::vector<NpuCheckRacecheckReport> MakeRacecheckDiagnostics(
    const std::vector<AccessPair>& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records,
    CbIndex currentIndex, const racecheck::PreparedAccess& current)
{
    return MakeDiagnostics(evidence, records, currentIndex, &current);
}

ReportRenderStatus NormalizeRacecheckReport(const NpuCheckRacecheckReport& report, ReportRecord* out)
{
    if (!out || report.checkRange.begin >= report.checkRange.end ||
        report.previousRange.begin > report.checkRange.begin || report.previousRange.end < report.checkRange.end ||
        report.currentRange.begin > report.checkRange.begin || report.currentRange.end < report.checkRange.end) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    try {
        using namespace detail;
        const auto& previous = report.previous;
        const auto& current = report.current;
        ConflictKind inferredKind;
        const auto key = ValidateAccesses(previous, current, &inferredKind);
        if (!SameKey(key, report.key) || report.launchId != current.header.launchId || report.kind != inferredKind) {
            return ReportRenderStatus::INVALID_ARGUMENT;
        }
        const char* pattern = report.kind == ConflictKind::RAW ? "hazard_raw" :
                              report.kind == ConflictKind::WAR ? "hazard_war" :
                              report.kind == ConflictKind::WAW ? "hazard_waw" :
                                                                 "atomic_semantic";
        ReportRecord resolved{{ReportTool::RACECHECK, pattern}, ReportSeverity::WARNING, {}, {}};
        auto& fields = resolved.fields;
        PutExecFields(ExecContext(current), &fields);
        PutAccess(previous, report.previousRange, "first", "previous", fields);
        PutAccess(current, report.currentRange, "second", "current", fields);
        fields["space"] = MemorySpaceName(static_cast<NpuCheckReportMemorySpace>(key.memorySpace));
        fields["overlapBegin"] = Hex(report.checkRange.begin);
        fields["overlapEnd"] = Hex(report.checkRange.end);
        fields["overlapBytes"] = std::to_string(report.checkRange.end - report.checkRange.begin);
        fields["memoryOwner"] = key.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ?
                                    "" :
                                    " in aicore (" + FormatCoreId(key.phyCoreId) + ") type (" +
                                        detail::FormatBlockType(current.header.blockType) + ")";
        resolved.stacks.push_back(FallbackStack(current, ReportStackRole::RELATED_ACCESS_B));
        resolved.stacks.push_back(FallbackStack(previous, ReportStackRole::RELATED_ACCESS_A));
        *out = std::move(resolved);
        return ReportRenderStatus::SUCCESS;
    } catch (const RacecheckError&) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
}

} // namespace npucheck
