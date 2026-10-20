// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.
#include "acl_san/aclsan_api.h"
#include <gtest/gtest.h>
#include "aclsan_memory_cbdata.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "device_instr/common/instruction_id.h"
#include "checker/memcheck.h"
#include "aclsan_device_data.h"
#include "aclsan_active_probe_plan.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"
#include <algorithm>

namespace {
using namespace aclsan;

MemoryRegisterState SingleGmToL1Iteration()
{
    MemoryRegisterState state{};
    state.dmaLoopSizes[static_cast<size_t>(DmaLoopDirection::GM_TO_CBUF)] =
        DmaLoopSizeParamField{DmaLoopDirection::GM_TO_CBUF, 1, 1};
    return state;
}

MemoryCbdataResult ConvertRaw(
    uint32_t id, std::array<uint64_t, 4> args, MemoryRegisterState state = {}, uint64_t arg4 = 0)
{
    // 这些 footprint 用例专门验证非原子传输。
    state.ctrl = CtrlRegisterParamField{0};
    AclsanRawTraceRecord raw{};
    raw.instrId = id;
    std::copy(args.begin(), args.end(), raw.args);
    raw.args[4] = arg4;
    const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
    if (!decoded) {
        return {MemoryCbdataStatus::INVALID_FIELD, {}};
    }
    return std::visit(
        [&](const auto& f) -> MemoryCbdataResult {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_constructible_v<MemoryInstructionField, T>) {
                return MemoryFieldToCbdataConverter{{}, state}.Convert(MemoryInstructionField{f});
            }
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        },
        decoded->params);
}

TEST(CubeMemory, AllRequestedIdsProduceLocalAccesses)
{
    auto state = SingleGmToL1Iteration();
    state.mte2Source = Mte2SourceParamField{1};
    state.mte2Nz = Mte2NzParamField{1, 1, 16, 16};
    for (uint32_t id : {72u,  73u,  74u,  75u,  76u,  77u,  78u,  79u,  80u,  81u,  82u,  149u, 150u,
                        151u, 152u, 158u, 173u, 91u,  92u,  168u, 169u, 170u, 171u, 400u, 401u, 402u,
                        403u, 404u, 405u, 406u, 407u, 408u, 409u, 410u, 411u, 412u, 413u, 414u, 415u}) {
        SCOPED_TRACE(id);
        std::array<uint64_t, 4> a{};
        if (id == 72) {
            a[3] = 1 | (1ULL << 12) | (1ULL << 24);
        } else if (id == 73) {
            a[2] = (1ULL << 4) | (1ULL << 25);
        } else if (id >= 74 && id <= 76) {
            a[2] = (1ULL << 4) | (32ULL << 25);
            a[3] = 32ULL << 40;
        } else if (id >= 77 && id <= 82) {
            a[2] = 16ULL << 48;
            a[3] = 16;
        } else if (id == 149 || id == 150) {
            a[1] = 1 | (1ULL << 16);
        } else if (id == 151 || id == 152) {
            a[2] = (1ULL << 32) | (1ULL << 40);
            a[3] = 1;
        } else if (id == 158 || id == 173) {
            a[2] = (1ULL << 4) | (1ULL << 16);
        } else if (id >= 400) {
            a[3] = 16 | (64ULL << 12) | (16ULL << 24) | (1ULL << 63);
        } else {
            a[2] = (16ULL << 4) | (16ULL << 16) | (256ULL << 32);
            a[3] = 16;
        }
        auto result = ConvertRaw(id, a, state);
        ASSERT_EQ(result.status, MemoryCbdataStatus::SUCCESS);
        EXPECT_TRUE(std::any_of(result.data.begin(), result.data.end(), [](const auto& d) {
            return d.memorySpace >= ACLSAN_DEVICE_MEMORY_SPACE_L1 && d.memorySpace <= ACLSAN_DEVICE_MEMORY_SPACE_L0C;
        }));
    }
}

TEST(CubeMemory, GmToL1ChecksIndependentDestinationStrideAndDmaLoops)
{
    MemoryRegisterState state{};
    const size_t direction = static_cast<size_t>(DmaLoopDirection::GM_TO_CBUF);
    state.dmaLoopSizes[direction] = DmaLoopSizeParamField{DmaLoopDirection::GM_TO_CBUF, 2, 1};
    state.dmaLoopStrides[direction][0] = DmaLoopStrideParamField{DmaLoopDirection::GM_TO_CBUF, 0, 4096, 128};
    const auto r = ConvertRaw(73, {0x7ff80, 0x100000, (2ULL << 4) | (1ULL << 25), 1 | (2ULL << 40)}, state);
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    EXPECT_EQ(r.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    EXPECT_EQ(r.data[1].layout.ndAffine.strides[0], 96);
    EXPECT_EQ(r.data[1].layout.ndAffine.strides[1], 128);
    npucheck::Memcheck checker;
    checker.QueueDeviceMemoryAccess(r.data[1]);
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].access.address, 0x80000u);
    EXPECT_EQ(reports[1].access.address, 0x80060u);
}

TEST(CubeMemory, MmadFractalRoundingAndConditionalAccumulatorRead)
{
    auto r = ConvertRaw(403, {0x3fc00, 0, 0, 16 | (64ULL << 12) | (16ULL << 24) | (1ULL << 63)});
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 3u);
    EXPECT_EQ(r.data[0].layout.range.bytes, 4096u);
    EXPECT_EQ(r.data[1].layout.range.bytes, 4096u);
    EXPECT_EQ(r.data[2].layout.range.bytes, 1024u);
    npucheck::Memcheck checker;
    for (auto d : r.data) {
        checker.QueueDeviceMemoryAccess(d);
    }
    EXPECT_TRUE(checker.OnSynchronization().empty());
    r = ConvertRaw(403, {0x40000, 0, 0, 17 | (9ULL << 12) | (17ULL << 24)});
    ASSERT_EQ(r.data.size(), 4u);
    EXPECT_EQ(r.data[0].layout.range.bytes, 2048u);
    EXPECT_EQ(r.data[2].layout.range.bytes, 4096u);
    EXPECT_EQ(r.data[3].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    for (auto d : r.data) {
        checker.QueueDeviceMemoryAccess(d);
    }
    EXPECT_EQ(checker.OnSynchronization().size(), 2u);
}

TEST(CubeMemory, CapacityBoundariesAndOverflowAreIndependentOfGmAllocations)
{
    for (auto entry :
         {std::pair<uint32_t, uint64_t>{ACLSAN_DEVICE_MEMORY_SPACE_L1, 524288},
          {ACLSAN_DEVICE_MEMORY_SPACE_L0A, 65536},
          {ACLSAN_DEVICE_MEMORY_SPACE_L0B, 65536},
          {ACLSAN_DEVICE_MEMORY_SPACE_L0C, 262144}}) {
        npucheck::Memcheck checker;
        AclsanDeviceMemoryAccessData d{};
        d.header.version = ACLSAN_API_VERSION;
        d.header.size = sizeof(d);
        d.memorySpace = entry.first;
        d.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        d.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        d.layout.range.bytes = 32;
        d.address = entry.second - 32;
        checker.QueueDeviceMemoryAccess(d);
        EXPECT_TRUE(checker.OnSynchronization().empty());
        for (uint64_t address : {entry.second - 16, entry.second, UINT64_MAX - 15}) {
            d.address = address;
            checker.QueueDeviceMemoryAccess(d);
            const auto reports = checker.OnSynchronization();
            ASSERT_EQ(reports.size(), 1u);
            EXPECT_EQ(uint32_t(reports[0].access.memorySpace), entry.first);
            EXPECT_EQ(reports[0].nearestAllocation.base, 0u);
            EXPECT_EQ(reports[0].nearestAllocation.bytes, entry.second);
        }
    }
}

TEST(CubeMemory, UnsupportedModesRetainKnownGmAccesses)
{
    auto r = ConvertRaw(73, {0, 0, (1ULL << 4) | (1ULL << 25) | (1ULL << 56), 0}, SingleGmToL1Iteration());
    EXPECT_EQ(r.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    ASSERT_FALSE(r.data.empty()); // Existing GM coverage is retained.
    EXPECT_EQ(r.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
}

TEST(CubeMemory, CallbackToCheckerReportsLocalBoundsAndRetainsInstructionContext)
{
    ParsedTraceRecord parsed{};
    parsed.record.instrId = 73;
    parsed.record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    parsed.record.pc = 0x1234;
    parsed.record.args[0] = 0x80000;
    parsed.record.args[1] = 0x100000;
    parsed.record.args[2] = (1ULL << 4) | (1ULL << 25);
    parsed.instrExecId = 23;
    parsed.launchId = 5;
    parsed.phyCoreId = 7;
    parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
    ASSERT_TRUE(decoded);
    auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded, SingleGmToL1Iteration());
    ASSERT_TRUE(callback);
    const auto& accesses = std::get<DeviceMemoryAccessDataList>(*callback);
    ASSERT_EQ(accesses.size(), 2u);
    npucheck::Memcheck checker;
    AclsanResourceData allocation{};
    allocation.ptr = reinterpret_cast<void*>(0x100000);
    allocation.bytes = 32;
    allocation.resourceId = 1;
    allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    checker.OnAllocation(allocation);
    for (const auto& access : accesses) {
        EXPECT_EQ(access.accessCount, 2u);
        checker.QueueDeviceMemoryAccess(access);
    }
    auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].common.exec.pc, 0x1234u);
    EXPECT_EQ(reports[0].common.exec.instrExecId, 23u);
    std::string text;
    EXPECT_EQ(
        npucheck::RenderNpuCheckReportRecord(npucheck::NpuCheckReportRecord::From(reports[0]), {}, &text),
        npucheck::ReportRenderStatus::SUCCESS);
    EXPECT_NE(text.find("Invalid L1 write of size 32 bytes"), std::string::npos);
    EXPECT_NE(text.find("L1 capacity 524288 bytes, valid offsets [0, 524288)"), std::string::npos);
    EXPECT_TRUE(checker.AnalysisComplete());

    // Unsupported local padding leaves only the proven GM read. No synthetic
    // memory/error callback is emitted, so completeness is not signaled here.
    parsed.record.args[2] |= 1ULL << 56;
    decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
    callback = TranslateDecodedTraceToCallbackData(parsed, *decoded, SingleGmToL1Iteration());
    ASSERT_TRUE(callback);
    const auto& knownAccesses = std::get<DeviceMemoryAccessDataList>(*callback);
    ASSERT_EQ(knownAccesses.size(), 1u);
    EXPECT_EQ(knownAccesses[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    EXPECT_EQ(knownAccesses[0].instructionId, 73U);
    EXPECT_EQ(
        knownAccesses[0].regDependencyMask0, (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_CBUF) |
                                                 (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_SET_PADDING));
    checker.QueueDeviceMemoryAccess(knownAccesses[0]);
    checker.OnSynchronization();
    EXPECT_TRUE(checker.AnalysisComplete());
}

TEST(CubeMemory, MemcheckSubscriptionIncludesMatrixProbes)
{
    EXPECT_NE(
        ProbeGroupMaskForCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) &
            PROBE_GROUP_MATRIX,
        0u);
}

TEST(CubeMemory, SetAndMxUseIndependentRowStrides)
{
    auto set = ConvertRaw(149, {0x7ff80, 2 | (2ULL << 16) | (4ULL << 32), 0, 0});
    ASSERT_EQ(set.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(set.data.size(), 1u);
    EXPECT_EQ(set.data[0].layout.blockRepeat.blockSize, 64u);
    EXPECT_EQ(set.data[0].layout.blockRepeat.repeatStride, 128);
    auto mx = ConvertRaw(151, {0, 0x100, 2 | (3ULL << 16) | (2ULL << 32) | (2ULL << 40), 8});
    ASSERT_EQ(mx.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(mx.data.size(), 1u);
    EXPECT_EQ(mx.data[0].address, 0x360u);
    EXPECT_EQ(mx.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(mx.data[0].layout.blockRepeat.blockSize, 64u);
    EXPECT_EQ(mx.data[0].layout.blockRepeat.repeatStride, 256);
}

TEST(CubeMemory, NdAndDnToNzWriteSameSparseDestination)
{
    MemoryRegisterState state{};
    state.mte2Nz = Mte2NzParamField{2, 2, 16, 64};
    for (uint32_t id : {78u, 81u}) {
        auto r = ConvertRaw(id, {0x1000, 0x100000, (3ULL << 48) | (64ULL << 4), 17 | (512ULL << 21)}, state);
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        const auto& d = r.data[1];
        EXPECT_EQ(d.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
        EXPECT_EQ(d.layout.ndAffine.elementBytes, 32u);
        EXPECT_EQ(d.layout.ndAffine.dims[0], 3u);
        EXPECT_EQ(d.layout.ndAffine.dims[1], 2u);
        EXPECT_EQ(d.layout.ndAffine.dims[2], 2u);
        EXPECT_EQ(d.layout.ndAffine.strides[0], 64);
        EXPECT_EQ(d.layout.ndAffine.strides[1], 512);
        EXPECT_EQ(d.layout.ndAffine.strides[2], 2048);
    }
}

TEST(CubeMemory, FixpipeReadsFullAccumulatorRowsForEveryDestination)
{
    for (uint32_t id : {91u, 92u, 168u, 169u, 170u, 171u}) {
        SCOPED_TRACE(id);
        auto r = ConvertRaw(id, {0x7fc00, 0x3fc00, (16ULL << 4) | (16ULL << 16) | (256ULL << 32), 16});
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        const auto read = std::find_if(r.data.begin(), r.data.end(), [](const auto& d) {
            return d.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0C;
        });
        ASSERT_NE(read, r.data.end());
        EXPECT_EQ(read->layout.range.bytes, 1024u);
        npucheck::Memcheck checker;
        checker.QueueDeviceMemoryAccess(*read);
        EXPECT_TRUE(checker.OnSynchronization().empty());
        auto bad = *read;
        bad.address = 0x3fe00;
        checker.QueueDeviceMemoryAccess(bad);
        EXPECT_EQ(checker.OnSynchronization().size(), 1u);
        if (id == 170 || id == 171) {
            EXPECT_EQ(r.data.size(), 1u);
        }
    }
}

TEST(CubeMemory, FixpipeNz2NdReadsOnlyTailElementsAndMatrixStride)
{
    MemoryRegisterState state{};
    state.loop3 = Loop3ParamField{2, 128, 1024};
    auto r = ConvertRaw(91, {0, 0, (17ULL << 4) | (2ULL << 16) | (32ULL << 32), 16 | (1ULL << 43)}, state);
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 5u);
    EXPECT_EQ(r.data[1].address, 0u);
    EXPECT_EQ(r.data[1].layout.range.bytes, 128u);
    EXPECT_EQ(r.data[2].address, 1024u);
    EXPECT_EQ(r.data[2].layout.blockRepeat.blockSize, 4u);
    EXPECT_EQ(r.data[2].layout.blockRepeat.repeatStride, 64);
    EXPECT_EQ(r.data[3].address, 8192u);
    EXPECT_EQ(r.data[4].address, 9216u);
}

TEST(CubeMemory, AlignedCopyChecksPaddedWriteLengthInsteadOfGmReadLength)
{
    const auto r = ConvertRaw(
        75, {0x7ffe0, 0x100000, (1ULL << 4) | (20ULL << 25) | (8ULL << 46) | (8ULL << 52), 64ULL << 40},
        SingleGmToL1Iteration());
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0].layout.range.bytes, 20u);
    EXPECT_EQ(r.data[1].layout.range.bytes, 64u);
    npucheck::Memcheck checker;
    checker.QueueDeviceMemoryAccess(r.data[1]);
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].access.accessBytes, 64u);
}

TEST(CubeMemory, MmadReportsBothSourceReadsInTheSameInstructionGroup)
{
    auto r = ConvertRaw(403, {0, 0x10000, 0x10000, 16 | (64ULL << 12) | (16ULL << 24) | (1ULL << 63)});
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    npucheck::Memcheck checker;
    for (auto d : r.data) {
        d.header.launchId = 3;
        d.header.instrExecId = 17;
        checker.QueueDeviceMemoryAccess(d);
    }
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].access.memorySpace, npucheck::NpuCheckReportMemorySpace::L0_A);
    EXPECT_EQ(reports[1].access.memorySpace, npucheck::NpuCheckReportMemorySpace::L0_B);
    EXPECT_EQ(reports[0].access.accessMode, npucheck::NpuCheckReportAccessMode::READ);
    EXPECT_EQ(reports[1].access.accessMode, npucheck::NpuCheckReportAccessMode::READ);
    EXPECT_EQ(reports[0].common.groupId, reports[1].common.groupId);
}
TEST(CubeMemory, AllEighteenL0LoadIdsProduceIndependentReadAndWrite)
{
    for (uint32_t id :
         {137u, 138u, 139u, 140u, 141u, 142u, 143u, 144u, 145u, 146u, 147u, 148u, 153u, 154u, 155u, 156u, 157u, 422u}) {
        SCOPED_TRACE(id);
        std::array<uint64_t, 4> args{0, 0, 0, 0};
        MemoryRegisterState state{};
        if (id <= 140) {
            args[2] = (1ULL << 16) | (1ULL << 24);
        } else if (id <= 148) {
            args[2] = (1ULL << 32) | (1ULL << 40);
            args[3] = 1 | (1ULL << 16);
        } else {
            const uint64_t c0 = (id == 153 || id == 155) ? 16 : (id == 154 || id == 156) ? 32 : 8;
            auto& fmatrix = state.fmatrix[0].emplace();
            fmatrix.width = 4;
            fmatrix.height = 4;
            auto& repeat = state.l3dRpt[0].emplace();
            repeat.repeatTimes = 1;
            repeat.dstStride = 1;
            args[2] = c0 | (16ULL << 16);
            args[3] = 1 | (1ULL << 6) | (1ULL << 12) | (1ULL << 20) | (1ULL << 28) | (1ULL << 36) | (c0 << 48);
        }
        const auto result = ConvertRaw(id, args, state);
        ASSERT_EQ(result.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_GE(result.data.size(), 2u);
        EXPECT_EQ(result.data.front().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
        EXPECT_EQ(result.data.front().accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        const bool a = (id >= 141 && id <= 144) || id == 153 || id == 154 || id == 422;
        EXPECT_EQ(result.data.back().memorySpace, a ? ACLSAN_DEVICE_MEMORY_SPACE_L0A : ACLSAN_DEVICE_MEMORY_SPACE_L0B);
        EXPECT_EQ(result.data.back().accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
        for (size_t i = 0; i < result.data.size(); ++i) {
            EXPECT_EQ(result.data[i].accessIndex, i);
            EXPECT_EQ(result.data[i].accessCount, result.data.size());
        }
    }
}

TEST(CubeMemory, Load2DDocumentedB32BoundaryExamples)
{
    for (uint32_t id : {144u, 148u}) {
        auto r = ConvertRaw(id, {0xfe00, 0, (1ULL << 32) | (1ULL << 40), 1 | (1ULL << 16)});
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        EXPECT_EQ(r.data[0].layout.range.bytes, 512u);
        EXPECT_EQ(r.data[1].layout.range.bytes, 512u);
        npucheck::Memcheck checker;
        for (const auto& access : r.data) {
            checker.QueueDeviceMemoryAccess(access);
        }
        EXPECT_TRUE(checker.OnSynchronization().empty());
        r.data[1].address = 0x10000;
        checker.QueueDeviceMemoryAccess(r.data[1]);
        auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1u);
        EXPECT_EQ(
            reports[0].access.memorySpace,
            id == 144 ? npucheck::NpuCheckReportMemorySpace::L0_A : npucheck::NpuCheckReportMemorySpace::L0_B);
        r.data[0].address = 0x80000;
        checker.QueueDeviceMemoryAccess(r.data[0]);
        reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1u);
        EXPECT_EQ(reports[0].access.accessMode, npucheck::NpuCheckReportAccessMode::READ);
        EXPECT_EQ(reports[0].access.memorySpace, npucheck::NpuCheckReportMemorySpace::L1);
    }
}

TEST(CubeMemory, Load2DTransposeUsesTypeDependentDestinationFractals)
{
    for (uint32_t id : {141u, 142u, 143u, 144u, 145u, 146u, 147u, 148u}) {
        auto r = ConvertRaw(id, {0, 0, 2 | (3ULL << 16) | (4ULL << 32) | (2ULL << 40), 16 | (32ULL << 16)}, {}, 1);
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        EXPECT_EQ(r.data[0].address, (3 * 16 + 2) * 512u);
        EXPECT_EQ(r.data[0].layout.blockRepeat.blockSize, 4 * 512u);
        EXPECT_EQ(r.data[0].layout.blockRepeat.repeatStride, 16 * 512);
        const uint64_t bits = (id == 141 || id == 146) ? 4 :
                              (id == 142 || id == 145) ? 16 :
                              (id == 143 || id == 147) ? 8 :
                                                         32;
        if (4 * bits / 16 == 1) {
            EXPECT_EQ(r.data[1].layout.range.bytes, 2 * 16 / bits * 512);
        } else {
            EXPECT_EQ(r.data[1].layout.blockRepeat.blockSize, 2 * 16 / bits * 512);
            EXPECT_EQ(r.data[1].layout.blockRepeat.repeatTimes, 4 * bits / 16);
            EXPECT_EQ(r.data[1].layout.blockRepeat.repeatStride, 32 * 512);
        }
    }
    const auto invalid = ConvertRaw(141, {0, 0, (1ULL << 32) | (1ULL << 40), 1 | (1ULL << 16)}, {}, 1);
    EXPECT_EQ(invalid.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    const auto unsignedStride =
        ConvertRaw(144, {0, 0, (1ULL << 16) | (1ULL << 32) | (1ULL << 40), 0xffff | (1ULL << 16)});
    ASSERT_EQ(unsignedStride.status, MemoryCbdataStatus::SUCCESS);
    EXPECT_EQ(unsignedStride.data[0].address, 65535ULL * 512);
}

TEST(CubeMemory, IndependentTransposeKeepsFractalGapsAndFullWidthGapIncrement)
{
    auto r = ConvertRaw(140, {0, 0, (2ULL << 16) | (32ULL << 24) | (63ULL << 44), 1 | (2ULL << 16)});
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    const auto& src = r.data[0].layout.ndAffine;
    const auto& dst = r.data[1].layout.ndAffine;
    EXPECT_EQ(src.elementBytes, 512u);
    EXPECT_EQ(src.dims[0], 4u);
    EXPECT_EQ(src.strides[0], 3 * 512);
    EXPECT_EQ(src.strides[1], 32 * 512);
    EXPECT_EQ(dst.strides[0], 2 * 512);
    EXPECT_EQ(dst.strides[1], 64 * 512);
    r = ConvertRaw(139, {0, 0, 1ULL << 16, 0xffff | (0xffffULL << 16)});
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    EXPECT_EQ(r.data[0].layout.blockRepeat.repeatStride, 65536LL * 512);
    EXPECT_EQ(r.data[1].layout.blockRepeat.repeatStride, 65536LL * 512);
    r = ConvertRaw(139, {0, 0, 1 | (1ULL << 16), 0});
    EXPECT_EQ(r.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
}

TEST(CubeMemory, Img2ColRegistersAreDecodedAndIsolatedByBankAndBlock)
{
    dav3510::Dav3510RegisterStateManager manager(7);
    const dav3510::Dav3510CoreKey a{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 0};
    const dav3510::Dav3510CoreKey b{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 1};
    for (uint32_t id : {386u, 387u, 390u, 391u}) {
        AclsanRawTraceRecord raw{};
        raw.instrId = id;
        raw.args[0] = id * 100;
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
        ASSERT_TRUE(decoded);
        if (id == 386 || id == 387) {
            manager.Update(a, std::get<FmatrixParamField>(decoded->params));
        } else {
            manager.Update(a, std::get<L3dRptParamField>(decoded->params));
        }
    }
    const auto state = manager.Get(a);
    ASSERT_TRUE(state);
    ASSERT_TRUE(state->fmatrix[0]);
    ASSERT_TRUE(state->fmatrix[1]);
    ASSERT_TRUE(state->l3dRpt[0]);
    ASSERT_TRUE(state->l3dRpt[1]);
    EXPECT_EQ(state->fmatrix[0]->width, 38600u);
    EXPECT_EQ(state->fmatrix[1]->width, 38700u);
    EXPECT_EQ(state->l3dRpt[0]->repeatStride, 39000u);
    EXPECT_EQ(state->l3dRpt[1]->repeatStride, 39100u);
    // Observed zero is a value, not a missing register; the other bank survives.
    manager.Update(a, FmatrixParamField{386});
    const auto zeroState = manager.Get(a);
    ASSERT_TRUE(zeroState->fmatrix[0]);
    EXPECT_EQ(zeroState->fmatrix[0]->width, 0u);
    EXPECT_EQ(zeroState->fmatrix[1]->width, 38700u);
    EXPECT_FALSE(manager.Get(b));
    manager.Reset();
    EXPECT_FALSE(manager.Get(a));
}

TEST(CubeMemory, Img2ColPaddingIsSynthesizedAndDestinationOffsetIsChecked)
{
    MemoryRegisterState state{};
    // 2x2 image padded on all sides to 4x4. Only four source pixels are read.
    auto& fmatrix = state.fmatrix[1].emplace();
    fmatrix.width = 2;
    fmatrix.height = 2;
    fmatrix.paddingLeft = 1;
    fmatrix.paddingRight = 1;
    fmatrix.paddingTop = 1;
    fmatrix.paddingBottom = 1;
    auto& repeat = state.l3dRpt[1].emplace();
    repeat.repeatTimes = 1;
    repeat.dstStride = 1;
    repeat.dstOffset = 1;
    const uint64_t config =
        1 | (1ULL << 6) | (1ULL << 12) | (1ULL << 20) | (1ULL << 28) | (1ULL << 36) | (1ULL << 47) | (16ULL << 48);
    auto r = ConvertRaw(155, {0xfe00, 0x7ff80, 16 | (16ULL << 16), config}, state);
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 5u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(r.data[i].address, 0x7ff80u + i * 32);
        EXPECT_EQ(r.data[i].layout.range.bytes, 32u);
    }
    EXPECT_EQ(r.data.back().address, 0x10000u);
    npucheck::Memcheck checker;
    for (const auto& d : r.data) {
        checker.QueueDeviceMemoryAccess(d);
    }
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].access.memorySpace, npucheck::NpuCheckReportMemorySpace::L0_B);
    r = ConvertRaw(155, {0, 0, 16 | (16ULL << 16), config}, {});
    EXPECT_EQ(r.status, MemoryCbdataStatus::MISSING_REGISTER_STATE);
    EXPECT_EQ(r.requiredRegisterInstructionId, 387u);
    state.l3dRpt[1].reset();
    r = ConvertRaw(155, {0, 0, 16 | (16ULL << 16), config}, state);
    EXPECT_EQ(r.status, MemoryCbdataStatus::MISSING_REGISTER_STATE);
    EXPECT_EQ(r.requiredRegisterInstructionId, 391u);
}

TEST(CubeMemory, Img2ColRejectsUnverifiedChannelPackingAndRepeatedWindows)
{
    MemoryRegisterState state{};
    auto& fmatrix = state.fmatrix[0].emplace();
    fmatrix.width = 4;
    fmatrix.height = 4;
    auto& repeat = state.l3dRpt[0].emplace();
    repeat.repeatTimes = 1;
    repeat.dstStride = 1;
    const uint64_t config = 1 | (1ULL << 6) | (1ULL << 12) | (1ULL << 20) | (1ULL << 28) | (1ULL << 36) | (32ULL << 48);
    auto r = ConvertRaw(153, {0, 0, 16 | (16ULL << 16), config}, state);
    EXPECT_EQ(r.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    state.l3dRpt[0]->repeatTimes = 2;
    r = ConvertRaw(154, {0, 0, 32 | (16ULL << 16), config}, state);
    EXPECT_EQ(r.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
}

TEST(CubeMemory, Img2ColDilationAndTransposePreserveSourceSetButChangeDestinationLayout)
{
    MemoryRegisterState state{};
    auto& fmatrix = state.fmatrix[0].emplace();
    fmatrix.width = 6;
    fmatrix.height = 4;
    auto& repeat = state.l3dRpt[0].emplace();
    repeat.repeatTimes = 1;
    repeat.dstStride = 4;
    // 1x2 filter with width dilation 2 gives a 4x4 output image.
    const uint64_t config = 1 | (1ULL << 6) | (2ULL << 12) | (1ULL << 20) | (2ULL << 28) | (1ULL << 36) | (16ULL << 48);
    const auto normal = ConvertRaw(153, {0, 0, 32 | (16ULL << 16), config}, state);
    const auto transposed = ConvertRaw(153, {0, 0, 32 | (16ULL << 16), config | (1ULL << 46)}, state);
    ASSERT_EQ(normal.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(transposed.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(normal.data.size(), 33u);
    ASSERT_EQ(transposed.data.size(), 33u);
    EXPECT_EQ(normal.data[0].address, 0u);
    EXPECT_EQ(normal.data[1].address, 64u);
    EXPECT_EQ(normal.data[2].address, 32u);
    EXPECT_EQ(normal.data[3].address, 96u);
    EXPECT_EQ(normal.data[30].address, 21 * 32u);
    EXPECT_EQ(normal.data[31].address, 23 * 32u);
    for (size_t i = 0; i < 32; ++i) {
        EXPECT_EQ(normal.data[i].address, transposed.data[i].address);
    }
    EXPECT_EQ(normal.data.back().layout.blockRepeat.blockSize, 512u);
    EXPECT_EQ(normal.data.back().layout.blockRepeat.repeatTimes, 2u);
    EXPECT_EQ(normal.data.back().layout.blockRepeat.repeatStride, 2048);
    EXPECT_EQ(transposed.data.back().layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
    EXPECT_EQ(transposed.data.back().layout.range.bytes, 1024u);
}

TEST(CubeMemory, L0LoadFailuresDoNotProduceSyntheticCallbacks)
{
    ParsedTraceRecord parsed{};
    parsed.record.instrId = 153;
    parsed.record.pipeline = ACLSAN_DEVICE_PIPE_MTE1;
    parsed.record.args[2] = 16 | (16ULL << 16);
    parsed.record.args[3] = 1 | (1ULL << 6) | (1ULL << 12) | (1ULL << 20) | (1ULL << 28) | (1ULL << 36) | (16ULL << 48);
    const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
    ASSERT_TRUE(decoded);
    const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded);
    EXPECT_FALSE(callback);
    auto overflow = ConvertRaw(144, {0, UINT64_MAX, 1 | (1ULL << 32) | (1ULL << 40), 1 | (1ULL << 16)});
    EXPECT_EQ(overflow.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    EXPECT_TRUE(overflow.data.empty());
    auto zero = ConvertRaw(144, {0, UINT64_MAX, 0, 0});
    EXPECT_EQ(zero.status, MemoryCbdataStatus::NO_ACCESS);
}
} // namespace

TEST(CubeMemoryRegression, CopyV2DestinationStrideIsGapAfterBurst)
{
    const auto result = ConvertRaw(73, {0x7ffe0, 0x100000, (2ULL << 4) | (1ULL << 25), 0}, SingleGmToL1Iteration());
    ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
    npucheck::Memcheck checker;
    for (const auto& access : result.data) {
        if (access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L1) {
            checker.QueueDeviceMemoryAccess(access);
        }
    }
    const auto reports = checker.OnSynchronization();
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].access.rangeEnd, 0x80020u);
}

TEST(CubeMemoryRegression, MissingAtomicConfigurationStillChecksGmWrites)
{
    for (uint32_t id : {56u, 57u, 58u, 59u, 60u, 61u, 83u, 91u}) {
        for (uint64_t address : {0x1000u, 0x2000u}) {
            SCOPED_TRACE(::testing::Message() << "id=" << id << " address=" << address);
            ParsedTraceRecord parsed{};
            parsed.record.instrId = id;
            parsed.record.args[0] = address;
            parsed.instrExecId = 23;
            MemoryRegisterState state{};
            if (id == 83) {
                parsed.record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
                parsed.record.args[2] = (1ULL << 4) | (32ULL << 25);
                state.dmaLoopSizes[static_cast<size_t>(DmaLoopDirection::UBUF_TO_GM)] =
                    DmaLoopSizeParamField{DmaLoopDirection::UBUF_TO_GM, 1, 1};
            } else if (id == 91) {
                parsed.record.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
                parsed.record.args[2] = (16ULL << 4) | (16ULL << 16) | (256ULL << 32);
                parsed.record.args[3] = 16;
            } else {
                parsed.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
                parsed.record.args[3] = UINT64_C(0x100000000);
                parsed.record.args[4] = ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;
            }
            const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
            ASSERT_TRUE(decoded);
            const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded, state);
            ASSERT_TRUE(callback);
            const auto& accesses = std::get<DeviceMemoryAccessDataList>(*callback);
            npucheck::Memcheck checker;
            AclsanResourceData allocation{};
            allocation.ptr = reinterpret_cast<void*>(0x1000);
            allocation.bytes = 4096;
            allocation.resourceId = 1;
            allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
            checker.OnAllocation(allocation);
            for (const auto& access : accesses) {
                if (access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
                    const uint32_t expectedFlags =
                        (id == 83 || id == 91) ?
                            ACLSAN_DEVICE_EVENT_FLAG_EXACT :
                            ACLSAN_DEVICE_EVENT_FLAG_EXACT | ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
                    EXPECT_EQ(access.header.flags, expectedFlags);
                    EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
                    if (id == 83 || id == 91) {
                        EXPECT_NE(access.regDependencyMask0 & (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CTRL), 0U);
                    }
                }
                checker.QueueDeviceMemoryAccess(access);
            }
            const auto reports = checker.OnSynchronization();
            ASSERT_EQ(reports.size(), address == 0x2000 ? 1u : 0u);
            if (!reports.empty()) {
                EXPECT_EQ(reports[0].access.accessMode, npucheck::NpuCheckReportAccessMode::WRITE);
                EXPECT_EQ(reports[0].access.address, address);
            }
            EXPECT_TRUE(checker.AnalysisComplete());
            EXPECT_EQ(checker.Stats().inexactDeviceOperations, 0u);
            EXPECT_EQ(checker.Stats().droppedDeviceOperations, 0u);
        }
    }
}

TEST(CubeMemoryRegression, Img2ColZeroRepeatCannotClaimExactCoverage)
{
    aclsan::MemoryRegisterState state{};
    auto& fmatrix = state.fmatrix[0].emplace();
    fmatrix.width = 4;
    fmatrix.height = 4;
    auto& repeat = state.l3dRpt[0].emplace();
    repeat.repeatTimes = 0;
    repeat.dstStride = 1;
    const uint64_t config = 1 | (1ULL << 6) | (1ULL << 12) | (1ULL << 20) | (1ULL << 28) | (1ULL << 36) | (16ULL << 48);
    auto result = ConvertRaw(153, {0, 0, 16 | (16ULL << 16), config}, state);
    EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::PARTIAL_COVERAGE);
}

TEST(CubeMemory, Img2ColRegisterFieldsDecodeExplicitly)
{
    for (uint32_t id : {386u, 387u}) {
        AclsanRawTraceRecord raw{};
        raw.instrId = id;
        raw.args[0] = 0x78563412ABCD1234ULL;
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
        ASSERT_TRUE(decoded);
        const auto& fields = std::get<FmatrixParamField>(decoded->params);
        EXPECT_EQ(fields.instrId, id);
        EXPECT_EQ(fields.width, 0x1234u);
        EXPECT_EQ(fields.height, 0xABCDu);
        EXPECT_EQ(fields.paddingLeft, 0x12u);
        EXPECT_EQ(fields.paddingRight, 0x34u);
        EXPECT_EQ(fields.paddingTop, 0x56u);
        EXPECT_EQ(fields.paddingBottom, 0x78u);
    }
    for (uint32_t id : {390u, 391u}) {
        AclsanRawTraceRecord raw{};
        raw.instrId = id;
        // Reserved bits are deliberately nonzero and must not leak into fields.
        raw.args[0] = 0xEFABCD12FF345678ULL;
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
        ASSERT_TRUE(decoded);
        const auto& fields = std::get<L3dRptParamField>(decoded->params);
        EXPECT_EQ(fields.instrId, id);
        EXPECT_EQ(fields.repeatStride, 0x5678u);
        EXPECT_EQ(fields.repeatTimes, 0x34u);
        EXPECT_TRUE(fields.repeatAlongK);
        EXPECT_EQ(fields.dstStride, 0x12u);
        EXPECT_EQ(fields.dstOffset, 0xABu);
    }
}

TEST(CubeMemory, L1ToBtAndFbSourceFootprintsAndBounds)
{
    for (uint32_t id : {167u, 423u, 424u, 425u, 426u}) {
        SCOPED_TRACE(id);
        EXPECT_TRUE(IsDefinedInstructionId(id));
        const uint64_t bytes = id == 167 ? 128 : 64;
        auto r = ConvertRaw(id, {0, 0x80000 - bytes, (1ULL << 4) | (2ULL << 16), 0});
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 1u);
        EXPECT_EQ(r.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
        EXPECT_EQ(r.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        ASSERT_EQ(r.data[0].layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
        EXPECT_EQ(r.data[0].layout.range.bytes, bytes);
        npucheck::Memcheck checker;
        checker.QueueDeviceMemoryAccess(r.data[0]);
        EXPECT_TRUE(checker.OnSynchronization().empty());
        r.data[0].address += 32;
        checker.QueueDeviceMemoryAccess(r.data[0]);
        EXPECT_EQ(checker.OnSynchronization().size(), 1u);
    }
}

TEST(CubeMemory, L1ToBtAndFbSourceGapVersusAbsoluteStride)
{
    for (uint32_t id : {167u, 423u, 424u, 425u, 426u}) {
        SCOPED_TRACE(id);
        const uint64_t bytes = id == 167 ? 128 : 64;
        const uint64_t strideField = id == 167 ? 6 : 2;
        const uint64_t stride = id == 167 ? 192 : 128;
        const auto r =
            ConvertRaw(id, {0, 0x80000 - bytes, (3ULL << 4) | (2ULL << 16) | (strideField << 32) | (10ULL << 48), 0});
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 1u);
        npucheck::Memcheck checker;
        checker.QueueDeviceMemoryAccess(r.data[0]);
        const auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 2u);
        EXPECT_EQ(reports[0].access.address, 0x80000 - bytes + stride);
        EXPECT_EQ(reports[1].access.address, 0x80000 - bytes + 2 * stride);
    }
    const auto repeated = ConvertRaw(167, {0, 0x7ff80, (3ULL << 4) | (2ULL << 16), 0});
    ASSERT_EQ(repeated.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(repeated.data.size(), 1u);
    EXPECT_EQ(repeated.data[0].layout.range.bytes, 128u);
}

TEST(CubeMemory, L1ToBtAndFbZeroTransfersAndOverflow)
{
    for (uint32_t id : {167u, 423u, 424u, 425u, 426u}) {
        SCOPED_TRACE(id);
        for (uint64_t config : {2ULL << 16, 1ULL << 4}) {
            const auto r = ConvertRaw(id, {0, 0, config, 0});
            EXPECT_EQ(r.status, MemoryCbdataStatus::NO_ACCESS);
            EXPECT_TRUE(r.data.empty());
        }
        const auto r = ConvertRaw(id, {0, UINT64_MAX - 31, (1ULL << 4) | (2ULL << 16), 0});
        EXPECT_EQ(r.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
        EXPECT_TRUE(r.data.empty());
    }
}

TEST(CubeMemory, L1ToBtAndFbReferenceConfigurationValidation)
{
    for (uint32_t id : {423u, 424u}) {
        for (uint64_t config : {(1ULL << 4) | (1ULL << 16), (1ULL << 4) | (2ULL << 16) | 8ULL}) {
            EXPECT_EQ(ConvertRaw(id, {0, 0, config, 0}).status, MemoryCbdataStatus::PARTIAL_COVERAGE);
        }
    }
    for (uint32_t id : {423u, 424u, 425u, 426u}) {
        EXPECT_EQ(
            ConvertRaw(id, {0, 0, (1ULL << 4) | (2ULL << 16) | (1ULL << 48), 0}).status,
            MemoryCbdataStatus::PARTIAL_COVERAGE);
    }
    for (uint32_t id : {425u, 426u}) {
        const auto r = ConvertRaw(id, {0, 0, (1ULL << 4) | (1ULL << 16) | 8ULL, 0});
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 1u);
        EXPECT_EQ(r.data[0].layout.range.bytes, 32u);
    }
    EXPECT_EQ(ConvertRaw(167, {0, 0, (1ULL << 4) | (1ULL << 16), 0}).status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    EXPECT_EQ(
        ConvertRaw(167, {0, 0, (1ULL << 4) | (2ULL << 16) | (1ULL << 48), 0}).status,
        MemoryCbdataStatus::PARTIAL_COVERAGE);
    EXPECT_EQ(
        ConvertRaw(167, {2ULL << 16, 0, (1ULL << 4) | (2ULL << 16), 0}).status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    const auto fb = ConvertRaw(167, {1ULL << 16, 0, (1ULL << 4) | (1ULL << 16) | (1ULL << 48), 0});
    ASSERT_EQ(fb.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(fb.data.size(), 1u);
    EXPECT_EQ(fb.data[0].layout.range.bytes, 64u);
}

TEST(CubeMemory, L1ToBtAndFbCallbackToMemcheck)
{
    for (uint32_t id : {167u, 423u, 424u, 425u, 426u}) {
        SCOPED_TRACE(id);
        ParsedTraceRecord parsed{};
        parsed.record.instrId = id;
        parsed.record.pipeline = id == 167 ? ACLSAN_DEVICE_PIPE_FIXPIPE : ACLSAN_DEVICE_PIPE_MTE1;
        parsed.record.pc = 0x1234;
        parsed.record.args[1] = 0x80000;
        parsed.record.args[2] = (1ULL << 4) | (2ULL << 16);
        parsed.instrExecId = 23;
        parsed.launchId = 5;
        parsed.phyCoreId = 7;
        parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
        ASSERT_TRUE(decoded);
        const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded);
        ASSERT_TRUE(callback);
        const auto& accesses = std::get<DeviceMemoryAccessDataList>(*callback);
        ASSERT_EQ(accesses.size(), 1u);
        EXPECT_EQ(accesses[0].accessCount, 1u);
        EXPECT_EQ(accesses[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        npucheck::Memcheck checker;
        checker.QueueDeviceMemoryAccess(accesses[0]);
        const auto reports = checker.OnSynchronization();
        ASSERT_EQ(reports.size(), 1u);
        EXPECT_EQ(reports[0].common.exec.pc, 0x1234u);
        EXPECT_EQ(reports[0].access.memorySpace, npucheck::NpuCheckReportMemorySpace::L1);
        EXPECT_EQ(reports[0].access.address, 0x80000u);
    }
}
