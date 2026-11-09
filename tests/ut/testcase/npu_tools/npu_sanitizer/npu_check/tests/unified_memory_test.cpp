/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "checker/checker.h"
#include "checker/memory_analysis.h"
#include "checker/initcheck.h"
#include "checker/memcheck.h"
#include "checker/racecheck.h"
#include "checker/synccheck.h"
#include "checker/racecheck_reports.h"
#include "device_instr/common/instruction_id.h"
#include <gtest/gtest.h>
#include <algorithm>

namespace npucheck {
namespace {
struct Session {
    std::shared_ptr<MemoryAnalysis> analysis = std::make_shared<MemoryAnalysis>(true);
    Memcheck bounds{analysis};
    Initcheck initialization{analysis};
    Racechecker race{race::Limits{}, analysis};
    Synccheck synchronization{analysis};
    bool syncEnabled = false;
    CheckerReportList reports;
    bool reverse = false;
    bool initEnabled = true;
    bool boundsEnabled = true;
    bool raceEnabled = true;
    template <typename T>
    void Send(AclsanCallbackDomain domain, AclsanCallbackId id, T data)
    {
        ASSERT_TRUE(analysis->BeginCallback(domain, id, &data));
        std::vector<Checker*> checkers;
        if (boundsEnabled)
            checkers.push_back(&bounds);
        if (initEnabled)
            checkers.push_back(&initialization);
        if (raceEnabled)
            checkers.push_back(&race);
        if (syncEnabled)
            checkers.push_back(&synchronization);
        if (reverse)
            std::reverse(checkers.begin(), checkers.end());
        for (Checker* checker : checkers)
            if (checker->IsSubscribed(domain, id))
                ASSERT_TRUE(checker->OnCallback(domain, id, &data, reports));
        analysis->EndCallback();
    }
    void Launch(uint64_t id, void* stream = nullptr)
    {
        AclsanLaunchData data{};
        data.common = {ACLSAN_API_VERSION, sizeof(data), nullptr, 0, 0};
        data.launchId = id;
        data.stream = stream;
        Send(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, data);
    }
    void Sync(void* stream = nullptr, int result = 0)
    {
        AclsanSynchronizeData data{};
        data.common = {ACLSAN_API_VERSION, sizeof(data), nullptr, 0, 0};
        data.stream = stream;
        data.common.result = result;
        Send(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, data);
    }
    void Allocate(uint64_t address = 0x10000, uint64_t resourceId = 1, uint32_t device = 0)
    {
        AclsanResourceData data{};
        data.common = {ACLSAN_API_VERSION, sizeof(data), nullptr, 0, 0};
        data.ptr = reinterpret_cast<void*>(address);
        data.bytes = 4096;
        data.resourceId = resourceId;
        data.deviceId = device;
        data.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
        Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, data);
    }
    void Transfer(AclsanMemoryTransferKind kind, uint64_t source, uint64_t destination, uint64_t bytes, int result = 0)
    {
        AclsanMemoryTransferData data{};
        data.common = {ACLSAN_API_VERSION, sizeof(data), "aclrtMemcpy", result, 0};
        data.source = reinterpret_cast<const void*>(source);
        data.destination = reinterpret_cast<void*>(destination);
        data.bytes = bytes;
        data.kind = kind;
        Send(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_TRANSFER_END, data);
    }
    void Memory(
        uint64_t launch, uint64_t exec, uint32_t pipe, uint32_t space, uint32_t mode, uint64_t address, uint64_t bytes,
        uint32_t flag = 0, uint64_t registerMask = 0)
    {
        AclsanDeviceMemoryAccessData data{};
        data.header.version = ACLSAN_API_VERSION;
        data.header.size = sizeof(data);
        data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        data.header.launchId = launch;
        data.header.instrExecId = exec;
        data.header.executor.blockType = space == ACLSAN_DEVICE_MEMORY_SPACE_UB ?
                                             ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR :
                                             ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        data.header.pipeline = pipe;
        data.accessCount = 1;
        data.instructionId = 1;
        data.memorySpace = space;
        data.memoryOwner = data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ? AclsanCoreInfo{} : data.header.executor;
        data.accessMode = mode;
        data.address = address;
        data.dataBits = 32;
        data.unitFlag = flag;
        data.regDependencyMask0 = registerMask;
        data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        data.layout.range.bytes = bytes;
        Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, data);
    }
};

TEST(UnifiedMemoryTest, AllocationIsAppliedOnceAndPagesAreLazy)
{
    Session s;
    s.Allocate();
    EXPECT_EQ(s.bounds.Stats().allocations, 1U);
    EXPECT_EQ(s.analysis->state->allocations.size(), 1U);
    EXPECT_EQ(s.analysis->state->initialized.Find({0, 0}), nullptr);
    EXPECT_EQ(s.analysis->state->Allocations().Classify(0, 0x10000, 4096).status, RangeStatus::VALID);
}
TEST(UnifiedMemoryTest, UnitFlagReusesOneGraphForBothCheckers)
{
    Session s;
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 512, 3);
    s.Memory(
        1, 2, ACLSAN_DEVICE_PIPE_FIXPIPE, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 512, 3);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_TRUE(s.race.AnalysisComplete()) << s.race.Summary();
    EXPECT_EQ(s.analysis->GraphBuilds(), 1U);
    EXPECT_TRUE(s.analysis->Closing().empty());
}
TEST(UnifiedMemoryTest, UnitFlagDoesNotInitializeUnwrittenBytesInItsHardwareUnit)
{
    Session s;
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 256, 3);
    s.Memory(
        1, 2, ACLSAN_DEVICE_PIPE_FIXPIPE, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 512, 3);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(s.reports.front());
    EXPECT_EQ(report.uninitBytes, 256U);
    EXPECT_EQ(report.firstUninitAddress, 256U);
    EXPECT_TRUE(s.initialization.AnalysisComplete());
}
TEST(UnifiedMemoryTest, HostSubmissionDoesNotOrderDifferentStreamsButSyncDoes)
{
    Session s;
    auto* a = reinterpret_cast<void*>(1);
    auto* b = reinterpret_cast<void*>(2);
    s.Launch(1, a);
    s.Launch(2, b);
    EXPECT_EQ(s.analysis->CompareLaunches(1, 2), race::HappensBeforeGraph::Relation::Unordered);
    s.Launch(3, a);
    EXPECT_EQ(s.analysis->CompareLaunches(1, 3), race::HappensBeforeGraph::Relation::Before);
    s.Sync(a);
    s.Launch(4, b);
    EXPECT_EQ(s.analysis->CompareLaunches(3, 4), race::HappensBeforeGraph::Relation::Before);
    EXPECT_EQ(s.analysis->CompareLaunches(2, 4), race::HappensBeforeGraph::Relation::Before);
}
TEST(UnifiedMemoryTest, HostWriteBeforeLaunchEstablishesInitialization)
{
    Session s;
    s.Allocate();
    AclsanMemoryWriteData write{};
    write.common = {ACLSAN_API_VERSION, sizeof(write), nullptr, 0, 0};
    write.destination = reinterpret_cast<void*>(0x10000);
    write.bytes = 512;
    s.Send(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, write);
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 512);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_EQ(s.analysis->GraphBuilds(), 1U);
}
TEST(UnifiedMemoryTest, KernelWriteSurvivesSyncAndInitializesLaterLaunch)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 512);
    s.Sync();
    s.Launch(2);
    s.Memory(
        2, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 512);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_EQ(s.analysis->GraphBuilds(), 2U);
}
TEST(UnifiedMemoryTest, FailedOrDifferentStreamSyncCannotCreateCompletionOrder)
{
    Session s;
    auto* a = reinterpret_cast<void*>(1);
    auto* b = reinterpret_cast<void*>(2);
    s.Launch(1, a);
    s.Sync(a, 1);
    s.Launch(2, b);
    EXPECT_EQ(s.analysis->CompareLaunches(1, 2), race::HappensBeforeGraph::Relation::Unordered);
    s.Sync(b);
    s.Launch(3, b);
    EXPECT_EQ(s.analysis->CompareLaunches(1, 3), race::HappensBeforeGraph::Relation::Unordered);
}
TEST(UnifiedMemoryTest, SerializationOfCallbacksDoesNotOrderDifferentHostThreads)
{
    Session s;
    auto* a = reinterpret_cast<void*>(1);
    auto* b = reinterpret_cast<void*>(2);
    s.Launch(1, a);
    s.Sync(a);
    std::thread independent([&] { s.Launch(2, b); });
    independent.join();
    EXPECT_EQ(s.analysis->CompareLaunches(1, 2), race::HappensBeforeGraph::Relation::Unordered);
}
TEST(UnifiedMemoryTest, LaterHostWriteCannotInitializeEarlierKernelRead)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    AclsanMemoryWriteData write{};
    write.common = {ACLSAN_API_VERSION, sizeof(write), nullptr, 0, 0};
    write.destination = reinterpret_cast<void*>(0x10000);
    write.bytes = 512;
    s.Send(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, write);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 512);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<NpuCheckRacecheckReport>(s.reports.front()));
    EXPECT_FALSE(s.initialization.AnalysisComplete());
    EXPECT_NE(s.initialization.Summary().find("unordered_initialization"), std::string::npos);
}
TEST(UnifiedMemoryTest, ReadWriteInstructionCannotInitializeItsOwnRead)
{
    Session s;
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C,
        ACLSAN_DEVICE_MEMORY_ACCESS_READ | ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 512);
    s.Memory(1, 2, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 512);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(s.reports.front()).common.exec.instrExecId, 1U);
    EXPECT_TRUE(s.initialization.AnalysisComplete());
}
TEST(UnifiedMemoryTest, FreeAndReallocateClearsInitializationProvenance)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 512);
    s.Sync();
    AclsanResourceData free{};
    free.common = {ACLSAN_API_VERSION, sizeof(free), nullptr, 0, 0};
    free.ptr = reinterpret_cast<void*>(0x10000);
    free.resourceId = 1;
    free.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    s.Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE, free);
    s.Allocate();
    s.Launch(2);
    s.Memory(
        2, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 512);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(s.reports.front()).uninitBytes, 512U);
    EXPECT_TRUE(s.initialization.AnalysisComplete());
}
TEST(UnifiedMemoryTest, FpcHandshakeResetDoesNotEraseInitializedMemory)
{
    Session s;
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 512);
    AclsanDeviceRegisterStateData reset{};
    reset.header.version = ACLSAN_API_VERSION;
    reset.header.size = sizeof(reset);
    reset.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    reset.header.launchId = 1;
    reset.header.instrExecId = 2;
    reset.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    reset.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    reset.value = UINT64_C(1) << 63U;
    reset.regId = ACLSAN_DEVICE_REGISTER_FPC;
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE, reset);
    s.Memory(1, 3, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 512);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
}

TEST(UnifiedMemoryTest, TwoLaunchesOnOneStreamShareInitializationAtOneSync)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    s.Launch(2);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 512);
    s.Memory(
        2, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 512);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_EQ(s.analysis->GraphBuilds(), 2U);
}
TEST(UnifiedMemoryTest, SuccessfulSyncMakesWriteVisibleToAnotherStream)
{
    Session s;
    s.Allocate();
    auto* a = reinterpret_cast<void*>(1);
    auto* b = reinterpret_cast<void*>(2);
    s.Launch(1, a);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 512);
    s.Sync(a);
    s.Launch(2, b);
    s.Memory(
        2, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 512);
    s.Sync(b);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
}

TEST(UnifiedMemoryTest, CheckerSelectionAndOrderDoNotChangeBoundsOrInitialization)
{
    for (bool reverse : {false, true}) {
        Session s;
        s.reverse = reverse;
        s.boundsEnabled = false;
        s.raceEnabled = false;
        s.Allocate();
        s.Launch(1);
        s.Memory(
            1, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000,
            128);
        s.Sync();
        ASSERT_EQ(s.reports.size(), 1U);
        EXPECT_EQ(std::get<NpuCheckInitcheckReport>(s.reports.front()).uninitBytes, 128U);
        EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    }
}
TEST(UnifiedMemoryTest, UbInitializationIsIgnored)
{
    Session s;
    s.Launch(1);
    s.Memory(1, 1, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_MEMORY_SPACE_UB, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 128);
    s.Memory(1, 2, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_MEMORY_SPACE_UB, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 256);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
}
TEST(UnifiedMemoryTest, ReversedCheckersShareKernelWriteWithoutInitcheckEnabled)
{
    Session s;
    s.reverse = true;
    s.initEnabled = false;
    s.Allocate();
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 128);
    s.Sync();
    s.initEnabled = true;
    s.Launch(2);
    s.Memory(
        2, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 128);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
}
TEST(UnifiedMemoryTest, UnorderedCrossLaunchGmWritesRemainRaceCandidates)
{
    Session s;
    s.initEnabled = false;
    s.Allocate();
    auto* a = reinterpret_cast<void*>(1);
    auto* b = reinterpret_cast<void*>(2);
    s.Launch(1, a);
    s.Launch(2, b);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 128);
    s.Memory(
        2, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10040, 128);
    s.Sync(a);
    s.Sync(b);
    ASSERT_EQ(s.reports.size(), 1U);
    const auto& report = std::get<NpuCheckRacecheckReport>(s.reports.front());
    EXPECT_EQ(report.kind, ConflictKind::WAW);
    EXPECT_EQ(report.previous.header.launchId, 1U);
    EXPECT_EQ(report.current.header.launchId, 2U);
    EXPECT_EQ(report.checkRange.begin, 0x10040U);
    EXPECT_EQ(report.checkRange.end, 0x10080U);
    EXPECT_TRUE(s.race.AnalysisComplete()) << s.race.Summary();
}
TEST(UnifiedMemoryTest, OrderedLaunchesDoNotSpendTheConflictPairAllowance)
{
    Session s;
    s.boundsEnabled = s.initEnabled = s.raceEnabled = false;
    s.Allocate();
    race::Limits limits;
    limits.comparisons = 1; // Two writes per launch would form four redundant pairs.
    for (uint64_t launch = 1; launch <= 2; ++launch) {
        s.Launch(launch);
        for (uint64_t i = 0; i < 2; ++i)
            s.Memory(
                launch, i + 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM,
                ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000 + i * 128, 128);
        AclsanSynchronizeData sync{};
        sync.common = {ACLSAN_API_VERSION, sizeof(sync), nullptr, 0, 0};
        ASSERT_TRUE(
            s.analysis->BeginCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync));
        std::set<std::string> reasons;
        EXPECT_TRUE(s.analysis->CrossLaunchConflicts(launch, limits, reasons).empty());
        EXPECT_TRUE(reasons.empty());
        EXPECT_TRUE(s.analysis->state->globalHistoryComplete);
        EXPECT_EQ(s.analysis->state->globalAccesses.size(), launch * 2);
        s.analysis->EndCallback();
    }
}
TEST(UnifiedMemoryTest, StridedOperandKeepsItsKnownAllocationAcrossUnmappedFragments)
{
    Session s;
    s.raceEnabled = false;
    s.Allocate();
    s.Launch(1);
    AclsanDeviceMemoryAccessData access{};
    access.header.version = ACLSAN_API_VERSION;
    access.header.size = sizeof(access);
    access.header.launchId = 1;
    access.header.instrExecId = 1;
    access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    access.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    access.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    access.accessCount = 1;
    access.instructionId = 1;
    access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    access.memoryOwner =
        access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ? AclsanCoreInfo{} : access.header.executor;
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    access.address = 0x10000;
    access.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    access.layout.ndAffine.rank = 1;
    access.layout.ndAffine.elementBytes = 4;
    access.layout.ndAffine.dims[0] = 2;
    access.layout.ndAffine.strides[0] = 8192;
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, access);
    s.Sync();
    EXPECT_EQ(s.bounds.Stats().errors, 1U);
    EXPECT_EQ(s.initialization.Stats().errors, 0U);
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_TRUE(s.bounds.AnalysisComplete());
}
TEST(UnifiedMemoryTest, UnknownOperandOriginCannotBePromotedToKnownBounds)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    AclsanDeviceMemoryAccessData access{};
    access.header.deviceId = 0;
    access.header.launchId = 1;
    access.address = 0x90000;
    EXPECT_EQ(s.analysis->ClassifyAccessRange(access, 0x90000, 4).status, RangeStatus::UNKNOWN);
    EXPECT_EQ(s.analysis->ClassifyAccessRange(access, 0x10000, 4).status, RangeStatus::UNKNOWN);
}
TEST(UnifiedMemoryTest, StridedOverflowDoesNotBelongToAnUnrelatedFreedAllocation)
{
    Session s;
    s.Allocate();
    s.Allocate(0x20000, 2);
    AclsanResourceData free{};
    free.common = {ACLSAN_API_VERSION, sizeof(free), nullptr, 0, 0};
    free.ptr = reinterpret_cast<void*>(0x20000);
    free.resourceId = 2;
    free.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    s.Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE, free);
    s.Launch(1);
    AclsanDeviceMemoryAccessData access{};
    access.header.launchId = 1;
    access.address = 0x10000;
    const auto result = s.analysis->ClassifyAccessRange(access, 0x20000, 4);
    EXPECT_EQ(result.status, RangeStatus::OUT_OF_BOUNDS);
    ASSERT_TRUE(result.allocation);
    EXPECT_EQ(result.allocation->base, 0x10000U);
}
TEST(UnifiedMemoryTest, UnorderedFreeCannotBeMisreportedAsDefiniteUseAfterFree)
{
    Session s;
    s.initEnabled = false;
    s.raceEnabled = false;
    s.Allocate();
    s.Launch(1);
    AclsanResourceData free{};
    free.common = {ACLSAN_API_VERSION, sizeof(free), nullptr, 0, 0};
    free.ptr = reinterpret_cast<void*>(0x10000);
    free.resourceId = 1;
    free.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    s.Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE, free);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 128);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.bounds.AnalysisComplete());
}

TEST(UnifiedMemoryTest, EveryAtomicModeReadsOldBytesAndInitializesItsResult)
{
    for (uint32_t mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        Session s;
        s.Launch(1);
        s.Memory(1, 1, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, mode, 0, 128);
        s.Memory(
            1, 2, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 128);
        s.Sync();
        ASSERT_EQ(s.reports.size(), 1U);
        EXPECT_EQ(std::get<NpuCheckInitcheckReport>(s.reports.front()).common.exec.instrExecId, 1U);
        EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    }
}

TEST(UnifiedMemoryTest, SyncCheckerReadsTheSameLedgerWithoutApplyingEventsTwice)
{
    Session s;
    s.syncEnabled = true;
    s.Launch(1);
    AclsanDeviceSyncData flag{};
    flag.header.version = ACLSAN_API_VERSION;
    flag.header.size = sizeof(flag);
    flag.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    flag.header.launchId = 1;
    flag.header.instrExecId = 1;
    flag.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    flag.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    flag.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
    flag.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
    flag.action = ACLSAN_DEVICE_SYNC_ACTION_SET;
    flag.srcPipe = ACLSAN_DEVICE_PIPE_MTE2;
    flag.dstPipe = ACLSAN_DEVICE_PIPE_VECTOR;
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, flag);
    flag.header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    flag.header.instrExecId = 2;
    flag.action = ACLSAN_DEVICE_SYNC_ACTION_WAIT;
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, flag);
    s.Sync();
    EXPECT_EQ(s.synchronization.Stats().syncEvents, 2U);
    EXPECT_EQ(s.synchronization.Stats().matchedPairs, 1U);
    EXPECT_EQ(s.synchronization.Stats().synchronizationEvents, 1U);
    EXPECT_TRUE(s.reports.empty());
}
TEST(UnifiedMemoryTest, MissingRegisterPlaceholderIsNotAnUnknownMemoryFootprint)
{
    Session s;
    s.raceEnabled = false;
    s.Launch(1);
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    data.header.launchId = 1;
    data.header.instrExecId = 1;
    data.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    data.instructionId = static_cast<uint32_t>(aclsan::InstructionId::NdDmaOutToUbufB8);
    data.regDependencyMask0 = UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CHANNEL_PARA;
    data.accessCount = 1;
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, data);
    EXPECT_EQ(s.analysis->Get(1).dependencyOnly.size(), 1U);
    EXPECT_TRUE(s.analysis->Get(1).memory.empty());
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    EXPECT_EQ(
        std::get<NpuCheckInitcheckReport>(s.reports.front()).registerId,
        static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_CHANNEL_PARA));
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
}
TEST(UnifiedMemoryTest, LifetimeHistoryDistinguishesPastValidAccessFromFutureUseAfterFree)
{
    Session s;
    s.initEnabled = false;
    s.Allocate();
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 128);
    s.Sync();
    AclsanResourceData free{};
    free.common = {ACLSAN_API_VERSION, sizeof(free), nullptr, 0, 0};
    free.ptr = reinterpret_cast<void*>(0x10000);
    free.resourceId = 1;
    free.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    s.Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE, free);
    EXPECT_EQ(s.analysis->Classify(1, {0, 0x10000, 128}).status, RangeStatus::VALID);
    s.Launch(2);
    s.Memory(
        2, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 128);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    const auto& report = std::get<NpuCheckMemcheckReport>(s.reports.front());
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE);
    EXPECT_NE(report.allocation.freeSerialNo, 0U);
}
TEST(UnifiedMemoryTest, ReverseCheckerOrderPreservesPartialUnitFlagInitialization)
{
    Session s;
    s.reverse = true;
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 256, 3);
    s.Memory(
        1, 2, ACLSAN_DEVICE_PIPE_FIXPIPE, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 512, 3);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(s.reports.front()).uninitBytes, 256U);
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_TRUE(s.race.AnalysisComplete()) << s.race.Summary();
    EXPECT_EQ(s.analysis->GraphBuilds(), 1U);
}

TEST(UnifiedMemoryTest, TransferAndLegacyH2dCallbacksDoNotDuplicateFacts)
{
    Session s;
    s.Allocate();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE, 0x8000, 0x10000, 512);
    AclsanMemoryWriteData legacy{};
    legacy.common = {ACLSAN_API_VERSION, sizeof(legacy), "aclrtMemcpy", 0, 0};
    legacy.destination = reinterpret_cast<void*>(0x10000);
    legacy.bytes = 512;
    s.Send(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, legacy);
    ASSERT_EQ(s.analysis->state->hostAccesses.size(), 1U);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x8000, 512);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_EQ(s.bounds.Stats().hostOperations, 2U);
    EXPECT_TRUE(s.race.AnalysisComplete()) << s.race.Summary();
}
TEST(UnifiedMemoryTest, D2hChecksOnlyTheInitializedSourceBytes)
{
    Session s;
    s.Allocate();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE, 0x8000, 0x10000, 256);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x8000, 512);
    ASSERT_EQ(s.reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(s.reports.front());
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_API_READ_UNINITIALIZED);
    EXPECT_EQ(report.uninitBytes, 256U);
    EXPECT_EQ(report.firstUninitAddress, 0x10100U);
    EXPECT_EQ(report.common.flags & kNpuCheckReportCommonHasExecContext, 0U);
}
TEST(UnifiedMemoryTest, D2dChecksBothResourcesAndPublishesItsActualWrite)
{
    Session s;
    s.Allocate();
    s.Allocate(0x20000, 2);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_DEVICE, 0x10000, 0x20000, 512);
    ASSERT_EQ(s.reports.size(), 1U); // The source has not been initialized.
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(s.reports.front()).uninitBytes, 512U);
    s.reports.clear();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x20000, 0x8000, 512);
    EXPECT_TRUE(s.reports.empty()); // Initialization checking does not propagate value taint.
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_DEVICE, 0x10ff0, 0x20ff0, 32);
    ASSERT_EQ(s.reports.size(), 2U);
    for (const auto& report : s.reports)
        EXPECT_EQ(
            std::get<NpuCheckMemcheckReport>(report).common.pattern,
            NpuCheckReportPattern::MEMCHECK_HOST_INVALID_ACCESS);
}
TEST(UnifiedMemoryTest, StreamCompletionMakesKernelWriteVisibleToD2h)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 512);
    s.Sync();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x8000, 512);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_TRUE(s.race.AnalysisComplete()) << s.race.Summary();
}
TEST(UnifiedMemoryTest, D2hBeforeCompletionIsUnknownForInitializationAndARaceCandidate)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x8000, 512);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.initialization.AnalysisComplete());
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 512);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    const auto& report = std::get<NpuCheckRacecheckReport>(s.reports.front());
    ASSERT_TRUE(report.previousHost);
    EXPECT_EQ(report.previousHost->apiName, "aclrtMemcpy");
    EXPECT_EQ(report.kind, ConflictKind::WAR);
    ReportRecord rendered;
    EXPECT_EQ(NormalizeRacecheckReport(report, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.fields.at("previousActor").find("Host API aclrtMemcpy"), std::string::npos);
    auto invalid = report;
    invalid.previousHost->bytes = 0;
    EXPECT_EQ(NormalizeRacecheckReport(invalid, &rendered), ReportRenderStatus::INVALID_ARGUMENT);
}
TEST(UnifiedMemoryTest, FailedTransferCannotPublishInitializationOrHistory)
{
    Session s;
    s.Allocate();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE, 0x8000, 0x10000, 512, 1);
    EXPECT_TRUE(s.analysis->state->hostAccesses.empty());
    EXPECT_FALSE(s.analysis->Complete());
    EXPECT_FALSE(s.initialization.AnalysisComplete());
    EXPECT_FALSE(s.bounds.AnalysisComplete());
}
TEST(UnifiedMemoryTest, FailedSynchronizationFlushesDiagnosticsWithoutCompletionEdge)
{
    Session s;
    s.syncEnabled = true;
    auto* a = reinterpret_cast<void*>(1);
    auto* b = reinterpret_cast<void*>(2);
    s.Launch(1, a);
    s.Sync(a, 1);
    s.Launch(2, b);
    EXPECT_EQ(s.synchronization.Stats().synchronizationEvents, 1U);
    EXPECT_EQ(s.analysis->CompareLaunches(1, 2), race::HappensBeforeGraph::Relation::Unordered);
    EXPECT_FALSE(s.bounds.AnalysisComplete());
}
TEST(UnifiedMemoryTest, ExactFootprintDoesNotRequireInitcheckOnlyRegisterProbesForRacecheck)
{
    Session s;
    s.initEnabled = false;
    s.Allocate();
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000, 32, 0,
        UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CHANNEL_PARA);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.race.AnalysisComplete()) << s.race.Summary();
    EXPECT_EQ(s.analysis->GraphBuilds(), 1U);
}
TEST(UnifiedMemoryTest, HostPointerOnAnotherDeviceIsUnknownRatherThanOutOfBounds)
{
    Session s;
    s.Allocate(0x10000, 1, 2);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x8000, 32);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.analysis->Complete());
    EXPECT_FALSE(s.bounds.AnalysisComplete());
}
TEST(UnifiedMemoryTest, UntrackedHostOperandCannotCreateFalseBoundsOrInitializationProof)
{
    Session s;
    s.Allocate();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE, 0x10000, 0x8000, 32);
    // Runtime could infer D2H from these addresses even when the argument says H2D.
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.analysis->Complete());
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x8000, 32);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.initialization.AnalysisComplete());
}
TEST(UnifiedMemoryTest, InitcheckAloneCannotCertifyReadWithUnorderedFree)
{
    Session s;
    s.boundsEnabled = false;
    s.raceEnabled = false;
    s.Allocate();
    s.Launch(1);
    AclsanResourceData free{};
    free.common = {ACLSAN_API_VERSION, sizeof(free), nullptr, 0, 0};
    free.ptr = reinterpret_cast<void*>(0x10000);
    free.resourceId = 1;
    free.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    s.Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE, free);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 128);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_NE(s.initialization.Summary().find("ambiguous_gm_lifetime=1"), std::string::npos);
}
TEST(UnifiedMemoryTest, RepeatedResourceQueriesDoNotDestroyRecordedHistory)
{
    Session s;
    s.Allocate(0x10000, 1);
    s.Allocate(0x20000, 2);
    s.Allocate(0x30000, 3);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE, 0x90000, 0x10000, 0);
    RangeResult result;
    for (size_t i = 0; i < 3333335; ++i) {
        result = s.analysis->Classify(0, {0, 0x10000, 4});
        if (result.status != RangeStatus::VALID) {
            break;
        }
    }
    EXPECT_EQ(result.status, RangeStatus::VALID);
    EXPECT_GT(s.analysis->ResourceRecordsScanned(), 10000000U);
    EXPECT_TRUE(s.analysis->Complete());
    EXPECT_EQ(s.analysis->Classify(0, {0, 0x30000, 4}).status, RangeStatus::VALID);
}

TEST(UnifiedMemoryTest, InitializationQueriesVisitOnlyOverlappingGenerationSources)
{
    Session s;
    s.boundsEnabled = s.raceEnabled = false;
    s.Allocate();
    s.Allocate(0x20000, 2);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE, 0x90000, 0x10000, 512);
    for (size_t i = 0; i < 9000; ++i) {
        s.analysis->RecordInitialization(0, 0, 0x10800, 4); // Same generation, disjoint bytes.
        s.analysis->RecordInitialization(0, 0, 0x20000, 4); // Different generation.
    }
    const auto scans = s.analysis->ResourceRecordsScanned();
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 512);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
    EXPECT_LE(s.analysis->InitializationCandidates(), 2U);
    EXPECT_LT(s.analysis->ResourceRecordsScanned() - scans, 30U);
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x90000, 512);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.analysis->Complete());
}

TEST(UnifiedMemoryTest, InitializationEvidenceCapacityCannotBeResetByAnotherLaunch)
{
    Session s;
    s.Allocate();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE, 0x90000, 0x10000, 0);
    for (size_t i = 0; i <= 100000; ++i) {
        s.analysis->RecordInitialization(0, 0, 0x10000, 4);
    }
    ASSERT_FALSE(s.analysis->Complete());
    s.Launch(1);
    s.Sync();
    EXPECT_FALSE(s.analysis->Complete());
}

AclsanDeviceMemoryAccessData LocalCopyOperand(uint32_t index, uint32_t space, uint32_t mode, uint64_t address)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.header.launchId = 1;
    data.header.instrExecId = 1;
    data.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    data.accessCount = 2;
    data.accessIndex = index;
    data.instructionId = 1;
    data.memorySpace = space;
    data.memoryOwner = data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ? AclsanCoreInfo{} : data.header.executor;
    data.accessMode = mode;
    data.address = address;
    data.dataBits = 32;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = 128;
    return data;
}

TEST(UnifiedMemoryTest, IgnoredUbOperandKeepsGmFragmentsComplete)
{
    Session s;
    s.Allocate();
    s.Launch(1);
    auto source = LocalCopyOperand(0, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000);
    auto destination = LocalCopyOperand(1, ACLSAN_DEVICE_MEMORY_SPACE_UB, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0);
    source.header.executor.blockType = destination.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, source);
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, destination);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(s.reports.front());
    EXPECT_EQ(report.firstUninitAddress, 0x10000U);
    EXPECT_EQ(report.uninitBytes, 128U);
    EXPECT_TRUE(s.analysis->Complete());
    EXPECT_TRUE(s.initialization.AnalysisComplete()) << s.initialization.Summary();
}

TEST(UnifiedMemoryTest, KnownInvalidSourceDoesNotEraseExactValidDestinationWrite)
{
    Session s;
    s.boundsEnabled = s.raceEnabled = false;
    s.Allocate();
    s.Launch(1);
    auto source = LocalCopyOperand(
        0, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ,
        InitStateTableIndex::LocalCapacity(ACLSAN_DEVICE_MEMORY_SPACE_L1) - 64);
    auto destination = LocalCopyOperand(1, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000);
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, source);
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, destination);
    s.Sync();
    s.Transfer(ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST, 0x10000, 0x90000, 128);
    EXPECT_TRUE(s.reports.empty());
    EXPECT_TRUE(s.analysis->Complete());
    EXPECT_FALSE(s.initialization.AnalysisComplete());
    EXPECT_NE(s.initialization.Summary().find("degraded_instructions=1"), std::string::npos);
}

TEST(UnifiedMemoryTest, FailedStorageRollsBackEarlierReadReportsOfSameInstruction)
{
    Session s;
    s.boundsEnabled = s.raceEnabled = false;
    s.Allocate();
    s.Launch(1);
    auto gm = LocalCopyOperand(0, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000);
    auto l1 = LocalCopyOperand(
        1, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ,
        InitStateTableIndex::LocalCapacity(ACLSAN_DEVICE_MEMORY_SPACE_L1) - 64);
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, gm);
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, l1);
    s.Memory(
        1, 2, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10200, 128);
    s.Sync();
    ASSERT_EQ(s.reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(s.reports.front());
    EXPECT_EQ(report.firstUninitAddress, 0x10200U);
    EXPECT_EQ(report.common.exec.instrExecId, 2U);
    EXPECT_FALSE(s.initialization.AnalysisComplete());
}

TEST(UnifiedMemoryTest, MissingOperandNeverBecomesALocalRangeDegradation)
{
    Session s;
    s.boundsEnabled = s.raceEnabled = false;
    s.Launch(1);
    auto incomplete = LocalCopyOperand(0, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0);
    s.Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, incomplete);
    s.Memory(1, 2, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 256, 128);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.initialization.AnalysisComplete());
    EXPECT_NE(s.initialization.Summary().find("degraded_instructions=0"), std::string::npos);
}
TEST(UnifiedMemoryTest, OversizedReportOperandRemainsAFatalResourceFailure)
{
    Session s;
    s.boundsEnabled = s.raceEnabled = false;
    AclsanResourceData allocation{};
    allocation.common = {ACLSAN_API_VERSION, sizeof(allocation), nullptr, 0, 0};
    allocation.ptr = reinterpret_cast<void*>(0x10000);
    allocation.bytes = UINT64_C(1) << 33;
    allocation.resourceId = 1;
    allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    s.Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, allocation);
    s.Launch(1);
    s.Memory(
        1, 1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000, 128);
    s.Memory(
        1, 2, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000,
        UINT64_C(1) << 32);
    s.Sync();
    EXPECT_TRUE(s.reports.empty());
    EXPECT_FALSE(s.initialization.AnalysisComplete());
    EXPECT_NE(s.initialization.Summary().find("initialization_resource_exhausted=1"), std::string::npos);
    EXPECT_NE(s.initialization.Summary().find("degraded_instructions=0"), std::string::npos);
}
} // namespace
} // namespace npucheck
