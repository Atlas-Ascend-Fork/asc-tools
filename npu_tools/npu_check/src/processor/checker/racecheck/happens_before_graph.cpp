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

namespace npucheck::race {
HappensBeforeGraph::HappensBeforeGraph(size_t edges, size_t work, size_t cache)
    : edgeBudget_(edges), workBudget_(work), cacheBudget_(cache)
{}
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
    while (!pending_.empty()) {
        const auto node = pending_.back();
        pending_.pop_back();
        if (work_ >= workBudget_)
            return std::nullopt;
        ++work_; // Also bound searches through nodes with no outgoing edges.
        for (const auto& e : edges_[node]) {
            if (work_ >= workBudget_)
                return std::nullopt;
            ++work_;
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
bool HappensBeforeGraph::Seal(bool complete)
{
    if (sealed_) {
        valid_ &= complete;
        return valid_;
    }
    sealed_ = true;
    valid_ &= complete;
    std::vector<size_t> degree(edges_.size());
    for (const auto& list : edges_)
        for (auto e : list)
            ++degree[e.target];
    for (size_t i = 0; i < degree.size(); ++i)
        if (!degree[i])
            order_.push_back(i);
    for (size_t i = 0; i < order_.size(); ++i)
        for (auto e : edges_[order_[i]])
            if (!--degree[e.target])
                order_.push_back(e.target);
    valid_ &= order_.size() == edges_.size();
    rank_.resize(edges_.size());
    for (size_t i = 0; i < order_.size(); ++i)
        rank_[order_[i]] = i;
    seen_.resize(edges_.size());
    pending_.reserve(edges_.size());
    return valid_;
}
HappensBeforeGraph::Path HappensBeforeGraph::Reachable(size_t from, size_t to)
{
    if (rank_[from] >= rank_[to])
        return Path::No;
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
    while (!pending_.empty() && !found) {
        auto node = pending_.back();
        pending_.pop_back();
        for (auto e : edges_[node]) {
            if (work_ >= workBudget_)
                return Path::Unknown; // Never cache an interrupted search as false.
            ++work_;
            if (e.target == to) {
                found = true;
                break;
            }
            if (rank_[e.target] < rank_[to] && seen_[e.target] != generation_) {
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
