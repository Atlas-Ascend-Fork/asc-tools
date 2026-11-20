/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/initcheck.h"
#include "device_instr/common/instruction_id.h"
#include "diagnostic/report_renderer.h"
#include "log_capture.h"
#include <gtest/gtest.h>
#include <set>
#include <type_traits>

namespace npucheck {
namespace {
AclsanDeviceMemoryAccessData UbAccess(uint64_t instruction, uint32_t mode, uint64_t address = 128, uint64_t bytes = 16)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.deviceId = 2;
    data.header.launchId = 11;
    data.header.executor = {18, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR};
    data.memoryOwner = data.header.executor;
    data.header.instrExecId = instruction;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    data.accessMode = mode;
    data.accessCount = 1;
    data.address = address;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = bytes;
    return data;
}
const auto ubScope =
    InitStateKey::Local(2, 11, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 0, ACLSAN_DEVICE_MEMORY_SPACE_UB);

TEST(UbInitcheckTest, PhysicalStateCapacityIncludesLastByte)
{
    MemoryInitcheck checker;
    EXPECT_EQ(InitStateTableIndex::LocalCapacity(ACLSAN_DEVICE_MEMORY_SPACE_UB), 256U * 1024U);
    EXPECT_EQ(checker.MarkInitialized(ubScope, 256U * 1024U - 1, 1), InitCheckStatus::OK);
}

TEST(UbInitcheckTest, ReadWithoutLayoutReportsUninitialized)
{
    MemoryInitcheck checker;
    const auto result = checker.CheckInstruction(ubScope, {UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ)});
    EXPECT_EQ(result.status, InitCheckStatus::OK);
    ASSERT_EQ(result.reports.size(), 1U);
    EXPECT_EQ(result.reports.front().uninitBytes, 16U);
}

TEST(UbInitcheckTest, CallbackChecksUbWithoutLayout)
{
    Initcheck checker;
    CheckerReportList reports;
    AclsanLaunchData launch{};
    launch.common = {ACLSAN_API_VERSION, sizeof(launch), nullptr, 0, 11};
    launch.launchId = 11;
    EXPECT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    AclsanOperatorExecutionData completion{};
    completion.common = {ACLSAN_API_VERSION, sizeof(completion), nullptr, 0, 11};
    completion.launchId = 11;
    EXPECT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &completion, reports));
    AclsanSynchronizeData sync{};
    sync.common.version = ACLSAN_API_VERSION;
    sync.common.size = sizeof(sync);
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
}
TEST(UbInitcheckTest, VfInternalExecutionsHaveIndependentOperandIndexes)
{
    auto read = UbAccess(7, ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ);
    read.vfInstrValid = 1;

    read.vfInstrExecSeq = 0;
    auto write = read;
    write.vfInstrExecSeq = 3;
    write.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE;
    write.address = 192;
    EXPECT_NE(race::Key(read), race::Key(write));
    MemoryInitcheck checker;
    ASSERT_EQ(checker.CheckInstruction(ubScope, {write}).status, InitCheckStatus::OK);
    const auto result = checker.CheckInstruction(ubScope, {read});
    ASSERT_EQ(result.status, InitCheckStatus::OK);
    ASSERT_EQ(result.reports.size(), 1U);
}

TEST(UbInitcheckTest, VfReadWriteModesTrackInitializedBytes)
{
    for (const auto readMode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ, ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ}) {
        MemoryInitcheck checker;
        auto read = UbAccess(7, readMode);
        read.vfInstrValid = 1;

        auto result = checker.CheckInstruction(ubScope, {read});
        ASSERT_EQ(result.status, InitCheckStatus::OK);
        ASSERT_EQ(result.reports.size(), 1U);
        EXPECT_EQ(result.reports[0].uninitBytes, 16U);
        auto write = read;
        write.vfInstrExecSeq = 1;
        write.accessMode = readMode == ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ ?
                               ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE :
                               ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE;
        ASSERT_EQ(checker.CheckInstruction(ubScope, {write}).status, InitCheckStatus::OK);
        read.vfInstrExecSeq = 2;
        result = checker.CheckInstruction(ubScope, {read});
        EXPECT_EQ(result.status, InitCheckStatus::OK);
        EXPECT_TRUE(result.reports.empty());
    }
}

TEST(UbInitcheckTest, ReportsUbCapacityAndCopyWritesDestinationAfterReportingSource)
{
    MemoryInitcheck checker;
    auto read = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    auto write = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 192);
    read.accessCount = write.accessCount = 2;
    write.accessIndex = 1;
    auto result = checker.CheckInstruction(ubScope, {read, write});
    ASSERT_EQ(result.status, InitCheckStatus::OK);
    ASSERT_EQ(result.reports.size(), 1U);
    EXPECT_EQ(result.reports[0].allocation.base, 0U);
    EXPECT_EQ(result.reports[0].allocation.bytes, 256U * 1024);
    EXPECT_EQ(result.reports[0].firstUninitOffset, 128U);
    EXPECT_TRUE(
        checker.CheckInstruction(ubScope, {UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 192)}).reports.empty());
}

TEST(UbInitcheckTest, IncompleteGroupCannotCommitWrites)
{
    MemoryInitcheck checker;
    auto write = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    write.accessCount = 2;
    EXPECT_EQ(checker.CheckInstruction(ubScope, {write}).status, InitCheckStatus::INCOMPLETE_RECORD);
    auto read = checker.CheckInstruction(ubScope, {UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ)});
    ASSERT_EQ(read.reports.size(), 1U);
}

class UbReplayTest : public testing::TestWithParam<bool> {
protected:
    void SetUp() override
    {
        analysis = std::make_shared<MemoryAnalysis>(GetParam());
        checker = std::make_unique<Initcheck>(analysis);
    }
    template <typename T>
    void Send(AclsanCallbackDomain domain, AclsanCallbackId id, const T& value)
    {
        if constexpr (std::is_same_v<T, AclsanLaunchData>) {
            pendingLaunches.insert(value.launchId);
        }
        if (GetParam()) {
            ASSERT_TRUE(analysis->BeginCallback(domain, id, &value));
        }
        ASSERT_TRUE(checker->OnCallback(domain, id, &value, reports));
        if (GetParam()) {
            analysis->EndCallback();
        }
    }
    void Launch(uint64_t launch = 11)
    {
        AclsanLaunchData data{};
        data.common.version = ACLSAN_API_VERSION;
        data.common.size = sizeof(data);
        data.launchId = launch;
        Send(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, data);
    }
    void Memory(AclsanDeviceMemoryAccessData data)
    {
        Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, data);
    }
    void Sync()
    {
        uint64_t frontier = 0;
        for (const auto launchId : pendingLaunches) {
            AclsanOperatorExecutionData completion{};
            completion.common = {ACLSAN_API_VERSION, sizeof(completion), nullptr, 0, launchId};
            completion.launchId = launchId;
            frontier = launchId;
            Send(ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, completion);
        }
        pendingLaunches.clear();
        AclsanSynchronizeData data{};
        data.common.version = ACLSAN_API_VERSION;
        data.common.size = sizeof(data);
        data.common.correlationId = frontier;
        Send(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, data);
    }
    void AllocateGm()
    {
        AclsanResourceData data{};
        data.common.version = ACLSAN_API_VERSION;
        data.common.size = sizeof(data);
        data.deviceId = 2;
        data.resourceId = 1;
        data.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
        data.ptr = reinterpret_cast<void*>(0x10000);
        data.bytes = 256;
        Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, data);
    }
    AclsanDeviceMemoryAccessData OtherAccess(uint64_t instruction, uint32_t mode, uint32_t space, uint64_t offset = 0)
    {
        auto data = UbAccess(instruction, mode, space == ACLSAN_DEVICE_MEMORY_SPACE_GM ? 0x10000 + offset : offset);
        data.memorySpace = space;
        if (space != ACLSAN_DEVICE_MEMORY_SPACE_GM) {
            data.header.executor = {0, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE};
            data.memoryOwner = data.header.executor;
        }
        return data;
    }
    std::set<uint64_t> pendingLaunches;
    std::shared_ptr<MemoryAnalysis> analysis;
    std::unique_ptr<Initcheck> checker;
    CheckerReportList reports;
};

TEST_P(UbReplayTest, VfCompletionAndLocalBarrierMakeWritesVisible)
{
    Launch();
    EXPECT_TRUE(checker->IsSubscribed(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF));
    auto write = UbAccess(7, ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE);
    write.vfInstrValid = 1;

    Memory(write);
    AclsanDeviceBarrierData barrier{};
    barrier.header = write.header;
    barrier.header.size = sizeof(barrier);
    barrier.barrierKind = ACLSAN_DEVICE_BARRIER_LOCAL_MEM;
    barrier.pipe = ACLSAN_DEVICE_PIPE_INVALID;
    barrier.srcMemType = ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE;
    barrier.dstMemType = ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD;

    barrier.vfInstrExecSeq = 1;
    Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER, barrier);
    auto read = write;
    read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ;
    read.vfInstrExecSeq = 2;
    Memory(read);
    AclsanDeviceVfData completion{};
    completion.header = write.header;
    completion.header.size = sizeof(completion);
    completion.status = ACLSAN_DEVICE_VF_SUCCESS;
    completion.accessCount = 2;
    completion.eventCount = completion.rawEventCount = 3;
    Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF, completion);
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}

TEST_P(UbReplayTest, VfReadWithoutCompletionStaysIncomplete)
{
    Launch();
    auto read = UbAccess(7, ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ);
    read.vfInstrValid = 1;

    Memory(read);
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker->AnalysisComplete());
    EXPECT_NE(checker->Summary().find("missing_vf_expansion"), std::string::npos);
}

TEST_P(UbReplayTest, VfExecutionSequenceMakesPriorWriteVisible)
{
    Launch();
    auto write = UbAccess(7, ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE);
    write.vfInstrValid = 1;

    Memory(write);
    auto read = write;
    read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ;
    read.vfInstrExecSeq = 1;
    Memory(read);
    AclsanDeviceVfData completion{};
    completion.header = write.header;
    completion.header.size = sizeof(completion);
    completion.status = ACLSAN_DEVICE_VF_SUCCESS;
    completion.accessCount = completion.eventCount = completion.rawEventCount = 2;
    Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF, completion);
    Sync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}

TEST_P(UbReplayTest, PartialWritePreservesHolesWithoutLayout)
{
    Launch();
    auto write = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    write.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    write.layout.blockRepeat = {};
    write.layout.blockRepeat.blockSize = 8;
    write.layout.blockRepeat.blockNum = 2;
    write.layout.blockRepeat.repeatTimes = 1;
    write.layout.blockRepeat.blockStride = 16;
    Memory(write);
    Memory(UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 128, 32));
    Sync();
    ASSERT_EQ(reports.size(), 1U) << checker->Summary();
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.uninitBytes, 16U);
    EXPECT_EQ(report.initializedBytes, 16U);
    EXPECT_EQ(report.firstUninitAddress, 136U);
    EXPECT_EQ(report.allocation.base, 0U);
    EXPECT_EQ(report.allocation.bytes, 256U * 1024);
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}

TEST_P(UbReplayTest, NoncontiguousReadReportsUninitializedFragment)
{
    Launch();
    Memory(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 128, 32));
    auto read = UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 128, 64);
    read.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    read.layout.blockRepeat = {};
    read.layout.blockRepeat.blockSize = 32;
    read.layout.blockRepeat.blockNum = 2;
    read.layout.blockRepeat.repeatTimes = 1;
    read.layout.blockRepeat.blockStride = 64;
    Memory(read);
    Sync();
    ASSERT_EQ(reports.size(), 1U) << checker->Summary();
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.access.accessBytes, 32U);
    EXPECT_EQ(report.initializedBytes, 0U);
    EXPECT_EQ(report.uninitBytes, 32U);
    EXPECT_EQ(report.firstUninitAddress, 192U);
    EXPECT_EQ(report.access.rangeBegin, 192U);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}

TEST_P(UbReplayTest, UninitializedUbReadStillInitializesLocalWrite)
{
    Launch();
    auto source = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    auto destination = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    destination.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
    source.accessCount = destination.accessCount = 2;
    destination.accessIndex = 1;
    source.header.executor = destination.header.executor = {0, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE};
    destination.memoryOwner = destination.header.executor;
    Memory(source);
    Memory(destination);
    auto read = UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    read.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
    read.header.executor = {0, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE};
    read.memoryOwner = read.header.executor;
    Memory(read);
    Sync();
    ASSERT_EQ(reports.size(), 1U) << checker->Summary();
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[0]).access.memorySpace, NpuCheckReportMemorySpace::UB);
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}

TEST_P(UbReplayTest, CoreAndLaunchStateAreIsolated)
{
    Launch();
    Memory(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE));
    auto otherCore = UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    otherCore.header.executor = {19, 1, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR};
    otherCore.memoryOwner = otherCore.header.executor;
    Memory(otherCore);
    Sync();
    ASSERT_EQ(reports.size(), 1U);
    Launch(12);
    auto nextLaunch = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    nextLaunch.header.launchId = 12;
    Memory(nextLaunch);
    Sync();
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}

TEST_P(UbReplayTest, MissingOperandRemainsIncomplete)
{
    Launch();
    auto write = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    write.accessCount = 2;
    Memory(write);
    Sync();
    EXPECT_FALSE(checker->AnalysisComplete());
    EXPECT_TRUE(reports.empty());
}

TEST_P(UbReplayTest, MissingLayoutDoesNotDegradeInitialization)
{
    Launch();
    Memory(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    ASSERT_EQ(reports.size(), 1U);
}

TEST_P(UbReplayTest, MissingLayoutChecksUbGmAndL1Reads)
{
    AllocateGm();
    Launch();
    Memory(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Memory(OtherAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_GM));
    Memory(OtherAccess(3, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_L1));
    Sync();
    ASSERT_EQ(reports.size(), 3U) << checker->Summary();
    std::set<NpuCheckReportMemorySpace> spaces;
    for (const auto& report : reports) {
        spaces.insert(std::get<NpuCheckInitcheckReport>(report).access.memorySpace);
    }
    EXPECT_EQ(
        spaces, (std::set<NpuCheckReportMemorySpace>{
                    NpuCheckReportMemorySpace::GM, NpuCheckReportMemorySpace::L1, NpuCheckReportMemorySpace::UB}));
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}

TEST_P(UbReplayTest, CrossCoreUbCopiesInitializeL1AcrossLayoutBoundaries)
{
    AclsanLaunchData launch{};
    launch.common = {ACLSAN_API_VERSION, sizeof(launch), nullptr, 0, 0};
    launch.launchId = 11;
    launch.ubMetadataValid = 1;
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY;
    launch.compilerAllocUbBytes = 64 * 1024;
    launch.dynamicUbBytes = 32 * 1024;
    Send(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, launch);

    constexpr uint64_t reservedBegin = 88 * 1024;
    const auto copy = [this](uint64_t exec, uint64_t ubAddress, uint64_t l1Address) {
        auto source = UbAccess(exec, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ubAddress, 64);
        source.instructionId = static_cast<uint32_t>(aclsan::InstructionId::CopyUbufToCbuf);
        source.header.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
        source.header.executor.phyCoreId = 18;
        auto destination = source;
        destination.memoryOwner = {0, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE};
        source.accessCount = destination.accessCount = 2;
        destination.accessIndex = 1;
        destination.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
        destination.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        destination.address = l1Address;
        Memory(source);
        Memory(destination);
        return destination.header;
    };
    copy(1, reservedBegin - 32, 0);
    auto header = copy(2, reservedBegin, 64); // Both actual L1 writes initialize their destinations.
    AclsanDeviceBarrierData barrier{};
    barrier.header = header;
    barrier.header.size = sizeof(barrier);
    barrier.header.instrExecId = 3;
    barrier.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    barrier.pipe = ACLSAN_DEVICE_PIPE_ALL;
    Send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER, barrier);

    auto read = UbAccess(4, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 128);
    read.header.executor = {0, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE};
    read.memoryOwner = read.header.executor;
    read.header.executor = header.executor;
    read.header.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    read.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
    Memory(read);
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    ASSERT_EQ(reports.size(), 2U);
    for (const auto& entry : reports) {
        const auto& report = std::get<NpuCheckInitcheckReport>(entry);
        EXPECT_EQ(report.access.memorySpace, NpuCheckReportMemorySpace::UB);
        EXPECT_EQ(report.uninitBytes, 64U);
    }
}

TEST_P(UbReplayTest, UbSourceReadReportsAndStillCommitsGmDestination)
{
    AclsanResourceData allocation{};
    allocation.common.version = ACLSAN_API_VERSION;
    allocation.common.size = sizeof(allocation);
    allocation.deviceId = 2;
    allocation.resourceId = 1;
    allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    allocation.ptr = reinterpret_cast<void*>(0x10000);
    allocation.bytes = 256;
    Send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, allocation);
    Launch();
    auto source = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    auto destination = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x10000);
    destination.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    source.accessCount = destination.accessCount = 2;
    destination.accessIndex = 1;
    Memory(source);
    Memory(destination);
    auto read = UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0x10000);
    read.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    Memory(read);
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).access.memorySpace, NpuCheckReportMemorySpace::UB);
}

TEST_P(UbReplayTest, EmptyRangeDoesNotRequirePublicProvider)
{
    Launch();
    Memory(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 0));
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    EXPECT_TRUE(reports.empty());
}

INSTANTIATE_TEST_SUITE_P(StandaloneAndManaged, UbReplayTest, testing::Bool());

} // namespace
} // namespace npucheck

#include "checker/ub_layout.h"
namespace npucheck {
TEST(UbLayoutTest, MixedDynamicBoundaryIsNot248KiB)
{
    AclsanLaunchData launch{};
    launch.ubMetadataValid = 1;
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY;
    launch.compilerAllocUbBytes = 64 * 1024;
    launch.dynamicUbBytes = 32 * 1024;
    auto layout = BuildUbLayout(launch);
    ASSERT_TRUE(layout.valid);
    auto region = layout.Find(86 * 1024);
    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->begin, 56U * 1024);
    EXPECT_EQ(region->end, 88U * 1024);
    EXPECT_FALSE(region->Contains(86 * 1024, 4 * 1024));
    EXPECT_TRUE(layout.Find(88 * 1024)->Contains(88 * 1024, 8 * 1024));
}
TEST(UbLayoutTest, SimdVfReservedStaysAtPhysicalTail)
{
    AclsanLaunchData launch{};
    launch.ubMetadataValid = 1;
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY;
    launch.compilerAllocUbBytes = 64 * 1024;
    launch.dynamicUbBytes = 32 * 1024;
    const auto layout = BuildUbLayout(launch);
    ASSERT_TRUE(layout.valid);
    EXPECT_EQ(layout.Find(248 * 1024)->kind, UbRegionKind::RESERVED);
    EXPECT_EQ(layout.Find(100 * 1024)->kind, UbRegionKind::DYNAMIC);
    EXPECT_EQ(layout.Find(100 * 1024)->end, 248U * 1024);
}
TEST(UbLayoutTest, UnknownOrOverflowingInputsDoNotBecomePureSimd)
{
    AclsanLaunchData launch{};
    EXPECT_FALSE(BuildUbLayout(launch).valid);
    launch.ubMetadataValid = 1;
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF;
    launch.compilerAllocUbBytes = UINT64_MAX;
    launch.dynamicUbBytes = 1024;
    EXPECT_FALSE(BuildUbLayout(launch).valid);
}
} // namespace npucheck

#include "checker/memcheck.h"
namespace npucheck {
namespace {
AclsanLaunchData MixedUbLaunch()
{
    AclsanLaunchData launch{};
    launch.common.version = ACLSAN_API_VERSION;
    launch.common.size = sizeof(launch);
    launch.launchId = 11;
    launch.ubMetadataValid = 1;
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY;
    launch.compilerAllocUbBytes = 64 * 1024;
    launch.dynamicUbBytes = 32 * 1024;
    return launch;
}
TEST(UbMemcheckIntegrationTest, MixedScalarCrossesDynamicEndBelow248KiB)
{
    Memcheck checker;
    CheckerReportList reports;
    auto launch = MixedUbLaunch();
    checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 88 * 1024 - 4, 8);
    checker.QueueDeviceMemoryAccess(access);
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].access.memorySpace, NpuCheckReportMemorySpace::UB);
    EXPECT_EQ(errors[0].allocation.base, 56U * 1024);
    EXPECT_EQ(errors[0].allocation.bytes, 32U * 1024);
}
TEST(UbMemcheckIntegrationTest, SimdVfDynamicCapacityDoesNotDependOnHostRequest)
{
    for (const auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
          ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ, ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE,
          ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ, ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE}) {
        for (const uint64_t request : {0U, 32U * 1024U, 64U * 1024U}) {
            SCOPED_TRACE(::testing::Message() << "mode=" << mode << " request=" << request);
            Memcheck checker;
            CheckerReportList reports;
            auto launch = MixedUbLaunch();
            launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY;
            launch.dynamicUbBytes = request;
            ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
            auto access = UbAccess(1, mode, 248U * 1024U - 32U, 32);
            if (mode != ACLSAN_DEVICE_MEMORY_ACCESS_READ && mode != ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) {
                access.vfInstrValid = 1;
            }
            checker.QueueDeviceMemoryAccess(access); // Last dynamic bytes remain valid for every host request.
            access.header.instrExecId = 2;
            access.vfInstrExecSeq = 1;
            access.layout.range.bytes = 64;
            checker.QueueDeviceMemoryAccess(access); // Crosses into the reservation.
            access.header.instrExecId = 3;
            access.vfInstrExecSeq = 2;
            access.address = 88U * 1024U;
            access.layout.range.bytes = 4;
            checker.QueueDeviceMemoryAccess(access); // The former unallocated range is valid dynamic memory.
            const auto errors = checker.OnSynchronization();
            ASSERT_EQ(errors.size(), 1U);
            EXPECT_EQ(errors[0].access.address, 248U * 1024U - 32U);
            EXPECT_EQ(errors[0].allocation.base, 56U * 1024U);
            EXPECT_EQ(errors[0].allocation.bytes, (248U - 56U) * 1024U);
            EXPECT_TRUE(checker.AnalysisComplete());
        }
    }
}
TEST(UbMemcheckIntegrationTest, SimdVfZeroRequestAndZeroStaticStillHaveUserMemory)
{
    MemoryAnalysis analysis;
    auto launch = MixedUbLaunch();
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY;
    launch.compilerAllocUbBytes = 8U * 1024U;
    launch.dynamicUbBytes = 0;
    analysis.RegisterUbLayout(launch);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    EXPECT_EQ(analysis.ClassifyUbRange(access, access.address, 4).status, RangeStatus::VALID);
    access.address = 248U * 1024U - 4U;
    EXPECT_EQ(analysis.ClassifyUbRange(access, access.address, 4).status, RangeStatus::VALID);
    access.address = 248U * 1024U;
    EXPECT_EQ(analysis.ClassifyUbRange(access, access.address, 4).status, RangeStatus::VALID);
}
TEST(UbMemcheckIntegrationTest, SimdVfReadBeyondHostRequestReportsUninitializedAndCopyInitializesTarget)
{
    auto analysis = std::make_shared<MemoryAnalysis>();
    auto launch = MixedUbLaunch();
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY;
    analysis->RegisterUbLayout(launch);
    MemoryInitcheck checker(analysis->state);
    auto source = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 88U * 1024U, 32);
    auto target = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 56U * 1024U, 32);
    source.accessCount = target.accessCount = 2;
    target.accessIndex = 1;
    const auto copy = checker.CheckInstruction(ubScope, {source, target});
    ASSERT_EQ(copy.reports.size(), 1U);
    EXPECT_EQ(copy.reports.front().uninitBytes, 32U);
    auto read = UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, target.address, 32);
    EXPECT_TRUE(checker.CheckInstruction(ubScope, {read}).reports.empty());
}
TEST(UbMemcheckIntegrationTest, SimdVfStillRejectsStaticCrossingAndReservationOverflow)
{
    Memcheck checker;
    CheckerReportList reports;
    auto launch = MixedUbLaunch();
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY;
    checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
    checker.QueueDeviceMemoryAccess(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 56U * 1024U - 4U, 8));
    checker.QueueDeviceMemoryAccess(UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 256U * 1024U - 4U, 8));
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 2U);
    EXPECT_EQ(errors[0].allocation.base, 0U);
    EXPECT_EQ(errors[0].allocation.bytes, 56U * 1024U);
    EXPECT_EQ(errors[1].allocation.base, 0U);
    EXPECT_EQ(errors[1].allocation.bytes, 256U * 1024U);
}
TEST(UbMemcheckIntegrationTest, RepeatCannotSelectAnotherRegionForSecondBlock)
{
    Memcheck checker;
    CheckerReportList reports;
    auto launch = MixedUbLaunch();
    checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 56 * 1024 - 32, 32);
    access.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    access.layout.blockRepeat.blockSize = 32;
    access.layout.blockRepeat.blockNum = 1;
    access.layout.blockRepeat.repeatTimes = 2;
    access.layout.blockRepeat.repeatStride = 32;
    checker.QueueDeviceMemoryAccess(access);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
}
TEST(UbMemcheckIntegrationTest, DiagnosticsDescribeLayoutAndInvalidRegion)
{
    struct Scenario {
        AclsanKernelVfType type;
        uint64_t address;
        const char* layout;
        const char* violation;
    };
    const Scenario scenarios[] = {
        {ACLSAN_KERNEL_VF_TYPE_NO_VF, 248U * 1024U - 4U, "USER [0x0, 0x3e000)",
         "outside USER (boundary 0x3e000); first invalid address 0x3e000 in RESERVED"},
        {ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY, 56U * 1024U - 4U, "DYNAMIC [0xe000, 0x3e000)",
         "outside STATIC (boundary 0xe000); first invalid address 0xe000 in DYNAMIC"},
        {ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY, 248U * 1024U - 4U, "DYNAMIC [0xe000, 0x3e000)",
         "outside DYNAMIC (boundary 0x3e000); first invalid address 0x3e000 in RESERVED"},
        {ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY, 256U * 1024U - 4U, "RESERVED [0x3e000, 0x40000)",
         "outside physical UB (end 0x40000); first invalid address 0x40000 in OUTSIDE_UB"},
        {ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY, 88U * 1024U - 4U, "DYNAMIC [0xe000, 0x16000)",
         "outside DYNAMIC (boundary 0x16000); first invalid address 0x16000 in RESERVED"},
        {ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY, 96U * 1024U, "SIMD_FORBIDDEN [0x18000, 0x40000)",
         "operand starts in SIMD_FORBIDDEN; first invalid address 0x18000 in SIMD_FORBIDDEN"},
        {ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF, 96U * 1024U - 4U, "RESERVED [0x16000, 0x18000)",
         "outside RESERVED (boundary 0x18000); first invalid address 0x18000 in SIMD_FORBIDDEN"},
    };
    for (const auto& scenario : scenarios) {
        SCOPED_TRACE(::testing::Message() << "type=" << scenario.type << " address=" << scenario.address);
        Memcheck checker;
        CheckerReportList reports;
        auto launch = MixedUbLaunch();
        launch.kernelVfType = scenario.type;
        checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
        checker.QueueDeviceMemoryAccess(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, scenario.address, 8));
        const auto errors = checker.OnSynchronization();
        ASSERT_EQ(errors.size(), 1U);
        std::string text;
        ASSERT_EQ(
            RenderNpuCheckReportRecord(NpuCheckReportRecord::From(errors[0]), {}, &text), ReportRenderStatus::SUCCESS);
        EXPECT_NE(text.find("UB layout (type " + std::to_string(scenario.type) + "):"), std::string::npos);
        EXPECT_NE(text.find(scenario.layout), std::string::npos);
        EXPECT_NE(text.find(scenario.violation), std::string::npos);
        EXPECT_NE(text.find("Invalid UB read of size 8 bytes"), std::string::npos);
    }
}
TEST(UbMemcheckIntegrationTest, DiagnosticsUseActualStridedFragmentWithoutFillingHoles)
{
    Memcheck checker;
    CheckerReportList reports;
    auto launch = MixedUbLaunch();
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY;
    checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 56U * 1024U - 32U, 16);
    access.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    access.layout.blockRepeat.blockSize = 16;
    access.layout.blockRepeat.blockNum = 1;
    access.layout.blockRepeat.repeatTimes = 2;
    access.layout.blockRepeat.repeatStride = 64;
    checker.QueueDeviceMemoryAccess(access);
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 1U);
    std::string text;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(errors[0]), {}, &text), ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("UB access: operand=0xdfe0, fragment=[0xe020, 0xe030)"), std::string::npos);
    EXPECT_NE(text.find("first invalid address 0xe020 in DYNAMIC"), std::string::npos);
}
TEST(UbMemcheckIntegrationTest, DiagnosticsDoNotGuessUnknownLayoutForPhysicalOverflow)
{
    Memcheck checker;
    checker.QueueDeviceMemoryAccess(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, UB_CAPACITY - 4U, 8));
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 1U);
    std::string text;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(errors[0]), {}, &text), ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("UB layout: UNKNOWN; physical UB [0x0, 0x40000)"), std::string::npos);
    EXPECT_NE(text.find("first invalid address 0x40000 in OUTSIDE_UB"), std::string::npos);
    EXPECT_EQ(text.find("UB layout (type 1)"), std::string::npos);
}
TEST(UbMemcheckIntegrationTest, DiagnosticsIdentifyLowerBoundaryForNegativeStride)
{
    Memcheck checker;
    CheckerReportList reports;
    auto launch = MixedUbLaunch();
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY;
    checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 56U * 1024U + 32U, 16);
    access.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    access.layout.ndAffine.rank = 1;
    access.layout.ndAffine.elementBytes = 16;
    access.layout.ndAffine.dims[0] = 2;
    access.layout.ndAffine.strides[0] = -64;
    checker.QueueDeviceMemoryAccess(access);
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 1U);
    std::string text;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(errors[0]), {}, &text), ReportRenderStatus::SUCCESS);
    EXPECT_NE(
        text.find("outside DYNAMIC (boundary 0xe000); first invalid address 0xdfe0 in STATIC"), std::string::npos);
}
TEST(UbMemcheckIntegrationTest, NegativeStridePhysicalOverflowReportsCurrentFragment)
{
    Memcheck checker;
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 256U * 1024U + 32U, 64);
    access.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    access.layout.ndAffine.rank = 1;
    access.layout.ndAffine.elementBytes = 64;
    access.layout.ndAffine.dims[0] = 2;
    access.layout.ndAffine.strides[0] = -64;
    checker.QueueDeviceMemoryAccess(access);
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 1U);
    std::string text;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(errors[0]), {}, &text), ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("first invalid address 0x40000 in OUTSIDE_UB"), std::string::npos) << text;
}

TEST(UbMemcheckIntegrationTest, DiagnosticsShowOverflowWithoutWrappingFragmentEnd)
{
    Memcheck checker;
    checker.QueueDeviceMemoryAccess(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, UINT64_MAX - 3U, 8));
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 1U);
    std::string text;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(errors[0]), {}, &text), ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("fragment=[0xfffffffffffffffc, overflow)"), std::string::npos);
    EXPECT_NE(text.find("first invalid address 0xfffffffffffffffc in OUTSIDE_UB"), std::string::npos);
}
TEST(UbMemcheckIntegrationTest, OrdinaryAndVfAccessUseTheDynamicSizeOfTheirLaunch)
{
    MemoryAnalysis analysis;
    auto launch = MixedUbLaunch();
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF;
    analysis.RegisterUbLayout(launch);
    ++launch.launchId;
    launch.dynamicUbBytes = 64 * 1024;
    analysis.RegisterUbLayout(launch);

    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 88 * 1024 - 16, 32);
    EXPECT_EQ(
        analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes).status, RangeStatus::OUT_OF_BOUNDS);
    access.vfInstrValid = 1;

    EXPECT_EQ(
        analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes).status, RangeStatus::OUT_OF_BOUNDS);

    access.header.launchId = launch.launchId;
    const auto decision = analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes);
    EXPECT_EQ(decision.status, RangeStatus::VALID);
    ASSERT_TRUE(decision.allocation);
    EXPECT_EQ(decision.allocation->base, 56U * 1024);
    EXPECT_EQ(decision.allocation->bytes, 64U * 1024);
    access.vfInstrValid = 0;
}
TEST(UbMemcheckIntegrationTest, FailedVfExpansionCannotReportComplete)
{
    Memcheck checker;
    CheckerReportList reports;
    EXPECT_TRUE(checker.IsSubscribed(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF));
    AclsanDeviceVfData completion{};
    completion.header = UbAccess(7, ACLSAN_DEVICE_MEMORY_ACCESS_READ).header;
    completion.header.size = sizeof(completion);
    completion.status = ACLSAN_DEVICE_VF_QUERY_FAILED;
    EXPECT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF, &completion, reports));
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(UbMemcheckIntegrationTest, VfModesReportDynamicBoundaryViolations)
{
    for (const auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ, ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE,
          ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ, ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE}) {
        Memcheck checker;
        CheckerReportList reports;
        auto launch = MixedUbLaunch();
        launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF;
        checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
        auto access = UbAccess(7, mode, 88 * 1024 - 16, 32);
        access.vfInstrValid = 1;

        checker.QueueDeviceMemoryAccess(access);
        const auto errors = checker.OnSynchronization();
        ASSERT_EQ(errors.size(), 1U);
        EXPECT_EQ(errors[0].allocation.base, 56U * 1024);
        EXPECT_TRUE(checker.AnalysisComplete());
    }
}

TEST(UbMemcheckIntegrationTest, ReservedStartAllowedButCrossingItsEndIsNot)
{
    Memcheck checker;
    CheckerReportList reports;
    auto launch = MixedUbLaunch();
    checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
    checker.QueueDeviceMemoryAccess(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 88 * 1024, 32));
    checker.QueueDeviceMemoryAccess(UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 96 * 1024 - 16, 32));
    checker.QueueDeviceMemoryAccess(UbAccess(3, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 96 * 1024, 4));
    const auto errors = checker.OnSynchronization();
    ASSERT_EQ(errors.size(), 2U);
    EXPECT_EQ(errors[0].allocation.base, 88U * 1024); // Crossed from the reserved region.
    EXPECT_EQ(errors[1].access.address, 96U * 1024);  // Started inside the forbidden tail.
    EXPECT_TRUE(checker.AnalysisComplete());
}
TEST(UbMemcheckIntegrationTest, LaunchLayoutDoesNotChangeInitialization)
{
    auto analysis = std::make_shared<MemoryAnalysis>();
    auto launch = MixedUbLaunch();
    analysis->RegisterUbLayout(launch);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 56 * 1024, 32);
    MemoryInitcheck checker(analysis->state);
    const auto result = checker.CheckInstruction(ubScope, {access});
    EXPECT_FALSE(result.reports.empty());
}
} // namespace
} // namespace npucheck

namespace npucheck {
TEST(UbLayoutTest, SimtFamiliesAreExplicitAndDoNotIncludeSimdConfigInstructions)
{
    EXPECT_TRUE(IsSimtMemoryInstruction(335));
    EXPECT_TRUE(IsSimtMemoryInstruction(346));
    EXPECT_TRUE(IsSimtMemoryInstruction(362));
    EXPECT_FALSE(IsSimtMemoryInstruction(386));
    EXPECT_FALSE(IsSimtMemoryInstruction(418));
    EXPECT_FALSE(IsSimtMemoryInstruction(481));
}
} // namespace npucheck

namespace npucheck {
TEST(UbMemcheckIntegrationTest, ManagedMemcheckAloneRetainsUbEvents)
{
    auto analysis = std::make_shared<MemoryAnalysis>(true);
    Memcheck checker(analysis);
    CheckerReportList reports;
    auto launch = MixedUbLaunch();
    ASSERT_TRUE(analysis->BeginCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch));
    checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports);
    analysis->EndCallback();
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 88 * 1024 - 4, 8);
    ASSERT_TRUE(
        analysis->BeginCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access));
    checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports);
    analysis->EndCallback();
    EXPECT_EQ(analysis->Get(11).memory.size(), 1U);
    AclsanSynchronizeData sync{};
    sync.common.version = ACLSAN_API_VERSION;
    sync.common.size = sizeof(sync);
    ASSERT_TRUE(analysis->BeginCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync));
    checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports);
    EXPECT_EQ(reports.size(), 1U);
    analysis->EndCallback();
}
} // namespace npucheck

namespace npucheck {
TEST(UbMemcheckIntegrationTest, MemcheckRejectsSimtTail)
{
    MemoryAnalysis analysis;
    analysis.RegisterUbLayout(MixedUbLaunch());
    const auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 96 * 1024, 4);
    EXPECT_EQ(
        analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes).status, RangeStatus::OUT_OF_BOUNDS);
}
TEST(UbMemcheckIntegrationTest, MissingHostMetadataDoesNotAssumePureSimd)
{
    MemoryAnalysis analysis;
    auto launch = MixedUbLaunch();
    launch.ubMetadataValid = 0;
    analysis.RegisterUbLayout(launch);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(
        analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes).status, RangeStatus::AMBIGUOUS);
}
TEST(UbMemcheckIntegrationTest, MissingLayoutWarnsOncePerLaunch)
{
    std::string log;
    test::ScopedLogCapture capture(log);
    MemoryAnalysis analysis;
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    for (const auto launch : {11U, 12U}) {
        access.header.launchId = launch;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            EXPECT_EQ(analysis.ClassifyUbRange(access, access.address, 16).status, RangeStatus::AMBIGUOUS);
        }
    }
    const std::string warning = "memcheck: UB layout unavailable";
    const auto first = log.find(warning);
    ASSERT_NE(first, std::string::npos);
    const auto second = log.find(warning, first + warning.size());
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(log.find(warning, second + warning.size()), std::string::npos);
    EXPECT_NE(log.find("device=2 launch=11"), std::string::npos);
    EXPECT_NE(log.find("device=2 launch=12"), std::string::npos);
    EXPECT_NE(log.find("reason=launch_metadata_unavailable"), std::string::npos);
}
TEST(UbMemcheckIntegrationTest, InvalidLayoutWarningExplainsBoundsFailure)
{
    std::string log;
    test::ScopedLogCapture capture(log);
    MemoryAnalysis analysis;
    auto launch = MixedUbLaunch();
    launch.dynamicUbBytes = UB_CAPACITY;
    analysis.RegisterUbLayout(launch);
    const auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(analysis.ClassifyUbRange(access, access.address, 16).status, RangeStatus::AMBIGUOUS);
    EXPECT_NE(log.find("reason=invalid_layout_inputs"), std::string::npos);
}
TEST(UbMemcheckIntegrationTest, PureSimdUserAndReservedRemainSeparate)
{
    MemoryAnalysis analysis;
    auto launch = MixedUbLaunch();
    launch.kernelVfType = ACLSAN_KERNEL_VF_TYPE_NO_VF;
    analysis.RegisterUbLayout(launch);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 248 * 1024 - 4, 8);
    EXPECT_EQ(
        analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes).status, RangeStatus::OUT_OF_BOUNDS);
    access.address = 248 * 1024;
}
} // namespace npucheck

namespace npucheck {
TEST(UbMemcheckIntegrationTest, DefaultReservedWithNoStaticAllocation)
{
    MemoryAnalysis analysis;
    auto launch = MixedUbLaunch();
    launch.compilerAllocUbBytes = 8 * 1024;
    analysis.RegisterUbLayout(launch);
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32);
    const auto decision = analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes);
    EXPECT_EQ(decision.status, RangeStatus::VALID);
    ASSERT_TRUE(decision.allocation);
    EXPECT_EQ(decision.allocation->base, 0U);
    EXPECT_EQ(decision.allocation->bytes, 32U * 1024);
    launch.compilerAllocUbBytes = 2 * 1024; // Too small for the supported default reservation.
    analysis.RegisterUbLayout(launch);
    EXPECT_EQ(
        analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes).status, RangeStatus::AMBIGUOUS);
}
} // namespace npucheck

namespace npucheck {
TEST(UbLayoutTest, SharedAlignmentExtendsUsableDynamicRegionUpToReserved)
{
    auto launch = MixedUbLaunch();
    launch.compilerAllocUbBytes = 65568;
    const auto layout = BuildUbLayout(launch);
    ASSERT_TRUE(layout.valid);
    const auto region = layout.Find(57376);
    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->kind, UbRegionKind::DYNAMIC);
    EXPECT_EQ(region->begin, 57376U);
    EXPECT_EQ(region->end, 90240U);
    const auto reserved = layout.Find(90240);
    ASSERT_TRUE(reserved);
    EXPECT_EQ(reserved->kind, UbRegionKind::RESERVED);
    EXPECT_EQ(reserved->end, 98432U);
    EXPECT_TRUE(region->Contains(90144, 96));
    EXPECT_FALSE(region->Contains(90144, 97));
    MemoryAnalysis analysis;
    analysis.RegisterUbLayout(launch);
    const auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 90144, 96);
    const auto decision = analysis.ClassifyUbRange(access, access.address, access.layout.range.bytes);
    EXPECT_EQ(decision.status, RangeStatus::VALID);
    ASSERT_TRUE(decision.allocation);
    EXPECT_EQ(decision.allocation->bytes, 32864U);
    launch.compilerAllocUbBytes -= 4; // The derived dynamic base must be 32B aligned.
    EXPECT_FALSE(BuildUbLayout(launch).valid);
}
} // namespace npucheck

namespace npucheck {
TEST(UbMemcheckIntegrationTest, MissingLayoutStillDetectsPhysicalOverflowButNeverAssumes248KiB)
{
    MemoryAnalysis analysis;
    auto access = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 256 * 1024, 4);
    EXPECT_EQ(analysis.ClassifyUbRange(access, access.address, 4).status, RangeStatus::OUT_OF_BOUNDS);
    access.address = 100 * 1024;
    EXPECT_EQ(analysis.ClassifyUbRange(access, access.address, 4).status, RangeStatus::AMBIGUOUS);
}
} // namespace npucheck

namespace npucheck {
TEST(UbAccessOnlyTest, ReservedBytesAreReadAndWrittenWithoutLayout)
{
    MemoryInitcheck checker;
    auto read = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 248 * 1024, 16);
    auto result = checker.CheckInstruction(ubScope, {read});
    ASSERT_EQ(result.status, InitCheckStatus::OK);
    ASSERT_EQ(result.reports.size(), 1U);
    EXPECT_EQ(result.reports.front().firstUninitAddress, read.address);
    auto write = read;
    write.header.instrExecId = 2;
    write.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    ASSERT_EQ(checker.CheckInstruction(ubScope, {write}).status, InitCheckStatus::OK);
    read.header.instrExecId = 3;
    EXPECT_TRUE(checker.CheckInstruction(ubScope, {read}).reports.empty());
}

TEST_P(UbReplayTest, AccessOnlyMissingLayoutStillChecksUbAndIndependentGm)
{
    AllocateGm();
    Launch();
    Memory(UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Memory(OtherAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_GM));
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    ASSERT_EQ(reports.size(), 2U);
}

TEST_P(UbReplayTest, AccessOnlyUbCopyCommitsGmDespiteMissingLayout)
{
    AllocateGm();
    Launch();
    auto source = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    auto target = OtherAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    source.accessCount = target.accessCount = 2;
    target.accessIndex = 1;
    Memory(source);
    Memory(target);
    Memory(OtherAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_GM));
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).access.memorySpace, NpuCheckReportMemorySpace::UB);
    Launch(12);
    auto read = OtherAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    read.header.launchId = 12;
    Memory(read);
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    EXPECT_EQ(reports.size(), 1U);
}

TEST_P(UbReplayTest, AccessOnlyGmToReservedUbChecksSourceAndInitializesTarget)
{
    AllocateGm();
    Launch();
    auto source = OtherAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    auto target = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 248 * 1024);
    source.accessCount = target.accessCount = 2;
    target.accessIndex = 1;
    Memory(source);
    Memory(target);
    Memory(UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, target.address));
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).access.memorySpace, NpuCheckReportMemorySpace::GM);
}
} // namespace npucheck

namespace npucheck {
TEST_P(UbReplayTest, AccessOnlyLayoutTypeDoesNotChangeWrittenBytes)
{
    const uint32_t types[] = {ACLSAN_KERNEL_VF_TYPE_UNKNOWN,          ACLSAN_KERNEL_VF_TYPE_NO_VF,
                              ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY,     ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY,
                              ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF, UINT32_MAX};
    for (const uint32_t type : types) {
        AclsanLaunchData launch{};
        launch.common = {ACLSAN_API_VERSION, sizeof(launch), nullptr, 0, 0};
        launch.launchId = 100 + static_cast<uint64_t>(type);
        launch.ubMetadataValid = 1;
        launch.kernelVfType = type;
        launch.compilerAllocUbBytes = 64 * 1024;
        launch.dynamicUbBytes = 32 * 1024;
        Send(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, launch);
        // Cross the static/dynamic boundary, then write/read reserved and SIMT tail bytes.
        uint64_t exec = 0;
        for (const auto address : {56U * 1024U - 8U, 88U * 1024U - 8U, 96U * 1024U, 248U * 1024U}) {
            auto write = UbAccess(++exec, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, address);
            write.header.launchId = launch.launchId;
            Memory(write);
            auto read = write;
            read.header.instrExecId = ++exec;
            read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
            Memory(read);
        }
        Sync();
        EXPECT_TRUE(reports.empty());
        EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    }
}

TEST_P(UbReplayTest, AccessOnlyUbBoundsReportDoesNotSuppressInitialization)
{
    auto launch = MixedUbLaunch();
    Send(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, launch);
    auto source = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 56 * 1024 - 8);
    auto target = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 248 * 1024);
    source.accessCount = target.accessCount = 2;
    target.accessIndex = 1;
    Memcheck bounds;
    CheckerReportList boundsReports;
    ASSERT_TRUE(bounds.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, boundsReports));
    bounds.QueueDeviceMemoryAccess(source);
    bounds.QueueDeviceMemoryAccess(target);
    const auto errors = bounds.OnSynchronization();
    ASSERT_EQ(errors.size(), 2U);
    for (const auto& error : errors) {
        reports.emplace_back(error);
    }
    Memory(source);
    Memory(target);
    Memory(UbAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, target.address));
    Sync();
    ASSERT_EQ(reports.size(), 3U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.back());
    EXPECT_EQ(report.access.address, source.address);
    EXPECT_EQ(report.uninitBytes, 16U);
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
}
} // namespace npucheck

namespace npucheck {
TEST_P(UbReplayTest, AccessOnlyOutOfRegionUbSourceStillInitializesGmAcrossLaunches)
{
    AllocateGm();
    auto launch = MixedUbLaunch();
    Send(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, launch);
    auto source = UbAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 96 * 1024);
    Memcheck bounds;
    CheckerReportList boundsReports;
    ASSERT_TRUE(bounds.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, boundsReports));
    bounds.QueueDeviceMemoryAccess(source);
    const auto errors = bounds.OnSynchronization();
    ASSERT_EQ(errors.size(), 1U);
    reports.emplace_back(errors.front());
    auto target = OtherAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    source.accessCount = target.accessCount = 2;
    target.accessIndex = 1;
    Memory(source);
    Memory(target);
    Memory(OtherAccess(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_GM));
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.back()).access.address, source.address);
    Launch(12);
    auto read = OtherAccess(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    read.header.launchId = 12;
    Memory(read);
    Sync();
    EXPECT_TRUE(checker->AnalysisComplete()) << checker->Summary();
    EXPECT_EQ(reports.size(), 2U);
}
} // namespace npucheck
