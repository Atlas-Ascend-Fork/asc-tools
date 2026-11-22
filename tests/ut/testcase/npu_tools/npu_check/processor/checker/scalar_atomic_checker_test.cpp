// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "acl_san/aclsan_cbdata_operator.h"
#include "checker/initcheck.h"
#include "checker/memcheck.h"
#include "checker/racecheck.h"
#include "checker/racecheck_reports.h"
#include "diagnostic/report_renderer.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <tuple>

namespace npucheck {
namespace {
constexpr uint32_t kCas = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_CAS;
constexpr uint64_t kGmBase = 0x1000;
constexpr std::array<uint32_t, 5> kAtomicModes = {
    ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN,
    ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_EXCH,
    ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_CAS};
constexpr std::array<NpuCheckReportAccessMode, 5> kReportModes = {
    NpuCheckReportAccessMode::ATOMIC_WRITE_ADD, NpuCheckReportAccessMode::ATOMIC_WRITE_MIN,
    NpuCheckReportAccessMode::ATOMIC_WRITE_MAX, NpuCheckReportAccessMode::ATOMIC_WRITE_EXCH,
    NpuCheckReportAccessMode::ATOMIC_WRITE_CAS};
constexpr std::array<const char*, 5> kAtomicNames = {"add", "min", "max", "exch", "cas"};
constexpr std::array<const char*, 5> kRaceNames = {
    "atomic write(add)", "atomic write(min)", "atomic write(max)", "atomic write(exch)", "atomic write(cas)"};

size_t AtomicIndex(uint32_t mode)
{
    return static_cast<size_t>(std::find(kAtomicModes.begin(), kAtomicModes.end(), mode) - kAtomicModes.begin());
}

NpuCheckReportAccessMode ReportMode(uint32_t mode) { return kReportModes.at(AtomicIndex(mode)); }

AclsanDeviceMemoryAccessData AtomicAccess(
    uint32_t mode, uint64_t instruction = 1, uint32_t space = ACLSAN_DEVICE_MEMORY_SPACE_GM)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.launchId = 17;
    data.header.instrExecId = instruction;
    data.header.deviceId = 2;
    data.header.executor.blockType =
        space == ACLSAN_DEVICE_MEMORY_SPACE_GM ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    data.memoryOwner = space == ACLSAN_DEVICE_MEMORY_SPACE_GM ? AclsanCoreInfo{} : data.header.executor;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.memorySpace = space;
    data.accessMode = mode;
    data.accessCount = 1;
    data.address = space == ACLSAN_DEVICE_MEMORY_SPACE_GM ? kGmBase : 0;
    data.layoutKind = ACLSAN_MEM_LAYOUT_SCALAR;
    data.layout.scalar.bytes = 4;
    data.dataBits = 32;
    data.alignSize = 4;
    data.instructionId = 56;
    return data;
}

class AtomicModePairTest : public testing::TestWithParam<std::tuple<uint32_t, uint32_t>> {};

TEST_P(AtomicModePairTest, OnlyMatchingAtomicOperationsAreCompatible)
{
    const auto [previous, current] = GetParam();
    std::vector<AclsanDeviceMemoryAccessData> records = {AtomicAccess(previous), AtomicAccess(current, 2)};
    MemStateTracker tracker(records);
    tracker.UpdateMemState(0, {});
    const auto candidates = tracker.CheckPotentialRace(1);
    ASSERT_EQ(candidates.size(), previous == current ? 0U : 1U);
    if (previous != current) {
        const auto reports = MakeRacecheckDiagnostics(candidates, records);
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(reports.front().kind, ConflictKind::ATOMIC_SEMANTIC);
        ReportRecord report;
        ASSERT_EQ(NormalizeRacecheckReport(reports.front(), &report), ReportRenderStatus::SUCCESS);
        EXPECT_EQ(report.fields.at("previousAccess"), kRaceNames.at(AtomicIndex(previous)));
        EXPECT_EQ(report.fields.at("currentAccess"), kRaceNames.at(AtomicIndex(current)));
    }
    tracker.UpdateMemState(1, {});
    const auto& segment = tracker.Tables().begin()->second.begin()->second;
    EXPECT_EQ(segment.atomicWriters.at(AtomicIndex(previous)).front(), 0U);
    EXPECT_EQ(segment.atomicWriters.at(AtomicIndex(current)).back(), 1U);
}

INSTANTIATE_TEST_SUITE_P(
    ScalarAtomics, AtomicModePairTest,
    testing::Combine(testing::ValuesIn(kAtomicModes), testing::ValuesIn(kAtomicModes)));

class AtomicModeTest : public testing::TestWithParam<uint32_t> {};

TEST_P(AtomicModeTest, OrdinaryReadsAndWritesConflictInBothDirections)
{
    for (const uint32_t ordinary : {1U, 2U}) {
        for (const bool atomicFirst : {false, true}) {
            SCOPED_TRACE(testing::Message() << ordinary << "/" << atomicFirst);
            std::vector<AclsanDeviceMemoryAccessData> records = {
                AtomicAccess(atomicFirst ? GetParam() : ordinary),
                AtomicAccess(atomicFirst ? ordinary : GetParam(), 2)};
            MemStateTracker tracker(records);
            tracker.UpdateMemState(0, {});
            const auto candidates = tracker.CheckPotentialRace(1);
            ASSERT_EQ(candidates.size(), 1U);
            const auto reports = MakeRacecheckDiagnostics(candidates, records);
            ASSERT_EQ(reports.size(), 1U);
            EXPECT_EQ(
                reports.front().kind, ordinary == 2 ? ConflictKind::WAW :
                                      atomicFirst   ? ConflictKind::RAW :
                                                      ConflictKind::WAR);
        }
    }
}

TEST_P(AtomicModeTest, MemcheckChecksBoundsOnceAndRendersTheAtomicOperation)
{
    Memcheck checker;
    AclsanResourceData allocation{};
    allocation.common.version = ACLSAN_API_VERSION;
    allocation.common.size = sizeof(allocation);
    allocation.ptr = reinterpret_cast<void*>(kGmBase);
    allocation.bytes = 4;
    allocation.deviceId = 2;
    allocation.resourceId = 1;
    allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    checker.OnAllocation(allocation);
    auto access = AtomicAccess(GetParam());
    access.layout.scalar.bytes = 8;
    checker.QueueDeviceMemoryAccess(access);
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().access.accessMode, ReportMode(GetParam()));
    EXPECT_EQ(reports.front().access.accessBytes, 8U);
    EXPECT_TRUE(checker.AnalysisComplete());
    std::string rendered;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(reports.front()), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(
        rendered.find(std::string("atomic write(") + kAtomicNames.at(AtomicIndex(GetParam())) + ")"),
        std::string::npos);
}

TEST_P(AtomicModeTest, EveryScalarAtomicInstructionCanCompleteRaceAnalysis)
{
    for (uint32_t instruction = 56; instruction <= 61; ++instruction) {
        SCOPED_TRACE(instruction);
        Racechecker checker;
        CheckerReportList reports;
        AclsanLaunchData launch{};
        launch.common.version = ACLSAN_API_VERSION;
        launch.common.size = sizeof(launch);
        launch.launchId = 17;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
        auto access = AtomicAccess(GetParam());
        access.instructionId = instruction;
        ASSERT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
        AclsanOperatorExecutionData completion{};
        completion.common = {ACLSAN_API_VERSION, sizeof(completion)};
        completion.launchId = launch.launchId;
        ASSERT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
        AclsanSynchronizeData sync{};
        sync.common.version = ACLSAN_API_VERSION;
        sync.common.size = sizeof(sync);
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
        EXPECT_TRUE(reports.empty());
    }
}

TEST_P(AtomicModeTest, MemoryInitcheckReadsTheOldValueBeforeInitializing)
{
    const InitStateKey scope{2, 0};
    MemoryInitcheck checker;
    ASSERT_EQ(checker.RegisterAllocation(scope, 1, kGmBase, 4), AllocationUpdateStatus::OK);
    ASSERT_EQ(checker.MarkInitialized(scope, kGmBase, 2), InitCheckStatus::OK);
    const auto result = checker.CheckInstruction(scope, {AtomicAccess(GetParam())});
    ASSERT_EQ(result.status, InitCheckStatus::OK);
    ASSERT_EQ(result.reports.size(), 1U);
    EXPECT_EQ(result.reports.front().access.accessMode, ReportMode(GetParam()));
    EXPECT_EQ(result.reports.front().uninitBytes, 2U);
    const auto later = checker.CheckInstruction(scope, {AtomicAccess(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 2)});
    ASSERT_EQ(later.status, InitCheckStatus::OK);
    EXPECT_TRUE(later.reports.empty());
}

INSTANTIATE_TEST_SUITE_P(ScalarAtomics, AtomicModeTest, testing::ValuesIn(kAtomicModes));

class AtomicInitcheckReplayBase : public testing::Test {
protected:
    void Launch(uint64_t id = 17)
    {
        AclsanLaunchData launch{};
        launch.common.version = ACLSAN_API_VERSION;
        launch.common.size = sizeof(launch);
        launch.launchId = id;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    }

    void SetUp() override
    {
        AclsanResourceData allocation{};
        allocation.common.version = ACLSAN_API_VERSION;
        allocation.common.size = sizeof(allocation);
        allocation.ptr = reinterpret_cast<void*>(kGmBase);
        allocation.bytes = 32;
        allocation.deviceId = 2;
        allocation.resourceId = 1;
        allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &allocation, reports));
        Launch();
    }

    void Access(const AclsanDeviceMemoryAccessData& access)
    {
        ASSERT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    }

    void Sync(uint64_t launchId = 17)
    {
        AclsanOperatorExecutionData completion{};
        completion.common = {ACLSAN_API_VERSION, sizeof(completion)};
        completion.launchId = launchId;
        ASSERT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
        AclsanSynchronizeData sync{};
        sync.common.version = ACLSAN_API_VERSION;
        sync.common.size = sizeof(sync);
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    }

    Initcheck checker;
    CheckerReportList reports;
};

class AtomicInitcheckReplayTest : public AtomicInitcheckReplayBase,
                                  public testing::WithParamInterface<std::tuple<uint32_t, uint32_t>> {};

TEST_P(AtomicInitcheckReplayTest, AllAtomicModesCheckUninitializedOldValuesInEveryTrackedSpace)
{
    const auto [mode, space] = GetParam();
    Access(AtomicAccess(mode, 1, space));
    Sync();
    EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.access.accessMode, ReportMode(mode));
    EXPECT_EQ(report.uninitBytes, 4U);
}

class AtomicCasInitcheckReplayTest : public AtomicInitcheckReplayBase, public testing::WithParamInterface<uint32_t> {};

TEST_P(AtomicCasInitcheckReplayTest, CasInitializesASubsequentRead)
{
    const auto space = GetParam();
    Access(AtomicAccess(kCas, 1, space));
    Access(AtomicAccess(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 2, space));
    Sync();
    EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& cas = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(cas.common.exec.instrExecId, 1U);
    EXPECT_EQ(cas.uninitBytes, 4U);
    if (space == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
        reports.clear();
        Launch(18);
        auto read = AtomicAccess(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1, space);
        read.header.launchId = 18;
        Access(read);
        Sync(18);
        EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
        EXPECT_TRUE(reports.empty());
    }
}

INSTANTIATE_TEST_SUITE_P(
    ScalarAtomics, AtomicInitcheckReplayTest,
    testing::Combine(
        testing::ValuesIn(kAtomicModes),
        testing::Values(
            ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_SPACE_L0A,
            ACLSAN_DEVICE_MEMORY_SPACE_L0B, ACLSAN_DEVICE_MEMORY_SPACE_L0C)));

INSTANTIATE_TEST_SUITE_P(
    ScalarAtomics, AtomicCasInitcheckReplayTest,
    testing::Values(
        ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_SPACE_L0A,
        ACLSAN_DEVICE_MEMORY_SPACE_L0B, ACLSAN_DEVICE_MEMORY_SPACE_L0C));

TEST(ScalarAtomicChecker, UnknownAtomicHeaderStillPreventsRaceAndInitializationAnalysis)
{
    auto access = AtomicAccess(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
    Racechecker race;
    Initcheck init;
    CheckerReportList reports;
    for (Checker* checker : {static_cast<Checker*>(&race), static_cast<Checker*>(&init)}) {
        AclsanLaunchData launch{};
        launch.common.version = ACLSAN_API_VERSION;
        launch.common.size = sizeof(launch);
        launch.launchId = 17;
        ASSERT_TRUE(checker->OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
        ASSERT_TRUE(checker->OnCallback(
            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
        AclsanOperatorExecutionData completion{};
        completion.common = {ACLSAN_API_VERSION, sizeof(completion)};
        completion.launchId = launch.launchId;
        ASSERT_TRUE(checker->OnCallback(
            ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
        AclsanSynchronizeData sync{};
        sync.common.version = ACLSAN_API_VERSION;
        sync.common.size = sizeof(sync);
        ASSERT_TRUE(
            checker->OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        EXPECT_FALSE(checker->AnalysisComplete());
    }
    EXPECT_TRUE(reports.empty());
}

TEST(ScalarAtomicChecker, NewModesUseExpectedAccessAndReportValues)
{
    EXPECT_EQ(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_EXCH, 6);
    EXPECT_EQ(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_CAS, 7);
    EXPECT_EQ(static_cast<uint32_t>(NpuCheckReportAccessMode::FREE), 4U);
    EXPECT_EQ(static_cast<uint32_t>(NpuCheckReportAccessMode::ATOMIC_WRITE_EXCH), 7U);
    EXPECT_EQ(static_cast<uint32_t>(NpuCheckReportAccessMode::ATOMIC_WRITE_CAS), 8U);
}

TEST(ScalarAtomicChecker, UnknownAtomicHeaderCannotCommitInitializationThroughTheMemoryApi)
{
    const InitStateKey scope{2, 0};
    MemoryInitcheck checker;
    ASSERT_EQ(checker.RegisterAllocation(scope, 1, kGmBase, 4), AllocationUpdateStatus::OK);
    auto write = AtomicAccess(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    write.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
    const auto result = checker.CheckInstruction(scope, {write});
    EXPECT_EQ(result.status, InitCheckStatus::INCOMPLETE_RECORD);
    EXPECT_TRUE(result.reports.empty());
    const auto read = checker.CheckInstruction(scope, {AtomicAccess(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 2)});
    ASSERT_EQ(read.status, InitCheckStatus::OK);
    ASSERT_EQ(read.reports.size(), 1U);
    EXPECT_EQ(read.reports.front().uninitBytes, 4U);
}
} // namespace
} // namespace npucheck
