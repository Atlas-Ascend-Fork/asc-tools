/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_H
#include "checker/checker.h"
#include "checker/memory_analysis.h"
#include "checker/mem_state_tracker.h"
#include "checker/racecheck/types.h"
#include "checker/racecheck/dependency_graph.h"
#include "checker/racecheck/happens_before_graph.h"
#include <map>
#include <memory>
#include <set>

namespace npucheck {
// Production entry: collect a launch, seal its HB graph, then replay into MemStateTracker.
class Racechecker final : public Checker {
public:
    explicit Racechecker(race::Limits limits = {}, std::shared_ptr<MemoryAnalysis> analysis = {});
    const std::vector<CallbackSpec>& GetSubscribedID() const override;
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) override;
    void Finalize(CheckerReportList& reports) override;
    std::string Summary() const override;
    bool HasErrors() const override;
    bool AnalysisComplete() const override;

private:
    struct Launch {
        uint64_t id;
        MemoryAnalysis::Launch owned;
        MemoryAnalysis::Launch* shared;
        std::vector<AclsanDeviceMemoryAccessData>& memory;
        std::vector<AclsanDeviceSyncData>& sync;
        std::vector<AclsanDeviceRegisterStateData>& registers;
        std::map<race::Instruction, std::set<uint32_t>>& skippedFragments;
        void* stream = nullptr;
        bool registered = false;
        std::string name;
        std::vector<AclsanDeviceBarrierData>& localBarriers;
        std::vector<AclsanDeviceVfData>& vfs;
        uint64_t skippedMemory = 0;
        std::set<std::string> reasons;
        explicit Launch(MemoryAnalysis* analysis = nullptr, uint64_t id = 0)
            : id(id),
              shared(analysis != nullptr && analysis->Managed() ? &analysis->Get(id) : &owned),
              memory(shared->memory),
              sync(shared->sync),
              registers(shared->registers),
              skippedFragments(shared->skippedFragments),
              localBarriers(shared->localBarriers),
              vfs(shared->vfs)
        {}
    };
    Launch& GetLaunch(uint64_t id) { return launches_.try_emplace(id, analysis_.get(), id).first->second; }
    std::shared_ptr<MemoryAnalysis> analysis_;
    class LaunchProcessor;
    bool MarkMalformedCallback();
    bool AcceptLaunch(uint64_t launchId);
    bool HandleHostMemory(AclsanCallbackId cbid, const void* data, CheckerReportList& reports);
    bool HandleStreamSynchronization(const void* data);
    bool HandleOperatorCompletion(const void* data, CheckerReportList& reports);
    bool HandleLaunch(const void* data);
    bool EventBudgetExceeded(Launch& launch, const AclsanDeviceEventHeader& header);
    bool FullDeviceEventBudget(const Launch& state) const;
    void StoreVfEvent(Launch& state, const AclsanDeviceVfData& vf);
    bool AcceptMemoryEvent(Launch& state, const AclsanDeviceMemoryAccessData& a);
    bool AcceptBarrierEvent(Launch& state, const AclsanDeviceBarrierData& barrier);
    bool AcceptSyncEvent(Launch& state, const AclsanDeviceSyncData& s);
    bool StoreDeviceEvent(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data);
    void Finish(uint64_t id, Launch& launch, CheckerReportList& reports);
    void Analyze(uint64_t id, Launch& launch, CheckerReportList& reports);
    void PublishConflicts(std::vector<NpuCheckRacecheckReport> conflicts, CheckerReportList& reports);
    void Collect(void* stream, bool final, CheckerReportList& reports);
    race::Limits limits_;
    std::map<uint64_t, Launch> launches_;
    std::set<uint64_t> finished_;
    std::map<std::string, uint64_t> reasons_;
    uint64_t launchesCount_ = 0, hostEvents_ = 0;
    uint64_t skippedMemoryEvents_ = 0, skippedSyncEvents_ = 0;
    uint64_t warnings_ = 0;
    uint64_t unsupportedUbEvents_ = 0;
    uint64_t raw_ = 0, war_ = 0, waw_ = 0, atomic_ = 0;
    size_t graphNodesPeak_ = 0, graphEdgesPeak_ = 0, graphQueryWorkPeak_ = 0;
    size_t stateBytesPeak_ = 0, stateSegmentsPeak_ = 0;
    bool incomplete_ = false;
};
} // namespace npucheck
#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_H
