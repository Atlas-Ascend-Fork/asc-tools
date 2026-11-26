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
#include <algorithm>
#include <limits>
#include <new>

namespace npucheck::race {
HappensBeforeGraph::HappensBeforeGraph(size_t edgeBudget, size_t workBudget, size_t cacheBudget)
    : edgeBudget_(edgeBudget), workBudget_(workBudget), cacheBudget_(cacheBudget)
{}
void HappensBeforeGraph::BeginAnalysis(size_t workBudget)
{
    analysisWork_ = 0;
    analysisBudget_ = workBudget;
}
std::vector<uint64_t> HappensBeforeGraph::BuildBoundaryReachability(
    const std::vector<size_t>& boundaries, BoundaryDirection direction, size_t byteBudget, size_t workBudget) const
{
    if (!sealed_ || !valid_ || boundaries.empty() ||
        std::any_of(boundaries.begin(), boundaries.end(), [this](size_t node) { return node >= edges_.size(); })) {
        return {};
    }
    const size_t words = (boundaries.size() - 1) / BOUNDARY_WORD_BITS + 1;
    if (edges_.size() > byteBudget / sizeof(uint64_t) / words || edges_.size() > workBudget / words ||
        edgeCount_ > workBudget / words - edges_.size()) {
        return {};
    }
    try {
        std::vector<uint64_t> table;
        if (edges_.size() > table.max_size() / words) {
            return {};
        }
        table.resize(edges_.size() * words);
        for (size_t i = 0; i < boundaries.size(); ++i) {
            table[boundaries[i] * words + i / BOUNDARY_WORD_BITS] |= uint64_t{1} << (i % BOUNDARY_WORD_BITS);
        }
        if (direction == BoundaryDirection::FromBoundary) {
            for (auto node : order_) {
                PropagateBoundaryWords(node, words, direction, table);
            }
        } else {
            for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
                PropagateBoundaryWords(*it, words, direction, table);
            }
        }
        return table;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

void HappensBeforeGraph::PropagateBoundaryWords(
    size_t node, size_t words, BoundaryDirection direction, std::vector<uint64_t>& table) const
{
    for (const auto& edge : edges_[node]) {
        const bool forward = direction == BoundaryDirection::FromBoundary;
        const auto source = (forward ? node : edge.target) * words;
        const auto target = (forward ? edge.target : node) * words;
        for (size_t word = 0; word < words; ++word) {
            table[target + word] |= table[source + word];
        }
    }
}
bool HappensBeforeGraph::SpendWork(size_t& queryWork)
{
    if (queryWork >= workBudget_ || analysisWork_ >= analysisBudget_) {
        return false;
    }
    ++queryWork;
    ++analysisWork_;
    if (work_ != std::numeric_limits<size_t>::max()) {
        ++work_;
    }
    return true;
}
size_t HappensBeforeGraph::AddNode()
{
    if (sealed_) {
        valid_ = false;
        return edges_.size();
    }
    edges_.emplace_back();
    return edges_.size() - 1;
}
bool HappensBeforeGraph::AddEdge(size_t from, size_t to, Reason reason)
{
    if (sealed_ || from >= edges_.size() || to >= edges_.size() || from == to || edgeCount_ >= edgeBudget_) {
        valid_ = false;
        return false;
    }
    edges_[from].push_back({to, reason});
    ++edgeCount_;
    return true;
}
std::optional<bool> HappensBeforeGraph::PrecedesDuringBuild(size_t from, size_t to)
{
    if (sealed_ || !valid_ || from >= edges_.size() || to >= edges_.size())
        return std::nullopt;
    if (from == to)
        return true;
    seen_.resize(edges_.size());
    if (++generation_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0);
        ++generation_;
    }
    pending_.clear();
    pending_.push_back(from);
    seen_[from] = generation_;
    // Bound each search independently. Aggregate work is telemetry: a checker
    // querying the shared graph must not spend another checker's search quota.
    size_t spent = 0;
    while (!pending_.empty()) {
        const auto node = pending_.back();
        pending_.pop_back();
        if (!SpendWork(spent)) {
            return std::nullopt;
        }
        for (const auto& e : edges_[node]) {
            if (!SpendWork(spent)) {
                return std::nullopt;
            }
            if (e.target == to)
                return true;
            if (seen_[e.target] != generation_) {
                seen_[e.target] = generation_;
                pending_.push_back(e.target);
            }
        }
    }
    return false;
}
bool HappensBeforeGraph::Seal(bool coverageComplete)
{
    if (sealed_) {
        valid_ = valid_ && coverageComplete;
        return valid_;
    }
    sealed_ = true;
    valid_ = valid_ && coverageComplete;
    std::vector<size_t> degree(edges_.size());
    for (const auto& list : edges_)
        for (auto e : list)
            ++degree[e.target];
    for (size_t i = 0; i < degree.size(); ++i)
        if (degree[i] == 0)
            order_.push_back(i);
    for (size_t i = 0; i < order_.size(); ++i)
        for (auto e : edges_[order_[i]])
            if (--degree[e.target] == 0)
                order_.push_back(e.target);
    valid_ = valid_ && order_.size() == edges_.size();
    rank_.resize(edges_.size());
    for (size_t i = 0; i < order_.size(); ++i)
        rank_[order_[i]] = i;
    seen_.resize(edges_.size());
    pending_.reserve(edges_.size());
    if (valid_) {
        // Greedy path cover over the actual edges. Each node not claimed by a
        // predecessor starts a chain; every node claims at most one successor.
        chain_.resize(edges_.size());
        size_t nextChainId = 0;
        for (auto node : order_) {
            if (chain_[node] == 0)
                chain_[node] = ++nextChainId;
            for (const auto& edge : edges_[node]) {
                if (chain_[edge.target] == 0) {
                    chain_[edge.target] = chain_[node];
                    break;
                }
            }
        }
        BuildReachabilityIntervals();
    }
    return valid_;
}

void HappensBeforeGraph::BuildReachabilityIntervals()
{
    reachabilityIntervals_.resize(edges_.size());
    struct Frame {
        size_t node;
        size_t nextEdge;
    };
    std::vector<Frame> stack;
    size_t finish = 0;
    for (const auto root : order_) {
        if (reachabilityIntervals_[root].lastReachable != 0)
            continue;
        reachabilityIntervals_[root].subtreeFirst = finish + 1;
        stack.push_back({root, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto node = frame.node;
            auto& interval = reachabilityIntervals_[node];
            if (frame.nextEdge == edges_[node].size()) {
                interval.lastReachable = ++finish;
                if (interval.firstReachable == 0)
                    interval.firstReachable = finish;
                stack.pop_back();
                continue;
            }
            const auto target = edges_[node][frame.nextEdge].target;
            auto& successor = reachabilityIntervals_[target];
            // Seal has already checked acyclicity, so no successor is active on this stack.
            if (successor.lastReachable == 0) {
                successor.subtreeFirst = finish + 1;
                stack.push_back({target, 0});
                continue;
            }
            if (interval.firstReachable == 0 || successor.firstReachable < interval.firstReachable)
                interval.firstReachable = successor.firstReachable;
            ++frame.nextEdge;
        }
    }
}

bool HappensBeforeGraph::MayReach(size_t from, size_t to) const
{
    // Every edge goes to an interval contained in its source's interval.
    const auto& source = reachabilityIntervals_[from];
    const auto& target = reachabilityIntervals_[to];
    return source.firstReachable <= target.firstReachable && target.lastReachable <= source.lastReachable;
}

bool HappensBeforeGraph::HasPathWitness(size_t from, size_t to) const
{
    if (rank_[from] > rank_[to])
        return false;
    const auto& source = reachabilityIntervals_[from];
    const auto finish = reachabilityIntervals_[to].lastReachable;
    // A DFS subtree consists only of descendants reached along real tree edges.
    return chain_[from] == chain_[to] || (source.subtreeFirst <= finish && finish <= source.lastReachable);
}

HappensBeforeGraph::Path HappensBeforeGraph::Reachable(size_t from, size_t to)
{
    if (rank_[from] >= rank_[to] || !MayReach(from, to))
        return Path::No;
    if (HasPathWitness(from, to))
        return Path::Yes;
    auto cached = cache_.find({from, to});
    if (cached != cache_.end())
        return cached->second ? Path::Yes : Path::No;
    if (++generation_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0);
        ++generation_;
    }
    pending_.clear();
    pending_.push_back(from);
    seen_[from] = generation_;
    bool found = false;
    size_t spent = 0;
    while (!pending_.empty() && !found) {
        auto node = pending_.back();
        pending_.pop_back();
        for (auto e : edges_[node]) {
            if (!SpendWork(spent)) {
                return Path::Unknown; // Never cache an interrupted search as false.
            }
            if (HasPathWitness(e.target, to)) {
                found = true;
                break;
            }
            if (rank_[e.target] < rank_[to] && seen_[e.target] != generation_ && MayReach(e.target, to)) {
                seen_[e.target] = generation_;
                pending_.push_back(e.target);
            }
        }
    }
    if (cache_.size() < cacheBudget_)
        cache_.emplace(std::make_pair(from, to), found);
    return found ? Path::Yes : Path::No;
}
HappensBeforeGraph::Relation HappensBeforeGraph::Compare(size_t a, size_t b)
{
    if (!sealed_ || !valid_ || a >= edges_.size() || b >= edges_.size() || a == b)
        return Relation::Unknown;
    bool forward = rank_[a] < rank_[b];
    auto path = forward ? Reachable(a, b) : Reachable(b, a);
    if (path == Path::Unknown)
        return Relation::Unknown;
    if (path == Path::No)
        return Relation::Unordered;
    return forward ? Relation::Before : Relation::After;
}
} // namespace npucheck::race
