// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "log_capture.h"
#include "checker/mem_state_tracker.h"
#include "acl_san/aclsan_api.h"
#include "checker/racecheck_reports.h"
#include <gtest/gtest.h>
#include <limits>
#include <algorithm>
#include <array>
#include <optional>
#include <random>
#include <set>
#include <type_traits>

namespace npucheck {
namespace {

class MemStateTrackerTest : public ::testing::Test {
protected:
    std::vector<AclsanDeviceMemoryAccessData> records;
    std::vector<AccessPair> reports;
    MemStateTracker tracker{records};

    CbIndex Add(
        AclsanDeviceMemoryAccessMode mode, uint64_t begin = 0, uint64_t end = 1024, uint32_t core = 10,
        uint32_t space = ACLSAN_DEVICE_MEMORY_SPACE_GM, uint32_t device = 0)
    {
        AclsanDeviceMemoryAccessData data{};
        data.header.version = ACLSAN_API_VERSION;
        data.header.size = sizeof(data);
        data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
        data.header.deviceId = device;
        data.header.phyCoreId = core;
        data.header.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
        data.header.blockType = space == ACLSAN_DEVICE_MEMORY_SPACE_UB ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR :
                                space == ACLSAN_DEVICE_MEMORY_SPACE_L1 ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE :
                                                                         ACLSAN_DEVICE_BLOCK_TYPE_AICORE;
        data.header.instrExecId = records.size() + 1;
        data.address = begin;
        data.memorySpace = space;
        data.accessMode = static_cast<uint32_t>(mode);
        data.dataBits = 32;
        data.alignSize = 4;
        data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        data.layout.range.bytes = end - begin;
        records.push_back(data);
        return records.size() - 1;
    }

    void Update(CbIndex index, const std::vector<AccessPair>& conflicts)
    {
        auto pending = reports;
        pending.insert(pending.end(), conflicts.begin(), conflicts.end());
        tracker.UpdateMemState(index, conflicts);
        reports.swap(pending);
    }

    void Accept(CbIndex index)
    {
        tracker.CheckPotentialRace(index);
        Update(index, {});
    }

    const SegmentTable& Gm() { return tracker.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0}); }

    void ExpectSegment(
        const SegmentTable& table, uint64_t begin, uint64_t end, MemOpState state, std::vector<CbIndex> writers,
        std::vector<CbIndex> readers)
    {
        const auto& segment = table.at(begin);
        EXPECT_EQ(segment.begin, begin);
        EXPECT_EQ(segment.end, end);
        EXPECT_EQ(segment.state, state);
        std::optional<CbIndex> ordinary;
        std::array<std::vector<CbIndex>, 3> atomic;
        for (auto index : writers) {
            const auto mode = records[index].accessMode;
            if (mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) {
                ASSERT_FALSE(ordinary.has_value());
                ordinary = index;
            } else {
                atomic.at(mode - ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD).push_back(index);
            }
        }
        EXPECT_EQ(segment.writer, ordinary);
        EXPECT_EQ(segment.atomicWriters, atomic);
        EXPECT_EQ(segment.readers, readers);
    }
};

TEST_F(MemStateTrackerTest, FirstAccessIsReadOnlyUntilUpdate)
{
    for (auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD}) {
        auto index = Add(mode, records.size() * 1024, (records.size() + 1) * 1024);
        const auto tableCount = tracker.Tables().size();
        EXPECT_TRUE(tracker.CheckPotentialRace(index).empty());
        EXPECT_EQ(tracker.Tables().size(), tableCount);
        Update(index, {});
        const auto& segment = Gm().at(index * 1024);
        EXPECT_EQ(
            segment.writer, mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE ? std::optional<CbIndex>{index} : std::nullopt);
        EXPECT_EQ(
            segment.atomicWriters[0], mode == ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD ?
                                          std::vector<CbIndex>{index} :
                                          std::vector<CbIndex>{});
        EXPECT_EQ(
            segment.readers,
            mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? std::vector<CbIndex>{index} : std::vector<CbIndex>{});
        EXPECT_EQ(
            segment.state, mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ  ? MemOpState::Read :
                           mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE ? MemOpState::Write :
                                                                       MemOpState::AtomicWrite);
    }
    EXPECT_EQ(records.size(), 3U);
    static_assert(std::is_same_v<decltype(Update(0, {})), void>);
}

TEST_F(MemStateTrackerTest, PreparedSparseNegativeStridePreservesEveryModeAndConflictBoundary)
{
    for (auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        SCOPED_TRACE(mode);
        const uint64_t base = static_cast<uint64_t>(mode) * 4096;
        const auto index = Add(mode, base + 16, base + 20);
        records[index].layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
        records[index].layout.ndAffine = {1, 4, {3, 0, 0, 0, 0}, {-8, 0, 0, 0, 0}};
        const auto prepared = racecheck::PrepareAccess(records[index]);
        const auto* storage = prepared.ranges.data();
        ASSERT_EQ(prepared.ranges.size(), 3U);
        EXPECT_EQ(prepared.mode, mode);
        EXPECT_TRUE(tracker.CheckPotentialRace(index, prepared).empty());
        tracker.UpdateMemState(index, {}, prepared);
        for (uint64_t offset : {0, 8, 16}) {
            ExpectSegment(
                Gm(), base + offset, base + offset + 4,
                mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ  ? MemOpState::Read :
                mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE ? MemOpState::Write :
                                                            MemOpState::AtomicWrite,
                mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? std::vector<CbIndex>{} : std::vector<CbIndex>{index},
                mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? std::vector<CbIndex>{index} : std::vector<CbIndex>{});
        }
        EXPECT_EQ(Gm().count(base + 4), 0U);
        EXPECT_EQ(Gm().count(base + 12), 0U);
        EXPECT_EQ(prepared.ranges.data(), storage);

        const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, base, base + 20);
        const auto current = racecheck::PrepareAccess(records[write]);
        const auto conflicts = tracker.CheckPotentialRace(write, current);
        ASSERT_EQ(conflicts.size(), 3U);
        const auto diagnostics = MakeRacecheckDiagnostics(conflicts, records, write, current);
        tracker.UpdateMemState(write, conflicts, current);
        ASSERT_EQ(diagnostics.size(), 3U);
        for (size_t i = 0; i < conflicts.size(); ++i) {
            const auto begin = base + i * 8;
            EXPECT_EQ(conflicts[i].begin, begin);
            EXPECT_EQ(conflicts[i].end, begin + 4);
            EXPECT_EQ(Gm().at(begin).state, MemOpState::Race);
            EXPECT_EQ(diagnostics[i].previousRange.begin, begin);
            EXPECT_EQ(diagnostics[i].previousRange.end, begin + 4);
            EXPECT_EQ(diagnostics[i].currentRange.begin, base);
            EXPECT_EQ(diagnostics[i].currentRange.end, base + 20);
        }
        ExpectSegment(Gm(), base + 4, base + 8, MemOpState::Write, {write}, {});
        ExpectSegment(Gm(), base + 12, base + 16, MemOpState::Write, {write}, {});
    }
}

TEST_F(MemStateTrackerTest, PreparedInactiveAccessDoesNotCreateState)
{
    const auto index = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    records[index].header.flags |= ACLSAN_DEVICE_EVENT_FLAG_PREDICATED;
    const auto prepared = racecheck::PrepareAccess(records[index]);
    EXPECT_TRUE(prepared.ranges.empty());
    EXPECT_TRUE(tracker.CheckPotentialRace(index, prepared).empty());
    tracker.UpdateMemState(index, {}, prepared);
    EXPECT_TRUE(tracker.Tables().empty());
}

TEST_F(MemStateTrackerTest, AtomicSemanticModeMatrix)
{
    // 原子访问模式：ADD=3、MIN=4、MAX=5。
    for (uint32_t previous = 3; previous <= 5; ++previous) {
        for (uint32_t current = 3; current <= 5; ++current) {
            SCOPED_TRACE(::testing::Message() << previous << "/" << current);
            const uint64_t begin = (previous * 8 + current) * 1024;
            const auto a = Add(static_cast<AclsanDeviceMemoryAccessMode>(previous), begin, begin + 4);
            ASSERT_NO_THROW(Accept(a));
            const auto b = Add(static_cast<AclsanDeviceMemoryAccessMode>(current), begin, begin + 4);
            std::vector<AccessPair> candidates;
            ASSERT_NO_THROW(candidates = tracker.CheckPotentialRace(b));
            const bool compatible = previous == current;
            ASSERT_EQ(candidates.size(), compatible ? 0U : 1U);
            if (!compatible) {
                EXPECT_EQ(candidates[0].prevIndex, a);
            }
            Update(b, {}); // 外部判序确认存在语义候选。
            ExpectSegment(Gm(), begin, begin + 4, MemOpState::AtomicWrite, {a, b}, {});
        }
    }
}

TEST_F(MemStateTrackerTest, ReadersCoverOrdinaryWriterButNotIncompatibleAtomicWriter)
{
    const auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 4);
    Accept(w);
    const auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    Accept(a);
    const auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    Accept(r);
    const auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 0, 4);
    std::vector<AccessPair> pairs;
    ASSERT_NO_THROW(pairs = tracker.CheckPotentialRace(b));
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].prevIndex, r);
    EXPECT_EQ(pairs[1].prevIndex, a);
    Update(b, {});
    ExpectSegment(Gm(), 0, 4, MemOpState::AtomicWrite, {w, a, b}, {r});
    const auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[1].prevIndex, b); // AtomicWrite 状态不能隐藏 MIN。
    Update(c, {pairs[1]});
    ExpectSegment(Gm(), 0, 4, MemOpState::Race, {w, a, b}, {r});
    EXPECT_TRUE(tracker.CheckPotentialRace(c).empty());
}

TEST_F(MemStateTrackerTest, AtomicModesUseSeparateWriterGroups)
{
    const auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    Accept(w);
    const auto min = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 0, 16);
    Accept(min);
    const auto add = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 16);
    Accept(add);
    const auto max = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX, 0, 16);
    Accept(max);
    const auto addAgain = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 16);
    const auto pairs = tracker.CheckPotentialRace(addAgain);
    std::set<CbIndex> previous;
    for (const auto& pair : pairs) {
        previous.insert(pair.prevIndex);
    }
    EXPECT_EQ(previous, (std::set<CbIndex>{w, min, max}));
    Update(addAgain, {});
    EXPECT_EQ(Gm().at(0).writer, std::optional<CbIndex>{w});
    EXPECT_EQ(Gm().at(0).atomicWriters[0], (std::vector<CbIndex>{add, addAgain}));
    EXPECT_EQ(Gm().at(0).atomicWriters[1], std::vector<CbIndex>{min});
    EXPECT_EQ(Gm().at(0).atomicWriters[2], std::vector<CbIndex>{max});
    const auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    previous.clear();
    for (const auto& pair : tracker.CheckPotentialRace(read)) {
        previous.insert(pair.prevIndex);
    }
    EXPECT_EQ(previous, (std::set<CbIndex>{w, min, add, max, addAgain}));
}

TEST_F(MemStateTrackerTest, ReadAndAtomicReplayDoNotDuplicateOutOfOrderHistory)
{
    for (auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        SCOPED_TRACE(mode);
        std::vector<CbIndex> indices;
        const uint64_t begin = records.size() * 32;
        for (size_t i = 0; i < 4; ++i) {
            indices.push_back(Add(mode, begin, begin + 16));
        }
        for (auto offset : {2U, 0U, 2U, 3U, 0U, 1U, 3U}) {
            Update(indices[offset], {});
        }
        ExpectSegment(
            Gm(), begin, begin + 16,
            mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ? MemOpState::Read : MemOpState::AtomicWrite,
            mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ?
                std::vector<CbIndex>{} :
                std::vector<CbIndex>{indices[2], indices[0], indices[3], indices[1]},
            mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ?
                std::vector<CbIndex>{indices[2], indices[0], indices[3], indices[1]} :
                std::vector<CbIndex>{});
    }
}

TEST_F(MemStateTrackerTest, InPlaceMixedAtomicSplitPreservesUncoveredAndFrozenHistory)
{
    const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32);
    Accept(write);
    const auto add = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 32);
    Accept(add);
    const auto min = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 8, 24);
    Accept(min);
    const auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 28);
    Accept(read);
    const auto max = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX, 12, 20);
    auto pairs = tracker.CheckPotentialRace(max);
    const auto found = std::find_if(pairs.begin(), pairs.end(), [&](const auto& pair) {
        return pair.prevIndex == min && pair.begin <= 14 && pair.end >= 18;
    });
    ASSERT_NE(found, pairs.end());
    auto conflict = *found;
    conflict.begin = 14;
    conflict.end = 18;
    Update(max, {conflict});
    ASSERT_EQ(Gm().size(), 9U);
    ExpectSegment(Gm(), 0, 4, MemOpState::AtomicWrite, {write, add}, {});
    ExpectSegment(Gm(), 4, 8, MemOpState::Read, {write, add}, {read});
    ExpectSegment(Gm(), 8, 12, MemOpState::Read, {write, add, min}, {read});
    ExpectSegment(Gm(), 12, 14, MemOpState::AtomicWrite, {write, add, min, max}, {read});
    ExpectSegment(Gm(), 14, 18, MemOpState::Race, {write, add, min}, {read});
    ExpectSegment(Gm(), 18, 20, MemOpState::AtomicWrite, {write, add, min, max}, {read});
    ExpectSegment(Gm(), 20, 24, MemOpState::Read, {write, add, min}, {read});
    ExpectSegment(Gm(), 24, 28, MemOpState::Read, {write, add}, {read});
    ExpectSegment(Gm(), 28, 32, MemOpState::AtomicWrite, {write, add}, {});
    Update(max, {}); // 重放不能追加重复 MAX，也不能改变已冻结 Race 边界。
    ExpectSegment(Gm(), 12, 14, MemOpState::AtomicWrite, {write, add, min, max}, {read});
    ExpectSegment(Gm(), 14, 18, MemOpState::Race, {write, add, min}, {read});
}

TEST_F(MemStateTrackerTest, AtomicRecordUsesCommonApiVersion)
{
    const auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    records[a].header.version = ACLSAN_API_VERSION;
    ASSERT_NO_THROW(Accept(a));
    ExpectSegment(Gm(), 0, 1024, MemOpState::AtomicWrite, {a}, {});
}

TEST_F(MemStateTrackerTest, IgnoresApiVersion)
{
    const auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    records[a].header.version = ACLSAN_API_VERSION + 1;
    EXPECT_NO_THROW(tracker.CheckPotentialRace(a));
    EXPECT_TRUE(tracker.Tables().empty());
}

TEST_F(MemStateTrackerTest, SwitchingAtomicModesKeepsAllRequiredWriters)
{
    const auto add = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    Accept(add);
    const auto min = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 0, 4);
    Accept(min);
    const auto max = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX, 0, 4);
    const auto mixed = tracker.CheckPotentialRace(max);
    ASSERT_EQ(mixed.size(), 2U);
    EXPECT_EQ(mixed[0].prevIndex, add);
    EXPECT_EQ(mixed[1].prevIndex, min);
    Update(max, {});
    const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 4);
    const auto ordinary = tracker.CheckPotentialRace(write);
    ASSERT_EQ(ordinary.size(), 3U);
    Update(write, {});
    ExpectSegment(Gm(), 0, 4, MemOpState::Write, {write}, {});
    const auto resumed = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, 0, 4);
    const auto next = tracker.CheckPotentialRace(resumed);
    ASSERT_EQ(next.size(), 1U);
    EXPECT_EQ(next[0].prevIndex, write);
    Update(resumed, {});
    ExpectSegment(Gm(), 0, 4, MemOpState::AtomicWrite, {write, resumed}, {});
    EXPECT_EQ(records[add].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
}

TEST_F(MemStateTrackerTest, SameCoreAndPipeStillProduceWaw)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    Accept(w);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 512, 2048);
    auto pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, w);
    EXPECT_EQ(pairs[0].curIndex, c);
    EXPECT_EQ(pairs[0].begin, 512U);
    EXPECT_EQ(pairs[0].end, 1024U);
    ExpectSegment(Gm(), 0, 1024, MemOpState::Write, {w}, {});
    Update(c, {});
    ASSERT_EQ(Gm().size(), 2U);
    ExpectSegment(Gm(), 0, 512, MemOpState::Write, {w}, {});
    ExpectSegment(Gm(), 512, 2048, MemOpState::Write, {c}, {});
}

TEST_F(MemStateTrackerTest, WrrwChecksAllReadersAndReplacesHistory)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    Accept(w);
    auto r1 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    auto pairs = tracker.CheckPotentialRace(r1);
    ASSERT_EQ(pairs.size(), 1U);
    Update(r1, {});
    auto r2 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(tracker.CheckPotentialRace(r2).at(0).prevIndex, w);
    Update(r2, {});
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].prevIndex, r1);
    EXPECT_EQ(pairs[1].prevIndex, r2);
    Update(c, {});
    ExpectSegment(Gm(), 0, 1024, MemOpState::Write, {c}, {});
}

TEST_F(MemStateTrackerTest, PartialConflictUpdatesAllRangesAndFreezesRace)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 1024, 2048);
    Accept(b);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 2560);
    auto pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 2U);
    Update(c, {pairs[1]});
    ASSERT_EQ(Gm().size(), 4U);
    ExpectSegment(Gm(), 0, 512, MemOpState::Write, {a}, {});
    ExpectSegment(Gm(), 512, 1024, MemOpState::Read, {a}, {c});
    ExpectSegment(Gm(), 1024, 2048, MemOpState::Race, {b}, {});
    ExpectSegment(Gm(), 2048, 2560, MemOpState::Read, {}, {c});
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].prevIndex, b);
    auto d = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 3072);
    pairs = tracker.CheckPotentialRace(d);
    for (const auto& pair : pairs) {
        EXPECT_TRUE(pair.end <= 1024 || pair.begin >= 2048);
    }
    Update(d, {});
    ExpectSegment(Gm(), 1024, 2048, MemOpState::Race, {b}, {});
    EXPECT_EQ(reports.size(), 1U);
}

TEST_F(MemStateTrackerTest, AtomicWritersAllRemainVisibleToNormalAccess)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    EXPECT_TRUE(tracker.CheckPotentialRace(b).empty());
    Update(b, {});
    ExpectSegment(Gm(), 0, 1024, MemOpState::AtomicWrite, {a, b}, {});
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    auto pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].prevIndex, a);
    EXPECT_EQ(pairs[1].prevIndex, b);
    Update(c, pairs);
    ExpectSegment(Gm(), 0, 1024, MemOpState::Race, {a, b}, {});
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(reports[0].prevIndex, a);
    EXPECT_EQ(reports[1].prevIndex, b);
}

TEST_F(MemStateTrackerTest, AtomicWritePreservesOrdinaryInitialization)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 4);
    Accept(w);
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    EXPECT_EQ(tracker.CheckPotentialRace(a).at(0).prevIndex, w);
    Update(a, {});
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    auto pairs = tracker.CheckPotentialRace(b);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, w);
    Update(b, pairs);
    ExpectSegment(Gm(), 0, 4, MemOpState::Race, {w, a}, {});
}

TEST_F(MemStateTrackerTest, AtomicWriteRetainsReadersAndUsesTheirTransitiveOrdering)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 4);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    Accept(r);
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    auto pairs = tracker.CheckPotentialRace(a);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, r); // 场景 14
    Update(a, {});
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    pairs = tracker.CheckPotentialRace(b);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, r); // 场景 17
    Update(b, pairs);
    ExpectSegment(Gm(), 0, 4, MemOpState::Race, {w, a}, {r});
}

TEST_F(MemStateTrackerTest, NormalWriteAfterAtomicWithReadersChecksOnlyAtomicWriters)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    Accept(r);
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    Accept(b);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    auto pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].prevIndex, a);
    EXPECT_EQ(pairs[1].prevIndex, b);
    Update(c, {});
    ExpectSegment(Gm(), 0, 1024, MemOpState::Write, {c}, {});
}

TEST_F(MemStateTrackerTest, ThreeOverlapsSplitAndLaterWriteMerges)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 4096);
    Accept(b);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 3000, 4000);
    EXPECT_TRUE(tracker.CheckPotentialRace(c).empty());
    Update(c, {});
    ASSERT_EQ(Gm().size(), 5U);
    ExpectSegment(Gm(), 3000, 4000, MemOpState::Read, {}, {b, c});
    ExpectSegment(Gm(), 4000, 4096, MemOpState::Read, {}, {b});
    auto d = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 4096);
    Accept(d);
    ASSERT_EQ(Gm().size(), 1U);
    ExpectSegment(Gm(), 0, 4096, MemOpState::Write, {d}, {});
}

TEST_F(MemStateTrackerTest, SevenStepPhysicalSpaceExample)
{
    auto u10 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    Accept(u10);
    auto u11 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024, 11, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_TRUE(tracker.CheckPotentialRace(u11).empty());
    Update(u11, {});
    auto ur = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 2048, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    Accept(ur);
    auto l1 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024, 20, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    EXPECT_TRUE(tracker.CheckPotentialRace(l1).empty());
    Update(l1, {});
    auto gm = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    Accept(gm);
    auto gr = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 2048, 11);
    auto pairs = tracker.CheckPotentialRace(gr);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, gm);
    Update(gr, pairs);
    auto uw = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 2048, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    Accept(uw);
    ASSERT_EQ(tracker.Tables().size(), 4U);
    const auto& ub10 = tracker.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_UB, 10});
    ASSERT_EQ(ub10.size(), 1U);
    ExpectSegment(ub10, 0, 2048, MemOpState::Write, {uw}, {});
    ExpectSegment(tracker.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_UB, 11}), 0, 1024, MemOpState::Write, {u11}, {});
    ExpectSegment(Gm(), 512, 1024, MemOpState::Race, {gm}, {});
    auto otherDevice = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 1024, 11, ACLSAN_DEVICE_MEMORY_SPACE_GM, 1);
    EXPECT_TRUE(tracker.CheckPotentialRace(otherDevice).empty());
}

TEST_F(MemStateTrackerTest, SparseBlockRepeatPreservesHolesAndNegativeStride)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 512, 516);
    auto& data = records[w];
    data.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    data.layout.blockRepeat = {2, 256, -512, 1, 0, 0};
    Accept(w);
    ASSERT_EQ(Gm().size(), 2U);
    ExpectSegment(Gm(), 0, 256, MemOpState::Write, {w}, {});
    ExpectSegment(Gm(), 512, 768, MemOpState::Write, {w}, {});
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 256, 512);
    EXPECT_TRUE(tracker.CheckPotentialRace(r).empty());
    Update(r, {});
    ExpectSegment(Gm(), 256, 512, MemOpState::Read, {}, {r});
}

TEST_F(MemStateTrackerTest, AscCopyUb2UbContiguousHasNoFalseConflict)
{
    // asc_copy_ub2ub(ub + 512, ub, 256)：PIPE_V 上的两次访问。
    auto source = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 256, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    auto destination = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 512, 768, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    for (auto index : {source, destination}) {
        records[index].header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
        records[index].header.instrExecId = 1001;
        records[index].accessCount = 2;
        records[index].dataBits = 8;
        records[index].alignSize = 32;
    }
    records[destination].accessIndex = 1;
    EXPECT_TRUE(tracker.CheckPotentialRace(source).empty());
    Update(source, {});
    EXPECT_TRUE(tracker.CheckPotentialRace(destination).empty());
    Update(destination, {});
    EXPECT_TRUE(reports.empty());
    const auto& ub = tracker.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_UB, 10});
    ASSERT_EQ(ub.size(), 2U);
    ExpectSegment(ub, 0, 256, MemOpState::Read, {}, {source});
    ExpectSegment(ub, 512, 768, MemOpState::Write, {destination}, {});
}

TEST_F(MemStateTrackerTest, AscCopyUb2UbInterleavedBlocksHaveNoFalseConflict)
{
    // asc_copy_ub2ub(ub + 32, ub, 4, 1, 1, 1)：跨度重叠，
    // 但实际 32 字节 DataBlock 不相交。重放归一化后的 cbdata，
    // 而不是设备轨迹；GM 传输及其同步不在本测试范围内。
    auto source = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 224, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    auto destination = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 32, 256, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    for (auto index : {source, destination}) {
        records[index].header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
        records[index].header.instrExecId = 1001;
        records[index].accessCount = 2;
        records[index].dataBits = 8;
        records[index].alignSize = 32;
        records[index].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
        records[index].layout.blockRepeat = {4, 32, 64, 1, 0, 0};
    }
    records[destination].accessIndex = 1;
    EXPECT_TRUE(tracker.CheckPotentialRace(source).empty());
    Update(source, {});
    EXPECT_TRUE(tracker.CheckPotentialRace(destination).empty());
    Update(destination, {});
    EXPECT_TRUE(reports.empty());
    const auto& ub = tracker.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_UB, 10});
    ASSERT_EQ(ub.size(), 8U);
    for (uint64_t block = 0; block < 4; ++block) {
        ExpectSegment(ub, block * 64, block * 64 + 32, MemOpState::Read, {}, {source});
        ExpectSegment(ub, block * 64 + 32, block * 64 + 64, MemOpState::Write, {destination}, {});
    }
    // 后续未同步指令仍然存在真实的 RAW 候选。
    auto consumer = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 96, 128, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    const auto pairs = tracker.CheckPotentialRace(consumer);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, destination);
    Update(consumer, pairs);
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].begin, 96U);
    EXPECT_EQ(reports[0].end, 128U);
}

TEST_F(MemStateTrackerTest, InvalidConflictsDoNotPartiallyUpdate)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 2048);
    auto pair = tracker.CheckPotentialRace(r).at(0);
    pair.curIndex = w;
    EXPECT_THROW(Update(r, {pair}), RacecheckError);
    ASSERT_EQ(Gm().size(), 1U);
    ExpectSegment(Gm(), 0, 1024, MemOpState::Write, {w}, {});
    EXPECT_TRUE(reports.empty());
    EXPECT_THROW(tracker.CheckPotentialRace(records.size()), RacecheckError);
}

TEST_F(MemStateTrackerTest, DefaultTrackerAcceptsAtomicWritesAndChecksOrdinaryAccesses)
{
    MemStateTracker defaultTracker{records};
    auto first = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    ASSERT_NO_THROW(defaultTracker.UpdateMemState(first, {}));
    auto second = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    EXPECT_TRUE(defaultTracker.CheckPotentialRace(second).empty());
    defaultTracker.UpdateMemState(second, {});

    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    const auto reads = defaultTracker.CheckPotentialRace(read);
    ASSERT_EQ(reads.size(), 2U);
    EXPECT_EQ(reads[0].prevIndex, first);
    EXPECT_EQ(reads[1].prevIndex, second);

    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    const auto writes = defaultTracker.CheckPotentialRace(write);
    ASSERT_EQ(writes.size(), 2U);
}

TEST_F(MemStateTrackerTest, UnknownModesAndInexactInputsAreRejected)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    for (uint32_t mode : {0U, 6U, 7U, std::numeric_limits<uint32_t>::max()}) {
        records[a].accessMode = mode;
        EXPECT_THROW(tracker.CheckPotentialRace(a), RacecheckError);
        EXPECT_THROW(tracker.UpdateMemState(a, {}), RacecheckError);
    }
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    for (auto flags :
         {ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED, ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED,
          ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR}) {
        records[w].header.flags = flags;
        EXPECT_THROW(tracker.CheckPotentialRace(w), RacecheckError);
    }
    EXPECT_TRUE(tracker.Tables().empty());
}

TEST_F(MemStateTrackerTest, UnknownAtomicConfigurationCannotBeCheckedOrCommittedAsOrdinaryWrite)
{
    const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    records[write].header.flags |= ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
    try {
        tracker.CheckPotentialRace(write);
        FAIL() << "unknown atomic configuration must leave race analysis incomplete";
    } catch (const RacecheckError& error) {
        EXPECT_EQ(error.Kind(), RacecheckErrorKind::IncompleteTrace);
    }
    EXPECT_THROW(tracker.UpdateMemState(write, {}), RacecheckError);
    EXPECT_TRUE(tracker.Tables().empty());
}

TEST_F(MemStateTrackerTest, RecordReallocationDoesNotInvalidateStoredIndices)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    Accept(w);
    for (size_t i = 0; i < 1000; ++i) {
        Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4096, 5120);
    }
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(tracker.CheckPotentialRace(r).at(0).prevIndex, w);
    Update(r, {});
    ExpectSegment(Gm(), 0, 1024, MemOpState::Read, {w}, {r});
}

TEST_F(MemStateTrackerTest, ReadAfterMultipleAtomicWritersChecksAllWriters)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    Accept(b);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    auto pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 2U);
    Update(c, {});
    ExpectSegment(Gm(), 0, 1024, MemOpState::Read, {a, b}, {c});
    auto d = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    pairs = tracker.CheckPotentialRace(d);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, c);
}

TEST_F(MemStateTrackerTest, ReadBeforeAtomicRemainsACandidateForAnotherAtomic)
{
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    Accept(r);
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 4);
    auto pairs = tracker.CheckPotentialRace(b);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, r);
    Update(b, pairs);
    ExpectSegment(Gm(), 0, 4, MemOpState::Race, {a}, {r});
}

TEST(FailureRangesTest, OrderedGroupsPreserveFirstEvidenceAndAdjacentBoundaries)
{
    const auto maximum = std::numeric_limits<uint64_t>::max();
    const std::vector<AccessPair> conflicts{
        {0, 6, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0, 4},   {1, 6, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0, 4},
        {2, 6, ACLSAN_DEVICE_MEMORY_SPACE_GM, 4, 8},   {3, 6, ACLSAN_DEVICE_MEMORY_SPACE_GM, 4, 8},
        {4, 6, ACLSAN_DEVICE_MEMORY_SPACE_GM, 16, 24}, {5, 6, ACLSAN_DEVICE_MEMORY_SPACE_GM, maximum - 4, maximum}};
    const auto ranges = racecheck::FailureRanges(conflicts);
    ASSERT_EQ(ranges.size(), 4U);
    const std::array<racecheck::FailedRange, 4> expected{
        {{0, 4, 0}, {4, 8, 2}, {16, 24, 4}, {maximum - 4, maximum, 5}}};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(ranges[i].begin, expected[i].begin);
        EXPECT_EQ(ranges[i].end, expected[i].end);
        EXPECT_EQ(ranges[i].evidence, expected[i].evidence);
    }
    EXPECT_TRUE(racecheck::FailureRanges({}).empty());
}

TEST(FailureRangesTest, OverlapAfterOrderedPrefixReprocessesAllEvidence)
{
    const std::vector<AccessPair> conflicts{
        {0, 4, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0, 4},
        {1, 4, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0, 4},
        {2, 4, ACLSAN_DEVICE_MEMORY_SPACE_GM, 4, 8},
        {3, 4, ACLSAN_DEVICE_MEMORY_SPACE_GM, 6, 12}};
    const auto ranges = racecheck::FailureRanges(conflicts);
    const std::array<racecheck::FailedRange, 4> expected{{{0, 4, 0}, {4, 6, 2}, {6, 8, 2}, {8, 12, 3}}};
    ASSERT_EQ(ranges.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(ranges[i].begin, expected[i].begin);
        EXPECT_EQ(ranges[i].end, expected[i].end);
        EXPECT_EQ(ranges[i].evidence, expected[i].evidence);
    }
}

TEST(FailureRangesTest, RandomizedInputsMatchBoundaryAndFirstEvidenceOracle)
{
    std::mt19937 random(20261018);
    for (size_t trial = 0; trial < 1000; ++trial) {
        SCOPED_TRACE(trial);
        std::vector<AccessPair> conflicts;
        uint64_t cursor = 0;
        for (size_t group = 0, count = random() % 24; group < count; ++group) {
            const uint64_t begin = trial % 2 == 0 ? cursor : random() % 64;
            const uint64_t end = begin + 1 + random() % 8;
            for (size_t repeat = 0, repeats = 1 + random() % 8; repeat < repeats; ++repeat) {
                conflicts.push_back({conflicts.size(), 1000, ACLSAN_DEVICE_MEMORY_SPACE_GM, begin, end});
            }
            cursor = end + random() % 3;
        }
        std::vector<uint64_t> boundaries;
        for (const auto& conflict : conflicts) {
            boundaries.push_back(conflict.begin);
            boundaries.push_back(conflict.end);
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        std::vector<racecheck::FailedRange> expected;
        for (size_t i = 1; i < boundaries.size(); ++i) {
            for (size_t evidence = 0; evidence < conflicts.size(); ++evidence) {
                const auto& conflict = conflicts[evidence];
                if (conflict.begin <= boundaries[i - 1] && boundaries[i] <= conflict.end) {
                    expected.push_back({boundaries[i - 1], boundaries[i], evidence});
                    break;
                }
            }
        }
        const auto actual = racecheck::FailureRanges(conflicts);
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(actual[i].begin, expected[i].begin);
            EXPECT_EQ(actual[i].end, expected[i].end);
            EXPECT_EQ(actual[i].evidence, expected[i].evidence);
        }
    }
}

TEST_F(MemStateTrackerTest, ReportsPreserveEveryConflictInInputOrder)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 16);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 8, 24);
    Accept(b);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32);
    auto pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 4U);
    std::reverse(pairs.begin(), pairs.end());
    const auto diagnostics = MakeRacecheckDiagnostics(pairs, records);
    ASSERT_EQ(diagnostics.size(), pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
        EXPECT_EQ(diagnostics[i].previous.header.instrExecId, records[pairs[i].prevIndex].header.instrExecId);
        EXPECT_EQ(diagnostics[i].current.header.instrExecId, records[c].header.instrExecId);
        EXPECT_EQ(diagnostics[i].checkRange.begin, pairs[i].begin);
        EXPECT_EQ(diagnostics[i].checkRange.end, pairs[i].end);
    }
    Update(c, pairs);
    ASSERT_EQ(reports.size(), 4U);
    ExpectSegment(Gm(), 24, 32, MemOpState::Read, {}, {c});
    auto next = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 24);
    EXPECT_TRUE(tracker.CheckPotentialRace(next).empty());
}

TEST_F(MemStateTrackerTest, ReportLayerDoesNotPartitionOrSelectOverlappingConflicts)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 16);
    Accept(a);
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 16);
    Accept(b);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto pairs = tracker.CheckPotentialRace(c);
    ASSERT_EQ(pairs.size(), 2U);
    // 仅验证报告生成；合成范围不提交给 tracker。
    pairs[0].end = 12;
    pairs[1].begin = 4;
    const auto diagnostics = MakeRacecheckDiagnostics({pairs[1], pairs[0]}, records);
    ASSERT_EQ(diagnostics.size(), 2U);
    EXPECT_EQ(diagnostics[0].previous.header.instrExecId, records[b].header.instrExecId);
    EXPECT_EQ(diagnostics[0].checkRange.begin, 4U);
    EXPECT_EQ(diagnostics[0].checkRange.end, 16U);
    EXPECT_EQ(diagnostics[1].previous.header.instrExecId, records[a].header.instrExecId);
    EXPECT_EQ(diagnostics[1].checkRange.begin, 0U);
    EXPECT_EQ(diagnostics[1].checkRange.end, 12U);
}

TEST_F(MemStateTrackerTest, OrdinaryOverwriteKeepsUncoveredAndRacingHistoryOnly)
{
    const auto atomic = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 32);
    Accept(atomic);
    const auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32);
    Accept(read);
    const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 4, 28);
    auto pairs = tracker.CheckPotentialRace(write);
    ASSERT_EQ(pairs.size(), 1U);
    pairs[0].begin = 12;
    pairs[0].end = 20;
    Update(write, pairs);
    ASSERT_EQ(Gm().size(), 5U);
    ExpectSegment(Gm(), 0, 4, MemOpState::Read, {atomic}, {read});
    ExpectSegment(Gm(), 4, 12, MemOpState::Write, {write}, {});
    ExpectSegment(Gm(), 12, 20, MemOpState::Race, {atomic}, {read});
    ExpectSegment(Gm(), 20, 28, MemOpState::Write, {write}, {});
    ExpectSegment(Gm(), 28, 32, MemOpState::Read, {atomic}, {read});
    EXPECT_EQ(Gm().at(4).readers.capacity(), 0U);
    EXPECT_EQ(Gm().at(20).readers.capacity(), 0U);
}

TEST_F(MemStateTrackerTest, OrdinaryOverwriteBatchesMixedHistoryBetweenNewAndFrozenRaces)
{
    const std::array<AclsanDeviceMemoryAccessMode, 8> modes{
        ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
        ACLSAN_DEVICE_MEMORY_ACCESS_READ,
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD,
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN,
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX,
        ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
        ACLSAN_DEVICE_MEMORY_ACCESS_READ,
        ACLSAN_DEVICE_MEMORY_ACCESS_WRITE};
    std::array<CbIndex, 8> old{};
    for (size_t i = 0; i < modes.size(); ++i) {
        old[i] = Add(modes[i], i * 8, i * 8 + 8);
        Accept(old[i]);
    }
    const auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 64);
    Accept(read);
    const auto freeze = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 24, 32);
    Update(freeze, tracker.CheckPotentialRace(freeze));
    const auto* frozenNode = &Gm().at(24);
    const auto* frozenReaders = frozenNode->readers.data();

    const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 4, 60);
    const auto candidates = tracker.CheckPotentialRace(write);
    std::vector<AccessPair> conflicts;
    for (const auto& pair : candidates) {
        if (pair.prevIndex == read && pair.begin == 16) {
            conflicts.push_back(pair);
            conflicts.back().begin = 18;
            conflicts.back().end = 20;
        }
        if (pair.prevIndex == old[6]) {
            conflicts.push_back(pair);
            conflicts.back().begin = 50;
            conflicts.back().end = 52;
        }
    }
    ASSERT_EQ(conflicts.size(), 2U);
    Update(write, conflicts);
    ASSERT_EQ(Gm().size(), 9U);
    ExpectSegment(Gm(), 0, 4, MemOpState::Read, {old[0]}, {read});
    ExpectSegment(Gm(), 4, 18, MemOpState::Write, {write}, {});
    ExpectSegment(Gm(), 18, 20, MemOpState::Race, {old[2]}, {read});
    ExpectSegment(Gm(), 20, 24, MemOpState::Write, {write}, {});
    ExpectSegment(Gm(), 24, 32, MemOpState::Race, {old[3]}, {read});
    ExpectSegment(Gm(), 32, 50, MemOpState::Write, {write}, {});
    ExpectSegment(Gm(), 50, 52, MemOpState::Race, {}, {old[6], read});
    ExpectSegment(Gm(), 52, 60, MemOpState::Write, {write}, {});
    ExpectSegment(Gm(), 60, 64, MemOpState::Read, {old[7]}, {read});
    EXPECT_EQ(&Gm().at(24), frozenNode);
    EXPECT_EQ(Gm().at(24).readers.data(), frozenReaders);
    ASSERT_EQ(reports.size(), 3U); // 原冻结段一条证据，加本次两条证据。
    EXPECT_EQ(reports[1].prevIndex, read);
    EXPECT_EQ(reports[2].prevIndex, old[6]);

    const auto later = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 64);
    const auto after = tracker.CheckPotentialRace(later);
    ASSERT_EQ(after.size(), 6U);
    const std::array<uint64_t, 6> begins{0, 4, 20, 32, 52, 60};
    const std::array<uint64_t, 6> ends{4, 18, 24, 50, 60, 64};
    for (size_t i = 0; i < after.size(); ++i) {
        EXPECT_EQ(after[i].begin, begins[i]);
        EXPECT_EQ(after[i].end, ends[i]);
        EXPECT_EQ(after[i].prevIndex, i == 0 ? old[0] : i == 5 ? old[7] : write);
    }
}

TEST_F(MemStateTrackerTest, SparseOrdinaryOverwritePreservesHolesAndSpanningFrozenRaceOnReplay)
{
    const auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 64);
    Accept(read);
    const auto freeze = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 8, 24);
    Update(freeze, tracker.CheckPotentialRace(freeze));
    const auto extra = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 38, 40);
    Accept(extra);
    const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 36, 44);
    records[write].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[write].layout.blockRepeat = {3, 8, -16, 1, 0, 0};
    const auto prepared = racecheck::PrepareAccess(records[write]);
    ASSERT_EQ(prepared.ranges.size(), 3U);
    const auto* frozenNode = &Gm().at(8);
    for (size_t replay = 0; replay < 2; ++replay) {
        tracker.UpdateMemState(write, {}, prepared);
        ASSERT_EQ(Gm().size(), 7U);
        ExpectSegment(Gm(), 0, 4, MemOpState::Read, {}, {read});
        ExpectSegment(Gm(), 4, 8, MemOpState::Write, {write}, {});
        ExpectSegment(Gm(), 8, 24, MemOpState::Race, {}, {read});
        ExpectSegment(Gm(), 24, 28, MemOpState::Write, {write}, {});
        ExpectSegment(Gm(), 28, 36, MemOpState::Read, {}, {read});
        ExpectSegment(Gm(), 36, 44, MemOpState::Write, {write}, {});
        ExpectSegment(Gm(), 44, 64, MemOpState::Read, {}, {read});
        EXPECT_EQ(&Gm().at(8), frozenNode);
    }
}

TEST_F(MemStateTrackerTest, FullReadKeepsDifferentWriterAndReaderHistoriesSeparate)
{
    const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 24);
    Accept(write);
    const auto oldRead = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 24, 48);
    Accept(oldRead);
    const auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 48);
    Accept(read);
    ASSERT_EQ(Gm().size(), 2U);
    ExpectSegment(Gm(), 0, 24, MemOpState::Read, {write}, {read});
    ExpectSegment(Gm(), 24, 48, MemOpState::Read, {}, {oldRead, read});
}

TEST_F(MemStateTrackerTest, SparseAffineUpdatesOneHistoricalNodeWithoutDuplicatingHistory)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    records[r].layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    records[r].layout.ndAffine = {2, 4, {2, 2, 0, 0, 0}, {8, 16, 0, 0, 0}};
    auto pairs = tracker.CheckPotentialRace(r);
    ASSERT_EQ(pairs.size(), 4U);
    Update(r, {});
    ASSERT_EQ(Gm().size(), 8U);
    for (uint64_t begin : {0U, 8U, 16U, 24U}) {
        ExpectSegment(Gm(), begin, begin + 4, MemOpState::Read, {w}, {r});
        ExpectSegment(Gm(), begin + 4, begin + 8, MemOpState::Write, {w}, {});
    }
    Update(r, {});
    for (uint64_t begin : {0U, 8U, 16U, 24U}) {
        ExpectSegment(Gm(), begin, begin + 4, MemOpState::Read, {w}, {r});
    }
}

TEST_F(MemStateTrackerTest, AdjacentAndOverlappingLayoutRangesAreNormalized)
{
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    records[r].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[r].layout.blockRepeat = {3, 8, 4, 2, 0, 8};
    Accept(r);
    ASSERT_EQ(Gm().size(), 1U);
    ExpectSegment(Gm(), 0, 24, MemOpState::Read, {}, {r});
}

TEST_F(MemStateTrackerTest, ScalarAndNegativeAffineLayoutsUseByteIntervals)
{
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 100, 104);
    records[r].layoutKind = ACLSAN_MEM_LAYOUT_SCALAR;
    records[r].layout.scalar.bytes = 4;
    Accept(r);
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 100, 104);
    records[w].layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    records[w].layout.ndAffine = {1, 4, {3, 0, 0, 0, 0}, {-4, 0, 0, 0, 0}};
    Accept(w);
    ASSERT_EQ(Gm().size(), 1U);
    ExpectSegment(Gm(), 92, 104, MemOpState::Write, {w}, {});
}

TEST_F(MemStateTrackerTest, MasksAndInactivePredicatesHaveExplicitSemantics)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    records[w].vectorMask0 = 1;
    EXPECT_THROW(tracker.CheckPotentialRace(w), RacecheckError);
    records[w].vectorMask0 = 0;
    records[w].header.flags |= ACLSAN_DEVICE_EVENT_FLAG_PREDICATED;
    EXPECT_TRUE(tracker.CheckPotentialRace(w).empty());
    Update(w, {});
    EXPECT_TRUE(tracker.Tables().empty());
    records[w].predicateMask0 = 1;
    EXPECT_THROW(Update(w, {}), RacecheckError);
    EXPECT_TRUE(tracker.Tables().empty());
}

TEST_F(MemStateTrackerTest, InvalidLayoutsAndAddressOverflowDoNotCreateTables)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    records[w].address = std::numeric_limits<uint64_t>::max() - 3;
    records[w].layout.range.bytes = 4;
    EXPECT_THROW(Update(w, {}), RacecheckError);
    records[w].address = 0;
    records[w].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[w].layout.blockRepeat = {2, 4, -8, 1, 0, 0};
    EXPECT_THROW(tracker.CheckPotentialRace(w), RacecheckError);
    records[w].layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    records[w].layout.ndAffine = {6, 4, {1, 1, 1, 1, 1}, {0, 0, 0, 0, 0}};
    EXPECT_THROW(tracker.CheckPotentialRace(w), RacecheckError);
    records[w].layout.ndAffine.rank = 1;
    records[w].layout.ndAffine.dims[0] = 0;
    EXPECT_THROW(Update(w, {}), RacecheckError);
    records[w].layout.ndAffine.dims[0] = 1;
    records[w].accessMode = 99;
    EXPECT_THROW(tracker.CheckPotentialRace(w), RacecheckError);
    EXPECT_TRUE(tracker.Tables().empty());
}

TEST_F(MemStateTrackerTest, InPlaceAddSkipsItsSourceReadAndPreservesOtherOperandHistory)
{
    // Add(A, B, A)：源读先于目标写提交。
    auto readA = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto readB = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 32, 48);
    auto writeA = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    for (auto index : {readA, readB, writeA}) {
        records[index].header.instrExecId = 100;
        records[index].accessCount = 3;
    }
    Accept(readA);
    Accept(readB);
    EXPECT_TRUE(tracker.CheckPotentialRace(writeA).empty());
    ExpectSegment(Gm(), 0, 16, MemOpState::Read, {}, {readA});
    // 自身源读属于指令内在行为，不是 WAR 候选。
    Update(writeA, {});
    ExpectSegment(Gm(), 0, 16, MemOpState::Write, {writeA}, {});
    ExpectSegment(Gm(), 32, 48, MemOpState::Read, {}, {readB});
    EXPECT_TRUE(reports.empty());
    auto nextRead = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    const auto nextPairs = tracker.CheckPotentialRace(nextRead);
    ASSERT_EQ(nextPairs.size(), 1U);
    EXPECT_EQ(nextPairs[0].prevIndex, writeA);
}

TEST_F(MemStateTrackerTest, InPlaceAddStillChecksUnorderedAccessFromAnotherCore)
{
    auto readA = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto otherRead = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16, 11);
    auto writeA = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    // 不同核上的相同数值指令 ID 也不能标识同一次动态执行。
    // 这条说明已合并到上一行。
    for (auto index : {readA, otherRead, writeA}) {
        records[index].header.instrExecId = 100;
    }
    Accept(readA);
    Accept(otherRead);
    const auto pairs = tracker.CheckPotentialRace(writeA);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, otherRead);
    // 只有另一个核的读需要外部判序，当前场景中它是无序的。
    Update(writeA, pairs);
    ExpectSegment(Gm(), 0, 16, MemOpState::Race, {}, {readA, otherRead});
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].prevIndex, otherRead);
    EXPECT_EQ(reports[0].curIndex, writeA);
}

TEST_F(MemStateTrackerTest, OwnSourceReadCannotBeSubmittedAsAConfirmedConflict)
{
    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[write].header.instrExecId = records[read].header.instrExecId;
    Accept(read);
    const AccessPair ownPair{read, write, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0, 16};
    EXPECT_TRUE(tracker.CheckPotentialRace(write).empty());
    EXPECT_THROW(tracker.UpdateMemState(write, {ownPair}), RacecheckError);
    ExpectSegment(Gm(), 0, 16, MemOpState::Read, {}, {read});
    EXPECT_TRUE(reports.empty());
}

TEST_F(MemStateTrackerTest, SameInstructionWriteThenReadStillProducesRawCandidate)
{
    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    records[read].header.instrExecId = records[write].header.instrExecId;
    Accept(write);
    const auto pairs = tracker.CheckPotentialRace(read);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, write);
    Update(read, {});
    ExpectSegment(Gm(), 0, 16, MemOpState::Read, {write}, {read});
    EXPECT_TRUE(reports.empty());
}

TEST_F(MemStateTrackerTest, SameInstructionOverlappingReadsAreAcceptedWithoutCandidates)
{
    auto first = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto second = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 8, 24);
    records[second].header.instrExecId = records[first].header.instrExecId;
    Accept(first);
    EXPECT_TRUE(tracker.CheckPotentialRace(second).empty());
    Update(second, {});
    ExpectSegment(Gm(), 0, 8, MemOpState::Read, {}, {first});
    ExpectSegment(Gm(), 8, 16, MemOpState::Read, {}, {first, second});
    ExpectSegment(Gm(), 16, 24, MemOpState::Read, {}, {second});
}

TEST_F(MemStateTrackerTest, PartialReadCreatesAllRequiredSegments)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 512, 2048);
    Update(r, tracker.CheckPotentialRace(r));
    ASSERT_EQ(Gm().size(), 3U);
    ExpectSegment(Gm(), 0, 512, MemOpState::Write, {w}, {});
    ExpectSegment(Gm(), 512, 1024, MemOpState::Race, {w}, {});
    ExpectSegment(Gm(), 1024, 2048, MemOpState::Read, {}, {r});
}

TEST_F(MemStateTrackerTest, SparseAccessAndFullHistoryProduceAllReports)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    Accept(r);
    auto sparse = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[sparse].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[sparse].layout.blockRepeat = {2, 4, 8, 1, 0, 0};
    auto pairs = tracker.CheckPotentialRace(sparse);
    ASSERT_EQ(pairs.size(), 2U);
    auto pending = MakeRacecheckDiagnostics(pairs, records);
    ASSERT_EQ(pending.size(), pairs.size());
    Update(sparse, pairs);
    ASSERT_EQ(Gm().size(), 4U);
    ExpectSegment(Gm(), 0, 4, MemOpState::Race, {w}, {r});
    ExpectSegment(Gm(), 8, 12, MemOpState::Race, {w}, {r});
}

TEST_F(MemStateTrackerTest, BoundaryOnlyOverlapDoesNotGenerateCandidates)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 16, 32);
    EXPECT_TRUE(tracker.CheckPotentialRace(r).empty());
    Accept(r);
    auto c = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 8);
    auto pair = tracker.CheckPotentialRace(c).at(0);
    EXPECT_EQ(pair.begin, 4U);
    EXPECT_EQ(pair.end, 8U);
    EXPECT_EQ(Gm().at(0).end, 16U); // 候选检查不会裁剪或拆分历史节点。
}

TEST_F(MemStateTrackerTest, DirectTraversalPreservesCandidateOrderClippingAndHoles)
{
    const auto left = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 100, 108);
    Accept(left);
    const auto middle = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 108, 116);
    Accept(middle);
    const auto right = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 120, 128);
    Accept(right);
    const auto current = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 104, 124);
    const auto prepared = racecheck::PrepareAccess(records[current]);
    const auto candidates = tracker.CheckPotentialRace(current, prepared);
    ASSERT_EQ(candidates.size(), 2U);
    EXPECT_EQ(candidates[0].prevIndex, left);
    EXPECT_EQ(candidates[0].curIndex, current);
    EXPECT_EQ(candidates[0].begin, 104U);
    EXPECT_EQ(candidates[0].end, 108U);
    EXPECT_EQ(candidates[1].prevIndex, right);
    EXPECT_EQ(candidates[1].curIndex, current);
    EXPECT_EQ(candidates[1].begin, 120U);
    EXPECT_EQ(candidates[1].end, 124U);
    for (const auto range : std::vector<racecheck::ByteRange>{{96, 100}, {108, 116}, {116, 120}, {128, 132}}) {
        const auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, range.begin, range.end);
        EXPECT_TRUE(tracker.CheckPotentialRace(read, racecheck::PrepareAccess(records[read])).empty());
    }
    const auto otherDevice = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 100, 128, 10, ACLSAN_DEVICE_MEMORY_SPACE_GM, 1);
    EXPECT_TRUE(tracker.CheckPotentialRace(otherDevice, racecheck::PrepareAccess(records[otherDevice])).empty());
    ASSERT_EQ(tracker.Tables().size(), 1U);
    ASSERT_EQ(Gm().size(), 3U);
    ExpectSegment(Gm(), 100, 108, MemOpState::Write, {left}, {});
    ExpectSegment(Gm(), 108, 116, MemOpState::Read, {}, {middle});
    ExpectSegment(Gm(), 120, 128, MemOpState::Write, {right}, {});
}

TEST_F(MemStateTrackerTest, RandomizedOrdinaryAccessesMatchAnIndependentByteOracle)
{
    struct ByteState {
        bool visited = false;
        bool race = false;
        std::optional<CbIndex> writer;
        std::vector<CbIndex> readers;
    };
    std::array<ByteState, 64> bytes{};
    std::mt19937 random(20261014);
    for (size_t step = 0; step < 300; ++step) {
        const uint64_t begin = random() % bytes.size();
        const uint64_t end = begin + 1 + random() % (bytes.size() - begin);
        const auto mode = random() % 2 ? ACLSAN_DEVICE_MEMORY_ACCESS_READ : ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        auto c = Add(mode, begin, end);
        auto pairs = tracker.CheckPotentialRace(c);
        using CandidateByte = std::pair<CbIndex, uint64_t>;
        std::set<CandidateByte> expected;
        for (uint64_t b = begin; b < end; ++b) {
            const auto& old = bytes[b];
            if (old.race) {
                continue;
            }
            if (mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
                if (old.writer) {
                    expected.emplace(*old.writer, b);
                }
            } else if (!old.readers.empty()) {
                for (auto reader : old.readers) {
                    expected.emplace(reader, b);
                }
            } else if (old.writer) {
                expected.emplace(*old.writer, b);
            }
        }
        std::set<CandidateByte> actual;
        std::vector<AccessPair> conflicts;
        for (const auto& pair : pairs) {
            EXPECT_EQ(pair.curIndex, c);
            for (uint64_t b = pair.begin; b < pair.end; ++b) {
                actual.emplace(pair.prevIndex, b);
            }
            if (step % 29 == 0 && pair.prevIndex % 3 == 0) {
                conflicts.push_back(pair);
            }
        }
        ASSERT_EQ(actual, expected) << "step " << step;
        Update(c, conflicts);
        for (uint64_t b = begin; b < end; ++b) {
            auto& state = bytes[b];
            if (state.race) {
                continue;
            }
            for (const auto& pair : conflicts) {
                if (pair.begin <= b && b < pair.end) {
                    state.race = true;
                }
            }
            if (state.race) {
                continue;
            }
            state.visited = true;
            if (mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
                state.readers.push_back(c);
            } else {
                state.writer = c;
                state.readers.clear();
            }
        }
        std::array<bool, 64> covered{};
        uint64_t previousEnd = 0;
        for (const auto& entry : Gm()) {
            const auto& segment = entry.second;
            ASSERT_EQ(entry.first, segment.begin);
            ASSERT_LT(segment.begin, segment.end);
            ASSERT_GE(segment.begin, previousEnd);
            previousEnd = segment.end;
            for (uint64_t b = segment.begin; b < segment.end; ++b) {
                covered[b] = true;
                const auto& expectedByte = bytes[b];
                EXPECT_EQ(segment.state == MemOpState::Race, expectedByte.race);
                if (!expectedByte.race) {
                    EXPECT_EQ(segment.readers, expectedByte.readers);
                    EXPECT_EQ(segment.writer, expectedByte.writer);
                }
            }
        }
        for (size_t b = 0; b < bytes.size(); ++b) {
            EXPECT_EQ(covered[b], bytes[b].visited || bytes[b].race);
        }
    }
}

TEST_F(MemStateTrackerTest, RandomizedMixedSparseAccessesAndReplayPreserveEveryByteHistory)
{
    constexpr size_t byteCount = 64;
    struct ByteState {
        bool visited = false;
        bool race = false;
        AclsanDeviceMemoryAccessMode last = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        std::optional<CbIndex> writer;
        std::vector<CbIndex> readers;
        std::array<std::vector<CbIndex>, 3> atomic;
    };
    struct AccessCase {
        CbIndex index;
        AclsanDeviceMemoryAccessMode mode;
        std::array<bool, byteCount> mask{};
    };
    for (uint32_t seed = 0; seed < 8; ++seed) {
        std::mt19937 random(20261019 + seed);
        MemStateTracker observed{records};
        std::array<ByteState, byteCount> bytes{};
        std::vector<AccessCase> accesses;
        for (size_t step = 0; step < 200; ++step) {
            SCOPED_TRACE(::testing::Message() << "seed=" << seed << " step=" << step);
            AccessCase current{};
            if (step != 0 && step % 17 == 0) {
                current = accesses[random() % accesses.size()];
            } else {
                current.mode = static_cast<AclsanDeviceMemoryAccessMode>(1 + random() % 5);
                if (random() % 2) {
                    const uint64_t begin = random() % byteCount;
                    const uint64_t end = begin + 1 + random() % (byteCount - begin);
                    current.index = Add(current.mode, begin, end);
                    for (uint64_t b = begin; b < end; ++b) {
                        current.mask[b] = true;
                    }
                } else {
                    const uint32_t count = 1 + random() % 4;
                    const uint32_t width = 1 + random() % 4;
                    const int64_t stride = 4 + random() % 9;
                    const uint64_t span = (count - 1) * stride + width;
                    const uint64_t begin = random() % (byteCount - span + 1);
                    const bool negative = random() % 2;
                    const uint64_t origin = negative ? begin + span - width : begin;
                    current.index = Add(current.mode, origin, origin + width);
                    auto& record = records[current.index];
                    record.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
                    record.layout.blockRepeat = {count, width, negative ? -stride : stride, 1, 0, 0};
                    // 独立按块计算真实字节集合，不使用 tracker 的范围展开结果。
                    for (uint64_t block = 0; block < count; ++block) {
                        for (uint64_t offset = 0; offset < width; ++offset) {
                            current.mask[begin + block * stride + offset] = true;
                        }
                    }
                }
                accesses.push_back(current);
            }

            using CandidateByte = std::pair<CbIndex, uint64_t>;
            std::set<CandidateByte> expected;
            for (size_t b = 0; b < byteCount; ++b) {
                const auto& old = bytes[b];
                if (!current.mask[b] || old.race) {
                    continue;
                }
                auto add = [&](CbIndex index) {
                    if (index != current.index) {
                        expected.emplace(index, b);
                    }
                };
                const bool write = current.mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
                const bool atomic = current.mode >= ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
                if (write && old.last == ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
                    for (auto index : old.readers) {
                        add(index);
                    }
                    continue;
                }
                if (atomic && !old.readers.empty()) {
                    for (auto index : old.readers) {
                        add(index);
                    }
                } else if (!(write && !old.readers.empty()) && old.writer) {
                    add(*old.writer);
                }
                for (size_t group = 0; group < old.atomic.size(); ++group) {
                    if (atomic &&
                        group == static_cast<size_t>(current.mode - ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD)) {
                        continue;
                    }
                    for (auto index : old.atomic[group]) {
                        add(index);
                    }
                }
            }
            const auto prepared = racecheck::PrepareAccess(records[current.index]);
            const auto candidates = observed.CheckPotentialRace(current.index, prepared);
            std::set<CandidateByte> actual;
            for (const auto& pair : candidates) {
                EXPECT_EQ(pair.curIndex, current.index);
                for (uint64_t b = pair.begin; b < pair.end; ++b) {
                    actual.emplace(pair.prevIndex, b);
                }
            }
            ASSERT_EQ(actual, expected);
            std::vector<AccessPair> conflicts;
            if (step >= 80 && step % 23 == 0 && !candidates.empty()) {
                auto pair = candidates[random() % candidates.size()];
                pair.begin += random() % (pair.end - pair.begin);
                pair.end = pair.begin + 1;
                conflicts.push_back(pair);
            }
            observed.UpdateMemState(current.index, conflicts, prepared);
            for (size_t b = 0; b < byteCount; ++b) {
                auto& state = bytes[b];
                if (!current.mask[b] || state.race) {
                    continue;
                }
                for (const auto& pair : conflicts) {
                    if (pair.begin <= b && b < pair.end) {
                        state.race = true;
                    }
                }
                if (state.race) {
                    continue;
                }
                state.visited = true;
                state.last = current.mode;
                if (current.mode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE) {
                    state.writer = current.index;
                    state.readers.clear();
                    for (auto& group : state.atomic) {
                        group.clear();
                    }
                } else {
                    auto& history = current.mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ?
                                        state.readers :
                                        state.atomic[current.mode - ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD];
                    if (std::find(history.begin(), history.end(), current.index) == history.end()) {
                        history.push_back(current.index);
                    }
                }
            }
            std::array<bool, byteCount> covered{};
            uint64_t previousEnd = 0;
            for (const auto& entry : observed.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0})) {
                const auto& segment = entry.second;
                ASSERT_EQ(entry.first, segment.begin);
                ASSERT_LT(segment.begin, segment.end);
                ASSERT_GE(segment.begin, previousEnd);
                ASSERT_LE(segment.end, byteCount);
                previousEnd = segment.end;
                for (uint64_t b = segment.begin; b < segment.end; ++b) {
                    covered[b] = true;
                    const auto& oracle = bytes[b];
                    const auto expectedState =
                        oracle.race                                      ? MemOpState::Race :
                        oracle.last == ACLSAN_DEVICE_MEMORY_ACCESS_READ  ? MemOpState::Read :
                        oracle.last == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE ? MemOpState::Write :
                                                                           MemOpState::AtomicWrite;
                    EXPECT_EQ(segment.state, expectedState) << "byte=" << b;
                    EXPECT_EQ(segment.writer, oracle.writer) << "byte=" << b;
                    EXPECT_EQ(segment.readers, oracle.readers) << "byte=" << b;
                    EXPECT_EQ(segment.atomicWriters, oracle.atomic) << "byte=" << b;
                }
            }
            for (size_t b = 0; b < byteCount; ++b) {
                EXPECT_EQ(covered[b], bytes[b].visited || bytes[b].race);
            }
        }
    }
}

TEST_F(MemStateTrackerTest, RaceEvidenceBoundariesRemainIndependent)
{
    MemStateTracker limited{records};
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 8);
    limited.UpdateMemState(w, {});
    auto r1 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    limited.UpdateMemState(r1, limited.CheckPotentialRace(r1));
    auto r2 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 6);
    EXPECT_NO_THROW(limited.UpdateMemState(r2, limited.CheckPotentialRace(r2)));
    const auto& gm = limited.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0});
    ASSERT_EQ(gm.size(), 3U);
    ExpectSegment(gm, 0, 4, MemOpState::Race, {w}, {});
    ExpectSegment(gm, 4, 6, MemOpState::Race, {w}, {});
    ExpectSegment(gm, 6, 8, MemOpState::Write, {w}, {});
}

TEST_F(MemStateTrackerTest, V2RejectsAnotherLaunchEvenOnDisjointPhysicalTables)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    records[w].header.launchId = 41;
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4096, 4100, 20, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    records[r].header.launchId = 42;
    records[r].header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    EXPECT_THROW(tracker.CheckPotentialRace(r), RacecheckError);
    EXPECT_THROW(Update(r, {}), RacecheckError);
    EXPECT_EQ(tracker.Tables().size(), 1U);
}

TEST_F(MemStateTrackerTest, V2TrustsUpstreamAtomicLegalityAndDoesNotCompareWidths)
{
    auto a = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 16);
    records[a].dataBits = 0;
    EXPECT_NO_THROW(Accept(a));
    auto b = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 16);
    records[b].dataBits = 64;
    EXPECT_NO_THROW(Accept(b));
    ExpectSegment(Gm(), 0, 16, MemOpState::AtomicWrite, {a, b}, {});
}

TEST_F(MemStateTrackerTest, V2DifferentFirstRaceEvidenceKeepsAdjacentBoundaries)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 8);
    Accept(w);
    auto r1 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4);
    Update(r1, tracker.CheckPotentialRace(r1));
    auto r2 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 8);
    Update(r2, tracker.CheckPotentialRace(r2));
    ASSERT_EQ(Gm().size(), 2U);
    ExpectSegment(Gm(), 0, 4, MemOpState::Race, {w}, {});
    ExpectSegment(Gm(), 4, 8, MemOpState::Race, {w}, {});
}

TEST_F(MemStateTrackerTest, L1ToUbTransferUsesActualSourceAndTargetCore)
{
    auto sourceWriter = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 1024, 20, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    auto targetReader = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 512, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    auto targetWriter = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 512, 1024, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    Accept(sourceWriter);
    Accept(targetReader);
    Accept(targetWriter);
    auto sourceRead = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 256, 1280, 20, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    auto targetWrite = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 256, 1280, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    auto sourcePairs = tracker.CheckPotentialRace(sourceRead);
    ASSERT_EQ(sourcePairs.size(), 1U);
    EXPECT_EQ(sourcePairs[0].prevIndex, sourceWriter);
    Update(sourceRead, sourcePairs);
    auto targetPairs = tracker.CheckPotentialRace(targetWrite);
    ASSERT_EQ(targetPairs.size(), 2U);
    EXPECT_EQ(targetPairs[0].prevIndex, targetReader);
    EXPECT_EQ(targetPairs[1].prevIndex, targetWriter);
    Update(targetWrite, {targetPairs[0]});
    const auto& source = tracker.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_L1, 20});
    const auto& target = tracker.Tables().at({0, ACLSAN_DEVICE_MEMORY_SPACE_UB, 10});
    ExpectSegment(source, 256, 1024, MemOpState::Race, {sourceWriter}, {});
    ExpectSegment(source, 1024, 1280, MemOpState::Read, {}, {sourceRead});
    ExpectSegment(target, 256, 512, MemOpState::Race, {}, {targetReader});
    ExpectSegment(target, 512, 1280, MemOpState::Write, {targetWrite}, {});
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(reports[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    EXPECT_EQ(records[reports[0].curIndex].header.phyCoreId, 20U);
    EXPECT_EQ(reports[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(records[reports[1].curIndex].header.phyCoreId, 10U);
}

TEST_F(MemStateTrackerTest, PrivateSpacesRejectWrongOrAmbiguousPhysicalCore)
{
    auto ub = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    records[ub].header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    EXPECT_THROW(tracker.CheckPotentialRace(ub), RacecheckError);
    EXPECT_THROW(tracker.UpdateMemState(ub, {}), RacecheckError);
    EXPECT_TRUE(tracker.Tables().empty());
    records[ub].header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    Accept(ub);
    auto l1 = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16, 10, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    EXPECT_THROW(tracker.CheckPotentialRace(l1), RacecheckError);
    EXPECT_THROW(tracker.UpdateMemState(l1, {}), RacecheckError);
    EXPECT_EQ(tracker.Tables().size(), 1U);
    auto otherCore = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16, 20, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    records[otherCore].header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    EXPECT_THROW(tracker.CheckPotentialRace(otherCore), RacecheckError);
}

TEST_F(MemStateTrackerTest, ReportPreparationAndUpdateFailureLeaveCallerReportsUnchanged)
{
    MemStateTracker limited{records};
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[w].header.launchId = 17;
    limited.UpdateMemState(w, {});
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 8);
    records[r].header.launchId = 17;
    auto pairs = limited.CheckPotentialRace(r);
    auto pending = MakeRacecheckDiagnostics(pairs, records);
    ASSERT_EQ(pending.size(), 1U);
    const auto& diagnostic = pending[0];
    EXPECT_EQ(diagnostic.launchId, 17U);
    EXPECT_EQ(diagnostic.key.phyCoreId, 0U);
    EXPECT_TRUE(reports.empty());
    pairs[0].end = 17; // 报告准备之后传入无效冲突，更新必须失败。
    EXPECT_THROW(limited.UpdateMemState(r, pairs), RacecheckError);
    EXPECT_TRUE(reports.empty());
    EXPECT_EQ(limited.Tables().begin()->second.size(), 1U);
    ExpectSegment(limited.Tables().begin()->second, 0, 16, MemOpState::Write, {w}, {});
}

TEST_F(MemStateTrackerTest, CommittedSnapshotIncludesWholeAffectedUbTableAndHistory)
{
    std::string log;
    test::ScopedLogCapture capture(log);
    MemStateTracker observed{records};
    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    auto untouched = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 32, 48, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    auto otherCore = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 64, 80, 11, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    auto otherDevice = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 96, 112, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB, 1);
    auto gm = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 128, 144);
    for (auto index : {write, untouched, otherCore, otherDevice, gm}) {
        observed.UpdateMemState(index, {});
    }
    log.clear();
    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 12, 10, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    observed.UpdateMemState(read, {}); // 外部判序已移除 RAW 候选。
    const auto begin = log.find("event=memstate.begin");
    ASSERT_NE(begin, std::string::npos);
    EXPECT_LT(log.find("event=update.commit"), begin);
    const auto snapshot = log.substr(begin);
    EXPECT_NE(snapshot.find("table_segments=4"), std::string::npos);
    EXPECT_NE(snapshot.find("event=memstate.end"), std::string::npos);
    std::vector<std::string> lines;
    for (size_t start = 0, end; (end = snapshot.find('\n', start)) != std::string::npos; start = end + 1) {
        lines.push_back(snapshot.substr(start, end - start));
    }
    std::vector<std::string> segments;
    for (const auto& line : lines) {
        EXPECT_NE(line.find("device=0 space=2 table_core=10"), std::string::npos) << line;
        if (line.find("event=memstate.segment ") != std::string::npos) {
            segments.push_back(line);
        }
    }
    ASSERT_EQ(segments.size(), 4U);
    const std::array<const char*, 4> ranges = {"range=[0,4)", "range=[4,12)", "range=[12,16)", "range=[32,48)"};
    for (size_t i = 0; i < ranges.size(); ++i) {
        EXPECT_NE(segments[i].find(ranges[i]), std::string::npos);
    }
    EXPECT_NE(segments[1].find("state=Read writers=1 readers=1"), std::string::npos);
    for (const auto& expected : {std::make_pair("memstate.writer", write), std::make_pair("memstate.reader", read)}) {
        const auto line = std::find_if(lines.begin(), lines.end(), [&](const std::string& entry) {
            return entry.find(std::string("event=") + expected.first + " ") != std::string::npos &&
                   entry.find("range=[4,12)") != std::string::npos;
        });
        ASSERT_NE(line, lines.end());
        EXPECT_NE(line->find("prev=" + std::to_string(expected.second) + " "), std::string::npos);
    }
}

TEST_F(MemStateTrackerTest, CommittedSnapshotShowsMergedTableAndFrozenRace)
{
    std::string log;
    test::ScopedLogCapture capture(log);
    MemStateTracker observed{records};
    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    observed.UpdateMemState(write, {});
    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 12);
    observed.UpdateMemState(read, {});
    auto overwrite = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    log.clear();
    observed.UpdateMemState(overwrite, {});
    auto begin = log.find("event=memstate.begin");
    ASSERT_NE(begin, std::string::npos);
    auto snapshot = log.substr(begin);
    EXPECT_NE(snapshot.find("table_segments=1"), std::string::npos);
    EXPECT_NE(snapshot.find("range=[0,16)"), std::string::npos);
    EXPECT_EQ(snapshot.find("range=[4,12)"), std::string::npos);
    log.clear();
    observed.UpdateMemState(read, observed.CheckPotentialRace(read));
    begin = log.find("event=memstate.begin");
    ASSERT_NE(begin, std::string::npos);
    snapshot = log.substr(begin);
    EXPECT_NE(snapshot.find("table_segments=3"), std::string::npos);
    EXPECT_NE(snapshot.find("range=[4,12)"), std::string::npos);
    EXPECT_NE(snapshot.find("state=Race writers=1 readers=0"), std::string::npos);
    log.clear();
    EXPECT_THROW(observed.UpdateMemState(records.size(), {}), RacecheckError);
    EXPECT_NE(log.find("event=update.failed"), std::string::npos);
    EXPECT_EQ(log.find("event=memstate."), std::string::npos);
}

TEST_F(MemStateTrackerTest, CommittedSnapshotsNameEverySupportedMemorySpace)
{
    const std::array<const char*, 8> names = {"GM", "UB", "L1", "L0A", "L0B", "L0C", "BT", "FB"};
    std::string log;
    test::ScopedLogCapture capture(log);
    MemStateTracker observed{records};
    for (uint32_t space = ACLSAN_DEVICE_MEMORY_SPACE_GM; space <= ACLSAN_DEVICE_MEMORY_SPACE_FB; ++space) {
        log.clear();
        const auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 10 + space, space);
        observed.UpdateMemState(write, {});
        EXPECT_NE(log.find(std::string("memory=") + names[space - 1] + " table_segments=1"), std::string::npos)
            << "space=" << space;
    }
}

TEST_F(MemStateTrackerTest, DiagnosticsCorrelateHistoryCandidatesAndUpdateOutcomes)
{
    std::string log;
    test::ScopedLogCapture capture(log);
    MemStateTracker observed{records};
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[w].header.launchId = 99;
    records[w].header.pc = 0xabcd;
    observed.CheckPotentialRace(w);
    observed.UpdateMemState(w, {});
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 24);
    records[r].header.launchId = 99;
    auto pairs = observed.CheckPotentialRace(r);
    observed.UpdateMemState(r, pairs);
    auto later = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 24);
    records[later].header.launchId = 99;
    observed.CheckPotentialRace(later);
    observed.UpdateMemState(later, {});
    for (const char* field :
         {"launch=99",
          "cb=0",
          "space=1",
          "table_core=0",
          "core=10",
          "block=0",
          "pipe=5",
          "pc=0xabcd",
          "mode=2",
          "range=[4,16)",
          "prev=0",
          "event=check.empty_table",
          "event=check.hole",
          "event=check.previous",
          "event=history.writer",
          "event=update.split",
          "event=update.hole",
          "event=race.first",
          "event=race.frozen",
          "event=check.skip_race",
          "event=update.skip_race",
          "event=update.commit"}) {
        EXPECT_NE(log.find(field), std::string::npos) << field;
    }
    auto bad = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    records[bad].header.launchId = 100;
    EXPECT_THROW(observed.UpdateMemState(bad, {}), RacecheckError);
    EXPECT_NE(log.find("event=update.failed"), std::string::npos);
    EXPECT_NE(log.find("tracker_launch=99"), std::string::npos);
    EXPECT_NE(log.find("record belongs to another launch"), std::string::npos);
}

TEST_F(MemStateTrackerTest, DisabledDebugLoggingDoesNotAffectCheckOrCommit)
{
    std::string log;
    test::ScopedLogCapture capture(
        [](void* context, int32_t, const char* message) { static_cast<std::string*>(context)->append(message); }, &log,
        DLOG_ERROR);
    MemStateTracker observed{records};
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    EXPECT_NO_THROW(observed.UpdateMemState(w, {}));
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 8);
    auto pairs = observed.CheckPotentialRace(r);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_NO_THROW(observed.UpdateMemState(r, pairs));
    ExpectSegment(observed.Tables().begin()->second, 4, 8, MemOpState::Race, {w}, {});
    EXPECT_THROW(observed.UpdateMemState(records.size(), {}), RacecheckError);
    EXPECT_EQ(log.find("level=DEBUG"), std::string::npos);
    EXPECT_NE(log.find("event=update.failed"), std::string::npos);
}

TEST_F(MemStateTrackerTest, FailedFirstUpdateDoesNotBindLaunchOrCreateTable)
{
    MemStateTracker limited{records};
    auto bad = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[bad].header.launchId = 1;
    records[bad].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[bad].layout.blockRepeat = {0, 4, 8, 1, 0, 0};
    EXPECT_THROW(limited.UpdateMemState(bad, {}), RacecheckError);
    EXPECT_TRUE(limited.Tables().empty());
    auto good = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[good].header.launchId = 2;
    EXPECT_NO_THROW(limited.UpdateMemState(good, {}));
    EXPECT_EQ(limited.Tables().size(), 1U);
}

TEST_F(MemStateTrackerTest, NarrowVisitsToExistingRaceNeverSplitOrAppendHistory)
{
    MemStateTracker limited{records};
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    limited.UpdateMemState(w, {});
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    limited.UpdateMemState(r, limited.CheckPotentialRace(r));
    for (uint64_t begin = 1; begin < 8; ++begin) {
        auto narrow = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, begin, 16 - begin);
        EXPECT_TRUE(limited.CheckPotentialRace(narrow).empty());
        EXPECT_NO_THROW(limited.UpdateMemState(narrow, {}));
        ASSERT_EQ(limited.Tables().begin()->second.size(), 1U);
        ExpectSegment(limited.Tables().begin()->second, 0, 16, MemOpState::Race, {w}, {});
    }
}

TEST_F(MemStateTrackerTest, SparseRangesCrossAndRevisitOneFrozenRaceWithoutSplittingIt)
{
    auto w = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    Accept(w);
    auto r = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 12);
    Update(r, tracker.CheckPotentialRace(r));
    auto sparse = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 2, 6);
    records[sparse].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[sparse].layout.blockRepeat = {2, 4, 8, 1, 0, 0};
    auto pairs = tracker.CheckPotentialRace(sparse);
    ASSERT_EQ(pairs.size(), 2U);
    EXPECT_EQ(pairs[0].begin, 2U);
    EXPECT_EQ(pairs[0].end, 4U);
    EXPECT_EQ(pairs[1].begin, 12U);
    EXPECT_EQ(pairs[1].end, 14U);
    Update(sparse, pairs);
    ASSERT_EQ(Gm().size(), 5U);
    ExpectSegment(Gm(), 4, 12, MemOpState::Race, {w}, {});
    ASSERT_EQ(reports.size(), 3U);
    EXPECT_EQ(reports[0].curIndex, r);
    EXPECT_EQ(reports[0].begin, 4U);
    EXPECT_EQ(reports[0].end, 12U);
    auto inside = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 5, 6);
    records[inside].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[inside].layout.blockRepeat = {2, 1, 5, 1, 0, 0};
    EXPECT_TRUE(tracker.CheckPotentialRace(inside).empty());
    Update(inside, {});
    ASSERT_EQ(Gm().size(), 5U);
    ExpectSegment(Gm(), 4, 12, MemOpState::Race, {w}, {});
    EXPECT_EQ(reports.size(), 3U);
}

TEST_F(MemStateTrackerTest, DiagnosticsExplainSkippedSourceReadAndRetainedExternalCandidate)
{
    std::string log;
    test::ScopedLogCapture capture(log);
    MemStateTracker observed{records};
    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    auto other = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16, 11);
    for (auto index : {read, write, other}) {
        records[index].header.instrExecId = 100;
    }
    observed.UpdateMemState(read, {});
    const auto ownPairs = observed.CheckPotentialRace(write);
    EXPECT_TRUE(ownPairs.empty());
    EXPECT_NE(log.find("event=check.skip_instruction_read_write"), std::string::npos);
    EXPECT_NE(log.find("WAR same_instruction=1 ordering=instruction_read_before_write"), std::string::npos);
    observed.UpdateMemState(write, {});
    log.clear();
    ASSERT_EQ(observed.CheckPotentialRace(other).size(), 1U);
    EXPECT_NE(log.find("same_instruction=0 ordering=external"), std::string::npos);
}

TEST_F(MemStateTrackerTest, InPlacePartialOverlapStillChecksAllExternalReaders)
{
    auto ownRead = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto otherRead = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 12, 20);
    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 8, 24);
    records[write].header.instrExecId = records[ownRead].header.instrExecId;
    Accept(ownRead);
    Accept(otherRead);
    auto pairs = tracker.CheckPotentialRace(write);
    ASSERT_EQ(pairs.size(), 2U);
    for (const auto& pair : pairs) {
        EXPECT_EQ(pair.prevIndex, otherRead);
    }
    EXPECT_EQ(pairs[0].begin, 12U);
    EXPECT_EQ(pairs[0].end, 16U);
    EXPECT_EQ(pairs[1].begin, 16U);
    EXPECT_EQ(pairs[1].end, 20U);
    Update(write, {});
    ExpectSegment(Gm(), 0, 8, MemOpState::Read, {}, {ownRead});
    ExpectSegment(Gm(), 8, 24, MemOpState::Write, {write}, {});
    EXPECT_TRUE(reports.empty());
}

TEST_F(MemStateTrackerTest, MissingInstructionIdentityCannotExemptReadWrite)
{
    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 16);
    auto write = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[read].header.instrExecId = 0;
    records[write].header.instrExecId = 0;
    Accept(read);
    auto pairs = tracker.CheckPotentialRace(write);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, read);
}

TEST_F(MemStateTrackerTest, SameInstructionWritesAndAtomicDestinationStillRequireOrdering)
{
    auto firstWrite = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    auto secondWrite = Add(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16);
    records[secondWrite].header.instrExecId = records[firstWrite].header.instrExecId;
    Accept(firstWrite);
    auto pairs = tracker.CheckPotentialRace(secondWrite);
    ASSERT_EQ(pairs.size(), 1U);
    auto read = Add(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 32, 48);
    auto atomic = Add(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 32, 48);
    records[atomic].header.instrExecId = records[read].header.instrExecId;
    Accept(read);
    pairs = tracker.CheckPotentialRace(atomic);
    ASSERT_EQ(pairs.size(), 1U);
    EXPECT_EQ(pairs[0].prevIndex, read);
}

} // namespace
} // namespace npucheck
