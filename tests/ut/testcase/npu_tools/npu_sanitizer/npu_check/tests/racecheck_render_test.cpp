/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include "diagnostic/report_renderer.h"
#include "checker/racecheck_reports.h"
#include "diagnostic/report/device_call_stack.h"
#include "diagnostic/report/report_assembler.h"
#include "acl_san/aclsan_cbdata_device.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::vector<std::uint64_t> queriedPcs;
}

extern "C" AclsanStatus aclsanGetDeviceCallStack(uint64_t pc, AclsanDeviceCallStack* stack)
{
    queriedPcs.push_back(pc);
    if (pc == 0x555) {
        throw std::bad_alloc();
    }
    if (pc == 0x222) {
        return ACLSAN_STATUS_ERROR_NOT_SUPPORTED;
    }
    stack->binaryId = pc + 1;
    stack->depth = 1;
    // 测试一个提供源代码栈帧、但不回显查询 PC 的解析器。
    stack->pc = 0;
    if (pc == 0x111) {
        std::strcpy(stack->frames[0].functionName, "CopyIn");
        std::strcpy(stack->frames[0].fileName, "/path/kernel.asc");
        stack->frames[0].line = 35;
        stack->frames[0].column = 9;
    }
    return pc == 0x444 ? ACLSAN_STATUS_ERROR_MAX_LIMIT_REACHED : ACLSAN_STATUS_SUCCESS;
}

namespace {
using namespace npucheck;
void Require(bool condition, const std::string& description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}
void Contains(const std::string& text, const std::string& expected)
{
    Require(text.find(expected) != std::string::npos, "expected output: " + expected);
}

AclsanDeviceMemoryAccessData Access(uint32_t mode, uint64_t begin, uint64_t end, uint64_t pc, uint32_t pipe)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.header.deviceId = 2;
    data.header.launchId = 42;
    data.header.phyCoreId = 10;
    data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.blockId = 1;
    data.header.pipeline = pipe;
    data.header.pc = pc;
    data.header.instrExecId = pc;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    data.accessMode = mode;
    data.address = begin;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = end - begin;
    return data;
}
NpuCheckRacecheckReport MakeRace()
{
    auto records = std::make_shared<std::vector<AclsanDeviceMemoryAccessData>>();
    records->push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 0x400, 0x111, ACLSAN_DEVICE_PIPE_MTE2));
    records->push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x200, 0x600, 0x222, ACLSAN_DEVICE_PIPE_VECTOR));
    return MakeRacecheckDiagnostics({{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0x200, 0x400}}, *records).front();
}
std::string Render(const NpuCheckRacecheckReport& report)
{
    std::string text;
    Require(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &text) == ReportRenderStatus::SUCCESS,
        "valid race report renders");
    return text;
}
ReportRecord Assemble(const NpuCheckRacecheckReport& report)
{
    CheckerReport input = report;
    auto result = AssembleReport(input);
    Require(result.status == ReportRenderStatus::SUCCESS, "valid evidence assembles");
    Require(result.reportId == report.reportId, "assembly retains report ID");
    return std::move(result.record);
}
void TestCurrentPreviousRanges()
{
    const auto text = Render(MakeRace());
    Contains(text, "WARNING:[RACECHECK] RAW (read after write) race detected.");
    Contains(text, "current access: READ by aicore (10) type (AIV) block (1) pipe (PIPE_V), range [0x200, 0x600)");
    Contains(text, "previous access: WRITE by aicore (10) type (AIV) block (1) pipe (PIPE_MTE2), range [0x0, 0x400)");
    Contains(text, "memory: UB in aicore (10) type (AIV), overlap [0x200, 0x400), 512 bytes");
    Require(text.find("device (2) launch (42)") == std::string::npos, "race output omits device and launch");
    Contains(text, "Current Device Frames:");
    Contains(text, "Previous Device Frames:");
    Contains(text, "pc 0x111");
    Contains(text, "pc 0x222");
    Contains(text, "unavailable");
}
void TestGmAtomicAndLargeOverlap()
{
    auto report = MakeRace();
    report.kind = ConflictKind::WAW;
    report.key = {2, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0};
    report.previous.memorySpace = report.current.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    report.previous.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    report.current.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    report.previous.layout.range.bytes = report.current.layout.range.bytes = 0x100000020ULL;
    report.current.address = report.checkRange.begin = 0;
    report.checkRange.end = 0x100000010ULL;
    report.previousRange = report.currentRange = {0, 0x100000020ULL};
    report.current.header.phyCoreId = 21;
    report.current.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    const auto text = Render(report);
    Contains(text, "WAW (write after write) race detected.");
    Contains(text, "previous access: ATOMIC_WRITE");
    Contains(text, "current access: WRITE by aicore (21) type (AIC)");
    Contains(text, "memory: GM, overlap [0x0, 0x100000010), 4294967312 bytes");
    report.kind = ConflictKind::WAR;
    report.previous.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    report.current.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    Contains(Render(report), "WAR (write after read) race detected.");
    report.previous.memorySpace = report.current.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
    report.previous.header.blockType = report.current.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    report.current.header.phyCoreId = 10;
    report.key = {2, ACLSAN_DEVICE_MEMORY_SPACE_L1, 10};
    Contains(Render(report), "memory: L1 in aicore (10) type (AIC)");
}
void ExpectInvalid(const NpuCheckRacecheckReport& report, const std::string& reason)
{
    std::string text = "stale text";
    Require(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &text) ==
            ReportRenderStatus::INVALID_ARGUMENT,
        reason);
    Require(text.empty(), "invalid report clears text");
    CheckerReport input = report;
    queriedPcs.clear();
    Require(AssembleReport(input).status == ReportRenderStatus::INVALID_ARGUMENT, reason);
    Require(queriedPcs.empty(), "invalid evidence never queries call stacks");
}
void TestInvalidRangesAndModes()
{
    auto report = MakeRace();
    report.checkRange.end = report.checkRange.begin;
    ExpectInvalid(report, "empty overlap is rejected");
    report = MakeRace();
    report.checkRange.begin = report.checkRange.end + 1;
    ExpectInvalid(report, "reversed overlap is rejected");
    report = MakeRace();
    report.previousRange.end = report.previousRange.begin;
    ExpectInvalid(report, "empty access is rejected");
    report = MakeRace();
    report.currentRange.begin = 0x300;
    ExpectInvalid(report, "overlap before current contiguous segment is rejected");
    report = MakeRace();
    report.checkRange.end = 0x401;
    ExpectInvalid(report, "overlap outside previous contiguous segment is rejected");
    report = MakeRace();
    report.current.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    ExpectInvalid(report, "different memory spaces are rejected");
    report = MakeRace();
    report.current.accessMode = 255;
    ExpectInvalid(report, "undefined access mode is rejected");
    report = MakeRace();
    ++report.current.header.launchId;
    ExpectInvalid(report, "cross-launch evidence is rejected");
    report = MakeRace();
    ++report.current.header.deviceId;
    ExpectInvalid(report, "cross-device evidence is rejected");
}
void TestUnsymbolizedStackRetainsQueriedPc()
{
    NpuCheckInitcheckReport report;
    report.common.exec.pc = 0x333;
    PopulateDeviceCallStack(report);
    Require(report.common.stackCount == 1, "stack enrichment inserts one stack");
    Require(report.common.stacks[0].frames[0].pc == 0x333, "unsymbolized frame retains queried PC");
}
void TestResolveDeviceCallStack()
{
    queriedPcs.clear();
    const auto known = ResolveDeviceCallStack(0x111);
    Require(known.status == ACLSAN_STATUS_SUCCESS, "resolver preserves status");
    Require(known.binaryId == 0x112 && known.frames.size() == 1, "resolver returns binary and frames");
    Require(known.frames[0].pc == 0x111 && known.frames[0].function == "CopyIn", "resolver preserves source and PC");
    const auto missing = ResolveDeviceCallStack(0x222);
    Require(
        missing.status == ACLSAN_STATUS_ERROR_NOT_SUPPORTED && missing.frames.empty(),
        "unsupported PC returns no frames");
    const auto truncated = ResolveDeviceCallStack(0x444);
    Require(
        truncated.status == ACLSAN_STATUS_ERROR_MAX_LIMIT_REACHED && truncated.frames.size() == 1,
        "truncated stacks retain available frames");
    const auto failed = ResolveDeviceCallStack(0x555);
    Require(failed.status != ACLSAN_STATUS_SUCCESS && failed.frames.empty(), "resolver isolates exceptions");
    const auto count = queriedPcs.size();
    const auto zero = ResolveDeviceCallStack(0);
    Require(zero.status == ACLSAN_STATUS_ERROR_INVALID_STATE && zero.frames.empty(), "zero PC is unresolved");
    Require(queriedPcs.size() == count, "zero PC does not query symbolizer");

    NpuCheckInitcheckReport enriched;
    enriched.common.exec.pc = 0x111;
    PopulateDeviceCallStack(enriched);
    enriched.common.exec.pc = 0x555;
    PopulateDeviceCallStack(enriched);
    Require(
        enriched.common.stackCount == 1 && enriched.common.stacks[0].frames.size() == 1,
        "failed optional enrichment retains an existing stack");
    Require(enriched.common.stacks[0].frames[0].function == "CopyIn", "failed enrichment retains source location");
}
void TestIndependentRaceStacks()
{
    const auto report = MakeRace();
    queriedPcs.clear();
    ReportRecord output;
    Require(NormalizeRacecheckReport(report, &output) == ReportRenderStatus::SUCCESS, "resolve evidence");
    Require(queriedPcs.empty(), "normalization defers call stacks until final assembly");
    output = Assemble(report);
    Require(
        output.fields.at("firstFunction") == "CopyIn", "symbolized previous location remains available to templates");
    Require(queriedPcs == std::vector<std::uint64_t>{0x222, 0x111}, "both actual PCs are independently resolved");
    Require(
        report.previous.header.pc == 0x111 && report.current.header.pc == 0x222,
        "symbolization leaves raw records unchanged");
    std::string text;
    Require(RenderReportRecord(output, {}, &text) == ReportRenderStatus::SUCCESS, "symbolized output renders");
    const auto current = text.find("Current Device Frames:");
    const auto previous = text.find("Previous Device Frames:");
    Require(current < previous, "current stack precedes previous stack");
    Contains(text.substr(current, previous - current), "pc 0x222");
    Contains(text.substr(current, previous - current), "Call stack unavailable.");
    Contains(text.substr(previous), "#0 CopyIn in /path/kernel.asc:35:9");
    output = Assemble(report);
    Require(output.stacks.size() == 2, "repeated assembly does not duplicate stack roles");
    auto unsymbolized = MakeRace();
    unsymbolized.current.header.pc = 0x333;
    Require(
        NormalizeRacecheckReport(unsymbolized, &output) == ReportRenderStatus::SUCCESS,
        "resolve unsymbolized evidence");
    output = Assemble(unsymbolized);
    Require(RenderReportRecord(output, {}, &text) == ReportRenderStatus::SUCCESS, "unsymbolized output renders");
    Contains(text, "#0 pc 0x333");
    Require(output.fields.at("binaryId") == std::to_string(0x334), "final assembly retains binary ID");
    auto failed = MakeRace();
    failed.current.header.pc = 0x555;
    output = Assemble(failed);
    Require(RenderReportRecord(output, {}, &text) == ReportRenderStatus::SUCCESS, "resolver failure still renders");
    Contains(text, "pc 0x555");
    Contains(text, "#0 CopyIn in /path/kernel.asc:35:9");
    auto noPc = MakeRace();
    noPc.current.header.pc = 0;
    Require(NormalizeRacecheckReport(noPc, &output) == ReportRenderStatus::SUCCESS, "resolve missing-PC evidence");
    output = Assemble(noPc);
    Require(RenderReportRecord(output, {}, &text) == ReportRenderStatus::SUCCESS, "missing-PC output renders");
    Contains(text, "pc 0x0");
    Contains(text, "Call stack unavailable.");
}
void TestAtomicSemanticRenderingAndSummary()
{
    auto records = std::make_shared<std::vector<AclsanDeviceMemoryAccessData>>();
    records->push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 0x400, 0x111, ACLSAN_DEVICE_PIPE_MTE2));
    records->push_back(
        Access(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX, 0x200, 0x600, 0x222, ACLSAN_DEVICE_PIPE_VECTOR));
    const auto report =
        MakeRacecheckDiagnostics({{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0x200, 0x400}}, *records).front();
    ReportRecord output;
    Require(NormalizeRacecheckReport(report, &output) == ReportRenderStatus::SUCCESS, "resolve semantic race");
    output = Assemble(report);
    std::string text;
    Require(RenderReportRecord(output, {}, &text) == ReportRenderStatus::SUCCESS, "render semantic race");
    Contains(text, "WARNING:[RACECHECK] ATOMIC_SEMANTIC race detected.");
    Contains(text, "current access: ATOMIC_WRITE_MAX");
    Contains(text, "previous access: ATOMIC_WRITE_ADD");
    Contains(text, "overlap [0x200, 0x400), 512 bytes");
    Contains(text, "Current Device Frames:");
    Contains(text, "Previous Device Frames:");
    Require(
        RenderNpuCheckReportBundle({NpuCheckReportRecord::From(report)}, {}, &text) == ReportRenderStatus::SUCCESS,
        "render semantic race bundle");
    Contains(text, "RACECHECK SUMMARY: 1 hazard displayed");
}
} // namespace

int main()
{
    auto fb = MakeRace();
    fb.key.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_FB;
    fb.previous.memorySpace = fb.current.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_FB;
    Contains(Render(fb), "memory: FB in aicore (10) type (AIV)");
    TestCurrentPreviousRanges();
    TestGmAtomicAndLargeOverlap();
    TestInvalidRangesAndModes();
    TestUnsymbolizedStackRetainsQueriedPc();
    TestResolveDeviceCallStack();
    TestIndependentRaceStacks();
    TestAtomicSemanticRenderingAndSummary();
    std::cout << "racecheck render tests passed\n";
}
