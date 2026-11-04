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

#include <algorithm>

namespace npucheck {
HostOrder::ActorId HostOrder::ThreadActor(std::thread::id thread)
{
    const auto found = threads_.find(thread);
    if (found != threads_.end())
        return found->second;
    if (counters_.size() >= kMaxActors) {
        complete_ = false;
        return kInvalidActor;
    }
    const auto actor = static_cast<ActorId>(counters_.size());
    counters_.push_back(0);
    current_.push_back(std::make_shared<const Clock>());
    lastIssued_.push_back(std::make_shared<const Clock>());
    lastIndex_.push_back(0);
    threads_.emplace(thread, actor);
    return actor;
}

HostOrder::ActorId HostOrder::StreamActor(const void* stream)
{
    const auto found = streams_.find(stream);
    if (found != streams_.end())
        return found->second;
    if (counters_.size() >= kMaxActors) {
        complete_ = false;
        return kInvalidActor;
    }
    const auto actor = static_cast<ActorId>(counters_.size());
    counters_.push_back(0);
    current_.push_back(std::make_shared<const Clock>());
    lastIssued_.push_back(std::make_shared<const Clock>());
    lastIndex_.push_back(0);
    streams_.emplace(stream, actor);
    return actor;
}

HostOrder::NodeId HostOrder::Issue(ActorId actor)
{
    if (actor == kInvalidActor || nodes_.size() >= kMaxNodes) {
        complete_ = false;
        return kInvalidNode;
    }
    ++counters_[actor];
    nodes_.push_back(Node{actor, counters_[actor], current_[actor]});
    lastIssued_[actor] = current_[actor];
    lastIndex_[actor] = counters_[actor];
    return nodes_.size() - 1;
}

void HostOrder::Join(ActorId destination, ActorId source)
{
    if (destination == kInvalidActor || source == kInvalidActor) {
        complete_ = false;
        return;
    }
    if (destination == source || !complete_)
        return;
    // Publish the source's last issued event, not its pending clock: a join that the
    // source has not materialized as an event must not be visible to the destination.
    current_[destination] = Merge(
        *current_[destination], destination, counters_[destination], *lastIssued_[source], source, lastIndex_[source]);
}

std::optional<bool> HostOrder::Precedes(NodeId first, NodeId second) const
{
    if (!complete_ || first == kInvalidNode || second == kInvalidNode)
        return std::nullopt;
    if (first >= nodes_.size() || second >= nodes_.size())
        return std::nullopt;
    if (first == second)
        return true;
    const auto& source = nodes_[first];
    const auto& target = nodes_[second];
    if (source.actor == target.actor)
        return source.index <= target.index;
    return Component(*target.clock, source.actor) >= source.index;
}

uint32_t HostOrder::Component(const Clock& clock, ActorId actor)
{
    const auto found = std::lower_bound(
        clock.begin(), clock.end(), actor,
        [](const std::pair<ActorId, uint32_t>& entry, ActorId key) { return entry.first < key; });
    return found != clock.end() && found->first == actor ? found->second : 0;
}

void HostOrder::Upsert(Clock& clock, ActorId actor, uint32_t value)
{
    const auto found = std::lower_bound(
        clock.begin(), clock.end(), actor,
        [](const std::pair<ActorId, uint32_t>& entry, ActorId key) { return entry.first < key; });
    if (found != clock.end() && found->first == actor)
        found->second = std::max(found->second, value);
    else
        clock.insert(found, {actor, value});
}

std::shared_ptr<const HostOrder::Clock> HostOrder::Merge(
    const Clock& first, ActorId firstActor, uint32_t firstIndex, const Clock& second, ActorId secondActor,
    uint32_t secondIndex)
{
    auto merged = std::make_shared<Clock>();
    merged->reserve(first.size() + second.size() + 2);
    size_t left = 0;
    size_t right = 0;
    while (left < first.size() || right < second.size()) {
        if (right >= second.size() || (left < first.size() && first[left].first < second[right].first)) {
            merged->push_back(first[left]);
            ++left;
        } else if (left >= first.size() || second[right].first < first[left].first) {
            merged->push_back(second[right]);
            ++right;
        } else {
            merged->push_back({first[left].first, std::max(first[left].second, second[right].second)});
            ++left;
            ++right;
        }
    }
    // Both timelines keep advancing after this join; the next join must observe the
    // progress up to that moment rather than the value captured inside a snapshot.
    Upsert(*merged, firstActor, firstIndex);
    Upsert(*merged, secondActor, secondIndex);
    return merged;
}
} // namespace npucheck
