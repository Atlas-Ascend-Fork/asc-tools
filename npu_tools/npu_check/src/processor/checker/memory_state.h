/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_CHECK_MEMORY_STATE_H
#define NPU_CHECK_MEMORY_STATE_H
#include "checker/allocation_registry.h"
#include "checker/mem_state_tracker.h"
#include <array>
#include <map>
#include <memory>
#include <tuple>
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

// Shared bounded-page mechanics; capacities come from the memory-space index.
class LocalInitStateTable : public InitStateTable {
public:
    InitStateQuery Query(uint64_t address, uint64_t bytes) const override;
    void Write(uint64_t address, uint64_t bytes, uint8_t pipe) override;
    void Clear(uint64_t address, uint64_t bytes) override;
    uint64_t LeafCount() const override { return leafCount_; }

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

// Session-owned state. Initialization pages and access histories allocate only
// when their checker is enabled and touches that storage.
class MemoryState {
public:
    InitStateTableIndex initialized;
    std::map<InitStateKey, AllocationRegistry> allocations;
    AllocationRegistry& Allocations(InitStateKey scope = {}) { return allocations[scope]; }

    void BeginEvent() { resourceApplied_ = false; }
    void EndEvent()
    {
        eventActive_ = false;
        resourceApplied_ = false;
    }
    void StartEvent()
    {
        BeginEvent();
        eventActive_ = true;
    }
    AllocationUpdateStatus Register(
        InitStateKey scope, uint64_t id, uint64_t address, uint64_t bytes, size_t node = AllocationRegistry::kNoNode);
    AllocationUpdateStatus Release(
        InitStateKey scope, uint64_t id, uint64_t address, size_t node = AllocationRegistry::kNoNode);
    // A single immutable replay record array backs race history indices.
    struct AccessHistory {
        std::vector<AclsanDeviceMemoryAccessData> records;
        MemStateTracker tracker;
        AccessHistory() : tracker(records) {}
    };
    // Physical GM history survives launch completion and allocation generations.
    // Local-memory histories remain isolated to their launch and owner.
    struct GlobalAccess {
        AclsanDeviceMemoryAccessData data;
        std::vector<racecheck::ByteRange> ranges;
    };
    std::vector<GlobalAccess> globalAccesses;
    bool globalHistoryComplete = true;
    size_t globalSegments = 0;
    std::vector<HostAccessEvidence> hostAccesses;
    std::unique_ptr<AccessHistory> NewAccessHistory() const { return std::make_unique<AccessHistory>(); }

private:
    bool eventActive_ = false;
    bool resourceApplied_ = false;
    AllocationUpdateStatus resourceStatus_ = AllocationUpdateStatus::NOT_FOUND;
};
} // namespace npucheck
#endif
