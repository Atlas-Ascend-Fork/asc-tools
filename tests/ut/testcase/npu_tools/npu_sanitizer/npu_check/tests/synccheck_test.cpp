// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "checker/synccheck.h"

#include <gtest/gtest.h>
#include "acl/acl.h"

namespace npucheck {
namespace {

AclsanDeviceSyncData MakeSyncEvent(
    uint32_t syncKind, uint32_t action, uint64_t objectId = 7, uint32_t dstPipe = ACLSAN_DEVICE_PIPE_MTE2,
    uint32_t mode = 0, uint64_t launchId = 0)
{
    AclsanDeviceSyncData data{};
    data.header.launchId = launchId;
    data.header.phyCoreId = 3;
    data.header.blockId = 5;
    data.syncKind = syncKind;
    data.action = action;
    data.scope = syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG ? ACLSAN_DEVICE_SYNC_SCOPE_BLOCK :
                                                                     ACLSAN_DEVICE_SYNC_SCOPE_PIPE;
    data.srcPipe = syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG ? ACLSAN_DEVICE_PIPE_VECTOR : 0;
    data.dstPipe = dstPipe;
    data.mode = mode;
    data.objectId = objectId;
    return data;
}

TEST(SynccheckTest, TracksFlagAndSyncBufStatesIndependently)
{
    Synccheck synccheck;
    synccheck.OnKernelLaunch(0, {});

    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_SET));
    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_ACTION_GET));
    EXPECT_EQ(synccheck.Stats().pendingOpens, 2U);

    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_WAIT));
    EXPECT_EQ(synccheck.Stats().pendingOpens, 1U);

    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_ACTION_RELEASE));
    const SynccheckStats stats = synccheck.Stats();
    EXPECT_EQ(stats.pendingOpens, 0U);
    EXPECT_EQ(stats.matchedPairs, 2U);
}

TEST(SynccheckTest, SynchronizationReportsPendingFlagAndSyncBuf)
{
    Synccheck synccheck;
    synccheck.OnKernelLaunch(0, {});
    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_SET));
    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_ACTION_GET));

    const auto reports = synccheck.OnSynchronization();

    EXPECT_EQ(reports.size(), 2U);
    const SynccheckStats stats = synccheck.Stats();
    EXPECT_EQ(stats.unconsumedOpens, 2U);
    EXPECT_EQ(stats.pendingOpens, 0U);
}

TEST(SynccheckTest, SynchronizationClearsAllReportedLaunches)
{
    Synccheck synccheck;
    synccheck.OnKernelLaunch(1, {});
    synccheck.OnKernelLaunch(2, {});
    synccheck.OnDeviceSync(MakeSyncEvent(
        ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_SET, 7, ACLSAN_DEVICE_PIPE_MTE2, 0, 1));
    synccheck.OnDeviceSync(MakeSyncEvent(
        ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_SET, 7, ACLSAN_DEVICE_PIPE_MTE2, 0, 2));

    const auto reports = synccheck.OnSynchronization();

    EXPECT_EQ(reports.size(), 2U);
    EXPECT_EQ(synccheck.Stats().unconsumedOpens, 2U);
    EXPECT_EQ(synccheck.Stats().pendingOpens, 0U);
}

TEST(SynccheckTest, DetectsDuplicateGetAcrossDifferentExactKeys)
{
    Synccheck synccheck;
    synccheck.OnKernelLaunch(0, {});
    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_ACTION_GET));

    synccheck.OnDeviceSync(MakeSyncEvent(
        ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_ACTION_GET, 7, ACLSAN_DEVICE_PIPE_MTE3, 1));
    const SynccheckStats stats = synccheck.Stats();
    EXPECT_EQ(stats.duplicateOpens, 1U);
    EXPECT_EQ(stats.pendingOpens, 1U);
}

TEST(SynccheckTest, DefersImmediateMismatchesUntilSynchronization)
{
    Synccheck synccheck;
    synccheck.OnKernelLaunch(0, {});
    synccheck.OnDeviceSync(MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_ACTION_GET));

    synccheck.OnDeviceSync(MakeSyncEvent(
        ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_ACTION_GET, 7, ACLSAN_DEVICE_PIPE_MTE3, 1));

    const auto synchronizationReports = synccheck.OnSynchronization();
    ASSERT_EQ(synchronizationReports.size(), 2U);
    EXPECT_EQ(synccheck.Stats().duplicateOpens, 1U);
    EXPECT_EQ(synccheck.Stats().unconsumedOpens, 1U);
}

TEST(SynccheckTest, PreservesDeviceBlockTypeInReport)
{
    Synccheck synccheck;
    synccheck.OnKernelLaunch(0, {});
    AclsanDeviceSyncData data = MakeSyncEvent(ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    data.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;

    synccheck.OnDeviceSync(data);
    const auto reports = synccheck.OnSynchronization();

    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().triggerPoint.exec.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
}

LaunchTopology MakeTopology(uint32_t kernelType, uint32_t groups = 1, uint16_t aivs = 2)
{
    LaunchTopology topology;
    topology.kernelType = kernelType; // Runtime types: CUBE=1, VECTOR=2, MIX=3.
    topology.kernelTypeValid = true;
    topology.aicRatio = 1;
    topology.aivRatio = aivs;
    topology.ratioValid = true;
    topology.groupCount = groups;
    topology.groupCountValid = true;
    return topology;
}

AclsanDeviceSyncData MakeCrossEvent(
    uint32_t mode, uint32_t action, bool cube, uint32_t block = 0, uint64_t flag = 3, uint64_t launch = 1)
{
    auto data = MakeSyncEvent(
        mode == 4 ? ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK : ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE, action, flag,
        ACLSAN_DEVICE_PIPE_MTE2, mode, launch);
    data.header.blockType = cube ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.blockId = block;
    return data;
}

TEST(SynccheckCrossCoreTest, NormalizesPureAndMixedLaunchTopologies)
{
    for (uint32_t kernelType : {1U, 2U, 3U}) {
        for (uint16_t aivs : {1, 2}) {
            Synccheck checker;
            checker.OnKernelLaunch(1, MakeTopology(kernelType, 3, aivs));
            const auto topology = checker.TopologyForLaunch(1);
            ASSERT_TRUE(topology.has_value());
            EXPECT_EQ(topology->expectedAicCount, kernelType == 2 ? 0U : 3U);
            EXPECT_EQ(topology->expectedAivCount, kernelType == 1 ? 0U : kernelType == 2 ? 3U : 3U * aivs);
        }
    }
}

TEST(SynccheckCrossCoreTest, ModeZeroRequiresAllPeersAndDefersEarlyWaits)
{
    for (bool cube : {false, true}) {
        Synccheck checker;
        checker.OnKernelLaunch(1, MakeTopology(cube ? 1 : 2, 2));
        checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_WAIT, cube, 0));
        checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, cube, 0));
        EXPECT_EQ(checker.Stats().producedCrossTokens, 0U);
        EXPECT_EQ(checker.Stats().waitWithoutSyncTokens, 0U);
        checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, cube, 1));
        EXPECT_EQ(checker.Stats().producedCrossTokens, 2U);
        EXPECT_EQ(checker.Stats().consumedCrossTokens, 1U);
        checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_WAIT, cube, 1));
        EXPECT_TRUE(checker.OnSynchronization().empty());
        EXPECT_EQ(checker.Stats().consumedCrossTokens, 2U);
    }
}

TEST(SynccheckCrossCoreTest, ModeOneAggregatesOnlyVectorsInTheSameGroup)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(3, 2));
    checker.OnDeviceSync(MakeCrossEvent(1, ACLSAN_DEVICE_SYNC_ACTION_SET, false, 2));
    EXPECT_EQ(checker.Stats().producedCrossTokens, 0U);
    checker.OnDeviceSync(MakeCrossEvent(1, ACLSAN_DEVICE_SYNC_ACTION_SET, false, 3));
    EXPECT_EQ(checker.CrossCounterValue(1, 1, ParticipantRole::AIV0, 3), 1U);
    EXPECT_EQ(checker.CrossCounterValue(1, 1, ParticipantRole::AIV1, 3), 1U);
    checker.OnDeviceSync(MakeCrossEvent(1, ACLSAN_DEVICE_SYNC_ACTION_WAIT, false, 2));
    checker.OnDeviceSync(MakeCrossEvent(1, ACLSAN_DEVICE_SYNC_ACTION_WAIT, false, 3));
    EXPECT_TRUE(checker.OnSynchronization().empty());
}

TEST(SynccheckCrossCoreTest, ModeTwoSupportsBothMixedRatiosAndBothDirections)
{
    for (uint16_t aivs : {1, 2}) {
        Synccheck checker;
        checker.OnKernelLaunch(1, MakeTopology(3, 1, aivs));
        checker.OnDeviceSync(MakeCrossEvent(2, ACLSAN_DEVICE_SYNC_ACTION_WAIT, true));
        for (uint32_t block = 0; block < aivs; ++block) {
            checker.OnDeviceSync(MakeCrossEvent(2, ACLSAN_DEVICE_SYNC_ACTION_WAIT, false, block));
            checker.OnDeviceSync(MakeCrossEvent(2, ACLSAN_DEVICE_SYNC_ACTION_SET, false, block));
        }
        checker.OnDeviceSync(MakeCrossEvent(2, ACLSAN_DEVICE_SYNC_ACTION_SET, true));
        EXPECT_EQ(checker.Stats().consumedCrossTokens, 1U + aivs);
        EXPECT_TRUE(checker.OnSynchronization().empty());
    }
}

TEST(SynccheckCrossCoreTest, ModeFourRoutesSelectedVectorAndItsCubeCounterBank)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(3));
    for (uint32_t block : {0U, 1U}) {
        const uint64_t cubeFlag = 3 + 16 * block;
        checker.OnDeviceSync(MakeCrossEvent(4, ACLSAN_DEVICE_SYNC_ACTION_SET, true, 0, cubeFlag));
        EXPECT_EQ(checker.CrossCounterValue(1, 0, block ? ParticipantRole::AIV1 : ParticipantRole::AIV0, 3), 1U);
        checker.OnDeviceSync(MakeCrossEvent(4, ACLSAN_DEVICE_SYNC_ACTION_WAIT, false, block));
        checker.OnDeviceSync(MakeCrossEvent(4, ACLSAN_DEVICE_SYNC_ACTION_SET, false, block));
        EXPECT_EQ(checker.CrossCounterValue(1, 0, ParticipantRole::AIC, cubeFlag), 1U);
        checker.OnDeviceSync(MakeCrossEvent(4, ACLSAN_DEVICE_SYNC_ACTION_WAIT, true, 0, cubeFlag));
    }
    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().consumedCrossTokens, 4U);
}

TEST(SynccheckCrossCoreTest, CounterSaturatesAtFifteenAndReportsOverflow)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(1));
    for (unsigned i = 0; i < 16; ++i) {
        checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true));
    }
    EXPECT_EQ(checker.CrossCounterValue(1, 0, ParticipantRole::AIC, 3), 15U);
    EXPECT_EQ(checker.Stats().counterOverflows, 1U);
    for (unsigned i = 0; i < 15; ++i) {
        checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_WAIT, true));
    }
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
    EXPECT_EQ(checker.Stats().unconsumedSyncTokens, 0U);
}

TEST(SynccheckCrossCoreTest, UnconsumedCountsAreAggregatedIntoOneReportPerCounter)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(1));
    for (unsigned i = 0; i < 3; ++i) {
        checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true));
    }
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
    EXPECT_EQ(checker.Stats().unconsumedSyncTokens, 3U);
    EXPECT_EQ(checker.Stats().unconsumedSyncTokenReports, 1U);
}

TEST(SynccheckCrossCoreTest, CrossCoreWaitCannotConsumeAnIntraBlockToken)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(3));
    checker.OnDeviceSync(MakeCrossEvent(4, ACLSAN_DEVICE_SYNC_ACTION_SET, true));
    checker.OnDeviceSync(MakeCrossEvent(2, ACLSAN_DEVICE_SYNC_ACTION_WAIT, false));
    EXPECT_EQ(checker.Stats().consumedCrossTokens, 0U);
    EXPECT_EQ(checker.OnSynchronization().size(), 2U);
    EXPECT_EQ(checker.Stats().unconsumedSyncTokens, 1U);
    EXPECT_EQ(checker.Stats().waitWithoutSyncTokens, 1U);
}

TEST(SynccheckCrossCoreTest, IncompleteRoundReportsMissingParticipantsWithoutProducingTokens)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(3));
    checker.OnDeviceSync(MakeCrossEvent(2, ACLSAN_DEVICE_SYNC_ACTION_SET, false));
    EXPECT_EQ(checker.PendingCrossSetRoundCount(1), 1U);
    EXPECT_EQ(checker.Stats().producedCrossTokens, 0U);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
    EXPECT_EQ(checker.Stats().participantMismatches, 1U);
}

TEST(SynccheckCrossCoreTest, UnfulfilledWaitIsReportedOnlyAtSynchronization)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(1));
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_WAIT, true));
    EXPECT_EQ(checker.Stats().waitWithoutSyncTokens, 0U);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
    EXPECT_EQ(checker.Stats().waitWithoutSyncTokens, 1U);
    EXPECT_TRUE(checker.OnSynchronization().empty());
}

TEST(SynccheckCrossCoreTest, StreamSynchronizationRetainsOtherLaunchAndRejectsLateEvents)
{
    Synccheck checker;
    int firstStream = 0;
    int secondStream = 0;
    checker.OnKernelLaunch(1, MakeTopology(1), &firstStream);
    checker.OnKernelLaunch(2, MakeTopology(1), &secondStream);
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true, 0, 3, 1));
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true, 0, 3, 2));
    EXPECT_EQ(checker.OnSynchronization(&firstStream).size(), 1U);
    EXPECT_FALSE(checker.TopologyForLaunch(1).has_value());
    EXPECT_TRUE(checker.TopologyForLaunch(2).has_value());
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_WAIT, true, 0, 3, 2));
    EXPECT_TRUE(checker.OnSynchronization(&secondStream).empty());
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true, 0, 3, 1));
    EXPECT_EQ(checker.Stats().invalidArguments, 1U);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
}

TEST(SynccheckCrossCoreTest, RejectsUnknownLaunchAndUnsupportedTopology)
{
    Synccheck checker;
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true));
    checker.OnKernelLaunch(2, {});
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true, 0, 3, 2));
    checker.OnKernelLaunch(3, MakeTopology(3, 1, 1));
    checker.OnDeviceSync(MakeCrossEvent(4, ACLSAN_DEVICE_SYNC_ACTION_SET, true, 0, 3, 3));
    EXPECT_EQ(checker.Stats().invalidArguments, 3U);
    EXPECT_EQ(checker.OnSynchronization().size(), 3U);
}

TEST(SynccheckCrossCoreTest, SynchronizationCallbackFinalizesPendingLaunchOnTimeout)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(1));
    checker.OnDeviceSync(MakeCrossEvent(0, ACLSAN_DEVICE_SYNC_ACTION_SET, true));
    AclsanSynchronizeData synchronization{};
    synchronization.common.result = ACL_ERROR_RT_STREAM_SYNC_TIMEOUT;
    CheckerReportList reports;
    EXPECT_TRUE(checker.OnCallback(
        ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &synchronization, reports));
    EXPECT_EQ(reports.size(), 1U);
    EXPECT_FALSE(checker.TopologyForLaunch(1).has_value());
    EXPECT_EQ(checker.Stats().unconsumedSyncTokens, 1U);
    EXPECT_EQ(checker.Stats().invalidArguments, 0U);
}

TEST(SynccheckCrossCoreTest, AnalysisCompleteTracksPendingFlagOperations)
{
    Synccheck checker;
    checker.OnKernelLaunch(1, MakeTopology(3));
    EXPECT_TRUE(checker.AnalysisComplete());
    checker.OnDeviceSync(MakeSyncEvent(
        ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_SET, 7, ACLSAN_DEVICE_PIPE_MTE2, 0, 1));
    EXPECT_FALSE(checker.AnalysisComplete());
    checker.OnDeviceSync(MakeSyncEvent(
        ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG, ACLSAN_DEVICE_SYNC_ACTION_WAIT, 7, ACLSAN_DEVICE_PIPE_MTE2, 0, 1));
    EXPECT_TRUE(checker.AnalysisComplete());
}

} // namespace
} // namespace npucheck
