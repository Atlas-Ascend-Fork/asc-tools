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
#include <algorithm>
#include <set>
#include <limits>

namespace npucheck::race {
namespace {
constexpr size_t MAP_CONTROL_BLOCK_ALLOWANCE = 64U;
constexpr size_t SEGMENT_METADATA_ALLOWANCE = 32U;
constexpr size_t PATCH_COST_MULTIPLIER = 3U;
constexpr size_t PATCH_BOOKKEEPING_ALLOWANCE = 128U;
constexpr size_t REPLACEMENT_BOOKKEEPING_ALLOWANCE = 64U;
} // namespace

size_t MemoryAccessState::Cost(const StatePtr& s)
{
    // Conservative logical accounting, including map/control-block allowances.
    // Shared states are charged per segment; this is not a process RSS measurement.
    return sizeof(Segment) + sizeof(uint64_t) + MAP_CONTROL_BLOCK_ALLOWANCE + sizeof(State) +
           SEGMENT_METADATA_ALLOWANCE + (s ? s->readers.capacity() * sizeof(size_t) : 0);
}
bool MemoryAccessState::Equal(const StatePtr& a, const StatePtr& b) { return a == b || (a && b && *a == *b); }
bool MemoryAccessState::Visit(const Storage& storage, Interval range, const Visitor& visitor) const
{
    if (range.begin >= range.end)
        return range.begin == range.end;
    auto space = spaces_.find(storage);
    if (space == spaces_.end())
        return visitor(range, {});
    const auto& tree = space->second;
    auto it = tree.upper_bound(range.begin);
    if (it != tree.begin())
        --it;
    auto cursor = range.begin;
    for (; it != tree.end() && it->first < range.end; ++it) {
        if (it->second.end <= cursor)
            continue;
        if (cursor < it->first && !visitor({cursor, it->first}, {}))
            return false;
        auto end = std::min(range.end, it->second.end);
        if (!visitor({std::max(cursor, it->first), end}, it->second.state))
            return false;
        cursor = end;
    }
    return cursor >= range.end || visitor({cursor, range.end}, {});
}
void MemoryAccessState::Split(Tree& tree, uint64_t position)
{
    auto it = tree.upper_bound(position);
    if (it == tree.begin())
        return;
    --it;
    if (it->first < position && position < it->second.end) {
        auto old = it->second;
        tree.emplace(position, old);
        it->second.end = position;
    }
}
bool MemoryAccessState::Apply(const std::vector<Update>& updates)
{
    struct Patch {
        Tree replacement;
        std::set<uint64_t> removed;
    };
    std::map<Storage, Patch> patches;
    size_t staging = 0, removedBytes = 0, removedCount = 0;
    const auto charge = [this, &staging](size_t amount) {
        if (bytes_ > byteBudget_ || staging > byteBudget_ - bytes_ || amount > byteBudget_ - bytes_ - staging)
            return false;
        staging += amount;
        peak_ = std::max(peak_, bytes_ + staging);
        return true;
    };
    try {
        for (size_t i = 0; i < updates.size(); ++i) {
            const auto& u = updates[i];
            if (!u.state || u.range.begin >= u.range.end)
                return false;
            if (i != 0 && (u.storage < updates[i - 1].storage ||
                           (u.storage == updates[i - 1].storage && u.range.begin < updates[i - 1].range.end)))
                return false;
            // Reserve a conservative bound for new endpoints, split survivors and bookkeeping.
            if (!charge(PATCH_COST_MULTIPLIER * Cost(u.state) + PATCH_BOOKKEEPING_ALLOWANCE))
                return false;
            auto& patch = patches[u.storage];
            auto source = spaces_.find(u.storage);
            if (source == spaces_.end())
                continue;
            auto it = source->second.upper_bound(u.range.begin);
            if (it != source->second.begin())
                --it;
            for (; it != source->second.end() && it->first < u.range.end; ++it) {
                if (it->second.end <= u.range.begin || patch.removed.count(it->first))
                    continue;
                if (!charge(PATCH_COST_MULTIPLIER * Cost(it->second.state) + REPLACEMENT_BOOKKEEPING_ALLOWANCE))
                    return false;
                patch.removed.insert(it->first);
                patch.replacement.emplace(*it);
                removedBytes += Cost(it->second.state);
                ++removedCount;
            }
        }
        for (const auto& u : updates) {
            auto& tree = patches.at(u.storage).replacement;
            Split(tree, u.range.begin);
            Split(tree, u.range.end);
            tree.erase(tree.lower_bound(u.range.begin), tree.lower_bound(u.range.end));
            tree.emplace(u.range.begin, Segment{u.range.end, u.state});
        }
        size_t addedBytes = 0, addedCount = 0;
        for (auto& p : patches) {
            auto& tree = p.second.replacement;
            for (auto it = tree.begin(); it != tree.end();) {
                auto next = std::next(it);
                if (next != tree.end() && it->second.end == next->first &&
                    Equal(it->second.state, next->second.state)) {
                    it->second.end = next->second.end;
                    tree.erase(next);
                } else
                    ++it;
            }
            for (const auto& s : tree) {
                addedBytes += Cost(s.second.state);
                ++addedCount;
            }
        }
        if (segments_ - removedCount > segmentBudget_ || addedCount > segmentBudget_ - (segments_ - removedCount))
            return false;
        if (bytes_ - removedBytes > byteBudget_ || addedBytes > byteBudget_ - (bytes_ - removedBytes))
            return false;
        // Allocate outer maps before touching live segments. Empty maps are not observable state.
        for (const auto& p : patches)
            spaces_.try_emplace(p.first);
        for (auto& p : patches) {
            auto& target = spaces_.at(p.first);
            for (auto key : p.second.removed)
                target.erase(key);
            while (!p.second.replacement.empty()) {
                auto node = p.second.replacement.extract(p.second.replacement.begin());
                auto it = target.insert(std::move(node)).position;
                if (it != target.begin()) {
                    auto prev = std::prev(it);
                    if (prev->second.end == it->first && Equal(prev->second.state, it->second.state)) {
                        prev->second.end = it->second.end;
                        addedBytes -= Cost(it->second.state);
                        --addedCount;
                        target.erase(it);
                        it = prev;
                    }
                }
                auto next = std::next(it);
                if (next != target.end() && it->second.end == next->first &&
                    Equal(it->second.state, next->second.state)) {
                    it->second.end = next->second.end;
                    removedBytes += Cost(next->second.state);
                    ++removedCount;
                    target.erase(next);
                }
            }
        }
        segments_ = segments_ + addedCount - removedCount;
        bytes_ = bytes_ + addedBytes - removedBytes;
        peak_ = std::max(peak_, bytes_);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
} // namespace npucheck::race
