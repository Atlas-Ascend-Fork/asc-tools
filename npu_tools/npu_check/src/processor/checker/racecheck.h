/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_RACECHECK_H
#define NPU_TOOLS_NPU_CHECK_RACECHECK_H
#include "checker/checker.h"
#include "checker/mem_state_tracker.h"
#include "checker/racecheck/types.h"
#include "checker/racecheck/dependency_graph.h"
#include "checker/racecheck/happens_before_graph.h"
#include <map>
#include <memory>
#include <set>

namespace npucheck {
enum class RacecheckOrder { Ordered, Unordered, Unknown };

// 独立同步模块契约：Ordered 证明 previous->current；
// Unordered 确认存在竞争；Unknown 必须停止当前 launch，不能发布结果。
// provider 观察因果提交、接收同步事件，并且必须覆盖完整轨迹，
// AnalysisComplete 才能返回 true。provider 的生命周期必须长于 Racecheck。
class RacecheckOrdering {
public:
    virtual ~RacecheckOrdering() = default;
    virtual RacecheckOrder Compare(
        const AclsanDeviceMemoryAccessData& previous, const AclsanDeviceMemoryAccessData& current) = 0;
    virtual void OnDeviceSync(const AclsanDeviceSyncData& data) = 0;
    virtual bool AnalysisComplete() const = 0;
};

class Racecheck final : public Checker {
public:
    // 默认详细追踪使用 ASCTOOL_DEBUG，失败使用 ASCTOOL_ERROR；可注入日志接收器。
    explicit Racecheck(RacecheckOrdering* ordering = nullptr, RacecheckDiagnostics diagnostics = {});
    const std::vector<CallbackSpec>& GetSubscribedID() const override;
    bool OnCallback(
        AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports) override;
    std::string Summary() const override;
    bool HasErrors() const override;
    bool AnalysisComplete() const override;

private:
    struct LaunchState {
        std::vector<AclsanDeviceMemoryAccessData> records;
        MemStateTracker tracker;
        explicit LaunchState(RacecheckDiagnostics diagnostics) : tracker(records, diagnostics) {}
    };
    bool OnMemoryAccess(const AclsanDeviceMemoryAccessData& data, CheckerReportList& reports);
    void Log(const char* event, const AclsanDeviceEventHeader* header, const char* detail, bool error = false)
        const noexcept;
    void StopLaunch(
        const AclsanDeviceEventHeader& header, const char* stage, const char* reason, const char* detail) noexcept;

    RacecheckOrdering* ordering_;
    RacecheckDiagnostics diagnostics_;
    // 空指针是已停止 launch 的标记，后续访问不得重新创建状态。
    std::map<std::pair<uint32_t, uint64_t>, std::unique_ptr<LaunchState>> launches_;
    // 连停止标记都无法创建时，停止会话后续访存分析。
    bool stopped_ = false;
    uint64_t accesses_ = 0;
    uint64_t committed_ = 0;
    uint64_t candidates_ = 0;
    uint64_t reportCount_ = 0;
    uint64_t failures_ = 0;
    uint64_t syncs_ = 0;
    bool warnedUnavailable_ = false;
};
// Production entry: collect a launch, seal its HB graph, then replay into MemStateTracker.
// Racecheck above remains available for externally ordered streaming callers.
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
    class LaunchProcessor;
    bool StoreDeviceEvent(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data);
    void Finish(uint64_t id, Launch& launch, CheckerReportList& reports);
    void Analyze(uint64_t id, Launch& launch, CheckerReportList& reports);
    void Collect(void* stream, bool final, CheckerReportList& reports);
    race::Limits limits_;
    std::map<uint64_t, Launch> launches_;
    std::set<uint64_t> finished_;
    std::map<std::string, uint64_t> reasons_;
    uint64_t memoryEvents_ = 0, syncEvents_ = 0, launchesCount_ = 0;
    uint64_t skippedMemoryEvents_ = 0, skippedSyncEvents_ = 0;
    uint64_t raw_ = 0, war_ = 0, waw_ = 0, atomic_ = 0;
    size_t graphNodesPeak_ = 0, graphEdgesPeak_ = 0, graphQueryWorkPeak_ = 0;
    size_t stateBytesPeak_ = 0, stateSegmentsPeak_ = 0;
    bool incomplete_ = false;
};
} // namespace npucheck
#endif
