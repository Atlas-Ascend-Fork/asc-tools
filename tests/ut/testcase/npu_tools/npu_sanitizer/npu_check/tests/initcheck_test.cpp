// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "acl_san/aclsan_api.h"
#include "checker/initcheck.h"
#include "checker/memcheck.h"
#include "device_instr/common/instruction_id.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <tuple>

namespace npucheck {
namespace {

AclsanDeviceMemoryAccessData Access(
    uint64_t instrExecId, uint64_t dependencies, uint32_t blockId = 0, uint64_t launchId = 11)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.launchId = launchId;
    data.header.instrExecId = instrExecId;
    data.header.deviceId = 2;
    data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.blockId = blockId;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    data.instructionId = 72;
    data.regDependencyMask0 = dependencies;
    return data;
}

AclsanDeviceRegisterStateData State(
    uint32_t registerId, uint64_t value = 0, uint32_t blockId = 0, uint64_t launchId = 11)
{
    AclsanDeviceRegisterStateData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.launchId = launchId;
    data.header.deviceId = 2;
    data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.blockId = blockId;
    data.regId = registerId;
    data.value = value;
    return data;
}

bool Dispatch(Initcheck& checker, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    return checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, cbid, data, reports);
}

class InitcheckHostWriteTest : public ::testing::Test {
protected:
    Initcheck checker;
    CheckerReportList reports;
    static constexpr uint64_t base = 0x100000;
    void SetUp() override
    {
        AclsanResourceData alloc{};
        alloc.common.version = ACLSAN_API_VERSION;
        alloc.common.size = sizeof(alloc);
        alloc.ptr = reinterpret_cast<void*>(base);
        alloc.bytes = 128;
        alloc.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
        alloc.deviceId = 2;
        alloc.resourceId = 1;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &alloc, reports));
    }
    bool Write(uint64_t bytes, int result = 0, uint32_t device = 2)
    {
        AclsanMemoryWriteData write{};
        write.common.version = ACLSAN_API_VERSION;
        write.common.size = sizeof(write);
        write.common.result = result;
        write.destination = reinterpret_cast<void*>(base);
        write.bytes = bytes;
        write.deviceId = device;
        return checker.OnCallback(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, &write, reports);
    }
    void ReadAndSync()
    {
        auto access = Access(1, 0);
        access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        access.accessCount = 1;
        access.address = base;
        access.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        access.layout.range.bytes = 128;
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    }
};

TEST_F(InitcheckHostWriteTest, PartialHostWriteOnlyInitializesWrittenBytes)
{
    ASSERT_TRUE(Write(64));
    ReadAndSync();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ);
    EXPECT_EQ(report.uninitBytes, 64U);
    EXPECT_EQ(report.firstUninitAddress, base + 64);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckHostWriteTest, CompleteHostWriteMakesReadClean)
{
    ASSERT_TRUE(Write(128));
    ReadAndSync();
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckHostWriteTest, FailedWriteDoesNotInitialize)
{
    ASSERT_TRUE(Write(128, 1));
    ReadAndSync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[0]).uninitBytes, 128U);
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckHostWriteTest, WrongDeviceAndOutOfBoundsWritesAreIncomplete)
{
    EXPECT_FALSE(Write(128, 0, 3));
    EXPECT_FALSE(Write(129));
    ReadAndSync();
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckHostWriteTest, PendingLaunchWithoutTraceIsNotComplete)
{
    AclsanLaunchData launch{};
    launch.launchId = 11;
    launch.stream = reinterpret_cast<void*>(0x1234);
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    EXPECT_FALSE(checker.AnalysisComplete());
    AclsanSynchronizeData otherStream{};
    ASSERT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &otherStream, reports));
    EXPECT_FALSE(checker.AnalysisComplete());
    otherStream.stream = launch.stream;
    ASSERT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &otherStream, reports));
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST_F(InitcheckHostWriteTest, WriteDuringPendingLaunchDoesNotRetroactivelyInitialize)
{
    AclsanLaunchData launch{};
    launch.launchId = 11;
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    EXPECT_FALSE(Write(128));
    ReadAndSync();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[0]).uninitBytes, 128U);
    EXPECT_FALSE(checker.AnalysisComplete());
}

class CrossCoreInitcheckReplayTest : public InitcheckHostWriteTest,
                                     public ::testing::WithParamInterface<std::tuple<uint32_t, bool>> {
protected:
    AclsanDeviceMemoryAccessData Memory(uint64_t id, uint32_t physical, bool cube, bool write, uint64_t bytes = 128)
    {
        auto data = Access(id, 0, 100U + physical);
        data.header.phyCoreId = physical;
        data.header.blockType = cube ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        data.header.pipeline =
            write ? (cube ? ACLSAN_DEVICE_PIPE_FIXPIPE : ACLSAN_DEVICE_PIPE_MTE3) : ACLSAN_DEVICE_PIPE_MTE2;
        data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        data.accessMode = write ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        data.accessCount = 1;
        data.address = base;
        data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        data.layout.range.bytes = bytes;
        return data;
    }

    void Flag(uint64_t id, uint32_t physical, bool cube, bool set, uint64_t flag)
    {
        const auto mode = std::get<0>(GetParam());
        AclsanDeviceSyncData data{};
        data.header = Memory(id, physical, cube, set).header;
        data.header.size = sizeof(data);
        data.header.sourceKind = ACLSAN_DEVICE_SOURCE_SET_WAIT_FLAG;
        data.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
        data.action = set ? ACLSAN_DEVICE_SYNC_ACTION_SET : ACLSAN_DEVICE_SYNC_ACTION_WAIT;
        data.scope = ACLSAN_DEVICE_SYNC_SCOPE_CORE;
        data.srcPipe = data.dstPipe = data.header.pipeline;
        data.mode = set || mode == 4 ? mode : 0;
        data.objectId = flag;
        if (std::get<1>(GetParam())) {
            data.header.sourceKind = ACLSAN_DEVICE_SOURCE_CROSS_CORE_SYNC;
            data.syncKind = mode == 4 ? ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK : ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE;
            data.scope = ACLSAN_DEVICE_SYNC_SCOPE_DEVICE;
            if (set) {
                data.dstPipe = 0;
            } else {
                data.srcPipe = 0;
                data.mode = 0; // The new WAIT callback does not carry the SET mode.
            }
        }
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_SYNC, &data, reports));
    }

    void Replay(uint64_t writtenBytes, bool matchingFlag = true)
    {
        const auto mode = std::get<0>(GetParam());
        const bool cubeWriter = mode == 4;
        const uint32_t writer = cubeWriter ? 0 : (mode == 0 ? 72 : 18);
        const uint32_t reader = mode == 2 ? 0 : (mode == 0 ? 18 : 19);
        const auto read = Memory(4, reader, mode == 2, false);
        // Deliver the consumer first: callback order must not establish visibility.
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
        Flag(3, reader, mode == 2, false, matchingFlag ? 8 : 9);
        Flag(2, writer, cubeWriter, true, cubeWriter ? 24 : 8);
        if (mode != 4) {
            // Global/group notification requires every active Vector participant.
            Flag(2, mode == 2 ? 19 : reader, false, true, 8);
        }
        if (writtenBytes) {
            const auto write = Memory(1, writer, cubeWriter, true, writtenBytes);
            ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
        }
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    }
};

TEST_P(CrossCoreInitcheckReplayTest, CompleteWriteInitializesConsumerRead)
{
    Replay(128);
    EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    EXPECT_TRUE(reports.empty());
}

TEST_P(CrossCoreInitcheckReplayTest, MissingWriteReportsUninitializedConsumerRead)
{
    Replay(0);
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
    EXPECT_EQ(report.uninitBytes, 128U);
    EXPECT_EQ(report.firstUninitAddress, base);
}

TEST_P(CrossCoreInitcheckReplayTest, PartialWriteReportsUninitializedSuffix)
{
    Replay(64);
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ);
    EXPECT_EQ(report.uninitBytes, 64U);
    EXPECT_EQ(report.firstUninitAddress, base + 64);
}

TEST_P(CrossCoreInitcheckReplayTest, MismatchedFlagRemainsIncomplete)
{
    Replay(128, false);
    EXPECT_FALSE(checker.AnalysisComplete());
    EXPECT_NE(checker.Summary().find("unmatched_cross_core_wait=1"), std::string::npos) << checker.Summary();
    EXPECT_TRUE(reports.empty());
}

INSTANTIATE_TEST_SUITE_P(
    LegacyAndCurrentFormats, CrossCoreInitcheckReplayTest,
    ::testing::Combine(::testing::Values(0U, 1U, 2U, 4U), ::testing::Bool()));

TEST(InitcheckTest, ReportsEachMissingRegisterOncePerUsePoint)
{
    Initcheck checker;
    CheckerReportList reports;
    const uint64_t dependencies =
        (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE) | (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_LOOP3);
    const auto first = Access(7, dependencies);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &first, reports));
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(
        std::get<NpuCheckInitcheckReport>(reports[0]).common.groupId,
        std::get<NpuCheckInitcheckReport>(reports[1]).common.groupId);

    auto splitAccess = first;
    splitAccess.header.serialNo = 1;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &splitAccess, reports));
    EXPECT_EQ(reports.size(), 2U);

    const auto second = Access(8, dependencies);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &second, reports));
    EXPECT_EQ(reports.size(), 4U);
}

TEST(InitcheckTest, ZeroAndRepeatedWritesInitializeRegister)
{
    Initcheck checker;
    CheckerReportList reports;
    const auto zero = State(ACLSAN_DEVICE_REGISTER_MTE2_SOURCE, 0);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &zero, reports));
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &zero, reports));
    const auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_EQ(checker.Stats().registerWrites, 2U);
}

TEST(InitcheckTest, StateIsIsolatedByCoreAndLaunchScope)
{
    Initcheck checker;
    CheckerReportList reports;
    const auto state = State(ACLSAN_DEVICE_REGISTER_MTE2_SOURCE, 3, 1);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &state, reports));
    const auto otherCore = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE, 2);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &otherCore, reports));
    ASSERT_EQ(reports.size(), 1U);

    AclsanSynchronizeData sync{};
    sync.common.result = 0;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    const auto sameCoreNextLaunch = Access(2, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE, 1, 12);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &sameCoreNextLaunch, reports));
    EXPECT_EQ(reports.size(), 2U);
}

TEST(InitcheckTest, LaterWriteStopsFutureReportsWithoutErasingEarlierError)
{
    Initcheck checker;
    CheckerReportList reports;
    const auto before = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &before, reports));
    const auto state = State(ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &state, reports));
    const auto after = Access(2, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &after, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_TRUE(checker.HasErrors());
}

TEST(InitcheckTest, KeepsStateAcrossCallbacksWithinSameLaunch)
{
    Initcheck checker;
    CheckerReportList reports;
    const auto state = State(ACLSAN_DEVICE_REGISTER_MTE2_SOURCE, 7);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &state, reports));

    const auto firstSnapshot = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &firstSnapshot, reports));
    const auto secondSnapshot = Access(2, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &secondSnapshot, reports));

    EXPECT_TRUE(reports.empty());
    EXPECT_EQ(checker.Stats().deviceOperations, 2U);
}

TEST(InitcheckTest, FailedSynchronizationRetainsStateUntilSuccessfulSynchronization)
{
    Initcheck checker;
    CheckerReportList reports;
    const auto state = State(ACLSAN_DEVICE_REGISTER_MTE2_SOURCE, 7);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &state, reports));

    AclsanSynchronizeData failed{};
    failed.common.result = 1;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &failed, reports));
    const auto afterFailure = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &afterFailure, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());

    AclsanSynchronizeData success{};
    success.common.result = 0;
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &success, reports));
    const auto afterSuccess = Access(2, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &afterSuccess, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[0]).registerId, ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
}

TEST(InitcheckTest, DoesNotTreatSetL12DOrUnrelatedStateAsInitialization)
{
    Initcheck checker;
    CheckerReportList reports;
    const auto unrelated = State(ACLSAN_DEVICE_REGISTER_SET_PADDING, 1);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &unrelated, reports));
    const auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));

    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[0]).registerId, ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
}

TEST(InitcheckTest, RejectsLegacyOrInvalidEvents)
{
    Initcheck checker;
    CheckerReportList reports;
    auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    access.header.size = offsetof(AclsanDeviceMemoryAccessData, regDependencyMask0);
    EXPECT_FALSE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    auto state = State(ACLSAN_DEVICE_REGISTER_COUNT);
    EXPECT_FALSE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &state, reports));
    access = Access(2, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_COUNT);
    EXPECT_FALSE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    access = Access(3, 0);
    access.regDependencyMask1 = 1;
    EXPECT_FALSE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST(InitcheckTest, AtomicCtrlInitializationIsIsolatedByLogicalBlockIdentity)
{
    Initcheck checker;
    CheckerReportList reports;
    auto state = State(ACLSAN_DEVICE_REGISTER_CTRL, 64);
    state.header.phyCoreId = 1;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &state, reports));
    auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CTRL);
    access.instructionId = 83;
    access.header.phyCoreId = 1;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    EXPECT_TRUE(reports.empty());
    // 另一个核上的不同执行也对应不同的逻辑 block。
    access.header.phyCoreId = 2;
    access.header.blockId = 1;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports.front());
    EXPECT_EQ(report.registerId, ACLSAN_DEVICE_REGISTER_CTRL);
    EXPECT_EQ(report.registerName, "ctrl");
    EXPECT_EQ(report.common.exec.phyCoreId, 2U);

    // 即使 block 编号相同，只要 type/device/launch 不同，仍然相互独立。
    const auto checkScope = [&](AclsanDeviceMemoryAccessData other) {
        const auto previousSize = reports.size();
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &other, reports));
        ASSERT_EQ(reports.size(), previousSize + 1);
        const auto group = std::get<NpuCheckInitcheckReport>(reports.back()).common.groupId;
        EXPECT_NE(group, std::get<NpuCheckInitcheckReport>(reports[0]).common.groupId);
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &other, reports));
        EXPECT_EQ(reports.size(), previousSize + 1);
    };
    auto other = access;
    other.header.blockId = 0;
    other.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    checkScope(other);
    other = access;
    ++other.header.deviceId;
    checkScope(other);
    other = access;
    ++other.header.launchId;
    checkScope(other);
}

TEST(InitcheckTest, OrdinaryWriteFallbackStillReportsMissingCtrl)
{
    Initcheck checker;
    CheckerReportList reports;
    auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CTRL);
    access.instructionId = 83;
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT | ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.front()).registerId, ACLSAN_DEVICE_REGISTER_CTRL);
    // The access is queued until stream synchronization.
    EXPECT_FALSE(checker.AnalysisComplete());
}

TEST_F(InitcheckHostWriteTest, MemcheckReportedInvalidReadDoesNotPoisonInitcheck)
{
    for (bool freed : {false, true}) {
        Memcheck memcheck;
        Initcheck initcheck;
        CheckerReportList combinedReports;
        auto dispatch = [&](AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data) {
            EXPECT_TRUE(memcheck.OnCallback(domain, cbid, data, combinedReports));
            EXPECT_TRUE(initcheck.OnCallback(domain, cbid, data, combinedReports));
        };
        AclsanResourceData allocation{};
        allocation.common.version = ACLSAN_API_VERSION;
        allocation.common.size = sizeof(allocation);
        allocation.ptr = reinterpret_cast<void*>(base);
        allocation.bytes = 128;
        allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
        allocation.deviceId = 2;
        allocation.resourceId = 1;
        dispatch(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &allocation);
        if (freed)
            dispatch(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE, &allocation);
        auto access = Access(1, 0);
        access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        access.accessCount = 1;
        access.address = base;
        access.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        access.layout.range.bytes = freed ? 128 : 129;
        dispatch(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access);
        AclsanSynchronizeData sync{};
        dispatch(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync);
        ASSERT_EQ(combinedReports.size(), 1U);
        EXPECT_TRUE(std::holds_alternative<NpuCheckMemcheckReport>(combinedReports[0]));
        EXPECT_TRUE(initcheck.AnalysisComplete());
        EXPECT_EQ(initcheck.Stats().malformedEvents, 0U);
        EXPECT_EQ(initcheck.Summary().find("invalid_memory_operation"), std::string::npos);
    }
}

TEST_F(InitcheckHostWriteTest, UnreportedInvalidReadAndLostTraceRemainIncomplete)
{
    for (bool lost : {false, true}) {
        Initcheck initcheck;
        CheckerReportList output;
        auto access = Access(1, 0);
        access.header.flags = lost ? ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR : ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        access.accessCount = 1;
        access.address = base;
        access.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        access.layout.range.bytes = 128;
        ASSERT_TRUE(Dispatch(initcheck, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, output));
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            initcheck.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, output));
        EXPECT_FALSE(initcheck.AnalysisComplete());
        EXPECT_EQ(initcheck.Stats().malformedEvents, 1U);
    }
}

NpuCheckMemcheckReport BoundsReportFor(const AclsanDeviceMemoryAccessData& access)
{
    NpuCheckMemcheckReport report{};
    report.common.tool = ReportTool::MEMCHECK;
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    auto& exec = report.common.exec;
    const auto& h = access.header;
    exec.deviceId = h.deviceId;
    exec.launchId = h.launchId;
    exec.blockType = h.blockType;
    exec.blockId = h.blockId;
    exec.phyCoreId = h.phyCoreId;
    exec.instrExecId = h.instrExecId;
    exec.pipeId = h.pipeline;
    exec.pc = h.pc;
    exec.siteId = h.siteId;
    return report;
}

TEST_F(InitcheckHostWriteTest, BoundsReportDoesNotHideTraceLossMismatchedInstructionOrInvalidWrite)
{
    for (int scenario = 0; scenario < 5; ++scenario) {
        SCOPED_TRACE(scenario);
        Initcheck initcheck;
        auto access = Access(1, 0);
        access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        access.accessCount = 1;
        access.address = base;
        access.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        access.layout.range.bytes = 129;
        auto bounds = BoundsReportFor(access);
        if (scenario == 0)
            access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR;
        else if (scenario == 1)
            ++bounds.common.exec.instrExecId;
        else if (scenario == 2)
            access.accessCount = 2; // Missing operand must remain incomplete.
        else if (scenario == 3)
            access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        else
            ++bounds.common.exec.phyCoreId;
        CheckerReportList output{bounds};
        ASSERT_TRUE(Dispatch(initcheck, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, output));
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            initcheck.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, output));
        EXPECT_FALSE(initcheck.AnalysisComplete());
        EXPECT_EQ(initcheck.Stats().malformedEvents, 1U);
        if (scenario == 3)
            EXPECT_NE(initcheck.Summary().find("invalid_gm_write=1"), std::string::npos);
    }
}

TEST_F(InitcheckHostWriteTest, InvalidInstructionDoesNotInitializeLocalOutputOrSuppressOtherReads)
{
    auto access = Access(1, 0);
    access.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    access.accessCount = 2;
    access.address = base;
    access.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    access.layout.range.bytes = 129;
    reports.emplace_back(BoundsReportFor(access));
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    auto local = access;
    local.accessIndex = 1;
    local.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
    local.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    local.address = 0;
    local.layout.range.bytes = 128;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &local, reports));
    local.header.instrExecId = 2;
    local.accessIndex = 0;
    local.accessCount = 1;
    local.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &local, reports));
    access.accessCount = 1;
    access.header.instrExecId = 3;
    access.layout.range.bytes = 128;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    AclsanSynchronizeData sync{};
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    ASSERT_EQ(reports.size(), 3U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[1]).common.exec.instrExecId, 2U);
    EXPECT_TRUE(std::holds_alternative<NpuCheckInitcheckReport>(reports.back()));
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports.back()).common.exec.instrExecId, 3U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(InitcheckTest, MissingStridePlaceholderReportsRegisterWithoutReplayingUnknownAddress)
{
    Initcheck checker;
    CheckerReportList reports;
    auto placeholder = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_NDDMA_LOOP0_STRIDE);
    placeholder.instructionId = static_cast<uint32_t>(aclsan::InstructionId::NdDmaOutToUbufB8);
    placeholder.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    placeholder.accessCount = 1;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &placeholder, reports));
    AclsanSynchronizeData sync{};
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[0]).registerId, ACLSAN_DEVICE_REGISTER_NDDMA_LOOP0_STRIDE);
    EXPECT_EQ(checker.Stats().errors, 1U);
    EXPECT_EQ(checker.Stats().malformedEvents, 0U);
    EXPECT_EQ(checker.Summary().find("memory_coverage_unknown"), std::string::npos);
    EXPECT_TRUE(checker.AnalysisComplete());
    // Standalone callbacks also close register/report state at synchronization.
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &placeholder, reports));
    EXPECT_EQ(reports.size(), 2U);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    EXPECT_TRUE(checker.AnalysisComplete());
    EXPECT_EQ(checker.Stats().malformedEvents, 0U);
}

TEST(InitcheckTest, MissingGmToL1RegisterPlaceholderDoesNotReplayOrInitializeMemory)
{
    using aclsan::InstructionId;
    for (auto opcode :
         {InstructionId::CopyGmToCbufAlignV2B8, InstructionId::CopyGmToCbufAlignV2B16,
          InstructionId::CopyGmToCbufAlignV2B32}) {
        SCOPED_TRACE(static_cast<uint32_t>(opcode));
        Initcheck checker;
        CheckerReportList reports;
        auto placeholder = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_CBUF);
        placeholder.instructionId = static_cast<uint32_t>(opcode);
        placeholder.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        placeholder.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
        placeholder.accessCount = 1;
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &placeholder, reports));
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(
            std::get<NpuCheckInitcheckReport>(reports[0]).registerId, ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_CBUF);
        EXPECT_EQ(checker.Stats().errors, 1U);
        EXPECT_EQ(checker.Stats().malformedEvents, 0U);
        EXPECT_TRUE(checker.AnalysisComplete());

        // Skipping the unknown transfer must not credit an L1 initialization write.
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &placeholder, reports));
        ASSERT_EQ(reports.size(), 2U); // Synchronization also cleared register-report deduplication.
        auto read = Access(2, 0);
        read.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        read.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        read.accessCount = 1;
        read.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
        read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        read.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        read.layout.range.bytes = 32;
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        ASSERT_EQ(reports.size(), 3U);
        EXPECT_EQ(
            std::get<NpuCheckInitcheckReport>(reports[2]).common.pattern,
            NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
        EXPECT_EQ(checker.Stats().errors, 3U);
        EXPECT_TRUE(checker.AnalysisComplete());
    }
}

TEST(InitcheckTest, GmToL1TraceLossAndPartialAccessesRemainIncomplete)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
        SCOPED_TRACE(scenario);
        Initcheck checker;
        CheckerReportList reports;
        auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_CBUF);
        access.instructionId = static_cast<uint32_t>(aclsan::InstructionId::CopyGmToCbufAlignV2B32);
        access.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
        access.accessCount = 1;
        if (scenario == 0)
            access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR;
        else if (scenario == 1)
            access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED;
        else if (scenario == 2)
            access.accessCount = 2;
        else
            access.regDependencyMask0 = 0;
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        EXPECT_EQ(checker.Stats().malformedEvents, 1U);
        EXPECT_FALSE(checker.AnalysisComplete());
    }
}

TEST(InitcheckTest, OtherEstimatedEventsRemainIncomplete)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
        SCOPED_TRACE(scenario);
        Initcheck checker;
        CheckerReportList reports;
        auto placeholder = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_NDDMA_LOOP0_STRIDE);
        placeholder.instructionId = static_cast<uint32_t>(aclsan::InstructionId::NdDmaOutToUbufB8);
        placeholder.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
        placeholder.accessCount = 1;
        if (scenario == 0)
            placeholder.regDependencyMask0 = 0;
        else if (scenario == 1)
            placeholder.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR;
        else if (scenario == 2)
            placeholder.accessCount = 2;
        else
            placeholder.instructionId = 83; // An unknown destination may write GM.
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &placeholder, reports));
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        EXPECT_EQ(checker.Stats().malformedEvents, 1U);
        EXPECT_FALSE(checker.AnalysisComplete());
    }
}

TEST(InitcheckTest, RejectsUnsupportedApiVersionForMemoryAndRegisterState)
{
    Initcheck checker;
    CheckerReportList reports;
    auto ctrl = State(ACLSAN_DEVICE_REGISTER_CTRL, 64);
    ctrl.header.version = ACLSAN_API_VERSION + 1;
    EXPECT_FALSE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &ctrl, reports));
    auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CTRL);
    access.header.version = ACLSAN_API_VERSION + 1;
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    EXPECT_FALSE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_FALSE(checker.AnalysisComplete());
}

class LocalInitcheckReplayTest : public ::testing::TestWithParam<uint32_t> {
protected:
    Initcheck checker;
    CheckerReportList reports;

    AclsanDeviceMemoryAccessData Memory(uint64_t id, uint32_t mode, uint64_t address = 0, uint64_t bytes = 128)
    {
        auto data = Access(id, 0);
        data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        data.header.pipeline = ACLSAN_DEVICE_PIPE_MATRIX;
        data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        data.memorySpace = GetParam();
        data.accessMode = mode;
        data.accessCount = 1;
        data.address = address;
        data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        data.layout.range.bytes = bytes;
        return data;
    }
    void Add(const AclsanDeviceMemoryAccessData& data)
    {
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &data, reports));
    }
    void Flag(uint64_t id, uint32_t action, uint32_t src, uint32_t dst)
    {
        AclsanDeviceSyncData data{};
        data.header = Memory(id, ACLSAN_DEVICE_MEMORY_ACCESS_READ).header;
        data.header.size = sizeof(data);
        data.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
        data.action = action;
        data.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
        data.srcPipe = src;
        data.dstPipe = dst;
        data.objectId = 3;
        ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_SYNC, &data, reports));
    }
    void Sync()
    {
        AclsanSynchronizeData data{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &data, reports));
    }
};

TEST_P(LocalInitcheckReplayTest, ArrivalOrderDoesNotInitializeEarlierRead)
{
    Add(Memory(3, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Add(Memory(2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE));
    Add(Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Sync();
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.common.exec.instrExecId, 1U);
    EXPECT_EQ(report.uninitBytes, 128U);
}

TEST_P(LocalInitcheckReplayTest, StridedWriteAcrossPagesKeepsHolesUninitialized)
{
    auto write = Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0xfff0);
    write.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    write.layout.blockRepeat = {2, 16, 64, 1, 0, 0};
    Add(write);
    Add(Memory(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0xfff0));
    Sync();
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ);
    EXPECT_EQ(report.uninitBytes, 96U);
    EXPECT_EQ(report.firstUninitAddress, 0x10000U);
}

TEST_P(LocalInitcheckReplayTest, ReadModifyWriteChecksOldBytesBeforeInitializingOutput)
{
    Add(Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD));
    Add(Memory(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Sync();
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.common.exec.instrExecId, 1U);
    EXPECT_EQ(report.uninitBytes, 128U);
}

TEST_P(LocalInitcheckReplayTest, SeparateReadAndWriteOperandsCheckOldBytesBeforeWriting)
{
    auto read = Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    read.accessCount = 2;
    auto write = read;
    write.accessIndex = 1;
    write.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    Add(write);
    Add(read);
    Add(Memory(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Sync();
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    const auto& report = std::get<NpuCheckInitcheckReport>(reports[0]);
    EXPECT_EQ(report.common.exec.instrExecId, 1U);
    EXPECT_EQ(report.uninitBytes, 128U);
}

TEST_P(LocalInitcheckReplayTest, BlocksAndMemorySpacesDoNotShareWrittenBytes)
{
    Add(Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE));
    auto otherSpace = Memory(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    otherSpace.memorySpace =
        GetParam() == ACLSAN_DEVICE_MEMORY_SPACE_L1 ? ACLSAN_DEVICE_MEMORY_SPACE_L0C : ACLSAN_DEVICE_MEMORY_SPACE_L1;
    Add(otherSpace);
    auto otherBlock = Memory(3, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    otherBlock.header.blockId = 1;
    Add(otherBlock);
    Add(Memory(4, ACLSAN_DEVICE_MEMORY_ACCESS_READ));
    Sync();
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 2U);
    for (const auto& item : reports) {
        const auto& report = std::get<NpuCheckInitcheckReport>(item);
        EXPECT_TRUE(report.common.exec.instrExecId == 2 || report.common.exec.instrExecId == 3);
        EXPECT_EQ(report.uninitBytes, 128U);
    }
}

TEST_P(LocalInitcheckReplayTest, NextLaunchStartsUndefinedWhileGmStateSurvives)
{
    AclsanResourceData allocation{};
    allocation.common.version = ACLSAN_API_VERSION;
    allocation.common.size = sizeof(allocation);
    allocation.ptr = reinterpret_cast<void*>(0x100000);
    allocation.bytes = 128;
    allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    allocation.deviceId = 2;
    allocation.resourceId = 1;
    ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &allocation, reports));
    auto gm = Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0x100000);
    gm.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    Add(gm);
    Add(Memory(2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE));
    Sync();
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_TRUE(reports.empty());
    auto localRead = Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    localRead.header.launchId = 12;
    Add(localRead);
    gm.header.instrExecId = 2;
    gm.header.launchId = 12;
    gm.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    Add(gm);
    Sync();
    ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckInitcheckReport>(reports[0]).common.exec.instrExecId, 1U);
}

TEST_P(LocalInitcheckReplayTest, FlagsProveLocalWriteVisibilityAcrossPipelines)
{
    const uint32_t src =
        GetParam() == ACLSAN_DEVICE_MEMORY_SPACE_L1 ? ACLSAN_DEVICE_PIPE_MTE2 : ACLSAN_DEVICE_PIPE_MATRIX;
    const uint32_t dst =
        GetParam() == ACLSAN_DEVICE_MEMORY_SPACE_L1 ? ACLSAN_DEVICE_PIPE_MTE1 : ACLSAN_DEVICE_PIPE_FIXPIPE;
    auto write = Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    write.header.pipeline = src;
    auto read = Memory(4, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    read.header.pipeline = dst;
    // Trace arrival order must not replace the SET/WAIT dependency.
    Add(read);
    Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT, src, dst);
    Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET, src, dst);
    Add(write);
    Sync();
    EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    EXPECT_TRUE(reports.empty());
}

TEST_P(LocalInitcheckReplayTest, UnorderedWriteCannotEstablishInitialization)
{
    auto write = Memory(1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    auto read = Memory(2, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    read.header.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    Add(write);
    Add(read);
    Sync();
    EXPECT_FALSE(checker.AnalysisComplete());
    EXPECT_NE(checker.Summary().find("unordered_initialization=1"), std::string::npos);
    EXPECT_TRUE(reports.empty());
}

INSTANTIATE_TEST_SUITE_P(
    L1AndL0C, LocalInitcheckReplayTest,
    ::testing::Values(uint32_t{ACLSAN_DEVICE_MEMORY_SPACE_L1}, uint32_t{ACLSAN_DEVICE_MEMORY_SPACE_L0C}));

} // namespace
} // namespace npucheck
