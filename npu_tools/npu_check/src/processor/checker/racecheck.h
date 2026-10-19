/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_CHECK_RACECHECK_H
#define NPU_CHECK_RACECHECK_H
#include "checker/checker.h"
#include "checker/racecheck/types.h"
#include <map>
#include <set>
#include <string>

namespace npucheck {
class Racechecker final : public Checker {
public:
    explicit Racechecker(race::Limits limits = {});
    const std::vector<CallbackSpec>& GetSubscribedID() const override;
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) override;
    void Finalize(CheckerReportList& reports) override;
    std::string Summary() const override;
    bool HasErrors() const override;
    bool AnalysisComplete() const override;

private:
    struct Launch {
        void* stream = nullptr;
        bool registered = false;
        std::string name;
        std::vector<AclsanDeviceMemoryAccessData> memory;
        std::vector<AclsanDeviceSyncData> sync;
        // Preserve fragment identity only, so an intentionally skipped fragment
        // is not mistaken for a missing callback from the same instruction.
        std::map<race::Instruction, std::set<uint32_t>> skippedFragments;
        uint64_t skippedMemory = 0;
        std::set<std::string> reasons;
    };
    void Finish(uint64_t id, Launch& launch, CheckerReportList& reports);
    void Analyze(uint64_t id, Launch& launch, CheckerReportList& reports);
    void Collect(void* stream, bool final, CheckerReportList& reports);
    race::Limits limits_;
    std::map<uint64_t, Launch> launches_;
    std::set<uint64_t> finished_;
    std::map<std::string, uint64_t> reasons_;
    uint64_t memoryEvents_ = 0, syncEvents_ = 0, launchesCount_ = 0, uncertain_ = 0;
    uint64_t skippedMemoryEvents_ = 0, skippedSyncEvents_ = 0;
    uint64_t raw_ = 0, war_ = 0, waw_ = 0, stoppedLaunches_ = 0;
    size_t graphNodesPeak_ = 0, graphEdgesPeak_ = 0, graphQueryWorkPeak_ = 0;
    size_t stateBytesPeak_ = 0, stateSegmentsPeak_ = 0;
    bool incomplete_ = false;
};
} // namespace npucheck
#endif
