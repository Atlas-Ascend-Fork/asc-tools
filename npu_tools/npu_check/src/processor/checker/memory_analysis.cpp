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
#include <algorithm>
#include "device_instr/common/instruction_id.h"

namespace npucheck {
HostOrder::NodeId MemoryAnalysis::HostNode()
{
    const auto node = order_.Issue(order_.ThreadActor(std::this_thread::get_id()));
    complete_ = complete_ && order_.Complete();
    return node;
}
bool MemoryAnalysis::HostEvent(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    const auto& common = *static_cast<const AclsanCallbackCommonData*>(data);
    if (common.version != ACLSAN_API_VERSION || common.size < sizeof(common))
        return false;
    if (common.result != 0) {
        if (domain == ACLSAN_CB_DOMAIN_MEMORY)
            complete_ = false;
        return true;
    }
    // A successful synchronization publishes the stream timeline to this thread
    // before the callback event is stamped, so later host work observes the join.
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        if (common.size < sizeof(AclsanSynchronizeData))
            return false;
        const auto& sync = *static_cast<const AclsanSynchronizeData*>(data);
        order_.Join(order_.ThreadActor(std::this_thread::get_id()), order_.StreamActor(sync.stream));
    }
    const auto node = HostNode();
    currentHostNode_ = node;
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH)
        return RegisterLaunch(*static_cast<const AclsanLaunchData*>(data));
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE)
        return CloseStream(*static_cast<const AclsanSynchronizeData*>(data));
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
    if (launch.common.size < sizeof(launch) || launch.launchId == 0 || boundaries_.count(launch.launchId))
        return false;
    auto& output = Get(launch.launchId);
    output.stream = launch.stream;
    output.registered = true;
    // Submission publishes this thread's program order to the stream. Consecutive
    // launches on one stream are ordinary program order inside the stream actor.
    const auto stream = order_.StreamActor(launch.stream);
    order_.Join(stream, order_.ThreadActor(std::this_thread::get_id()));
    const auto entry = order_.Issue(stream);
    const auto exit = order_.Issue(stream);
    boundaries_.emplace(launch.launchId, std::make_pair(entry, exit));
    complete_ = complete_ && order_.Complete();
    return true;
}
bool MemoryAnalysis::CloseStream(const AclsanSynchronizeData& sync)
{
    if (sync.common.size < sizeof(sync))
        return false;
    for (const auto& launch : launches_)
        if (launch.second.registered && launch.second.stream == sync.stream)
            closing_.insert(launch.first);
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

bool MemoryAnalysis::DeviceEvent(AclsanCallbackId id, const void* data)
{
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    const size_t required = id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS ? sizeof(AclsanDeviceMemoryAccessData) :
                            id == ACLSAN_CBID_DEVICE_STATE         ? sizeof(AclsanDeviceRegisterStateData) :
                            id == ACLSAN_CBID_DEVICE_SYNC          ? sizeof(AclsanDeviceSyncData) :
                                                                     sizeof(AclsanDeviceBarrierData);
    if (h.version != ACLSAN_API_VERSION || h.size < required || h.launchId == 0 || h.instrExecId == 0)
        return false;
    auto& launch = Get(h.launchId);
    constexpr size_t MAX_EVENTS = 100000;
    if (id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS &&
        static_cast<const AclsanDeviceMemoryAccessData*>(data)->memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB) {
        const auto& access = *static_cast<const AclsanDeviceMemoryAccessData*>(data);
        if (access.accessCount > 0 && access.accessIndex < access.accessCount) {
            const auto key = race::Key(h);
            if (launch.skippedFragments.find(key) == launch.skippedFragments.end() &&
                launch.skippedFragments.size() >= MAX_EVENTS) {
                launch.reasons.insert("event_budget");
                return true;
            }
            launch.skippedFragments[key].insert(access.accessIndex);
        }
        return true;
    }
    if (launch.memory.size() + launch.sync.size() + launch.registers.size() + launch.dependencyOnly.size() >=
        MAX_EVENTS) {
        launch.reasons.insert("event_budget");
        return true;
    }
    if (id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        const auto& a = *static_cast<const AclsanDeviceMemoryAccessData*>(data);
        const auto core = race::DomainOf(h);
        using aclsan::InstructionId;
        const auto instruction = static_cast<InstructionId>(a.instructionId);
        const bool registerTransfer =
            instruction == InstructionId::NdDmaOutToUbufB8 || instruction == InstructionId::NdDmaOutToUbufB16 ||
            instruction == InstructionId::NdDmaOutToUbufB32 || instruction == InstructionId::CopyGmToCbufAlignV2B8 ||
            instruction == InstructionId::CopyGmToCbufAlignV2B16 ||
            instruction == InstructionId::CopyGmToCbufAlignV2B32;
        const bool missing = (a.regDependencyMask0 & ~launch.registerMasks[core]) != 0;
        if (registerTransfer && missing && h.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED && a.accessCount == 1 &&
            a.accessIndex == 0 && a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN && a.layoutKind == 0 &&
            a.accessMode == 0 && a.address == 0 && a.vectorMask0 == 0 && a.vectorMask1 == 0 && a.predicateMask0 == 0 &&
            a.predicateMask1 == 0 && a.memoryFlags == 0 && a.unitFlag == 0) {
            launch.dependencyOnly.push_back(a);
            return true;
        }
        // An exact footprint is usable independently of whether register setter
        // probes were requested. Register initialization is a checker verdict.
        if (h.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT || a.vectorMask0 || a.vectorMask1 || a.predicateMask0 ||
            a.predicateMask1 || a.memoryFlags) {
            launch.inexactMemory = true;
            // Keep the raw operand for bounds checking; the common graph will
            // reject incomplete observations rather than invent a footprint.
        }
        launch.memory.push_back(a);
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
    if (!data)
        return false;
    bool valid = true;
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION)
        valid = DeviceEvent(id, data);
    else
        valid = HostEvent(domain, id, data);
    if (managed_ && valid) {
        ObserveSynchronization(domain, id, data);
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
    if (!launch.dependency) {
        if (invalidSynchronization_.count(id) != 0)
            launch.reasons.insert("invalid_synchronization");
        if (launch.inexactMemory)
            launch.reasons.insert("incomplete_memory_observation");
        launch.dependency = std::make_unique<race::DependencyGraph>(limits);
        race::BuildDependencyGraph(
            launch.memory, launch.sync, launch.skippedFragments, limits, launch.reasons, *launch.dependency,
            launch.registers);
        ++graphBuilds_;
    }
    reasons.insert(launch.reasons.begin(), launch.reasons.end());
    reasons.insert(incompleteReasons_.begin(), incompleteReasons_.end());
    if (!complete_)
        reasons.insert("host_graph_incomplete");
    return *launch.dependency;
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
