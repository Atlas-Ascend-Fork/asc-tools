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
#include "aclsan_simt_ub_trace.h"
#include "aclsan_ub_layout_builder.h"
#include "dbi/ctrlbin/ctrlbin_bindings.h"
#include "device_instr/common/instruction_id.h"
#include "checker/initcheck.h"
#include "checker/racecheck.h"
#include "checker/mem_state_tracker.h"
#include <gtest/gtest.h>
#include <limits>
#include <cstring>
#include <memory>

namespace npucheck {
namespace {
AclsanSimtUbMemoryAccessData SimtWrite(uint64_t address, uint64_t bytes, uint32_t thread = 0)
{
    AclsanSimtUbMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.sourceKind = ACLSAN_DEVICE_SOURCE_VECTOR;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT | ACLSAN_DEVICE_EVENT_FLAG_SIMT_UB;
    data.header.launchId = 1;
    data.header.instrExecId = 2;
    data.header.pc = 0x100;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    data.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = bytes;
    data.address = address;
    data.simt.threadX = thread;
    data.simt.blockDimX = 32;
    data.simt.blockDimY = data.simt.blockDimZ = 1;
    data.simt.validFlags = ACLSAN_SIMT_CONTEXT_THREAD | ACLSAN_SIMT_CONTEXT_BLOCK_DIM | ACLSAN_SIMT_CONTEXT_ADDRESS;
    data.ubLayout = {0x40000, {{0, 0x3000}, {0x3000, 0x13080}, {0x13080, 0x15080}, {0x20000, 0x40000}}, 1, 0};
    return data;
}

std::string Render(const NpuCheckMemcheckReport& report)
{
    std::string text;
    EXPECT_EQ(RenderNpuCheckReportRecord(NpuCheckReportRecord::From(report), {}, &text), ReportRenderStatus::SUCCESS);
    return text;
}

// Allocate only the pre-UB ABI prefix, so consumers must not copy the optional tail.
std::unique_ptr<char[]> LegacyGmWrite()
{
    auto data = SimtWrite(0x1000, 8);
    data.header.size = sizeof(AclsanDeviceMemoryAccessData);
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    data.accessCount = 1;
    auto buffer = std::make_unique<char[]>(data.header.size);
    std::memcpy(buffer.get(), &data, data.header.size);
    return buffer;
}

TEST(SimtUbMemcheckTest, LegacyGmPrefixAcceptedByInitcheck)
{
    auto buffer = LegacyGmWrite();
    auto& data = *reinterpret_cast<AclsanDeviceMemoryAccessData*>(buffer.get());
    data.regDependencyMask0 = UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CTRL;
    Initcheck checker;
    CheckerReportList reports;
    EXPECT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, buffer.get(), reports));
    EXPECT_EQ(checker.Stats().malformedEvents, 0U);
    EXPECT_EQ(reports.size(), 1U);
}

TEST(SimtUbMemcheckTest, LegacyGmPrefixAcceptedByStreamingRacecheckAndTracker)
{
    auto buffer = LegacyGmWrite();
    const auto& data = *reinterpret_cast<const AclsanDeviceMemoryAccessData*>(buffer.get());
    EXPECT_NO_THROW(racecheck::PrepareAccess(data));
    Racecheck checker;
    CheckerReportList reports;
    EXPECT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, buffer.get(), reports));
}

TEST(SimtUbMemcheckTest, LegacyGmPrefixAcceptedByProductionRacecheck)
{
    auto buffer = LegacyGmWrite();
    Racechecker checker;
    CheckerReportList reports;
    AclsanLaunchData launch{};
    launch.common.version = ACLSAN_API_VERSION;
    launch.common.size = sizeof(launch);
    launch.launchId = 1;
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    EXPECT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, buffer.get(), reports));
    AclsanOperatorExecutionData completion{};
    completion.common.version = ACLSAN_API_VERSION;
    completion.common.size = sizeof(completion);
    completion.launchId = launch.launchId;
    EXPECT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
    AclsanSynchronizeData sync{};
    sync.common.version = ACLSAN_API_VERSION;
    sync.common.size = sizeof(sync);
    sync.common.correlationId = launch.launchId;
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    EXPECT_TRUE(checker.AnalysisComplete());
    EXPECT_TRUE(reports.empty());
}

TEST(SimtUbMemcheckTest, SharedRegionExactEndpointsAndDynamicPaddingAreLegal)
{
    Memcheck checker;
    for (const auto& range :
         {std::pair<uint64_t, uint64_t>{0, 0x3000},
          {0x3000, 0x10080},
          {0x13080, 0x2000},
          {0x20000, 0x20000},
          {0x13000, 4},
          {0x13078, 8},
          {0x13001, 0x7f}}) {
        checker.QueueSimtUbMemoryAccess(SimtWrite(range.first, range.second));
    }
    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().deviceOperations, 7U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, DataCacheWritesWithinRegionAreLegalForEveryCacheSize)
{
    for (uint64_t dynamic : {0x10001U, 0x23000U, 0x33000U}) {
        AclsanUbLayout layout{};
        const AclsanUbLaunchMetadata metadata{
            0x5000, 0x3000, dynamic, 3, ACLSAN_UB_METADATA_COMPILER | ACLSAN_UB_METADATA_STATIC};
        ASSERT_TRUE(aclsan::BuildSimtUbLayout(aclsan::SocVersion::DAV_3510, metadata, layout));
        for (uint64_t bytes : {1U, 2U, 4U, 8U, 16U}) {
            for (uint64_t address :
                 {layout.regions[3].begin, layout.regions[3].begin + 0x100U, layout.physicalBytes - bytes}) {
                Memcheck checker;
                auto access = SimtWrite(address, bytes, 7);
                access.ubLayout = layout;
                access.alignSize = bytes;
                checker.QueueSimtUbMemoryAccess(access);
                const auto reports = checker.OnSynchronization();
                EXPECT_TRUE(reports.empty()) << dynamic << ":" << address << ":" << bytes;
                EXPECT_TRUE(checker.OnSynchronization().empty());
                EXPECT_TRUE(checker.AnalysisComplete());
            }
        }
    }
}

TEST(SimtUbMemcheckTest, CrossingAdjacentRegionsReportsOnceWithStartingRegion)
{
    for (const auto& range : {std::pair<uint64_t, uint64_t>{0x2ffc, 8}, {0x13078, 16}, {0x1507c, 8}}) {
        Memcheck checker;
        checker.QueueSimtUbMemoryAccess(SimtWrite(range.first, range.second, 7));
        const auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(reports[0].common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
        EXPECT_EQ(reports[0].access.address, range.first);
        EXPECT_EQ(reports[0].access.accessBytes, range.second);
        ASSERT_TRUE(reports[0].simtUb);
        EXPECT_EQ(reports[0].simtUb->threadId, 7U);
        const auto text = Render(reports[0]);
        const auto region = range.first < 0x3000  ? "static memory" :
                            range.first < 0x13080 ? "dynamic memory" :
                                                    "reserved space";
        EXPECT_NE(text.find(std::string("Invalid UB ") + region + " write"), std::string::npos);
    }
    Memcheck checker;
    auto adjacent = SimtWrite(0x1fffc, 8);
    adjacent.ubLayout.regions[1].end = 0x1e000;
    adjacent.ubLayout.regions[2] = {0x1e000, 0x20000};
    checker.QueueSimtUbMemoryAccess(adjacent);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
}

TEST(SimtUbMemcheckTest, HolesPhysicalBoundsAndArithmeticOverflowAreInvalid)
{
    for (const auto& range :
         {std::pair<uint64_t, uint64_t>{0x15080, 4},
          {0x1fffc, 8},
          {0x3fffc, 8},
          {0x40000, 4},
          {UINT64_MAX - 3, 8},
          {1, UINT64_MAX}}) {
        Memcheck checker;
        checker.QueueSimtUbMemoryAccess(SimtWrite(range.first, range.second));
        const auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1U) << range.first;
        const auto text = Render(reports[0]);
        if (range.first >= 0x15080 && range.first < 0x20000)
            EXPECT_NE(text.find("Invalid UB unallocated space write"), std::string::npos);
        if (range.first == 0x3fffc)
            EXPECT_NE(text.find("Invalid UB Data Cache write"), std::string::npos);
        EXPECT_TRUE(checker.AnalysisComplete());
    }
}

TEST(SimtUbMemcheckTest, PerThreadWritesAndThreadZeroRemainSeparate)
{
    Memcheck checker;
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x2ffc, 4, 1));
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x2ffc, 8, 0));
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].simtUb->threadId, 0U);
    EXPECT_EQ(reports[0].common.exec.pc, 0x100U);
    const auto text = Render(reports[0]);
    EXPECT_NE(text.find("SIMT thread: 0"), std::string::npos);
    EXPECT_NE(text.find("dynamic memory [0x3000, 0x13080)"), std::string::npos);
    EXPECT_NE(text.find("Data Cache [0x20000, 0x40000)"), std::string::npos);
    EXPECT_LT(text.find("Address 0x2ffc"), text.find("UB allocated regions:"));
}

TEST(SimtUbMemcheckTest, AtomicAndReductionWriteModesUseIdenticalBounds)
{
    for (uint32_t mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        Memcheck checker;
        auto access = SimtWrite(0x2ffe, 4);
        access.accessMode = mode;
        checker.QueueSimtUbMemoryAccess(access);
        EXPECT_EQ(checker.OnSynchronization().size(), 1U);
        EXPECT_TRUE(checker.AnalysisComplete());
    }
}

TEST(SimtUbMemcheckTest, ReadsInactiveThreadsAndZeroWritesHaveNoViolation)
{
    Memcheck checker;
    auto access = SimtWrite(0x40000, 4);
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    checker.QueueSimtUbMemoryAccess(access);
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_PREDICATED;
    checker.QueueSimtUbMemoryAccess(access);
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x40000, 0));
    access = SimtWrite(0x40000, 4);
    access.layoutKind = ACLSAN_MEM_LAYOUT_SCALAR;
    access.layout.scalar.bytes = 0;
    checker.QueueSimtUbMemoryAccess(access);
    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
    EXPECT_EQ(checker.Stats().deviceOperations, 0U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, IgnoredUbEventsDoNotHideCollectionLoss)
{
    auto read = SimtWrite(0x40000, 4);
    read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    auto inactive = SimtWrite(0x40000, 4);
    inactive.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_PREDICATED;
    for (uint32_t flag :
         {ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR, ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED,
          ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED}) {
        for (auto access : {SimtWrite(0x40000, 0), read, inactive}) {
            Memcheck checker;
            access.header.flags |= flag;
            checker.QueueSimtUbMemoryAccess(access);
            EXPECT_TRUE(checker.OnSynchronization().empty());
            EXPECT_GT(checker.Stats().droppedDeviceOperations, 0U);
            EXPECT_FALSE(checker.AnalysisComplete());
        }
    }
}

TEST(SimtUbMemcheckTest, UnfinishedUbQueueRemainsIncompleteAfterFinalization)
{
    Memcheck checker;
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x15080, 4));
    CheckerReportList reports;
    checker.Finalize(reports);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, InvalidUbRecordIdentityCannotBecomeACompletedAnalysis)
{
    for (bool missingLaunch : {false, true}) {
        Memcheck checker;
        auto access = SimtWrite(0x15080, 4);
        if (missingLaunch)
            access.header.launchId = 0;
        else
            access.header.instrExecId = 0;
        checker.QueueSimtUbMemoryAccess(access);
        EXPECT_TRUE(checker.OnSynchronization().empty());
        EXPECT_EQ(checker.Stats().droppedDeviceOperations, 1U);
        EXPECT_FALSE(checker.AnalysisComplete());
    }
}

TEST(SimtUbMemcheckTest, ManagedUnknownUbLaunchPreservesReliableErrorsButIsIncomplete)
{
    auto analysis = std::make_shared<MemoryAnalysis>(true);
    Memcheck checker(analysis);
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x15080, 4));
    CheckerReportList reports;
    checker.Finalize(reports);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, ManagedLateUbRecordCannotRestoreCompletedAnalysis)
{
    auto analysis = std::make_shared<MemoryAnalysis>(true);
    Memcheck checker(analysis);
    CheckerReportList reports;
    AclsanLaunchData launch{};
    launch.common.version = ACLSAN_API_VERSION;
    launch.common.size = sizeof(launch);
    launch.launchId = 1;
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x3000, 4));
    AclsanOperatorExecutionData completion{};
    completion.common.version = ACLSAN_API_VERSION;
    completion.common.size = sizeof(completion);
    completion.launchId = 1;
    ASSERT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
    ASSERT_TRUE(checker.AnalysisComplete());
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x15080, 4));
    checker.Finalize(reports);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, ManagedUbBeforeLaunchCannotBeConfirmedByALaterLaunch)
{
    auto analysis = std::make_shared<MemoryAnalysis>(true);
    Memcheck checker(analysis);
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x15080, 4));
    CheckerReportList reports;
    AclsanLaunchData launch{};
    launch.common.version = ACLSAN_API_VERSION;
    launch.common.size = sizeof(launch);
    launch.launchId = 1;
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    AclsanOperatorExecutionData completion{};
    completion.common.version = ACLSAN_API_VERSION;
    completion.common.size = sizeof(completion);
    completion.launchId = 1;
    ASSERT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
    EXPECT_FALSE(checker.AnalysisComplete());
    checker.Finalize(reports);
    EXPECT_EQ(reports.size(), 1U);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, StandaloneMixedGmUbQueuesRetainPendingCountsAcrossCompletions)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
        const bool ubFirst = (scenario & 1) != 0;
        const bool gmCompletesFirst = (scenario & 2) != 0;
        Memcheck checker;
        auto ub = SimtWrite(0x15080, 4);
        auto gm = SimtWrite(0x1000, 4);
        gm.header.launchId = 2;
        gm.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        gm.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        CheckerReportList reports;
        AclsanLaunchData launch{};
        launch.common.version = ACLSAN_API_VERSION;
        launch.common.size = sizeof(launch);
        launch.launchId = gm.header.launchId;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
        if (ubFirst)
            checker.QueueSimtUbMemoryAccess(ub);
        checker.QueueDeviceMemoryAccess(gm);
        if (!ubFirst)
            checker.QueueSimtUbMemoryAccess(ub);
        EXPECT_EQ(checker.Stats().pendingDeviceOperations, 2U);
        AclsanOperatorExecutionData completion{};
        completion.common.version = ACLSAN_API_VERSION;
        completion.common.size = sizeof(completion);
        completion.launchId = gmCompletesFirst ? 2 : 1;
        ASSERT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
        EXPECT_EQ(checker.Stats().pendingDeviceOperations, 1U);
        EXPECT_FALSE(checker.AnalysisComplete());
        completion.launchId = gmCompletesFirst ? 1 : 2;
        ASSERT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
        EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
        EXPECT_TRUE(checker.AnalysisComplete());
        EXPECT_EQ(reports.size(), 2U);
    }
}

TEST(SimtUbMemcheckTest, MissingLayoutThreadOrExactRangeMarksIncompleteWithoutOob)
{
    for (int invalid = 0; invalid < 10; ++invalid) {
        Memcheck checker;
        auto access = SimtWrite(0x40000, 4);
        if (invalid == 0)
            access.header.size = sizeof(AclsanDeviceMemoryAccessData);
        if (invalid == 1)
            access.ubLayout.valid = 0;
        if (invalid == 2)
            access.ubLayout.regions[1].begin = 0x2000;
        if (invalid == 3)
            access.ubLayout.regions[3].end = 0x50000;
        if (invalid == 4)
            access.simt.validFlags = 0;
        if (invalid == 5)
            access.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
        if (invalid == 6)
            access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
        if (invalid == 7)
            access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED;
        if (invalid == 8)
            access.header.version = ACLSAN_API_VERSION + 1;
        if (invalid == 9)
            access.accessMode = UINT32_MAX;
        checker.QueueSimtUbMemoryAccess(access);
        EXPECT_TRUE(checker.OnSynchronization().empty()) << invalid;
        EXPECT_FALSE(checker.AnalysisComplete()) << invalid;
        EXPECT_EQ(checker.Stats().droppedDeviceOperations, 1U) << invalid;
    }
}

TEST(SimtUbMemcheckTest, DroppedRecordsRemainIncompleteButReliableWritesAreChecked)
{
    Memcheck checker;
    auto access = SimtWrite(0x2ffc, 8);
    access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR;
    checker.QueueSimtUbMemoryAccess(access);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, UbCollectionFailureMarkerMarksManagedAndStandaloneIncomplete)
{
    for (bool managed : {false, true}) {
        auto analysis = std::make_shared<MemoryAnalysis>(managed);
        Memcheck checker(analysis);
        AclsanLaunchData launch{};
        launch.common.version = ACLSAN_API_VERSION;
        launch.common.size = sizeof(launch);
        launch.launchId = 1;
        if (managed) {
            ASSERT_TRUE(analysis->BeginCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch));
            analysis->EndCallback();
        }
        auto marker = SimtWrite(0, 0);
        marker.header.flags = ACLSAN_DEVICE_EVENT_FLAG_SIMT_UB | ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED |
                              ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR;
        marker.ubLayout = {};
        marker.simt = {};
        CheckerReportList reports;
        EXPECT_TRUE(checker.OnCallback(
            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, &marker, reports));
        EXPECT_TRUE(reports.empty());
        EXPECT_TRUE(checker.OnSynchronization().empty());
        EXPECT_GT(checker.Stats().droppedDeviceOperations, 0U);
        EXPECT_FALSE(checker.AnalysisComplete());
    }
}

TEST(SimtUbMemcheckTest, ManagedUbQueueDoesNotEnterOrdinaryDependencyGraph)
{
    auto analysis = std::make_shared<MemoryAnalysis>(true);
    Memcheck checker(analysis);
    AclsanLaunchData launch{};
    launch.common.version = ACLSAN_API_VERSION;
    launch.common.size = sizeof(launch);
    launch.launchId = 1;
    ASSERT_TRUE(analysis->BeginCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch));
    analysis->EndCallback();
    auto legacy = LegacyGmWrite();
    ASSERT_TRUE(
        analysis->BeginCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, legacy.get()));
    analysis->EndCallback();
    auto ub = SimtWrite(0x3000, 4);
    CheckerReportList reports;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, &ub, reports));
    EXPECT_EQ(analysis->Get(1).memory.size(), 1U);
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 1U);
    std::set<std::string> reasons;
    (void)analysis->Graph(1, race::Limits{}, reasons);
    EXPECT_EQ(reasons.count("owner_mapping_unknown"), 0U);
}

TEST(SimtUbMemcheckTest, OrdinaryScalarAndDmaUbWritesCheckPartitionBoundaries)
{
    struct Scenario {
        uint32_t source;
        uint32_t pipeline;
        aclsan::InstructionId instruction;
        uint32_t kernelType;
        uint64_t address;
        uint64_t bytes;
        uint64_t regionBase;
        uint64_t regionBytes;
    };
    const Scenario scenarios[] = {
        {ACLSAN_DEVICE_SOURCE_ST, ACLSAN_DEVICE_PIPE_SCALAR, aclsan::InstructionId::StpB32, ACLSAN_KERNEL_VF_TYPE_NO_VF,
         0x3dffc, 8, 0, 0x3e000},
        {ACLSAN_DEVICE_SOURCE_MTE2, ACLSAN_DEVICE_PIPE_MTE2, aclsan::InstructionId::CopyGmToUbufAlignV2B8,
         ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY, 0x15fe0, 64, 0xe000, 0x8000},
    };
    for (const auto& scenario : scenarios) {
        SCOPED_TRACE(::testing::Message() << "source=" << scenario.source);
        Memcheck checker;
        CheckerReportList callbacks;
        AclsanLaunchData launch{};
        launch.common.version = ACLSAN_API_VERSION;
        launch.common.size = sizeof(launch);
        launch.launchId = 1;
        launch.ubMetadataValid = 1;
        launch.kernelVfType = scenario.kernelType;
        launch.compilerAllocUbBytes = scenario.regionBase == 0 ? 0 : 0x10000;
        launch.dynamicUbBytes = scenario.regionBase == 0 ? 0 : 0x8000;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, callbacks));
        AclsanDeviceMemoryAccessData access{};
        access.header.version = ACLSAN_API_VERSION;
        access.header.size = sizeof(access);
        access.header.launchId = launch.launchId;
        access.header.instrExecId = 2;
        access.header.sourceKind = scenario.source;
        access.header.pipeline = scenario.pipeline;
        access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        access.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        access.memoryOwner = access.header.executor;
        access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
        access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        access.accessCount = 1;
        access.instructionId = static_cast<uint32_t>(scenario.instruction);
        access.address = scenario.address;
        access.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        access.layout.range.bytes = scenario.bytes;
        checker.QueueDeviceMemoryAccess(access);
        const auto reports = checker.OnSynchronization();
        EXPECT_EQ(reports.size(), 1U);
        EXPECT_EQ(checker.Stats().deviceOperations, 1U);
        EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
        EXPECT_TRUE(checker.AnalysisComplete());
        if (reports.size() != 1U)
            continue;
        EXPECT_EQ(reports[0].access.address, scenario.address);
        EXPECT_EQ(reports[0].access.accessBytes, scenario.bytes);
        EXPECT_EQ(reports[0].allocation.base, scenario.regionBase);
        EXPECT_EQ(reports[0].allocation.bytes, scenario.regionBytes);
        EXPECT_TRUE(reports[0].ubContext);
        EXPECT_FALSE(reports[0].simtUb);
    }
}

TEST(SimtUbMemcheckTest, EmptyStaticAndDynamicRegionsAndLargeCrossingAreHandledOnce)
{
    Memcheck checker;
    auto access = SimtWrite(0, 8);
    access.ubLayout.regions[0] = {0, 0};
    access.ubLayout.regions[1] = {0, 0};
    access.ubLayout.regions[2] = {0, 8192};
    checker.QueueSimtUbMemoryAccess(access);
    EXPECT_TRUE(checker.OnSynchronization().empty());
    access.address = 8190;
    checker.QueueSimtUbMemoryAccess(access);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x2ff0, 0x1d020));
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
}

TEST(SimtUbMemcheckTest, ExistingInvalidAccessTemplateCanBeOverridden)
{
    Memcheck checker;
    checker.QueueSimtUbMemoryAccess(SimtWrite(0x13078, 16));
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    ReportTemplateOverrides overrides{{{ReportTool::MEMCHECK, "invalid_access"}, {"CUSTOM {{space}} {{address}}\n"}}};
    std::string text;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(reports[0]), overrides, &text),
        ReportRenderStatus::SUCCESS);
    EXPECT_EQ(text.find("CUSTOM UB dynamic memory 13078"), 0U);
    EXPECT_NE(text.find("SIMT thread: 0"), std::string::npos);
    EXPECT_NE(text.find("reserved space [0x13080, 0x15080)"), std::string::npos);
}

TEST(SimtUbMemcheckTest, ShortCallbackCannotReadAnAbsentLayoutTail)
{
    Memcheck checker;
    const auto access = SimtWrite(0x40000, 4);
    alignas(AclsanDeviceMemoryAccessData) unsigned char prefix[sizeof(AclsanDeviceMemoryAccessData)]{};
    std::memcpy(prefix, &access, sizeof(prefix));
    reinterpret_cast<AclsanDeviceEventHeader*>(prefix)->size = sizeof(prefix);
    CheckerReportList reports;
    EXPECT_FALSE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, prefix, reports));
    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(SimtUbMemcheckTest, ManagedInterleavedLaunchesRetainTheirOwnLayouts)
{
    auto analysis = std::make_shared<MemoryAnalysis>(true);
    Memcheck checker(analysis);
    CheckerReportList reports;
    const auto callback = [&](AclsanCallbackDomain domain, AclsanCallbackId id, const void* data) {
        const bool privateUb = IsSimtUbMemcheckCallback(domain, id, data);
        if (!privateUb)
            EXPECT_TRUE(analysis->BeginCallback(domain, id, data));
        EXPECT_TRUE(checker.OnCallback(domain, id, data, reports));
        if (!privateUb)
            analysis->EndCallback();
    };
    AclsanLaunchData launch{};
    launch.common.version = ACLSAN_API_VERSION;
    launch.common.size = sizeof(launch);
    launch.launchId = 1;
    launch.stream = reinterpret_cast<void*>(1);
    callback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch);
    launch.launchId = 2;
    launch.stream = reinterpret_cast<void*>(2);
    callback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch);
    auto first = SimtWrite(0x13078, 16, 13);
    callback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, &first);
    auto second = first;
    second.header.launchId = 2;
    second.simt.threadX = 0;
    second.ubLayout.regions[1].end = 0x1e000;
    second.ubLayout.regions[2] = {0x1e000, 0x20000};
    callback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, &second);
    EXPECT_TRUE(analysis->Get(1).skippedFragments.empty());
    EXPECT_TRUE(analysis->Get(2).skippedFragments.empty());
    EXPECT_TRUE(reports.empty());
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 2U);
    AclsanOperatorExecutionData completion{};
    completion.common.version = ACLSAN_API_VERSION;
    completion.common.size = sizeof(completion);
    completion.launchId = 2;
    completion.stream = reinterpret_cast<void*>(2);
    callback(ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion);
    EXPECT_TRUE(reports.empty());
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 1U);
    completion.launchId = 1;
    completion.stream = reinterpret_cast<void*>(1);
    callback(ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
    EXPECT_EQ(checker.Stats().errors, 1U);
    EXPECT_TRUE(checker.AnalysisComplete());
}
TEST(SimtUbMemcheckTest, ConvertedRecordCarriesLaunchLayoutIntoCheckerAndReport)
{
    aclsan::ParsedSimtTraceRecord parsed{};
    parsed.launchId = 17;
    parsed.instrExecId = 3;
    parsed.record.instructionId = static_cast<uint32_t>(InstrType::SIMT_STS_B128);
    parsed.record.pc = 0x1234;
    parsed.record.base = 0x13078;
    parsed.record.blockDimX = 32;
    parsed.record.blockDimY = parsed.record.blockDimZ = 1;
    parsed.record.threadX = 7;
    const AclsanUbLaunchMetadata metadata{
        0x5000, 0x3000, 0x10001, 3, ACLSAN_UB_METADATA_COMPILER | ACLSAN_UB_METADATA_STATIC};
    AclsanUbLayout layout{};
    ASSERT_TRUE(aclsan::BuildSimtUbLayout(aclsan::SocVersion::DAV_3510, metadata, layout));
    const auto converted = aclsan::TranslateSimtUbTraceRecord(parsed, layout);
    ASSERT_TRUE(converted);
    Memcheck checker;
    checker.QueueSimtUbMemoryAccess(*converted);
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].common.exec.launchId, 17U);
    EXPECT_EQ(reports[0].common.exec.pc, 0x1234U);
    EXPECT_EQ(reports[0].simtUb->threadId, 7U);
    EXPECT_NE(Render(reports[0]).find("Invalid UB dynamic memory write of size 16 bytes"), std::string::npos);
    auto missing = *converted;
    missing.ubLayout.valid = 0;
    Memcheck incomplete;
    incomplete.QueueSimtUbMemoryAccess(missing);
    EXPECT_TRUE(incomplete.OnSynchronization().empty());
    EXPECT_FALSE(incomplete.AnalysisComplete());
}

} // namespace
} // namespace npucheck
