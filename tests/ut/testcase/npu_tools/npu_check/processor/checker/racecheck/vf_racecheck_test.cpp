/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_cbdata_operator.h"
#include "device_instr/common/instruction_id.h"
#include "checker/racecheck.h"
#include "checker/racecheck_reports.h"
#include "aclsan_device_data.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include <gtest/gtest.h>
#include <array>
#include <random>

namespace {
using namespace npucheck;
constexpr uint32_t VLD = ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ;
constexpr uint32_t VST = ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE;
constexpr uint32_t SLD = ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ;
constexpr uint32_t SST = ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE;

TEST(InstructionIdentityTest, SeparatesLaunchesForOuterAndVfEvents)
{
    AclsanDeviceMemoryAccessData access{};
    access.header.launchId = 1;
    access.header.instrExecId = 42;
    auto other = access;
    other.header.launchId = 2;
    EXPECT_NE(race::Key(access), race::Key(other));

    access.vfInstrValid = other.vfInstrValid = 1;
    access.vfInstrExecSeq = other.vfInstrExecSeq = 0;
    EXPECT_NE(race::Key(access), race::Key(other));

    AclsanDeviceBarrierData barrier{};
    barrier.header = access.header;
    barrier.barrierKind = ACLSAN_DEVICE_BARRIER_LOCAL_MEM;
    EXPECT_EQ(race::Key(access), race::Key(barrier));
    barrier.header.launchId = 2;
    EXPECT_NE(race::Key(access), race::Key(barrier));
}

TEST(InstructionIdentityTest, VfEventsUseHeaderParentAndInternalSequence)
{
    AclsanDeviceMemoryAccessData access{};
    access.header.launchId = 7;
    access.header.instrExecId = 42;
    access.vfInstrValid = 1;
    access.vfInstrExecSeq = 0;

    auto other = access;
    other.header.instrExecId = 43;
    EXPECT_NE(race::Key(access), race::Key(other));
    other = access;
    other.vfInstrExecSeq = 1;
    EXPECT_NE(race::Key(access), race::Key(other));
    EXPECT_NE(race::Key(access), race::Key(access.header));

    AclsanDeviceBarrierData barrier{};
    barrier.header = access.header;
    barrier.barrierKind = ACLSAN_DEVICE_BARRIER_LOCAL_MEM;
    EXPECT_EQ(race::Key(access), race::Key(barrier));
    ++barrier.header.instrExecId;
    EXPECT_NE(race::Key(access), race::Key(barrier));
}

AclsanDeviceMemoryAccessData Access(
    uint64_t parent, uint64_t index, uint32_t serial, uint32_t mode, [[maybe_unused]] uint32_t count = 2,
    uint64_t address = 0)
{
    AclsanDeviceMemoryAccessData a{};
    a.header = {
        ACLSAN_API_VERSION,
        sizeof(a),
        1,
        0x200 + index * 4,
        0,
        ACLSAN_DEVICE_SOURCE_VECTOR,
        parent,
        serial,
        0,
        {18, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR},
        ACLSAN_DEVICE_PIPE_VECTOR,
        ACLSAN_DEVICE_EVENT_FLAG_EXACT};
    a.accessMode = mode;
    a.address = address;
    a.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    a.memoryOwner = a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ? AclsanCoreInfo{} : a.header.executor;
    a.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    a.layout.range.bytes = 64;
    a.accessIndex = 0;
    a.accessCount = 1;
    a.instructionId = static_cast<uint32_t>(aclsan::InstructionId::Vf);
    a.vfInstrValid = 1;
    a.vfInstrExecSeq = index;
    return a;
}

TEST(InstructionIdentityTest, DynamicVfKeyGroupsOnlyTheSameParentInvocation)
{
    auto access = Access(10, 0, 0, VLD);
    auto same = access;
    same.vfInstrExecSeq = 9;
    EXPECT_TRUE(race::SameDynamicVf(access, same));

    auto otherLaunch = same;
    ++otherLaunch.header.launchId;
    EXPECT_FALSE(race::SameDynamicVf(access, otherLaunch));

    auto otherBlock = same;
    ++otherBlock.header.executor.blockId;
    EXPECT_FALSE(race::SameDynamicVf(access, otherBlock));

    auto ordinary = access;
    ordinary.vfInstrValid = 0;
    EXPECT_FALSE(race::SameDynamicVf(access, ordinary));
}

struct Harness {
    std::shared_ptr<MemoryAnalysis> analysis;
    Racechecker checker;
    CheckerReportList reports;
    explicit Harness(race::Limits limits = {}, bool managed = false)
        : analysis(std::make_shared<MemoryAnalysis>(managed)), checker(limits, analysis)
    {
        AclsanLaunchData l{};
        l.common.version = ACLSAN_API_VERSION;
        l.common.size = sizeof(l);
        l.launchId = 1;
        if (analysis->Managed()) {
            analysis->BeginCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &l);
        }
        checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &l, reports);
    }
    template <class T>
    void Add(AclsanCallbackId id, const T& a)
    {
        if (analysis->Managed()) {
            ASSERT_TRUE(analysis->BeginCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, id, &a));
        }
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, id, &a, reports));
        if (analysis->Managed()) {
            analysis->EndCallback();
        }
    }
    void Add(const AclsanDeviceMemoryAccessData& a) { Add(ACLSAN_CBID_DEVICE_MEMORY_ACCESS, a); }
    void Complete(uint64_t parent, uint32_t accesses, uint32_t events, bool success = true, uint32_t warnings = 0)
    {
        AclsanDeviceVfData v{};
        v.header = Access(parent, 0, 0, VLD).header;
        v.header.instrExecId = parent;
        v.header.size = sizeof(v);
        v.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        v.status = success ? ACLSAN_DEVICE_VF_SUCCESS : ACLSAN_DEVICE_VF_QUERY_FAILED;
        v.accessCount = accesses;
        v.eventCount = events;
        v.rawEventCount = events;
        v.warningFlags = warnings;
        Add(ACLSAN_CBID_DEVICE_VF, v);
    }
    void Bar(uint64_t parent, uint64_t index, uint32_t serial, uint32_t src, uint32_t dst)
    {
        AclsanDeviceBarrierData b{};
        b.header = Access(parent, index, serial, VLD).header;
        b.header.size = sizeof(b);
        b.barrierKind = ACLSAN_DEVICE_BARRIER_LOCAL_MEM;
        b.pipe = ACLSAN_DEVICE_PIPE_INVALID;
        b.srcMemType = src;
        b.dstMemType = dst;
        b.vfInstrExecSeq = index;
        Add(ACLSAN_CBID_DEVICE_PIPE_BARRIER, b);
    }
    void Flag(uint64_t parent, uint32_t action, uint32_t src, uint32_t dst)
    {
        AclsanDeviceSyncData s{};
        s.header = Access(parent, 0, 0, VLD).header;
        s.header.instrExecId = parent;
        s.header.size = sizeof(s);
        s.header.flags = 0;
        s.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        s.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
        s.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
        s.action = action;
        s.srcPipe = src;
        s.dstPipe = dst;
        Add(ACLSAN_CBID_DEVICE_SYNC, s);
    }
    void End()
    {
        AclsanOperatorExecutionData completion{};
        completion.common = {ACLSAN_API_VERSION, sizeof(completion), nullptr, 0, 1};
        completion.launchId = 1;
        const auto domain = ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION;
        const auto id = ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE;
        if (analysis->Managed()) {
            ASSERT_TRUE(analysis->BeginCallback(domain, id, &completion));
        }
        ASSERT_TRUE(checker.OnCallback(domain, id, &completion, reports));
        if (analysis->Managed()) {
            analysis->EndCallback();
        }
        AclsanSynchronizeData s{};
        s.common.version = ACLSAN_API_VERSION;
        s.common.size = sizeof(s);
        s.common.correlationId = 1;
        if (analysis->Managed()) {
            analysis->BeginCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &s);
        }
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &s, reports);
        if (analysis->Managed()) {
            analysis->EndCallback();
        }
    }
};

TEST(VfRacecheck, VfModeWithoutIndexIsIncompleteEvenWhenInstructionIdIsZero)
{
    Harness h;
    auto a = Access(10, 0, 0, VST);
    auto b = Access(10, 1, 1, VLD);
    a.vfInstrValid = b.vfInstrValid = 0;
    b.instructionId = 0;
    a.instructionId = b.instructionId = 0;
    h.Add(a);
    h.Add(b);
    h.End();
    EXPECT_FALSE(h.checker.AnalysisComplete());
}
TEST(VfRacecheck, DifferentPcsWithinOneVfAreIgnored)
{
    Harness h;
    h.Add(Access(10, 0, 0, VST));
    h.Add(Access(10, 1, 1, VLD));
    h.Complete(10, 2, 2);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}
TEST(VfRacecheck, SamePcLoopStoresWithinOneVfAreIgnored)
{
    Harness h;
    auto a = Access(10, 7, 0, VST);
    auto b = Access(10, 9, 1, VST);
    b.header.pc = a.header.pc;
    h.Add(a);
    h.Add(b);
    h.Complete(10, 2, 2);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}
TEST(VfRacecheck, ScalarAndVectorGroupsWithinOneVfAreIgnored)
{
    for (auto first : {SLD, SST, VLD, VST}) {
        Harness h;
        h.Add(Access(10, 0, 0, first));
        h.Add(Access(10, 1, 1, SST));
        h.Complete(10, 2, 2);
        h.End();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_TRUE(h.reports.empty()) << first;
    }
}

TEST(VfRacecheck, ReplayedVfAccessesRetainFirstWriteWitness)
{
    Harness h;
    auto external = Access(1, 0, 0, VST, 1);
    external.vfInstrValid = 0;
    external.instructionId = 0;
    external.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    external.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    h.Add(external);
    h.Add(Access(10, 0, 0, VLD));
    const auto firstWrite = Access(10, 1, 1, VST);
    h.Add(firstWrite);
    h.Add(Access(10, 2, 2, VLD));
    h.Add(Access(10, 3, 3, VST));
    h.Complete(10, 4, 4);
    h.End();
    ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    ASSERT_EQ(h.reports.size(), 1U);
    const auto& report = std::get<NpuCheckRacecheckReport>(h.reports.front());
    EXPECT_EQ(report.kind, ConflictKind::WAW);
    const auto& vf = report.previous.vfInstrValid ? report.previous : report.current;
    EXPECT_EQ(vf.accessMode, VST);
    EXPECT_EQ(vf.vfInstrExecSeq, 1U);
    EXPECT_EQ(vf.header.pc, firstWrite.header.pc);
    EXPECT_EQ(report.checkRange.begin, 0U);
    EXPECT_EQ(report.checkRange.end, 64U);
}

TEST(VfRacecheck, ReplayedVfReportsDistinctOverlappingRanges)
{
    for (bool delayVf : {false, true}) {
        SCOPED_TRACE(delayVf);
        Harness h;
        auto external = Access(1, 0, 0, VST, 1);
        external.vfInstrValid = 0;
        external.instructionId = 0;
        external.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        external.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
        h.Add(external);
        if (delayVf)
            h.Complete(5, 0, 0);
        h.Add(Access(10, 0, 0, VLD));
        auto write = Access(10, 1, 1, VST, 1, 32);
        write.layout.range.bytes = 32;
        h.Add(write);
        h.Complete(10, 2, 2);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        ASSERT_EQ(h.reports.size(), 2U);
        std::array<bool, 2> seen{};
        for (const auto& entry : h.reports) {
            const auto& report = std::get<NpuCheckRacecheckReport>(entry);
            const bool vfPrevious = report.previous.vfInstrValid != 0;
            const auto& vf = vfPrevious ? report.previous : report.current;
            const auto& externalAccess = vfPrevious ? report.current : report.previous;
            ASSERT_NE(vf.vfInstrValid, 0);
            EXPECT_EQ(externalAccess.vfInstrValid, 0);
            EXPECT_EQ(externalAccess.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
            ASSERT_LT(vf.vfInstrExecSeq, seen.size());
            const size_t index = vf.vfInstrExecSeq;
            EXPECT_FALSE(seen[index]);
            seen[index] = true;
            EXPECT_EQ(vf.accessMode, index == 0 ? VLD : VST);
            EXPECT_EQ(
                report.kind, index == 0 ? (vfPrevious ? ConflictKind::WAR : ConflictKind::RAW) : ConflictKind::WAW);
            EXPECT_EQ(report.checkRange.begin, index * 32U);
            EXPECT_EQ(report.checkRange.end, (index + 1U) * 32U);
        }
    }
}

TEST(VfRacecheck, RepeatedVfAccessesFitSmallComparisonBudget)
{
    constexpr size_t COMPARISON_BUDGET = 4;
    race::Limits limits;
    limits.comparisons = COMPARISON_BUDGET;
    Harness h(limits);
    auto external = Access(1, 0, 0, VST, 1);
    external.vfInstrValid = 0;
    external.instructionId = 0;
    external.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    external.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    h.Add(external);
    for (uint32_t i = 0; i < 40; ++i) {
        h.Add(Access(10, i, i, VST));
    }
    h.Complete(10, 40, 40);
    h.End();
    ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_EQ(h.reports.size(), 1U);
}

TEST(VfRacecheck, VfSummaryChargesBytesButNotTrackerSegments)
{
    constexpr size_t INSUFFICIENT_BYTES = 700;
    constexpr size_t SUFFICIENT_BYTES = 1024;
    for (const auto byteBudget : {INSUFFICIENT_BYTES, SUFFICIENT_BYTES}) {
        SCOPED_TRACE(byteBudget);
        race::Limits limits;
        limits.stateBytes = byteBudget;
        limits.stateSegments = 1;
        Harness h(limits);
        h.Add(Access(10, 0, 0, VST, 1));
        h.Complete(10, 1, 1);
        h.End();
        const bool expectedComplete = byteBudget == SUFFICIENT_BYTES;
        const auto summary = h.checker.Summary();
        EXPECT_EQ(h.checker.AnalysisComplete(), expectedComplete) << summary;
        EXPECT_EQ(summary.find("memory_state_budget") == std::string::npos, expectedComplete);
        EXPECT_TRUE(h.reports.empty());
    }
}

TEST(VfRacecheck, CompressedVfPreservesReadWriteConflictSemantics)
{
    for (const auto externalMode : {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE}) {
        for (const auto first : {VLD, VST}) {
            for (const auto second : {VLD, VST}) {
                SCOPED_TRACE(::testing::Message() << externalMode << '/' << first << '/' << second);
                Harness h;
                auto external = Access(1, 0, 0, VST, 1);
                external.vfInstrValid = 0;
                external.instructionId = 0;
                external.accessMode = externalMode;
                external.header.pipeline = externalMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? ACLSAN_DEVICE_PIPE_MTE3 :
                                                                                              ACLSAN_DEVICE_PIPE_MTE2;
                h.Add(external);
                h.Add(Access(10, 0, 0, first));
                h.Add(Access(10, 1, 1, second));
                h.Complete(10, 2, 2);
                h.End();
                ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
                const bool conflict =
                    externalMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE || first == VST || second == VST;
                EXPECT_EQ(h.reports.size(), conflict ? 1U : 0U);
            }
        }
    }
}

TEST(VfRacecheck, SynchronizedEarlierVfDoesNotHideLaterVfConflict)
{
    Harness h;
    h.Add(Access(10, 0, 0, VST, 1));
    h.Complete(10, 1, 1);
    h.Flag(11, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE2);
    h.Flag(12, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE2);
    auto external = Access(13, 0, 0, VST, 1);
    external.vfInstrValid = 0;
    external.instructionId = 0;
    external.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    external.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    h.Add(external);
    h.Add(Access(20, 0, 0, VLD, 1));
    h.Complete(20, 1, 1);
    h.End();
    ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    ASSERT_EQ(h.reports.size(), 1U);
    const auto& report = std::get<NpuCheckRacecheckReport>(h.reports.front());
    const auto& vf = report.previous.vfInstrValid ? report.previous : report.current;
    EXPECT_EQ(vf.header.instrExecId, 20U);
}

TEST(VfRacecheck, CompressedRangesMatchByteLevelConflictOracle)
{
    std::mt19937 random(23);
    for (size_t trial = 0; trial < 80; ++trial) {
        SCOPED_TRACE(trial);
        Harness h;
        auto external = Access(1, 0, 0, VST, 1, random() % 48);
        external.vfInstrValid = 0;
        external.instructionId = 0;
        external.accessMode = random() % 2 ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        external.header.pipeline = external.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE ? ACLSAN_DEVICE_PIPE_MTE2 :
                                                                                              ACLSAN_DEVICE_PIPE_MTE3;
        external.layout.range.bytes = 1 + random() % 16;
        h.Add(external);
        std::array<bool, 64> expected{}, actual{};
        for (uint32_t i = 0; i < 8; ++i) {
            auto vf = Access(10, i, i, random() % 2 ? VST : VLD, 1, random() % 48);
            vf.layout.range.bytes = 1 + random() % 16;
            h.Add(vf);
            if (external.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ && vf.accessMode == VLD)
                continue;
            for (uint64_t byte = 0; byte < expected.size(); ++byte) {
                if (external.address <= byte && byte < external.address + external.layout.range.bytes &&
                    vf.address <= byte && byte < vf.address + vf.layout.range.bytes) {
                    expected[byte] = true;
                }
            }
        }
        h.Complete(10, 8, 8);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        for (const auto& item : h.reports) {
            const auto& report = std::get<NpuCheckRacecheckReport>(item);
            const auto& vf = report.previous.vfInstrValid ? report.previous : report.current;
            ASSERT_TRUE(vf.vfInstrValid);
            EXPECT_LE(vf.address, report.checkRange.begin);
            EXPECT_GE(vf.address + vf.layout.range.bytes, report.checkRange.end);
            ASSERT_LE(report.checkRange.end, actual.size());
            for (uint64_t byte = report.checkRange.begin; byte < report.checkRange.end; ++byte)
                actual[byte] = true;
        }
        EXPECT_EQ(actual, expected);
    }
}
TEST(VfRacecheck, DirectionalBarriersDoNotRestoreSameVfChecks)
{
    for (bool correct : {false, true}) {
        Harness h;
        h.Add(Access(10, 3, 0, VLD));
        h.Bar(
            10, 17, 1, correct ? ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD : ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE,
            ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE);
        auto b = Access(10, 18, 2, SST);
        b.accessIndex = 0;
        h.Add(b);
        h.Complete(10, 2, 3);
        h.End();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_TRUE(h.reports.empty());
    }
}
TEST(VfRacecheck, MultipleRangesOfOneGroupDoNotRaceWithEachOther)
{
    Harness h;
    auto a = Access(10, 8, 0, VST);
    a.accessCount = 2;
    auto b = a;
    b.accessIndex = 1;
    b.address = 128;
    h.Add(a);
    h.Add(b);
    h.Complete(10, 2, 2);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}
TEST(VfRacecheck, OuterWaitOrdersEveryInternalGroup)
{
    for (bool synchronized : {false, true}) {
        Harness h;
        auto producer = Access(1, 0, 0, VST, 1);
        producer.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        producer.vfInstrValid = 0;
        producer.instructionId = 0;
        producer.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        producer.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
        h.Add(producer);
        if (synchronized) {
            h.Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_VECTOR);
            h.Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_VECTOR);
        }
        h.Add(Access(10, 0, 0, VLD));
        h.Add(Access(10, 1, 1, SLD));
        h.Complete(10, 2, 2);
        h.End();
        EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.empty(), synchronized);
    }
}
TEST(VfRacecheck, SuccessEmptyAndMissingExpansionAreDifferent)
{
    Harness empty;
    empty.Complete(10, 0, 0);
    empty.End();
    EXPECT_TRUE(empty.checker.AnalysisComplete()) << empty.checker.Summary();
    Harness missing;
    missing.Add(Access(10, 0, 0, VST, 1));
    missing.End();
    EXPECT_FALSE(missing.checker.AnalysisComplete());
}
TEST(VfRacecheck, QueryFailurePoisonsLaunchEvenWhenOtherMemoryExists)
{
    Harness h;
    auto a = Access(1, 0, 0, VST, 1);
    a.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    a.vfInstrValid = 0;
    a.instructionId = 0;
    a.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    a.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    h.Add(a);
    h.Complete(10, 0, 0, false);
    h.End();
    EXPECT_FALSE(h.checker.AnalysisComplete());
}
TEST(VfRacecheck, MissingBarrierCannotCertifyCompleteExpansion)
{
    Harness h;
    h.Add(Access(10, 0, 0, VLD));
    h.Add(Access(10, 1, 1, SLD));
    h.Complete(10, 2, 3);
    h.End();
    EXPECT_FALSE(h.checker.AnalysisComplete());
}
TEST(VfRacecheck, FollowingVfStartsAfterEveryPriorBranch)
{
    Harness h;
    h.Add(Access(10, 0, 0, VLD));
    h.Add(Access(10, 1, 1, SLD));
    h.Complete(10, 2, 2);
    h.Add(Access(20, 0, 0, VST, 1));
    h.Complete(20, 1, 1);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(VfRacecheck, DirectionalBarriersNeverReportSameVfPairs)
{
    const uint32_t modes[] = {VLD, VST, SLD, SST};
    const std::pair<uint32_t, uint32_t> selectors[] = {{3, 3}, {2, 1}, {1, 2},  {2, 2}, {3, 12}, {2, 4},
                                                       {1, 8}, {2, 8}, {12, 3}, {8, 1}, {4, 2},  {8, 2}};
    for (auto selector : selectors) {
        for (size_t src = 0; src < 4; ++src) {
            for (size_t dst = 0; dst < 4; ++dst) {
                if ((src % 2) == 0 && (dst % 2) == 0) {
                    continue; // read/read has no hazard
                }
                Harness h;
                h.Add(Access(10, 0, 0, modes[src]));
                h.Bar(10, 1, 1, selector.first, selector.second);
                auto b = Access(10, 2, 2, modes[dst]);
                b.accessIndex = 0;
                h.Add(b);
                h.Complete(10, 2, 3);
                h.End();
                ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
                EXPECT_TRUE(h.reports.empty()) << selector.first << ':' << selector.second << ' ' << src << ' ' << dst;
            }
        }
    }
}
TEST(VfRacecheck, CompletionOrdersEveryBranchBeforeFollowingMte3)
{
    for (bool synchronized : {false, true}) {
        Harness h;
        h.Add(Access(10, 0, 0, VLD));
        h.Add(Access(10, 1, 1, SLD));
        h.Complete(10, 2, 2);
        if (synchronized) {
            h.Flag(11, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE3);
            h.Flag(12, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE3);
        }
        auto a = Access(20, 0, 0, VST, 1);
        a.vfInstrValid = 0;
        a.instructionId = 0;
        a.header.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
        a.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        h.Add(a);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.empty(), synchronized);
    }
}
TEST(VfRacecheck, OperandAndEnvelopeDuplicatesMustAgree)
{
    for (unsigned mismatch = 0; mismatch < 4; ++mismatch) {
        Harness h;
        auto a = Access(10, 0, 0, VST, 1);
        h.Add(a);
        h.Complete(10, 1, 1);
        if (mismatch == 1) {
            ++a.vfInstrExecSeq;
        }
        if (mismatch == 2) {
            ++a.header.serialNo;
        }
        h.Add(a);
        h.Complete(10, mismatch == 3 ? 2 : 1, 1);
        h.End();
        EXPECT_EQ(h.checker.AnalysisComplete(), mismatch == 0) << h.checker.Summary();
    }
}
TEST(VfRacecheck, NoncontiguousGroupAndMissingAccessAreIncomplete)
{
    for (bool missing : {false, true}) {
        Harness h;
        h.Add(Access(10, 7, 0, VST, 3));
        if (!missing) {
            h.Add(Access(10, 8, 1, VLD, 3));
        }
        h.Add(Access(10, 7, 2, VST, 3));
        h.Complete(10, 3, 3);
        h.End();
        EXPECT_FALSE(h.checker.AnalysisComplete());
    }
}
TEST(VfRacecheck, MaskWarningDoesNotHideConflictOrInvalidateGraph)
{
    Harness h;
    h.Add(Access(10, 0, 0, VST));
    h.Add(Access(10, 1, 1, VLD));
    h.Complete(10, 2, 2, true, ACLSAN_DEVICE_VF_MASK_NOT_REFRESHED);
    h.End();
    ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
    EXPECT_NE(h.checker.Summary().find("warnings=1"), std::string::npos);
}
TEST(VfRacecheck, RemoteOwnerUsesExecutorForSynchronizationAndOwnerForMemory)
{
    for (bool sameOwner : {false, true}) {
        Harness h;
        auto a = Access(1, 0, 0, VST, 1);
        a.vfInstrValid = 0;
        a.instructionId = 0;
        a.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        a.header.pipeline = ACLSAN_DEVICE_PIPE_MTE1;
        a.header.executor.phyCoreId = 0;
        a.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        a.memoryOwner.phyCoreId = sameOwner ? 18 : 19;
        a.memoryOwner.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        h.Add(a);
        h.Add(Access(10, 0, 0, VLD, 1));
        h.Complete(10, 1, 1);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.size(), sameOwner ? 1U : 0U);
    }
}
TEST(VfRacecheck, TranslatedRemoteTransferKeepsOneExecutorInstruction)
{
    aclsan::ParsedTraceRecord raw{};
    raw.launchId = 1;
    raw.instrExecId = 1;
    raw.phyCoreId = 0;
    raw.blockId = 3;
    raw.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    raw.physicalCoreSlotCount = 108;
    raw.record.instrId = 158;
    raw.record.pipeline = ACLSAN_DEVICE_PIPE_MTE1;
    raw.record.args[0] = 0;
    raw.record.args[1] = 128;
    raw.record.args[2] = (1ULL << 4) | (1ULL << 16);
    const auto decoded = aclsan::dav3510::GetDeviceInstructionDecoder().decode(raw.record);
    ASSERT_TRUE(decoded);
    const auto data = aclsan::TranslateDecodedTraceToCallbackData(raw, *decoded);
    ASSERT_TRUE(data);
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*data);
    ASSERT_NE(accesses, nullptr);
    ASSERT_EQ(accesses->size(), 2U);
    Harness h;
    for (const auto& a : *accesses) {
        EXPECT_EQ(a.header.executor.phyCoreId, raw.phyCoreId);
        EXPECT_EQ(a.header.executor.blockId, raw.blockId);
        EXPECT_EQ(a.header.executor.blockType, raw.blockType);
        EXPECT_EQ(a.accessCount, 2U);
        h.Add(a);
        if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB) {
            EXPECT_EQ(a.memoryOwner.phyCoreId, 18U);
        }
    }
    h.Add(Access(10, 0, 0, VLD, 1));
    h.Complete(10, 1, 1);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_EQ(h.reports.size(), 1U);
}
TEST(VfRacecheck, ExternalScalarUbDoesNotInheritInternalScalarOrder)
{
    for (bool synchronized : {false, true}) {
        aclsan::ParsedTraceRecord raw{};
        raw.launchId = 1;
        raw.instrExecId = 1;
        raw.phyCoreId = 18;
        raw.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        raw.record.instrId = 24;
        raw.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        raw.record.args[0] = UINT64_C(0x100080000);
        raw.record.args[3] = UINT64_C(0x100000000);
        const auto decoded = aclsan::dav3510::GetDeviceInstructionDecoder().decode(raw.record);
        ASSERT_TRUE(decoded);
        const auto data = aclsan::TranslateDecodedTraceToCallbackData(raw, *decoded);
        ASSERT_TRUE(data);
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*data);
        ASSERT_NE(accesses, nullptr);
        ASSERT_EQ(accesses->size(), 1U);
        EXPECT_EQ(accesses->front().address, 0U);
        EXPECT_EQ(accesses->front().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        Harness h;
        h.Add(accesses->front());
        if (synchronized) {
            h.Flag(2, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_VECTOR);
            h.Flag(3, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_SCALAR, ACLSAN_DEVICE_PIPE_VECTOR);
        }
        h.Add(Access(10, 0, 0, SLD, 1));
        h.Complete(10, 1, 1);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.empty(), synchronized);
    }
}

TEST(VfRacecheck, DisjointVfRangesHaveDistinctReports)
{
    Harness h;
    auto prior = Access(1, 0, 0, VST, 1);
    prior.vfInstrValid = 0;
    prior.instructionId = 0;
    prior.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    prior.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    h.Add(prior);
    auto a = Access(10, 0, 0, VLD);
    a.layout.range.bytes = 32;
    a.accessCount = 2;
    auto b = a;
    b.address = 32;
    b.accessIndex = 1;
    h.Add(a);
    h.Add(b);
    h.Complete(10, 2, 2);
    h.End();
    ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    ASSERT_EQ(h.reports.size(), 2U);
    EXPECT_NE(
        std::get<NpuCheckRacecheckReport>(h.reports[0]).reportId,
        std::get<NpuCheckRacecheckReport>(h.reports[1]).reportId);
}
TEST(VfRacecheck, ConflictingBarrierPipeCannotDependOnCallbackArrivalOrder)
{
    for (bool badFirst : {false, true}) {
        Harness h;
        AclsanDeviceBarrierData b{};
        b.header = Access(10, 0, 0, VLD).header;
        b.header.size = sizeof(b);
        b.barrierKind = ACLSAN_DEVICE_BARRIER_LOCAL_MEM;
        b.srcMemType = b.dstMemType = ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL;
        for (bool bad : {badFirst, !badFirst}) {
            b.pipe = bad ? ACLSAN_DEVICE_PIPE_SCALAR : ACLSAN_DEVICE_PIPE_INVALID;
            h.Add(ACLSAN_CBID_DEVICE_PIPE_BARRIER, b);
        }
        h.Complete(10, 0, 1);
        h.End();
        EXPECT_FALSE(h.checker.AnalysisComplete());
    }
}

TEST(VfRacecheck, ConflictSplitsAreChargedToStateSegmentBudget)
{
    race::Limits limits;
    limits.stateSegments = 1;
    Harness h(limits);
    auto prior = Access(1, 0, 0, VST, 1);
    prior.vfInstrValid = 0;
    prior.instructionId = 0;
    prior.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    prior.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    prior.layout.range.bytes = 128;
    h.Add(prior);
    auto a = Access(10, 7, 0, VST, 2, 32);
    a.layout.range.bytes = 16;
    auto b = Access(10, 7, 1, VST, 2, 64);
    b.layout.range.bytes = 16;
    a.accessCount = b.accessCount = 2;
    b.accessIndex = 1;
    h.Add(a);
    h.Add(b);
    h.Complete(10, 2, 2);
    h.End();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("memory_state_budget"), std::string::npos);
}
TEST(VfRacecheck, VfEventAndGraphBudgetsFailClosed)
{
    for (bool graph : {false, true}) {
        race::Limits limits;
        if (graph) {
            limits.edges = 1;
        } else {
            limits.events = 2;
        }
        Harness h(limits);
        h.Add(Access(10, 0, 0, VST));
        h.Add(Access(10, 1, 1, VLD));
        h.Complete(10, 2, 2);
        h.End();
        EXPECT_FALSE(h.checker.AnalysisComplete());
    }
}
TEST(VfRacecheck, SamePhysicalPipeOrdersVfsAcrossBlocksRegardlessOfCallbackOrder)
{
    for (bool reverse : {false, true}) {
        Harness h;
        auto a = Access(10, 0, 0, VST, 1);
        auto b = Access(20, 0, 0, VLD, 1);
        constexpr uint32_t SLOT_ID_SHIFT = 32;
        a.header.instrExecId |= uint64_t{a.header.executor.phyCoreId} << SLOT_ID_SHIFT;
        b.header.instrExecId |= uint64_t{b.header.executor.phyCoreId} << SLOT_ID_SHIFT;
        b.header.executor.blockId = 1;
        h.Add(a);
        h.Add(b);
        auto v = AclsanDeviceVfData{};
        v.header = a.header;
        v.header.size = sizeof(v);
        v.accessCount = v.eventCount = v.rawEventCount = 1;
        auto w = v;
        w.header = b.header;
        w.header.size = sizeof(w);
        h.Add(ACLSAN_CBID_DEVICE_VF, reverse ? w : v);
        h.Add(ACLSAN_CBID_DEVICE_VF, reverse ? v : w);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_TRUE(h.reports.empty());
    }
}

TEST(VfRacecheck, VfsOnDifferentPhysicalPipesStillNeedSynchronization)
{
    Harness h;
    auto a = Access(10, 0, 0, VST, 1);
    auto b = Access(20, 0, 0, VLD, 1);
    b.header.executor.phyCoreId = 19;
    h.Add(a);
    h.Add(b);
    AclsanDeviceVfData completion{};
    completion.header = a.header;
    completion.header.size = sizeof(completion);
    completion.accessCount = completion.eventCount = completion.rawEventCount = 1;
    h.Add(ACLSAN_CBID_DEVICE_VF, completion);
    completion.header = b.header;
    completion.header.size = sizeof(completion);
    h.Add(ACLSAN_CBID_DEVICE_VF, completion);
    h.End();
    ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_EQ(h.reports.size(), 1U);
}

TEST(VfRacecheck, CrossCoreFlagOrdersRemoteWriterThroughItsExecutor)
{
    for (bool synchronized : {false, true}) {
        Harness h;
        constexpr uint32_t MIX_KERNEL_TYPE = 3;
        LaunchTopology topology{};
        topology.kernelType = MIX_KERNEL_TYPE;
        topology.kernelTypeValid = true;
        topology.aicRatio = 1;
        topology.aivRatio = 2;
        topology.ratioValid = true;
        topology.groupCount = 1;
        topology.groupCountValid = true;
        h.analysis->Synchronization()->OnKernelLaunch(1, topology, nullptr);
        auto writer = Access(1, 0, 0, VST, 1);
        writer.vfInstrValid = 0;
        writer.instructionId = 0;
        writer.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        writer.header.executor.phyCoreId = 0;
        writer.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        writer.header.pipeline = ACLSAN_DEVICE_PIPE_MTE1;
        writer.memoryOwner.phyCoreId = 18;
        writer.memoryOwner.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
        h.Add(writer);
        auto reader = Access(10, 0, 0, VLD, 1);
        reader.header.executor.phyCoreId = 18;
        reader.memoryOwner = reader.header.executor;
        if (synchronized) {
            for (bool set : {true, false}) {
                AclsanDeviceSyncData sync{};
                sync.header = set ? writer.header : reader.header;
                sync.header.instrExecId = set ? 2 : 3;
                sync.header.size = sizeof(sync);
                sync.header.sourceKind = ACLSAN_DEVICE_SOURCE_CROSS_CORE_SYNC;
                sync.syncKind = ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK;
                sync.action = set ? ACLSAN_DEVICE_SYNC_ACTION_SET : ACLSAN_DEVICE_SYNC_ACTION_WAIT;
                sync.scope = ACLSAN_DEVICE_SYNC_SCOPE_DEVICE;
                sync.mode = 4;
                sync.objectId = 8;
                sync.srcPipe = set ? ACLSAN_DEVICE_PIPE_MTE1 : 0U;
                sync.dstPipe = set ? 0U : ACLSAN_DEVICE_PIPE_VECTOR;
                h.Add(ACLSAN_CBID_DEVICE_SYNC, sync);
            }
        }
        h.Add(reader);
        AclsanDeviceVfData v{};
        v.header = reader.header;
        v.header.size = sizeof(v);
        v.accessCount = v.eventCount = v.rawEventCount = 1;
        h.Add(ACLSAN_CBID_DEVICE_VF, v);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.empty(), synchronized);
    }
}
TEST(VfRacecheck, OneExecutorCanWriteTwoIndependentUbOwners)
{
    Harness h;
    auto first = Access(1, 0, 0, VST);
    first.vfInstrValid = 0;
    first.instructionId = 0;
    first.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    first.header.executor.phyCoreId = 0;
    first.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    first.header.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    first.memoryOwner.phyCoreId = 18;
    first.memoryOwner.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    first.accessCount = 2;
    auto second = first;
    second.accessIndex = 1;
    second.memoryOwner.phyCoreId = 19;
    h.Add(first);
    h.Add(second);
    h.Add(Access(10, 0, 0, VLD, 1));
    h.Complete(10, 1, 1);
    h.End();
    ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    ASSERT_EQ(h.reports.size(), 1U);
    EXPECT_EQ(std::get<NpuCheckRacecheckReport>(h.reports[0]).key.phyCoreId, 18U);
}
TEST(VfRacecheck, ReusingInputNeedsVfReadToMte2WriteOrder)
{
    for (bool synchronized : {false, true}) {
        Harness h;
        h.Add(Access(10, 0, 0, VLD, 1));
        h.Complete(10, 1, 1);
        if (synchronized) {
            h.Flag(11, ACLSAN_DEVICE_SYNC_ACTION_SET, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE2);
            h.Flag(12, ACLSAN_DEVICE_SYNC_ACTION_WAIT, ACLSAN_DEVICE_PIPE_VECTOR, ACLSAN_DEVICE_PIPE_MTE2);
        }
        auto write = Access(20, 0, 0, VST, 1);
        write.vfInstrValid = 0;
        write.instructionId = 0;
        write.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        write.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
        h.Add(write);
        h.End();
        ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
        EXPECT_EQ(h.reports.empty(), synchronized);
    }
}
} // namespace

TEST(VfRacecheck, ManagedAnalysisSharesVfGraphAndRetainsUbAccesses)
{
    Harness h({}, true);
    h.Add(Access(10, 0, 0, VST));
    h.Add(Access(10, 1, 1, VLD));
    h.Complete(10, 2, 2);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_FALSE(h.checker.HasErrors());
    EXPECT_EQ(h.analysis->GraphBuilds(), 1U);
}

TEST(VfRacecheck, ManagedAnalysisKeepsSimtAndVfRecordsSeparate)
{
    Harness h({}, true);
    h.Add(Access(10, 0, 0, VST));
    h.Add(Access(10, 1, 1, VLD));
    h.Complete(10, 2, 2);

    auto simt = Access(20, 0, 2, VLD);
    simt.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    simt.vfInstrValid = 0;
    simt.simt.validFlags = ACLSAN_SIMT_CONTEXT_THREAD;
    simt.simt.threadX = 7;
    ASSERT_TRUE(
        h.analysis->BeginCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, &simt));
    h.analysis->EndCallback();

    const auto& launch = h.analysis->Get(1);
    ASSERT_EQ(launch.memory.size(), 2U);
    ASSERT_EQ(launch.vfs.size(), 1U);
    ASSERT_EQ(launch.simtMemory.size(), 1U);
    EXPECT_EQ(launch.simtMemory.front().simt.threadX, 7U);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_FALSE(h.checker.HasErrors());
    EXPECT_EQ(h.analysis->GraphBuilds(), 1U);
}

TEST(VfRacecheck, ManagedRacecheckRetainsDmaAccessesAroundVf)
{
    for (bool writerFirst : {false, true}) {
        for (bool synchronized : {false, true}) {
            Harness h({}, true);
            auto dma = Access(writerFirst ? 1 : 20, 0, 0, VST, 1);
            dma.vfInstrValid = 0;
            dma.instructionId = 0;
            dma.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
            dma.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
            if (writerFirst) {
                h.Add(dma);
            } else {
                h.Add(Access(10, 0, 0, VLD, 1));
                h.Complete(10, 1, 1);
            }
            if (synchronized) {
                const auto src = writerFirst ? ACLSAN_DEVICE_PIPE_MTE2 : ACLSAN_DEVICE_PIPE_VECTOR;
                const auto dst = writerFirst ? ACLSAN_DEVICE_PIPE_VECTOR : ACLSAN_DEVICE_PIPE_MTE2;
                h.Flag(writerFirst ? 2 : 11, ACLSAN_DEVICE_SYNC_ACTION_SET, src, dst);
                h.Flag(writerFirst ? 3 : 12, ACLSAN_DEVICE_SYNC_ACTION_WAIT, src, dst);
            }
            if (writerFirst) {
                h.Add(Access(10, 0, 0, VLD, 1));
                h.Complete(10, 1, 1);
            } else {
                h.Add(dma);
            }
            h.End();
            ASSERT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
            EXPECT_EQ(h.reports.empty(), synchronized) << "writerFirst=" << writerFirst << " " << h.checker.Summary();
            EXPECT_EQ(h.analysis->GraphBuilds(), 1U);
        }
    }
}

TEST(VfRacecheck, SequenceGapsDoNotImplyMissingEvents)
{
    Harness h;
    h.Add(Access(10, 0, 100, VST));
    h.Bar(10, 500, 101, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD);
    h.Add(Access(10, 1000, 102, VLD));
    h.Complete(10, 2, 3);
    h.End();
    EXPECT_TRUE(h.checker.AnalysisComplete()) << h.checker.Summary();
    EXPECT_TRUE(h.reports.empty());
}

TEST(VfRacecheck, InternalInstructionRequiresMatchingParentCompletion)
{
    Harness h;
    auto a = Access(10, 0, 0, VST);
    auto b = a;
    ++b.header.instrExecId;
    h.Add(a);
    h.Add(b);
    h.Complete(10, 2, 2);
    h.End();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("missing_vf_expansion"), std::string::npos);
}

TEST(VfRacecheck, ExecutionCannotChangeParentOrSequenceBetweenFragments)
{
    for (bool changeParent : {false, true}) {
        Harness h;
        auto a = Access(10, 0, 0, VST);
        a.accessCount = 2;
        auto b = a;
        b.accessIndex = 1;
        if (changeParent) {
            b.header.instrExecId = 20;
        } else {
            b.vfInstrExecSeq = 1;
        }
        h.Add(a);
        h.Add(b);
        h.Complete(10, 2, 2);
        h.End();
        EXPECT_FALSE(h.checker.AnalysisComplete());
        EXPECT_NE(h.checker.Summary().find("missing_access_fragment"), std::string::npos);
        if (changeParent) {
            EXPECT_NE(h.checker.Summary().find("missing_vf_expansion"), std::string::npos);
        }
    }
}

TEST(VfRacecheck, FailedLaterFixEndpointDoesNotPublishEarlierConflict)
{
    race::Limits limits;
    limits.stateSegments = 1;
    Harness h(limits);
    auto prior = Access(1, 0, 0, VST);
    prior.header.instrExecId = 1;
    prior.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    prior.vfInstrValid = 0;
    prior.instructionId = 0;
    prior.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    h.Add(prior);
    auto first = prior;
    first.header.instrExecId = 2;
    first.header.executor = {0, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE};
    first.header.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    first.accessCount = 2;
    auto second = first;
    second.accessIndex = 1;
    second.address = 128;
    h.Add(first);
    h.Add(second);
    h.End();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("memory_state_budget"), std::string::npos);
    EXPECT_FALSE(h.checker.HasErrors());
    EXPECT_TRUE(h.reports.empty());
}

TEST(VfRacecheck, UbAtomicWithoutScopeAndGranularityCannotCertifyClean)
{
    Harness h;
    auto a = Access(1, 0, 0, VST);
    a.header.instrExecId = 1;
    a.vfInstrValid = 0;
    a.instructionId = 0;
    a.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    h.Add(a);
    h.End();
    EXPECT_FALSE(h.checker.AnalysisComplete());
    EXPECT_NE(h.checker.Summary().find("atomic_semantics_unknown"), std::string::npos);
}
