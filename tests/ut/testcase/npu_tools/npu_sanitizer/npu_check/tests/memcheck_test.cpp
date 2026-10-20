// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "acl_san/aclsan_api.h"
#include "checker/memcheck.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace npucheck {
namespace {

using npucheck::NpuCheckReportAccessMode;
using npucheck::NpuCheckReportDistanceKind;
using npucheck::NpuCheckReportPattern;
using npucheck::ReportRenderStatus;

template <typename T>
void InitCommon(T& data, uint64_t correlation = 0, int result = 0)
{
    data = {};
    data.common.version = ACLSAN_API_VERSION;
    data.common.size = sizeof(T);
    data.common.correlationId = correlation;
    data.common.result = result;
}

AclsanResourceData AllocationEvent(uint64_t base, uint64_t bytes, uint64_t id, uint32_t deviceId = 0)
{
    AclsanResourceData data{};
    InitCommon(data);
    data.ptr = reinterpret_cast<void*>(base);
    data.bytes = bytes;
    data.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    data.deviceId = deviceId;
    data.resourceId = id;
    return data;
}

AclsanDeviceMemoryAccessData Access(
    DeviceSourceKind sourceKind, uint64_t address, uint64_t bytes, uint32_t deviceId = 0)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.pipeline = sourceKind == DeviceSourceKind::MTE2 ? ACLSAN_DEVICE_PIPE_MTE2 : ACLSAN_DEVICE_PIPE_MTE3;
    data.header.sourceKind = static_cast<uint32_t>(sourceKind);
    data.header.siteId = 7;
    data.header.blockId = 3;
    data.header.deviceId = deviceId;
    data.header.pc = 0x170;
    data.address = address;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    data.accessMode =
        sourceKind == DeviceSourceKind::MTE2 ? ACLSAN_DEVICE_MEMORY_ACCESS_READ : ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    data.accessCount = 1;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = bytes;
    return data;
}

AclsanDeviceMemoryAccessData ScalarAccess(uint32_t instrId, uint64_t address, uint64_t bytes, uint32_t deviceId = 0)
{
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    data.header.sourceKind = instrId < 68 ? ACLSAN_DEVICE_SOURCE_ST : ACLSAN_DEVICE_SOURCE_LD;
    data.header.siteId = 7;
    data.header.blockId = 3;
    data.header.deviceId = deviceId;
    data.header.pc = 0x170;
    data.address = address;
    data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    // 下面的合成原子夹具显式提供已知操作。
    data.accessMode = instrId < 68 ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ;
    data.accessCount = 1;
    data.dataBits = static_cast<uint32_t>(bytes * 8U);
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = bytes;
    return data;
}

TEST(MemcheckTest, CubeLocalBoundsUseDav3510CapacityAndPreserveContext)
{
    const std::array<std::pair<uint32_t, uint64_t>, 4> spaces{
        {{ACLSAN_DEVICE_MEMORY_SPACE_L1, 512 * 1024},
         {ACLSAN_DEVICE_MEMORY_SPACE_L0A, 64 * 1024},
         {ACLSAN_DEVICE_MEMORY_SPACE_L0B, 64 * 1024},
         {ACLSAN_DEVICE_MEMORY_SPACE_L0C, 256 * 1024}}};
    for (const auto& [space, capacity] : spaces) {
        SCOPED_TRACE(space);
        Memcheck checker;
        auto access = Access(DeviceSourceKind::MTE2, capacity - 32, 32);
        access.memorySpace = space;
        access.header.launchId = 42;
        checker.QueueDeviceMemoryAccess(access);
        EXPECT_TRUE(checker.OnSynchronization().empty());
        ++access.address;
        checker.QueueDeviceMemoryAccess(access);
        const auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(reports[0].allocation.base, 0U);
        EXPECT_EQ(reports[0].allocation.bytes, capacity);
        EXPECT_EQ(reports[0].common.exec.launchId, 42U);
        EXPECT_EQ(reports[0].common.exec.siteId, 7U);
        EXPECT_EQ(reports[0].common.exec.pc, 0x170U);
        EXPECT_EQ(reports[0].distanceBytes, 1);
        std::string rendered;
        ASSERT_EQ(
            RenderNpuCheckReportRecord(NpuCheckReportRecord::From(reports[0]), {}, &rendered),
            ReportRenderStatus::SUCCESS);
        EXPECT_NE(rendered.find("capacity " + std::to_string(capacity) + " bytes"), std::string::npos);
        EXPECT_TRUE(checker.AnalysisComplete());
    }
}

TEST(MemcheckTest, CubeLocalBoundsDoNotConsultGlobalAllocationsAndDetectOverflow)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(65536, 128, 1));
    for (uint64_t address : {uint64_t{65536}, std::numeric_limits<uint64_t>::max() - 3}) {
        auto access = Access(DeviceSourceKind::MTE2, address, 16);
        access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L0A;
        checker.QueueDeviceMemoryAccess(access);
    }
    EXPECT_EQ(checker.OnSynchronization().size(), 2U);
}

TEST(MemcheckTest, CubeLocalBoundsDoNotRequireMemoryFlags)
{
    Memcheck checker;
    auto access = Access(DeviceSourceKind::MTE2, 65536, 16);
    access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L0A;
    EXPECT_EQ(access.memoryFlags, 0U);
    checker.QueueDeviceMemoryAccess(access);
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].access.memorySpace, NpuCheckReportMemorySpace::L0_A);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(MemcheckTest, CubeAffineBoundsVisitStridedSegments)
{
    Memcheck checker;
    auto access = Access(DeviceSourceKind::MTE2, 65536 - 64, 0);
    access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L0A;
    access.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    access.layout.ndAffine.rank = 1;
    access.layout.ndAffine.elementBytes = 16;
    access.layout.ndAffine.dims[0] = 2;
    access.layout.ndAffine.strides[0] = 32;
    checker.QueueDeviceMemoryAccess(access);
    EXPECT_TRUE(checker.OnSynchronization().empty());
    access.layout.ndAffine.dims[0] = 3;
    checker.QueueDeviceMemoryAccess(access);
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].access.address, 65536U);
}

TEST(MemcheckTest, ScalarDevAccessChecksWholeAllocationRangeAndUseAfterFree)
{
    for (uint32_t id = 64; id <= 71; ++id) {
        SCOPED_TRACE(id);
        const uint64_t bytes = 8U >> ((id - 64) % 4);
        Memcheck checker;
        const auto allocation = AllocationEvent(0x1000, 32, 1);
        checker.OnAllocation(allocation);
        for (bool invalid : {false, true}) {
            const uint64_t address = 0x1020 - bytes + (invalid ? 1U : 0U);
            const auto access = ScalarAccess(id, address, bytes);
            checker.QueueDeviceMemoryAccess(access);
            const auto reports = checker.OnSynchronization();
            if (!invalid) {
                EXPECT_TRUE(reports.empty());
            } else {
                ASSERT_EQ(reports.size(), 1U);
                EXPECT_EQ(reports.front().common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
                EXPECT_EQ(
                    reports.front().access.accessMode,
                    id < 68 ? NpuCheckReportAccessMode::WRITE : NpuCheckReportAccessMode::READ);
            }
            if (!invalid) {
                checker.OnFree(allocation);
                checker.QueueDeviceMemoryAccess(access);
                const auto freedReports = checker.OnSynchronization();
                ASSERT_EQ(freedReports.size(), 1U);
                EXPECT_EQ(freedReports.front().common.pattern, NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE);
                checker.OnAllocation(allocation);
            }
        }
    }
}

TEST(MemcheckTest, ScalarAtomicAccessChecksEveryWidthOnce)
{
    for (uint32_t id = 56; id <= 61; ++id) {
        SCOPED_TRACE(id);
        const uint64_t bytes = (32U >> ((id - 56) % 3)) / 8U;
        Memcheck checker;
        const auto allocation = AllocationEvent(0x1000, 32, 1);
        checker.OnAllocation(allocation);
        for (bool invalid : {false, true}) {
            const uint64_t address = invalid ? 0x1020 : 0x1020 - bytes;
            auto access = ScalarAccess(id, address, bytes);
            access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
            checker.QueueDeviceMemoryAccess(access);
            const auto reports = checker.OnSynchronization();
            if (!invalid) {
                EXPECT_TRUE(reports.empty());
            } else {
                ASSERT_EQ(reports.size(), 1U);
                EXPECT_EQ(reports.front().common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
                EXPECT_EQ(reports[0].access.accessMode, NpuCheckReportAccessMode::ATOMIC_WRITE_ADD);
            }
        }
        EXPECT_EQ(checker.Stats().errors, 1U);
        checker.OnFree(allocation);
        auto access = ScalarAccess(id, 0x1000, bytes);
        access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
        checker.QueueDeviceMemoryAccess(access);
        const auto freedReports = checker.OnSynchronization();
        ASSERT_EQ(freedReports.size(), 1U);
        EXPECT_EQ(freedReports[0].common.pattern, NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE);
        EXPECT_EQ(freedReports[0].access.accessMode, NpuCheckReportAccessMode::ATOMIC_WRITE_ADD);
        EXPECT_EQ(checker.Stats().errors, 2U);
    }
}

TEST(MemcheckTest, ReportsOutOfBoundsReadAtSynchronization)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x100000, 4096, 1));
    const auto validRead = Access(DeviceSourceKind::MTE2, 0x100100, 64);
    auto invalidRead = Access(DeviceSourceKind::MTE2, 0x100ff0, 64);
    invalidRead.header.sourceKind = ACLSAN_DEVICE_SOURCE_UNKNOWN;
    invalidRead.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    invalidRead.header.phyCoreId = 75;
    invalidRead.header.blockId = 7;
    invalidRead.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    checker.QueueDeviceMemoryAccess(validRead);
    checker.QueueDeviceMemoryAccess(invalidRead);

    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 2U);
    const auto reports = checker.OnSynchronization();

    ASSERT_EQ(reports.size(), 1U);
    const auto& report = reports.front();
    EXPECT_EQ(report.common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
    EXPECT_EQ(report.access.accessMode, NpuCheckReportAccessMode::READ);
    EXPECT_EQ(report.common.exec.pc, invalidRead.header.pc);
    EXPECT_TRUE(report.common.exec.file.empty());
    EXPECT_EQ(report.common.exec.line, 0U);
    EXPECT_EQ(report.common.stackCount, 0U);
    EXPECT_EQ(report.nearestAllocation.base, 0x100000U);
    EXPECT_EQ(report.nearestAllocation.bytes, 4096U);
    EXPECT_EQ(report.distanceKind, NpuCheckReportDistanceKind::AFTER);
    EXPECT_EQ(report.distanceBytes, 48);

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(npucheck::NpuCheckReportRecord::From(report), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("Invalid GM read of size 64 bytes"), std::string::npos);
    EXPECT_NE(rendered.find("by aicore (75) type (AIC) block (7) pipe (PIPE_MTE2)"), std::string::npos);
    EXPECT_NE(rendered.find("Address 0x100ff0 is out of bounds"), std::string::npos);
    // The public template omits allocation-distance details; structured fields are checked above.
    EXPECT_EQ(rendered.find("nearest allocation"), std::string::npos);
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
    EXPECT_EQ(checker.Stats().errors, 1U);
    EXPECT_TRUE(checker.OnSynchronization().empty());
}

TEST(MemcheckTest, SkipsEstimatedAccessWithoutInterpretingItsLayout)
{
    Memcheck checker;
    auto estimated = Access(DeviceSourceKind::MTE2, 0, 0);
    estimated.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    estimated.layoutKind = 0;
    estimated.regDependencyMask0 = UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE;

    checker.QueueDeviceMemoryAccess(estimated);

    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().deviceOperations, 0U);
    EXPECT_EQ(checker.Stats().inexactDeviceOperations, 1U);
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
}

TEST(MemcheckTest, FailedLaunchOrSynchronizationMakesAnalysisIncomplete)
{
    CheckerReportList reports;
    Memcheck launchChecker;
    AclsanLaunchData launch{};
    InitCommon(launch, 0, 500001);
    ASSERT_TRUE(launchChecker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    EXPECT_FALSE(launchChecker.AnalysisComplete());
    EXPECT_EQ(launchChecker.Stats().failedLaunches, 1U);
    Memcheck syncChecker;
    AclsanSynchronizeData sync{};
    InitCommon(sync, 0, 507015);
    ASSERT_TRUE(
        syncChecker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    EXPECT_FALSE(syncChecker.AnalysisComplete());
}

TEST(MemcheckTest, IgnoresHostResourceEvents)
{
    Memcheck checker;
    auto hostResource = AllocationEvent(0x180000, 256, 2);
    hostResource.memorySpace = ACLSAN_MEMORY_SPACE_HOST;

    checker.OnAllocation(hostResource);
    checker.OnFree(hostResource);

    EXPECT_EQ(checker.Stats().allocations, 0U);
    EXPECT_EQ(checker.Stats().frees, 0U);
}

TEST(MemcheckTest, ReportsOutOfBoundsWriteAndIgnoresNonGmAccesses)
{
    Memcheck checker;
    const auto invalidWrite = Access(DeviceSourceKind::MTE3, 0x200000, 64);
    checker.QueueDeviceMemoryAccess(invalidWrite);

    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().access.accessMode, NpuCheckReportAccessMode::WRITE);

    auto nonGm = invalidWrite;
    nonGm.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    checker.QueueDeviceMemoryAccess(nonGm);
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
    EXPECT_TRUE(checker.OnSynchronization().empty());
}

TEST(MemcheckTest, ExpandsBlockRepeatAndAffineLayouts)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x200000, 512, 2));

    auto blockRepeat = Access(DeviceSourceKind::MTE3, 0x2001e8, 1);
    blockRepeat.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    blockRepeat.layout.blockRepeat.blockNum = 1;
    blockRepeat.layout.blockRepeat.blockSize = 16;
    blockRepeat.layout.blockRepeat.repeatTimes = 2;
    blockRepeat.layout.blockRepeat.repeatStride = 16;
    checker.QueueDeviceMemoryAccess(blockRepeat);
    auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);

    auto ndAffine = Access(DeviceSourceKind::MTE3, 0x2001f8, 1);
    ndAffine.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    ndAffine.layout.ndAffine.rank = 1;
    ndAffine.layout.ndAffine.elementBytes = 8;
    ndAffine.layout.ndAffine.dims[0] = 2;
    ndAffine.layout.ndAffine.strides[0] = 4;
    checker.QueueDeviceMemoryAccess(ndAffine);
    reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
}

TEST(MemcheckTest, UsesByteStrideForSparseGmAccesses)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x1000, 32, 1));
    checker.OnAllocation(AllocationEvent(0x1080, 32, 2));
    checker.OnAllocation(AllocationEvent(0x1100, 32, 3));

    auto blockRepeat = Access(DeviceSourceKind::MTE2, 0x1000, 1);
    blockRepeat.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    blockRepeat.layout.blockRepeat.blockNum = 3;
    blockRepeat.layout.blockRepeat.blockSize = 32;
    blockRepeat.layout.blockRepeat.blockStride = 128;
    blockRepeat.layout.blockRepeat.repeatTimes = 1;
    checker.QueueDeviceMemoryAccess(blockRepeat);
    EXPECT_TRUE(checker.OnSynchronization().empty());

    blockRepeat.layout.blockRepeat.blockNum = 4;
    checker.QueueDeviceMemoryAccess(blockRepeat);
    const auto reports = checker.OnSynchronization();

    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().access.address, 0x1180U);
    EXPECT_EQ(reports.front().access.accessBytes, 32U);
    EXPECT_EQ(reports.front().access.rangeBegin, 0x1180U);
    EXPECT_EQ(reports.front().access.rangeEnd, 0x11a0U);
}

TEST(MemcheckTest, ProcessesMoreThanFourRangesFromOneInstruction)
{
    constexpr uint64_t kBase = 0x8000;
    constexpr uint64_t kStride = 0x100;
    constexpr uint64_t kAccessBytes = 16;
    constexpr uint32_t kAccessCount = 9;
    constexpr uint64_t kInstrExecId = 41;
    constexpr uint64_t kSerialNo = 73;

    Memcheck checker;
    for (uint32_t index = 0; index + 1 < kAccessCount; ++index) {
        checker.OnAllocation(AllocationEvent(kBase + index * kStride, kAccessBytes, index + 1));
    }

    for (uint32_t index = 0; index < kAccessCount; ++index) {
        auto access = Access(DeviceSourceKind::MTE2, kBase + index * kStride, kAccessBytes);
        access.header.instrExecId = kInstrExecId;
        access.header.serialNo = kSerialNo;
        access.accessIndex = index;
        access.accessCount = kAccessCount;
        checker.QueueDeviceMemoryAccess(access);
    }

    EXPECT_EQ(checker.Stats().pendingDeviceOperations, kAccessCount);
    const auto reports = checker.OnSynchronization();

    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
    EXPECT_EQ(reports.front().access.address, kBase + (kAccessCount - 1) * kStride);
    EXPECT_EQ(reports.front().access.accessBytes, kAccessBytes);
    EXPECT_EQ(reports.front().common.exec.instrExecId, kInstrExecId);
    EXPECT_EQ(reports.front().common.exec.serialNo, kSerialNo);
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
    EXPECT_EQ(checker.Stats().errors, 1U);
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
}

TEST(MemcheckTest, ProcessesDisjointValidRangesWithoutCheckingTheirEnvelope)
{
    constexpr uint64_t kBase = 0xa000;
    constexpr uint64_t kStride = 0x100;
    constexpr uint64_t kAccessBytes = 16;
    constexpr uint32_t kAccessCount = 9;
    constexpr uint64_t kInstrExecId = 42;
    constexpr uint64_t kSerialNo = 74;

    Memcheck checker;
    for (uint32_t index = 0; index < kAccessCount; ++index) {
        const uint64_t address = kBase + index * kStride;
        checker.OnAllocation(AllocationEvent(address, kAccessBytes, index + 1));

        auto access = Access(DeviceSourceKind::MTE2, address, kAccessBytes);
        access.header.instrExecId = kInstrExecId;
        access.header.serialNo = kSerialNo;
        access.accessIndex = index;
        access.accessCount = kAccessCount;
        checker.QueueDeviceMemoryAccess(access);
    }

    EXPECT_EQ(checker.Stats().pendingDeviceOperations, kAccessCount);
    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().pendingDeviceOperations, 0U);
    EXPECT_EQ(checker.Stats().errors, 0U);
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
}

TEST(MemcheckTest, ChecksScalarAccessAtAllocationBoundary)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x3000, 16, 1));

    auto scalar = Access(DeviceSourceKind::MTE2, 0x300f, 1);
    scalar.layoutKind = ACLSAN_MEM_LAYOUT_SCALAR;
    scalar.layout.scalar.bytes = 1;
    checker.QueueDeviceMemoryAccess(scalar);
    EXPECT_TRUE(checker.OnSynchronization().empty());

    scalar.layout.scalar.bytes = 2;
    checker.QueueDeviceMemoryAccess(scalar);
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().access.address, 0x300fU);
    EXPECT_EQ(reports.front().access.accessBytes, 2U);
}

TEST(MemcheckTest, RepresentsTruncatedRowsWithoutTouchingPadding)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x4000, 20, 1));

    auto truncated = Access(DeviceSourceKind::MTE2, 0x4000, 1);
    truncated.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    truncated.layout.blockRepeat.blockNum = 3;
    truncated.layout.blockRepeat.blockSize = 4;
    truncated.layout.blockRepeat.blockStride = 8;
    truncated.layout.blockRepeat.repeatTimes = 1;
    checker.QueueDeviceMemoryAccess(truncated);

    EXPECT_TRUE(checker.OnSynchronization().empty());
}

TEST(MemcheckTest, CoalescesLargeDenseAffineLayout)
{
    Memcheck checker;
    constexpr uint64_t kBase = 0x100000;
    constexpr uint64_t kBytes = 2ULL * 1024 * 1024;
    checker.OnAllocation(AllocationEvent(kBase, kBytes, 1));

    auto dense = Access(DeviceSourceKind::MTE2, kBase, 1);
    dense.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    dense.layout.ndAffine.rank = 5;
    dense.layout.ndAffine.elementBytes = 1;
    dense.layout.ndAffine.dims[0] = 2;
    dense.layout.ndAffine.dims[1] = 16;
    dense.layout.ndAffine.dims[2] = 16;
    dense.layout.ndAffine.dims[3] = 64;
    dense.layout.ndAffine.dims[4] = 64;
    dense.layout.ndAffine.strides[0] = 1024 * 1024;
    dense.layout.ndAffine.strides[1] = 64 * 1024;
    dense.layout.ndAffine.strides[2] = 4 * 1024;
    dense.layout.ndAffine.strides[3] = 64;
    dense.layout.ndAffine.strides[4] = 1;
    checker.QueueDeviceMemoryAccess(dense);

    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
}

TEST(MemcheckTest, RepresentsBatchedNzZnZzAndNnLayouts)
{
    constexpr uint32_t kNdRank = 5;
    constexpr uint64_t kBase = 0x7000;
    constexpr uint64_t kMatrixBytes = 36;
    constexpr std::array<uint64_t, kNdRank> kDimensions{2, 2, 3, 2, 3};
    const std::array<std::array<int64_t, kNdRank>, 4> formatStrides{{
        {kMatrixBytes, 6, 12, 3, 1}, // NZ
        {kMatrixBytes, 18, 6, 1, 2}, // ZN
        {kMatrixBytes, 18, 6, 3, 1}, // ZZ
        {kMatrixBytes, 6, 12, 1, 2}, // NN
    }};

    for (const auto& strides : formatStrides) {
        Memcheck checker;
        checker.OnAllocation(AllocationEvent(kBase, 2 * kMatrixBytes - 1, 1));
        auto access = Access(DeviceSourceKind::MTE2, kBase, 1);
        access.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
        access.layout.ndAffine.rank = kNdRank;
        access.layout.ndAffine.elementBytes = 1;
        for (uint32_t dimension = 0; dimension < kNdRank; ++dimension) {
            access.layout.ndAffine.dims[dimension] = kDimensions[dimension];
            access.layout.ndAffine.strides[dimension] = strides[dimension];
        }
        checker.QueueDeviceMemoryAccess(access);

        const auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(reports.front().access.address, kBase);
        EXPECT_EQ(reports.front().access.rangeEnd, kBase + 2 * kMatrixBytes);
        EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
    }
}

TEST(MemcheckTest, DropsOversizedSparseAffineLayoutWithoutEnvelopeFalsePositive)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x5000, 8, 1));
    checker.OnAllocation(AllocationEvent(0x100005000, 8, 2));

    auto sparse = Access(DeviceSourceKind::MTE2, 0x5000, 1);
    sparse.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    sparse.layout.ndAffine.rank = 2;
    sparse.layout.ndAffine.elementBytes = 8;
    sparse.layout.ndAffine.dims[0] = 1025;
    sparse.layout.ndAffine.dims[1] = 1025;
    sparse.layout.ndAffine.strides[0] = 1LL << 32U;
    sparse.layout.ndAffine.strides[1] = 1LL << 32U;
    checker.QueueDeviceMemoryAccess(sparse);

    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 1U);
}

TEST(MemcheckTest, DropsExtremeAndMalformedLayouts)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x1000, 64, 1));

    auto extreme = Access(DeviceSourceKind::MTE2, 0x1000, 1);
    extreme.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    extreme.layout.blockRepeat.blockNum = std::numeric_limits<uint32_t>::max();
    extreme.layout.blockRepeat.blockSize = std::numeric_limits<uint32_t>::max();
    extreme.layout.blockRepeat.blockStride = std::numeric_limits<int64_t>::max();
    extreme.layout.blockRepeat.repeatTimes = std::numeric_limits<uint32_t>::max();
    extreme.layout.blockRepeat.repeatStride = std::numeric_limits<int64_t>::max();
    checker.QueueDeviceMemoryAccess(extreme);

    auto zeroCount = Access(DeviceSourceKind::MTE2, 0x1000, 2);
    zeroCount.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    zeroCount.layout.blockRepeat.blockNum = 0;
    zeroCount.layout.blockRepeat.blockSize = 1;
    zeroCount.layout.blockRepeat.repeatTimes = 1;
    checker.QueueDeviceMemoryAccess(zeroCount);

    auto invalidRank = Access(DeviceSourceKind::MTE2, 0x1000, 3);
    invalidRank.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    invalidRank.layout.ndAffine.rank = 6;
    invalidRank.layout.ndAffine.elementBytes = 1;
    checker.QueueDeviceMemoryAccess(invalidRank);

    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 3U);
}

TEST(MemcheckTest, SupportsNegativeBlockAndAffineStrides)
{
    constexpr uint64_t kElementBytes = 16;
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x1000, kElementBytes, 1));
    checker.OnAllocation(AllocationEvent(0x1040, kElementBytes, 2));
    checker.OnAllocation(AllocationEvent(0x1080, kElementBytes, 3));

    auto blockRepeat = Access(DeviceSourceKind::MTE2, 0x1080, 1);
    blockRepeat.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    blockRepeat.layout.blockRepeat.blockNum = 3;
    blockRepeat.layout.blockRepeat.blockSize = kElementBytes;
    blockRepeat.layout.blockRepeat.blockStride = -0x40;
    blockRepeat.layout.blockRepeat.repeatTimes = 1;
    checker.QueueDeviceMemoryAccess(blockRepeat);
    EXPECT_TRUE(checker.OnSynchronization().empty());

    auto affine = Access(DeviceSourceKind::MTE2, 0x1080, 1);
    affine.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
    affine.layout.ndAffine.rank = 1;
    affine.layout.ndAffine.elementBytes = kElementBytes;
    affine.layout.ndAffine.dims[0] = 4;
    affine.layout.ndAffine.strides[0] = -0x40;
    checker.QueueDeviceMemoryAccess(affine);
    const auto reports = checker.OnSynchronization();

    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().access.address, 0xfc0U);
    EXPECT_EQ(reports.front().access.accessBytes, kElementBytes);
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
}

TEST(MemcheckTest, DropsLayoutWhoseSegmentAddressOverflows)
{
    Memcheck checker;
    auto overflow = Access(DeviceSourceKind::MTE2, std::numeric_limits<uint64_t>::max() - 7, 1);
    overflow.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
    overflow.layout.blockRepeat.blockNum = 2;
    overflow.layout.blockRepeat.blockSize = 8;
    overflow.layout.blockRepeat.blockStride = 16;
    overflow.layout.blockRepeat.repeatTimes = 1;
    checker.QueueDeviceMemoryAccess(overflow);

    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 1U);
}

TEST(MemcheckTest, EveryAtomicModeChecksTheWholeReadModifyWriteRangeOnce)
{
    const std::pair<AclsanDeviceMemoryAccessMode, NpuCheckReportAccessMode> modes[] = {
        {ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, NpuCheckReportAccessMode::ATOMIC_WRITE_ADD},
        {ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN, NpuCheckReportAccessMode::ATOMIC_WRITE_MIN},
        {ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX, NpuCheckReportAccessMode::ATOMIC_WRITE_MAX}};
    for (const auto& [wireMode, reportMode] : modes) {
        SCOPED_TRACE(wireMode);
        Memcheck checker;
        const auto allocation = AllocationEvent(0x1000, 32, 1);
        checker.OnAllocation(allocation);
        auto access = Access(DeviceSourceKind::MTE3, 0x101c, 4);
        access.accessMode = wireMode;
        checker.QueueDeviceMemoryAccess(access);
        EXPECT_TRUE(checker.OnSynchronization().empty());
        access.layout.range.bytes = 8;
        checker.QueueDeviceMemoryAccess(access);
        const auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(reports[0].access.accessMode, reportMode);
        EXPECT_EQ(reports[0].access.accessBytes, 8U);
        EXPECT_EQ(checker.Stats().errors, 1U);
    }
}

TEST(MemcheckTest, IgnoresApiVersionWhenProcessingAccess)
{
    auto access = Access(DeviceSourceKind::MTE3, 0x1000, 4);
    access.header.version = ACLSAN_API_VERSION + 1;
    access.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    Memcheck checker;
    CheckerReportList reports;
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
    EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0U);
    EXPECT_EQ(checker.OnSynchronization().size(), 1U);
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(MemcheckTest, ReportsAtomicWriteOnceAndHonorsPredicate)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x200000, 512, 2));

    auto atomicWrite = Access(DeviceSourceKind::MTE3, 0x2001fc, 8);
    atomicWrite.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD;
    checker.QueueDeviceMemoryAccess(atomicWrite);
    const auto reports = checker.OnSynchronization();

    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports[0].access.accessMode, NpuCheckReportAccessMode::ATOMIC_WRITE_ADD);
    EXPECT_EQ(reports[0].access.rangeBegin, 0x2001fcU);
    EXPECT_EQ(reports[0].access.rangeEnd, 0x200204U);
    EXPECT_EQ(checker.Stats().errors, 1U);
    std::string rendered;
    ASSERT_EQ(
        RenderNpuCheckReportRecord(NpuCheckReportRecord::From(reports[0]), {}, &rendered), ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("Invalid GM atomic write(add) of size 8 bytes"), std::string::npos);

    atomicWrite.header.flags = kDeviceEventFlagPredicated;
    atomicWrite.predicateMask0 = 0;
    atomicWrite.predicateMask1 = 0;
    checker.QueueDeviceMemoryAccess(atomicWrite);
    EXPECT_TRUE(checker.OnSynchronization().empty());
    EXPECT_EQ(checker.Stats().errors, 1U);
}

TEST(MemcheckTest, ReportsUnknownAndFreedRanges)
{
    Memcheck checker;
    checker.QueueDeviceMemoryAccess(Access(DeviceSourceKind::MTE2, 0x500000, 8));
    const auto unknown = checker.OnSynchronization();
    ASSERT_EQ(unknown.size(), 1U);
    EXPECT_EQ(unknown.front().common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
    const auto released = AllocationEvent(0x300000, 128, 21);
    checker.OnAllocation(released);
    checker.OnFree(released);
    checker.QueueDeviceMemoryAccess(Access(DeviceSourceKind::MTE2, 0x300010, 16));
    const auto diagnostics = checker.OnSynchronization();

    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.front().common.pattern, NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE);
    EXPECT_EQ(diagnostics.front().allocation.state, 2U);
}

TEST(MemcheckTest, UsesDeviceSpecificAllocationRanges)
{
    Memcheck checker;
    checker.OnAllocation(AllocationEvent(0x600000, 64, 31, 0));
    checker.OnAllocation(AllocationEvent(0x600000, 128, 31, 1));
    checker.QueueDeviceMemoryAccess(Access(DeviceSourceKind::MTE2, 0x600030, 32, 0));

    const auto reports = checker.OnSynchronization();

    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front().common.pattern, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS);
    EXPECT_EQ(reports.front().common.exec.deviceId, 0U);
}

TEST(MemcheckTest, GroupsDerivedDataByCompleteInstructionIdentity)
{
    Memcheck checker;
    auto aicRead = Access(DeviceSourceKind::MTE2, 0x700000, 64, 1);
    aicRead.header.launchId = 9;
    aicRead.header.blockId = 0;
    aicRead.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    aicRead.header.phyCoreId = 3;
    aicRead.header.instrExecId = 1;
    aicRead.header.serialNo = 0;
    aicRead.accessIndex = 0;
    aicRead.accessCount = 2;

    auto aicWrite = aicRead;
    aicWrite.header.serialNo = 1;
    aicWrite.address = 0x710000;
    aicWrite.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    aicWrite.accessIndex = 1;

    auto aivRead = aicRead;
    aivRead.header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    aivRead.header.phyCoreId = 4;
    aivRead.address = 0x720000;

    checker.QueueDeviceMemoryAccess(aicRead);
    checker.QueueDeviceMemoryAccess(aicWrite);
    checker.QueueDeviceMemoryAccess(aivRead);
    const auto reports = checker.OnSynchronization();

    ASSERT_EQ(reports.size(), 3U);
    EXPECT_EQ(reports[0].common.groupId, reports[1].common.groupId);
    EXPECT_NE(reports[0].common.groupId, reports[2].common.groupId);
    EXPECT_EQ(reports[0].common.exec.phyCoreId, 3U);
    EXPECT_EQ(reports[2].common.exec.phyCoreId, 4U);

    std::string rendered;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(npucheck::NpuCheckReportRecord::From(reports[2]), {}, &rendered),
        ReportRenderStatus::SUCCESS);
    EXPECT_NE(rendered.find("by aicore (4) type (AIV) block (0) pipe (PIPE_MTE2)"), std::string::npos);
}

} // namespace
} // namespace npucheck
