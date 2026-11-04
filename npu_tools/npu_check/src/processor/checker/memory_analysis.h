/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_CHECK_MEMORY_ANALYSIS_H
#define NPU_CHECK_MEMORY_ANALYSIS_H
#include "acl_san/aclsan_api.h"
#include "acl_san/aclsan_cbdata.h"
#include "checker/memory_state.h"
#include "checker/memory_resources.h"
#include "checker/host_order.h"
#include "checker/racecheck/dependency_graph.h"
#include "checker/sync_ledger.h"
#include <set>
#include <thread>

namespace npucheck {
// Shared by checker adapters within one ToolManager session. No worker, polling,
// device allocation is required.
class MemoryAnalysis {
public:
    // Per-launch index over write operands. Querying a reader only visits writes that
    // overlap its read ranges instead of every access of the launch; the page commit
    // below is what keeps that filter from changing verdicts.
    struct WriteRange {
        uint64_t begin = 0;
        uint64_t end = 0;
        uint32_t access = 0;
    };
    struct StorageWrites {
        std::vector<WriteRange> ranges;     // sorted by begin
        std::vector<uint64_t> prefixMaxEnd; // running max end, for reverse pruning
    };
    struct Launch {
        void* stream = nullptr;
        bool registered = false;
        std::vector<AclsanDeviceMemoryAccessData> memory;
        std::vector<AclsanDeviceMemoryAccessData> dependencyOnly;
        std::map<race::Domain, uint64_t> registerMasks;
        std::vector<AclsanDeviceSyncData> sync;
        std::vector<AclsanDeviceRegisterStateData> registers;
        std::map<race::Instruction, std::set<uint32_t>> skippedFragments;
        std::set<std::string> reasons;
        bool inexactMemory = false;
        size_t initializationQueryWork = 0;
        std::unique_ptr<race::DependencyGraph> dependency;
        bool storageIndexReady = false;
        std::vector<uint8_t> accessValid; // 0 unknown, 1 usable source, 2 invalid
        std::map<race::Storage, StorageWrites> writesByStorage;
        std::map<size_t, std::vector<uint32_t>> accessesByNode;
        std::set<race::Storage> pageCommitted;
    };
    std::shared_ptr<MemoryState> state = std::make_shared<MemoryState>();
    explicit MemoryAnalysis(bool managed = false) : managed_(managed) {}
    bool Managed() const { return managed_; }
    Launch& Get(uint64_t id) { return launches_[id]; }
    const std::set<uint64_t>& Closing() const { return closing_; }
    bool BeginCallback(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data);
    void EndCallback();
    race::DependencyGraph& Graph(uint64_t id, const race::Limits& limits, std::set<std::string>& reasons);
    race::HappensBeforeGraph::Relation CompareLaunches(uint64_t previous, uint64_t current);
    void RecordInitialization(uint64_t launchId, uint32_t device, uint64_t address, uint64_t bytes);
    std::vector<race::Interval> InitializationsBefore(
        uint64_t launchId, uint32_t device, const std::vector<race::Interval>& reads, std::set<std::string>& reasons);
    size_t GraphBuilds() const { return graphBuilds_; }
    size_t ResourceRecordsScanned() const { return resources_.RecordsScanned(); }
    size_t InitializationCandidates() const { return initializationCandidates_; }
    bool Complete() const { return complete_; }
    static std::vector<CallbackSpec> RequiredCallbacks();
    RangeResult Classify(uint64_t launchId, const MemoryLocation& location);
    RangeResult ClassifyAccessRange(const AclsanDeviceMemoryAccessData& access, uint64_t address, uint64_t bytes);
    AllocationUpdateStatus ResourceUpdate() const { return resourceUpdate_; }
    bool HostWriteRecorded() const { return hostWriteRecorded_; }
    std::shared_ptr<SyncLedger> Synchronization() const { return synchronization_; }
    const std::vector<NpuCheckSynccheckReport>& SynchronizationReports() const { return synchronizationReports_; }
    std::vector<race::Interval> PrecedingWrites(
        uint64_t launchId, size_t readerNode, const race::Storage& storage, std::set<std::string>& reasons);
    struct HostAccess {
        HostAccessEvidence evidence;
        RangeResult resource;
        bool initializationKnown = true;
        uint64_t uninitializedBytes = 0, firstUninitializedAddress = 0;
    };
    const std::vector<HostAccess>& HostOperations() const { return hostOperations_; }
    bool IsLegacyMemcpy(AclsanCallbackId id, const AclsanCallbackCommonData& common) const;
    race::HappensBeforeGraph::Relation CompareHostToLaunch(size_t hostNode, uint64_t launchId);
    race::HappensBeforeGraph::Relation CompareHosts(size_t first, size_t second);
    std::vector<NpuCheckRacecheckReport> HostConflicts(const race::Limits& limits, std::set<std::string>& reasons);
    std::vector<NpuCheckRacecheckReport> CrossLaunchHostConflicts(
        uint64_t launchId, const race::Limits& limits, std::set<std::string>& reasons);
    void CommitLaunch(uint64_t launchId);
    std::vector<NpuCheckRacecheckReport> CrossLaunchConflicts(
        uint64_t launchId, const race::Limits& limits, std::set<std::string>& reasons);

private:
    bool managed_;
    bool complete_ = true;
    std::set<std::string> incompleteReasons_;
    HostOrder order_;
    std::map<uint64_t, std::pair<size_t, size_t>> boundaries_;
    std::map<uint64_t, Launch> launches_;
    std::set<uint64_t> closing_;
    size_t graphBuilds_ = 0;
    size_t currentHostNode_ = 0;
    struct Initialization {
        race::Interval range;
        size_t node;
    };
    struct InitializationIndex {
        std::vector<Initialization> writes;
        std::vector<uint64_t> prefixMaxEnd;
        bool sorted = true;
    };
    using InitializationKey = std::pair<uint32_t, uint64_t>;
    std::map<InitializationKey, InitializationIndex> initializations_;
    size_t initializationCount_ = 0;
    size_t initializationCandidates_ = 0;
    void AddInitialization(uint32_t device, race::Interval range, size_t node, uint64_t allocationSequence);
    std::vector<const Initialization*> OverlappingInitializations(
        uint32_t device, uint64_t allocationSequence, const std::vector<race::Interval>& reads);
    MemoryResources resources_;
    AllocationUpdateStatus resourceUpdate_ = AllocationUpdateStatus::NOT_FOUND;
    bool hostWriteRecorded_ = false;
    std::shared_ptr<SyncLedger> synchronization_ = std::make_shared<SyncLedger>();
    std::vector<NpuCheckSynccheckReport> synchronizationReports_;
    std::set<uint64_t> invalidSynchronization_;
    std::vector<HostAccess> hostOperations_;
    std::set<uint32_t> uncertainInitializationDevices_;
    void HostOperand(uint32_t device, uint64_t address, uint64_t bytes, uint32_t mode, const char* apiName);
    void QueryHostInitialization(HostAccess& access);
    void BuildStorageIndex(uint64_t launchId, Launch& launch, const race::DependencyGraph& dependency);
    static void CollectOverlappingWrites(
        const StorageWrites& writes, uint64_t begin, uint64_t end, std::vector<uint32_t>& output);
    void CommitWritePage(uint64_t launchId, Launch& launch, const race::Storage& storage);
    bool HostTransfer(const AclsanMemoryTransferData& transfer);
    bool HostWrite(const AclsanMemoryWriteData& write);
    void ObserveSynchronization(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data);
    bool HostEvent(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data);
    bool DeviceEvent(AclsanCallbackId id, const void* data);
    bool RegisterLaunch(const AclsanLaunchData& launch);
    bool CloseStream(const AclsanSynchronizeData& sync);
    bool ResourceEvent(AclsanCallbackId id, const AclsanResourceData& resource);
    HostOrder::NodeId HostNode();
};
} // namespace npucheck
#endif
