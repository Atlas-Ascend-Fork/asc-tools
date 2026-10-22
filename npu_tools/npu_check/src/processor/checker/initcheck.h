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
#include "checker/allocation_registry.h"
#include "checker/checker.h"
#include "diagnostic/report_message.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace npucheck {

// 状态字：[7] 同步辅助标志，[6:4] 访问状态，[3:0] 流水编号。
// 此阶段仅维护 DEFAULT/WRITE；读取不修改状态，避免将“读过”误认为“写过”。
enum class GmMemoryStatus : uint8_t { DEFAULT = 0, READ = 1, GLOBAL_READ = 2, WRITE = 3, RACE = 4 };

struct GmInitQuery {
    uint64_t uninitializedBytes = 0;
    uint64_t firstUninitializedAddress = 0;
};

class GmInitState {
public:
    static constexpr uint64_t kAddressLimit = UINT64_C(1) << 48;
    static constexpr uint64_t kPageBytes = UINT64_C(1) << 16;
    GmInitState();
    static bool ValidRange(uint64_t address, uint64_t bytes);
    GmInitQuery Query(uint64_t address, uint64_t bytes) const;
    void Write(uint64_t address, uint64_t bytes, uint8_t pipe);
    void Clear(uint64_t address, uint64_t bytes);
    uint64_t LeafCount() const { return leafCount_; }

private:
    struct Leaf {
        std::array<uint8_t, kPageBytes> states{};
        uint32_t writtenBytes = 0;
    };
    using Middle = std::array<std::unique_ptr<Leaf>, kPageBytes>;
    using Root = std::array<std::unique_ptr<Middle>, kPageBytes>;
    std::unique_ptr<Root> root_;
    uint64_t leafCount_ = 0;
};

struct GmScope {
    uint32_t deviceId = 0;
    uint64_t contextId = 0;
    bool operator<(const GmScope& other) const
    {
        return std::tie(deviceId, contextId) < std::tie(other.deviceId, other.contextId);
    }
};
enum class GmCheckStatus {
    OK,
    INVALID_ALLOCATION,
    INVALID_RECORD,
    INCOMPLETE_RECORD,
    RESOURCE_EXHAUSTED,
};
struct GmCheckResult {
    GmCheckStatus status = GmCheckStatus::OK;
    std::vector<NpuCheckInitcheckReport> reports;
};

// Host 内存检查核心；默认 GM，片上实例使用固定空间容量且由调用方按 launch/core 隔离。
// 同步回放传入已证明先行的写范围并关闭 commitWrites，避免任意回放顺序污染基线。
// 普通顺序调用方必须按依赖顺序提交完整指令，并在写入完成后调用 MarkInitialized。
// free 必须晚于使用该分配的记录回放；contextId 必须标识上下文实例而非可复用裸句柄。
// 非 OK 表示该操作未完成检测，不能当作“无错误”；记录丢失后应停止使用该上下文并重建。
class GmInitcheck {
public:
    explicit GmInitcheck(uint32_t space = ACLSAN_DEVICE_MEMORY_SPACE_GM, uint64_t capacity = 0)
        : space_(space), capacity_(capacity)
    {}
    // Half-open, sorted and disjoint ranges from writes proven to precede this instruction.
    using InitializedRanges = std::vector<std::pair<uint64_t, uint64_t>>;
    AllocationUpdateStatus RegisterAllocation(GmScope scope, uint64_t id, uint64_t address, uint64_t bytes);
    AllocationUpdateStatus ReleaseAllocation(GmScope scope, uint64_t id, uint64_t address);
    GmCheckStatus MarkInitialized(GmScope scope, uint64_t address, uint64_t bytes, uint8_t pipe = 0);
    GmCheckResult CheckInstruction(
        GmScope scope, const std::vector<AclsanDeviceMemoryAccessData>& accesses,
        const InitializedRanges& precedingWrites = {}, bool commitWrites = true);
    void ResetContext(GmScope scope);

private:
    struct Context {
        AllocationRegistry allocations;
        GmInitState state;
    };
    uint32_t space_;
    uint64_t capacity_;
    std::map<GmScope, Context> contexts_;
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
    std::set<UsageKey> reportedUsages_;
    std::map<InstructionKey, uint64_t> reportGroups_;
    InitcheckStats stats_{};
    uint64_t nextReportId_ = 1;
    uint64_t nextGroupId_ = 1;
    GmInitcheck gmInitcheck_;
    std::set<void*> pendingLaunchStreams_;
    std::vector<AclsanDeviceMemoryAccessData> pendingMemoryAccesses_;
    std::vector<AclsanDeviceSyncData> pendingSyncEvents_;
    std::map<uint64_t, void*> launchStreams_;
    std::map<uint64_t, std::set<std::string>> replayReasons_;
    std::map<std::string, uint64_t> replayFailures_;
    std::set<uint32_t> uncertainDevices_;
    uint64_t syncEvents_ = 0;
};

} // namespace npucheck

#endif
