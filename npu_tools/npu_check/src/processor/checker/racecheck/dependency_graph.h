/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_CHECK_DEPENDENCY_GRAPH_H
#define NPU_CHECK_DEPENDENCY_GRAPH_H

#include "checker/racecheck/happens_before_graph.h"
#include "checker/racecheck/types.h"

#include <map>
#include <set>
#include <vector>

namespace npucheck::race {
struct DependencyGraph {
    struct Access {
        const AclsanDeviceMemoryAccessData& data;
        size_t node;
        std::vector<Interval> ranges;
    };

    explicit DependencyGraph(const Limits& limits = {}) : graph(limits.edges, limits.queryWork, limits.queryCache) {}

    HappensBeforeGraph graph;
    std::vector<Access> accesses;
};

void BuildDependencyGraph(
    std::vector<AclsanDeviceMemoryAccessData>& memory, std::vector<AclsanDeviceSyncData>& sync,
    const std::map<Instruction, std::set<uint32_t>>& skippedFragments, const Limits& limits,
    std::set<std::string>& reasons, DependencyGraph& output);
} // namespace npucheck::race

#endif
