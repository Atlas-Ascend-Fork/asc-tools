// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "checker/initcheck.h"
#include "diagnostic/report_renderer.h"
#include <gtest/gtest.h>

namespace npucheck {
namespace {
const GmScope scope{2, 17};
constexpr uint64_t base = 0x100000;

AclsanDeviceMemoryAccessData Access(uint64_t address, uint64_t bytes, uint32_t mode)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.deviceId = scope.deviceId;
    data.header.launchId = 11;
    data.header.instrExecId = 7;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    data.accessMode = mode;
    data.accessCount = 1;
    data.address = address;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = bytes;
    return data;
}

TEST(GmInitcheckTest, RendersPartialReadWithDistinctAllocationAndReadSizes)
{
    GmInitcheck checker;
    ASSERT_EQ(checker.RegisterAllocation(scope, 9, base, 256), AllocationUpdateStatus::OK);
    ASSERT_EQ(checker.MarkInitialized(scope, base, 64), GmCheckStatus::OK);
    ASSERT_EQ(checker.MarkInitialized(scope, base + 192, 64), GmCheckStatus::OK);
    auto access = Access(base, 128, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    access.header.pc = 0x123456789abc;
    auto result = checker.CheckInstruction(scope, {access});
    ASSERT_EQ(result.status, GmCheckStatus::OK);
    ASSERT_EQ(result.reports.size(), 1U);
    const auto& report = result.reports[0];
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ);
    EXPECT_EQ(report.firstUninitAddress, base + 64);
    EXPECT_EQ(report.uninitBytes, 64U);
    EXPECT_EQ(report.initializedBytes, 64U);
    EXPECT_EQ(report.allocationInitializedBytes, 128U);
    EXPECT_EQ(report.access.accessBytes, 128U);
    EXPECT_EQ(report.allocation.allocId, 9U);
    std::string text;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(result.reports[0]), {}, &text),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("Partially uninitialized GM memory read\n"), std::string::npos);
    EXPECT_NE(text.find("Read size: 128 bytes; uninitialized: 64 bytes"), std::string::npos);
    EXPECT_NE(text.find("Access start: 0x100000; first uninitialized byte: 0x100040"), std::string::npos);
    EXPECT_NE(
        text.find("Allocation base: 0x100000; initialized bytes: 128; allocation instance: 1"), std::string::npos);
    EXPECT_NE(text.find("device 2"), std::string::npos);
    EXPECT_NE(text.find("pc=0x123456789abc"), std::string::npos);
}

TEST(GmInitcheckTest, ReadingDoesNotInitializeAndLaterWriteDoes)
{
    GmInitcheck checker;
    ASSERT_EQ(checker.RegisterAllocation(scope, 1, base, 256), AllocationUpdateStatus::OK);
    auto read = Access(base, 256, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    for (int i = 0; i < 2; ++i) {
        auto result = checker.CheckInstruction(scope, {read});
        ASSERT_EQ(result.reports.size(), 1U);
        EXPECT_EQ(result.reports[0].common.pattern, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ);
        EXPECT_EQ(result.reports[0].uninitBytes, 256U);
    }
    EXPECT_EQ(checker.MarkInitialized(scope, base, 256), GmCheckStatus::OK);
    EXPECT_TRUE(checker.CheckInstruction(scope, {read}).reports.empty());
}

TEST(GmInitcheckTest, CrossesLeafAndMiddleTableBoundaries)
{
    for (uint64_t boundary : {UINT64_C(1) << 16, UINT64_C(1) << 32}) {
        GmInitcheck checker;
        ASSERT_EQ(checker.RegisterAllocation(scope, 1, boundary - 32, 96), AllocationUpdateStatus::OK);
        ASSERT_EQ(checker.MarkInitialized(scope, boundary - 16, 32), GmCheckStatus::OK);
        auto result = checker.CheckInstruction(scope, {Access(boundary - 32, 96, ACLSAN_DEVICE_MEMORY_ACCESS_READ)});
        ASSERT_EQ(result.reports.size(), 1U);
        EXPECT_EQ(result.reports[0].uninitBytes, 64U);
        EXPECT_EQ(result.reports[0].firstUninitAddress, boundary - 32);
    }
}

TEST(GmInitcheckTest, FreeAndReuseDoesNotInheritStateOrClearNeighbor)
{
    GmInitcheck checker;
    ASSERT_EQ(checker.RegisterAllocation(scope, 1, base, 64), AllocationUpdateStatus::OK);
    ASSERT_EQ(checker.RegisterAllocation(scope, 2, base + 64, 64), AllocationUpdateStatus::OK);
    ASSERT_EQ(checker.MarkInitialized(scope, base, 64), GmCheckStatus::OK);
    ASSERT_EQ(checker.MarkInitialized(scope, base + 64, 64), GmCheckStatus::OK);
    ASSERT_EQ(checker.ReleaseAllocation(scope, 1, base), AllocationUpdateStatus::OK);
    ASSERT_EQ(checker.RegisterAllocation(scope, 3, base, 64), AllocationUpdateStatus::OK);
    EXPECT_EQ(checker.CheckInstruction(scope, {Access(base, 64, ACLSAN_DEVICE_MEMORY_ACCESS_READ)}).reports.size(), 1U);
    EXPECT_TRUE(
        checker.CheckInstruction(scope, {Access(base + 64, 64, ACLSAN_DEVICE_MEMORY_ACCESS_READ)}).reports.empty());
}

TEST(GmInitcheckTest, DeviceAndContextAreIsolatedWhileLaunchesShareState)
{
    GmInitcheck checker;
    const GmScope otherContext{2, 18};
    const GmScope otherDevice{3, 17};
    for (auto s : {scope, otherContext, otherDevice}) {
        ASSERT_EQ(checker.RegisterAllocation(s, 1, base, 64), AllocationUpdateStatus::OK);
    }
    ASSERT_EQ(checker.MarkInitialized(scope, base, 64), GmCheckStatus::OK);
    auto read = Access(base, 64, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    read.header.launchId = 99;
    EXPECT_TRUE(checker.CheckInstruction(scope, {read}).reports.empty());
    EXPECT_EQ(checker.CheckInstruction(otherContext, {read}).reports.size(), 1U);
    read.header.deviceId = 3;
    EXPECT_EQ(checker.CheckInstruction(otherDevice, {read}).reports.size(), 1U);
}

TEST(GmInitcheckTest, AllReadOperandsAreCheckedBeforeAnyWritesWithoutTaintPropagation)
{
    GmInitcheck checker;
    ASSERT_EQ(checker.RegisterAllocation(scope, 1, base, 128), AllocationUpdateStatus::OK);
    auto write = Access(base, 64, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    auto read = Access(base, 64, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    write.accessCount = read.accessCount = 2;
    read.accessIndex = 1;
    auto result = checker.CheckInstruction(scope, {write, read});
    ASSERT_EQ(result.status, GmCheckStatus::OK);
    ASSERT_EQ(result.reports.size(), 1U);
    read.accessCount = 1;
    read.accessIndex = 0;
    EXPECT_TRUE(checker.CheckInstruction(scope, {read}).reports.empty());
    auto rmw = Access(base + 64, 64, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    EXPECT_EQ(checker.CheckInstruction(scope, {rmw}).reports.size(), 1U);
    EXPECT_TRUE(checker.CheckInstruction(scope, {rmw}).reports.empty());
}

TEST(GmInitcheckTest, StridedAccessDoesNotWriteOrReadHoles)
{
    GmInitcheck checker;
    ASSERT_EQ(checker.RegisterAllocation(scope, 1, base, 128), AllocationUpdateStatus::OK);
    auto access = Access(base, 0, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    access.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    access.layout.blockRepeat = {2, 16, 64, 1, 0, 0};
    EXPECT_EQ(checker.CheckInstruction(scope, {access}).status, GmCheckStatus::OK);
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    EXPECT_TRUE(checker.CheckInstruction(scope, {access}).reports.empty());
    auto result = checker.CheckInstruction(scope, {Access(base, 128, ACLSAN_DEVICE_MEMORY_ACCESS_READ)});
    ASSERT_EQ(result.reports.size(), 1U);
    EXPECT_EQ(result.reports[0].uninitBytes, 96U);
    EXPECT_EQ(result.reports[0].firstUninitAddress, base + 16);
}

TEST(GmInitcheckTest, UnknownOutOfBoundsAndIncompleteRecordsAreNotUninitializedErrors)
{
    GmInitcheck checker;
    auto read = Access(base, 128, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    auto result = checker.CheckInstruction(scope, {read});
    EXPECT_EQ(result.status, GmCheckStatus::INVALID_ALLOCATION);
    EXPECT_TRUE(result.reports.empty());
    ASSERT_EQ(checker.RegisterAllocation(scope, 1, base, 64), AllocationUpdateStatus::OK);
    EXPECT_EQ(checker.CheckInstruction(scope, {read}).status, GmCheckStatus::INVALID_ALLOCATION);
    read.layout.range.bytes = 64;
    read.header.flags = ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR;
    result = checker.CheckInstruction(scope, {read});
    EXPECT_EQ(result.status, GmCheckStatus::INCOMPLETE_RECORD);
    EXPECT_TRUE(result.reports.empty());
}

TEST(GmInitStateTest, AllocationQueryCountsFullPartialAndAbsentPages)
{
    GmInitState state;
    constexpr auto page = GmInitState::kPageBytes;
    state.Write(base, page, 0);
    state.Write(base + page, 64, 0);
    state.Write(base + 2 * page + 32, 32, 0);
    const auto result = state.Query(base, 4 * page);
    EXPECT_EQ(result.firstUninitializedAddress, base + page + 64);
    EXPECT_EQ(result.uninitializedBytes, 3 * page - 96);
    EXPECT_EQ(state.Query(base + 17, 32).uninitializedBytes, 0U);
    state.Clear(base + page, 64);
    EXPECT_EQ(state.Query(base, 4 * page).firstUninitializedAddress, base + page);
    EXPECT_EQ(state.Query(base, 4 * page).uninitializedBytes, 3 * page - 32);
}

TEST(GmInitStateTest, SparseReadsDoNotAllocateAndClearReclaimsPages)
{
    GmInitState state;
    EXPECT_EQ(state.Query(0x123456789, 128).uninitializedBytes, 128U);
    EXPECT_EQ(state.LeafCount(), 0U);
    state.Write(0xffff, 2, 4);
    EXPECT_EQ(state.LeafCount(), 2U);
    EXPECT_EQ(state.Query(0xffff, 2).uninitializedBytes, 0U);
    state.Clear(0xffff, 2);
    EXPECT_EQ(state.LeafCount(), 0U);
    EXPECT_EQ(state.Query(0xffff, 2).uninitializedBytes, 2U);
}

} // namespace
} // namespace npucheck
