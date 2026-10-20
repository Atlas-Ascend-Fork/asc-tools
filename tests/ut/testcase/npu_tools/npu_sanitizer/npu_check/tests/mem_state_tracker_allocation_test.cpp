// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

// 隔离的可执行文件：分配器覆盖实现不会链接进 LLT 二进制。
#include "acl_san/aclsan_api.h"
#include "checker/mem_state_tracker.h"
#include "checker/racecheck_reports.h"
#include "checker/racecheck.h"
#include <cstdio>
#include <gtest/gtest.h>
#include <cstdlib>
#include <new>
#include <sstream>

namespace {
thread_local int allocationFailureCountdown = -1;
thread_local bool failAllAllocations = false;
thread_local size_t trackedMinimumAllocationSize = 0;
thread_local size_t trackedExactAllocationSize = 0;
thread_local size_t trackedAllocations = 0;
void* Allocate(size_t size)
{
    if (failAllAllocations) {
        throw std::bad_alloc();
    }
    if ((trackedMinimumAllocationSize != 0 && size >= trackedMinimumAllocationSize) ||
        (trackedExactAllocationSize != 0 && size == trackedExactAllocationSize)) {
        ++trackedAllocations;
    }
    if (allocationFailureCountdown == 0) {
        allocationFailureCountdown = -1; // fail once so the diagnostic exception can allocate
        throw std::bad_alloc();
    }
    if (allocationFailureCountdown > 0) {
        --allocationFailureCountdown;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}
} // namespace
void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace npucheck {
namespace {
AclsanDeviceMemoryAccessData Access(
    AclsanDeviceMemoryAccessMode mode, uint64_t begin, uint64_t end, uint64_t instruction)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.header.instrExecId = instruction;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    data.accessMode = static_cast<uint32_t>(mode);
    data.address = begin;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = end - begin;
    return data;
}
std::string Snapshot(const MemStateTracker& tracker)
{
    std::ostringstream out;
    for (const auto& table : tracker.Tables()) {
        out << table.first.deviceId << ',' << table.first.memorySpace << ',' << table.first.phyCoreId << ':';
        for (const auto& entry : table.second) {
            const auto& segment = entry.second;
            out << segment.begin << '-' << segment.end << ':' << static_cast<int>(segment.state) << ':';
            if (segment.writer) {
                out << 'W' << *segment.writer;
            }
            for (size_t group = 0; group < segment.atomicWriters.size(); ++group) {
                for (auto index : segment.atomicWriters[group]) {
                    out << 'A' << group << ':' << index;
                }
            }
            for (auto index : segment.readers) {
                out << 'R' << index;
            }
        }
    }
    return out.str();
}

class UnorderedAccesses final : public RacecheckOrdering {
public:
    RacecheckOrder Compare(const AclsanDeviceMemoryAccessData&, const AclsanDeviceMemoryAccessData&) override
    {
        return RacecheckOrder::Unordered;
    }
    void OnDeviceSync(const AclsanDeviceSyncData&) override {}
    bool AnalysisComplete() const override { return true; }
};

struct FailureLog {
    char message[4096]{};
    size_t errors = 0;
    RacecheckDiagnostics Diagnostics()
    {
        return {
            [](void*, const char*) {}, this,
            [](void* context, const char* text) {
                auto& log = *static_cast<FailureLog*>(context);
                ++log.errors;
                std::snprintf(log.message, sizeof(log.message), "%s", text);
            }};
    }
};

TEST(RacecheckAllocationTest, EveryCallbackAllocationFailureStopsOnlyAffectedLaunch)
{
    bool reachedSuccess = false;
    bool failedDuringUpdate = false;
    bool failedDuringPreparation = false;
    for (int failurePoint = 0; failurePoint < 200; ++failurePoint) {
        UnorderedAccesses ordering;
        FailureLog log;
        Racecheck checker(&ordering, log.Diagnostics());
        CheckerReportList reports;
        auto write = Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 1);
        auto read = Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 32, 2);
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
        allocationFailureCountdown = failurePoint;
        const bool success =
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports);
        allocationFailureCountdown = -1;
        if (success) {
            reachedSuccess = true;
            break;
        }
        ASSERT_GE(log.errors, 1U) << "allocation " << failurePoint;
        const auto errorCount = log.errors;
        EXPECT_NE(std::string(log.message).find("reason=host_memory_allocation_failed"), std::string::npos);
        EXPECT_NE(std::string(log.message).find("state discarded"), std::string::npos);
        failedDuringUpdate |= std::string(log.message).find("stage=memory_state_update") != std::string::npos;
        failedDuringPreparation |= std::string(log.message).find("stage=access_preparation") != std::string::npos;
        ASSERT_TRUE(reports.empty());
        EXPECT_FALSE(checker.AnalysisComplete());
        EXPECT_FALSE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
        EXPECT_EQ(log.errors, errorCount);
        write.header.launchId = 2;
        EXPECT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    }
    EXPECT_TRUE(reachedSuccess);
    EXPECT_TRUE(failedDuringUpdate);
    EXPECT_TRUE(failedDuringPreparation);
}

TEST(RacecheckAllocationTest, PersistentAllocationFailureCanStillLogAndDiscardLaunch)
{
    UnorderedAccesses ordering;
    FailureLog log;
    Racecheck checker(&ordering, log.Diagnostics());
    CheckerReportList reports;
    auto write = Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 1);
    auto read = Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 32, 2);
    ASSERT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    failAllAllocations = true;
    const bool result =
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports);
    failAllAllocations = false;
    EXPECT_FALSE(result);
    EXPECT_EQ(log.errors, 1U);
    EXPECT_NE(std::string(log.message).find("insufficient host memory"), std::string::npos);
    EXPECT_NE(std::string(log.message).find("state discarded"), std::string::npos);
    EXPECT_FALSE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    write.header.launchId = 2;
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
}

TEST(RacecheckAllocationTest, FailureToAllocateStopMarkerStopsSession)
{
    UnorderedAccesses ordering;
    FailureLog log;
    Racecheck checker(&ordering, log.Diagnostics());
    CheckerReportList reports;
    auto write = Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 1);
    failAllAllocations = true;
    const bool result =
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports);
    failAllAllocations = false;
    EXPECT_FALSE(result);
    EXPECT_EQ(log.errors, 1U);
    EXPECT_NE(std::string(log.message).find("unable to retain launch stop marker"), std::string::npos);
    write.header.launchId = 2;
    EXPECT_FALSE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    EXPECT_EQ(log.errors, 1U);
    EXPECT_TRUE(reports.empty());
}

TEST(RacecheckAllocationTest, CurrentSparseLayoutIsExpandedOnceAcrossCheckReportsAndUpdate)
{
    constexpr size_t rangeCount = 128;
    for (bool withConflict : {false, true}) {
        SCOPED_TRACE(withConflict);
        UnorderedAccesses ordering;
        FailureLog log;
        Racecheck checker(&ordering, log.Diagnostics());
        CheckerReportList reports;
        if (withConflict) {
            auto write = Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 2048, 1);
            ASSERT_TRUE(checker.OnCallback(
                ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
        }
        auto read = Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 100, 104, 2);
        read.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
        read.layout.ndAffine = {};
        read.layout.ndAffine.elementBytes = 4;
        read.layout.ndAffine.rank = 1;
        read.layout.ndAffine.dims[0] = rangeCount;
        read.layout.ndAffine.strides[0] = 8;
        trackedAllocations = 0;
        trackedExactAllocationSize = rangeCount * sizeof(racecheck::ByteRange);
        const bool success =
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports);
        trackedExactAllocationSize = 0;
        EXPECT_TRUE(success);
        EXPECT_EQ(trackedAllocations, 1U);
        EXPECT_EQ(log.errors, 0U);
        ASSERT_EQ(reports.size(), withConflict ? rangeCount : 0U);
        if (withConflict) {
            for (size_t i = 0; i < rangeCount; ++i) {
                const auto& report = std::get<NpuCheckRacecheckReport>(reports[i]);
                EXPECT_EQ(report.currentRange.begin, 100 + i * 8);
                EXPECT_EQ(report.currentRange.end, 104 + i * 8);
                EXPECT_EQ(report.previousRange.begin, 0U);
                EXPECT_EQ(report.previousRange.end, 2048U);
                EXPECT_EQ(report.kind, ConflictKind::RAW);
            }
        }
    }
}

TEST(MemStateTrackerAllocationTest, PreparedQueriesWithoutCandidatesDoNotAllocate)
{
    for (auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        for (bool sparse : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "mode=" << mode << " sparse=" << sparse);
            std::vector<AclsanDeviceMemoryAccessData> records;
            MemStateTracker tracker{records};
            for (size_t i = 0; i < 3; ++i) {
                records.push_back(Access(mode, i * 8, i * 8 + 4, i + 1));
                tracker.UpdateMemState(i, {});
            }
            auto current = Access(mode, 2, 18, 4);
            if (sparse) {
                current.address = 16;
                current.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
                current.layout.ndAffine = {1, 2, {3, 0, 0, 0, 0}, {-8, 0, 0, 0, 0}};
            }
            records.push_back(current);
            const auto prepared = racecheck::PrepareAccess(records.back());
            const auto before = Snapshot(tracker);
            std::vector<AccessPair> candidates;
            trackedAllocations = 0;
            trackedMinimumAllocationSize = 1;
            try {
                candidates = tracker.CheckPotentialRace(3, prepared);
            } catch (...) {
                trackedMinimumAllocationSize = 0;
                throw;
            }
            trackedMinimumAllocationSize = 0;
            EXPECT_TRUE(candidates.empty());
            EXPECT_EQ(trackedAllocations, 0U);
            EXPECT_EQ(Snapshot(tracker), before);
        }
    }
}

TEST(MemStateTrackerAllocationTest, RepeatedConflictRangeOnlyAllocatesOneResult)
{
    constexpr size_t conflictCount = 16000;
    std::vector<AccessPair> conflicts;
    conflicts.reserve(conflictCount);
    for (size_t i = 0; i < conflictCount; ++i) {
        conflicts.push_back({i, conflictCount, ACLSAN_DEVICE_MEMORY_SPACE_GM, 0, 16});
    }
    std::vector<racecheck::FailedRange> ranges;
    trackedAllocations = 0;
    trackedMinimumAllocationSize = 1;
    try {
        ranges = racecheck::FailureRanges(conflicts);
    } catch (...) {
        trackedMinimumAllocationSize = 0;
        throw;
    }
    trackedMinimumAllocationSize = 0;
    EXPECT_EQ(trackedAllocations, 1U);
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_EQ(ranges[0].begin, 0U);
    EXPECT_EQ(ranges[0].end, 16U);
    EXPECT_EQ(ranges[0].evidence, 0U);
}

TEST(MemStateTrackerAllocationTest, ReadAndAtomicAppendReuseExistingHistoryStorage)
{
    for (auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        SCOPED_TRACE(mode);
        constexpr size_t historySize = 65; // 原地 vector 应保留此前几何扩容的剩余空间。
        std::vector<AclsanDeviceMemoryAccessData> records;
        records.reserve(historySize + 1);
        MemStateTracker tracker{records};
        for (size_t i = 0; i < historySize; ++i) {
            records.push_back(Access(mode, 0, 16, i + 1));
            tracker.UpdateMemState(i, {});
        }
        const auto& before = tracker.Tables().begin()->second.at(0);
        const auto& history = mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ?
                                  before.readers :
                                  before.atomicWriters[mode - ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD];
        const auto* storage = history.data();
        ASSERT_GT(history.capacity(), history.size());
        records.push_back(Access(mode, 0, 16, historySize + 1));
        trackedAllocations = 0;
        trackedMinimumAllocationSize = historySize * sizeof(CbIndex);
        try {
            tracker.UpdateMemState(historySize, {});
        } catch (...) {
            trackedMinimumAllocationSize = 0;
            throw;
        }
        trackedMinimumAllocationSize = 0;
        EXPECT_EQ(trackedAllocations, 0U);
        const auto& after = tracker.Tables().begin()->second.at(0);
        const auto& updated = mode == ACLSAN_DEVICE_MEMORY_ACCESS_READ ?
                                  after.readers :
                                  after.atomicWriters[mode - ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD];
        EXPECT_EQ(updated.data(), storage);
        EXPECT_EQ(updated.size(), historySize + 1);
        EXPECT_EQ(updated.back(), historySize);
    }
}

TEST(MemStateTrackerAllocationTest, FullOrdinaryOverwriteDoesNotAllocateDiscardedHistory)
{
    constexpr size_t historySize = 128;
    for (auto mode : {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD}) {
        SCOPED_TRACE(mode);
        std::vector<AclsanDeviceMemoryAccessData> records;
        records.reserve(historySize + 1);
        MemStateTracker tracker{records};
        for (size_t i = 0; i < historySize; ++i) {
            records.push_back(Access(mode, 0, 16, i + 1));
            tracker.UpdateMemState(i, {});
        }
        records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, historySize + 1));
        trackedAllocations = 0;
        trackedMinimumAllocationSize = historySize * sizeof(CbIndex);
        try {
            tracker.UpdateMemState(historySize, {});
        } catch (...) {
            trackedMinimumAllocationSize = 0;
            throw;
        }
        trackedMinimumAllocationSize = 0;
        EXPECT_EQ(trackedAllocations, 0U);
        const auto& segment = tracker.Tables().begin()->second.at(0);
        EXPECT_EQ(segment.state, MemOpState::Write);
        EXPECT_EQ(segment.writer, std::optional<CbIndex>{historySize});
        EXPECT_TRUE(segment.readers.empty());
        EXPECT_EQ(segment.readers.capacity(), 0U);
    }
}

TEST(MemStateTrackerAllocationTest, FullOrdinaryOverwriteDoesNotAllocateAcrossExistingSegments)
{
    constexpr size_t segmentCount = 64;
    for (auto mode : {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD}) {
        SCOPED_TRACE(mode);
        std::vector<AclsanDeviceMemoryAccessData> records;
        records.reserve(segmentCount + 2);
        MemStateTracker tracker{records};
        for (size_t i = 0; i < segmentCount; ++i) {
            records.push_back(Access(mode, i * 16, (i + 1) * 16, i + 1));
            tracker.UpdateMemState(i, {});
        }
        ASSERT_EQ(tracker.Tables().begin()->second.size(), segmentCount);
        records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, segmentCount * 16, segmentCount + 1));
        const auto prepared = racecheck::PrepareAccess(records.back());
        ASSERT_EQ(tracker.CheckPotentialRace(segmentCount, prepared).size(), segmentCount);
        // 外部判序已排除全部候选；覆盖已有节点无需申请新的存储。
        trackedAllocations = 0;
        trackedMinimumAllocationSize = 1;
        try {
            tracker.UpdateMemState(segmentCount, {}, prepared);
        } catch (...) {
            trackedMinimumAllocationSize = 0;
            throw;
        }
        trackedMinimumAllocationSize = 0;
        EXPECT_EQ(trackedAllocations, 0U);
        ASSERT_EQ(tracker.Tables().begin()->second.size(), 1U);
        records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, segmentCount * 16, segmentCount + 2));
        const auto candidates = tracker.CheckPotentialRace(segmentCount + 1);
        ASSERT_EQ(candidates.size(), 1U);
        EXPECT_EQ(candidates[0].prevIndex, segmentCount);
        EXPECT_EQ(candidates[0].begin, 0U);
        EXPECT_EQ(candidates[0].end, segmentCount * 16);
    }
}

TEST(MemStateTrackerAllocationTest, OrdinaryOverwriteFillsUntrackedGapsWithoutAllocating)
{
    constexpr size_t segmentCount = 64;
    for (auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD}) {
        SCOPED_TRACE(mode);
        std::vector<AclsanDeviceMemoryAccessData> records;
        records.reserve(segmentCount + 2);
        MemStateTracker tracker{records};
        for (size_t i = 0; i < segmentCount; ++i) {
            records.push_back(Access(mode, i * 32, i * 32 + 16, i + 1));
            tracker.UpdateMemState(i, {});
        }
        const auto* firstNode = &tracker.Tables().begin()->second.at(0);
        records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, segmentCount * 32, segmentCount + 1));
        const auto prepared = racecheck::PrepareAccess(records.back());
        ASSERT_EQ(tracker.CheckPotentialRace(segmentCount, prepared).size(), segmentCount);
        trackedAllocations = 0;
        trackedMinimumAllocationSize = 1;
        try {
            tracker.UpdateMemState(segmentCount, {}, prepared);
        } catch (...) {
            trackedMinimumAllocationSize = 0;
            throw;
        }
        trackedMinimumAllocationSize = 0;
        EXPECT_EQ(trackedAllocations, 0U);
        const auto& table = tracker.Tables().begin()->second;
        ASSERT_EQ(table.size(), 1U);
        const auto& segment = table.at(0);
        EXPECT_EQ(&segment, firstNode);
        EXPECT_EQ(segment.end, segmentCount * 32);
        EXPECT_EQ(segment.state, MemOpState::Write);
        EXPECT_EQ(segment.writer, std::optional<CbIndex>{segmentCount});
        EXPECT_TRUE(segment.readers.empty());
        for (const auto& group : segment.atomicWriters) {
            EXPECT_TRUE(group.empty());
        }
        records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, segmentCount * 32, segmentCount + 2));
        const auto candidates = tracker.CheckPotentialRace(segmentCount + 1);
        ASSERT_EQ(candidates.size(), 1U);
        EXPECT_EQ(candidates[0].prevIndex, segmentCount);
        EXPECT_EQ(candidates[0].begin, 0U);
        EXPECT_EQ(candidates[0].end, segmentCount * 32);
    }
}

TEST(MemStateTrackerAllocationTest, OrdinaryOverwriteReusesExistingNodeWithoutAllocating)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 1), Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 2)};
    MemStateTracker tracker{records};
    tracker.UpdateMemState(0, {});
    const auto* node = &tracker.Tables().begin()->second.at(0);
    const auto prepared = racecheck::PrepareAccess(records[1]);
    trackedAllocations = 0;
    trackedMinimumAllocationSize = 1;
    try {
        tracker.UpdateMemState(1, {}, prepared);
    } // 同步判序已排除 WAW。
    catch (...) {
        trackedMinimumAllocationSize = 0;
        throw;
    }
    trackedMinimumAllocationSize = 0;
    EXPECT_EQ(trackedAllocations, 0U);
    const auto& updated = tracker.Tables().begin()->second.at(0);
    EXPECT_EQ(&updated, node);
    EXPECT_EQ(updated.writer, std::optional<CbIndex>{1});
}

TEST(MemStateTrackerAllocationTest, OrdinaryOverwriteSkipsFrozenRaceWithoutCopyingHistory)
{
    constexpr size_t historySize = 128;
    std::vector<AclsanDeviceMemoryAccessData> records;
    records.reserve(historySize + 2);
    MemStateTracker tracker{records};
    for (size_t i = 0; i < historySize; ++i) {
        records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, i + 1));
        tracker.UpdateMemState(i, {});
    }
    records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 32, historySize + 1));
    tracker.UpdateMemState(historySize, tracker.CheckPotentialRace(historySize));
    const auto* node = &tracker.Tables().begin()->second.at(0);
    const auto* history = node->readers.data();
    records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 4, 28, historySize + 2));
    ASSERT_TRUE(tracker.CheckPotentialRace(historySize + 1).empty());
    trackedAllocations = 0;
    trackedMinimumAllocationSize = historySize * sizeof(CbIndex);
    try {
        tracker.UpdateMemState(historySize + 1, {});
    } catch (...) {
        trackedMinimumAllocationSize = 0;
        throw;
    }
    trackedMinimumAllocationSize = 0;
    EXPECT_EQ(trackedAllocations, 0U);
    const auto& table = tracker.Tables().begin()->second;
    ASSERT_EQ(table.size(), 1U);
    const auto& frozen = table.at(0);
    EXPECT_EQ(&frozen, node);
    EXPECT_EQ(frozen.readers.data(), history);
    EXPECT_EQ(frozen.end, 32U);
    EXPECT_EQ(frozen.state, MemOpState::Race);
    EXPECT_EQ(frozen.readers.size(), historySize);
}

TEST(MemStateTrackerAllocationTest, PartialOverwriteCopiesOnlyRetainedHistory)
{
    constexpr size_t historySize = 128;
    std::vector<AclsanDeviceMemoryAccessData> records;
    records.reserve(historySize + 1);
    MemStateTracker tracker{records};
    for (size_t i = 0; i < historySize; ++i) {
        records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, i + 1));
        tracker.UpdateMemState(i, {});
    }
    records.push_back(Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 4, 28, historySize + 1));
    trackedAllocations = 0;
    trackedMinimumAllocationSize = historySize * sizeof(CbIndex);
    try {
        tracker.UpdateMemState(historySize, {});
    } catch (...) {
        trackedMinimumAllocationSize = 0;
        throw;
    }
    trackedMinimumAllocationSize = 0;
    EXPECT_LE(trackedAllocations, 1U); // 左右两份历史只需新增一份存储。
    const auto& table = tracker.Tables().begin()->second;
    ASSERT_EQ(table.size(), 3U);
    EXPECT_EQ(table.at(0).readers.size(), historySize);
    EXPECT_EQ(table.at(28).readers, table.at(0).readers);
    EXPECT_EQ(table.at(4).writer, std::optional<CbIndex>{historySize});
    EXPECT_EQ(table.at(4).readers.capacity(), 0U);
}

TEST(MemStateTrackerAllocationTest, EveryOrdinaryOverwriteAllocationFailureDiscardsAndStopsTracker)
{
    for (bool racing : {false, true}) {
        SCOPED_TRACE(racing);
        bool reachedSuccess = false;
        for (int failurePoint = 0; failurePoint < 200; ++failurePoint) {
            std::vector<AclsanDeviceMemoryAccessData> records{
                Access(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 0, 32, 1),
                Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 32, 2),
                Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 4, 28, 3)};
            MemStateTracker tracker{records};
            tracker.UpdateMemState(0, {});
            tracker.UpdateMemState(1, {});
            auto conflicts = tracker.CheckPotentialRace(2);
            ASSERT_EQ(conflicts.size(), 1U);
            conflicts[0].begin = 12;
            conflicts[0].end = 20;
            if (!racing) {
                conflicts.clear();
            }
            bool failed = false;
            bool wrongException = false;
            allocationFailureCountdown = failurePoint;
            try {
                tracker.UpdateMemState(2, conflicts);
            } catch (const RacecheckError& error) {
                failed = true;
                wrongException = error.Kind() != RacecheckErrorKind::MemoryAllocationFailed;
            } catch (...) {
                failed = true;
                wrongException = true;
            }
            allocationFailureCountdown = -1;
            ASSERT_FALSE(wrongException) << "allocation " << failurePoint;
            if (failed) {
                ASSERT_TRUE(tracker.Tables().empty()) << "allocation " << failurePoint;
                EXPECT_THROW(tracker.CheckPotentialRace(2), RacecheckError);
                EXPECT_THROW(tracker.UpdateMemState(2, conflicts), RacecheckError);
                continue;
            }
            const auto& table = tracker.Tables().begin()->second;
            EXPECT_EQ(table.at(0).readers, std::vector<CbIndex>{1});
            EXPECT_EQ(table.at(28).atomicWriters[0], std::vector<CbIndex>{0});
            EXPECT_EQ(table.at(28).readers, std::vector<CbIndex>{1});
            EXPECT_EQ(table.at(4).writer, std::optional<CbIndex>{2});
            EXPECT_EQ(table.at(4).readers.capacity(), 0U);
            if (racing) {
                EXPECT_EQ(table.at(12).state, MemOpState::Race);
                EXPECT_EQ(table.at(12).atomicWriters[0], std::vector<CbIndex>{0});
                EXPECT_EQ(table.at(12).readers, std::vector<CbIndex>{1});
            }
            if (!failed) {
                reachedSuccess = true;
                break;
            }
        }
        ASSERT_TRUE(reachedSuccess);
    }
}

TEST(MemStateTrackerAllocationTest, AllocationFailureAfterBatchedEraseDiscardsAndStopsTracker)
{
    std::vector<AclsanDeviceMemoryAccessData> records{
        Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4, 1),
        Access(ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, 4, 8, 2),
        Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 8, 3)};
    FailureLog log;
    MemStateTracker tracker{records, log.Diagnostics()};
    tracker.UpdateMemState(0, {});
    tracker.UpdateMemState(1, {});
    records[2].layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    records[2].layout.blockRepeat = {2, 8, 16, 1, 0, 0};
    const auto prepared = racecheck::PrepareAccess(records[2]);
    // 第一块复用节点并删除第二个旧节点；第二块补建节点时才发生分配失败。
    allocationFailureCountdown = 0;
    bool allocationFailed = false;
    try {
        tracker.UpdateMemState(2, {}, prepared);
    } catch (const RacecheckError& error) {
        allocationFailed = error.Kind() == RacecheckErrorKind::MemoryAllocationFailed;
    } catch (...) {
        allocationFailureCountdown = -1;
        throw;
    }
    allocationFailureCountdown = -1;
    ASSERT_TRUE(allocationFailed);
    EXPECT_TRUE(tracker.Tables().empty());
    EXPECT_NE(std::string(log.message).find("state discarded; tracker stopped"), std::string::npos);
    EXPECT_THROW(tracker.CheckPotentialRace(2), RacecheckError);
    EXPECT_THROW(tracker.UpdateMemState(2, {}), RacecheckError);
}

TEST(MemStateTrackerAllocationTest, EveryReadAndAtomicUpdateAllocationFailureDiscardsAndStopsTracker)
{
    for (auto mode :
         {ACLSAN_DEVICE_MEMORY_ACCESS_READ, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD,
          ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}) {
        SCOPED_TRACE(mode);
        for (bool existingTable : {false, true}) {
            SCOPED_TRACE(existingTable);
            bool reachedSuccess = false;
            for (int failurePoint = 0; failurePoint < 200; ++failurePoint) {
                std::vector<AclsanDeviceMemoryAccessData> records{
                    Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 1), Access(mode, 4, 32, 2)};
                FailureLog log;
                MemStateTracker tracker{records, log.Diagnostics()};
                if (existingTable) {
                    tracker.UpdateMemState(0, {});
                }
                const auto pairs = tracker.CheckPotentialRace(1);
                bool failed = false;
                bool wrongException = false;
                allocationFailureCountdown = failurePoint;
                try {
                    tracker.UpdateMemState(1, pairs);
                } catch (const RacecheckError& error) {
                    failed = true;
                    wrongException = error.Kind() != RacecheckErrorKind::MemoryAllocationFailed;
                } catch (...) {
                    failed = true;
                    wrongException = true;
                }
                allocationFailureCountdown = -1;
                ASSERT_FALSE(wrongException) << "allocation " << failurePoint;
                if (!failed) {
                    reachedSuccess = true;
                    break;
                }
                ASSERT_TRUE(tracker.Tables().empty()) << "allocation " << failurePoint;
                EXPECT_NE(std::string(log.message).find("state discarded; tracker stopped"), std::string::npos);
                EXPECT_THROW(tracker.CheckPotentialRace(1), RacecheckError);
                EXPECT_THROW(tracker.UpdateMemState(1, pairs), RacecheckError);
            }
            ASSERT_TRUE(reachedSuccess);
        }
    }
}

TEST(MemStateTrackerAllocationTest, EveryCandidateAllocationFailureRaisesAndLeavesTableUnchanged)
{
    bool reachedSuccess = false;
    for (int failurePoint = 0; failurePoint < 100; ++failurePoint) {
        std::vector<AclsanDeviceMemoryAccessData> records{
            Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 16, 1), Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 32, 2)};
        MemStateTracker tracker{records};
        tracker.UpdateMemState(0, {});
        const auto before = Snapshot(tracker);
        bool failed = false;
        bool wrongException = false;
        allocationFailureCountdown = failurePoint;
        try {
            tracker.CheckPotentialRace(1);
        } catch (const RacecheckError& error) {
            failed = true;
            wrongException = error.Kind() != RacecheckErrorKind::MemoryAllocationFailed;
        } catch (...) {
            failed = true;
            wrongException = true;
        }
        allocationFailureCountdown = -1;
        ASSERT_FALSE(wrongException);
        ASSERT_EQ(Snapshot(tracker), before);
        if (!failed) {
            reachedSuccess = true;
            break;
        }
    }
    ASSERT_TRUE(reachedSuccess);
}
TEST(RacecheckAllocationTest, PublishedReportsSurviveFailedInPlaceUpdate)
{
    bool reachedSuccess = false;
    for (int failurePoint = 0; failurePoint < 300; ++failurePoint) {
        UnorderedAccesses ordering;
        FailureLog log;
        Racecheck checker(&ordering, log.Diagnostics());
        CheckerReportList reports;
        auto write = Access(ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, 8, 1);
        auto first = Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, 4, 2);
        auto second = Access(ACLSAN_DEVICE_MEMORY_ACCESS_READ, 4, 16, 3);
        auto dispatch = [&](const auto& data) {
            return checker.OnCallback(
                ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &data, reports);
        };
        ASSERT_TRUE(dispatch(write));
        ASSERT_TRUE(dispatch(first));
        ASSERT_EQ(reports.size(), 1U);
        allocationFailureCountdown = failurePoint;
        const bool success = dispatch(second);
        allocationFailureCountdown = -1;
        if (success) {
            reachedSuccess = true;
            ASSERT_EQ(reports.size(), 2U);
            EXPECT_EQ(std::get<NpuCheckRacecheckReport>(reports[1]).reportId, 2U);
            break;
        }
        ASSERT_EQ(reports.size(), 1U) << "allocation " << failurePoint;
        const auto& previous = std::get<NpuCheckRacecheckReport>(reports[0]);
        EXPECT_EQ(previous.checkRange.begin, 0U);
        EXPECT_EQ(previous.checkRange.end, 4U);
        EXPECT_EQ(previous.current.header.instrExecId, 2U);
        EXPECT_FALSE(dispatch(second));
        EXPECT_FALSE(checker.AnalysisComplete());
        write.header.launchId = 2;
        first.header.launchId = 2;
        ASSERT_TRUE(dispatch(write));
        ASSERT_TRUE(dispatch(first));
        ASSERT_EQ(reports.size(), 2U);
        EXPECT_EQ(std::get<NpuCheckRacecheckReport>(reports[1]).reportId, 2U);
    }
    ASSERT_TRUE(reachedSuccess);
}

} // namespace
} // namespace npucheck
