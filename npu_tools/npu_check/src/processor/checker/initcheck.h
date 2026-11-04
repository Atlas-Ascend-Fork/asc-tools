/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_INITCHECK_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_INITCHECK_H

#include "acl_san/aclsan_api.h"
#include "checker/memory_analysis.h"
#include "checker/checker.h"
#include "diagnostic/report_message.h"

#include <array>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace npucheck {

enum class InitCheckStatus {
    OK,
    INVALID_ALLOCATION,
    INVALID_RECORD,
    INCOMPLETE_RECORD,
    RESOURCE_EXHAUSTED,
    UNRESOLVED_ALLOCATION,
};
struct InitCheckResult {
    InitCheckStatus status = InitCheckStatus::OK;
    std::vector<NpuCheckInitcheckReport> reports;
};

// Memory-space-neutral instruction checker. State tables are owned by the index;
// allocation registries exist only for GM. Local keys identify device/launch/core/space.
// Replay checks all reads before publishing writes, with visibility proven by dependency edges.
// Non-OK means analysis did not complete and must not be reported as error-free.
class MemoryInitcheck {
public:
    // Half-open, sorted and disjoint ranges from writes proven to precede this instruction.
    explicit MemoryInitcheck(std::shared_ptr<MemoryState> state = {})
        : state_(state ? std::move(state) : std::make_shared<MemoryState>())
    {}
    void SetState(std::shared_ptr<MemoryState> state) { state_ = std::move(state); }
    using RangeClassifier = std::function<RangeResult(uint64_t, uint64_t)>;
    using InitializedRanges = std::vector<std::pair<uint64_t, uint64_t>>;
    AllocationUpdateStatus RegisterAllocation(InitStateKey scope, uint64_t id, uint64_t address, uint64_t bytes);
    AllocationUpdateStatus ReleaseAllocation(InitStateKey scope, uint64_t id, uint64_t address);
    InitCheckStatus MarkInitialized(InitStateKey scope, uint64_t address, uint64_t bytes, uint8_t pipe = 0);
    // Local dependency replay restricts reads to table bytes covered by proven
    // preceding writes. Other stored writes must not establish visibility.
    InitCheckResult CheckInstruction(
        InitStateKey scope, const std::vector<AclsanDeviceMemoryAccessData>& accesses,
        const InitializedRanges& precedingWrites = {}, bool commitWrites = true, bool restrictToPrecedingWrites = false,
        const RangeClassifier& classifier = {});
    void ResetContext(InitStateKey scope);
    void ResetLaunch(uint32_t device, uint64_t launch) { state_->initialized.EraseLaunch(device, launch); }

private:
    std::shared_ptr<MemoryState> state_;
    uint64_t nextReportId_ = 1;
    uint64_t nextGroupId_ = 1;
};

struct InitcheckStats {
    uint64_t registerWrites = 0;
    uint64_t deviceOperations = 0;
    uint64_t errors = 0;
    uint64_t synchronizations = 0;
    uint64_t malformedEvents = 0;
    uint64_t failedLaunches = 0;
    uint64_t failedSynchronizations = 0;
    uint64_t failedHostWrites = 0;
};

class Initcheck final : public Checker {
public:
    explicit Initcheck(std::shared_ptr<MemoryAnalysis> analysis = {})
        : analysis_(analysis ? std::move(analysis) : std::make_shared<MemoryAnalysis>()),
          memoryInitcheck_(analysis_->state)
    {}
    const std::vector<CallbackSpec>& GetSubscribedID() const override;
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) override;
    void Finalize(CheckerReportList& reports) override;
    std::string Summary() const override;
    bool HasErrors() const override;
    bool AnalysisComplete() const override;
    InitcheckStats Stats() const;

private:
    std::shared_ptr<MemoryAnalysis> analysis_;
    // (blockType, blockId) 唯一标识一次 device/launch 内的执行。
    using CoreKey = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t>;
    using UsageKey = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t, uint32_t>;
    using InstructionKey = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t>;

    bool OnRegisterState(const AclsanDeviceRegisterStateData& data);
    bool OnMemoryAccess(const AclsanDeviceMemoryAccessData& data, CheckerReportList& reports);
    void OnHostMemory(const AclsanCallbackCommonData& common, CheckerReportList& reports);
    bool OnHostWrite(const AclsanMemoryWriteData& data);
    void OnAllocation(const AclsanResourceData& data);
    void OnFree(const AclsanResourceData& data);
    NpuCheckInitcheckReport MakeReport(const AclsanDeviceMemoryAccessData& data, uint32_t registerId, uint64_t groupId);
    void OnSynchronization(const AclsanSynchronizeData& data, CheckerReportList& reports);
    bool OnDeviceSync(AclsanCallbackId cbid, const void* data);
    void FlushMemoryAccesses(void* stream, CheckerReportList& reports);
    void ReplayLaunch(
        uint64_t launchId, std::vector<AclsanDeviceMemoryAccessData>& memory, std::vector<AclsanDeviceSyncData>& sync,
        CheckerReportList& reports);

    std::map<CoreKey, uint64_t> initializedRegisters_;
    std::map<uint64_t, std::vector<AclsanDeviceRegisterStateData>> unitFlagRegisters_;
    std::set<UsageKey> reportedUsages_;
    std::map<InstructionKey, uint64_t> reportGroups_;
    bool memoryInvalid_ = false;
    uint64_t l0Operations_ = 0;
    InitcheckStats stats_{};
    uint64_t nextReportId_ = 1;
    uint64_t nextGroupId_ = 1;
    MemoryInitcheck memoryInitcheck_;
    std::set<void*> pendingLaunchStreams_;
    std::vector<AclsanDeviceMemoryAccessData> pendingMemoryAccesses_;
    std::vector<AclsanDeviceSyncData> pendingSyncEvents_;
    std::map<uint64_t, void*> launchStreams_;
    std::map<uint64_t, std::set<std::string>> replayReasons_;
    std::map<std::string, uint64_t> replayFailures_;
    std::set<uint32_t> uncertainDevices_;
    uint64_t syncEvents_ = 0;
    uint64_t degradedInstructions_ = 0;
};

} // namespace npucheck

#endif
