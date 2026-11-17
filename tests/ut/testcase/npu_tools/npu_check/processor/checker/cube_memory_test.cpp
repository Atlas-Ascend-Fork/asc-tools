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
#include "checker/initcheck.h"
#include "aclsan_device_data.h"
#include "aclsan_active_probe_plan.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"
#include <algorithm>
#include "checker/racecheck/types.h"
#include "aclsan_register_dependency.h"
#include <set>

namespace {
using namespace aclsan;

TEST(CubeMemoryInitcheck, DecodedFp16LoadsAndMmadCheckBothOperandSpaces)
{
    for (bool testB : {false, true}) {
        for (uint32_t initializedFractals : {0U, 1U, 2U}) {
            SCOPED_TRACE(testB);
            SCOPED_TRACE(initializedFractals);
            npucheck::Initcheck checker;
            npucheck::CheckerReportList reports;
            AclsanLaunchData launch{};
            launch.launchId = 17;
            ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
            uint64_t instruction = 1;
            const auto emit = [&](uint32_t id, std::array<uint64_t, 4> args, uint32_t pipe) {
                ParsedTraceRecord parsed{};
                parsed.record.instrId = id;
                parsed.record.pc = 0x1234;
                parsed.record.pipeline = pipe;
                std::copy(args.begin(), args.end(), parsed.record.args);
                parsed.launchId = 17;
                parsed.instrExecId = ++instruction;
                parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
                const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
                ASSERT_TRUE(decoded);
                const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded);
                ASSERT_TRUE(callback);
                if (const auto* accesses = std::get_if<DeviceMemoryAccessDataList>(&*callback)) {
                    for (const auto& access : *accesses)
                        ASSERT_TRUE(checker.OnCallback(
                            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
                } else {
                    const auto& flag = std::get<AclsanDeviceSyncData>(*callback);
                    ASSERT_TRUE(checker.OnCallback(
                        ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &flag, reports));
                }
            };
            for (uint32_t loadId : {142U, 145U}) {
                const uint32_t count = ((loadId == 145U) == testB) ? initializedFractals : 2U;
                if (count)
                    emit(
                        loadId, {0, 0, (1ULL << 32) | (uint64_t(count) << 40), 1 | (1ULL << 16)},
                        ACLSAN_DEVICE_PIPE_MTE1);
            }
            emit(440, {ACLSAN_DEVICE_PIPE_MTE1, ACLSAN_DEVICE_PIPE_MATRIX, 3, 0}, ACLSAN_DEVICE_PIPE_SCALAR);
            emit(442, {ACLSAN_DEVICE_PIPE_MTE1, ACLSAN_DEVICE_PIPE_MATRIX, 3, 0}, ACLSAN_DEVICE_PIPE_SCALAR);
            emit(401, {0, 0, 0, 16 | (32ULL << 12) | (16ULL << 24) | (1ULL << 63)}, ACLSAN_DEVICE_PIPE_MATRIX);
            AclsanSynchronizeData sync{};
            ASSERT_TRUE(checker.OnCallback(
                ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
            EXPECT_TRUE(checker.AnalysisComplete());
            // L1 source diagnostics remain the existing checker's responsibility.
            npucheck::CheckerReportList l0Reports;
            for (const auto& record : reports) {
                const auto& report = std::get<npucheck::NpuCheckInitcheckReport>(record);
                if (report.access.memorySpace == npucheck::NpuCheckReportMemorySpace::L0_A ||
                    report.access.memorySpace == npucheck::NpuCheckReportMemorySpace::L0_B)
                    l0Reports.push_back(record);
            }
            if (initializedFractals == 2) {
                EXPECT_TRUE(l0Reports.empty());
                continue;
            }
            ASSERT_EQ(l0Reports.size(), 1U);
            const auto& report = std::get<npucheck::NpuCheckInitcheckReport>(l0Reports.front());
            EXPECT_EQ(
                report.access.memorySpace,
                testB ? npucheck::NpuCheckReportMemorySpace::L0_B : npucheck::NpuCheckReportMemorySpace::L0_A);
            EXPECT_EQ(report.firstUninitAddress, initializedFractals * 512U);
            EXPECT_EQ(report.initializedBytes, initializedFractals * 512U);
            EXPECT_EQ(report.uninitBytes, (2U - initializedFractals) * 512U);
            EXPECT_EQ(report.access.accessBytes, 1024U);
            EXPECT_EQ(report.common.exec.pc, 0x1234U);
        }
    }
}

TEST(CubeMemoryInitcheck, MxLoadsWithoutDecodedDestinationsPreserveIncompleteCoverage)
{
    for (uint32_t id : {151U, 152U}) {
        ParsedTraceRecord parsed{};
        parsed.record.instrId = id;
        parsed.record.args[2] = (1ULL << 32) | (1ULL << 40);
        parsed.record.args[3] = 1;
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
        ASSERT_TRUE(decoded);
        const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded);
        ASSERT_TRUE(callback);
        const auto& accesses = std::get<DeviceMemoryAccessDataList>(*callback);
        ASSERT_GE(accesses.size(), 2U);
        EXPECT_EQ(accesses.front().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
        EXPECT_EQ(accesses.back().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN);
        EXPECT_NE(accesses.back().header.flags & ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED, 0U);
    }
}

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
                MemoryCbdataContext context{};
                context.physicalCoreSlotCount = 108;
                context.executor.blockType =
                    id == 173 ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
                context.executor.phyCoreId = id == 173 ? 18 : 0;
                return MemoryFieldToCbdataConverter{context, state}.Convert(MemoryInstructionField{f});
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

TEST(CubeMemory, PaddingSelectorWithoutPaddingPreservesL1Write)
{
    // CANN 9.3 DataCopy sets bit 58 even when both padding counts are zero.
    for (uint32_t id : {74u, 75u, 76u}) {
        SCOPED_TRACE(id);
        auto result = ConvertRaw(
            id, {0x400, 0x2000, (1ULL << 4) | (512ULL << 25) | (1ULL << 58), 512ULL | (512ULL << 40)},
            SingleGmToL1Iteration());
        ASSERT_EQ(result.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(result.data.size(), 2u);
        EXPECT_EQ(result.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
        const auto& write = result.data[1];
        EXPECT_EQ(write.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
        EXPECT_EQ(write.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
        EXPECT_EQ(write.address, 0x400u);
        ASSERT_EQ(write.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
        EXPECT_EQ(write.layout.range.bytes, 512u);
        EXPECT_EQ(write.accessCount, 2u);
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
    EXPECT_EQ(r.data[0].memoryFlags, 0U);
    EXPECT_EQ(r.data[0].validNz.rows, 17U);
    EXPECT_EQ(r.data[0].validNz.rowDataBytes, 36U);
    for (auto d : r.data) {
        checker.QueueDeviceMemoryAccess(d);
    }
    EXPECT_EQ(checker.OnSynchronization().size(), 2u);
}

TEST(CubeMemoryInitcheck, MmadPaddingDoesNotHideUninitializedValidOperands)
{
    using npucheck::ReportSeverity;
    struct Case {
        InstructionId id;
        uint64_t m, k, n;
        bool disableGemv;
    };
    for (const auto& c : {
             Case{InstructionId::MadS8, 30, 70, 64, true}, // MMAD sample, scenario 1 dimensions.
             Case{InstructionId::MadS8, 32, 64, 64, true},
             Case{InstructionId::MadBf16F32, 32, 70, 48, true},
             Case{InstructionId::MadF32F32, 32, 72, 40, true},
             Case{InstructionId::MadF16F32, 1, 256, 32, false},
             Case{InstructionId::MadF16F32, 1, 256, 32, true},
         }) {
        SCOPED_TRACE(testing::Message() << c.m << "x" << c.k << "x" << c.n << " disableGemv=" << c.disableGemv);
        ParsedTraceRecord parsed{};
        parsed.record.instrId = static_cast<uint32_t>(c.id);
        parsed.record.pipeline = ACLSAN_DEVICE_PIPE_MATRIX;
        parsed.record.args[3] = c.m | (c.k << 12) | (c.n << 24) | (uint64_t(c.disableGemv) << 61);
        parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        parsed.launchId = 17;
        parsed.instrExecId = 1;
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
        ASSERT_TRUE(decoded);
        const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded);
        ASSERT_TRUE(callback);

        npucheck::Initcheck checker;
        npucheck::CheckerReportList reports;
        AclsanLaunchData launch{};
        launch.launchId = parsed.launchId;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
        for (const auto& access : std::get<DeviceMemoryAccessDataList>(*callback)) {
            ASSERT_TRUE(checker.OnCallback(
                ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
        }
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
        ASSERT_EQ(reports.size(), 3U); // Uninitialized A, B, and L0C accumulator.
        for (const auto& record : reports) {
            const auto& report = std::get<npucheck::NpuCheckInitcheckReport>(record);
            EXPECT_EQ(report.common.severity, ReportSeverity::ERROR);
        }
        EXPECT_TRUE(checker.HasErrors());
        EXPECT_EQ(checker.Stats().warnings, 0U);
    }
}

// Feed writes and the decoded read on one pipeline, so visibility is explicit.
// Offsets are supplied independently of the converter's logical metadata.
npucheck::CheckerReportList CheckReadAfterWrites(
    AclsanDeviceMemoryAccessData read, const std::vector<std::pair<uint64_t, uint64_t>>& writes)
{
    npucheck::Initcheck checker;
    npucheck::CheckerReportList reports;
    read.header.version = ACLSAN_API_VERSION;
    read.header.size = sizeof(read);
    read.header.launchId = 71;
    read.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
    read.accessIndex = 0;
    read.accessCount = 1;
    read.regDependencyMask0 = read.regDependencyMask1 = 0;
    AclsanLaunchData launch{};
    launch.launchId = read.header.launchId;
    EXPECT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
    uint64_t sequence = 0;
    for (const auto& range : writes) {
        auto write = read;
        write.header.instrExecId = ++sequence;
        write.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        write.address = read.address + range.first;
        write.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        write.layout = {};
        write.layout.range.bytes = range.second;
        write.validNz = {};
        write.memoryFlags = 0;
        EXPECT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &write, reports));
    }
    read.header.instrExecId = ++sequence;
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &read, reports));
    AclsanSynchronizeData sync{};
    EXPECT_TRUE(
        checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
    EXPECT_TRUE(checker.AnalysisComplete()) << checker.Summary();
    return reports;
}

TEST(CubeMemoryInitcheck, MmadRequiresValidBytesInEachNzOperand)
{
    // M=17, N=21, K=19, fp16. Distinct axes cross a tile boundary.
    const auto converted =
        ConvertRaw(uint32_t(InstructionId::MadF16F32), {4096, 1024, 2048, 17 | (19ULL << 12) | (21ULL << 24)});
    ASSERT_EQ(converted.status, MemoryCbdataStatus::SUCCESS);
    for (const auto& read : converted.data) {
        if (read.accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
            continue;
        }
        const bool c = read.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0C;
        const uint64_t rowBytes = c ? 64 : 32;
        const uint64_t validBytes = c ? 84 : 38;
        const uint64_t rows = read.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0B ? 21 : 17;
        std::vector<std::pair<uint64_t, uint64_t>> writes;
        for (uint64_t block = 0; block < 2; ++block)
            for (uint64_t row = 0; row < rows; ++row)
                writes.emplace_back(
                    block * 32 * rowBytes + row * rowBytes, std::min(rowBytes, validBytes - block * rowBytes));
        for (bool omitValidByte : {false, true}) {
            auto initialized = writes;
            if (omitValidByte) {
                --initialized.back().second; // Last valid byte, not the first uninitialized byte.
            }
            const auto reports = CheckReadAfterWrites(read, initialized);
            ASSERT_EQ(reports.size(), 1U);
            const auto& report = std::get<npucheck::NpuCheckInitcheckReport>(reports.front());
            EXPECT_EQ(
                report.common.severity,
                omitValidByte ? npucheck::ReportSeverity::ERROR : npucheck::ReportSeverity::WARNING);
            EXPECT_EQ(report.uninitBytes, 64 * rowBytes - rows * validBytes + uint64_t(omitValidByte));
        }
        EXPECT_TRUE(CheckReadAfterWrites(read, {{0, 64 * rowBytes}}).empty());
    }
}

TEST(CubeMemoryInitcheck, InvalidPaddingMetadataNeverDowngradesMissingValidData)
{
    const auto converted =
        ConvertRaw(uint32_t(InstructionId::MadF16F32), {0, 0, 0, 17 | (19ULL << 12) | (21ULL << 24)});
    ASSERT_EQ(converted.status, MemoryCbdataStatus::SUCCESS);
    for (auto invalid :
         {AclsanMemValidNzLayout{0, 38, 1024, 32, 0}, AclsanMemValidNzLayout{17, 0, 1024, 32, 0},
          AclsanMemValidNzLayout{17, 38, 16, 32, 0}, AclsanMemValidNzLayout{17, 38, 1024, 32, 1}}) {
        auto read = converted.data.front();
        read.validNz = invalid;
        const auto reports = CheckReadAfterWrites(read, {});
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(
            std::get<npucheck::NpuCheckInitcheckReport>(reports.front()).common.severity,
            npucheck::ReportSeverity::ERROR);
    }
}

TEST(CubeMemoryInitcheck, MxKPaddingRemainsRequiredEvenWhenMNeedsPadding)
{
    const auto converted = ConvertRaw(
        uint32_t(InstructionId::MadMxE4m3E4m3), {0, 0, 0, 30 | (128ULL << 12) | (32ULL << 24) | (1ULL << 63)});
    ASSERT_EQ(converted.status, MemoryCbdataStatus::SUCCESS);
    const auto& read = converted.data.front();
    for (uint64_t initializedK : {96U, 128U}) {
        std::vector<std::pair<uint64_t, uint64_t>> writes;
        for (uint64_t block = 0; block < initializedK / 32; ++block)
            writes.emplace_back(block * 1024, 30 * 32);
        const auto reports = CheckReadAfterWrites(read, writes);
        ASSERT_EQ(reports.size(), 1U);
        EXPECT_EQ(
            std::get<npucheck::NpuCheckInitcheckReport>(reports.front()).common.severity,
            initializedK == 128 ? npucheck::ReportSeverity::WARNING : npucheck::ReportSeverity::ERROR);
    }
}

TEST(CubeMemoryInitcheck, ChannelSplitOnlyDowngradesMissingInvalidHalfColumns)
{
    for (uint64_t sourceRows : {16U, 32U}) {
        SCOPED_TRACE(sourceRows);
        const auto converted = ConvertRaw(
            uint32_t(InstructionId::FixL0cToOutF32),
            {0x100000, 4096, (24ULL << 4) | (16ULL << 16) | (256ULL << 32), sourceRows | (1ULL << 42)});
        ASSERT_EQ(converted.status, MemoryCbdataStatus::SUCCESS);
        const auto read = *std::find_if(converted.data.begin(), converted.data.end(), [](const auto& d) {
            return d.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0C;
        });
        for (bool omitValidByte : {false, true}) {
            std::vector<std::pair<uint64_t, uint64_t>> writes{{0, 1024}};
            for (uint64_t row = 0; row < 16; ++row)
                writes.emplace_back(sourceRows * 64 + row * 64, 32);
            if (omitValidByte) {
                --writes.back().second;
            }
            const auto reports = CheckReadAfterWrites(read, writes);
            ASSERT_EQ(reports.size(), 1U);
            const auto& report = std::get<npucheck::NpuCheckInitcheckReport>(reports.front());
            EXPECT_EQ(
                report.common.severity,
                omitValidByte ? npucheck::ReportSeverity::ERROR : npucheck::ReportSeverity::WARNING);
            EXPECT_EQ(report.uninitBytes, 512 + uint64_t(omitValidByte));
            std::string rendered;
            ASSERT_EQ(
                npucheck::RenderNpuCheckReportRecord(npucheck::NpuCheckReportRecord::From(report), {}, &rendered),
                npucheck::ReportRenderStatus::SUCCESS);
            EXPECT_EQ(rendered.find("ignored hardware padding") != std::string::npos, !omitValidByte);
        }
    }
}

TEST(CubeMemoryInitcheck, Mte1WarnsForL1PaddingAndErrorsForReadsBeyondThePaddedLayout)
{
    struct Case {
        uint32_t writeId, readId;
        uint64_t rows, columns, paddedRows, elementBytes, readOffset, config0, config1, missing;
        npucheck::ReportSeverity severity;
        uint16_t matrices = 1;
        bool reuse = false;
    };
    using npucheck::ReportSeverity;
    for (bool managed : {false, true}) {
        for (const auto& c : {
                 Case{
                     77, 143, 30, 70, 32, 1, 0, (2ULL << 32) | (3ULL << 40), 2 | (2ULL << 16), 192,
                     ReportSeverity::WARNING},
                 Case{
                     77, 137, 70, 40, 96, 1, 2048, (2ULL << 16) | (6ULL << 24) | (1ULL << 44), 0, 1664,
                     ReportSeverity::WARNING},
                 Case{79, 424, 1, 40, 1, 4, 0, (1ULL << 4) | (6ULL << 16), 0, 32, ReportSeverity::WARNING},
                 Case{
                     77, 143, 30, 70, 32, 1, 0, (2ULL << 32) | (3ULL << 40), 2 | (2ULL << 16), 192,
                     ReportSeverity::ERROR, 1, true},
                 Case{
                     77, 143, 30, 70, 48, 1, 0, (2ULL << 32) | (3ULL << 40), 3 | (2ULL << 16), 192,
                     ReportSeverity::WARNING}, // Column gaps are skipped by the read.
                 Case{
                     77, 143, 30, 70, 48, 1, 0, (3ULL << 32) | (3ULL << 40), 3 | (3ULL << 16), 1728,
                     ReportSeverity::ERROR}, // Reading the extra gap is not padding.
                 Case{
                     78, 142, 30, 70, 32, 2, 0, (2ULL << 32) | (5ULL << 40), 2 | (2ULL << 16), 320,
                     ReportSeverity::WARNING},
                 Case{
                     78, 145, 40, 70, 48, 2, 0, (3ULL << 32) | (5ULL << 40), 3 | (3ULL << 16), 1280,
                     ReportSeverity::WARNING},
                 Case{
                     79, 148, 40, 70, 48, 4, 0, (3ULL << 32) | (9ULL << 40), 3 | (3ULL << 16), 2304,
                     ReportSeverity::WARNING},
                 Case{
                     77, 143, 30, 70, 32, 1, 3072, (2ULL << 32) | (3ULL << 40), 2 | (2ULL << 16), 192,
                     ReportSeverity::WARNING, 2},
                 Case{79, 424, 1, 40, 1, 4, 192, (1ULL << 4) | (6ULL << 16), 0, 32, ReportSeverity::WARNING, 2},
                 Case{
                     77, 143, 30, 70, 32, 1, 0, (2ULL << 32) | (4ULL << 40), 2 | (2ULL << 16), 1216,
                     ReportSeverity::ERROR},
             }) {
            SCOPED_TRACE(c.readId);
            SCOPED_TRACE(managed);
            constexpr uint64_t GM = 0x100000;
            auto analysis = std::make_shared<npucheck::MemoryAnalysis>(managed);
            npucheck::Initcheck checker(analysis);
            const auto send = [&](auto domain, auto id, const auto* data, auto& output) {
                if (managed && !analysis->BeginCallback(domain, id, data)) {
                    return false;
                }
                const bool ok = checker.OnCallback(domain, id, data, output);
                if (managed) {
                    analysis->EndCallback();
                }
                return ok;
            };
            npucheck::CheckerReportList reports;
            AclsanResourceData allocation{};
            allocation.common = {ACLSAN_API_VERSION, sizeof(allocation), nullptr, 0, 0};
            allocation.ptr = reinterpret_cast<void*>(GM);
            allocation.bytes = 16384;
            allocation.resourceId = 1;
            allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
            ASSERT_TRUE(send(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &allocation, reports));
            AclsanMemoryWriteData hostWrite{};
            hostWrite.common.version = ACLSAN_API_VERSION;
            hostWrite.common.size = sizeof(hostWrite);
            hostWrite.destination = allocation.ptr;
            hostWrite.bytes = allocation.bytes;
            ASSERT_TRUE(send(ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END, &hostWrite, reports));
            AclsanLaunchData launch{};
            launch.common = {ACLSAN_API_VERSION, sizeof(launch), nullptr, 0, 0};
            launch.launchId = 17;
            ASSERT_TRUE(send(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
            MemoryRegisterState state{};
            const uint16_t matrixStride =
                c.rows == 1 && c.elementBytes == 4 ? 6 : c.paddedRows * ((c.columns * c.elementBytes + 31) / 32);
            state.mte2Nz = Mte2NzParamField{c.matrices, 1, static_cast<uint16_t>(c.paddedRows), matrixStride};
            AclsanDeviceRegisterStateData nz{};
            nz.header.version = ACLSAN_API_VERSION;
            nz.header.size = sizeof(nz);
            nz.header.launchId = launch.launchId;
            nz.header.instrExecId = 1;
            nz.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
            nz.regId = ACLSAN_DEVICE_REGISTER_MTE2_NZ;
            ASSERT_TRUE(send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE, &nz, reports));
            uint64_t instruction = 1;
            const auto emit = [&](uint32_t id, uint32_t pipe, std::array<uint64_t, 4> args) {
                ParsedTraceRecord parsed{};
                parsed.record.instrId = id;
                parsed.record.pipeline = pipe;
                std::copy(args.begin(), args.end(), parsed.record.args);
                parsed.launchId = launch.launchId;
                parsed.instrExecId = ++instruction;
                parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
                const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
                ASSERT_TRUE(decoded);
                const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded, state);
                ASSERT_TRUE(callback);
                if (const auto* accesses = std::get_if<DeviceMemoryAccessDataList>(&*callback)) {
                    for (const auto& access : *accesses)
                        ASSERT_TRUE(send(
                            ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
                } else {
                    const auto& sync = std::get<AclsanDeviceSyncData>(*callback);
                    ASSERT_TRUE(send(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &sync, reports));
                }
            };
            emit(
                c.writeId, ACLSAN_DEVICE_PIPE_MTE2,
                {0, GM, (c.rows << 48) | (c.columns * c.elementBytes << 4), c.columns});
            if (c.reuse) {
                emit(c.writeId, ACLSAN_DEVICE_PIPE_MTE2, {0, GM, (1ULL << 48) | (32ULL << 4), 32});
            }
            emit(440, ACLSAN_DEVICE_PIPE_SCALAR, {ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE1, 0, 0});
            emit(442, ACLSAN_DEVICE_PIPE_SCALAR, {ACLSAN_DEVICE_PIPE_MTE2, ACLSAN_DEVICE_PIPE_MTE1, 0, 0});
            emit(c.readId, ACLSAN_DEVICE_PIPE_MTE1, {0, c.readOffset, c.config0, c.config1});
            AclsanSynchronizeData sync{};
            sync.common = {ACLSAN_API_VERSION, sizeof(sync), nullptr, 0, 0};
            ASSERT_TRUE(send(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
            ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
            ASSERT_FALSE(reports.empty());
            uint64_t missing = 0;
            for (const auto& record : reports) {
                const auto& report = std::get<npucheck::NpuCheckInitcheckReport>(record);
                EXPECT_EQ(report.access.memorySpace, npucheck::NpuCheckReportMemorySpace::L1);
                missing += report.uninitBytes;
                if (c.severity == ReportSeverity::WARNING) {
                    EXPECT_EQ(report.common.severity, ReportSeverity::WARNING);
                }
            }
            EXPECT_EQ(missing, c.missing);
            EXPECT_EQ(checker.HasErrors(), c.severity == ReportSeverity::ERROR);
        }
    }
}

TEST(CubeMemoryInitcheck, ChannelSplitDoesNotHideEntirelyUninitializedSource)
{
    for (const uint64_t n : {16U, 24U, 32U}) {
        SCOPED_TRACE(n);
        constexpr uint64_t GM_ADDRESS = 0x100000;
        ParsedTraceRecord parsed{};
        parsed.record.instrId = static_cast<uint32_t>(InstructionId::FixL0cToOutF32);
        parsed.record.args[0] = GM_ADDRESS;
        parsed.record.args[2] = (n << 4) | (16ULL << 16) | (256ULL << 32);
        parsed.record.args[3] = 16 | (1ULL << 42); // F32 Channel Split into Nz.
        parsed.record.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
        parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        parsed.launchId = 17;
        parsed.instrExecId = 1;
        MemoryRegisterState state{};
        state.ctrl = CtrlRegisterParamField{0};
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
        ASSERT_TRUE(decoded);
        const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded, state);
        ASSERT_TRUE(callback);
        const auto& accesses = std::get<DeviceMemoryAccessDataList>(*callback);
        ASSERT_EQ(accesses.size(), 2U);

        npucheck::Initcheck checker;
        npucheck::CheckerReportList reports;
        AclsanResourceData allocation{};
        allocation.ptr = reinterpret_cast<void*>(GM_ADDRESS);
        allocation.bytes = 16384;
        allocation.resourceId = 1;
        allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &allocation, reports));
        AclsanLaunchData launch{};
        launch.launchId = parsed.launchId;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch, reports));
        AclsanDeviceRegisterStateData ctrl{};
        ctrl.header = accesses.front().header;
        ctrl.header.size = sizeof(ctrl);
        ctrl.regId = ACLSAN_DEVICE_REGISTER_CTRL;
        ASSERT_TRUE(checker.OnCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE, &ctrl, reports));
        for (const auto& access : accesses) {
            ASSERT_TRUE(checker.OnCallback(
                ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &access, reports));
        }
        AclsanSynchronizeData sync{};
        ASSERT_TRUE(
            checker.OnCallback(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync, reports));
        ASSERT_TRUE(checker.AnalysisComplete()) << checker.Summary();
        ASSERT_EQ(reports.size(), 1U);
        const auto& report = std::get<npucheck::NpuCheckInitcheckReport>(reports.front());
        const bool warning = false; // Valid L0C data is also entirely uninitialized.
        EXPECT_EQ(report.access.memorySpace, npucheck::NpuCheckReportMemorySpace::L0_C);
        EXPECT_EQ(report.access.accessBytes, n == 16 ? 1024U : 2048U);
        EXPECT_EQ(
            report.common.severity, warning ? npucheck::ReportSeverity::WARNING : npucheck::ReportSeverity::ERROR);
        EXPECT_EQ(checker.HasErrors(), !warning);
        EXPECT_EQ(checker.Stats().warnings, warning ? 1U : 0U);
    }
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
        d.memoryOwner = d.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ? AclsanCoreInfo{} : d.header.executor;
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
            EXPECT_EQ(r.data.size(), 2u);
            EXPECT_EQ(r.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
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

TEST(CubeMemory, L0LoadFailuresPreserveIncompleteCoverage)
{
    ParsedTraceRecord parsed{};
    parsed.record.instrId = 153;
    parsed.record.pipeline = ACLSAN_DEVICE_PIPE_MTE1;
    parsed.record.args[2] = 16 | (16ULL << 16);
    parsed.record.args[3] = 1 | (1ULL << 6) | (1ULL << 12) | (1ULL << 20) | (1ULL << 28) | (1ULL << 36) | (16ULL << 48);
    const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
    ASSERT_TRUE(decoded);
    const auto callback = TranslateDecodedTraceToCallbackData(parsed, *decoded);
    ASSERT_TRUE(callback);
    const auto& accesses = std::get<DeviceMemoryAccessDataList>(*callback);
    ASSERT_EQ(accesses.size(), 1U);
    EXPECT_EQ(accesses[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN);
    EXPECT_NE(accesses[0].header.flags & ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED, 0U);
    EXPECT_EQ(accesses[0].accessCount, 1U);
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

TEST(CubeMemory, Nz2DnMatchesDocumentedByteFootprintForAllDestinations)
{
    for (uint32_t id : {91U, 92U, 168U, 169U, 170U, 171U})
        for (uint32_t n : {7U, 16U, 30U, 32U})
            for (uint32_t c0 : {1U, 3U}) {
                SCOPED_TRACE(::testing::Message() << id << '/' << n << '/' << c0);
                MemoryRegisterState state{};
                state.ctrl = CtrlRegisterParamField{0};
                state.loop3 = Loop3ParamField{2, 240, 4096};
                state.channelPara = ChannelParaParamField{uint64_t(c0) << 48};
                const auto result = ConvertRaw(
                    id, {0x400, 0x800, (uint64_t(n) << 4) | (5ULL << 16) | (80ULL << 32), 80ULL | (1ULL << 62)}, state);
                ASSERT_EQ(result.status, MemoryCbdataStatus::SUCCESS);
                EXPECT_FALSE(result.requiresIncompleteEvent);
                std::set<uint64_t> actualSource, actualDestination, expectedSource, expectedDestination;
                for (const auto& a : result.data) {
                    std::vector<npucheck::race::Interval> ranges;
                    ASSERT_TRUE(npucheck::race::Normalize(a, 1U << 20U, ranges));
                    auto& bytes = a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0C ? actualSource : actualDestination;
                    for (auto range : ranges)
                        for (uint64_t byte = range.begin; byte < range.end; ++byte)
                            bytes.insert(byte);
                }
                for (uint64_t h = 0; h < 2; ++h)
                    for (uint64_t column = 0; column < n; ++column)
                        for (uint64_t row = 0; row < 5; ++row)
                            for (uint64_t byte = 0; byte < 4; ++byte) {
                                expectedSource.insert(
                                    0x800 + h * 64 * 240 + (column / 16) * 64 * 80 + (column % 16) * 4 + row * 64 * c0 +
                                    byte);
                                expectedDestination.insert(0x400 + h * 4 * 4096 + column * 4 * 80 + row * 4 + byte);
                            }
                EXPECT_EQ(actualSource, expectedSource);
                EXPECT_EQ(actualDestination, expectedDestination);
            }
}

TEST(CubeMemory, Nz2DnMissingOrIllegalChannelStateCannotClaimCompleteCoverage)
{
    for (uint32_t id : {91U, 168U, 170U})
        for (uint32_t mode : {0U, 1U, 2U, 3U}) {
            MemoryRegisterState state{};
            state.ctrl = CtrlRegisterParamField{0};
            state.loop3 = Loop3ParamField{1, 0, 0};
            if (mode != 0)
                state.channelPara = ChannelParaParamField{uint64_t(mode == 1 ? 0 : mode == 2 ? 2 : 1) << 48};
            const uint64_t flag = mode == 2 ? 3ULL << 32 : 0;
            const auto result = ConvertRaw(
                id,
                {0x400, mode == 3 ? UINT64_MAX - 3 : 0x800, (30ULL << 4) | (16ULL << 16) | (64ULL << 32),
                 80ULL | (1ULL << 62) | flag},
                state);
            EXPECT_NE(result.status, MemoryCbdataStatus::SUCCESS);
            EXPECT_NE(result.status, MemoryCbdataStatus::NO_ACCESS);
        }
}

TEST(CubeMemory, ChannelAndFpcDecodeKeepValuesAndRegisterStateIsolation)
{
    for (const auto item :
         {std::pair<uint32_t, uint32_t>{123, ACLSAN_DEVICE_REGISTER_CHANNEL_PARA}, {161, ACLSAN_DEVICE_REGISTER_FPC}}) {
        ParsedTraceRecord raw{};
        raw.record.instrId = item.first;
        raw.record.args[0] = UINT64_C(0xabcd000000000123);
        raw.launchId = 1;
        raw.instrExecId = 2;
        raw.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw.record);
        ASSERT_TRUE(decoded);
        const auto callback = MakeRegisterStateData(raw, *decoded);
        ASSERT_TRUE(callback);
        EXPECT_EQ(callback->regId, item.second);
        EXPECT_EQ(callback->value, raw.record.args[0]);
    }
    dav3510::Dav3510RegisterStateManager registers(1);
    const dav3510::Dav3510CoreKey first{ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 1, 2, 0};
    auto other = first;
    ++other.phyCoreId;
    registers.Update(first, ChannelParaParamField{3ULL << 48});
    ASSERT_TRUE(registers.Get(first));
    ASSERT_TRUE(registers.Get(first)->channelPara);
    EXPECT_EQ(registers.Get(first)->channelPara->value, 3ULL << 48);
    EXPECT_FALSE(registers.Get(other));
    registers.Reset();
    EXPECT_FALSE(registers.Get(first));
}
