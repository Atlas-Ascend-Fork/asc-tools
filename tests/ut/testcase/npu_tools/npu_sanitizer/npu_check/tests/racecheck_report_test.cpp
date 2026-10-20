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
#include "checker/racecheck_reports.h"
#include "checker/racecheck.h"
#include "diagnostic/pipeline_name.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <set>

namespace npucheck {
namespace {

TEST(PipelineNameTest, UsesOnePipeNameFormatAndUnknownValue)
{
    EXPECT_STREQ(detail::DevicePipelineName(ACLSAN_DEVICE_PIPE_SCALAR), "PIPE_S");
    EXPECT_STREQ(detail::DevicePipelineName(ACLSAN_DEVICE_PIPE_VECTOR), "PIPE_V");
    EXPECT_STREQ(detail::DevicePipelineName(ACLSAN_DEVICE_PIPE_MTE2), "PIPE_MTE2");
    EXPECT_STREQ(detail::DevicePipelineName(ACLSAN_DEVICE_PIPE_FIXPIPE), "PIPE_FIX");
    EXPECT_STREQ(detail::DevicePipelineName(ACLSAN_DEVICE_PIPE_INVALID), "PIPE_UNKNOWN");
    EXPECT_STREQ(detail::DevicePipelineName(999), "PIPE_UNKNOWN");
}

AclsanDeviceMemoryAccessData Memory(
    uint32_t mode, uint64_t begin, uint64_t end, uint32_t core = 10, uint64_t instruction = 1)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.header.launchId = 7;
    data.header.pc = 0x100 + instruction * 4;
    data.header.phyCoreId = core;
    data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.blockId = 1;
    data.header.pipeline =
        mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? ACLSAN_DEVICE_PIPE_VECTOR : ACLSAN_DEVICE_PIPE_MTE2;
    data.header.instrExecId = instruction;
    data.address = begin;
    data.accessMode = mode;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    data.accessCount = 1;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = end - begin;
    return data;
}

NpuCheckRacecheckReport Diagnostic(const AccessPair& evidence, const std::vector<AclsanDeviceMemoryAccessData>& records)
{
    return MakeRacecheckDiagnostics({evidence}, records).front();
}

ReportRecord Resolve(const NpuCheckRacecheckReport& report)
{
    ReportRecord output;
    EXPECT_EQ(NormalizeRacecheckReport(report, &output), ReportRenderStatus::SUCCESS);
    return output;
}

TEST(RacecheckReportTest, UsesBothActualRangesAndConfirmedOverlap)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 1536, 10, 2)};
    AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 512, 768};
    const auto report = Diagnostic(evidence, records);
    EXPECT_EQ(report.previous.header.instrExecId, 1U);
    EXPECT_EQ(report.current.header.instrExecId, 2U);
    EXPECT_EQ(report.launchId, 7U);
    EXPECT_EQ(report.key.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(report.key.phyCoreId, 10U);
    EXPECT_EQ(report.kind, ConflictKind::RAW);
    EXPECT_EQ(report.checkRange.begin, 512U);
    EXPECT_EQ(report.checkRange.end, 768U);
    const auto output = Resolve(report);
    EXPECT_EQ(output.severity, ReportSeverity::WARNING);
    EXPECT_EQ(output.fields.at("previousRangeBegin"), "0");
    EXPECT_EQ(output.fields.at("previousRangeEnd"), "400");
    EXPECT_EQ(output.fields.at("currentRangeBegin"), "200");
    EXPECT_EQ(output.fields.at("currentRangeEnd"), "600");
    EXPECT_EQ(output.fields.at("firstPipe"), "PIPE_MTE2");
    EXPECT_EQ(output.fields.at("secondPipe"), "PIPE_V");
}

TEST(RacecheckReportTest, SparseRangesNeverCrossHoles)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 96), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 16, 96, 10, 2)};
    records[0].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[0].layout.blockRepeat = {2, 32, 64, 1, 0, 0};
    AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 64, 80};
    auto report = Diagnostic(evidence, records);
    const auto output = Resolve(report);
    EXPECT_EQ(output.fields.at("previousRangeBegin"), "40");
    EXPECT_EQ(output.fields.at("previousRangeEnd"), "60");
    EXPECT_EQ(output.fields.at("currentRangeBegin"), "10");
    EXPECT_EQ(output.fields.at("currentRangeEnd"), "60");
    evidence.begin = 16;
    evidence.end = 80;
    EXPECT_THROW(Diagnostic(evidence, records), RacecheckError);
}

TEST(RacecheckReportTest, SharedCurrentLayoutKeepsHolesAndReportsOutlivePreparedStorage)
{
    std::vector<NpuCheckRacecheckReport> reports;
    {
        std::vector<AclsanDeviceMemoryAccessData> records{
            Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 16, 20, 10, 2)};
        records[1].layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
        records[1].layout.ndAffine = {1, 4, {3, 0, 0, 0, 0}, {-8, 0, 0, 0, 0}};
        const auto prepared = racecheck::PrepareAccess(records[1]);
        reports = MakeRacecheckDiagnostics({{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 8, 12}}, records, 1, prepared);
        EXPECT_THROW(
            MakeRacecheckDiagnostics({{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 12}}, records, 1, prepared),
            RacecheckError);
        EXPECT_THROW(
            MakeRacecheckDiagnostics({{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 8, 12}}, records, 0, prepared),
            RacecheckError);
    }
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].previousRange.begin, 0U);
    EXPECT_EQ(reports[0].previousRange.end, 32U);
    EXPECT_EQ(reports[0].currentRange.begin, 8U);
    EXPECT_EQ(reports[0].currentRange.end, 12U);
    EXPECT_EQ(reports[0].kind, ConflictKind::RAW);
    const auto output = Resolve(reports[0]);
    EXPECT_EQ(output.fields.at("currentRangeBegin"), "8");
    EXPECT_EQ(output.fields.at("currentRangeEnd"), "c");
}

TEST(RacecheckReportTest, GmKeepsTrueExecutionCoresAndByteWidths)
{
    constexpr uint64_t end = UINT64_C(1) << 34;
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, end, 10),
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 1, end, 11, 2)};
    for (auto& data : records) {
        data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    }
    AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_GM, 1, end};
    const auto report = Diagnostic(evidence, records);
    const auto output = Resolve(report);
    EXPECT_EQ(output.fields.at("firstCoreId"), "10");
    EXPECT_EQ(output.fields.at("secondCoreId"), "11");
    EXPECT_EQ(output.fields.at("memoryOwner"), "");
    EXPECT_EQ(output.fields.at("overlapBytes"), std::to_string(end - 1));
    EXPECT_EQ(output.fields.at("previousRangeEnd"), "400000000");
    EXPECT_EQ(report.key.phyCoreId, 0U);
    EXPECT_EQ(report.kind, ConflictKind::WAW);
}

TEST(RacecheckReportTest, AtomicModeUsesPublicAccessMode)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32),
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 32, 10, 2)};
    AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 32};
    const auto report = Diagnostic(evidence, records);
    EXPECT_EQ(Resolve(report).fields.at("currentAccess"), "ATOMIC_WRITE_ADD");
    EXPECT_EQ(report.kind, ConflictKind::WAR);
}

TEST(RacecheckReportTest, AtomicSemanticEvidenceKeepsConcreteModesAndRejectsCompatiblePair)
{
    for (auto mode : {ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        const std::vector<AclsanDeviceMemoryAccessData> records{
            Memory(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 32), Memory(mode, 0, 32, 10, 2)};
        const auto report = Diagnostic({0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 32}, records);
        const auto output = Resolve(report);
        EXPECT_EQ(output.key.pattern, "atomic_semantic");
        EXPECT_EQ(output.fields.at("previousAccess"), "ATOMIC_WRITE_ADD");
        EXPECT_EQ(output.fields.at("overlapBytes"), "32");
        auto invalid = report;
        invalid.key.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
        ReportRecord unused;
        EXPECT_EQ(NormalizeRacecheckReport(invalid, &unused), ReportRenderStatus::INVALID_ARGUMENT);
    }
    const std::vector<AclsanDeviceMemoryAccessData> compatible{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 0, 32),
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 0, 32, 10, 2)};
    EXPECT_THROW(Diagnostic((AccessPair{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 32}), compatible), RacecheckError);
}

TEST(RacecheckReportTest, FbSpaceDoesNotAliasPrivateReportSpace)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2)};
    for (auto& data : records) {
        data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_FB;
    }
    AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_FB, 0, 32};
    const auto report = Diagnostic(evidence, records);
    EXPECT_EQ(report.key.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_FB);
    EXPECT_EQ(Resolve(report).fields.at("space"), "FB");
}

TEST(RacecheckReportTest, RejectsInvalidEvidenceAndOwnReadWrite)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32, 10, 2)};
    AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 32};
    auto invalid = evidence;
    invalid.prevIndex = 2;
    EXPECT_THROW(Diagnostic(invalid, records), RacecheckError);
    invalid = evidence;
    invalid.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
    EXPECT_THROW(Diagnostic(invalid, records), RacecheckError);
    invalid = evidence;
    invalid.begin = 1;
    invalid.end = 33;
    EXPECT_THROW(Diagnostic(invalid, records), RacecheckError);
    records[0].header.phyCoreId = 11;
    EXPECT_THROW(Diagnostic(evidence, records), RacecheckError);
    records[0].header.phyCoreId = 10;
    records[0].header.launchId = 8;
    EXPECT_THROW(Diagnostic(evidence, records), RacecheckError);
    records[0].header.launchId = records[1].header.launchId;
    records[0].header.deviceId = 1;
    EXPECT_THROW(Diagnostic(evidence, records), RacecheckError);
    records[0].header.deviceId = records[1].header.deviceId;
    records[1].header.instrExecId = records[0].header.instrExecId;
    EXPECT_THROW(Diagnostic(evidence, records), RacecheckError);
}

TEST(RacecheckReportTest, OwnedAccessesRemainStableAcrossSourceReallocation)
{
    auto records = std::make_shared<std::vector<AclsanDeviceMemoryAccessData>>();
    records->push_back(Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32));
    records->push_back(Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2));
    const AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 32};
    auto report = Diagnostic(evidence, *records);
    (*records)[0].header.pc = 0x999;
    const auto capacity = records->capacity();
    while (records->size() <= capacity) {
        records->push_back(Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 64, 96));
    }
    records.reset();
    EXPECT_EQ(Resolve(report).fields.at("previousAccess"), "WRITE");
    EXPECT_EQ(Resolve(report).fields.at("currentAccess"), "READ");
    EXPECT_EQ(Resolve(report).fields.at("overlapBytes"), "32");
}

TEST(RacecheckReportTest, InvalidStoredIdentityIsRejectedAtConsumption)
{
    const std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2)};
    const AccessPair evidence{0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 32};
    const auto original = Diagnostic(evidence, records);
    for (int corruption = 0; corruption < 12; ++corruption) {
        auto report = original;
        switch (corruption) {
            case 0:
                report.previousRange.end = 0;
                break;
            case 1:
                report.currentRange.begin = report.checkRange.end;
                break;
            case 2:
                ++report.launchId;
                break;
            case 3:
                ++report.key.deviceId;
                break;
            case 4:
                ++report.key.phyCoreId;
                break;
            case 5:
                report.checkRange.end = report.checkRange.begin;
                break;
            case 6:
                report.kind = static_cast<ConflictKind>(255);
                break;
            case 7:
                ++report.current.header.launchId;
                break;
            case 8:
                ++report.previous.header.deviceId;
                break;
            case 9:
                report.current.accessMode = 255;
                break;
            case 10:
                report.current.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
                break;
            case 11:
                report.previousRange.begin = report.checkRange.begin + 1;
                break;
        }
        ReportRecord output;
        EXPECT_EQ(NormalizeRacecheckReport(report, &output), ReportRenderStatus::INVALID_ARGUMENT);
    }
}

TEST(RacecheckReportTest, SparseLayoutBeyondFormerLimitIsAccepted)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 1, 10, 2)};
    constexpr uint32_t blocks = (1u << 20u) + 1;
    records[0].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[0].layout.blockRepeat = {blocks, 1, 2, 1, 0, 0};
    const auto report = Diagnostic({0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 1}, records);
    EXPECT_EQ(report.previousRange.begin, 0U);
    EXPECT_EQ(report.previousRange.end, 1U);
}

TEST(RacecheckReportTest, LayoutRangeCountOverflowIsReportedBeforeAllocation)
{
    auto access = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1);
    access.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    access.layout.ndAffine.elementBytes = 1;
    access.layout.ndAffine.rank = 3;
    for (size_t i = 0; i < 3; ++i) {
        access.layout.ndAffine.dims[i] = 1u << 22u;
        access.layout.ndAffine.strides[i] = 2 + i;
    }
    std::vector<AclsanDeviceMemoryAccessData> records{access};
    MemStateTracker tracker(records);
    try {
        tracker.UpdateMemState(0, {});
        FAIL() << "expected layout range count overflow";
    } catch (const RacecheckError& error) {
        EXPECT_EQ(error.Kind(), RacecheckErrorKind::CapacityExceeded);
        EXPECT_NE(std::string(error.what()).find("layout range count overflow"), std::string::npos);
    }
    EXPECT_TRUE(tracker.Tables().empty());
}

class Ordering final : public RacecheckOrdering {
public:
    RacecheckOrder result = RacecheckOrder::Unordered;
    size_t syncs = 0;
    bool complete = true;
    RacecheckOrder Compare(const AclsanDeviceMemoryAccessData&, const AclsanDeviceMemoryAccessData&) override
    {
        return result;
    }
    void OnDeviceSync(const AclsanDeviceSyncData&) override { ++syncs; }
    bool AnalysisComplete() const override { return complete; }
};

TEST(RacecheckCheckerTest, HostAllocationFailureStopsLaunchWithClearError)
{
    class FailingOrdering final : public RacecheckOrdering {
    public:
        RacecheckOrder Compare(const AclsanDeviceMemoryAccessData&, const AclsanDeviceMemoryAccessData&) override
        {
            throw std::bad_alloc();
        }
        void OnDeviceSync(const AclsanDeviceSyncData&) override {}
        bool AnalysisComplete() const override { return true; }
    } ordering;
    std::string log;
    Racecheck checker(
        &ordering,
        {[](void* context, const char* message) { static_cast<std::string*>(context)->append(message); }, &log});
    CheckerReportList reports;
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32);
    auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    log.clear();
    EXPECT_FALSE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    EXPECT_NE(log.find("level=ERROR"), std::string::npos);
    EXPECT_NE(log.find("reason=host_memory_allocation_failed"), std::string::npos);
    EXPECT_NE(log.find("stage=ordering"), std::string::npos);
    EXPECT_NE(log.find("host memory"), std::string::npos);
    EXPECT_NE(log.find("state discarded"), std::string::npos);
    EXPECT_NE(log.find("launch=7"), std::string::npos);
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
    log.clear();
    EXPECT_FALSE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_EQ(log.find("level=ERROR"), std::string::npos);
    EXPECT_NE(log.find("event=access.skipped"), std::string::npos);
    write.header.launchId = 8;
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
}

TEST(RacecheckCheckerTest, ContainerMaximumCapacityIsNotReportedAsHostMemoryFailure)
{
    Ordering ordering;
    std::string log;
    Racecheck checker(
        &ordering,
        {[](void* context, const char* message) { static_cast<std::string*>(context)->append(message); }, &log});
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1);
    write.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    write.layout.ndAffine.elementBytes = 1;
    write.layout.ndAffine.rank = 2;
    write.layout.ndAffine.dims[0] = 1u << 30u;
    write.layout.ndAffine.dims[1] = 1u << 30u;
    write.layout.ndAffine.strides[0] = 2;
    write.layout.ndAffine.strides[1] = 3;
    CheckerReportList reports;
    EXPECT_FALSE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_NE(log.find("level=ERROR"), std::string::npos);
    EXPECT_NE(log.find("container maximum capacity exceeded"), std::string::npos);
    EXPECT_NE(log.find("stage=access_preparation"), std::string::npos);
    EXPECT_NE(log.find("state discarded"), std::string::npos);
    EXPECT_EQ(log.find("host_memory_allocation_failed"), std::string::npos);
    EXPECT_TRUE(reports.empty());
}

TEST(RacecheckCheckerTest, ReportsBothReadersConflictingWithOneWriter)
{
    Ordering ordering;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    auto first = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 1);
    auto second = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2);
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32, 10, 3);
    for (const auto* access : {&first, &second, &write}) {
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, access, reports));
    }
    ASSERT_EQ(reports.size(), 2U);
    for (size_t i = 0; i < reports.size(); ++i) {
        const auto& report = std::get<NpuCheckRacecheckReport>(reports[i]);
        EXPECT_EQ(report.kind, ConflictKind::WAR);
        const auto output = Resolve(report);
        EXPECT_EQ(output.fields.at("firstPc"), i == 0 ? "104" : "108");
        EXPECT_EQ(output.fields.at("secondPc"), "10c");
        EXPECT_EQ(output.fields.at("overlapBytes"), "32");
        EXPECT_EQ(report.reportId, i + 1);
    }
    // 已经进入 Race 状态的区间仍不再检查或重复报告。
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_EQ(reports.size(), 2U);
}

TEST(RacecheckReportTest, PreparedReportDoesNotDependOnSourceRecords)
{
    auto records = std::make_shared<std::vector<AclsanDeviceMemoryAccessData>>();
    records->push_back(Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32));
    records->push_back(Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2));
    const auto report = Diagnostic({0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 0, 32}, *records);
    const auto before = Resolve(report);
    (*records)[0].header.pc = 0x999;
    (*records)[0].accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    (*records)[0].layout.range.bytes = 64;
    const auto after = Resolve(report);
    EXPECT_EQ(after.fields, before.fields);
}

TEST(RacecheckReportTest, DisplaysOnlyContinuousSegmentsInvolvedInConflict)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 3072), Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 2048, 10, 2)};
    records[0].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[0].layout.blockRepeat = {2, 1024, 2048, 1, 0, 0};
    const auto report = Diagnostic({0, 1, ACLSAN_DEVICE_MEMORY_SPACE_UB, 512, 1024}, records);
    const auto output = Resolve(report);
    EXPECT_EQ(output.fields.at("previousRangeBegin"), "0");
    EXPECT_EQ(output.fields.at("previousRangeEnd"), "400");
    EXPECT_EQ(output.fields.at("currentRangeBegin"), "200");
    EXPECT_EQ(output.fields.at("currentRangeEnd"), "800");
    EXPECT_EQ(output.fields.at("overlapBegin"), "200");
    EXPECT_EQ(output.fields.at("overlapEnd"), "400");
}

TEST(RacecheckCheckerTest, AtomicSemanticRaceReportsEveryUnorderedPairAndSkipsRaceState)
{
    for (const auto order : {RacecheckOrder::Ordered, RacecheckOrder::Unordered, RacecheckOrder::Unknown}) {
        Ordering ordering;
        ordering.result = order;
        Racecheck checker(&ordering);
        CheckerReportList reports;
        auto add = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 32);
        auto min = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 0, 32, 10, 2);
        const auto dispatch = [&](const auto& access) {
            return checker.OnCallback(
                ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports);
        };
        ASSERT_TRUE(dispatch(add));
        ASSERT_TRUE(dispatch(add)); // 相同的已知类型不需要判序，两个 writer 都会保留。
        EXPECT_TRUE(reports.empty());
        EXPECT_EQ(dispatch(min), order != RacecheckOrder::Unknown);
        if (order == RacecheckOrder::Unordered) {
            ASSERT_EQ(reports.size(), 2U);
            const auto& report = std::get<NpuCheckRacecheckReport>(reports[0]);
            EXPECT_EQ(report.kind, ConflictKind::ATOMIC_SEMANTIC);
            EXPECT_EQ(report.previous.header.instrExecId, 1U);
            EXPECT_EQ(report.current.header.instrExecId, 2U);
            EXPECT_EQ(Resolve(report).fields.at("currentAccess"), "ATOMIC_WRITE_MIN");
            ASSERT_TRUE(dispatch(min));
            EXPECT_EQ(reports.size(), 2U);
        } else {
            EXPECT_TRUE(reports.empty());
        }
        EXPECT_EQ(checker.AnalysisComplete(), order != RacecheckOrder::Unknown);
    }
}

TEST(RacecheckCheckerTest, DefaultConfigurationCannotClaimCompleteWithoutOrdering)
{
    Racecheck checker;
    CheckerReportList reports;
    auto data = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32);
    auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2);
    const auto& ids = checker.GetSubscribedID();
    EXPECT_EQ(
        std::count(
            ids.begin(), ids.end(),
            CallbackSpec{ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS}),
        1);
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &data, reports));
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.HasErrors());
    EXPECT_FALSE(checker.AnalysisComplete());
    EXPECT_NE(checker.Summary().find("ordering_available=false"), std::string::npos);
    EXPECT_NE(checker.Summary().find("committed_operations=2"), std::string::npos);
    EXPECT_NE(checker.Summary().find("candidates=1"), std::string::npos);
}

TEST(RacecheckCheckerTest, OwnedReportsOutliveCheckerAndKeepLaunchIdentity)
{
    CheckerReportList reports;
    {
        Ordering ordering;
        Racecheck checker(&ordering);
        auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32);
        auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2);
        for (uint32_t device : {0U, 1U}) {
            for (uint64_t launch : {7U, 8U}) {
                write.header.deviceId = read.header.deviceId = device;
                write.header.launchId = read.header.launchId = launch;
                ASSERT_TRUE(checker.OnCallback(
                    ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
                ASSERT_TRUE(checker.OnCallback(
                    ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
            }
        }
    }
    ASSERT_EQ(reports.size(), 4U);
    for (const auto& item : reports) {
        const auto& report = std::get<NpuCheckRacecheckReport>(item);
        EXPECT_EQ(report.previous.header.instrExecId, 1U);
        EXPECT_EQ(report.current.header.instrExecId, 2U);
        EXPECT_EQ(report.previous.header.launchId, report.launchId);
        EXPECT_EQ(report.current.header.deviceId, report.key.deviceId);
        ReportRecord output;
        ASSERT_EQ(NormalizeRacecheckReport(report, &output), ReportRenderStatus::SUCCESS);
        EXPECT_EQ(output.fields.find("raceContext"), output.fields.end());
    }
}

TEST(RacecheckCheckerTest, ReportsOnlyCommittedFirstConflict)
{
    Ordering ordering;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024);
    auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 1536, 10, 2);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_TRUE(reports.empty());
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckRacecheckReport>(reports[0]);
    EXPECT_EQ(report.checkRange.begin, 512U);
    EXPECT_EQ(report.checkRange.end, 1024U);
    EXPECT_EQ(Resolve(report).severity, ReportSeverity::WARNING);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    EXPECT_EQ(reports.size(), 1U);
    EXPECT_TRUE(checker.HasErrors());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(RacecheckCheckerTest, IgnoresApiVersion)
{
    Ordering ordering;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32);
    auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2);
    write.header.version = ACLSAN_API_VERSION + 1;
    read.header.version = ACLSAN_API_VERSION + 1;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(RacecheckCheckerTest, RejectsShortMemoryCallbackBeforeCopy)
{
    Ordering ordering;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    AclsanDeviceEventHeader header{};
    header.version = ACLSAN_API_VERSION;
    header.size = sizeof(header);
    EXPECT_FALSE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &header, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(RacecheckCheckerTest, OwnReadWriteDoesNotWarnOrLoseOtherInstructionRace)
{
    Ordering ordering;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32);
    auto write = read;
    write.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_TRUE(reports.empty());
    read.header.instrExecId = 2;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckRacecheckReport>(reports[0]).kind, ConflictKind::RAW);
}

TEST(RacecheckCheckerTest, UnknownOrderingAndInvalidAccessNeverPublishRace)
{
    Ordering ordering;
    ordering.result = RacecheckOrder::Unknown;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024);
    auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 1536, 10, 2);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_FALSE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
    ordering.result = RacecheckOrder::Unordered;
    Racecheck constrained(&ordering);
    read.memoryFlags = 1;
    ASSERT_TRUE(
        constrained.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_FALSE(
        constrained.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(constrained.HasErrors());
    EXPECT_FALSE(constrained.AnalysisComplete());
}

TEST(RacecheckCheckerTest, OrderedPairsAndIndependentLaunchesDoNotWarn)
{
    Ordering ordering;
    ordering.result = RacecheckOrder::Ordered;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32);
    auto read = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 10, 2);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    ordering.result = RacecheckOrder::Unordered;
    read.header.launchId = 8;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    AclsanDeviceSyncData sync{};
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &sync, reports));
    EXPECT_EQ(ordering.syncs, 1U);
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(RacecheckCheckerTest, ManySparseConflictsKeepIndependentSegments)
{
    Ordering ordering;
    Racecheck checker(&ordering);
    CheckerReportList reports;
    constexpr uint32_t blocks = 1024;
    auto write = Memory(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, blocks * 64);
    write.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    write.layout.blockRepeat = {blocks, 32, 64, 1, 0, 0};
    auto read = write;
    read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    read.header.instrExecId = 2;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    ASSERT_EQ(reports.size(), blocks);
    for (size_t i = 0; i < reports.size(); ++i) {
        const auto& report = std::get<NpuCheckRacecheckReport>(reports[i]);
        EXPECT_EQ(report.checkRange.begin, i * 64);
        EXPECT_EQ(report.checkRange.end, i * 64 + 32);
        EXPECT_EQ(report.previous.header.instrExecId, 1U);
        EXPECT_EQ(report.current.header.instrExecId, 2U);
        EXPECT_EQ(report.previous.header.pc, write.header.pc);
        ReportRecord output;
        ASSERT_EQ(NormalizeRacecheckReport(report, &output), ReportRenderStatus::SUCCESS);
        EXPECT_EQ(output.fields.at("overlapBytes"), "32");
        EXPECT_EQ(output.fields.at("previousRangeBegin"), output.fields.at("overlapBegin"));
        EXPECT_EQ(output.fields.at("currentRangeEnd"), output.fields.at("overlapEnd"));
    }
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    EXPECT_EQ(reports.size(), blocks);
    EXPECT_TRUE(checker.AnalysisComplete());
}
} // namespace
} // namespace npucheck
