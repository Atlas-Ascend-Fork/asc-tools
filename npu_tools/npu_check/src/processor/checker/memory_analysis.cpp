/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/memory_analysis.h"
#include "device_instr/common/instruction_id.h"
#include "npu_tool_log.h"
#include "securec.h"

#include <cstddef>

namespace npucheck {
HostOrder::NodeId MemoryAnalysis::HostNode()
{
    const auto node = order_.Issue(order_.ThreadActor(std::this_thread::get_id()));
    complete_ = complete_ && order_.Complete();
    return node;
}
bool MemoryAnalysis::HandleOperatorCompletion(
    AclsanCallbackId id, const AclsanCallbackCommonData& common, const void* data)
{
    if (id != ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE || common.size < sizeof(AclsanOperatorExecutionData)) {
        return false;
    }
    const auto& event = *static_cast<const AclsanOperatorExecutionData*>(data);
    const auto launch = launches_.find(event.launchId);
    if (launch == launches_.end()) {
        return event.launchId != 0 && boundaries_.count(event.launchId) != 0;
    }
    if (!launch->second.registered || launch->second.stream != event.stream) {
        return false;
    }
    if (common.result != 0 || event.flags != ACLSAN_OPERATOR_EXECUTION_FLAG_NONE) {
        complete_ = false;
    }
    closing_.insert(event.launchId);
    return true;
}

bool MemoryAnalysis::HostEvent(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    const auto& common = *static_cast<const AclsanCallbackCommonData*>(data);
    if (common.version != ACLSAN_API_VERSION || common.size < sizeof(common))
        return false;
    if (domain == ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION) {
        return HandleOperatorCompletion(id, common, data);
    }
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        if (common.size < sizeof(AclsanSynchronizeData))
            return false;
        return RecordSynchronization(*static_cast<const AclsanSynchronizeData*>(data), std::this_thread::get_id());
    }
    if (common.result != 0) {
        if (domain == ACLSAN_CB_DOMAIN_MEMORY)
            complete_ = false;
        return true;
    }
    const auto node = HostNode();
    currentHostNode_ = node;
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH)
        return RegisterLaunch(*static_cast<const AclsanLaunchData*>(data));
    if (domain == ACLSAN_CB_DOMAIN_MEMORY) {
        if (IsLegacyMemcpy(id, common))
            return true;
        return id == ACLSAN_CBID_MEMORY_TRANSFER_END ?
                   HostTransfer(*static_cast<const AclsanMemoryTransferData*>(data)) :
                   HostWrite(*static_cast<const AclsanMemoryWriteData*>(data));
    }
    if (domain == ACLSAN_CB_DOMAIN_RESOURCE)
        return ResourceEvent(id, *static_cast<const AclsanResourceData*>(data));
    return true;
}
bool MemoryAnalysis::RegisterLaunch(const AclsanLaunchData& launch)
{
    if (launch.common.size < sizeof(launch) || launch.launchId == 0 || boundaries_.count(launch.launchId) != 0U)
        return false;
    auto& output = Get(launch.launchId);
    output.stream = launch.stream;
    output.registered = true;
    RegisterUbLayout(launch);
    // Submission publishes this thread's program order to the stream. Consecutive
    // launches on one stream are ordinary program order inside the stream actor.
    const auto stream = order_.StreamActor(launch.stream);
    order_.Join(stream, order_.ThreadActor(std::this_thread::get_id()));
    const auto entry = order_.Issue(stream);
    const auto exit = order_.Issue(stream);
    boundaries_.emplace(launch.launchId, std::make_pair(entry, exit));
    boundaryStreams_[launch.launchId] = launch.stream;
    streamLaunches_[launch.stream] = launch.launchId;
    complete_ = complete_ && order_.Complete();
    return true;
}
bool MemoryAnalysis::RecordSynchronization(const AclsanSynchronizeData& sync, std::thread::id thread)
{
    if (sync.common.version != ACLSAN_API_VERSION || sync.common.size < sizeof(sync))
        return false;
    if (managed_) {
        synchronizationReports_.clear();
        ObserveSynchronization(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync);
    }
    if (sync.common.result != 0) {
        complete_ = false;
        return true;
    }
    const auto actor = order_.ThreadActor(thread);
    const uint64_t frontier = sync.common.correlationId;
    if (frontier == 0) {
        if (streamLaunches_.count(sync.stream) != 0)
            complete_ = false;
    } else {
        const auto boundary = boundaries_.find(frontier);
        const auto stream = boundaryStreams_.find(frontier);
        if (boundary == boundaries_.end() || stream == boundaryStreams_.end() || stream->second != sync.stream) {
            complete_ = false;
            return false;
        }
        order_.JoinNode(actor, boundary->second.second);
    }
    (void)order_.Issue(actor);
    complete_ = complete_ && order_.Complete();
    return true;
}
bool MemoryAnalysis::ResourceEvent(AclsanCallbackId id, const AclsanResourceData& resource)
{
    if (resource.common.size < sizeof(resource))
        return false;
    if (resource.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE)
        return true;
    if (id != ACLSAN_CBID_RESOURCE_MEMORY_ALLOC && id != ACLSAN_CBID_RESOURCE_MEMORY_FREE)
        return true;
    resourceUpdate_ = resources_.Apply(id, resource, *state, currentHostNode_);
    complete_ = complete_ && resources_.Complete();
    return true;
}

namespace {
bool IsRegisterTransfer(uint32_t instructionId)
{
    using aclsan::InstructionId;
    switch (static_cast<InstructionId>(instructionId)) {
        case InstructionId::NdDmaOutToUbufB8:
        case InstructionId::NdDmaOutToUbufB16:
        case InstructionId::NdDmaOutToUbufB32:
        case InstructionId::CopyGmToCbufAlignV2B8:
        case InstructionId::CopyGmToCbufAlignV2B16:
        case InstructionId::CopyGmToCbufAlignV2B32:
            return true;
        default:
            return false;
    }
}

bool HasFootprintModifiers(const AclsanDeviceMemoryAccessData& access)
{
    return access.vectorMask0 != 0 || access.vectorMask1 != 0 || access.predicateMask0 != 0 ||
           access.predicateMask1 != 0 || access.memoryFlags != 0;
}
} // namespace

void MemoryAnalysis::StoreDeviceMemory(Launch& launch, const AclsanDeviceMemoryAccessData& access)
{
    const auto core = race::DomainOf(access.header);
    const bool missing = (access.regDependencyMask0 & ~launch.registerMasks[core]) != 0;
    const bool hasModifiers = HasFootprintModifiers(access);
    if (IsRegisterTransfer(access.instructionId) && missing &&
        access.header.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED && access.accessCount == 1 &&
        access.accessIndex == 0 && access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN && access.layoutKind == 0 &&
        access.accessMode == 0 && access.address == 0 && !hasModifiers && access.unitFlag == 0) {
        launch.dependencyOnly.push_back(access);
        return;
    }
    // Keep raw operands for bounds checking even when the graph cannot use their footprint.
    if (access.header.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT || hasModifiers) {
        launch.inexactMemory = true;
    }
    launch.memory.push_back(access);
}

bool MemoryAnalysis::DeviceEvent(AclsanCallbackId id, const void* data)
{
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    const size_t required = id == ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS ? sizeof(AclsanDeviceMemoryAccessData) :
                            id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS      ? offsetof(AclsanDeviceMemoryAccessData, simt) :
                            id == ACLSAN_CBID_DEVICE_VF                 ? sizeof(AclsanDeviceVfData) :
                            id == ACLSAN_CBID_DEVICE_STATE              ? sizeof(AclsanDeviceRegisterStateData) :
                            id == ACLSAN_CBID_DEVICE_SYNC               ? sizeof(AclsanDeviceSyncData) :
                                                                          sizeof(AclsanDeviceBarrierData);
    if (h.version != ACLSAN_API_VERSION || h.size < required || h.launchId == 0 || h.instrExecId == 0)
        return false;
    auto& launch = Get(h.launchId);
    // VF-expanded UB accesses and ordinary UB transfers must reach the same graph.
    if (race::ExceedsBudget(
            limits_.LaunchEventBudget(),
            {launch.memory.size(), launch.simtMemory.size(), launch.sync.size(), launch.registers.size(),
             launch.dependencyOnly.size(), launch.vfs.size(), launch.localBarriers.size(), 1})) {
        if (launch.reasons.insert("event_budget").second) {
            ASCTOOL_WARNING(
                "analysis budget: launch=%llu stage=ingress limit=%zu", static_cast<unsigned long long>(h.launchId),
                limits_.LaunchEventBudget());
        }
        return true;
    }
    if (id == ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS) {
        const auto& access = *static_cast<const AclsanDeviceMemoryAccessData*>(data);
        if ((h.flags & (ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED | ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED |
                        ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR)) == 0) {
            launch.simtMemory.push_back(access);
        }
    } else if (id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        AclsanDeviceMemoryAccessData access{};
        if (memcpy_s(&access, sizeof(access), data, offsetof(AclsanDeviceMemoryAccessData, simt)) != EOK) {
            return false;
        }
        access.header.size = sizeof(access);
        StoreDeviceMemory(launch, access);
    } else if (id == ACLSAN_CBID_DEVICE_VF) {
        launch.vfs.push_back(*static_cast<const AclsanDeviceVfData*>(data));
    } else if (id == ACLSAN_CBID_DEVICE_STATE) {
        const auto& state = *static_cast<const AclsanDeviceRegisterStateData*>(data);
        if (state.regId >= ACLSAN_DEVICE_REGISTER_COUNT || state.regId >= 64)
            return false;
        launch.registerMasks[race::DomainOf(h)] |= UINT64_C(1) << state.regId;
        if (state.regId == ACLSAN_DEVICE_REGISTER_FPC)
            launch.registers.push_back(state);
    } else if (id == ACLSAN_CBID_DEVICE_SYNC) {
        launch.sync.push_back(race::NormalizeSync(*static_cast<const AclsanDeviceSyncData*>(data)));
    } else if (id == ACLSAN_CBID_DEVICE_PIPE_BARRIER) {
        const auto& barrier = *static_cast<const AclsanDeviceBarrierData*>(data);
        if (barrier.barrierKind == ACLSAN_DEVICE_BARRIER_LOCAL_MEM) {
            launch.localBarriers.push_back(barrier);
            return true;
        }
        AclsanDeviceSyncData sync{};
        sync.header = h;
        sync.scope = ACLSAN_DEVICE_SYNC_SCOPE_PIPE;
        sync.srcPipe = h.pipeline;
        sync.dstPipe = barrier.pipe;
        launch.sync.push_back(sync);
    }
    return true;
}
bool MemoryAnalysis::BeginCallback(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    state->StartEvent();
    resourceUpdate_ = AllocationUpdateStatus::NOT_FOUND;
    hostWriteRecorded_ = false;
    hostOperations_.clear();
    synchronizationReports_.clear();
    if (data == nullptr)
        return false;
    bool valid = true;
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION)
        valid = DeviceEvent(id, data);
    else
        valid = HostEvent(domain, id, data);
    if (managed_ && valid) {
        if (domain != ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
            ObserveSynchronization(domain, id, data);
        }
        // Publish validated writes for every completed launch before any checker
        // queries this batch. Visibility still requires an HB edge to its reader.
        for (auto launchId : closing_)
            CommitLaunch(launchId);
    }
    complete_ = complete_ && valid;
    return valid;
}
void MemoryAnalysis::EndCallback()
{
    for (auto id : closing_) {
        launches_.erase(id);
    }
    closing_.clear();
    state->EndEvent();
}
race::DependencyGraph& MemoryAnalysis::Graph(uint64_t id, const race::Limits& limits, std::set<std::string>& reasons)
{
    auto& launch = Get(id);
    const auto& policy = managed_ ? limits_ : limits;
    if (launch.dependencyLimits && !(*launch.dependencyLimits == policy))
        launch.reasons.insert("analysis_budget_policy_mismatch");
    if (!launch.dependency) {
        launch.dependencyLimits = policy;
        if (race::ExceedsBudget(
                policy.LaunchEventBudget(),
                {launch.memory.size(), launch.sync.size(), launch.registers.size(), launch.dependencyOnly.size(),
                 launch.vfs.size(), launch.localBarriers.size()})) {
            launch.reasons.insert("event_budget");
            ASCTOOL_WARNING(
                "analysis budget: launch=%llu stage=events memory=%zu sync=%zu registers=%zu limit=%zu",
                static_cast<unsigned long long>(id), launch.memory.size(), launch.sync.size(), launch.registers.size(),
                policy.LaunchEventBudget());
        }
        if (invalidSynchronization_.count(id) != 0)
            launch.reasons.insert("invalid_synchronization");
        if (launch.inexactMemory)
            launch.reasons.insert("incomplete_memory_observation");
        launch.dependency = std::make_unique<race::DependencyGraph>(
            policy, managed_ ? rangeBudget_ : std::shared_ptr<race::RangeStorageBudget>{});
        ASCTOOL_INFO(
            "analysis policy: launch=%llu events=%zu segments=%zu edges=%zu queryWork=%zu analysisWork=%zu",
            static_cast<unsigned long long>(id), policy.LaunchEventBudget(), policy.FootprintIntervalBudget(),
            policy.GraphEdgeBudget(), policy.ReachabilityQueryWorkBudget(), policy.ReachabilityPhaseWorkBudget());
        race::BuildDependencyGraph(
            launch.memory, launch.sync, launch.skippedFragments, policy, launch.reasons, *launch.dependency,
            launch.registers, launch.vfs, launch.localBarriers);
        ++graphBuilds_;
    }
    reasons.insert(launch.reasons.begin(), launch.reasons.end());
    reasons.insert(incompleteReasons_.begin(), incompleteReasons_.end());
    if (!complete_)
        reasons.insert("host_graph_incomplete");
    return *launch.dependency;
}
bool MemoryAnalysis::HasUnfinishedPredecessor(uint64_t launchId) const
{
    const auto current = boundaries_.find(launchId);
    if (current == boundaries_.end())
        return false;
    for (const auto& launch : launches_) {
        if (launch.first == launchId || !launch.second.registered)
            continue;
        const auto previous = boundaries_.find(launch.first);
        if (previous == boundaries_.end())
            continue;
        const auto precedes = order_.Precedes(previous->second.second, current->second.first);
        if (precedes && *precedes)
            return true;
    }
    return false;
}
race::HappensBeforeGraph::Relation MemoryAnalysis::CompareLaunches(uint64_t previous, uint64_t current)
{
    using Relation = race::HappensBeforeGraph::Relation;
    const auto a = boundaries_.find(previous), b = boundaries_.find(current);
    if (!complete_ || a == boundaries_.end() || b == boundaries_.end() || previous == current)
        return Relation::Unknown;
    const auto forward = order_.Precedes(a->second.second, b->second.first);
    const auto backward = order_.Precedes(b->second.second, a->second.first);
    if (!forward || !backward)
        return Relation::Unknown;
    return *forward ? Relation::Before : *backward ? Relation::After : Relation::Unordered;
}
} // namespace npucheck
