/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_VF_SUBGRAPH_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_VF_SUBGRAPH_H
#include "checker/racecheck/happens_before_graph.h"
#include "acl_san/aclsan_cbdata_device.h"
#include "npu_tool_log.h"
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace npucheck::race {
// A parent VF participates in outer PIPE_V order through entry/completion only.
// Racecheck models distinct internal memory instructions as completing in vfInstrExecSeq order.
class VfSubgraph {
public:
    VfSubgraph(HappensBeforeGraph& graph, std::set<std::string>& reasons)
        : graph_(graph), reasons_(reasons), entry_(graph.AddNode()), completion_(graph.AddNode())
    {}
    size_t Entry() const { return entry_; }
    size_t Completion() const { return completion_; }
    size_t AddAccess(const AclsanDeviceMemoryAccessData& a)
    {
        const auto kind = ClassOf(a.accessMode);
        const auto index = a.vfInstrExecSeq;
        auto group = groups_.find(index);
        if (group == groups_.end()) {
            group = groups_.emplace(index, Group{graph_.AddNode(), kind}).first;
        }
        ASCTOOL_DEBUG(
            "racecheck DAG graph=%p VF access launchId=%llu parentVfInstrExecId=%llu vfInstrExecSeq=%llu node=%zu",
            static_cast<void*>(&graph_), static_cast<unsigned long long>(a.header.launchId),
            static_cast<unsigned long long>(a.header.instrExecId), static_cast<unsigned long long>(a.vfInstrExecSeq),
            group->second.node);
        Observe(a.vfInstrExecSeq, index, false);
        return group->second.node;
    }
    void AddBarrier(const AclsanDeviceBarrierData& b)
    {
        Observe(b.vfInstrExecSeq, b.vfInstrExecSeq, true);
        barriers_.emplace(b.vfInstrExecSeq, b);
    }
    void Build()
    {
        std::array<std::optional<size_t>, VF_LOCAL_MEM_ACCESS_CLASS_COUNT> prefix{}, acquired{};
        std::optional<size_t> lastAccess;
        std::set<uint64_t> visited;
        for (const auto& position : positions_) {
            const auto serial = position.first;
            const auto index = position.second.first;
            if (position.second.second) {
                const auto& b = barriers_.at(serial);
                const auto node = graph_.AddNode();
                ASCTOOL_DEBUG(
                    "racecheck DAG graph=%p VF barrier launchId=%llu parentVfInstrExecId=%llu vfInstrExecSeq=%llu "
                    "node=%zu src=%u dst=%u",
                    static_cast<void*>(&graph_), static_cast<unsigned long long>(b.header.launchId),
                    static_cast<unsigned long long>(b.header.instrExecId),
                    static_cast<unsigned long long>(b.vfInstrExecSeq), node, b.srcMemType, b.dstMemType);
                Edge(entry_, node);
                Edge(node, completion_);
                for (size_t c = 0; c < VF_LOCAL_MEM_ACCESS_CLASS_COUNT; ++c) {
                    if ((b.srcMemType & (1u << c)) != 0 && prefix[c]) {
                        Edge(*prefix[c], node);
                    }
                    if ((b.dstMemType & (1u << c)) != 0) {
                        Join(acquired[c], node);
                    }
                }
                continue;
            }
            auto& group = groups_.at(index);
            if (!visited.insert(index).second) {
                continue;
            }
            Edge(entry_, group.node);
            Edge(group.node, completion_);
            for (size_t c = 0; c < VF_LOCAL_MEM_ACCESS_CLASS_COUNT; ++c) {
                if ((group.classes & (1u << c)) != 0 && acquired[c]) {
                    Edge(*acquired[c], group.node);
                }
            }
            if (lastAccess) {
                ASCTOOL_DEBUG(
                    "racecheck DAG graph=%p VF completion order edge=%zu->%zu", static_cast<void*>(&graph_),
                    *lastAccess, group.node);
                Edge(*lastAccess, group.node);
            }
            lastAccess = group.node;
            for (size_t c = 0; c < VF_LOCAL_MEM_ACCESS_CLASS_COUNT; ++c) {
                if ((group.classes & (1u << c)) != 0) {
                    Join(prefix[c], group.node);
                }
            }
        }
        Edge(entry_, completion_); // Valid for success-empty and barrier-only VFs as well.
    }

private:
    // Vector/scalar load/store classes represented by LocalMem access bits.
    static constexpr size_t VF_LOCAL_MEM_ACCESS_CLASS_COUNT = 4;
    struct Group {
        size_t node;
        uint32_t classes;
    };
    static uint32_t ClassOf(uint32_t mode)
    {
        switch (mode) {
            case ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ:
                return ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD;
            case ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE:
                return ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE;
            case ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ:
                return ACLSAN_DEVICE_LOCAL_MEM_SCALAR_LOAD;
            case ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE:
                return ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE;
            default:
                return 0;
        }
    }
    void Observe(uint64_t serial, uint64_t index, bool barrier)
    {
        positions_.emplace(serial, std::make_pair(index, barrier));
    }
    void Edge(size_t from, size_t to)
    {
        if (!graph_.AddEdge(from, to, HappensBeforeGraph::Reason::InstructionBoundary)) {
            reasons_.insert("graph_budget");
        }
    }
    void Join(std::optional<size_t>& prefix, size_t node)
    {
        if (!prefix) {
            prefix = node;
            return;
        }
        if (*prefix == node || reasons_.count("graph_budget") != 0) {
            return;
        }
        const auto joined = graph_.AddNode();
        Edge(*prefix, joined);
        Edge(node, joined);
        prefix = joined;
    }
    HappensBeforeGraph& graph_;
    std::set<std::string>& reasons_;
    size_t entry_, completion_;
    std::map<uint64_t, Group> groups_;
    std::map<uint64_t, std::pair<uint64_t, bool>> positions_;
    std::map<uint64_t, AclsanDeviceBarrierData> barriers_;
};
} // namespace npucheck::race
#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_VF_SUBGRAPH_H
