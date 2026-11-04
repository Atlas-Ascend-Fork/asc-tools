/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_CHECK_HOST_ORDER_H
#define NPU_CHECK_HOST_ORDER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace npucheck {
// Session-wide happens-before between host callbacks, launch boundaries and streams.
//
// Every event belongs to exactly one actor: a host thread or a stream. The observed
// edges are program order inside an actor, submission (thread -> stream) and
// synchronization (stream -> thread); no other edge exists, and every edge targets a
// newly created event. Events are therefore stamped with a vector clock when they are
// issued, and "does A precede B" becomes one clock lookup instead of a graph search.
//
// The representation does not grow with the number of events: nodes store an actor, an
// index and a shared clock snapshot, and a fresh snapshot is only created when a submit
// or a synchronization joins two actors. Adding streams or host threads (multi-stream,
// multi-operator sessions) adds one clock component per actor, not a new edge class.
class HostOrder {
public:
    using NodeId = size_t;
    using ActorId = uint32_t;
    static constexpr NodeId kInvalidNode = static_cast<NodeId>(-1);
    static constexpr ActorId kInvalidActor = static_cast<ActorId>(-1);
    static constexpr size_t kMaxNodes = 100000;
    static constexpr size_t kMaxActors = 1024;

    // Dense actor for a host thread. Repeated calls return the same actor.
    ActorId ThreadActor(std::thread::id thread);
    // Dense actor for a stream handle. Handles are stable for the session; a reused
    // handle is treated as the same timeline, which is sound because destroying a
    // stream waits for its work.
    ActorId StreamActor(const void* stream);

    // Opens a new event on the actor's timeline.
    NodeId Issue(ActorId actor);
    // Publishes the source timeline to the destination actor (submit or sync).
    void Join(ActorId destination, ActorId source);

    // True when no observation was dropped. Unknown coverage answers nullopt, which is
    // the same three-state contract the graph-based queries expose.
    std::optional<bool> Precedes(NodeId first, NodeId second) const;

    bool Complete() const { return complete_; }
    size_t Nodes() const { return nodes_.size(); }
    size_t Actors() const { return counters_.size(); }

private:
    using Clock = std::vector<std::pair<ActorId, uint32_t>>;
    struct Node {
        ActorId actor = kInvalidActor;
        uint32_t index = 0;
        std::shared_ptr<const Clock> clock;
    };

    static uint32_t Component(const Clock& clock, ActorId actor);
    static void Upsert(Clock& clock, ActorId actor, uint32_t value);
    static std::shared_ptr<const Clock> Merge(
        const Clock& first, ActorId firstActor, uint32_t firstIndex, const Clock& second, ActorId secondActor,
        uint32_t secondIndex);

    std::map<std::thread::id, ActorId> threads_;
    std::map<const void*, ActorId> streams_;
    std::vector<std::shared_ptr<const Clock>> current_;
    std::vector<std::shared_ptr<const Clock>> lastIssued_;
    std::vector<uint32_t> lastIndex_;
    std::vector<uint32_t> counters_;
    std::vector<Node> nodes_;
    bool complete_ = true;
};
} // namespace npucheck
#endif
