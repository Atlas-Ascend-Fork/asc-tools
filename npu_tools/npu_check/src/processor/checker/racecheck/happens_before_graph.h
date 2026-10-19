/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_CHECK_HAPPENS_BEFORE_GRAPH_H
#define NPU_CHECK_HAPPENS_BEFORE_GRAPH_H
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace npucheck::race {
// Node identity and replay rank are never ordering evidence by themselves.
class HappensBeforeGraph {
public:
    enum class Relation { Before, After, Unordered, Unknown };
    enum class Reason {
        VectorOrder,
        PipelineOrder,
        InstructionBoundary,
        UnitFlag,
        PublishPrefix,
        TokenMatch,
        AcquireSuffix,
        BarrierPrefix
    };
    struct Edge {
        size_t target;
        Reason reason;
    };
    HappensBeforeGraph(size_t edgeBudget, size_t workBudget, size_t cacheBudget);
    size_t AddNode();
    bool AddEdge(size_t from, size_t to, Reason reason);
    bool Seal(bool coverageComplete = true);
    // Bounded, uncached reachability for resolving notification sources while
    // edges are still being added. nullopt means unknown, never "no path".
    std::optional<bool> PrecedesDuringBuild(size_t from, size_t to);
    Relation Compare(size_t a, size_t b);
    const std::vector<size_t>& Order() const { return order_; }
    size_t QueryWork() const { return work_; }
    size_t EdgeCount() const { return edgeCount_; }

private:
    enum class Path { Yes, No, Unknown };
    Path Reachable(size_t from, size_t to);
    std::vector<std::vector<Edge>> edges_;
    std::vector<size_t> order_, rank_, seen_, pending_;
    std::map<std::pair<size_t, size_t>, bool> cache_;
    size_t edgeBudget_, workBudget_, cacheBudget_, edgeCount_ = 0, work_ = 0, generation_ = 0;
    bool sealed_ = false;
    bool valid_ = true;
};
} // namespace npucheck::race
#endif
