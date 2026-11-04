/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/racecheck/happens_before_graph.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <random>

namespace npucheck::race {
namespace {
using Graph = HappensBeforeGraph;
constexpr auto EDGE = Graph::Reason::TokenMatch;
TEST(HappensBeforeGraphTest, BuildQueriesObserveNewEdgesWithoutCachingNegativeResults)
{
    Graph graph(10, 100, 10);
    const auto a = graph.AddNode(), b = graph.AddNode(), c = graph.AddNode();
    ASSERT_EQ(graph.PrecedesDuringBuild(a, c), false);
    ASSERT_TRUE(graph.AddEdge(a, b, EDGE));
    ASSERT_TRUE(graph.AddEdge(b, c, EDGE));
    EXPECT_EQ(graph.PrecedesDuringBuild(a, c), true);
    EXPECT_EQ(graph.PrecedesDuringBuild(c, a), false);
    ASSERT_TRUE(graph.Seal());
    EXPECT_EQ(graph.Compare(a, c), Graph::Relation::Before);
    EXPECT_FALSE(graph.PrecedesDuringBuild(a, c).has_value());
}

TEST(HappensBeforeGraphTest, BuildQueryBudgetExhaustionIsUnknown)
{
    Graph graph(10, 1, 10);
    const auto a = graph.AddNode(), b = graph.AddNode();
    ASSERT_TRUE(graph.AddEdge(a, b, EDGE));
    EXPECT_FALSE(graph.PrecedesDuringBuild(a, b).has_value());
}

TEST(HappensBeforeGraphTest, PathsAgreeWithIndependentTransitiveClosure)
{
    std::mt19937 rng(211);
    for (size_t cache : {0U, 100U})
        for (int trial = 0; trial < 80; ++trial) {
            Graph graph(1000, 100000, cache);
            bool paths[16][16]{};
            std::vector<size_t> ids;
            for (size_t i = 0; i < 16; ++i)
                ids.push_back(graph.AddNode());
            std::shuffle(ids.begin(), ids.end(), rng);
            for (size_t i = 0; i < 16; ++i)
                for (size_t j = i + 1; j < 16; ++j)
                    if (rng() % 4 == 0) {
                        ASSERT_TRUE(graph.AddEdge(ids[i], ids[j], EDGE));
                        paths[ids[i]][ids[j]] = true;
                    }
            for (size_t k = 0; k < 16; ++k)
                for (size_t i = 0; i < 16; ++i)
                    for (size_t j = 0; j < 16; ++j)
                        paths[i][j] |= paths[i][k] && paths[k][j];
            ASSERT_TRUE(graph.Seal());
            for (size_t i = 0; i < 16; ++i)
                for (size_t j = 0; j < 16; ++j) {
                    const auto expected = i == j      ? Graph::Relation::Unknown :
                                          paths[i][j] ? Graph::Relation::Before :
                                          paths[j][i] ? Graph::Relation::After :
                                                        Graph::Relation::Unordered;
                    EXPECT_EQ(graph.Compare(i, j), expected);
                }
        }
}
TEST(HappensBeforeGraphTest, UnknownNeverTurnsIntoNoPath)
{
    Graph graph(10, 1, 10);
    for (int i = 0; i < 4; ++i)
        graph.AddNode();
    ASSERT_TRUE(graph.AddEdge(0, 1, EDGE));
    ASSERT_TRUE(graph.AddEdge(1, 2, EDGE));
    ASSERT_TRUE(graph.Seal());
    EXPECT_EQ(graph.Compare(0, 2), Graph::Relation::Unknown);
    EXPECT_EQ(graph.Compare(0, 2), Graph::Relation::Unknown);
    // The budget bounds one search, not the session: a shallow query after an
    // exhausted deep one still answers instead of inheriting Unknown.
    EXPECT_EQ(graph.Compare(0, 1), Graph::Relation::Before);
    EXPECT_EQ(graph.QueryWork(), 3U);
    EXPECT_EQ(graph.Compare(2, 3), Graph::Relation::Unordered); // Empty adjacency needs no work.
}
TEST(HappensBeforeGraphTest, LaterQueryAnswersAfterAnExhaustedSearch)
{
    Graph graph(10, 2, 10);
    for (int i = 0; i < 3; ++i)
        graph.AddNode();
    ASSERT_TRUE(graph.AddEdge(0, 1, EDGE));
    ASSERT_TRUE(graph.AddEdge(1, 2, EDGE));
    ASSERT_TRUE(graph.Seal());
    EXPECT_EQ(graph.Compare(0, 2), Graph::Relation::Before);
}
TEST(HappensBeforeGraphTest, CyclesMissingCoverageAndPostSealMutationFailClosed)
{
    for (int mode = 0; mode < 4; ++mode) {
        Graph graph(10, 100, 10);
        graph.AddNode();
        graph.AddNode();
        ASSERT_TRUE(graph.AddEdge(0, 1, EDGE));
        if (mode == 0) {
            ASSERT_TRUE(graph.AddEdge(1, 0, EDGE));
        }
        const bool sealed = graph.Seal(mode != 1);
        EXPECT_EQ(sealed, mode >= 2);
        if (mode == 2) {
            EXPECT_FALSE(graph.AddEdge(1, 0, EDGE));
        }
        if (mode == 3)
            graph.AddNode();
        EXPECT_EQ(graph.Compare(0, 1), Graph::Relation::Unknown);
    }
}
TEST(HappensBeforeGraphTest, AggregateWorkDoesNotSpendFutureQueriesQuota)
{
    Graph graph(600, 600, 0);
    for (size_t i = 0; i <= 512; ++i) {
        graph.AddNode();
        if (i != 0) {
            ASSERT_TRUE(graph.AddEdge(i - 1, i, EDGE));
        }
    }
    ASSERT_TRUE(graph.Seal());
    for (size_t i = 0; i < 200000; ++i) {
        if (graph.Compare(0, 512) != Graph::Relation::Before) {
            FAIL() << "A previous query exhausted this query's quota at iteration " << i;
        }
    }
    EXPECT_GT(graph.QueryWork(), 100000000U);
    EXPECT_EQ(graph.Compare(0, 1), Graph::Relation::Before);
}
TEST(HappensBeforeGraphTest, CheckerAnalysisAllowancesAreIndependent)
{
    Graph graph(10, 100, 0);
    graph.AddNode();
    graph.AddNode();
    graph.AddNode();
    ASSERT_TRUE(graph.AddEdge(0, 1, EDGE));
    ASSERT_TRUE(graph.AddEdge(1, 2, EDGE));
    ASSERT_TRUE(graph.Seal());
    graph.BeginAnalysis(1);
    EXPECT_EQ(graph.Compare(0, 2), Graph::Relation::Unknown);
    // The same checker cannot silently recover by starting another search.
    EXPECT_EQ(graph.Compare(0, 1), Graph::Relation::Unknown);
    graph.BeginAnalysis(100);
    EXPECT_EQ(graph.Compare(0, 2), Graph::Relation::Before);
}
} // namespace
} // namespace npucheck::race
