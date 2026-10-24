// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "checker/initcheck.h"
#include "device_instr/common/instruction_id.h"

#include <gtest/gtest.h>

namespace npucheck {
namespace {

constexpr uint32_t kA = ACLSAN_DEVICE_MEMORY_SPACE_L0A;
constexpr uint32_t kB = ACLSAN_DEVICE_MEMORY_SPACE_L0B;
constexpr uint32_t kRead = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
constexpr uint32_t kWrite = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
constexpr uint32_t kMte1 = ACLSAN_DEVICE_PIPE_MTE1;
constexpr uint32_t kMatrix = ACLSAN_DEVICE_PIPE_MATRIX;

class InitcheckMemoryTest : public testing::Test {
protected:
    void Launch(uint64_t id = 11, void* stream = nullptr)
    {
        AclsanLaunchData data{};
        data.common.version = ACLSAN_API_VERSION;
        data.common.size = sizeof(data);
        data.launchId = id;
        data.stream = stream;
        EXPECT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &data, reports));
    }

    AclsanDeviceMemoryAccessData Range(
        uint64_t instruction, uint32_t space, uint32_t mode, uint64_t address = 0, uint64_t bytes = 512,
        uint32_t pipe = kMatrix)
    {
        AclsanDeviceMemoryAccessData data{};
        data.header.version = ACLSAN_API_VERSION;
        data.header.size = sizeof(data);
        data.header.launchId = 11;
        data.header.instrExecId = instruction;
        data.header.deviceId = 2;
        data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        data.header.pipeline = pipe;
        data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        data.memorySpace = space;
        data.accessMode = mode;
        data.accessCount = 1;
        data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        data.layout.range.bytes = bytes;
        data.address = address;
        data.dataBits = 16;
        data.instructionId = mode == kWrite ? 142 : 401;
        return data;
    }

    bool Memory(const AclsanDeviceMemoryAccessData& data)
    {
        return checker.OnCallback(
            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &data, reports);
    }

    void Sync(void* stream = nullptr, int result = 0)
    {
        AclsanSynchronizeData data{};
        data.common.version = ACLSAN_API_VERSION;
        data.common.size = sizeof(data);
        data.common.result = result;
        data.stream = stream;
        EXPECT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &data, reports));
    }

    void Flag(uint64_t instruction, uint32_t action, uint64_t event = 3)
    {
        AclsanDeviceSyncData data{};
        data.header = Range(instruction, kA, kRead).header;
        data.header.size = sizeof(data);
        data.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        data.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
        data.action = action;
        data.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
        data.srcPipe = kMte1;
        data.dstPipe = kMatrix;
        data.objectId = event;
        EXPECT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &data, reports));
    }

    void Barrier(uint64_t instruction, uint32_t pipe)
    {
        AclsanDeviceBarrierData data{};
        data.header = Range(instruction, kA, kRead).header;
        data.header.size = sizeof(data);
        data.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        data.pipe = pipe;
        EXPECT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER, &data, reports));
    }

    Initcheck checker;
    CheckerReportList reports;
};

TEST_F(InitcheckMemoryTest, UntouchedL0AReadReportsAllBytes)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    EXPECT_TRUE(reports.empty());
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
    EXPECT_EQ(report.access.memorySpace, NpuCheckReportMemorySpace::L0_A);
    EXPECT_EQ(report.firstUninitAddress, 0U);
    EXPECT_EQ(report.uninitBytes, 512U);
    EXPECT_EQ(report.initializedBytes, 0U);
    EXPECT_TRUE(checker.HasErrors());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, PartialL0BReadReportsExactMissingBytes)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kB, kWrite)));
    ASSERT_TRUE(Memory(Range(2, kB, kRead, 0, 1024)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ);
    EXPECT_EQ(report.access.memorySpace, NpuCheckReportMemorySpace::L0_B);
    EXPECT_EQ(report.firstUninitAddress, 512U);
    EXPECT_EQ(report.uninitBytes, 512U);
    EXPECT_EQ(report.initializedBytes, 512U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, FullSamePipelineWriteInitializesRead)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 1024)));
    ASSERT_TRUE(Memory(Range(2, kA, kRead, 0, 1024)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.HasErrors());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, L0AWriteDoesNotInitializeL0B)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite)));
    ASSERT_TRUE(Memory(Range(2, kB, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).access.memorySpace, NpuCheckReportMemorySpace::L0_B);
}

TEST_F(InitcheckMemoryTest, ReplaysShuffledCallbacksByInstructionOrder)
{
    Launch();
    ASSERT_TRUE(Memory(Range(3, kA, kRead)));
    ASSERT_TRUE(Memory(Range(1, kA, kWrite)));
    ASSERT_TRUE(Memory(Range(2, kB, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).common.exec.instrExecId, 2U);
}

TEST_F(InitcheckMemoryTest, FutureWriteDoesNotRepairEarlierRead)
{
    Launch();
    ASSERT_TRUE(Memory(Range(2, kA, kWrite)));
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    ASSERT_TRUE(Memory(Range(3, kA, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).common.exec.instrExecId, 1U);
}

TEST_F(InitcheckMemoryTest, ReadWriteChecksExistingBytesBeforeWriting)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kRead | kWrite)));
    ASSERT_TRUE(Memory(Range(2, kA, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).common.exec.instrExecId, 1U);
}

TEST_F(InitcheckMemoryTest, EveryInstructionReadPrecedesItsWrites)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.accessCount = 2;
    auto read = Range(1, kA, kRead);
    read.instructionId = write.instructionId;
    read.accessCount = 2;
    read.accessIndex = 1;
    ASSERT_TRUE(Memory(write));
    ASSERT_TRUE(Memory(read));
    ASSERT_TRUE(Memory(Range(2, kA, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).common.exec.instrExecId, 1U);
}

TEST_F(InitcheckMemoryTest, SparseWriteKeepsStrideGapsUninitialized)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    write.layout.blockRepeat = {3, 32, 64, 1, 0, 0};
    ASSERT_TRUE(Memory(write));
    ASSERT_TRUE(Memory(Range(2, kA, kRead, 0, 160)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.firstUninitAddress, 32U);
    EXPECT_EQ(report.uninitBytes, 64U);
    EXPECT_EQ(report.initializedBytes, 96U);
}

TEST_F(InitcheckMemoryTest, ReverseAffineWriteKeepsStrideGapsUninitialized)
{
    Launch();
    auto write = Range(1, kA, kWrite, 128);
    write.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    write.layout.ndAffine = {};
    write.layout.ndAffine.rank = 1;
    write.layout.ndAffine.elementBytes = 32;
    write.layout.ndAffine.dims[0] = 3;
    write.layout.ndAffine.strides[0] = -64;
    ASSERT_TRUE(Memory(write));
    ASSERT_TRUE(Memory(Range(2, kA, kRead, 0, 160)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.firstUninitAddress, 32U);
    EXPECT_EQ(report.uninitBytes, 64U);
    EXPECT_EQ(report.initializedBytes, 96U);
}

TEST_F(InitcheckMemoryTest, ContextSeparatesDeviceLaunchBlockAndBlockType)
{
    Launch();
    Launch(12);
    ASSERT_TRUE(Memory(Range(1, kA, kWrite)));
    for (unsigned variation = 0; variation < 4; ++variation) {
        auto read = Range(2, kA, kRead);
        if (variation == 0)
            read.header.deviceId = 3;
        if (variation == 1)
            read.header.launchId = 12;
        if (variation == 2)
            read.header.blockId = 1;
        if (variation == 3)
            read.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE;
        ASSERT_TRUE(Memory(read));
    }
    Sync();
    EXPECT_EQ(reports.size(), 4U);
}

TEST_F(InitcheckMemoryTest, MatchingFlagMakesMte1WritesVisibleToMatrix)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 512, kMte1)));
    Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET);
    Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    ASSERT_TRUE(Memory(Range(4, kA, kRead)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, UnsynchronizedForeignWriteMarksCoverageIncomplete)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 512, kMte1)));
    ASSERT_TRUE(Memory(Range(2, kA, kRead)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
    EXPECT_FALSE(checker.HasErrors());
}

TEST_F(InitcheckMemoryTest, LaterForeignWriteMayRaceEarlierRead)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    ASSERT_TRUE(Memory(Range(2, kA, kWrite, 0, 512, kMte1)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, DisjointFutureForeignWriteDoesNotHideMissingRead)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    ASSERT_TRUE(Memory(Range(2, kA, kWrite, 512, 512, kMte1)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, ReadOrderedBeforeForeignWriteStillReportsUninitialized)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    for (uint32_t action : {ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_SYNC_ACTION_WAIT}) {
        AclsanDeviceSyncData flag{};
        flag.header = Range(action + 1, kA, kRead).header;
        flag.header.size = sizeof(flag);
        flag.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
        flag.action = action;
        flag.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
        flag.srcPipe = kMatrix;
        flag.dstPipe = kMte1;
        EXPECT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &flag, reports));
    }
    ASSERT_TRUE(Memory(Range(4, kA, kWrite, 0, 512, kMte1)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, InexactFutureWritePreventsEarlierFalseReadDiagnostic)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    auto write = Range(2, kA, kWrite, 0, 512, kMte1);
    write.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    ASSERT_TRUE(Memory(write));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, FlagSnapshotExcludesWritesAfterSet)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 512, kMte1)));
    Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET);
    ASSERT_TRUE(Memory(Range(3, kB, kWrite, 0, 512, kMte1)));
    Flag(4, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    ASSERT_TRUE(Memory(Range(5, kA, kRead)));
    ASSERT_TRUE(Memory(Range(6, kB, kRead)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, ReusedFlagCapturesNewGeneration)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 512, kMte1)));
    Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET);
    Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    ASSERT_TRUE(Memory(Range(4, kB, kWrite, 0, 512, kMte1)));
    Flag(5, ACLSAN_DEVICE_SYNC_ACTION_SET);
    Flag(6, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    ASSERT_TRUE(Memory(Range(7, kA, kRead)));
    ASSERT_TRUE(Memory(Range(8, kB, kRead)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, FailedSyncPreservesPendingOperations)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    Sync(nullptr, 1);
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
    Sync();
    ASSERT_EQ(reports.size(), 1U);
}

TEST_F(InitcheckMemoryTest, SuccessfulSyncOnlyFinalizesItsStream)
{
    auto* first = reinterpret_cast<void*>(0x1000);
    auto* second = reinterpret_cast<void*>(0x2000);
    Launch(11, first);
    Launch(12, second);
    ASSERT_TRUE(Memory(Range(1, kA, kRead)));
    auto read = Range(1, kB, kRead);
    read.header.launchId = 12;
    ASSERT_TRUE(Memory(read));
    Sync(first);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).common.exec.launchId, 11U);
    EXPECT_FALSE(checker.AnalysisComplete());
    Sync(second);
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.back()).common.exec.launchId, 12U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, InexactWriteDoesNotProduceFalseReadDiagnostic)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    Memory(write);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, OutOfBoundsWriteCannotInitializeValidBytes)
{
    Launch();
    Memory(Range(1, kA, kWrite, 65520, 32));
    Memory(Range(2, kA, kRead, 65520, 16));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, MissingInstructionEndpointCannotCommitWrite)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.accessCount = 2;
    Memory(write);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, UnmatchedWaitCannotProveReadOrdering)
{
    Launch();
    Flag(1, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, L0LoadPreservesExistingL1SourceCheck)
{
    Launch();
    auto source = Range(1, ACLSAN_DEVICE_MEMORY_SPACE_L1, kRead, 0, 512, kMte1);
    source.instructionId = 142;
    source.accessCount = 2;
    auto target = Range(1, kA, kWrite, 0, 512, kMte1);
    target.accessCount = 2;
    target.accessIndex = 1;
    ASSERT_TRUE(Memory(target));
    ASSERT_TRUE(Memory(source));
    Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET);
    Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    ASSERT_TRUE(Memory(Range(4, kA, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.access.memorySpace, NpuCheckReportMemorySpace::L1);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, MmadPreservesExistingL0CAccumulatorCheck)
{
    Launch();
    Memory(Range(1, kA, kWrite));
    Memory(Range(2, kB, kWrite));
    auto a = Range(3, kA, kRead);
    a.accessCount = 3;
    auto b = a;
    b.memorySpace = kB;
    b.accessIndex = 1;
    auto accumulator = a;
    accumulator.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L0C;
    accumulator.accessMode = kRead | kWrite;
    accumulator.accessIndex = 2;
    Memory(a);
    Memory(b);
    Memory(accumulator);
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.access.memorySpace, NpuCheckReportMemorySpace::L0_C);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, InvalidL1SourceCannotInitializeL0Destination)
{
    Launch();
    auto source = Range(1, ACLSAN_DEVICE_MEMORY_SPACE_L1, kRead, 512 * 1024, 512, kMte1);
    source.instructionId = 142;
    source.accessCount = 2;
    auto destination = Range(1, kA, kWrite, 0, 512, kMte1);
    destination.accessCount = 2;
    destination.accessIndex = 1;
    Memory(source);
    Memory(destination);
    NpuCheckMemcheckReport bounds{};
    bounds.common.tool = ReportTool::MEMCHECK;
    bounds.common.pattern = NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS;
    bounds.common.flags = kNpuCheckReportCommonHasExecContext;
    bounds.common.exec.deviceId = source.header.deviceId;
    bounds.common.exec.launchId = source.header.launchId;
    bounds.common.exec.blockType = source.header.blockType;
    bounds.common.exec.blockId = source.header.blockId;
    bounds.common.exec.phyCoreId = source.header.phyCoreId;
    bounds.common.exec.instrExecId = source.header.instrExecId;
    bounds.common.exec.pipeId = source.header.pipeline;
    reports.emplace_back(bounds);
    Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET);
    Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    Memory(Range(4, kA, kRead));
    Sync();
    ASSERT_EQ(reports.size(), 2U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.back());
    EXPECT_EQ(report.access.memorySpace, NpuCheckReportMemorySpace::L0_A);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, BoundsReportCannotHideIncompleteL0Write)
{
    for (bool missingEndpoint : {false, true}) {
        checker = Initcheck{};
        reports.clear();
        Launch();
        auto write = Range(1, kA, kWrite);
        if (missingEndpoint)
            write.accessCount = 2;
        else
            write.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED | ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED;
        Memory(write);
        NpuCheckMemcheckReport bounds{};
        bounds.common.tool = ReportTool::MEMCHECK;
        bounds.common.pattern = NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS;
        bounds.common.flags = kNpuCheckReportCommonHasExecContext;
        bounds.common.exec.deviceId = write.header.deviceId;
        bounds.common.exec.launchId = write.header.launchId;
        bounds.common.exec.blockType = write.header.blockType;
        bounds.common.exec.blockId = write.header.blockId;
        bounds.common.exec.phyCoreId = write.header.phyCoreId;
        bounds.common.exec.instrExecId = write.header.instrExecId;
        bounds.common.exec.pipeId = write.header.pipeline;
        reports.emplace_back(bounds);
        Memory(Range(2, kA, kRead));
        Sync();
        EXPECT_EQ(reports.size(), 1U);
        EXPECT_FALSE(checker.AnalysisComplete());
        EXPECT_NE(checker.Summary().find("incomplete_memory_operations="), std::string::npos);
    }
}

TEST_F(InitcheckMemoryTest, UnsupportedL0UnitFlagCannotInitializeBytes)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.unitFlag = 1;
    Memory(write);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
    EXPECT_NE(checker.Summary().find("incomplete_memory_operations="), std::string::npos);
}

TEST_F(InitcheckMemoryTest, ScalarAccessAtLastLegalByteIsTracked)
{
    Launch();
    auto write = Range(1, kA, kWrite, 65535, 1);
    write.layoutKind = ACLSAN_MEM_LAYOUT_SCALAR;
    write.layout.scalar.bytes = 1;
    ASSERT_TRUE(Memory(write));
    ASSERT_TRUE(Memory(Range(2, kA, kRead, 65534, 2)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.firstUninitAddress, 65534U);
    EXPECT_EQ(report.uninitBytes, 1U);
    EXPECT_EQ(report.initializedBytes, 1U);
}

TEST_F(InitcheckMemoryTest, AllPipelineBarrierPublishesMte1Writes)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 512, kMte1)));
    Barrier(2, ACLSAN_DEVICE_PIPE_ALL);
    ASSERT_TRUE(Memory(Range(3, kA, kRead)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, SinglePipelineBarrierDoesNotPublishToMatrix)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 512, kMte1)));
    Barrier(2, kMte1);
    ASSERT_TRUE(Memory(Range(3, kA, kRead)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, MalformedBarrierCannotPublishWrites)
{
    Launch();
    ASSERT_TRUE(Memory(Range(1, kA, kWrite, 0, 512, kMte1)));
    AclsanDeviceBarrierData barrier{};
    barrier.header = Range(2, kA, kRead).header;
    barrier.header.size = sizeof(barrier);
    barrier.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    barrier.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    barrier.pipe = ACLSAN_DEVICE_PIPE_ALL;
    checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER, &barrier, reports);
    Memory(Range(3, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, UnknownInstructionFootprintMarksCoreIncomplete)
{
    Launch();
    auto marker = Range(1, ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN, kRead);
    marker.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    Memory(marker);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, InstructionGroupMismatchCannotCommitWrite)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.accessCount = 2;
    auto source = Range(1, ACLSAN_DEVICE_MEMORY_SPACE_L1, kRead);
    source.accessCount = 2;
    source.accessIndex = 1;
    // Endpoints from different decoded instructions cannot form one complete operation.
    ASSERT_NE(write.instructionId, source.instructionId);
    Memory(write);
    Memory(source);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, OverlappingAffineDimensionsHaveBoundedFootprintWork)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    write.layout.ndAffine = {};
    write.layout.ndAffine.rank = 4;
    write.layout.ndAffine.elementBytes = 65412;
    for (unsigned i = 0; i < 4; ++i) {
        write.layout.ndAffine.dims[i] = 32;
        write.layout.ndAffine.strides[i] = 1;
    }
    ASSERT_TRUE(Memory(write));
    ASSERT_TRUE(Memory(Range(2, kA, kRead, 0, 65536)));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, MaskedL0WriteDoesNotInventInitializedBytes)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.predicateMask0 = 1;
    Memory(write);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, MalformedMemoryEventSuppressesUnreliableDiagnostics)
{
    Launch();
    auto write = Range(1, kA, kWrite);
    write.header.size = sizeof(AclsanDeviceEventHeader);
    EXPECT_FALSE(Memory(write));
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, NonL0RegisterFailureDoesNotSuppressL0Read)
{
    Launch();
    auto source = Range(1, ACLSAN_DEVICE_MEMORY_SPACE_GM, kRead, 0, 512, ACLSAN_DEVICE_PIPE_MTE2);
    source.instructionId = static_cast<uint32_t>(aclsan::InstructionId::NdDmaOutToUbufB8);
    source.regDependencyMask0 = UINT64_C(1) << ACLSAN_DEVICE_REGISTER_NDDMA_LOOP0_STRIDE;
    ASSERT_TRUE(Memory(source));
    ASSERT_TRUE(Memory(Range(2, kA, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(
        std::get<NpuCheckInitcheckReport>(reports[1]).common.pattern,
        NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, MissingL0DestinationWithRegisterErrorRemainsIncomplete)
{
    Launch();
    auto source = Range(1, ACLSAN_DEVICE_MEMORY_SPACE_L1, kRead, 0, 512, kMte1);
    source.instructionId = static_cast<uint32_t>(aclsan::InstructionId::LoadCbufToCaB16);
    source.regDependencyMask0 = UINT64_C(1) << ACLSAN_DEVICE_REGISTER_LOOP3;
    source.accessCount = 2;
    ASSERT_TRUE(Memory(source));
    ASSERT_TRUE(Memory(Range(2, kA, kRead)));
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(
        std::get<NpuCheckInitcheckReport>(reports.front()).common.pattern,
        NpuCheckReportPattern::INITCHECK_UNINITIALIZED_REGISTER_USE);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, InterCoreSyncDoesNotRequireNotificationMatching)
{
    for (uint32_t kind :
         {ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE,
          ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK}) {
        SCOPED_TRACE(kind);
        checker = Initcheck{};
        reports.clear();
        Launch();
        AclsanDeviceSyncData data{};
        data.header = Range(1, kA, kRead).header;
        data.header.size = sizeof(data);
        data.syncKind = kind;
        data.action = ACLSAN_DEVICE_SYNC_ACTION_WAIT;
        data.scope = ACLSAN_DEVICE_SYNC_SCOPE_CORE;
        data.srcPipe = kMte1;
        data.dstPipe = kMatrix;
        EXPECT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &data, reports));
        Memory(Range(2, kB, kRead));
        Sync();
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_TRUE(checker.AnalysisComplete());
    }
}

TEST_F(InitcheckMemoryTest, AsyncIssueOrderAloneDoesNotInitializeRead)
{
    Launch();
    Memory(Range(1, kA, kWrite, 0, 512, kMte1));
    Memory(Range(2, kA, kRead, 0, 512, kMte1));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, PipelineBarrierEstablishesDmaCompletion)
{
    Launch();
    Memory(Range(1, kA, kWrite, 0, 512, kMte1));
    Barrier(2, kMte1);
    Memory(Range(3, kA, kRead, 0, 512, kMte1));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, PendingTraceAtFinalizationRemainsIncomplete)
{
    Launch();
    Memory(Range(1, kA, kRead));
    checker.Finalize(reports);
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, UncertainSourceDoesNotCauseDefinitiveDestinationErrors)
{
    Launch();
    Memory(Range(1, kA, kWrite, 0, 512, kMte1));
    auto read = Range(2, kA, kRead);
    read.accessCount = 2;
    auto write = Range(2, kB, kWrite);
    write.instructionId = read.instructionId;
    write.accessCount = 2;
    write.accessIndex = 1;
    Memory(read);
    Memory(write);
    Memory(Range(3, kB, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, InvalidSpaceCannotHideAnInitializationSource)
{
    Launch();
    Memory(Range(1, UINT32_MAX, kWrite));
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckMemoryTest, MissingLocalDestinationCannotBeHiddenByRetainedL1Source)
{
    Launch();
    auto source = Range(1, ACLSAN_DEVICE_MEMORY_SPACE_L1, kRead, 0, 512, kMte1);
    source.accessCount = 2;
    Memory(source);
    Memory(Range(2, kA, kRead));
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

} // namespace
} // namespace npucheck
