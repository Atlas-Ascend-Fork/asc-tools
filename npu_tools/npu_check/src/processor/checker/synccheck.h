/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_SYNCCHECK_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_SYNCCHECK_H
#include "checker/memory_analysis.h"
#include "checker/sync_ledger.h"

namespace npucheck {
// The managed checker reads the session ledger. Standalone callers own one ledger.
class Synccheck final : public Checker {
public:
    explicit Synccheck(std::shared_ptr<MemoryAnalysis> analysis = {})
        : analysis_(std::move(analysis)),
          ledger_(analysis_ ? analysis_->Synchronization() : std::make_shared<SyncLedger>())
    {}
    const std::vector<CallbackSpec>& GetSubscribedID() const override { return ledger_->GetSubscribedID(); }
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) override;
    std::string Summary() const override { return ledger_->Summary(); }
    bool HasErrors() const override { return ledger_->HasErrors(); }
    bool AnalysisComplete() const override { return ledger_->AnalysisComplete(); }
    void OnKernelLaunch(uint64_t id, const LaunchTopology& topology, void* stream = nullptr)
    {
        ledger_->OnKernelLaunch(id, topology, stream);
    }
    void OnDeviceSync(const AclsanDeviceSyncData& data) { ledger_->OnDeviceSync(data); }
    std::vector<NpuCheckSynccheckReport> OnSynchronization() { return ledger_->OnSynchronization(); }
    std::vector<NpuCheckSynccheckReport> OnSynchronization(void* stream) { return ledger_->OnSynchronization(stream); }
    SynccheckStats Stats() const { return ledger_->Stats(); }
    std::optional<LaunchTopology> TopologyForLaunch(uint64_t id) const { return ledger_->TopologyForLaunch(id); }
    std::optional<uint32_t> CrossCounterValue(uint64_t id, uint32_t group, ParticipantRole role, uint32_t flag) const
    {
        return ledger_->CrossCounterValue(id, group, role, flag);
    }
    size_t PendingCrossSetRoundCount(uint64_t id) const { return ledger_->PendingCrossSetRoundCount(id); }

private:
    std::shared_ptr<MemoryAnalysis> analysis_;
    std::shared_ptr<SyncLedger> ledger_;
};
} // namespace npucheck
#endif
