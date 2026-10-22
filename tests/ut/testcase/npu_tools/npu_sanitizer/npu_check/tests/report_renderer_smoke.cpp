// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "acl_san/aclsan_api.h"
#include "diagnostic/report_renderer.h"
#include "checker/racecheck_reports.h"
#include "diagnostic/report/report_catalog.h"
#include "diagnostic/report/report_fields.h"
#include "diagnostic/report/report_normalizer.h"

#include "acl_san/aclsan_cbdata_device.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using npucheck::NpuCheckInitcheckReport;
using npucheck::NpuCheckMemcheckReport;
using npucheck::NpuCheckRacecheckReport;
using npucheck::NpuCheckReportAccessMode;
using npucheck::NpuCheckReportAllocation;
using npucheck::NpuCheckReportCommon;
using npucheck::NpuCheckReportDistanceKind;
using npucheck::NpuCheckReportMemoryAccess;
using npucheck::NpuCheckReportMemorySpace;
using npucheck::NpuCheckReportPattern;
using npucheck::NpuCheckReportRecord;
using npucheck::NpuCheckSoccheckReport;
using npucheck::NpuCheckSyncBarrierError;
using npucheck::NpuCheckSynccheckReport;
using npucheck::NpuCheckSyncDetailKind;
using npucheck::NpuCheckSyncMismatchReason;
using npucheck::NpuCheckSyncObjectError;
using npucheck::NpuCheckSyncPairingError;
using npucheck::NpuCheckSyncPairKind;
using npucheck::NpuCheckSyncPoint;
using npucheck::NpuCheckSyncPrimitiveKind;
using npucheck::NpuCheckSyncSequenceError;
using npucheck::ReportCallStack;
using npucheck::ReportFields;
using npucheck::ReportFrame;
using npucheck::ReportRecord;
using npucheck::ReportRenderStatus;
using npucheck::ReportSeverity;
using npucheck::ReportStackFormat;
using npucheck::ReportStackRole;
using npucheck::ReportTemplate;
using npucheck::ReportTemplateKey;
using npucheck::ReportTemplateOverrides;
using npucheck::ReportTool;

template <typename Report, typename = void>
struct CanCreateNpuCheckRecordFrom : std::false_type {};

template <typename Report>
struct CanCreateNpuCheckRecordFrom<Report, std::void_t<decltype(NpuCheckReportRecord::From(std::declval<Report>()))>>
    : std::true_type {};

template <typename Descriptor, typename = void>
struct HasDefaultSeverity : std::false_type {};

template <typename Descriptor>
struct HasDefaultSeverity<Descriptor, std::void_t<decltype(std::declval<Descriptor>().defaultSeverity)>>
    : std::true_type {};

template <typename Descriptor, typename = void>
struct HasSummaryTag : std::false_type {};

template <typename Descriptor>
struct HasSummaryTag<Descriptor, std::void_t<decltype(std::declval<Descriptor>().summaryTag)>> : std::true_type {};

template <typename Report, typename = void>
struct HasPattern : std::false_type {};

template <typename Report>
struct HasPattern<Report, std::void_t<decltype(std::declval<Report>().pattern)>> : std::true_type {};

static_assert(
    !CanCreateNpuCheckRecordFrom<NpuCheckMemcheckReport&&>::value,
    "NpuCheckReportRecord must not borrow a temporary report");
static_assert(
    !CanCreateNpuCheckRecordFrom<const NpuCheckMemcheckReport&&>::value,
    "NpuCheckReportRecord must not borrow a const temporary report");
static_assert(std::is_same_v<npucheck::detail::PatternCatalog::key_type, ReportTemplateKey>);
static_assert(!HasDefaultSeverity<npucheck::detail::PatternDescriptor>::value);
static_assert(!HasSummaryTag<npucheck::detail::PatternDescriptor>::value);
static_assert(std::is_same_v<decltype(NpuCheckReportCommon{}.pattern), NpuCheckReportPattern>);
static_assert(std::is_same_v<decltype(NpuCheckReportRecord{}.pattern), NpuCheckReportPattern>);
static_assert(std::is_same_v<decltype(npucheck::detail::PatternDescriptor{}.value), NpuCheckReportPattern>);
static_assert(!HasPattern<NpuCheckMemcheckReport>::value);
static_assert(!HasPattern<NpuCheckInitcheckReport>::value);
static_assert(!HasPattern<NpuCheckRacecheckReport>::value);
static_assert(!HasPattern<NpuCheckSynccheckReport>::value);
static_assert(!HasPattern<NpuCheckSoccheckReport>::value);
TEST(ReportFieldsTest, FormatsBlockTypes)
{
    using npucheck::detail::FormatBlockType;
    static_assert(std::is_same_v<decltype(FormatBlockType(ACLSAN_DEVICE_BLOCK_TYPE_AICORE)), std::string>);

    EXPECT_EQ(FormatBlockType(ACLSAN_DEVICE_BLOCK_TYPE_AICORE), "AICORE");
    EXPECT_EQ(FormatBlockType(ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR), "AIV");
    EXPECT_EQ(FormatBlockType(ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE), "AIC");
    EXPECT_EQ(FormatBlockType(std::numeric_limits<std::uint32_t>::max()), "<unknown>");
}

std::string ReadFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

ReportFields MakePatternSnapshotFields(const ReportTemplate& reportTemplate)
{
    ReportFields fields;
    std::size_t pos = 0;
    while ((pos = reportTemplate.text.find("{{", pos)) != std::string::npos) {
        const std::size_t close = reportTemplate.text.find("}}", pos + 2);
        if (close == std::string::npos) {
            break;
        }
        const std::string field = reportTemplate.text.substr(pos + 2, close - pos - 2);
        fields.emplace(field, "<" + field + ">");
        pos = close + 2;
    }
    return fields;
}

ReportRecord MakeMemcheckInvalidAccessRecord()
{
    return ReportRecord{
        {ReportTool::MEMCHECK, "invalid_access"},
        ReportSeverity::ERROR,
        {
            {"space", "GM"},
            {"access", "read"},
            {"accessBytes", "16"},
            {"function", "kernel"},
            {"offset", "10"},
            {"file", "kernel.cpp"},
            {"line", "42"},
            {"coreId", "3"},
            {"blockType", "AIC"},
            {"blockId", "7"},
            {"pipeName", "PIPE_MTE2"},
            {"launchId", "41"},
            {"address", "1000"},
            {"distanceBytes", "64"},
            {"before|after", "after"},
            {"base", "0fc0"},
            {"bytes", "128"},
            {"hostFunction", "aclrtLaunchKernel"},
            {"hostPc", "400123"},
            {"hostBinary", "libacl.so"},
        }};
}

ReportRecord MakeInitcheckRecord()
{
    return ReportRecord{
        {ReportTool::INITCHECK, "uninitialized_read"},
        ReportSeverity::ERROR,
        {
            {"space", "GM"},
            {"accessBytes", "32"},
            {"function", "init_kernel"},
            {"offset", "18"},
            {"file", "init.cpp"},
            {"line", "21"},
            {"coreId", "4"},
            {"blockType", "AIV"},
            {"blockId", "2"},
            {"pipeName", "PIPE_MTE2"},
            {"launchId", "42"},
            {"address", "3000"},
            {"uninitBytes", "32"},
            {"firstUninitAddress", "3000"},
            {"base", "3000"},
            {"allocationInitializedBytes", "0"},
            {"allocSerialNo", "1"},
            {"deviceId", "0"},
            {"pc", "1234"},
            {"location", "init_kernel+0x12 in init.cpp:21"},
        }};
}

NpuCheckInitcheckReport MakeRegisterInitcheckReport()
{
    NpuCheckInitcheckReport report{};
    report.common.tool = ReportTool::INITCHECK;
    report.common.pattern = NpuCheckReportPattern::INITCHECK_UNINITIALIZED_REGISTER_USE;
    report.common.severity = ReportSeverity::ERROR;
    report.common.flags = npucheck::kNpuCheckReportCommonHasExecContext;
    report.common.exec.pc = 0x180;
    report.common.exec.phyCoreId = 5;
    report.common.exec.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    report.common.exec.blockId = 2;
    report.common.exec.pipeName = "PIPE_MTE2";
    report.common.exec.launchId = 43;
    report.registerId = ACLSAN_DEVICE_REGISTER_MTE2_SOURCE;
    report.registerName = "mte2_source";
    report.setterInstruction = "set_mte2_src_para";
    report.consumerInstruction = "load_gm_to_cbuf_2dv2";
    return report;
}

NpuCheckRacecheckReport MakeRaceEvidence(npucheck::ConflictKind kind = npucheck::ConflictKind::RAW)
{
    auto records = std::make_shared<std::vector<AclsanDeviceMemoryAccessData>>(2);
    for (size_t i = 0; i < records->size(); ++i) {
        auto& data = (*records)[i];
        data.header.version = ACLSAN_API_VERSION;
        data.header.size = sizeof(data);
        data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        data.header.instrExecId = i + 1;
        data.header.launchId = 41;
        data.header.phyCoreId = 7;
        data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        data.header.pipeline = i == 0 ? ACLSAN_DEVICE_PIPE_MTE3 : ACLSAN_DEVICE_PIPE_MTE2;
        data.header.pc = i == 0 ? 0x111 : 0x222;
        data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
        data.accessMode = i == 0 ? (kind == npucheck::ConflictKind::WAR ? ACLSAN_DEVICE_MEMORY_ACCESS_READ :
                                                                          ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) :
                                   (kind == npucheck::ConflictKind::RAW ? ACLSAN_DEVICE_MEMORY_ACCESS_READ :
                                                                          ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
        data.address = 0x2000;
        data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        data.layout.range.bytes = 16;
    }
    return npucheck::MakeRacecheckDiagnostics({{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0x2000, 0x2010}}, *records)
        .front();
}

ReportRecord MakeRaceRecord()
{
    return ReportRecord{
        {ReportTool::RACECHECK, "hazard_raw"},
        ReportSeverity::WARNING,
        {
            {"space", "UB"},
            {"currentAccess", "READ"},
            {"previousAccess", "WRITE"},
            {"currentRangeBegin", "2000"},
            {"currentRangeEnd", "2010"},
            {"previousRangeBegin", "2000"},
            {"previousRangeEnd", "2010"},
            {"overlapBegin", "2000"},
            {"overlapEnd", "2010"},
            {"overlapBytes", "16"},
            {"memoryOwner", "core (0)"},
            {"firstType", "AIV"},
            {"firstBlock", "8"},
            {"secondType", "AIV"},
            {"secondBlock", "8"},
            {"address", "2000"},
            {"blockId", "8"},
            {"firstCoreId", "0"},
            {"firstPipe", "PIPE_MTE3"},
            {"firstFunction", "writer"},
            {"firstOffset", "20"},
            {"firstFile", "race.cpp"},
            {"firstLine", "11"},
            {"firstLaunchId", "41"},
            {"secondCoreId", "1"},
            {"secondPipe", "PIPE_MTE2"},
            {"secondFunction", "reader"},
            {"secondOffset", "30"},
            {"secondFile", "race.cpp"},
            {"secondLine", "19"},
            {"secondLaunchId", "42"},
            {"currentValue", "0xab"},
        }};
}

ReportRecord MakeSyncRecord()
{
    return ReportRecord{
        {ReportTool::SYNCCHECK, "pairing_mismatch"},
        ReportSeverity::ERROR,
        {
            {"reasonText", "WAIT_FLAG without matching SET_FLAG"},
            {"triggerOperation", "WAIT_FLAG"},
            {"triggerCoreId", "2"},
            {"triggerType", "AIC"},
            {"triggerBlock", "5"},
            {"triggerPipe", "PIPE_V"},
            {"triggerLaunchId", "42"},
            {"triggerLocation", "sync_kernel+0x44 in sync.cpp:88"},
            {"relatedPointLine", "=========     related point: expected SET_FLAG\n"},
            {"expectedOperationLine", ""},
            {"pairKind", "SET_WAIT_FLAG"},
            {"pairKey", "srcPipe=PIPE_V, dstPipe=PIPE_MTE2, id=7"},
        }};
}

NpuCheckSynccheckReport MakePairingReport(NpuCheckSyncMismatchReason reason, NpuCheckSyncPairKind pairKind)
{
    NpuCheckSynccheckReport report{};
    report.common.tool = ReportTool::SYNCCHECK;
    report.common.pattern = NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH;
    report.common.severity = ReportSeverity::ERROR;
    report.common.flags = npucheck::kNpuCheckReportCommonHasExecContext;
    report.primitiveKind = pairKind == NpuCheckSyncPairKind::SET_WAIT_FLAG ? NpuCheckSyncPrimitiveKind::SET_WAIT_FLAG :
                                                                             NpuCheckSyncPrimitiveKind::GET_RLS_BUF;
    report.detailKind = NpuCheckSyncDetailKind::PAIRING;
    report.hasRelatedPoint = true;
    report.triggerPoint.hasExecContext = true;
    report.triggerPoint.exec.phyCoreId = 1;
    report.triggerPoint.exec.blockId = 4;
    report.triggerPoint.exec.pipeName = "PIPE_MTE2";
    report.triggerPoint.exec.function = "SyncOperation";
    report.triggerPoint.exec.offset = 0x20;
    report.triggerPoint.exec.file = "sync.cpp";
    report.triggerPoint.exec.line = 30;
    report.common.exec = report.triggerPoint.exec;

    const bool setWait = pairKind == NpuCheckSyncPairKind::SET_WAIT_FLAG;
    const char* open = setWait ? "SET_FLAG" : "GET_BUF";
    const char* close = setWait ? "WAIT_FLAG" : "RLS_BUF";
    if (reason == NpuCheckSyncMismatchReason::DUPLICATE_OPEN) {
        report.triggerPoint.operation = open;
        report.relatedPoint.operation = open;
        report.relatedPoint.hasExecContext = true;
        report.relatedPoint.exec = report.triggerPoint.exec;
        report.relatedPoint.exec.pc = 0x1010;
        report.relatedPoint.exec.offset = 0x10;
        report.relatedPoint.exec.line = 24;
    } else if (reason == NpuCheckSyncMismatchReason::UNMATCHED_CLOSE) {
        report.triggerPoint.operation = close;
        report.relatedPoint.operation = open;
    } else {
        report.triggerPoint.operation = open;
        report.relatedPoint.operation = close;
    }
    report.detail =
        setWait ?
            NpuCheckSyncPairingError{reason, {pairKind, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE2, 0, 42}} :
            NpuCheckSyncPairingError{reason, {pairKind, ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_MTE2, 3, 42}};
    return report;
}

NpuCheckSynccheckReport MakeSynccheckReport(NpuCheckReportPattern pattern)
{
    if (pattern == NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH) {
        return MakePairingReport(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE, NpuCheckSyncPairKind::SET_WAIT_FLAG);
    }

    NpuCheckSynccheckReport report{};
    report.common.tool = ReportTool::SYNCCHECK;
    report.common.pattern = pattern;
    report.common.severity = ReportSeverity::ERROR;
    report.common.flags = npucheck::kNpuCheckReportCommonHasExecContext;
    report.triggerPoint.operation = "SYNC_OPERATION";
    report.triggerPoint.hasExecContext = true;
    report.triggerPoint.exec.phyCoreId = 0;
    report.triggerPoint.exec.blockId = 1;
    report.triggerPoint.exec.pipeName = "PIPE_S";
    report.triggerPoint.exec.pc = 0x100;
    report.triggerPoint.exec.kernelName = "sync_kernel";
    report.common.exec = report.triggerPoint.exec;

    switch (pattern) {
        case NpuCheckReportPattern::SYNCCHECK_INTRA_CORE_DIVERGENT:
        case NpuCheckReportPattern::SYNCCHECK_INTER_CORE_DIVERGENT:
        case NpuCheckReportPattern::SYNCCHECK_PARTICIPANT_MISMATCH:
            report.primitiveKind = NpuCheckSyncPrimitiveKind::BARRIER;
            report.detailKind = NpuCheckSyncDetailKind::BARRIER;
            report.detail = NpuCheckSyncBarrierError{"participant set differs", "AICore", 0x3, 0xf, 1};
            break;
        case NpuCheckReportPattern::SYNCCHECK_INVALID_ARGUMENT:
        case NpuCheckReportPattern::SYNCCHECK_OBJECT_NOT_INITIALIZED:
        case NpuCheckReportPattern::SYNCCHECK_DEADLOCK:
            report.primitiveKind = NpuCheckSyncPrimitiveKind::SYNC_OBJECT;
            report.detailKind = NpuCheckSyncDetailKind::OBJECT;
            report.detail = NpuCheckSyncObjectError{"sync object error", 1, 0x1000, 0x3, 1000};
            break;
        case NpuCheckReportPattern::SYNCCHECK_INSTRUCTION_SEQUENCE_MISMATCH:
            report.primitiveKind = NpuCheckSyncPrimitiveKind::INSTRUCTION_SEQUENCE;
            report.detailKind = NpuCheckSyncDetailKind::SEQUENCE;
            report.hasRelatedPoint = true;
            report.relatedPoint.operation = "EXPECTED_OPERATION";
            report.detail = NpuCheckSyncSequenceError{"instruction order differs", 2, 0x3};
            break;
        case NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH:
            break;
        default:
            break;
    }
    return report;
}

ReportRecord MakeSocRecord()
{
    return ReportRecord{
        {ReportTool::SOCCHECK, "register_mismatch"},
        ReportSeverity::ERROR,
        {
            {"function", "soc_kernel"},
            {"offset", "4"},
            {"file", "soc.cpp"},
            {"line", "9"},
            {"coreId", "6"},
            {"blockType", "AIC"},
            {"blockId", "1"},
            {"pipeName", "PIPE_S"},
            {"launchId", "43"},
            {"registerId", "17"},
            {"expectedValue", "10"},
            {"observedValue", "11"},
        }};
}

TEST(ReportRendererTest, RendersTemplateAndWritesText)
{
    const ReportTemplate tpl{"========= {{severity}}: {{title}}\n{{body}}\n"};
    const ReportFields fields{
        {"severity", "ERROR"},
        {"title", "Invalid access"},
        {"body", "=========     at foo+0x10 in bar.cpp:27"},
    };

    std::string rendered;
    EXPECT_EQ(npucheck::RenderReportText(tpl, fields, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(rendered, "========= ERROR: Invalid access\n=========     at foo+0x10 in bar.cpp:27\n");

    std::ostringstream stream;
    EXPECT_EQ(npucheck::WriteReportTextToStream(rendered, &stream), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(stream.str(), rendered);

    const std::string path = "/tmp/aclsan_report_renderer_smoke.txt";
    EXPECT_EQ(npucheck::WriteReportTextToFile(rendered, path), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(ReadFile(path), rendered);
}

TEST(ReportRendererTest, ReportsTemplateErrors)
{
    const ReportTemplate tpl{"========= {{severity}}: {{title}}\n"};
    const ReportFields fields{{"severity", "ERROR"}, {"title", "Invalid access"}};
    std::string rendered;

    EXPECT_EQ(
        npucheck::RenderReportText(ReportTemplate{"{{missing}}"}, fields, &rendered),
        ReportRenderStatus::MISSING_FIELD);
    EXPECT_EQ(
        npucheck::RenderReportText(ReportTemplate{"{{missing"}, fields, &rendered),
        ReportRenderStatus::MALFORMED_TEMPLATE);
    EXPECT_EQ(npucheck::RenderReportText(tpl, fields, nullptr), ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_EQ(npucheck::WriteReportTextToStream(rendered, nullptr), ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_EQ(npucheck::WriteReportTextToFile(rendered, ""), ReportRenderStatus::OPEN_FAILED);
}

TEST(ReportRendererTest, ListsAndRendersBuiltinTemplates)
{
    const auto builtinKeys = npucheck::ListBuiltinReportTemplates();
    EXPECT_GE(builtinKeys.size(), 32U);
    EXPECT_NE(
        std::find(builtinKeys.begin(), builtinKeys.end(), ReportTemplateKey{ReportTool::MEMCHECK, "invalid_access"}),
        builtinKeys.end());
    EXPECT_NE(
        std::find(builtinKeys.begin(), builtinKeys.end(), ReportTemplateKey{ReportTool::SYNCCHECK, "pairing_mismatch"}),
        builtinKeys.end());

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderReportRecord(MakeMemcheckInvalidAccessRecord(), {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(
        rendered, "========= ERROR:[MEMCHECK] Invalid GM read of size 16 bytes\n"
                  "=========     at kernel+0x10 in kernel.cpp:42\n"
                  "=========     by aicore (3) type (AIC) block (7) pipe (PIPE_MTE2)\n"
                  "=========     Address 0x1000 is out of bounds\n");

    EXPECT_EQ(npucheck::RenderReportRecord(MakeInitcheckRecord(), {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("========= ERROR:[INITCHECK] Uninitialized GM memory read\n"), std::string::npos);

    const auto registerReport = MakeRegisterInitcheckReport();
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(registerReport), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("Register mte2_source was used before initialization"), std::string::npos);
    EXPECT_NE(
        rendered.find("load_gm_to_cbuf_2dv2 requires initializing register mte2_source before execution"),
        std::string::npos);

    EXPECT_EQ(npucheck::RenderReportRecord(MakeRaceRecord(), {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("========= WARNING:[RACECHECK] RAW (read after write) race detected."), std::string::npos);

    EXPECT_EQ(npucheck::RenderReportRecord(MakeSyncRecord(), {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(
        rendered.find("Synchronization pairing mismatch: WAIT_FLAG without matching SET_FLAG"), std::string::npos);

    EXPECT_EQ(npucheck::RenderReportRecord(MakeSocRecord(), {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("========= ERROR:[SOCCHECK] SOC register mismatch detected."), std::string::npos);
}

TEST(ReportRendererTest, PreservesRaceKindsAndIndependentTemplateOverrides)
{
    const std::pair<const char*, const char*> hazards[] = {
        {"hazard_raw", "RAW (read after write)"},
        {"hazard_war", "WAR (write after read)"},
        {"hazard_waw", "WAW (write after write)"}};
    for (const auto& [pattern, kind] : hazards) {
        SCOPED_TRACE(pattern);
        auto record = MakeRaceRecord();
        record.key.pattern = pattern;
        std::string rendered;
        const ReportTemplateOverrides overrides{{record.key, {"{{raceKind}}/{{pattern}}\n"}}};
        ASSERT_EQ(npucheck::RenderReportRecord(record, overrides, &rendered), ReportRenderStatus::SUCCESS);
        EXPECT_EQ(rendered, std::string(kind) + "/" + pattern + "\n");

        ASSERT_EQ(npucheck::RenderReportRecord(record, {}, &rendered), ReportRenderStatus::SUCCESS);
        EXPECT_NE(rendered.find(std::string(kind) + " race detected."), std::string::npos);
    }
}

TEST(ReportRendererTest, MatchesBuiltinPatternOutputSnapshot)
{
    std::string actual;
    for (const auto& [key, descriptor] : npucheck::detail::GetPatternCatalog()) {
        ASSERT_EQ(descriptor.reportTemplate.text.rfind("========= {{Severity}}:[", 0), 0U);
        EXPECT_EQ(descriptor.reportTemplate.text.find("{{Tool}}"), std::string::npos);
        std::string rendered;
        ASSERT_EQ(
            npucheck::RenderReportRecord(
                ReportRecord{key, ReportSeverity::ERROR, MakePatternSnapshotFields(descriptor.reportTemplate)}, {},
                &rendered),
            ReportRenderStatus::SUCCESS);
        if (!actual.empty()) {
            actual.push_back('\n');
        }
        actual.append("===== ");
        actual.append(npucheck::ReportToolName(key.tool));
        actual.push_back('.');
        actual.append(key.pattern);
        actual.append(" =====\n");
        actual.append(rendered);
    }

    const std::string snapshot = ReadFile(NPU_CHECK_REPORT_PATTERN_SNAPSHOT);
    const std::size_t bodyStart = snapshot.find("\n\n");
    ASSERT_NE(bodyStart, std::string::npos);
    EXPECT_EQ(actual, snapshot.substr(bodyStart + 2));
}

TEST(ReportRendererTest, CatalogOwnsCompletePatternMetadata)
{
    using npucheck::detail::FindPatternDescriptor;
    using npucheck::detail::GetPatternCatalog;

    const auto* invalidAccess =
        FindPatternDescriptor(ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
    ASSERT_NE(invalidAccess, nullptr);
    EXPECT_FALSE(invalidAccess->reportTemplate.text.empty());
    EXPECT_EQ(invalidAccess, FindPatternDescriptor(ReportTemplateKey{ReportTool::MEMCHECK, "invalid_access"}));

    const auto* unused = FindPatternDescriptor(ReportTemplateKey{ReportTool::INITCHECK, "unused_memory"});
    ASSERT_NE(unused, nullptr);
    EXPECT_FALSE(unused->reportTemplate.text.empty());

    const auto& catalog = GetPatternCatalog();
    EXPECT_EQ(catalog.size(), 32U);
    for (const char* removedPattern : {"atomic_race", "cross_pipe_race", "inter_core_race", "invalid_remote_access"}) {
        EXPECT_EQ(FindPatternDescriptor(ReportTemplateKey{ReportTool::RACECHECK, removedPattern}), nullptr)
            << removedPattern;
    }
    std::set<NpuCheckReportPattern> patternValues;
    for (const auto& [key, descriptor] : catalog) {
        EXPECT_FALSE(key.pattern.empty());
        EXPECT_FALSE(descriptor.reportTemplate.text.empty()) << key.pattern;
        EXPECT_TRUE(patternValues.insert(descriptor.value).second) << key.pattern;
        EXPECT_EQ(&descriptor, FindPatternDescriptor(key.tool, descriptor.value)) << key.pattern;
    }
    EXPECT_EQ(patternValues.size(), catalog.size());
}

TEST(ReportRendererTest, AppendsStructuredCallStacks)
{
    ReportRecord record = MakeMemcheckInvalidAccessRecord();

    ReportCallStack rawStack{};
    rawStack.role = ReportStackRole::HOST_LAUNCH;
    rawStack.format = ReportStackFormat::RAW_TEXT;
    rawStack.rawText = "aclrtLaunchKernel [0x400123] in libacl.so\n";
    record.stacks.push_back(rawStack);

    ReportCallStack frameStack{};
    frameStack.role = ReportStackRole::FAULT_DEVICE;
    frameStack.format = ReportStackFormat::FRAMES;
    ReportFrame deviceFrame{};
    deviceFrame.pc = 0x100;
    deviceFrame.offset = 0x10;
    deviceFrame.function = "kernel";
    deviceFrame.file = "kernel.cpp";
    deviceFrame.line = 42;
    frameStack.frames.push_back(deviceFrame);
    ReportFrame callerFrame{};
    callerFrame.pc = 0x80;
    callerFrame.function = "launch_kernel";
    callerFrame.file = "launch.cpp";
    callerFrame.line = 18;
    frameStack.frames.push_back(callerFrame);
    record.stacks.push_back(frameStack);

    std::string rendered;
    EXPECT_EQ(npucheck::RenderReportRecord(record, {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(
        rendered.find("=========  Host Frames:\n"
                      "=========     aclrtLaunchKernel [0x400123] in libacl.so\n"),
        std::string::npos);
    EXPECT_NE(
        rendered.find("=========  Device Frames:\n"
                      "=========     #0 kernel in kernel.cpp:42\n"
                      "=========     #1 launch_kernel in launch.cpp:18\n"),
        std::string::npos);
    EXPECT_EQ(rendered.find("Device Frame:"), std::string::npos);
    EXPECT_EQ(rendered.find("Host Frame:"), std::string::npos);
}

TEST(ReportRendererTest, RejectsRemovedBothCallStackFormat)
{
    NpuCheckSynccheckReport report =
        MakePairingReport(NpuCheckSyncMismatchReason::UNCONSUMED_OPEN, NpuCheckSyncPairKind::GET_RLS_BUF);
    report.triggerPoint.exec.pc = 0x164;
    report.common.exec = report.triggerPoint.exec;
    report.triggerPoint.stackRole = ReportStackRole::SYNC_TRIGGER;
    report.common.stackCount = 1;
    ReportCallStack& stack = report.common.stacks[0];
    stack.role = ReportStackRole::SYNC_TRIGGER;
    stack.format = static_cast<ReportStackFormat>(3);
    stack.rawText = "[CALL-STACK] pc=0x164 status=available binary_id=1\n"
                    "  #0 GetBufInternal at kernel_event.h:718:13\n";
    stack.frames.push_back(ReportFrame{0x164, 0, "GetBufInternal", "kernel_event.h", 718, 13});

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, UsesOnlyDeviceOrHostCallStackHeadings)
{
    const std::array<ReportStackRole, 4> hostRoles = {
        ReportStackRole::HOST_LAUNCH,
        ReportStackRole::HOST_ALLOC,
        ReportStackRole::HOST_FREE,
        ReportStackRole::HOST_API_CALL,
    };
    for (const ReportStackRole role : hostRoles) {
        EXPECT_STREQ(npucheck::ReportStackRoleTitle(role), "Host Frames:");
    }

    const std::array<ReportStackRole, 11> deviceRoles = {
        ReportStackRole::NONE,
        ReportStackRole::FAULT_DEVICE,
        ReportStackRole::RELATED_ACCESS_A,
        ReportStackRole::RELATED_ACCESS_B,
        ReportStackRole::SYNC_PRODUCER,
        ReportStackRole::SYNC_CONSUMER,
        ReportStackRole::STATE_PRODUCER,
        ReportStackRole::STATE_CONSUMER,
        ReportStackRole::PEER_DEVICE,
        ReportStackRole::SYNC_TRIGGER,
        ReportStackRole::SYNC_RELATED,
    };
    for (const ReportStackRole role : deviceRoles) {
        EXPECT_STREQ(npucheck::ReportStackRoleTitle(role), "Device Frames:");
    }
}

TEST(ReportRendererTest, RendersOnlyActiveCommonCallStackPrefix)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;
    report.common.stackCount = 1;
    report.apiName = "aclrtLaunchKernel";
    report.apiErrorName = "ACL_ERROR_FAILURE";
    report.apiErrorMessage = "launch failed";

    report.common.stacks[0].role = ReportStackRole::HOST_LAUNCH;
    report.common.stacks[0].format = ReportStackFormat::RAW_TEXT;
    report.common.stacks[0].rawText = "active host frame";
    report.common.stacks[1].role = ReportStackRole::HOST_LAUNCH;
    report.common.stacks[1].format = ReportStackFormat::RAW_TEXT;
    report.common.stacks[1].rawText = "inactive host frame";

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("runtime API call to aclrtLaunchKernel"), std::string::npos);
    EXPECT_NE(rendered.find("active host frame"), std::string::npos);
    EXPECT_EQ(rendered.find("inactive host frame"), std::string::npos);
}

TEST(ReportRendererTest, UsesExplicitUnknownPhysicalCoreSentinel)
{
    EXPECT_EQ(npucheck::NpuCheckReportExecContext{}.phyCoreId, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(npucheck::NpuCheckSyncPoint{}.exec.phyCoreId, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(npucheck::NpuCheckSocStateRef{}.ownerCoreId, std::numeric_limits<std::uint32_t>::max());
}

TEST(ReportRendererTest, PrefersStructuredFaultFrameForSourceLocation)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS;
    report.common.exec.function = "fallback_function";
    report.common.exec.offset = 0x10;
    report.common.exec.file = "fallback.cpp";
    report.common.exec.line = 7;
    report.common.stackCount = 2;
    report.common.stacks[0].role = ReportStackRole::FAULT_DEVICE;
    report.common.stacks[0].format = ReportStackFormat::FRAMES;
    report.common.stacks[0].frames.push_back(ReportFrame{});
    report.common.stacks[0].frames.push_back(
        ReportFrame{0x100, 0x20, "symbolized_function", "symbolized.cpp", 42, 0, 0, 0});
    report.common.stacks[1].role = ReportStackRole::HOST_LAUNCH;
    report.common.stacks[1].format = ReportStackFormat::RAW_TEXT;
    report.common.stacks[1].rawText = "real host frame";
    report.access.accessBytes = 4;

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("at symbolized_function+0x20 in symbolized.cpp:42"), std::string::npos);
    EXPECT_EQ(CountOccurrences(rendered, "=========  Host Frames:"), 1U);
    EXPECT_EQ(rendered.find("Host Frame: <unknown>"), std::string::npos);
    EXPECT_NE(rendered.find("real host frame"), std::string::npos);
}

TEST(ReportRendererTest, UsesAllocationMemorySpaceForAllocationPatterns)
{
    NpuCheckMemcheckReport leak{};
    leak.common.tool = ReportTool::MEMCHECK;
    leak.common.pattern = NpuCheckReportPattern::MEMCHECK_LEAK;
    leak.access.memorySpace = NpuCheckReportMemorySpace::UB;
    leak.allocation.memorySpace = NpuCheckReportMemorySpace::GM;

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(leak), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("memory space GM"), std::string::npos);

    NpuCheckInitcheckReport unused{};
    unused.common.tool = ReportTool::INITCHECK;
    unused.common.pattern = NpuCheckReportPattern::INITCHECK_UNUSED_MEMORY;
    unused.access.memorySpace = NpuCheckReportMemorySpace::UB;
    unused.allocation.memorySpace = NpuCheckReportMemorySpace::L1;

    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(unused), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("Unused L1 memory"), std::string::npos);
}

TEST(ReportRendererTest, OmitsLaunchIdForMemcheckDeviceExecutionPoint)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_MISALIGNED_ACCESS;
    report.common.exec.phyCoreId = 18;
    report.common.exec.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    report.common.exec.blockId = 0;
    report.common.exec.pipeName = "PIPE_MTE2";
    report.common.exec.launchId = 41;

    std::string rendered;
    ASSERT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("by aicore (18) type (AIV) block (0) pipe (PIPE_MTE2)"), std::string::npos);
    EXPECT_EQ(rendered.find("in launch ("), std::string::npos);

    report.common.exec.launchId = 0;
    ASSERT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("pipe (PIPE_MTE2)"), std::string::npos);
    EXPECT_EQ(rendered.find("in launch ("), std::string::npos);
}

TEST(ReportRendererTest, FallsBackToProgramCounterWhenFaultIsNotSymbolized)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_MISALIGNED_ACCESS;
    report.common.exec.pc = 0xabc;
    report.common.exec.kernelName = "fallback_kernel";

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("at pc 0xabc in fallback_kernel"), std::string::npos);
    EXPECT_EQ(rendered.find("<unknown>+0x0 in <unknown>:0"), std::string::npos);
    EXPECT_NE(rendered.find("by aicore (<unknown>)"), std::string::npos);
    EXPECT_EQ(rendered.find("4294967295"), std::string::npos);
}

TEST(ReportRendererTest, NormalizerPreservesToolValidationAndStoredRaceRanges)
{
    const auto race = MakeRaceEvidence();
    const auto init = MakeRegisterInitcheckReport();
    ReportRecord output;
    ASSERT_EQ(
        npucheck::detail::NormalizeReport(NpuCheckReportRecord::From(race), &output), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(output.key.tool, ReportTool::RACECHECK);
    EXPECT_EQ(output.fields.at("overlapBytes"), "16");
    ASSERT_EQ(
        npucheck::detail::NormalizeReport(NpuCheckReportRecord::From(init), &output), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(output.key.tool, ReportTool::INITCHECK);

    auto invalid = NpuCheckReportRecord::From(init);
    invalid.tool = ReportTool::MEMCHECK;
    EXPECT_EQ(npucheck::detail::NormalizeReport(invalid, &output), ReportRenderStatus::INVALID_ARGUMENT);
    auto limited = race;
    limited.previousRange.end = limited.checkRange.begin;
    EXPECT_EQ(
        npucheck::detail::NormalizeReport(NpuCheckReportRecord::From(limited), &output),
        ReportRenderStatus::INVALID_ARGUMENT);
    ASSERT_EQ(
        npucheck::detail::NormalizeReport(NpuCheckReportRecord::From(race), &output), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(output.key.tool, ReportTool::RACECHECK);
    EXPECT_EQ(output.fields.at("overlapBytes"), "16");
}

TEST(ReportRendererTest, ResolvesEachRaceProgramCounterFromOwnedAccesses)
{
    const auto report = MakeRaceEvidence();
    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("pc 0x111"), std::string::npos);
    EXPECT_NE(rendered.find("pc 0x222"), std::string::npos);
}

TEST(ReportRendererTest, RejectsRaceEvidenceWithWrongPhysicalOwner)
{
    auto report = MakeRaceEvidence();
    ++report.key.phyCoreId;
    std::string rendered = "stale";
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, RejectsCommonCallStackCountBeyondFixedCapacity)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;
    report.common.stackCount = 9;

    std::string rendered = "stale";
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, RejectsRecordToolMismatch)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::INITCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;

    std::string rendered = "stale";
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, RejectsOuterToolAndPayloadMismatchSafely)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;
    NpuCheckReportRecord record = NpuCheckReportRecord::From(report);
    ASSERT_TRUE(std::holds_alternative<const NpuCheckMemcheckReport*>(record.GetPayload()));
    record.tool = ReportTool::INITCHECK;

    std::string rendered = "stale";
    EXPECT_EQ(npucheck::RenderNpuCheckReportRecord(record, {}, &rendered), ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, RejectsRecordPatternMismatch)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;
    NpuCheckReportRecord record = NpuCheckReportRecord::From(report);
    record.pattern = NpuCheckReportPattern::MEMCHECK_LEAK;

    std::string rendered = "stale";
    EXPECT_EQ(npucheck::RenderNpuCheckReportRecord(record, {}, &rendered), ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, RejectsPatternOwnedByAnotherTool)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH;

    std::string rendered = "stale";
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, RejectsNullSelectedReportPointer)
{
    NpuCheckReportRecord record{};
    record.tool = ReportTool::MEMCHECK;
    record.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;

    std::string rendered = "stale";
    EXPECT_EQ(npucheck::RenderNpuCheckReportRecord(record, {}, &rendered), ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, RejectsDuplicateActiveCallStackRoles)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;
    report.common.stackCount = 2;
    report.common.stacks[0].role = ReportStackRole::HOST_LAUNCH;
    report.common.stacks[1].role = ReportStackRole::HOST_LAUNCH;

    std::string rendered = "stale";
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(rendered.empty());
}

TEST(ReportRendererTest, ValidatesActiveCallStackMetadataAndBoundaries)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_API_ERROR;
    report.common.stackCount = npucheck::kNpuCheckReportStackMax;
    for (std::uint32_t i = 0; i < report.common.stackCount; ++i) {
        report.common.stacks[i].role = static_cast<ReportStackRole>(i + 1);
        report.common.stacks[i].format = ReportStackFormat::NONE;
    }

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);

    report.common.stackCount = 1;
    report.common.stacks[0].role = static_cast<ReportStackRole>(0);
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);

    report.common.stacks[0].role = ReportStackRole::FAULT_DEVICE;
    report.common.stacks[0].format = static_cast<ReportStackFormat>(99);
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);

    report.common.stacks[0].format = ReportStackFormat::FRAMES;
    report.common.stacks[0].frames.resize(17);
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::INVALID_ARGUMENT);
}

TEST(ReportRendererTest, LoadsTemplateOverrides)
{
    const std::string overridePath = "/tmp/aclsan_report_renderer_templates.conf";
    {
        std::ofstream config(overridePath, std::ios::binary);
        config << "memcheck.invalid_access=USER {{Severity}} {{address}}\\n";
    }

    ReportTemplateOverrides overrides;
    std::string rendered;
    EXPECT_EQ(npucheck::LoadReportTemplateOverridesFromFile(overridePath, &overrides), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(
        npucheck::RenderReportRecord(MakeMemcheckInvalidAccessRecord(), overrides, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_EQ(rendered, "USER ERROR 1000\n");

    const ReportRecord unknownRecord{{ReportTool::MEMCHECK, "unknown"}, ReportSeverity::ERROR, {}};
    EXPECT_EQ(npucheck::RenderReportRecord(unknownRecord, {}, &rendered), ReportRenderStatus::UNKNOWN_TEMPLATE);
}

TEST(ReportRendererTest, DoesNotInjectToolTemplateFields)
{
    const ReportTemplateKey key{ReportTool::MEMCHECK, "invalid_access"};
    std::string rendered;

    EXPECT_EQ(
        npucheck::RenderReportRecord(
            MakeMemcheckInvalidAccessRecord(), ReportTemplateOverrides{{key, {"{{Tool}}\n"}}}, &rendered),
        ReportRenderStatus::MISSING_FIELD);
    EXPECT_EQ(
        npucheck::RenderReportRecord(
            MakeMemcheckInvalidAccessRecord(), ReportTemplateOverrides{{key, {"{{tool}}\n"}}}, &rendered),
        ReportRenderStatus::MISSING_FIELD);
}

TEST(ReportRendererTest, RendersBundleSummaries)
{
    const ReportRecord leakRecord{
        {ReportTool::MEMCHECK, "leak"},
        ReportSeverity::WARNING,
        {
            {"bytes", "128"},
            {"base", "0fc0"},
            {"allocId", "12"},
            {"space", "GM"},
            {"allocFunction", "aclrtMalloc"},
            {"allocPc", "400456"},
            {"hostBinary", "libacl.so"},
        }};
    const ReportRecord unusedRecord{
        {ReportTool::INITCHECK, "unused_memory"},
        ReportSeverity::INFO,
        {
            {"space", "GM"},
            {"base", "0fc0"},
            {"bytes", "128"},
            {"unusedBytes", "64"},
            {"firstUninitOffset", "40"},
            {"firstUninitAddress", "1000"},
            {"unusedPercent", "50"},
        }};
    const ReportRecord deadlockRecord{
        {ReportTool::SYNCCHECK, "deadlock"},
        ReportSeverity::ERROR,
        {
            {"reason", "wait cannot complete"},
            {"triggerOperation", "WAIT_FLAG"},
            {"triggerCoreId", "2"},
            {"triggerType", "AIC"},
            {"triggerBlock", "5"},
            {"triggerPipe", "PIPE_V"},
            {"triggerLaunchId", "42"},
            {"triggerLocation", "pc 0x5000 in sync_kernel"},
            {"relatedPointLine", ""},
            {"waitingMask", "f"},
            {"timeoutNs", "1000"},
            {"objectLine", "=========     sync object 0x99\n"},
        }};
    const std::vector<ReportRecord> records{
        MakeMemcheckInvalidAccessRecord(), MakeRaceRecord(), MakeSocRecord(), leakRecord, unusedRecord, deadlockRecord};

    std::string rendered;
    EXPECT_EQ(npucheck::RenderReportBundle(records, {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(rendered.rfind("========= NPU-CHECK\n", 0), 0U);
    EXPECT_NE(rendered.find("========= MEMCHECK SUMMARY: 1 errors, 1 warnings, 0 infos, 1 leaks"), std::string::npos);
    EXPECT_NE(
        rendered.find("========= INITCHECK SUMMARY: 0 errors, 0 warnings, 1 infos, 1 unused memory reports"),
        std::string::npos);
    EXPECT_NE(
        rendered.find("========= RACECHECK SUMMARY: 1 hazard displayed (0 errors, 1 warnings, 0 infos)"),
        std::string::npos);
    EXPECT_NE(
        rendered.find("========= SYNCCHECK SUMMARY: 1 errors, 0 warnings, 0 infos, 1 deadlocks"), std::string::npos);
    EXPECT_NE(rendered.find("========= SOCCHECK SUMMARY: 1 errors, 0 warnings, 0 infos"), std::string::npos);
    EXPECT_NE(
        rendered.find("========= ERROR SUMMARY: 3 errors\n"
                      "=========     MEMCHECK: 1 errors\n"
                      "=========     INITCHECK: 0 errors\n"
                      "=========     RACECHECK: 0 errors\n"
                      "=========     SYNCCHECK: 1 errors\n"
                      "=========     SOCCHECK: 1 errors\n"),
        std::string::npos);
    EXPECT_EQ(rendered.find("=========     FATAL:"), std::string::npos);
}

TEST(ReportRendererTest, SummarizesOnlyToolsPresentInBundle)
{
    const std::vector<ReportRecord> records{MakeMemcheckInvalidAccessRecord()};

    std::string rendered;
    ASSERT_EQ(npucheck::RenderReportBundle(records, {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("========= MEMCHECK SUMMARY:"), std::string::npos);
    EXPECT_NE(rendered.find("=========     MEMCHECK:"), std::string::npos);
    EXPECT_EQ(rendered.find("INITCHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("RACECHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("SYNCCHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("SOCCHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("=========     INITCHECK:"), std::string::npos);
    EXPECT_EQ(rendered.find("=========     RACECHECK:"), std::string::npos);
    EXPECT_EQ(rendered.find("=========     SYNCCHECK:"), std::string::npos);
    EXPECT_EQ(rendered.find("=========     SOCCHECK:"), std::string::npos);
}

TEST(ReportRendererTest, EmptyBundleHasOnlyGlobalSummary)
{
    std::string rendered;
    ASSERT_EQ(npucheck::RenderReportBundle({}, {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_EQ(rendered.find("MEMCHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("INITCHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("RACECHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("SYNCCHECK SUMMARY"), std::string::npos);
    EXPECT_EQ(rendered.find("SOCCHECK SUMMARY"), std::string::npos);
    EXPECT_NE(rendered.find("========= ERROR SUMMARY: 0 errors\n"), std::string::npos);
    EXPECT_EQ(rendered.find("=========     FATAL:"), std::string::npos);
}

TEST(ReportRendererTest, RendersPairingMismatchReasonForDifferentOperationKinds)
{
    struct Case {
        NpuCheckSyncPairKind pairKind;
        NpuCheckSyncMismatchReason reason;
        const char* expectedHeadline;
        const char* expectedRelated;
    };
    const Case cases[] = {
        {NpuCheckSyncPairKind::SET_WAIT_FLAG, NpuCheckSyncMismatchReason::DUPLICATE_OPEN,
         "Synchronization pairing mismatch: duplicate SET_FLAG.",
         "related point: previous SET_FLAG at SyncOperation+0x10 in sync.cpp:24 is still "
         "pending"},
        {NpuCheckSyncPairKind::SET_WAIT_FLAG, NpuCheckSyncMismatchReason::UNMATCHED_CLOSE,
         "Synchronization pairing mismatch: WAIT_FLAG without matching SET_FLAG.",
         "related point: expected SET_FLAG, but no matching point was found for this pair key"},
        {NpuCheckSyncPairKind::SET_WAIT_FLAG, NpuCheckSyncMismatchReason::UNCONSUMED_OPEN,
         "Synchronization pairing mismatch: SET_FLAG without matching WAIT_FLAG.",
         "related point: expected WAIT_FLAG, but no matching point was found for this pair key"},
        {NpuCheckSyncPairKind::GET_RLS_BUF, NpuCheckSyncMismatchReason::DUPLICATE_OPEN,
         "Synchronization pairing mismatch: duplicate GET_BUF.",
         "related point: previous GET_BUF at SyncOperation+0x10 in sync.cpp:24 is still "
         "pending"},
        {NpuCheckSyncPairKind::GET_RLS_BUF, NpuCheckSyncMismatchReason::UNMATCHED_CLOSE,
         "Synchronization pairing mismatch: RLS_BUF without matching GET_BUF.",
         "related point: expected GET_BUF, but no matching point was found for this pair key"},
        {NpuCheckSyncPairKind::GET_RLS_BUF, NpuCheckSyncMismatchReason::UNCONSUMED_OPEN,
         "Synchronization pairing mismatch: GET_BUF without matching RLS_BUF.",
         "related point: expected RLS_BUF, but no matching point was found for this pair key"},
    };

    for (const Case& testCase : cases) {
        NpuCheckSynccheckReport report = MakePairingReport(testCase.reason, testCase.pairKind);

        std::string rendered;
        ASSERT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::SUCCESS);
        EXPECT_NE(rendered.find(testCase.expectedHeadline), std::string::npos);
        EXPECT_NE(rendered.find(testCase.expectedRelated), std::string::npos);
        EXPECT_EQ(rendered.find("in launch ("), std::string::npos);
        EXPECT_EQ(rendered.find("observed sequence"), std::string::npos);
    }
}

TEST(ReportRendererTest, SeparatesBundleRecordsWithOneEmptyLine)
{
    const ReportTemplateKey key{ReportTool::MEMCHECK, "invalid_access"};
    const ReportTemplateOverrides overrides{{key, {"{{recordName}}\n"}}};
    const std::vector<ReportRecord> records{
        {key, ReportSeverity::ERROR, {{"recordName", "first record"}}},
        {key, ReportSeverity::ERROR, {{"recordName", "second record"}}},
    };

    std::string rendered;
    ASSERT_EQ(npucheck::RenderReportBundle(records, overrides, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("first record\n\nsecond record\n"), std::string::npos);
    EXPECT_EQ(rendered.find("first record\n\n\nsecond record\n"), std::string::npos);
}

TEST(ReportRendererTest, RendersStructuredPairingMismatchEvidence)
{
    NpuCheckSynccheckReport report =
        MakePairingReport(NpuCheckSyncMismatchReason::DUPLICATE_OPEN, NpuCheckSyncPairKind::SET_WAIT_FLAG);
    report.triggerPoint.exec.launchId = 42;
    report.common.exec.launchId = 42;
    report.relatedPoint.exec.launchId = 41;
    report.common.stackCount = 2;
    report.common.stacks[0].role = ReportStackRole::SYNC_TRIGGER;
    report.common.stacks[0].format = ReportStackFormat::FRAMES;
    report.common.stacks[0].frames.push_back(ReportFrame{0, 0x20, "SecondSet", "sync.cpp", 30});
    report.common.stacks[1].role = ReportStackRole::SYNC_RELATED;
    report.common.stacks[1].format = ReportStackFormat::FRAMES;
    report.common.stacks[1].frames.push_back(ReportFrame{0x1010, 0x10, "FirstSet", "sync.cpp", 24});
    report.triggerPoint.stackRole = ReportStackRole::SYNC_TRIGGER;
    report.relatedPoint.stackRole = ReportStackRole::SYNC_RELATED;

    std::string rendered;
    ASSERT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("Synchronization pairing mismatch: duplicate SET_FLAG."), std::string::npos);
    EXPECT_NE(
        rendered.find("related point: previous SET_FLAG at FirstSet+0x10 in sync.cpp:24 is still pending"),
        std::string::npos);
    EXPECT_NE(rendered.find("pipe (PIPE_MTE2) at SecondSet+0x20 in sync.cpp:30"), std::string::npos);
    EXPECT_NE(rendered.find("expected WAIT_FLAG before another SET_FLAG"), std::string::npos);
    EXPECT_EQ(rendered.find("in launch ("), std::string::npos);
    EXPECT_NE(
        rendered.find("pair kind SET_WAIT_FLAG, key (srcPipe=PIPE_V, dstPipe=PIPE_MTE2, id=42)"), std::string::npos);
    EXPECT_EQ(CountOccurrences(rendered, "=========  Device Frames:"), 2U);
    EXPECT_EQ(CountOccurrences(rendered, "=========     #0 "), 2U);
    EXPECT_EQ(CountOccurrences(rendered, "=========     #1 "), 0U);
    EXPECT_EQ(rendered.find("Trigger Point Device Backtrace:"), std::string::npos);
    EXPECT_EQ(rendered.find("Related Point Device Backtrace:"), std::string::npos);
}

TEST(ReportRendererTest, IncludesLaunchIdForSoccheckProducerAndConsumer)
{
    NpuCheckSoccheckReport report{};
    report.common.tool = ReportTool::SOCCHECK;
    report.common.pattern = NpuCheckReportPattern::SOCCHECK_CROSS_CORE_STATE_INCONSISTENT;
    report.producer.phyCoreId = 3;
    report.producer.launchId = 41;
    report.consumer.phyCoreId = 4;
    report.consumer.launchId = 42;

    std::string rendered;
    ASSERT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("consumer aicore (4) in launch (42) observed"), std::string::npos);
    EXPECT_NE(rendered.find("producer aicore (3) in launch (41) expected"), std::string::npos);
}

TEST(ReportRendererTest, OmitsLaunchIdForSynccheckActualRelatedPoint)
{
    NpuCheckSynccheckReport report = MakeSynccheckReport(NpuCheckReportPattern::SYNCCHECK_PARTICIPANT_MISMATCH);
    report.triggerPoint.exec.launchId = 42;
    report.common.exec.launchId = 42;
    report.hasRelatedPoint = true;
    report.relatedPoint.operation = "BARRIER";
    report.relatedPoint.hasExecContext = true;
    report.relatedPoint.exec.phyCoreId = 3;
    report.relatedPoint.exec.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    report.relatedPoint.exec.blockId = 1;
    report.relatedPoint.exec.pipeName = "PIPE_MTE2";
    report.relatedPoint.exec.launchId = 41;

    std::string rendered;
    ASSERT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("pipe (PIPE_S) at pc 0x100 in sync_kernel"), std::string::npos);
    EXPECT_NE(
        rendered.find("related point: BARRIER by aicore (3) type (AIV) block (1) pipe (PIPE_MTE2) at"),
        std::string::npos);
    EXPECT_EQ(rendered.find("in launch ("), std::string::npos);
}

TEST(ReportRendererTest, RendersUnconsumedGetBufferWithExpectedRelatedPoint)
{
    NpuCheckSynccheckReport report{};
    report.common.tool = ReportTool::SYNCCHECK;
    report.common.pattern = NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH;
    report.common.severity = ReportSeverity::ERROR;
    report.common.flags = npucheck::kNpuCheckReportCommonHasExecContext;
    report.primitiveKind = NpuCheckSyncPrimitiveKind::GET_RLS_BUF;
    report.detailKind = NpuCheckSyncDetailKind::PAIRING;
    report.triggerPoint.operation = "GET_BUF";
    report.triggerPoint.hasExecContext = true;
    report.triggerPoint.exec.phyCoreId = 1;
    report.triggerPoint.exec.blockId = 4;
    report.triggerPoint.exec.pipeName = "PIPE_MTE2";
    report.triggerPoint.exec.function = "AcquireBuf";
    report.triggerPoint.exec.offset = 0x10;
    report.triggerPoint.exec.file = "sync.cpp";
    report.triggerPoint.exec.line = 20;
    report.common.exec = report.triggerPoint.exec;
    report.hasRelatedPoint = true;
    report.relatedPoint.operation = "RLS_BUF";
    report.detail = NpuCheckSyncPairingError{
        NpuCheckSyncMismatchReason::UNCONSUMED_OPEN,
        {NpuCheckSyncPairKind::GET_RLS_BUF, ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_MTE2, 3, 42},
    };

    std::string rendered;
    ASSERT_EQ(
        npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("Synchronization pairing mismatch: GET_BUF without matching RLS_BUF."), std::string::npos);
    EXPECT_NE(
        rendered.find("related point: expected RLS_BUF, but no matching point was found for this pair key"),
        std::string::npos);
    EXPECT_NE(rendered.find("pair kind GET_RLS_BUF, key (pipe=PIPE_MTE2, id=42, mode=3)"), std::string::npos);
}

TEST(ReportRendererTest, RejectsInvalidPairingMismatchMetadata)
{
    std::string rendered;
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE, NpuCheckSyncPairKind::SET_WAIT_FLAG);
        report.detailKind = NpuCheckSyncDetailKind::BARRIER;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE, NpuCheckSyncPairKind::SET_WAIT_FLAG);
        std::get<NpuCheckSyncPairingError>(report.detail).reason = NpuCheckSyncMismatchReason::UNKNOWN;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::UNCONSUMED_OPEN, NpuCheckSyncPairKind::GET_RLS_BUF);
        report.primitiveKind = NpuCheckSyncPrimitiveKind::SET_WAIT_FLAG;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::UNCONSUMED_OPEN, NpuCheckSyncPairKind::GET_RLS_BUF);
        std::get<NpuCheckSyncPairingError>(report.detail).key.srcPipe = ACLSAN_DEVICE_PIPE_VECTOR;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::UNCONSUMED_OPEN, NpuCheckSyncPairKind::GET_RLS_BUF);
        report.relatedPoint.hasExecContext = true;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::DUPLICATE_OPEN, NpuCheckSyncPairKind::SET_WAIT_FLAG);
        report.common.exec.pc = 0xdead;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
}

TEST(ReportRendererTest, RejectsOrphanRelatedPointAndMismatchedPointStackPc)
{
    std::string rendered;
    {
        NpuCheckSynccheckReport report = MakeSynccheckReport(NpuCheckReportPattern::SYNCCHECK_INTRA_CORE_DIVERGENT);
        report.relatedPoint.operation = "UNREFERENCED_OPERATION";
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::DUPLICATE_OPEN, NpuCheckSyncPairKind::SET_WAIT_FLAG);
        report.triggerPoint.exec.pc = 0x1000;
        report.common.exec = report.triggerPoint.exec;
        report.triggerPoint.stackRole = ReportStackRole::SYNC_TRIGGER;
        report.common.stackCount = 1;
        report.common.stacks[0].role = ReportStackRole::SYNC_TRIGGER;
        report.common.stacks[0].format = ReportStackFormat::FRAMES;
        report.common.stacks[0].frames.push_back(ReportFrame{0x2000, 0x20, "DifferentInstruction", "sync.cpp", 30});
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report =
            MakePairingReport(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE, NpuCheckSyncPairKind::SET_WAIT_FLAG);
        report.common.flags = 0;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
    {
        NpuCheckSynccheckReport report = MakeSynccheckReport(NpuCheckReportPattern::SYNCCHECK_INVALID_ARGUMENT);
        report.primitiveKind = static_cast<NpuCheckSyncPrimitiveKind>(99);
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::INVALID_ARGUMENT);
    }
}

TEST(ReportRendererTest, RendersStructuredReportsFromEachCheckerStruct)
{
    NpuCheckMemcheckReport memcheck{};
    memcheck.common.tool = ReportTool::MEMCHECK;
    memcheck.common.pattern = NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS;
    memcheck.common.severity = ReportSeverity::ERROR;
    memcheck.common.exec.function = "kernel";
    memcheck.common.exec.offset = 0x10;
    memcheck.common.exec.file = "kernel.cpp";
    memcheck.common.exec.line = 42;
    memcheck.common.exec.phyCoreId = 3;
    memcheck.common.exec.blockId = 7;
    memcheck.common.exec.pipeName = "PIPE_MTE2";
    memcheck.access.memorySpace = NpuCheckReportMemorySpace::GM;
    memcheck.access.accessMode = NpuCheckReportAccessMode::READ;
    memcheck.access.accessBytes = 16;
    memcheck.access.address = 0x1000;
    memcheck.nearestAllocation.base = 0x0fc0;
    memcheck.nearestAllocation.bytes = 128;
    memcheck.distanceKind = NpuCheckReportDistanceKind::AFTER;
    memcheck.distanceBytes = 64;

    NpuCheckInitcheckReport initcheck{};
    initcheck.common.tool = ReportTool::INITCHECK;
    initcheck.common.pattern = NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ;
    initcheck.common.severity = ReportSeverity::ERROR;
    initcheck.common.exec.function = "init_kernel";
    initcheck.common.exec.offset = 0x18;
    initcheck.common.exec.file = "init.cpp";
    initcheck.common.exec.line = 21;
    initcheck.common.exec.phyCoreId = 4;
    initcheck.common.exec.blockId = 2;
    initcheck.common.exec.pipeName = "PIPE_MTE2";
    initcheck.access.memorySpace = NpuCheckReportMemorySpace::GM;
    initcheck.access.accessBytes = 32;
    initcheck.access.address = 0x3000;

    const auto racecheck = MakeRaceEvidence();

    NpuCheckSynccheckReport synccheck =
        MakePairingReport(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE, NpuCheckSyncPairKind::SET_WAIT_FLAG);

    NpuCheckSoccheckReport soccheck{};
    soccheck.common.tool = ReportTool::SOCCHECK;
    soccheck.common.pattern = NpuCheckReportPattern::SOCCHECK_REGISTER_MISMATCH;
    soccheck.common.severity = ReportSeverity::ERROR;
    soccheck.common.exec.function = "soc_kernel";
    soccheck.common.exec.offset = 0x4;
    soccheck.common.exec.file = "soc.cpp";
    soccheck.common.exec.line = 9;
    soccheck.common.exec.phyCoreId = 6;
    soccheck.common.exec.blockId = 1;
    soccheck.common.exec.pipeName = "PIPE_S";
    soccheck.state.registerId = 17;
    soccheck.state.expectedValue = 0x10;
    soccheck.state.observedValue = 0x11;

    std::vector<NpuCheckReportRecord> records{
        NpuCheckReportRecord::From(memcheck),  NpuCheckReportRecord::From(initcheck),
        NpuCheckReportRecord::From(racecheck), NpuCheckReportRecord::From(synccheck),
        NpuCheckReportRecord::From(soccheck),
    };

    std::string rendered;
    for (const NpuCheckReportRecord& record : records) {
        EXPECT_EQ(npucheck::RenderNpuCheckReportRecord(record, {}, &rendered), ReportRenderStatus::SUCCESS);
    }
    EXPECT_EQ(npucheck::RenderNpuCheckReportBundle(records, {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("========= ERROR:[MEMCHECK] Invalid GM read of size 16 bytes"), std::string::npos);
    EXPECT_NE(rendered.find("========= ERROR:[INITCHECK] Uninitialized GM memory read\n"), std::string::npos);
    EXPECT_NE(rendered.find("========= WARNING:[RACECHECK] RAW (read after write) race detected."), std::string::npos);
    EXPECT_NE(
        rendered.find(
            "========= ERROR:[SYNCCHECK] Synchronization pairing mismatch: WAIT_FLAG without matching SET_FLAG."),
        std::string::npos);
    EXPECT_NE(rendered.find("========= ERROR:[SOCCHECK] SOC register mismatch detected."), std::string::npos);
    EXPECT_NE(rendered.find("========= ERROR SUMMARY: 4 errors"), std::string::npos);
}

TEST(ReportRendererTest, RendersAllStructuredPatternTemplates)
{
    std::string rendered;

    for (const auto pattern :
         {NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS, NpuCheckReportPattern::MEMCHECK_MISALIGNED_ACCESS,
          NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE, NpuCheckReportPattern::MEMCHECK_USE_BEFORE_ALLOC,
          NpuCheckReportPattern::MEMCHECK_INVALID_FREE, NpuCheckReportPattern::MEMCHECK_DOUBLE_FREE,
          NpuCheckReportPattern::MEMCHECK_LEAK, NpuCheckReportPattern::MEMCHECK_API_ERROR}) {
        NpuCheckMemcheckReport report{};
        report.common.tool = ReportTool::MEMCHECK;
        report.common.pattern = pattern;
        report.common.severity = ReportSeverity::ERROR;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::SUCCESS);
    }

    for (const auto pattern :
         {NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ,
          NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ, NpuCheckReportPattern::INITCHECK_UNUSED_MEMORY,
          NpuCheckReportPattern::INITCHECK_API_READ_UNINITIALIZED}) {
        NpuCheckInitcheckReport report{};
        report.common.tool = ReportTool::INITCHECK;
        report.common.pattern = pattern;
        report.common.severity = ReportSeverity::ERROR;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::SUCCESS);
    }

    for (const auto kind : {npucheck::ConflictKind::RAW, npucheck::ConflictKind::WAR, npucheck::ConflictKind::WAW}) {
        const auto report = MakeRaceEvidence(kind);
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::SUCCESS);
    }

    for (const auto pattern :
         {NpuCheckReportPattern::SYNCCHECK_INTRA_CORE_DIVERGENT, NpuCheckReportPattern::SYNCCHECK_INTER_CORE_DIVERGENT,
          NpuCheckReportPattern::SYNCCHECK_INVALID_ARGUMENT, NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH,
          NpuCheckReportPattern::SYNCCHECK_PARTICIPANT_MISMATCH, NpuCheckReportPattern::SYNCCHECK_DEADLOCK,
          NpuCheckReportPattern::SYNCCHECK_OBJECT_NOT_INITIALIZED,
          NpuCheckReportPattern::SYNCCHECK_INSTRUCTION_SEQUENCE_MISMATCH}) {
        NpuCheckSynccheckReport report = MakeSynccheckReport(pattern);
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::SUCCESS);
    }

    for (const auto pattern :
         {NpuCheckReportPattern::SOCCHECK_UNINITIALIZED_STATE_READ, NpuCheckReportPattern::SOCCHECK_REGISTER_MISMATCH,
          NpuCheckReportPattern::SOCCHECK_ILLEGAL_STATE_TRANSITION, NpuCheckReportPattern::SOCCHECK_STATE_NOT_RESTORED,
          NpuCheckReportPattern::SOCCHECK_CROSS_CORE_STATE_INCONSISTENT,
          NpuCheckReportPattern::SOCCHECK_SCOPE_VIOLATION}) {
        NpuCheckSoccheckReport report{};
        report.common.tool = ReportTool::SOCCHECK;
        report.common.pattern = pattern;
        report.common.severity = ReportSeverity::ERROR;
        EXPECT_EQ(
            npucheck::RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &rendered),
            ReportRenderStatus::SUCCESS);
    }
}

} // namespace

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
