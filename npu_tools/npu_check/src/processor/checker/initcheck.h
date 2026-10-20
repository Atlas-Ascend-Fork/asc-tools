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

#include "checker/checker.h"

#include <cstdint>
#include <map>
#include <set>
#include <tuple>

namespace npucheck {

struct InitcheckStats {
    uint64_t registerWrites = 0;
    uint64_t deviceOperations = 0;
    uint64_t errors = 0;
    uint64_t synchronizations = 0;
    uint64_t malformedEvents = 0;
    uint64_t failedLaunches = 0;
    uint64_t failedSynchronizations = 0;
};

class Initcheck final : public Checker {
public:
    const std::vector<CallbackSpec>& GetSubscribedID() const override;
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) override;
    std::string Summary() const override;
    bool HasErrors() const override;
    bool AnalysisComplete() const override;
    InitcheckStats Stats() const;

private:
    // (blockType, blockId) 唯一标识一次 device/launch 内的执行。
    using CoreKey = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t>;
    using UsageKey = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t, uint32_t>;
    using InstructionKey = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t>;

    bool OnRegisterState(const AclsanDeviceRegisterStateData& data);
    bool OnMemoryAccess(const AclsanDeviceMemoryAccessData& data, CheckerReportList& reports);
    NpuCheckInitcheckReport MakeReport(const AclsanDeviceMemoryAccessData& data, uint32_t registerId, uint64_t groupId);
    void OnSynchronization(const AclsanSynchronizeData& data);

    std::map<CoreKey, uint64_t> initializedRegisters_;
    std::set<UsageKey> reportedUsages_;
    std::map<InstructionKey, uint64_t> reportGroups_;
    InitcheckStats stats_{};
    uint64_t nextReportId_ = 1;
    uint64_t nextGroupId_ = 1;
};

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_INITCHECK_H
