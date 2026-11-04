/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_ALLOCATION_REGISTRY_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_ALLOCATION_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace npucheck {

struct Allocation {
    uint64_t resourceId = 0;
    uint64_t base = 0;
    uint64_t bytes = 0;
    uint32_t deviceId = 0;
    uint64_t allocSequence = 0;
    uint64_t freeSequence = 0;
};

enum class AllocationUpdateStatus : uint8_t {
    OK,
    INVALID_RANGE,
    OVERLAP,
    NOT_FOUND,
    DOUBLE_FREE,
    STALE_RESOURCE,
};

enum class RangeStatus : uint8_t {
    VALID,
    OUT_OF_BOUNDS,
    USE_AFTER_FREE,
    AMBIGUOUS,
    UNKNOWN,
    OVERFLOW,
};

struct RangeResult {
    RangeStatus status = RangeStatus::UNKNOWN;
    std::optional<Allocation> allocation;
};

// Session ledger for device allocations. Every allocation exists exactly once: the
// live and tombstone maps only hold indices into `records_`, so lifecycle queries and
// host-order stamps read the same record. Adding a second copy of the geometry (as the
// former per-session version list did) made the two histories drift once either side
// trimmed or capped its own entries.
class AllocationRegistry {
public:
    static constexpr size_t kNoNode = static_cast<size_t>(-1);
    static constexpr size_t kMaxRecords = 100000;

    struct Record {
        Allocation allocation;
        size_t allocated = kNoNode;  // host order node that made the allocation visible
        std::optional<size_t> freed; // host order node of a successful free
    };

    AllocationUpdateStatus Register(
        uint64_t resourceId, uint64_t base, uint64_t bytes, uint32_t deviceId, size_t node = kNoNode);
    AllocationUpdateStatus Release(uint64_t resourceId, uint64_t base, uint32_t deviceId, size_t node = kNoNode);
    RangeResult Classify(uint32_t deviceId, uint64_t address, uint64_t bytes) const;
    std::optional<Allocation> Nearest(uint32_t deviceId, uint64_t address) const;

    const std::vector<Record>& Records() const { return records_; }
    bool Complete() const { return complete_; }

private:
    using IndexMap = std::map<uint64_t, uint32_t>;
    using ResourceKey = std::pair<uint32_t, uint64_t>;

    static std::optional<uint64_t> RangeEnd(uint64_t base, uint64_t bytes);
    static bool Overlaps(uint64_t firstBase, uint64_t firstBytes, uint64_t secondBase, uint64_t secondBytes);
    const Allocation& At(uint32_t index) const { return records_[index].allocation; }
    Allocation& At(uint32_t index) { return records_[index].allocation; }
    std::optional<Allocation> FindStartingAllocation(const IndexMap& allocations, uint64_t address) const;
    std::optional<Allocation> FindCrossedAllocation(const IndexMap& allocations, uint64_t address, uint64_t end) const;
    static uint64_t DistanceToAllocation(const Allocation& allocation, uint64_t address);
    size_t TombstoneCount() const;
    void EraseReusedTombstones(uint64_t base, uint64_t bytes, uint32_t deviceId);
    void TrimTombstones();
    uint32_t AppendRecord(const Allocation& allocation, size_t allocated, std::optional<size_t> freed);

    std::vector<Record> records_;
    std::unordered_map<uint32_t, IndexMap> live_;
    std::unordered_map<uint32_t, IndexMap> tombstones_;
    std::map<ResourceKey, uint32_t> liveByResource_;
    uint64_t nextSequence_ = 1;
    bool complete_ = true;
    static constexpr size_t kMaxTombstones = 4096;
};

} // namespace npucheck
#endif
