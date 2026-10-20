// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "acl_san/aclsan_api.h"
#include "checker/initcheck.h"

#include <gtest/gtest.h>

#include <cstddef>

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
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(InitcheckTest, IgnoresApiVersionForMemoryAndRegisterState)
{
    Initcheck checker;
    CheckerReportList reports;
    auto ctrl = State(ACLSAN_DEVICE_REGISTER_CTRL, 64);
    ctrl.header.version = ACLSAN_API_VERSION + 1;
    ASSERT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_STATE, &ctrl, reports));
    auto access = Access(1, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CTRL);
    access.header.version = ACLSAN_API_VERSION + 1;
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    EXPECT_TRUE(Dispatch(checker, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    EXPECT_TRUE(reports.empty());
    EXPECT_TRUE(checker.AnalysisComplete());
}

} // namespace
} // namespace npucheck
