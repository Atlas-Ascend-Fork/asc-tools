/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/racecheck/memory_access_state.h"
#include <gtest/gtest.h>
#include <array>
#include <random>

namespace npucheck::race {
namespace {
const Storage OWNER{ACLSAN_DEVICE_MEMORY_SPACE_GM, Domain{0, 0, 0, 0}};
MemoryAccessState::StatePtr Writer(size_t id)
{
    auto state = std::make_shared<MemoryAccessState::State>();
    state->writer = id;
    return state;
}
std::vector<size_t> Bytes(const MemoryAccessState& memory, size_t length)
{
    std::vector<size_t> result(length, 0);
    EXPECT_TRUE(memory.Visit(OWNER, {0, length}, [&](Interval r, const auto& s) {
        for (auto i = r.begin; i < r.end; ++i)
            result[i] = s && s->writer ? *s->writer : 0;
        return true;
    }));
    return result;
}
TEST(MemoryAccessStateTest, RandomTransactionsAgreeWithByteOracleAndCoalesce)
{
    std::mt19937 rng(349);
    MemoryAccessState memory(10000, 1U << 24);
    std::vector<size_t> oracle(256);
    for (size_t round = 0; round < 1000; ++round) {
        std::vector<MemoryAccessState::Update> plan;
        size_t cursor = rng() % 12;
        while (cursor < oracle.size()) {
            const auto end = std::min(oracle.size(), cursor + 1 + rng() % 20);
            const auto id = 1 + rng() % 5;
            plan.push_back({OWNER, {cursor, end}, Writer(id)});
            std::fill(oracle.begin() + cursor, oracle.begin() + end, id);
            cursor = end + rng() % 12;
        }
        ASSERT_TRUE(memory.Apply(plan));
        EXPECT_EQ(Bytes(memory, oracle.size()), oracle);
        size_t segments = 0;
        for (size_t i = 0; i < oracle.size(); ++i)
            if (oracle[i] && (!i || oracle[i - 1] != oracle[i]))
                ++segments;
        EXPECT_EQ(memory.SegmentCount(), segments);
        EXPECT_LT(memory.AccountedBytes(), 1U << 20);
    }
    ASSERT_TRUE(memory.Apply({{OWNER, {0, 256}, Writer(9)}}));
    EXPECT_EQ(memory.SegmentCount(), 1U);
}
TEST(MemoryAccessStateTest, FailureDoesNotApplyAnyPartialUpdate)
{
    MemoryAccessState memory(2, 1U << 20);
    ASSERT_TRUE(memory.Apply({{OWNER, {0, 64}, Writer(1)}}));
    const auto before = Bytes(memory, 128);
    const auto bytes = memory.AccountedBytes();
    EXPECT_FALSE(memory.Apply({{OWNER, {8, 16}, Writer(2)}, {OWNER, {32, 40}, Writer(3)}}));
    EXPECT_EQ(Bytes(memory, 128), before);
    EXPECT_EQ(memory.AccountedBytes(), bytes);
    EXPECT_EQ(memory.SegmentCount(), 1U);
    EXPECT_FALSE(memory.Apply({{OWNER, {1, 8}, Writer(2)}, {OWNER, {7, 10}, Writer(3)}}));
    EXPECT_EQ(Bytes(memory, 128), before);
    MemoryAccessState small(100, 1);
    EXPECT_FALSE(small.Apply({{OWNER, {0, 1}, Writer(1)}}));
    EXPECT_EQ(small.SegmentCount(), 0U);
}
TEST(MemoryAccessStateTest, ReaderFrontierStorageIsSharedAcrossSplitsAndReclaimed)
{
    MemoryAccessState memory(100, 1U << 20);
    auto readers = std::make_shared<MemoryAccessState::State>();
    readers->writer = 1;
    readers->readers.assign(1000, 2);
    ASSERT_TRUE(memory.Apply({{OWNER, {0, 64}, readers}}));
    const auto withReaders = memory.AccountedBytes();
    ASSERT_TRUE(memory.Apply({{OWNER, {8, 16}, Writer(3)}}));
    EXPECT_EQ(memory.SegmentCount(), 3U);
    ASSERT_TRUE(memory.Visit(OWNER, {16, 64}, [&](Interval, const auto& state) {
        EXPECT_EQ(state.get(), readers.get());
        return true;
    }));
    ASSERT_TRUE(memory.Apply({{OWNER, {0, 64}, Writer(4)}}));
    EXPECT_EQ(memory.SegmentCount(), 1U);
    EXPECT_LT(memory.AccountedBytes(), withReaders);
}
TEST(MemoryAccessStateTest, FullWidthRangesAndOwnerIsolationHaveNoByteAllocation)
{
    MemoryAccessState memory(100, 4096);
    auto other = OWNER;
    std::get<1>(other) = Domain{1, 0, 0, 0};
    const Interval range{1ULL << 50, UINT64_MAX};
    ASSERT_TRUE(memory.Apply({{OWNER, range, Writer(1)}, {other, range, Writer(2)}}));
    EXPECT_EQ(memory.SegmentCount(), 2U);
    for (auto owner : {OWNER, other})
        ASSERT_TRUE(memory.Visit(owner, range, [&](Interval r, const auto& state) {
            EXPECT_EQ(r.begin, range.begin);
            EXPECT_EQ(r.end, range.end);
            EXPECT_EQ(*state->writer, owner == OWNER ? 1U : 2U);
            return true;
        }));
}
} // namespace
} // namespace npucheck::race
