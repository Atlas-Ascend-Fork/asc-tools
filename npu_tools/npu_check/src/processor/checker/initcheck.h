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

// One state byte per application byte: [6:4] access state, [3:0] writer pipeline.
// Reads never change initialization state. This model does not propagate value taint.
enum class InitMemoryStatus : uint8_t { DEFAULT = 0, READ = 1, GLOBAL_READ = 2, WRITE = 3, RACE = 4 };
struct InitStateQuery {
    uint64_t uninitializedBytes = 0;
    uint64_t firstUninitializedAddress = 0;
};
class InitStateTable {
public:
    static constexpr uint64_t kPageBytes = UINT64_C(1) << 16;
    virtual ~InitStateTable() = default;
    virtual InitStateQuery Query(uint64_t address, uint64_t bytes) const = 0;
    virtual void Write(uint64_t address, uint64_t bytes, uint8_t pipe) = 0;
    virtual void Clear(uint64_t address, uint64_t bytes) = 0;
    virtual uint64_t LeafCount() const = 0;
};
namespace detail {
constexpr uint8_t kInitWrite = static_cast<uint8_t>(InitMemoryStatus::WRITE) << 4;
inline bool InitWritten(uint8_t state) { return (state & 0x70) == kInitWrite; }
struct InitStatePage {
    std::array<uint8_t, InitStateTable::kPageBytes> states{};
    uint32_t writtenBytes = 0;
};
} // namespace detail

class GmInitStateTable final : public InitStateTable {
public:
    static constexpr uint64_t kAddressLimit = UINT64_C(1) << 48;
    GmInitStateTable();
    static bool ValidRange(uint64_t address, uint64_t bytes);
    InitStateQuery Query(uint64_t address, uint64_t bytes) const override;
    void Write(uint64_t address, uint64_t bytes, uint8_t pipe) override;
    void Clear(uint64_t address, uint64_t bytes) override;
    uint64_t LeafCount() const override { return leafCount_; }

private:
    using Leaf = detail::InitStatePage;
    using Middle = std::array<std::unique_ptr<Leaf>, kPageBytes>;
    using Root = std::array<std::unique_ptr<Middle>, kPageBytes>;
    void ValidateRange(uint64_t address, uint64_t bytes) const;
    const Leaf* FindLeaf(uint64_t address) const;
    std::unique_ptr<Leaf>& LeafSlot(uint64_t address);
    std::unique_ptr<Root> root_;
    uint64_t leafCount_ = 0;
};

// Shared bounded-page mechanics. Only named memory-space tables can be constructed.
class LocalInitStateTable : public InitStateTable {
public:
    InitStateQuery Query(uint64_t address, uint64_t bytes) const override;
    void Write(uint64_t address, uint64_t bytes, uint8_t pipe) override;
    void Clear(uint64_t address, uint64_t bytes) override;
    uint64_t LeafCount() const override { return leafCount_; }

protected:
    explicit LocalInitStateTable(uint64_t capacity);

private:
    using Leaf = detail::InitStatePage;
    void ValidateRange(uint64_t address, uint64_t bytes) const;
    const Leaf* FindLeaf(uint64_t address) const;
    std::unique_ptr<Leaf>& LeafSlot(uint64_t address);
    const uint64_t capacity_;
    std::vector<std::unique_ptr<Leaf>> pages_;
    uint64_t leafCount_ = 0;
};

class L1InitStateTable final : public LocalInitStateTable {
public:
    // dav-3510 local memory capacity, in bytes.
    static constexpr uint64_t kCapacity = 512 * 1024;
    L1InitStateTable();
};

class L0CInitStateTable final : public LocalInitStateTable {
public:
    // dav-3510 local memory capacity, in bytes.
    static constexpr uint64_t kCapacity = 256 * 1024;
    L0CInitStateTable();
};

struct InitStateKey {
    uint32_t deviceId = 0;
    uint64_t contextId = 0;
    uint64_t launchId = 0;
    uint32_t blockType = 0;
    uint32_t blockId = 0;
    uint32_t space = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    static InitStateKey Local(uint32_t device, uint64_t launch, uint32_t type, uint32_t block, uint32_t space)
    {
        return {device, 0, launch, type, block, space};
    }
    bool operator<(const InitStateKey& other) const
    {
        return std::tie(space, deviceId, contextId, launchId, blockType, blockId) <
               std::tie(other.space, other.deviceId, other.contextId, other.launchId, other.blockType, other.blockId);
    }
};

class InitStateTableIndex {
public:
    static uint64_t LocalCapacity(uint32_t space);
    static bool ValidKey(const InitStateKey& key);
    InitStateTable& GetOrCreate(const InitStateKey& key);
    InitStateTable* Find(const InitStateKey& key);
    void Erase(const InitStateKey& key);
    void EraseLaunch(uint32_t device, uint64_t launch);

private:
    std::map<InitStateKey, std::unique_ptr<InitStateTable>> tables_;
};

enum class InitCheckStatus {
    OK,
    INVALID_ALLOCATION,
    INVALID_RECORD,
    INCOMPLETE_RECORD,
    RESOURCE_EXHAUSTED,
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
    using InitializedRanges = std::vector<std::pair<uint64_t, uint64_t>>;
    AllocationUpdateStatus RegisterAllocation(InitStateKey scope, uint64_t id, uint64_t address, uint64_t bytes);
    AllocationUpdateStatus ReleaseAllocation(InitStateKey scope, uint64_t id, uint64_t address);
    InitCheckStatus MarkInitialized(InitStateKey scope, uint64_t address, uint64_t bytes, uint8_t pipe = 0);
    // Local dependency replay restricts reads to table bytes covered by proven
    // preceding writes. Other stored writes must not establish visibility.
    InitCheckResult CheckInstruction(
        InitStateKey scope, const std::vector<AclsanDeviceMemoryAccessData>& accesses,
        const InitializedRanges& precedingWrites = {}, bool commitWrites = true,
        bool restrictToPrecedingWrites = false);
    void ResetContext(InitStateKey scope);
    void ResetLaunch(uint32_t device, uint64_t launch) { tables_.EraseLaunch(device, launch); }

private:
    InitStateTableIndex tables_;
    std::map<InitStateKey, AllocationRegistry> gmAllocations_;
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
    MemoryInitcheck memoryInitcheck_;
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
