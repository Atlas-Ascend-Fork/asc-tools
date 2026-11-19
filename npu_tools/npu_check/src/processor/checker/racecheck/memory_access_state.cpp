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
#include <iterator>
#include <set>

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
struct MemoryAccessState::UpdatePlan {
    struct Patch {
        Tree replacement;
        std::set<uint64_t> removed;
    };
    std::map<Storage, Patch> patches;
    size_t staging = 0;
    size_t removedBytes = 0;
    size_t removedCount = 0;
    size_t addedBytes = 0;
    size_t addedCount = 0;
};

bool MemoryAccessState::ChargeStaging(size_t amount, size_t& staging)
{
    if (bytes_ > byteBudget_ || staging > byteBudget_ - bytes_ || amount > byteBudget_ - bytes_ - staging) {
        return false;
    }
    staging += amount;
    peak_ = std::max(peak_, bytes_ + staging);
    return true;
}

bool MemoryAccessState::StageUpdate(const Update& update, UpdatePlan& plan)
{
    // Reserve a conservative bound for new endpoints, split survivors and bookkeeping.
    if (!ChargeStaging(PATCH_COST_MULTIPLIER * Cost(update.state) + PATCH_BOOKKEEPING_ALLOWANCE, plan.staging)) {
        return false;
    }
    auto& patch = plan.patches[update.storage];
    const auto source = spaces_.find(update.storage);
    if (source == spaces_.end()) {
        return true;
    }
    const auto& tree = source->second;
    auto it = tree.upper_bound(update.range.begin);
    if (it != tree.begin()) {
        --it;
    }
    for (; it != tree.end() && it->first < update.range.end; ++it) {
        if (it->second.end <= update.range.begin || patch.removed.count(it->first) != 0U) {
            continue;
        }
        const auto cost = Cost(it->second.state);
        if (!ChargeStaging(PATCH_COST_MULTIPLIER * cost + REPLACEMENT_BOOKKEEPING_ALLOWANCE, plan.staging)) {
            return false;
        }
        patch.removed.insert(it->first);
        patch.replacement.emplace(*it);
        plan.removedBytes += cost;
        ++plan.removedCount;
    }
    return true;
}

void MemoryAccessState::PrepareReplacements(const std::vector<Update>& updates, UpdatePlan& plan)
{
    for (const auto& update : updates) {
        auto& tree = plan.patches.at(update.storage).replacement;
        Split(tree, update.range.begin);
        Split(tree, update.range.end);
        tree.erase(tree.lower_bound(update.range.begin), tree.lower_bound(update.range.end));
        tree.emplace(update.range.begin, Segment{update.range.end, update.state});
    }
    for (auto& entry : plan.patches) {
        auto& tree = entry.second.replacement;
        for (auto it = tree.begin(); it != tree.end();) {
            const auto next = std::next(it);
            if (next != tree.end() && it->second.end == next->first && Equal(it->second.state, next->second.state)) {
                it->second.end = next->second.end;
                tree.erase(next);
            } else {
                ++it;
            }
        }
        for (const auto& segment : tree) {
            plan.addedBytes += Cost(segment.second.state);
            ++plan.addedCount;
        }
    }
}

void MemoryAccessState::CommitReplacements(UpdatePlan& plan)
{
    // Allocate outer maps before touching live segments. Empty maps are not observable state.
    for (const auto& entry : plan.patches) {
        spaces_.try_emplace(entry.first);
    }
    for (auto& entry : plan.patches) {
        auto& target = spaces_.at(entry.first);
        auto& patch = entry.second;
        for (const auto key : patch.removed) {
            target.erase(key);
        }
        while (!patch.replacement.empty()) {
            auto node = patch.replacement.extract(patch.replacement.begin());
            auto it = target.insert(std::move(node)).position;
            if (it != target.begin()) {
                auto prev = std::prev(it);
                if (prev->second.end == it->first && Equal(prev->second.state, it->second.state)) {
                    prev->second.end = it->second.end;
                    plan.addedBytes -= Cost(it->second.state);
                    --plan.addedCount;
                    target.erase(it);
                    it = prev;
                }
            }
            const auto next = std::next(it);
            if (next != target.end() && it->second.end == next->first && Equal(it->second.state, next->second.state)) {
                it->second.end = next->second.end;
                plan.removedBytes += Cost(next->second.state);
                ++plan.removedCount;
                target.erase(next);
            }
        }
    }
    segments_ = segments_ + plan.addedCount - plan.removedCount;
    bytes_ = bytes_ + plan.addedBytes - plan.removedBytes;
    peak_ = std::max(peak_, bytes_);
}

bool MemoryAccessState::Apply(const std::vector<Update>& updates)
{
    UpdatePlan plan;
    try {
        for (size_t i = 0; i < updates.size(); ++i) {
            const auto& update = updates[i];
            if (update.state == nullptr || update.range.begin >= update.range.end) {
                return false;
            }
            if (i != 0U &&
                (update.storage < updates[i - 1].storage ||
                 (update.storage == updates[i - 1].storage && update.range.begin < updates[i - 1].range.end))) {
                return false;
            }
            if (!StageUpdate(update, plan)) {
                return false;
            }
        }
        PrepareReplacements(updates, plan);
        const auto retainedCount = segments_ - plan.removedCount;
        const auto retainedBytes = bytes_ - plan.removedBytes;
        if (retainedCount > segmentBudget_ || plan.addedCount > segmentBudget_ - retainedCount ||
            retainedBytes > byteBudget_ || plan.addedBytes > byteBudget_ - retainedBytes) {
            return false;
        }
        CommitReplacements(plan);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
} // namespace npucheck::race
