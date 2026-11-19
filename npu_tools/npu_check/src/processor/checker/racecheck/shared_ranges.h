/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_CHECK_SHARED_RANGES_H
#define NPU_CHECK_SHARED_RANGES_H

#include "checker/racecheck/types.h"
#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>

namespace npucheck::race {
inline size_t SaturatingRangeCount(size_t current, size_t extra)
{
    const auto maximum = std::numeric_limits<size_t>::max();
    return extra > maximum - current ? maximum : current + extra;
}

// Counts actual vector capacities. Histories retain this state through storage leases.
// Cache ownership is deliberately not part of this state, avoiding a reference cycle.
class RangeStorageBudget {
public:
    explicit RangeStorageBudget(size_t limit) : limit_(limit) {}
    size_t Used() const { return used_.load(std::memory_order_relaxed); }
    size_t Peak() const { return peak_.load(std::memory_order_relaxed); }
    size_t Limit() const { return limit_; }

private:
    friend class RangeStorageReservation;
    friend class RangeStorage;
    bool Reserve(size_t count)
    {
        const auto maximum = std::numeric_limits<size_t>::max() / sizeof(Interval);
        auto used = used_.load(std::memory_order_relaxed);
        for (;;) {
            if (used > limit_ || used > maximum || count > limit_ - used || count > maximum - used)
                return false;
            const auto next = used + count;
            if (used_.compare_exchange_weak(used, next, std::memory_order_relaxed)) {
                auto peak = peak_.load(std::memory_order_relaxed);
                while (peak < next && !peak_.compare_exchange_weak(peak, next, std::memory_order_relaxed)) {
                }
                return true;
            }
        }
    }
    void Release(size_t count) noexcept { used_.fetch_sub(count, std::memory_order_relaxed); }
    const size_t limit_;
    std::atomic<size_t> used_{0};
    std::atomic<size_t> peak_{0};
};

class RangeStorageReservation {
public:
    RangeStorageReservation(std::shared_ptr<RangeStorageBudget> budget, size_t count)
        : budget_(std::move(budget)), count_(count), reserved_(budget_ && budget_->Reserve(count))
    {}
    ~RangeStorageReservation()
    {
        if (reserved_)
            budget_->Release(count_);
    }
    RangeStorageReservation(const RangeStorageReservation&) = delete;
    RangeStorageReservation& operator=(const RangeStorageReservation&) = delete;
    bool Reserved() const { return reserved_; }
    void Transfer() noexcept { reserved_ = false; }

private:
    std::shared_ptr<RangeStorageBudget> budget_;
    size_t count_;
    bool reserved_;
};

class RangeStorage {
public:
    class AdmissionKey {
        friend class RangeStoragePool;
        AdmissionKey() = default;
    };
    RangeStorage(AdmissionKey, std::vector<Interval>&& values, std::shared_ptr<RangeStorageBudget> budget) noexcept
        : values_(std::move(values)), budget_(std::move(budget))
    {}
    ~RangeStorage()
    {
        const auto count = values_.capacity();
        std::vector<Interval>().swap(values_);
        budget_->Release(count);
    }
    RangeStorage(const RangeStorage&) = delete;
    RangeStorage& operator=(const RangeStorage&) = delete;
    const std::vector<Interval>& Values() const { return values_; }

private:
    std::vector<Interval> values_;
    std::shared_ptr<RangeStorageBudget> budget_;
};

class SharedRanges {
public:
    SharedRanges() = default;
    explicit SharedRanges(std::shared_ptr<const RangeStorage> storage) : storage_(std::move(storage)) {}
    const std::vector<Interval>& Values() const
    {
        static const std::vector<Interval> empty;
        return storage_ ? storage_->Values() : empty;
    }
    operator const std::vector<Interval>&() const { return Values(); }
    auto begin() const { return Values().begin(); }
    auto end() const { return Values().end(); }
    size_t size() const { return Values().size(); }
    bool empty() const { return Values().empty(); }
    const Interval& front() const { return Values().front(); }
    const Interval& back() const { return Values().back(); }
    const Interval& operator[](size_t index) const { return Values()[index]; }
    const void* StorageIdentity() const { return storage_.get(); }

private:
    std::shared_ptr<const RangeStorage> storage_;
};

enum class RangeStorageStatus { Complete, InvalidLayout, MaterializationBudget, StorageBudget };
class RangeStoragePool {
public:
    struct Statistics {
        size_t calls = 0;
        size_t hits = 0;
        size_t materialized = 0;
        size_t admitted = 0;
        size_t storageRejected = 0;
        size_t materializationRejected = 0;
        size_t untracked = 0;
        bool cacheDisabled = false;
    };
    explicit RangeStoragePool(const Limits& limits, std::shared_ptr<RangeStorageBudget> budget = {})
        : limits_(limits),
          budget_(budget ? std::move(budget) : std::make_shared<RangeStorageBudget>(limits.LiveRangeIntervalBudget()))
    {
        if (budget_->Limit() > limits_.LiveRangeIntervalBudget())
            throw std::invalid_argument("Range storage budget exceeds immutable analysis policy");
    }
    RangeStorageStatus Normalize(const AclsanDeviceMemoryAccessData& access, SharedRanges& result)
    {
        result = {};
        stats_.calls = SaturatingRangeCount(stats_.calls, 1);
        NormalizationObservation geometry;
        if (DescribeNormalization(access, geometry) != NormalizationStatus::Complete)
            return RangeStorageStatus::InvalidLayout;
        if (geometry.empty)
            return RangeStorageStatus::Complete;
        auto key = GeometryKey(geometry);
        if (key) {
            const auto found = cache_.find(*key);
            if (found != cache_.end()) {
                stats_.hits = SaturatingRangeCount(stats_.hits, 1);
                result = found->second;
                return RangeStorageStatus::Complete;
            }
        }
        return NormalizeNew(access, key, result);
    }
    RangeStorageStatus Admit(std::vector<Interval> values, SharedRanges& result)
    {
        result = {};
        for (const auto range : values)
            if (range.begin >= range.end)
                return RangeStorageStatus::InvalidLayout;
        if (values.empty())
            return RangeStorageStatus::Complete;
        MergeIntervals(values);
        std::vector<Interval> compact(values.begin(), values.end());
        std::vector<Interval>().swap(values);
        RangeStorageReservation reservation(budget_, compact.capacity());
        if (!reservation.Reserved()) {
            stats_.storageRejected = SaturatingRangeCount(stats_.storageRejected, 1);
            return RangeStorageStatus::StorageBudget;
        }
        auto storage = std::make_shared<RangeStorage>(RangeStorage::AdmissionKey{}, std::move(compact), budget_);
        reservation.Transfer();
        result = SharedRanges(std::move(storage));
        stats_.admitted = SaturatingRangeCount(stats_.admitted, 1);
        return RangeStorageStatus::Complete;
    }
    const Statistics& Stats() const { return stats_; }
    const std::shared_ptr<RangeStorageBudget>& Budget() const { return budget_; }
    size_t CachedKeys() const { return cache_.size(); }
    void ClearCache() { cache_.clear(); }

private:
    using Key = std::tuple<uint64_t, uint64_t, std::vector<std::pair<uint64_t, uint64_t>>>;
    std::optional<Key> GeometryKey(const NormalizationObservation& geometry)
    {
        if (stats_.cacheDisabled || limits_.RangeGeometryCacheEntryBudget() == 0)
            return std::nullopt;
        try {
            return Key{geometry.base, geometry.bytes, geometry.sparseAxes};
        } catch (const std::bad_alloc&) {
            DisableCache();
            return std::nullopt;
        }
    }
    void DisableCache()
    {
        cache_.clear();
        stats_.cacheDisabled = true;
    }
    void Remember(const std::optional<Key>& key, const SharedRanges& ranges)
    {
        if (!key || stats_.cacheDisabled || cache_.size() >= limits_.RangeGeometryCacheEntryBudget()) {
            stats_.untracked = SaturatingRangeCount(stats_.untracked, 1);
            return;
        }
        try {
            cache_.emplace(*key, ranges);
        } catch (const std::bad_alloc&) {
            DisableCache();
            stats_.untracked = SaturatingRangeCount(stats_.untracked, 1);
        }
    }
    RangeStorageStatus NormalizeNew(
        const AclsanDeviceMemoryAccessData& access, const std::optional<Key>& key, SharedRanges& result)
    {
        std::vector<Interval> values;
        const auto status = NormalizeDetailed(access, limits_.FootprintIntervalBudget(), values);
        if (status == NormalizationStatus::InvalidLayout)
            return RangeStorageStatus::InvalidLayout;
        if (status == NormalizationStatus::SegmentBudget) {
            stats_.materializationRejected = SaturatingRangeCount(stats_.materializationRejected, 1);
            return RangeStorageStatus::MaterializationBudget;
        }
        stats_.materialized = SaturatingRangeCount(stats_.materialized, 1);
        const auto admitted = Admit(std::move(values), result);
        if (admitted == RangeStorageStatus::Complete)
            Remember(key, result);
        return admitted;
    }
    const Limits limits_;
    std::shared_ptr<RangeStorageBudget> budget_;
    Statistics stats_;
    std::map<Key, SharedRanges> cache_;
};
} // namespace npucheck::race
#endif
