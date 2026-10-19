/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/racecheck.h"
#include "diagnostic/report_renderer.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <random>

namespace npucheck {
namespace {
AclsanDeviceMemoryAccessData Memory(
    uint64_t n, uint32_t mode = 2, uint32_t pipe = ACLSAN_DEVICE_PIPE_MTE2, uint64_t address = 0x1000,
    uint64_t bytes = 8)
{
    AclsanDeviceMemoryAccessData a{};
    a.header.version = ACLSAN_API_VERSION;
    a.header.size = sizeof(a);
    a.header.launchId = 1;
    a.header.instrExecId = n;
    a.header.pc = 0x100 + n;
    a.header.pipeline = pipe;
    a.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    a.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    a.address = address;
    a.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    a.accessMode = mode;
    a.accessCount = 1;
    a.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    a.layout.range.bytes = bytes;
    return a;
}
AclsanDeviceSyncData Flag(
    uint64_t n, uint32_t action, uint32_t src = ACLSAN_DEVICE_PIPE_MTE2, uint32_t dst = ACLSAN_DEVICE_PIPE_VECTOR,
    uint64_t token = 0)
{
    AclsanDeviceSyncData s{};
    s.header = Memory(n).header;
    s.header.size = sizeof(s);
    s.header.flags = 0;
    s.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
    s.action = action;
    s.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
    s.srcPipe = src;
    s.dstPipe = dst;
    s.objectId = token;
    return s;
}
AclsanDeviceBarrierData Barrier(uint64_t n, uint32_t pipe)
{
    AclsanDeviceBarrierData b{};
    b.header = Memory(n).header;
    b.header.size = sizeof(b);
    b.header.flags = ACLSAN_DEVICE_EVENT_FLAG_NONE;
    b.pipe = pipe;
    return b;
}
struct Harness {
    Racechecker checker;
    CheckerReportList reports;
    explicit Harness(race::Limits limits = {}) : checker(limits) { Launch(1); }
    void Launch(uint64_t id, void* stream = nullptr)
    {
        AclsanLaunchData l{};
        l.common.version = ACLSAN_API_VERSION;
        l.common.size = sizeof(l);
        l.launchId = id;
        l.stream = stream;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &l, reports));
    }
    void Add(const AclsanDeviceMemoryAccessData& a)
    {
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &a, reports));
    }
    void Add(const AclsanDeviceSyncData& a)
    {
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &a, reports));
    }
    void Add(const AclsanDeviceBarrierData& a)
    {
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER, &a, reports));
    }
    void Sync(void* stream = nullptr, int result = 0)
    {
        AclsanSynchronizeData s{};
        s.common.version = ACLSAN_API_VERSION;
        s.common.size = sizeof(s);
        s.common.result = result;
        s.stream = stream;
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &s, reports));
    }
};

AclsanDeviceMemoryAccessData UnitMemory(
    uint64_t seq, bool write, uint32_t flag, uint64_t address = 0, uint64_t bytes = 512)
{
    auto a = Memory(seq, write ? 2 : 1, write ? ACLSAN_DEVICE_PIPE_MATRIX : ACLSAN_DEVICE_PIPE_FIXPIPE, address, bytes);
    a.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    a.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L0C;
    a.unitFlag = flag;
    return a;
}

TEST(RacecheckTest, CompletionOrderedPipelinesAndIssueOnlyPipelines)
{
    for (auto pipe :
         {ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_PIPE_MTE1,
          ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_PIPE_FIXPIPE}) {
        Harness h;
        auto a = UnitMemory(1, true, 0);
        a.header.pipeline = pipe;
        h.Add(a);
        a.header.instrExecId = 2;
        h.Add(a);
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete());
        const bool ordered =
            pipe == ACLSAN_DEVICE_PIPE_SCALAR || pipe == ACLSAN_DEVICE_PIPE_VECTOR || pipe == ACLSAN_DEVICE_PIPE_MATRIX;
        EXPECT_EQ(h.reports.empty(), ordered) << pipe;
    }
}

TEST(RacecheckTest, UnitFlagAccumulationMultipleReadersAndReuse)
{
    Harness h;
    // Arrival order must not determine the unit generation.
    h.Add(UnitMemory(7, true, 2));
    h.Add(UnitMemory(6, false, 3));
    h.Add(UnitMemory(5, false, 2));
    h.Add(UnitMemory(4, false, 2));
    h.Add(UnitMemory(3, true, 3));
    h.Add(UnitMemory(2, true, 2));
    h.Add(UnitMemory(1, true, 2));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagPartialDrainAndIndependentBlocks)
{
    Harness h;
    h.Add(UnitMemory(1, true, 3, 0, 1024));
    h.Add(UnitMemory(2, false, 3, 0));
    h.Add(UnitMemory(3, true, 3, 0));
    h.Add(UnitMemory(4, false, 3, 512));
    h.Add(UnitMemory(5, false, 3, 0));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagDoesNotProtectDisabledRead)
{
    Harness h;
    h.Add(UnitMemory(1, true, 3));
    h.Add(UnitMemory(2, false, 0));
    h.Add(UnitMemory(3, false, 3));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete());
    EXPECT_FALSE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagKeepDoesNotPublishAndMissingReleaseIsIncomplete)
{
    for (bool missingPublish : {false, true}) {
        Harness h;
        h.Add(UnitMemory(1, true, missingPublish ? 2 : 3));
        h.Add(UnitMemory(2, false, 2));
        h.Add(UnitMemory(3, true, 2));
        h.Sync();
        EXPECT_FALSE(h.checker.AnalysisComplete());
        EXPECT_TRUE(h.reports.empty());
    }
}

TEST(RacecheckTest, UnitFlagKeepsBytePrecisionWithinOneUnit)
{
    Harness h;
    auto write = UnitMemory(1, true, 3, 0, 32);
    auto second = write;
    write.accessCount = second.accessCount = 2;
    second.accessIndex = 1;
    second.address = 64;
    h.Add(write);
    h.Add(second);
    h.Add(UnitMemory(2, false, 3, 64, 32));
    h.Add(UnitMemory(3, true, 2, 0, 96));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagBarriersCoverAllInstructionFragments)
{
    Harness h;
    h.Add(UnitMemory(1, true, 3, 0, 1024));
    auto read = UnitMemory(2, false, 3, 0, 1024);
    read.accessCount = 2;
    auto output = read;
    output.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    output.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    output.accessIndex = 1;
    h.Add(read);
    h.Add(output);
    auto set = Flag(3, 1, ACLSAN_DEVICE_PIPE_FIXPIPE, ACLSAN_DEVICE_PIPE_MTE2);
    set.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    auto wait = set;
    wait.header.instrExecId = 4;
    wait.action = ACLSAN_DEVICE_SYNC_ACTION_WAIT;
    h.Add(set);
    h.Add(wait);
    output.header.instrExecId = 5;
    output.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    output.accessCount = 1;
    output.accessIndex = 0;
    output.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    output.unitFlag = 0;
    h.Add(output);
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagExpansionHonorsBudget)
{
    race::Limits limits;
    limits.edges = 4;
    Harness h(limits);
    h.Add(UnitMemory(1, true, 3, 0, 512 * 64));
    h.Add(UnitMemory(2, false, 3, 0, 512 * 64));
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagSeparateAccumulatorReadAndWriteShareOneTransition)
{
    Harness h;
    auto read = UnitMemory(1, true, 3);
    read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    read.accessCount = 2;
    auto write = read;
    write.accessIndex = 1;
    write.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    h.Add(read);
    h.Add(write);
    h.Add(UnitMemory(2, false, 3));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagLargeIdenticalTileHistoryIsCompact)
{
    race::Limits limits;
    limits.edges = 64;
    Harness h(limits);
    h.Add(UnitMemory(1, true, 3, 0, 128 * 1024));
    h.Add(UnitMemory(2, false, 3, 0, 128 * 1024));
    h.Add(UnitMemory(3, true, 2, 0, 128 * 1024));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagCannotCrossPhysicalCores)
{
    Harness h;
    h.Add(UnitMemory(1, true, 3));
    auto read = UnitMemory(2, false, 3);
    read.header.phyCoreId = 1;
    h.Add(read);
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
}

TEST(RacecheckTest, UnitFlagDoesNotOrderFixpipeGmOutputs)
{
    Harness h;
    h.Add(UnitMemory(1, true, 3));
    for (uint64_t i : {2, 3}) {
        auto read = UnitMemory(i, false, i == 2 ? 2 : 3);
        read.accessCount = 2;
        auto output = read;
        output.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        output.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        output.accessIndex = 1;
        h.Add(read);
        h.Add(output);
    }
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_FALSE(h.reports.empty());
}

TEST(RacecheckTest, DifferentEventIdsDoNotPair)
{
    Harness h;
    h.Add(Memory(1, 2, ACLSAN_DEVICE_PIPE_MTE2, 0x1000));
    h.Add(Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_VECTOR, 0));
    h.Add(Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_VECTOR, 1));
    h.Add(Memory(4, 1, ACLSAN_DEVICE_PIPE_VECTOR, 0x1000));
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, UnitFlagDoesNotBecomeAnOrdinaryFlagOrAnUnprovenRace)
{
    Harness h;
    auto write = Memory(1, 2, ACLSAN_DEVICE_PIPE_MATRIX, 0);
    write.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    write.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L0C;
    auto read = write;
    read.header.instrExecId = 2;
    read.header.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    read.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    h.Add(write);
    h.Add(read);
    auto sync = Flag(1, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_MATRIX, ACLSAN_DEVICE_PIPE_FIXPIPE);
    sync.header = write.header;
    sync.header.size = sizeof(sync);
    sync.scope = ACLSAN_DEVICE_SYNC_SCOPE_PIPE;
    sync.mode = 2;
    h.Add(sync);
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_TRUE(h.reports.empty());
    EXPECT_NE(h.checker.Summary().find("unit_flag_semantics_unknown=1"), std::string::npos);
    EXPECT_EQ(h.checker.Summary().find("conflicting_duplicate"), std::string::npos);
}

TEST(RacecheckTest, BarrierOrdersLaterSamePipeAccess)
{
    Harness h;
    h.Add(Memory(1, 2, ACLSAN_DEVICE_PIPE_MTE2, 0x1000));
    h.Add(Barrier(2, ACLSAN_DEVICE_PIPE_MTE2));
    h.Add(Memory(3, 2, ACLSAN_DEVICE_PIPE_MTE2, 0x1000));
    h.Sync();
    EXPECT_TRUE(h.reports.empty());
    EXPECT_TRUE(h.checker.AnalysisComplete());
}

// Intentionally unrelated logical block IDs ensure matching uses physical groups.
AclsanDeviceMemoryAccessData CrossMemory(uint64_t seq, uint32_t physical, bool cube, bool write, uint64_t addr = 0x2000)
{
    auto a = Memory(seq, write ? 2U : 1U, write ? ACLSAN_DEVICE_PIPE_MTE3 : ACLSAN_DEVICE_PIPE_MTE2, addr);
    a.header.blockType = cube ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    a.header.phyCoreId = physical;
    a.header.blockId = 100U + physical;
    a.header.pipeline = cube && write ? ACLSAN_DEVICE_PIPE_FIXPIPE : a.header.pipeline;
    a.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    return a;
}
AclsanDeviceSyncData CrossSync(uint64_t seq, uint32_t physical, bool cube, bool set, uint32_t mode, uint64_t flag = 8)
{
    auto s = Flag(seq, set ? ACLSAN_DEVICE_SYNC_ACTION_SET : ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    s.header = CrossMemory(seq, physical, cube, set).header;
    s.header.size = sizeof(s);
    s.scope = ACLSAN_DEVICE_SYNC_SCOPE_CORE;
    s.mode = set || mode == 4 ? mode : 0; // WAIT_FLAG_DEV has no mode operand.
    s.srcPipe = s.dstPipe = s.header.pipeline;
    s.objectId = flag;
    return s;
}

TEST(RacecheckTest, CrossMode1AndGlobalMode0KeepPhysicalGroupsDistinct)
{
    for (uint32_t mode : {0U, 1U}) {
        Harness h;
        h.Add(CrossMemory(1, 18, false, true));
        for (uint32_t physical : {18U, 19U, 72U, 73U}) {
            h.Add(CrossSync(2, physical, false, true, mode));
            h.Add(CrossSync(3, physical, false, false, mode));
        }
        h.Add(CrossMemory(4, 73, false, false));
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.size(), mode == 0 ? 0U : 1U);
    }
}

TEST(RacecheckTest, CrossMode1OrdersBothSubblocksOnEitherDie)
{
    for (uint32_t first : {18U, 20U, 72U}) {
        Harness h;
        h.Add(CrossMemory(1, first, false, true));
        h.Add(CrossSync(2, first, false, true, 1));
        h.Add(CrossSync(2, first + 1, false, true, 1));
        h.Add(CrossSync(3, first, false, false, 1));
        h.Add(CrossSync(3, first + 1, false, false, 1));
        h.Add(CrossMemory(4, first + 1, false, false));
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_TRUE(h.reports.empty());
    }
}

TEST(RacecheckTest, CrossMode4RoutesBothSubblocksInBothDirectionsAndReusesFlags)
{
    for (uint32_t die : {0U, 1U})
        for (uint32_t sub : {0U, 1U})
            for (bool reverse : {false, true}) {
                Harness h;
                const uint32_t c = die * 54U, v = c + 18U + sub;
                for (uint64_t round = 0; round < 4; ++round) {
                    const uint64_t seq = round * 4, addr = 0x2000 + round * 512;
                    h.Add(CrossMemory(seq + 1, reverse ? v : c, !reverse, true, addr));
                    h.Add(CrossSync(seq + 2, reverse ? v : c, !reverse, true, 4, reverse ? 8 : 8 + sub * 16));
                    h.Add(CrossSync(seq + 3, reverse ? c : v, reverse, false, 4, reverse ? 8 + sub * 16 : 8));
                    h.Add(CrossMemory(seq + 4, reverse ? c : v, reverse, false, addr));
                }
                h.Sync();
                EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
                EXPECT_TRUE(h.reports.empty());
            }
}

TEST(RacecheckTest, CrossMode4DoesNotOrderUnselectedSubblock)
{
    Harness h;
    h.Add(CrossMemory(1, 0, true, true));
    h.Add(CrossSync(2, 0, true, true, 4, 24));
    h.Add(CrossSync(3, 19, false, false, 4, 8));
    h.Add(CrossMemory(4, 18, false, false));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_EQ(h.reports.size(), 1U);
}

TEST(RacecheckTest, CrossMode4WrongGroupFlagOrSubblockCannotMatch)
{
    for (uint32_t receiver : {18U, 73U}) {
        Harness h;
        h.Add(CrossMemory(1, 0, true, true));
        h.Add(CrossSync(2, 0, true, true, 4, 24));
        h.Add(CrossSync(3, receiver, false, false, 4, 8));
        h.Add(CrossMemory(4, receiver, false, false));
        h.Sync();
        EXPECT_FALSE(h.checker.AnalysisComplete());
        EXPECT_NE(h.checker.Summary().find("unmatched_cross_core_wait"), std::string::npos);
        EXPECT_TRUE(h.reports.empty());
    }
}

TEST(RacecheckTest, CrossMode2RequiresBothVectorParticipantsForCubeWait)
{
    for (bool missing : {false, true}) {
        Harness h;
        h.Add(CrossMemory(1, 18, false, true));
        h.Add(CrossMemory(1, 19, false, false, 0x9000));
        h.Add(CrossSync(2, 18, false, true, 2));
        if (!missing)
            h.Add(CrossSync(2, 19, false, true, 2));
        h.Add(CrossSync(3, 0, true, false, 2));
        h.Add(CrossMemory(4, 0, true, false));
        h.Sync();
        EXPECT_EQ(h.checker.AnalysisComplete(), !missing) << h.checker.Summary();
        EXPECT_TRUE(h.reports.empty());
    }
}

TEST(RacecheckTest, CrossMode1MissingLaterWaveCannotBorrowAnotherGroupSet)
{
    Harness h;
    for (uint32_t physical : {18U, 19U, 72U, 73U}) {
        h.Add(CrossSync(1, physical, false, true, 1));
        h.Add(CrossSync(2, physical, false, false, 1));
        if (physical != 19)
            h.Add(CrossSync(3, physical, false, true, 1));
        h.Add(CrossSync(4, physical, false, false, 1));
    }
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("unmatched_cross_core_wait"), std::string::npos);
}

TEST(RacecheckTest, CrossMode2And4DoNotJoinIndependentGroups)
{
    for (uint32_t mode : {2U, 4U}) {
        Harness h;
        h.Add(CrossMemory(1, 0, true, true));
        for (uint32_t c : {0U, 54U}) {
            h.Add(CrossSync(2, c, true, true, mode, mode == 4 ? 24 : 8));
            if (mode == 2)
                h.Add(CrossSync(3, c + 18, false, false, mode));
            h.Add(CrossSync(3, c + 19, false, false, mode));
        }
        h.Add(CrossMemory(4, 73, false, false));
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.size(), 1U);
    }
}

TEST(RacecheckTest, CrossCounterMayRetainUnusedNotificationsPerReceiver)
{
    Harness h;
    h.Add(CrossSync(1, 0, true, true, 4));
    h.Add(CrossMemory(2, 18, false, false)); // This receiver never waits.
    h.Add(CrossSync(1, 54, true, true, 4));
    h.Add(CrossSync(2, 72, false, false, 4));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
}

TEST(RacecheckTest, CrossIndependentGroupsMayUseSameFlagWithDifferentModes)
{
    Harness h;
    h.Add(CrossMemory(3, 18, false, false));
    for (uint32_t v : {18U, 19U}) {
        h.Add(CrossSync(1, v, false, true, 1));
        h.Add(CrossSync(2, v, false, false, 1));
    }
    h.Add(CrossSync(1, 54, true, true, 2));
    h.Add(CrossSync(2, 72, false, false, 2));
    h.Add(CrossSync(2, 73, false, false, 2));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
}

TEST(RacecheckTest, CrossIndependentNotificationSourcesRemainAmbiguous)
{
    Harness h;
    for (uint32_t v : {18U, 19U}) {
        h.Add(CrossSync(1, v, false, true, 1));
        h.Add(CrossSync(2, v, false, false, 1));
        h.Add(CrossSync(4, v, false, false, 2));
    }
    h.Add(CrossSync(3, 0, true, true, 2));
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("ambiguous_cross_core_notification_order"), std::string::npos);
}

TEST(RacecheckTest, CrossCounterTwoSetsOneOrTwoWaitsProtectOnlyConsumedPrefix)
{
    for (bool safe : {false, true}) {
        Harness h;
        h.Add(CrossMemory(1, 0, true, true, 0x2000));
        h.Add(CrossSync(2, 0, true, true, 4));
        h.Add(CrossMemory(3, 0, true, true, 0x4000));
        h.Add(CrossSync(4, 0, true, true, 4));
        h.Add(CrossSync(1, 18, false, false, 4));
        if (safe)
            h.Add(CrossSync(2, 18, false, false, 4));
        h.Add(CrossMemory(3, 18, false, false, 0x4000));
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.size(), safe ? 0U : 1U);
    }
}

TEST(RacecheckTest, CrossCounterMode1ThenMode0WithAccumulatedNotifications)
{
    for (bool safe : {false, true}) {
        Harness h;
        // Same senders, same flag, two modes; no drain between SETs.
        for (uint32_t v : {18U, 19U}) {
            h.Add(CrossSync(1, v, false, true, 1));
            if (v == 18)
                h.Add(CrossMemory(2, v, false, true));
            h.Add(CrossSync(3, v, false, true, 0));
            h.Add(CrossSync(4, v, false, false, 0));
            if (safe)
                h.Add(CrossSync(5, v, false, false, 0));
        }
        h.Add(CrossMemory(6, 19, false, false));
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.size(), safe ? 0U : 1U);
    }
}

TEST(RacecheckTest, CrossCounterModeSwitchResolvedByHandshakeOnAnotherFlag)
{
    Harness h;
    h.Add(CrossMemory(1, 0, true, true));
    for (uint32_t v : {18U, 19U}) {
        h.Add(CrossSync(1, v, false, true, 1));
        h.Add(CrossSync(2, v, false, false, 1));
        h.Add(CrossSync(3, v, false, true, 2, 9));
        h.Add(CrossSync(4, v, false, false, 2));
        h.Add(CrossMemory(5, v, false, false));
    }
    h.Add(CrossSync(2, 0, true, false, 2, 9));
    h.Add(CrossSync(3, 0, true, true, 2));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, CrossSetScalarOrderSpansDifferentPipesAndFlags)
{
    Harness h;
    h.Add(CrossMemory(1, 18, false, true));
    h.Add(CrossSync(2, 18, false, true, 4, 9));
    auto later = CrossSync(3, 18, false, true, 4, 8);
    later.srcPipe = later.dstPipe = ACLSAN_DEVICE_PIPE_VECTOR;
    h.Add(later);
    h.Add(CrossSync(1, 0, true, false, 4, 8));
    h.Add(CrossMemory(2, 0, true, false));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, CrossSetInheritsExplicitSynchronizationIntoScalar)
{
    Harness h;
    h.Add(CrossMemory(1, 18, false, true));
    auto set = Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_PIPE_SCALAR);
    set.header = CrossMemory(2, 18, false, false).header;
    set.header.size = sizeof(set);
    auto wait = set;
    wait.header.instrExecId = 3;
    wait.action = ACLSAN_DEVICE_SYNC_ACTION_WAIT;
    h.Add(set);
    h.Add(wait);
    auto cross = CrossSync(4, 18, false, true, 4);
    cross.srcPipe = cross.dstPipe = ACLSAN_DEVICE_PIPE_VECTOR;
    h.Add(cross);
    h.Add(CrossSync(1, 0, true, false, 4));
    h.Add(CrossMemory(2, 0, true, false));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(RacecheckTest, CrossSetSelectedPipeDoesNotPublishOtherMemoryPipelines)
{
    for (bool correctPipe : {false, true}) {
        Harness h;
        auto write = CrossMemory(1, 18, false, true);
        h.Add(write);
        auto set = CrossSync(2, 18, false, true, 4);
        if (!correctPipe)
            set.srcPipe = set.dstPipe = ACLSAN_DEVICE_PIPE_VECTOR;
        h.Add(set);
        h.Add(CrossSync(1, 0, true, false, 4));
        h.Add(CrossMemory(2, 0, true, false));
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.size(), correctPipe ? 0U : 1U);
    }
}

TEST(RacecheckTest, RawWarWawAndReadRead)
{
    for (uint32_t first : {1U, 2U})
        for (uint32_t second : {1U, 2U}) {
            Harness h;
            h.Add(Memory(1, first));
            h.Add(Memory(2, second, ACLSAN_DEVICE_PIPE_VECTOR));
            h.Sync();
            ASSERT_TRUE(h.checker.AnalysisComplete());
            if (first == 1 && second == 1) {
                EXPECT_TRUE(h.reports.empty());
                continue;
            }
            ASSERT_EQ(h.reports.size(), 1U);
            auto r = std::get<NpuCheckRacecheckReport>(h.reports[0]);
            EXPECT_EQ(r.hazardKind, first == 1 ? 2U : second == 1 ? 1U : 3U);
            EXPECT_EQ(r.overlapBytes, 8U);
            EXPECT_TRUE(h.checker.HasErrors());
        }
}
TEST(RacecheckTest, FlagOnlyOrdersSourcePrefixAndDestinationSuffix)
{
    for (bool afterSet : {false, true}) {
        Harness h;
        if (!afterSet)
            h.Add(Memory(1));
        h.Add(Flag(2, 1));
        if (afterSet)
            h.Add(Memory(3));
        if (!afterSet)
            h.Add(Memory(4, 1, ACLSAN_DEVICE_PIPE_VECTOR));
        h.Add(Flag(5, 2));
        if (afterSet)
            h.Add(Memory(6, 1, ACLSAN_DEVICE_PIPE_VECTOR));
        h.Sync();
        ASSERT_EQ(h.reports.size(), 1U);
        EXPECT_TRUE(h.checker.AnalysisComplete());
    }
}

TEST(RacecheckTest, TransitiveSynchronizationWithoutIntermediateMemoryAndTokenReuse)
{
    Harness h;
    h.Add(Memory(1));
    h.Add(Flag(2, 1));
    h.Add(Flag(3, 2));
    h.Add(Flag(4, 1, 1, 5));
    h.Add(Flag(5, 2, 1, 5));
    h.Add(Memory(6, 1, 5));
    h.Add(Flag(7, 1));
    h.Add(Flag(8, 2));
    h.Add(Memory(9, 1, 1));
    h.Sync();
    EXPECT_TRUE(h.reports.empty());
    EXPECT_TRUE(h.checker.AnalysisComplete());
}
TEST(RacecheckTest, InvalidAndUnknownSyncNeverProvesClean)
{
    for (int mode = 0; mode < 5; ++mode) {
        Harness h;
        h.Add(Memory(1));
        auto s = Flag(2, 1);
        auto w = Flag(3, 2);
        if (mode == 0)
            w.objectId = 9;
        if (mode == 1)
            std::swap(w.srcPipe, w.dstPipe);
        if (mode == 2)
            w.header.blockId = 1;
        if (mode == 3)
            s.syncKind = ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF;
        if (mode == 4) {
            auto duplicate = Flag(4, 1);
            h.Add(duplicate);
            w.header.instrExecId = 5;
        }
        h.Add(s);
        h.Add(w);
        h.Add(Memory(6, 1, 1));
        h.Sync();
        EXPECT_FALSE(h.checker.AnalysisComplete());
        EXPECT_FALSE(h.checker.HasErrors());
        EXPECT_TRUE(h.reports.empty());
    }
}
TEST(RacecheckTest, ShadowByteMasksPagesAndStrideHoles)
{
    Harness h;
    h.Add(Memory(1, 2, 4, 4094, 4));
    h.Add(Memory(2, 1, 1, 4098, 2));
    h.Add(Memory(3, 1, 5, 4095, 2));
    h.Sync();
    ASSERT_EQ(h.reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckRacecheckReport>(h.reports[0]).overlapBytes, 2U);
    Harness strided;
    auto a = Memory(1);
    a.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    a.layout.blockRepeat = {2, 4, 8, 1, 0, 0};
    strided.Add(a);
    strided.Add(Memory(2, 1, 1, 0x1004, 4));
    strided.Sync();
    EXPECT_TRUE(strided.reports.empty());
    EXPECT_TRUE(strided.checker.AnalysisComplete());
}
TEST(RacecheckTest, DenseAffineAxesCoalesceWithoutChangingExpansionBudget)
{
    auto a = Memory(1, 1, ACLSAN_DEVICE_PIPE_MTE2, 0x10000);
    a.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    a.layout.ndAffine = {5, 2, {2, 4, 8, 4, 16}, {-4096, 1024, 128, -32, 2}};
    std::vector<race::Interval> ranges;
    ASSERT_TRUE(race::Normalize(a, 4096, ranges));
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_EQ(ranges[0].begin, 0x10000U - 4096U - 96U);
    EXPECT_EQ(ranges[0].end - ranges[0].begin, 8192U);
    // Keep the original raw Cartesian-product limit, even for a dense result.
    EXPECT_FALSE(race::Normalize(a, 4095, ranges));
    EXPECT_TRUE(ranges.empty());
    a.layout.ndAffine = {2, 4, {2, 3, 0, 0, 0}, {64, 4, 0, 0, 0}};
    ASSERT_TRUE(race::Normalize(a, 6, ranges));
    ASSERT_EQ(ranges.size(), 2U);
    EXPECT_EQ(ranges[0].begin, a.address);
    EXPECT_EQ(ranges[0].end, a.address + 12);
    EXPECT_EQ(ranges[1].begin, a.address + 64);
    EXPECT_EQ(ranges[1].end, a.address + 76);
}
TEST(RacecheckTest, CoalescingPreservesSignedStrideAndAddressOverflowChecks)
{
    auto a = Memory(1, 1, ACLSAN_DEVICE_PIPE_MTE2, uint64_t{1} << 63);
    a.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    a.layout.ndAffine = {1, 1, {2, 0, 0, 0, 0}, {std::numeric_limits<int64_t>::min(), 0, 0, 0, 0}};
    std::vector<race::Interval> ranges;
    ASSERT_TRUE(race::Normalize(a, 2, ranges));
    ASSERT_EQ(ranges.size(), 2U);
    EXPECT_EQ(ranges[0].begin, 0U);
    EXPECT_EQ(ranges[0].end, 1U);
    EXPECT_EQ(ranges[1].begin, uint64_t{1} << 63);
    a.address = 3;
    a.layout.ndAffine = {1, 4, {2, 0, 0, 0, 0}, {-4, 0, 0, 0, 0}};
    EXPECT_FALSE(race::Normalize(a, 2, ranges));
    EXPECT_TRUE(ranges.empty());
    a.address = std::numeric_limits<uint64_t>::max() - 3;
    a.layout.ndAffine.strides[0] = 0;
    EXPECT_FALSE(race::Normalize(a, 2, ranges));
    a.address = 0;
    ASSERT_TRUE(race::Normalize(a, 2, ranges));
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_EQ(ranges[0].begin, 0U);
    EXPECT_EQ(ranges[0].end, 4U);
}
TEST(RacecheckTest, AffineCoalescingMatchesIndependentByteFootprintOracle)
{
    std::mt19937 rng(191827);
    for (size_t trial = 0; trial < 400; ++trial) {
        auto a = Memory(1, 1, ACLSAN_DEVICE_PIPE_MTE2, 256);
        a.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
        auto& layout = a.layout.ndAffine;
        layout.rank = 1 + rng() % 5;
        layout.elementBytes = 1 + rng() % 8;
        for (uint32_t d = 0; d < layout.rank; ++d) {
            layout.dims[d] = 1 + rng() % 4;
            layout.strides[d] = static_cast<int64_t>(rng() % 25) - 12;
        }
        // Enumerate individual bytes directly, without sorting/folding axes
        // or using the production interval-merging helper.
        std::set<uint64_t> expected;
        std::function<void(uint32_t, int64_t)> expand = [&](uint32_t d, int64_t address) {
            if (d == layout.rank) {
                for (uint32_t b = 0; b < layout.elementBytes; ++b)
                    expected.insert(static_cast<uint64_t>(address + b));
                return;
            }
            for (uint64_t i = 0; i < layout.dims[d]; ++i)
                expand(d + 1, address + static_cast<int64_t>(i) * layout.strides[d]);
        };
        expand(0, a.address);
        std::vector<race::Interval> ranges;
        ASSERT_TRUE(race::Normalize(a, 1024, ranges)) << trial;
        std::set<uint64_t> actual;
        for (const auto range : ranges)
            for (auto address = range.begin; address < range.end; ++address)
                actual.insert(address);
        EXPECT_EQ(actual, expected) << trial;
    }
}
TEST(RacecheckTest, NegativeStrideNdAndInvalidArithmetic)
{
    std::vector<race::Interval> ranges;
    auto a = Memory(1);
    a.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    a.layout.ndAffine = {2, 2, {2, 2, 0, 0, 0}, {-8, 2, 0, 0, 0}};
    ASSERT_TRUE(race::Normalize(a, 16, ranges));
    ASSERT_EQ(ranges.size(), 2U);
    EXPECT_EQ(ranges[0].begin, 0xff8U);
    EXPECT_EQ(ranges[1].end, 0x1004U);
    a.address = 0;
    EXPECT_FALSE(race::Normalize(a, 16, ranges));
    a = Memory(1, 2, 4, UINT64_MAX - 1, 8);
    EXPECT_FALSE(race::Normalize(a, 16, ranges));
    a.layout.range.bytes = 0;
    EXPECT_TRUE(race::Normalize(a, 0, ranges));
    EXPECT_TRUE(ranges.empty());
}
TEST(RacecheckTest, PrivateOwnersIsolatedGlobalPeersStableUnderInputPermutation)
{
    auto a = Memory(1);
    auto b = Memory(1, 1, 1);
    b.header.blockId = 1;
    b.header.phyCoreId = 1;
    Harness privateMemory;
    privateMemory.Add(a);
    privateMemory.Add(b);
    privateMemory.Sync();
    EXPECT_TRUE(privateMemory.reports.empty());
    a.memorySpace = b.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    for (bool reverse : {false, true}) {
        Harness h;
        h.Add(reverse ? b : a);
        h.Add(reverse ? a : b);
        h.Sync();
        ASSERT_EQ(h.reports.size(), 1U);
        const auto& r = std::get<NpuCheckRacecheckReport>(h.reports[0]);
        EXPECT_EQ(r.hazardKind, 1U);
        EXPECT_EQ(r.first.exec.blockId, 0U);
        EXPECT_TRUE(r.unorderedPeers);
    }
}
TEST(RacecheckTest, FirstWawStopsLaunchAndSameNonVectorPipeNeedsSynchronization)
{
    for (auto pipe : {ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE2}) {
        Harness h;
        h.Add(Memory(1));
        h.Add(Memory(2, 2, pipe));
        h.Add(Memory(3, 1, ACLSAN_DEVICE_PIPE_MTE3));
        h.Sync();
        ASSERT_EQ(h.reports.size(), 1U);
        EXPECT_EQ(std::get<NpuCheckRacecheckReport>(h.reports[0]).hazardKind, 3U);
        EXPECT_TRUE(h.checker.HasErrors());
        EXPECT_TRUE(h.checker.AnalysisComplete());
        EXPECT_NE(h.checker.Summary().find("stopped_launches=1"), std::string::npos);
    }
}

TEST(RacecheckTest, VectorHardwareOrderCoversRawWarWawRegardlessOfCallbackOrder)
{
    for (const auto modes : {std::pair{2U, 1U}, std::pair{1U, 2U}, std::pair{2U, 2U}}) {
        Harness h;
        h.Add(Memory(3, modes.second, ACLSAN_DEVICE_PIPE_VECTOR));
        h.Add(Memory(2, 1, ACLSAN_DEVICE_PIPE_VECTOR, 0x2000));
        h.Add(Memory(1, modes.first, ACLSAN_DEVICE_PIPE_VECTOR));
        h.Sync();
        EXPECT_TRUE(h.reports.empty());
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    }
}
TEST(RacecheckTest, VectorHardwareOrderDoesNotOrderOtherDomainsOrPipes)
{
    for (bool otherDomain : {false, true}) {
        Harness h;
        auto a = Memory(1, 2, ACLSAN_DEVICE_PIPE_VECTOR);
        auto b = Memory(2, 1, otherDomain ? ACLSAN_DEVICE_PIPE_VECTOR : ACLSAN_DEVICE_PIPE_MTE3);
        a.memorySpace = b.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        if (otherDomain)
            b.header.blockId = 1;
        h.Add(a);
        h.Add(b);
        h.Sync();
        ASSERT_EQ(h.reports.size(), 1U);
        EXPECT_TRUE(h.checker.HasErrors());
        EXPECT_TRUE(h.checker.AnalysisComplete());
    }
}
TEST(RacecheckTest, ExplicitSynchronizationOrdersPipesAndVectorOrder)
{
    Harness h;
    h.Add(Memory(1));
    h.Add(Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET));
    h.Add(Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT));
    h.Add(Memory(4, 1, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Add(Memory(5, 2, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Add(Flag(6, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE3));
    h.Add(Flag(7, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE3));
    h.Add(Memory(8, 1, ACLSAN_DEVICE_PIPE_MTE3));
    h.Sync();
    EXPECT_TRUE(h.reports.empty());
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
}
TEST(RacecheckTest, LongPublishedPrefixAndRepeatedWaitsStayWithinLinearGraphBudget)
{
    race::Limits limits;
    limits.edges = 16384;
    Harness h(limits);
    uint64_t sequence = 1;
    // A realistic tile can contain thousands of unordered DMA accesses and
    // repeatedly publish their prefix. Historical accesses must not be copied
    // into every SET or every later acquire.
    for (uint64_t i = 0; i < 3000; ++i)
        h.Add(Memory(sequence++, 1, ACLSAN_DEVICE_PIPE_MTE2, 0x1000 + i * 8));
    for (uint64_t round = 0; round < 128; ++round) {
        h.Add(Flag(
            sequence++, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE3, round % 16));
        h.Add(Flag(
            sequence++, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE3, round % 16));
    }
    h.Add(Memory(sequence++, 2, ACLSAN_DEVICE_PIPE_MTE3, 0x1000));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}
TEST(RacecheckTest, IndependentWaitsRetainBothAcquiredPrefixes)
{
    Harness h;
    h.Add(Memory(1, 1, ACLSAN_DEVICE_PIPE_MTE2, 0x1000));
    h.Add(Memory(2, 1, ACLSAN_DEVICE_PIPE_MTE1, 0x2000));
    h.Add(Flag(3, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE3, 0));
    h.Add(Flag(4, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE3, 0));
    h.Add(Flag(5, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_MTE1, ACLSAN_DEVICE_PIPE_MTE3, 1));
    h.Add(Flag(6, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_MTE1, ACLSAN_DEVICE_PIPE_MTE3, 1));
    h.Add(Memory(7, 2, ACLSAN_DEVICE_PIPE_MTE3, 0x1000));
    h.Add(Memory(8, 2, ACLSAN_DEVICE_PIPE_MTE3, 0x2000));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}
TEST(RacecheckTest, VectorOrderingEdgesRespectGraphBudget)
{
    race::Limits limits;
    limits.edges = 0;
    Harness h(limits);
    h.Add(Memory(1, 2, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Add(Memory(2, 1, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("graph_budget=1"), std::string::npos);
}
TEST(RacecheckTest, ReadWriteFragmentsDeduplicatedAndNoSelfRace)
{
    Harness h;
    auto a = Memory(1, 3);
    auto fragment = a;
    a.accessCount = 2;
    fragment.accessCount = 2;
    fragment.accessIndex = 1;
    h.Add(a);
    h.Add(fragment);
    h.Add(a);
    h.Add(Memory(2, 3, 1));
    h.Sync();
    ASSERT_EQ(h.reports.size(), 1U);
    for (auto& v : h.reports)
        EXPECT_EQ(std::get<NpuCheckRacecheckReport>(v).overlapBytes, 8U);
    Harness missing;
    missing.Add(a);
    missing.Sync();
    EXPECT_FALSE(missing.checker.AnalysisComplete());
}
TEST(RacecheckTest, InexactCallbacksDoNotDowngradeExactRawWarWaw)
{
    for (auto flags : {0U, 2U, 3U, 5U, 9U, 17U, 129U}) {
        for (auto modes : {std::pair{2U, 1U}, std::pair{1U, 2U}, std::pair{2U, 2U}}) {
            Harness h;
            h.Add(Memory(1, modes.first));
            auto inexact = Memory(2, 2, ACLSAN_DEVICE_PIPE_VECTOR);
            inexact.header.flags = flags;
            // Its malformed footprint must never enter the graph or shadow.
            inexact.accessCount = 0;
            inexact.layoutKind = 0;
            h.Add(inexact);
            h.Add(Memory(3, modes.second, ACLSAN_DEVICE_PIPE_VECTOR));
            h.Sync();
            ASSERT_EQ(h.reports.size(), 1U);
            EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
            EXPECT_TRUE(h.checker.HasErrors());
            EXPECT_EQ(
                std::get<NpuCheckRacecheckReport>(h.reports[0]).hazardKind, modes.first == 1  ? 2U :
                                                                            modes.second == 1 ? 1U :
                                                                                                3U);
            EXPECT_NE(h.checker.Summary().find("skipped_memory_events=1"), std::string::npos);
        }
    }
}
TEST(RacecheckTest, SkippedFragmentDoesNotInvalidateExactSibling)
{
    Harness h;
    auto exact = Memory(1);
    exact.accessCount = 2;
    auto inexact = exact;
    inexact.accessIndex = 1;
    inexact.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    h.Add(inexact);
    h.Add(exact);
    h.Add(Memory(2, 1, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Sync();
    ASSERT_EQ(h.reports.size(), 1U);
    EXPECT_TRUE(h.checker.HasErrors());
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
}
TEST(RacecheckTest, OnlyInexactAccessesAreSkippedWithoutHazards)
{
    Harness h;
    auto a = Memory(1);
    a.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    h.Add(a);
    h.Sync();
    EXPECT_TRUE(h.reports.empty());
    EXPECT_TRUE(h.checker.AnalysisComplete());
    EXPECT_FALSE(h.checker.HasErrors());
    EXPECT_NE(h.checker.Summary().find("skipped_memory_events=1"), std::string::npos);
}
TEST(RacecheckTest, AnalysisBudgetsStillFailClosed)
{
    for (bool shadow : {false, true}) {
        race::Limits limits;
        if (shadow)
            limits.stateBytes = 1;
        else
            limits.comparisons = 0;
        Harness h(limits);
        h.Add(Memory(1));
        h.Add(Memory(2, 1, 1));
        h.Sync();
        EXPECT_FALSE(h.checker.AnalysisComplete());
        EXPECT_FALSE(h.checker.HasErrors());
    }
}
TEST(RacecheckTest, StreamIsolationSnapshotsAndFinalization)
{
    Harness h;
    auto stream = reinterpret_cast<void*>(1);
    h.Launch(2, stream);
    h.Add(Memory(1));
    h.Sync(nullptr, 1); // Failed sync does not close this launch.
    EXPECT_TRUE(h.reports.empty());
    auto a = Memory(1);
    a.header.launchId = 2;
    auto b = Memory(2, 1, 1);
    b.header.launchId = 2;
    h.Add(a);
    h.Add(b);
    h.Sync(stream);
    ASSERT_EQ(h.reports.size(), 1U);
    h.Sync(stream);
    EXPECT_EQ(h.reports.size(), 1U);
    h.checker.Finalize(h.reports);
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("missing_sync_end=1"), std::string::npos);
}
TEST(RacecheckTest, SuccessfulRetryPreservesKnownSyncFailure)
{
    Harness h;
    h.Add(Memory(1));
    h.Add(Memory(2, 1, 1));
    h.Sync(nullptr, 1);
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_FALSE(h.checker.HasErrors());
    EXPECT_NE(h.checker.Summary().find("synchronization_failed=1"), std::string::npos);
}
TEST(RacecheckTest, MissingSyncOrEmptyObservationCannotBeClean)
{
    Harness pending;
    pending.Add(Memory(1));
    pending.checker.Finalize(pending.reports);
    EXPECT_FALSE(pending.checker.AnalysisComplete());
    Harness empty;
    empty.Sync();
    EXPECT_FALSE(empty.checker.AnalysisComplete());
    EXPECT_NE(empty.checker.Summary().find("empty_memory_observation=1"), std::string::npos);
}
TEST(RacecheckTest, InexactSynchronizationMakesOrderingIncomplete)
{
    for (auto flags :
         {ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED, ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED, ACLSAN_DEVICE_EVENT_FLAG_PREDICATED,
          ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR}) {
        Harness h;
        h.Add(Memory(1));
        auto set = Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET);
        set.header.flags = flags;
        h.Add(set);
        h.Add(Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT));
        h.Add(Memory(4, 1, 1));
        h.Sync();
        EXPECT_FALSE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_FALSE(h.checker.HasErrors());
        EXPECT_TRUE(h.reports.empty());
        EXPECT_NE(h.checker.Summary().find("skipped_sync_events=1"), std::string::npos);
    }
}
TEST(RacecheckTest, SkippedSyncGenerationPreventsLaunchWideVerdict)
{
    for (bool skipSet : {false, true}) {
        Harness h;
        auto set = Flag(1, ACLSAN_DEVICE_SYNC_ACTION_SET);
        auto wait = Flag(2, ACLSAN_DEVICE_SYNC_ACTION_WAIT);
        (skipSet ? set : wait).header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
        h.Add(set);
        h.Add(wait);
        h.Add(Memory(3));
        h.Add(Flag(4, ACLSAN_DEVICE_SYNC_ACTION_SET));
        h.Add(Flag(5, ACLSAN_DEVICE_SYNC_ACTION_WAIT));
        h.Add(Memory(6, 1, ACLSAN_DEVICE_PIPE_VECTOR));
        h.Sync();
        EXPECT_FALSE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_TRUE(h.reports.empty());
        EXPECT_NE(h.checker.Summary().find("skipped_sync_events=1"), std::string::npos);
    }
}
TEST(RacecheckTest, CoverageIsExplicitlyLimitedToObservedCallbacks)
{
    Harness h;
    h.Add(Memory(1));
    h.Add(Memory(2, 1, 1));
    h.Sync();
    ASSERT_EQ(h.reports.size(), 1U);
    const auto& report = std::get<NpuCheckRacecheckReport>(h.reports[0]);
    EXPECT_EQ(report.coverage, "observed_cbdata");
    EXPECT_EQ(report.ordering, "no_observed_happens_before_path");
    EXPECT_NE(h.checker.Summary().find("trace_completeness=unverified"), std::string::npos);
}
TEST(RacecheckTest, RenderIncludesOverlapAndBothSitesWithoutInventedValues)
{
    Harness h;
    h.Add(Memory(1));
    h.Add(Memory(2, 1, 1));
    h.Sync();
    ASSERT_EQ(h.reports.size(), 1U);
    auto& r = std::get<NpuCheckRacecheckReport>(h.reports[0]);
    std::string text;
    ASSERT_EQ(RenderNpuCheckReportRecord(NpuCheckReportRecord::From(r), {}, &text), ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("bytes=8"), std::string::npos);
    EXPECT_NE(text.find("coverage=observed_cbdata"), std::string::npos);
    EXPECT_EQ(text.find("Current Value"), std::string::npos);
    EXPECT_EQ(text.find("Incoming Value"), std::string::npos);
}
TEST(RacecheckTest, RandomExactFootprintsAgreeWithExhaustiveByteOracle)
{
    std::mt19937 rng(937);
    for (int trial = 0; trial < 50; ++trial) {
        Harness h;
        std::vector<AclsanDeviceMemoryAccessData> accesses;
        // Each access has a separate domain so no implicit local order can affect the oracle.
        for (uint32_t i = 0; i < 12; ++i) {
            auto a = Memory(1, 1 + rng() % 2, 4, 0x1000 + rng() % 32, 1 + rng() % 12);
            a.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
            a.header.blockId = i;
            a.header.phyCoreId = i;
            accesses.push_back(a);
        }
        std::set<std::pair<uint32_t, uint32_t>> expected, actual;
        for (size_t i = 0; i < accesses.size(); ++i)
            for (size_t j = i + 1; j < accesses.size(); ++j) {
                auto a = accesses[i], b = accesses[j];
                if ((a.accessMode == 2 || b.accessMode == 2) &&
                    std::max(a.address, b.address) <
                        std::min(a.address + a.layout.range.bytes, b.address + b.layout.range.bytes))
                    expected.emplace(i, j);
            }
        std::shuffle(accesses.begin(), accesses.end(), rng);
        for (auto a : accesses)
            h.Add(a);
        h.Sync();
        for (auto& v : h.reports) {
            auto r = std::get<NpuCheckRacecheckReport>(v);
            actual.emplace(
                std::min(r.first.exec.blockId, r.second.exec.blockId),
                std::max(r.first.exec.blockId, r.second.exec.blockId));
        }
        EXPECT_EQ(actual.empty(), expected.empty());
        EXPECT_LE(actual.size(), 1U);
        for (const auto& pair : actual)
            EXPECT_EQ(expected.count(pair), 1U);
        EXPECT_TRUE(h.checker.AnalysisComplete());
    }
}

TEST(RacecheckTest, AtomicAndUnknownOwnersRemainUnsupported)
{
    for (bool atomic : {false, true}) {
        Harness h;
        auto a = Memory(1);
        if (atomic)
            a.instructionId = 56;
        else
            a.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN;
        h.Add(a);
        h.Add(Memory(2, 1, 1));
        h.Sync();
        EXPECT_FALSE(h.checker.AnalysisComplete());
        EXPECT_FALSE(h.checker.HasErrors());
    }
}
TEST(RacecheckTest, UnresolvedMasksAreSkipped)
{
    for (int mode = 0; mode < 5; ++mode) {
        Harness h;
        auto a = Memory(1);
        if (mode == 0)
            a.vectorMask0 = 1;
        if (mode == 1)
            a.vectorMask1 = 1;
        if (mode == 2)
            a.predicateMask0 = 1;
        if (mode == 3)
            a.predicateMask1 = 1;
        if (mode == 4)
            a.memoryFlags = 1;
        h.Add(a);
        h.Add(Memory(2, 1, 1));
        h.Sync();
        EXPECT_TRUE(h.checker.AnalysisComplete());
        EXPECT_TRUE(h.reports.empty());
        EXPECT_NE(h.checker.Summary().find("skipped_memory_events=1"), std::string::npos);
    }
}
TEST(RacecheckTest, ConflictingDuplicateAndLateRecordInvalidateSession)
{
    Harness h;
    auto a = Memory(1);
    h.Add(a);
    a.address += 1;
    h.Add(a);
    h.Sync();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_FALSE(
        h.checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &a, h.reports));
}

TEST(RacecheckTest, StateBudgetFailureIsIncompleteAndFullWidthAddressesWork)
{
    race::Limits limits;
    limits.stateBytes = 1;
    Harness limited(limits);
    limited.Add(Memory(1));
    limited.Sync();
    EXPECT_FALSE(limited.checker.AnalysisComplete());
    EXPECT_NE(limited.checker.Summary().find("state_plan_budget=1"), std::string::npos);
    Harness wide;
    wide.Add(Memory(1, 2, ACLSAN_DEVICE_PIPE_MTE2, UINT64_C(1) << 48U));
    wide.Sync();
    EXPECT_TRUE(wide.checker.AnalysisComplete());
}
TEST(RacecheckTest, DisabledReachabilityCachePreservesOrdering)
{
    race::Limits limits;
    limits.queryCache = 0;
    Harness h(limits);
    h.Add(Memory(1));
    h.Add(Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET));
    h.Add(Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT));
    h.Add(Memory(4, 1, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete());
    EXPECT_TRUE(h.reports.empty());
}
TEST(RacecheckTest, EarlierReaderSurvivesLaterOrderedReaderAndPcCollision)
{
    Harness h;
    auto first = Memory(1, 1, ACLSAN_DEVICE_PIPE_MTE1);
    auto second = Memory(2, 1, ACLSAN_DEVICE_PIPE_MTE2);
    second.header.pc = first.header.pc + (UINT64_C(1) << 32U);
    h.Add(first);
    h.Add(second);
    h.Add(Flag(3, ACLSAN_DEVICE_SYNC_ACTION_SET));
    h.Add(Flag(4, ACLSAN_DEVICE_SYNC_ACTION_WAIT));
    h.Add(Memory(5, 2, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Sync();
    ASSERT_TRUE(h.checker.AnalysisComplete());
    ASSERT_EQ(h.reports.size(), 1U);
    EXPECT_NE(h.checker.Summary().find("war=1"), std::string::npos);
}
TEST(RacecheckTest, ExactOverlapAcrossLeafAndRootBoundaries)
{
    for (auto boundary : {UINT64_C(1) << 16U, UINT64_C(1) << 32U}) {
        Harness h;
        h.Add(Memory(1, 2, ACLSAN_DEVICE_PIPE_MTE2, boundary - 3, 8));
        h.Add(Memory(2, 1, ACLSAN_DEVICE_PIPE_VECTOR, boundary - 1, 4));
        h.Sync();
        ASSERT_TRUE(h.checker.AnalysisComplete());
        ASSERT_EQ(h.reports.size(), 1U);
        const auto& report = std::get<NpuCheckRacecheckReport>(h.reports.front());
        EXPECT_EQ(report.overlapBytes, 4U);
        ASSERT_EQ(report.overlapRanges.size(), 1U);
        EXPECT_EQ(report.overlapRanges.front().first, boundary - 1);
        EXPECT_EQ(report.overlapRanges.front().second, boundary + 3);
    }
}
TEST(RacecheckTest, WriterSurvivesReadAndDetectsUnorderedSecondRead)
{
    Harness h;
    h.Add(Memory(1));
    // C follows an independent, longer synchronization path. B is visited before C,
    // but B must not erase A's writer state.
    h.Add(Memory(2, 1, ACLSAN_DEVICE_PIPE_MTE1, 0x2000));
    h.Add(Flag(3, 1, ACLSAN_DEVICE_PIPE_MTE1, ACLSAN_DEVICE_PIPE_SCALAR));
    h.Add(Flag(4, 2, ACLSAN_DEVICE_PIPE_MTE1, ACLSAN_DEVICE_PIPE_SCALAR));
    h.Add(Flag(5, 1, ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_MTE3));
    h.Add(Flag(6, 2, ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_MTE3));
    h.Add(Flag(7, 1));
    h.Add(Flag(8, 2));
    h.Add(Memory(9, 1, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Add(Memory(10, 1, ACLSAN_DEVICE_PIPE_MTE3));
    h.Sync();
    ASSERT_EQ(h.reports.size(), 1U);
    const auto& r = std::get<NpuCheckRacecheckReport>(h.reports[0]);
    EXPECT_EQ(r.hazardKind, 1U);
    EXPECT_EQ(r.first.exec.instrExecId, 1U);
    EXPECT_EQ(r.second.exec.instrExecId, 10U);
}
TEST(RacecheckTest, SamePipeReadersCannotOverwriteEarlierUnorderedReader)
{
    Harness h;
    h.Add(Memory(1, 1));
    h.Add(Flag(2, 1));
    h.Add(Flag(3, 2));
    h.Add(Memory(4, 1)); // Same MTE2, issued after SET, not covered by that SET.
    h.Add(Memory(5, 2, ACLSAN_DEVICE_PIPE_VECTOR));
    h.Sync();
    ASSERT_EQ(h.reports.size(), 1U);
    const auto& r = std::get<NpuCheckRacecheckReport>(h.reports[0]);
    EXPECT_EQ(r.hazardKind, 2U);
    EXPECT_EQ(r.first.exec.instrExecId, 4U);
    EXPECT_EQ(r.second.exec.instrExecId, 5U);
}
TEST(RacecheckTest, OverlappingOperandsDoNotCreateSelfRaces)
{
    Harness h;
    auto a = Memory(1, 1);
    a.accessCount = 2;
    auto b = a;
    b.accessIndex = 1;
    b.accessMode = 2;
    b.address += 4;
    h.Add(b);
    h.Add(a);
    h.Sync();
    EXPECT_TRUE(h.reports.empty());
    EXPECT_TRUE(h.checker.AnalysisComplete());
}
TEST(RacecheckTest, HugeContiguousRangeDoesNotAllocatePerByte)
{
    race::Limits limits;
    limits.stateBytes = 4096;
    Harness h(limits);
    h.Add(Memory(1, 2, ACLSAN_DEVICE_PIPE_VECTOR, 1ULL << 50, 1ULL << 40));
    h.Add(Memory(2, 1, ACLSAN_DEVICE_PIPE_VECTOR, 1ULL << 50, 1ULL << 40));
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}
TEST(RacecheckTest, RandomSynchronizationAndFootprintsMatchAllPairsOracle)
{
    // Independent predecessor-set model: do not use the production graph or state table.
    std::mt19937 rng(712);
    constexpr uint32_t pipes[] = {
        ACLSAN_DEVICE_PIPE_MTE1, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE3, ACLSAN_DEVICE_PIPE_VECTOR};
    for (int trial = 0; trial < 150; ++trial) {
        Harness h;
        std::vector<AclsanDeviceMemoryAccessData> memory;
        std::vector<AclsanDeviceSyncData> sync;
        std::array<std::set<size_t>, 4> prefixes, acquired;
        std::vector<std::set<size_t>> ancestors;
        uint64_t sequence = 1;
        for (size_t i = 0; i < 16; ++i) {
            if (rng() % 2 == 0) {
                size_t src = rng() % 4, dst = (src + 1 + rng() % 3) % 4;
                sync.push_back(Flag(sequence++, 1, pipes[src], pipes[dst]));
                sync.push_back(Flag(sequence++, 2, pipes[src], pipes[dst]));
                acquired[dst].insert(prefixes[src].begin(), prefixes[src].end());
                prefixes[dst].insert(prefixes[src].begin(), prefixes[src].end());
            }
            const size_t pipe = rng() % 4;
            auto a = Memory(sequence++, 1 + rng() % 3, pipes[pipe], 0x1000 + rng() % 48, 1 + rng() % 12);
            auto prior = acquired[pipe];
            if (pipe == 3)
                prior.insert(prefixes[pipe].begin(), prefixes[pipe].end());
            // Prefix sets include the ancestors of every instruction they publish.
            ancestors.push_back(prior);
            prefixes[pipe].insert(prior.begin(), prior.end());
            prefixes[pipe].insert(i);
            memory.push_back(a);
        }
        std::set<std::pair<uint64_t, uint64_t>> conflicts;
        for (size_t i = 0; i < memory.size(); ++i)
            for (size_t j = i + 1; j < memory.size(); ++j) {
                const auto& a = memory[i];
                const auto& b = memory[j];
                if (((a.accessMode | b.accessMode) & 2U) && !ancestors[j].count(i) &&
                    std::max(a.address, b.address) <
                        std::min(a.address + a.layout.range.bytes, b.address + b.layout.range.bytes))
                    conflicts.emplace(a.header.instrExecId, b.header.instrExecId);
            }
        std::shuffle(memory.begin(), memory.end(), rng);
        std::shuffle(sync.begin(), sync.end(), rng);
        for (const auto& a : memory)
            h.Add(a);
        for (const auto& a : sync)
            h.Add(a);
        h.Sync();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.empty(), conflicts.empty());
        ASSERT_LE(h.reports.size(), 1U);
        if (!h.reports.empty()) {
            const auto& r = std::get<NpuCheckRacecheckReport>(h.reports[0]);
            const auto pair = std::minmax(r.first.exec.instrExecId, r.second.exec.instrExecId);
            EXPECT_EQ(conflicts.count(pair), 1U);
        }
    }
}
TEST(RacecheckTest, LaunchLimitDoesNotPreventClosingExistingBatch)
{
    race::Limits limits;
    limits.events = 2;
    Harness h(limits);
    h.Launch(2);
    auto a = Memory(1);
    h.Add(a);
    a.header.launchId = 2;
    h.Add(a);
    h.Sync();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
}
} // namespace
} // namespace npucheck
