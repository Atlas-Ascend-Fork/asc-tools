/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "checker/host_order.h"
#include "checker/racecheck/happens_before_graph.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <thread>
#include <vector>

namespace {
using npucheck::HostOrder;
using Graph = npucheck::race::HappensBeforeGraph;

// Mirrors MemoryAnalysis: a callback, a submission (event -> join -> entry -> exit)
// and a synchronization (join -> event), with the graph built from the same edges.
class DifferentialModel {
public:
    DifferentialModel(int threads, int streams)
        : graph_(1000000, 1000000000ULL, 0), tail_(threads + streams, kNone), pending_(threads + streams)
    {
        for (int i = 0; i < threads; ++i)
            threads_.push_back(order_.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1000 + i))));
        for (int i = 0; i < streams; ++i)
            streams_.push_back(order_.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(0x2000 + i))));
    }
    void Callback(int thread) { Issue(thread); }
    void Submit(int thread, int stream)
    {
        Issue(thread);
        Join(Stream(stream), thread);
        Issue(Stream(stream));
        Issue(Stream(stream));
    }
    void Sync(int thread, int stream)
    {
        Join(thread, Stream(stream));
        Issue(thread);
    }
    size_t Nodes() const { return nodes_; }
    std::optional<bool> GraphPrecedes(size_t first, size_t second) { return graph_.PrecedesDuringBuild(first, second); }
    std::optional<bool> OrderPrecedes(size_t first, size_t second) const { return order_.Precedes(first, second); }
    bool Complete() const { return order_.Complete(); }

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);
    size_t Issue(int who)
    {
        const auto node = graph_.AddNode();
        if (tail_[who] != kNone)
            graph_.AddEdge(tail_[who], node, Graph::Reason::HostOrder);
        for (auto source : pending_[who])
            graph_.AddEdge(source, node, Graph::Reason::HostOrder);
        pending_[who].clear();
        tail_[who] = node;
        ++nodes_;
        return order_.Issue(Actor(who));
    }
    void Join(int to, int from)
    {
        if (tail_[from] != kNone)
            pending_[to].push_back(tail_[from]);
        order_.Join(Actor(to), Actor(from));
    }
    int Stream(int index) const { return static_cast<int>(threads_.size()) + index; }
    HostOrder::ActorId Actor(int who) const
    {
        return who < static_cast<int>(threads_.size()) ? threads_[who] : streams_[who - threads_.size()];
    }

    Graph graph_;
    HostOrder order_;
    std::vector<HostOrder::ActorId> threads_;
    std::vector<HostOrder::ActorId> streams_;
    std::vector<size_t> tail_;
    std::vector<std::vector<size_t>> pending_;
    size_t nodes_ = 0;
};
} // namespace

TEST(HostOrderTest, ProgramOrderInsideOneTimeline)
{
    HostOrder order;
    const auto actor = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(1)));
    const auto first = order.Issue(actor);
    const auto second = order.Issue(actor);
    const auto self = order.Precedes(first, first);
    const auto forward = order.Precedes(first, second);
    const auto backward = order.Precedes(second, first);
    ASSERT_TRUE(self.has_value());
    ASSERT_TRUE(forward.has_value());
    ASSERT_TRUE(backward.has_value());
    EXPECT_TRUE(*self);
    EXPECT_TRUE(*forward);
    EXPECT_FALSE(*backward);
}

TEST(HostOrderTest, IndependentThreadsAreUnordered)
{
    HostOrder order;
    const auto self = std::this_thread::get_id();
    std::thread::id other;
    std::thread probe([&other]() { other = std::this_thread::get_id(); });
    probe.join();

    const auto first = order.Issue(order.ThreadActor(self));
    const auto second = order.Issue(order.ThreadActor(other));
    const auto forward = order.Precedes(first, second);
    const auto backward = order.Precedes(second, first);
    ASSERT_TRUE(forward.has_value());
    ASSERT_TRUE(backward.has_value());
    EXPECT_FALSE(*forward);
    EXPECT_FALSE(*backward);
}

TEST(HostOrderTest, SubmissionOrdersLaunchAfterSubmitter)
{
    HostOrder order;
    const auto thread = order.ThreadActor(std::this_thread::get_id());
    const auto stream = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(7)));

    const auto submit = order.Issue(thread);
    order.Join(stream, thread);
    const auto entry = order.Issue(stream);
    const auto exit = order.Issue(stream);

    const auto first = order.Precedes(submit, entry);
    const auto second = order.Precedes(entry, exit);
    const auto third = order.Precedes(exit, submit);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_TRUE(*first);
    EXPECT_TRUE(*second);
    EXPECT_FALSE(*third);
}

TEST(HostOrderTest, SynchronizationOrdersTwoStreams)
{
    HostOrder order;
    const auto thread = order.ThreadActor(std::this_thread::get_id());
    const auto first = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(11)));
    const auto second = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(12)));

    const auto submit = order.Issue(thread);
    order.Join(first, thread);
    const auto firstEntry = order.Issue(first);
    order.Join(thread, first);
    const auto sync = order.Issue(thread);
    order.Join(second, thread);
    const auto secondEntry = order.Issue(second);

    const auto a = order.Precedes(submit, firstEntry);
    const auto b = order.Precedes(firstEntry, sync);
    const auto c = order.Precedes(sync, secondEntry);
    const auto d = order.Precedes(firstEntry, secondEntry);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(*a);
    EXPECT_TRUE(*b);
    EXPECT_TRUE(*c);
    EXPECT_TRUE(*d);
}

TEST(HostOrderTest, TwoStreamsWithoutSyncStayUnordered)
{
    HostOrder order;
    const auto thread = order.ThreadActor(std::this_thread::get_id());
    const auto first = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(21)));
    const auto second = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(22)));

    order.Issue(thread);
    order.Join(first, thread);
    const auto firstEntry = order.Issue(first);
    order.Issue(thread);
    order.Join(second, thread);
    const auto secondEntry = order.Issue(second);

    const auto forward = order.Precedes(firstEntry, secondEntry);
    const auto backward = order.Precedes(secondEntry, firstEntry);
    ASSERT_TRUE(forward.has_value());
    ASSERT_TRUE(backward.has_value());
    EXPECT_FALSE(*forward);
    EXPECT_FALSE(*backward);
}

TEST(HostOrderTest, JoinWithoutFollowingEventDoesNotLeakOrdering)
{
    // The production call sites always issue an event right after a join. If a future
    // caller does not, the join must still publish only the source's last event, so the
    // answer has to match graph reachability for this sequence as well.
    HostOrder order;
    Graph graph(1000, 1000000, 0);
    const auto first = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(31)));
    const auto second = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(32)));
    const auto third = order.StreamActor(reinterpret_cast<const void*>(static_cast<uintptr_t>(33)));
    const auto issue = [&graph, &order](HostOrder::ActorId actor) {
        return std::make_pair(order.Issue(actor), graph.AddNode());
    };
    const auto source = issue(first);
    const auto tail = issue(first);
    graph.AddEdge(source.second, tail.second, Graph::Reason::HostOrder);
    const auto joined = issue(second);
    order.Join(second, first);
    order.Join(third, second);
    const auto target = issue(third);
    graph.AddEdge(joined.second, target.second, Graph::Reason::HostOrder);
    EXPECT_EQ(graph.PrecedesDuringBuild(tail.second, target.second), order.Precedes(tail.first, target.first));
}

TEST(HostOrderTest, MatchesGraphOnRandomSessions)
{
    for (unsigned seed = 1; seed <= 40; ++seed) {
        std::mt19937 rng(seed);
        const int threads = 1 + static_cast<int>(rng() % 2);
        const int streams = 1 + static_cast<int>(rng() % 3);
        DifferentialModel model(threads, streams);
        for (int step = 0; step < 40; ++step) {
            const int thread = static_cast<int>(rng() % threads);
            const int stream = static_cast<int>(rng() % streams);
            const auto pick = rng() % 100;
            if (pick < 25)
                model.Callback(thread);
            else if (pick < 65)
                model.Submit(thread, stream);
            else
                model.Sync(thread, stream);
        }
        ASSERT_TRUE(model.Complete()) << "seed " << seed;
        const auto nodes = model.Nodes();
        for (size_t first = 0; first < nodes; ++first) {
            for (size_t second = 0; second < nodes; ++second) {
                ASSERT_EQ(model.GraphPrecedes(first, second), model.OrderPrecedes(first, second))
                    << "seed " << seed << " first " << first << " second " << second;
            }
        }
    }
}
