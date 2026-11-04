/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMCHECK_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMCHECK_H

#include "acl_san/aclsan_api.h"
#include "acl_san/aclsan_cbdata.h"
#include "checker/memory_analysis.h"
#include "checker/checker.h"
#include "diagnostic/device_protocol.h"
#include "diagnostic/report_renderer.h"

#include <cstdint>
#include <vector>

namespace npucheck {

struct MemcheckStats {
    uint64_t allocations = 0;
    uint64_t frees = 0;
    uint64_t deviceOperations = 0;
    uint64_t synchronizationEvents = 0;
    uint64_t errors = 0;
    uint64_t warnings = 0;
    uint64_t pendingDeviceOperations = 0;
    uint64_t droppedDeviceOperations = 0;
    uint64_t failedLaunches = 0;
    uint64_t failedSynchronizations = 0;
    uint64_t inexactDeviceOperations = 0;
    uint64_t hostOperations = 0, incompleteHostOperations = 0;
};

class Memcheck final : public Checker {
public:
    explicit Memcheck(std::shared_ptr<MemoryAnalysis> analysis = {})
        : analysis_(analysis ? std::move(analysis) : std::make_shared<MemoryAnalysis>()), memoryState_(analysis_->state)
    {}
    const std::vector<CallbackSpec>& GetSubscribedID() const override;
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) override;
    std::string Summary() const override;
    bool HasErrors() const override;
    bool AnalysisComplete() const override;

    void OnAllocation(const AclsanResourceData& data);
    void OnFree(const AclsanResourceData& data);
    void QueueDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& data);
    std::vector<npucheck::NpuCheckMemcheckReport> OnSynchronization();
    MemcheckStats Stats() const;

private:
    std::vector<npucheck::NpuCheckMemcheckReport> CheckAccess(
        const AclsanDeviceMemoryAccessData& data, npucheck::NpuCheckReportAccessMode accessMode, uint64_t address,
        uint64_t bytes, uint64_t groupId);
    std::vector<npucheck::NpuCheckMemcheckReport> CheckDeviceMemoryAccess(
        const AclsanDeviceMemoryAccessData& data, uint64_t groupId);
    void OnHostMemory(const AclsanCallbackCommonData& common, CheckerReportList& reports);
    void Count(const std::vector<npucheck::NpuCheckMemcheckReport>& reports);

    std::shared_ptr<MemoryAnalysis> analysis_;
    std::shared_ptr<MemoryState> memoryState_;
    std::vector<AclsanDeviceMemoryAccessData> pendingDeviceAccesses_;
    MemcheckStats stats_{};
    uint64_t nextReportId_ = 1;
    uint64_t nextGroupId_ = 1;
    static constexpr size_t maxPendingDeviceOperations = 1u << 20u;
};

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMCHECK_H
