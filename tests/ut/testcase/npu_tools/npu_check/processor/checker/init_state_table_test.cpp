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
#include <gtest/gtest.h>
#include <stdexcept>
#include <type_traits>

namespace npucheck {
namespace {
template <typename T>
class LocalStateTableTest : public ::testing::Test {};
using LocalTables = ::testing::Types<L1InitStateTable, L0CInitStateTable>;
TYPED_TEST_SUITE(LocalStateTableTest, LocalTables);

TYPED_TEST(LocalStateTableTest, CountsIndividualBytesAcrossPagesAndPreservesHoles)
{
    TypeParam table;
    constexpr auto page = InitStateTable::kPageBytes;
    EXPECT_EQ(table.Query(0, TypeParam::kCapacity).uninitializedBytes, TypeParam::kCapacity);
    EXPECT_EQ(table.LeafCount(), 0U);
    table.Write(page - 1, 2, 1);
    EXPECT_EQ(table.LeafCount(), 2U);
    auto q = table.Query(page - 2, 4);
    EXPECT_EQ(q.uninitializedBytes, 2U);
    EXPECT_EQ(q.firstUninitializedAddress, page - 2);
    table.Write(page + 2, 1, 2);
    q = table.Query(page - 1, 4);
    EXPECT_EQ(q.uninitializedBytes, 1U);
    EXPECT_EQ(q.firstUninitializedAddress, page + 1);
    table.Clear(page - 1, 1);
    EXPECT_EQ(table.LeafCount(), 1U);
    EXPECT_EQ(table.Query(page, 1).uninitializedBytes, 0U);
    table.Clear(0, TypeParam::kCapacity);
    EXPECT_EQ(table.LeafCount(), 0U);
}

TYPED_TEST(LocalStateTableTest, RejectsInvalidWritesWithoutChangingState)
{
    TypeParam table;
    constexpr auto capacity = TypeParam::kCapacity;
    EXPECT_THROW(table.Write(capacity - 1, 2, 0), std::out_of_range);
    EXPECT_THROW(table.Write(UINT64_MAX, 2, 0), std::out_of_range);
    EXPECT_THROW(table.Write(0, 1, 16), std::invalid_argument);
    EXPECT_THROW(table.Query(capacity, 1), std::out_of_range);
    EXPECT_THROW(table.Clear(capacity, 1), std::out_of_range);
    EXPECT_EQ(table.LeafCount(), 0U);
    EXPECT_EQ(table.Query(capacity, 0).uninitializedBytes, 0U);
    table.Write(capacity - 1, 1, 15);
    EXPECT_EQ(table.Query(capacity - 2, 2).uninitializedBytes, 1U);
    EXPECT_EQ(table.Query(capacity - 1, 1).uninitializedBytes, 0U);
    table.Write(capacity - 1, 1, 0);
    EXPECT_EQ(table.Query(0, capacity).uninitializedBytes, capacity - 1);
}

TEST(InitStateTableIndexTest, SeparatesSpaceDeviceContextLaunchAndCore)
{
    InitStateTableIndex index;
    const InitStateKey gm{2, 17};
    const auto l1 = InitStateKey::Local(2, 11, 1, 3, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    const auto l0c = InitStateKey::Local(2, 11, 1, 3, ACLSAN_DEVICE_MEMORY_SPACE_L0C);
    EXPECT_EQ(index.Find(gm), nullptr);
    EXPECT_NE(dynamic_cast<GmInitStateTable*>(&index.GetOrCreate(gm)), nullptr);
    EXPECT_NE(dynamic_cast<L1InitStateTable*>(&index.GetOrCreate(l1)), nullptr);
    EXPECT_NE(dynamic_cast<L0CInitStateTable*>(&index.GetOrCreate(l0c)), nullptr);
    index.GetOrCreate(gm).Write(0, 1, 0);
    index.GetOrCreate(l1).Write(0, 1, 0);
    EXPECT_EQ(index.GetOrCreate(l0c).Query(0, 1).uninitializedBytes, 1U);
    EXPECT_EQ(index.GetOrCreate({2, 18}).Query(0, 1).uninitializedBytes, 1U);
    EXPECT_EQ(index.GetOrCreate({3, 17}).Query(0, 1).uninitializedBytes, 1U);
    for (auto key :
         {InitStateKey::Local(3, 11, 1, 3, l1.space), InitStateKey::Local(2, 12, 1, 3, l1.space),
          InitStateKey::Local(2, 11, 2, 3, l1.space), InitStateKey::Local(2, 11, 1, 4, l1.space)}) {
        EXPECT_EQ(index.GetOrCreate(key).Query(0, 1).uninitializedBytes, 1U);
    }
    index.EraseLaunch(2, 11);
    EXPECT_EQ(index.Find(l1), nullptr);
    EXPECT_EQ(index.Find(l0c), nullptr);
    EXPECT_NE(index.Find(InitStateKey::Local(3, 11, 1, 3, l1.space)), nullptr);
    EXPECT_NE(index.Find(InitStateKey::Local(2, 12, 1, 3, l1.space)), nullptr);
    EXPECT_EQ(index.GetOrCreate(gm).Query(0, 1).uninitializedBytes, 0U);
    EXPECT_EQ(index.GetOrCreate(l1).Query(0, 1).uninitializedBytes, 1U);
    index.Erase(gm);
    EXPECT_EQ(index.GetOrCreate(gm).Query(0, 1).uninitializedBytes, 1U);
}
} // namespace
} // namespace npucheck
