/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <gtest/gtest.h>
#include "aclsan_memory_cbdata.h"
#include "aclsan_active_probe_plan.h"
#include "aclsan_trace_buffer.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "device_instr/common/instruction_id.h"
#include "dbi/trace_buffer_abi.h"
#include <algorithm>
#include <cstring>

namespace {
using namespace aclsan;

MemoryRegisterState UbDmaState(DmaLoopDirection direction)
{
    MemoryRegisterState state{};
    state.ctrl = CtrlRegisterParamField{0};
    state.dmaLoopSizes[static_cast<size_t>(direction)] = DmaLoopSizeParamField{direction, 1, 1};
    return state;
}

MemoryCbdataResult ConvertRawWithContext(
    uint32_t id, std::array<uint64_t, 4> args, MemoryCbdataContext context, MemoryRegisterState state = {})
{
    AclsanRawTraceRecord raw{};
    raw.instrId = id;
    std::copy(args.begin(), args.end(), raw.args);
    const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
    if (!decoded) {
        return {MemoryCbdataStatus::INVALID_FIELD, {}};
    }
    context.instructionId = id;
    context.launchId = 9;
    context.instrExecId = 7;
    return std::visit(
        [&](const auto& f) -> MemoryCbdataResult {
            if constexpr (std::is_constructible_v<MemoryInstructionField, std::decay_t<decltype(f)>>) {
                return MemoryFieldToCbdataConverter{context, state}.Convert(MemoryInstructionField{f});
            }
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        },
        decoded->params);
}

MemoryCbdataResult ConvertUbRaw(uint32_t id, std::array<uint64_t, 4> args, MemoryRegisterState state = {})
{
    MemoryCbdataContext context{};
    context.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    context.coreId = 4;
    return ConvertRawWithContext(id, args, context, state);
}

TEST(UbMemory, GmToUbVariantsWritePaddedBytesWithIndependentStride)
{
    for (uint32_t id : {84u, 85u, 86u}) {
        SCOPED_TRACE(id);
        const uint32_t elementBytes = 1u << (id - 84);
        const auto r = ConvertUbRaw(
            id, {0x100, 0x100000, (3ULL << 4) | (20ULL << 25) | (2ULL << 46) | (3ULL << 52), 64 | (128ULL << 40)},
            UbDmaState(DmaLoopDirection::GM_TO_UBUF));
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        const auto& ub = r.data[1];
        EXPECT_EQ(ub.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        EXPECT_EQ(ub.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
        EXPECT_EQ(ub.address, 0x100u);
        EXPECT_EQ(ub.layoutKind, ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
        EXPECT_EQ(ub.layout.blockRepeat.blockSize, (20 + 5 * elementBytes + 31) / 32 * 32);
        EXPECT_EQ(ub.layout.blockRepeat.repeatTimes, 3u);
        EXPECT_EQ(ub.layout.blockRepeat.repeatStride, 128);
        EXPECT_EQ(r.data[0].layout.blockRepeat.blockSize, 20u);
        for (size_t i = 0; i < r.data.size(); ++i) {
            EXPECT_EQ(r.data[i].accessIndex, i);
            EXPECT_EQ(r.data[i].accessCount, 2u);
            EXPECT_EQ(r.data[i].header.phyCoreId, 4u);
            EXPECT_EQ(r.data[i].header.instrExecId, 7u);
        }
    }
}

TEST(UbMemory, UbToGmReadsSourceStrideAndAtomicAppliesOnlyToGmWrite)
{
    auto state = UbDmaState(DmaLoopDirection::UBUF_TO_GM);
    state.ctrl = CtrlRegisterParamField{1ULL << 6};
    const auto r = ConvertUbRaw(83, {0x100000, 0x100, (2ULL << 4) | (20ULL << 25), 64 | (96ULL << 40)}, state);
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    const auto& ub = r.data[1];
    EXPECT_EQ(ub.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(ub.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(ub.layout.blockRepeat.blockSize, 32u);
    EXPECT_EQ(ub.layout.blockRepeat.repeatStride, 96);
}

TEST(UbMemory, NdDmaVariantsPreserveDestinationStrides)
{
    for (uint32_t id : {87u, 88u, 89u}) {
        SCOPED_TRACE(id);
        MemoryRegisterState state{};
        state.ndDmaPadCount = NdDmaPadCountParamField{};
        // Decode the full SET register: source stride [59:20], destination [19:0].
        for (uint32_t i = 0; i < 2; ++i) {
            AclsanRawTraceRecord raw{};
            raw.instrId = 132 + i;
            raw.args[0] = ((i == 0 ? 1ULL : 16ULL) << 20) | (i == 0 ? 2 : 32);
            const auto d = dav3510::GetDeviceInstructionDecoder().decode(raw);
            ASSERT_TRUE(d);
            state.ndDmaLoopStrides[i] = std::get<NdDmaLoopStrideParamField>(d->params);
        }
        const auto r =
            ConvertUbRaw(id, {0x100, 0x100000, (3ULL << 4) | (2ULL << 24) | (1ULL << 44), 1 | (1ULL << 20)}, state);
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        const auto& ub = r.data[1];
        const uint32_t bytes = 1u << (id - 87);
        EXPECT_EQ(ub.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        EXPECT_EQ(ub.layoutKind, ACLSAN_MEM_LAYOUT_ND_AFFINE);
        EXPECT_EQ(ub.layout.ndAffine.elementBytes, bytes);
        EXPECT_EQ(ub.layout.ndAffine.dims[0], 3u);
        EXPECT_EQ(ub.layout.ndAffine.dims[1], 2u);
        EXPECT_EQ(ub.layout.ndAffine.strides[0], 2 * bytes);
        EXPECT_EQ(ub.layout.ndAffine.strides[1], 32 * bytes);
    }
}

TEST(UbMemory, UbToUbDecodesAndGeneratesIndependentReadAndWrite)
{
    const auto r = ConvertUbRaw(174, {0x100, 0x800, 3ULL | (2ULL << 16) | (1ULL << 32) | (2ULL << 48), 0});
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0].address, 0x800u);
    EXPECT_EQ(r.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(r.data[1].address, 0x100u);
    EXPECT_EQ(r.data[1].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    for (size_t i = 0; i < r.data.size(); ++i) {
        EXPECT_EQ(r.data[i].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        EXPECT_EQ(r.data[i].layout.blockRepeat.blockSize, 64u);
        EXPECT_EQ(r.data[i].layout.blockRepeat.repeatTimes, 3u);
        EXPECT_EQ(r.data[i].layout.blockRepeat.repeatStride, i == 0 ? 96 : 128);
        EXPECT_EQ(r.data[i].accessCount, 2u);
        EXPECT_EQ(r.data[i].accessIndex, i);
    }
}

TEST(UbMemory, OtherVectorIdsRemainUndecoded)
{
    for (uint32_t id : {175u, 176u, 177u, 178u, 417u, 418u, 419u, 420u, 421u}) {
        AclsanRawTraceRecord raw{};
        raw.instrId = id;
        EXPECT_FALSE(dav3510::GetDeviceInstructionDecoder().decode(raw));
    }
}

TEST(UbMemory, CbufUbCopiesUseSubBlockAndOwnerHeaders)
{
    const uint64_t config0 = 1ULL | (2ULL << 4U) | (3ULL << 16U) | (1ULL << 32U) | (2ULL << 48U);
    MemoryCbdataContext context{};
    context.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    context.coreId = 0;
    context.blockId = 1;
    context.physicalCoreCount = 6;
    const auto l1ToUb = ConvertRawWithContext(158, {0x100, 0x200, config0, 0}, context);
    ASSERT_EQ(l1ToUb.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(l1ToUb.data.size(), 2u);
    EXPECT_EQ(l1ToUb.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    EXPECT_EQ(l1ToUb.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(l1ToUb.data[0].header.phyCoreId, 0u);
    EXPECT_EQ(l1ToUb.data[0].header.blockId, 1u);
    EXPECT_EQ(l1ToUb.data[0].header.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
    EXPECT_EQ(l1ToUb.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(l1ToUb.data[1].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    EXPECT_EQ(l1ToUb.data[1].header.phyCoreId, 2u);
    EXPECT_EQ(l1ToUb.data[1].header.blockId, 3u);
    EXPECT_EQ(l1ToUb.data[1].header.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
    EXPECT_EQ(l1ToUb.data[1].layout.blockRepeat.blockSize, 96u);
    EXPECT_EQ(l1ToUb.data[1].layout.blockRepeat.repeatStride, 160u);

    MemoryCbdataContext vectorContext = context;
    vectorContext.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    vectorContext.coreId = 2;
    vectorContext.blockId = 3;
    const auto ubToL1 = ConvertRawWithContext(173, {0x200, 0x100, config0, 0}, vectorContext);
    ASSERT_EQ(ubToL1.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(ubToL1.data.size(), 2u);
    EXPECT_EQ(ubToL1.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    EXPECT_EQ(ubToL1.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    EXPECT_EQ(ubToL1.data[0].header.phyCoreId, 0u);
    EXPECT_EQ(ubToL1.data[0].header.blockId, 1u);
    EXPECT_EQ(ubToL1.data[0].header.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
    EXPECT_EQ(ubToL1.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(ubToL1.data[1].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(ubToL1.data[1].header.phyCoreId, 2u);
    EXPECT_EQ(ubToL1.data[1].header.blockId, 3u);
    EXPECT_EQ(ubToL1.data[1].header.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
}

TEST(UbMemory, TraceIdsStayDistinctWhenVectorWritersShareL1Owner)
{
    std::vector<uint8_t> buffer;
    std::string error;
    ASSERT_TRUE(InitializeTraceBuffer(buffer, 6, 1, 9, error));
    // The same PC and local record ordinal on the two AIVs in one mix group.
    for (uint32_t sub = 0; sub < 2; ++sub) {
        const uint32_t core = 1 + sub;
        const size_t offset = sizeof(AclsanTraceBufferHeader) + core * ASCSAN_TRACE_BYTES_PER_CORE;
        const AclsanTraceSliceHeader slice{1, 0, core, 0};
        std::memcpy(buffer.data() + offset, &slice, sizeof(slice));
        AclsanRawTraceRecord raw{};
        raw.instrId = 173;
        raw.pc = 0x1c8;
        raw.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
        raw.category = DeviceInstructionCategory::MemoryAccess;
        raw.blockId = sub;
        raw.args[0] = sub * 32;
        raw.args[1] = 0x1000;
        raw.args[2] = (1ULL << 4) | (1ULL << 16);
        std::memcpy(buffer.data() + offset + sizeof(slice), &raw, sizeof(raw));
    }
    const auto parsed = ParseTraceBuffer(buffer.data(), buffer.size(), 6, 1, 9, 0);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.records.size(), 2u);
    std::vector<AclsanDeviceEventHeader> l1Headers;
    for (const auto& record : parsed.records) {
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(record.record);
        ASSERT_TRUE(decoded);
        MemoryCbdataContext context{};
        context.instructionId = 173;
        context.launchId = record.launchId;
        context.instrExecId = record.instrExecId;
        context.blockType = record.blockType;
        context.blockId = record.blockId;
        context.coreId = record.phyCoreId;
        context.pipeline = record.record.pipeline;
        context.physicalCoreCount = record.physicalCoreCount;
        const auto result =
            MemoryFieldToCbdataConverter{context}.Convert(std::get<LocalMemoryTransferParamField>(decoded->params));
        ASSERT_EQ(result.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(result.data.size(), 2u);
        const auto& l1 = result.data[0];
        const auto& ub = result.data[1];
        EXPECT_EQ(l1.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
        EXPECT_EQ(l1.header.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
        EXPECT_EQ(l1.header.phyCoreId, 0u);
        EXPECT_EQ(l1.header.blockId, 0u);
        EXPECT_EQ(ub.header.phyCoreId, record.phyCoreId);
        EXPECT_EQ(l1.header.instrExecId, ub.header.instrExecId);
        l1Headers.push_back(l1.header);
    }
    EXPECT_NE(l1Headers[0].instrExecId, l1Headers[1].instrExecId);
}

TEST(UbMemory, L0cUbFixpipeWritesSelectedAndDualVectorOwners)
{
    const uint64_t config0 = (16ULL << 4U) | (16ULL << 16U) | (256ULL << 32U);
    MemoryCbdataContext context{};
    context.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    context.coreId = 0;
    context.blockId = 1;
    context.physicalCoreCount = 6;

    const auto selected = ConvertRawWithContext(170, {0x400, 0x800, config0, 16ULL | (1ULL << 18U)}, context);
    ASSERT_EQ(selected.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(selected.data.size(), 2u);
    EXPECT_EQ(selected.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L0C);
    EXPECT_EQ(selected.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(selected.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(selected.data[1].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    EXPECT_EQ(selected.data[1].header.phyCoreId, 2u);
    EXPECT_EQ(selected.data[1].header.blockId, 3u);
    EXPECT_EQ(selected.data[1].header.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
    EXPECT_EQ(selected.data[1].layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
    EXPECT_EQ(selected.data[1].layout.range.bytes, 1024u);

    const uint64_t config0M = (16ULL << 4U) | (32ULL << 16U) | (256ULL << 32U);
    const auto dualM = ConvertRawWithContext(170, {0x400, 0x800, config0M, 1ULL << 16U}, context);
    ASSERT_EQ(dualM.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(dualM.data.size(), 3u);
    EXPECT_EQ(dualM.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(dualM.data[2].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(dualM.data[1].header.phyCoreId, 1u);
    EXPECT_EQ(dualM.data[1].header.blockId, 2u);
    EXPECT_EQ(dualM.data[2].header.phyCoreId, 2u);
    EXPECT_EQ(dualM.data[2].header.blockId, 3u);

    const uint64_t config0N = (32ULL << 4U) | (16ULL << 16U) | (256ULL << 32U);
    const auto dualN = ConvertRawWithContext(170, {0x400, 0x800, config0N, 2ULL << 16U}, context);
    ASSERT_EQ(dualN.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(dualN.data.size(), 3u);
    EXPECT_EQ(dualN.data[1].layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
    EXPECT_EQ(dualN.data[1].layout.range.bytes, 1024u);
    EXPECT_EQ(dualN.data[2].layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
    EXPECT_EQ(dualN.data[2].layout.range.bytes, 1024u);
}

TEST(MemoryAccessIndexing, PackedFixAtAccessLimitIncludesGmAndLocalFragments)
{
    FixL0cToOutParamField field{};
    field.instrId = static_cast<uint32_t>(InstructionId::FixL0cToOutF32);
    field.srcAddr = 0x800;
    field.dstAddr = 0x100000;
    field.nSize = 16;
    field.mSize = 31;
    field.loopSrtStride = 32;
    field.loopDstStride = 64;
    field.quantPre = 25;
    field.nz2ndEnable = true;
    MemoryRegisterState state{};
    state.ctrl = CtrlRegisterParamField{0};
    state.loop3 = Loop3ParamField{128, 1024, 4096};

    const auto result = MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    ASSERT_EQ(result.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(result.data.size(), 4096u);
    EXPECT_EQ(result.data.front().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    EXPECT_EQ(result.data[3967].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    EXPECT_EQ(result.data[3968].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L0C);
    EXPECT_EQ(result.data.back().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L0C);
    for (size_t i = 0; i < result.data.size(); ++i) {
        EXPECT_EQ(result.data[i].accessIndex, i);
        EXPECT_EQ(result.data[i].accessCount, 4096u);
    }
}

TEST(UbMemory, CrossCoreTopologyRoundTripsAcrossBothDies)
{
    for (uint32_t count : {6u, 108u}) {
        for (uint32_t core = 0; core < count; ++core) {
            if (!IsAicPhysicalCore(core, count)) {
                continue;
            }
            for (uint32_t sub : {0u, 1u}) {
                const auto vector = AivPhysicalCoreForAic(core, sub, count);
                EXPECT_LT(vector, count);
                EXPECT_FALSE(IsAicPhysicalCore(vector, count));
                EXPECT_EQ(AicPhysicalCoreForAiv(vector, count), core);
            }
        }
    }
    EXPECT_EQ(AivPhysicalCoreForAic(0, 0, 0), UINT32_MAX);
    EXPECT_EQ(AivPhysicalCoreForAic(0, 2, 108), UINT32_MAX);
    EXPECT_EQ(AicPhysicalCoreForAiv(0, 108), UINT32_MAX);
    EXPECT_EQ(AivBlockIdForAic(UINT32_MAX, 1), UINT32_MAX);
}

TEST(UbMemory, FixpipeUbFilteringAndQuantizedS32UseLocalDestination)
{
    MemoryCbdataContext context{};
    context.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    context.physicalCoreCount = 108;
    context.coreId = 56;
    context.blockId = 3;
    // A numerically equal GM parameter range must not remove this UB write.
    context.parameterBase = 0x400;
    context.parameterBytes = 4096;
    const auto r = ConvertRawWithContext(
        171, {0x400, 0x800, (16ULL << 4) | (16ULL << 16) | (512ULL << 32), 16ULL | (1ULL << 18) | (8ULL << 34)},
        context);
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0].header.phyCoreId, 56u);
    EXPECT_EQ(r.data[1].header.phyCoreId, 77u);
    EXPECT_EQ(r.data[1].header.blockId, 7u);
    EXPECT_EQ(r.data[1].dataBits, 8u);
    EXPECT_EQ(r.data[1].layout.range.bytes, 256u);
    EXPECT_EQ(r.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
}

TEST(UbMemory, DualTargetNz2NdPreservesRowsAndMatrixStrideOnEachUb)
{
    MemoryCbdataContext context{};
    context.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    context.physicalCoreCount = 6;
    MemoryRegisterState state{};
    state.loop3 = Loop3ParamField{2, 64, 4096};
    for (uint32_t dual : {1u, 2u}) {
        const auto r = ConvertRawWithContext(
            170,
            {0x400, 0x800, (30ULL << 4) | (48ULL << 16) | (64ULL << 32), 64ULL | (uint64_t(dual) << 16) | (1ULL << 43)},
            context, state);
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 6u);
        // Four L0C fragments belong to the AIC; each UB target has its own
        // sequence, even though its result was merged before the outer merge.
        for (size_t i = 0; i < r.data.size(); ++i) {
            EXPECT_EQ(r.data[i].accessIndex, i < 4 ? i : 0u);
            EXPECT_EQ(r.data[i].accessCount, i < 4 ? 4u : 1u);
        }
        size_t targets = 0;
        for (const auto& a : r.data) {
            if (a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_UB) {
                continue;
            }
            ASSERT_EQ(a.layoutKind, ACLSAN_MEM_LAYOUT_ND_AFFINE);
            EXPECT_EQ(a.layout.ndAffine.elementBytes, dual == 1 ? 120u : 60u);
            EXPECT_EQ(a.layout.ndAffine.dims[0], dual == 1 ? 24u : 48u);
            EXPECT_EQ(a.layout.ndAffine.dims[1], 2u);
            EXPECT_EQ(a.layout.ndAffine.strides[0], 256);
            EXPECT_EQ(a.layout.ndAffine.strides[1], 16384);
            EXPECT_EQ(a.header.phyCoreId, 1u + targets);
            ++targets;
        }
        EXPECT_EQ(targets, 2u);
    }
}

TEST(UbMemory, UnknownTopologyAndReservedDualModeNeverInventUbOwners)
{
    MemoryCbdataContext context{};
    context.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    const std::array<uint64_t, 4> copy{0x400, 0x800, (1ULL << 4) | (1ULL << 16), 0};
    const auto missing = ConvertRawWithContext(158, copy, context);
    EXPECT_EQ(missing.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    EXPECT_TRUE(missing.requiresIncompleteEvent);
    ASSERT_EQ(missing.data.size(), 1u);
    EXPECT_EQ(missing.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    MemoryCbdataContext vectorContext{};
    vectorContext.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    vectorContext.coreId = 4;
    vectorContext.blockId = 7;
    const auto ubKnown = ConvertRawWithContext(173, copy, vectorContext);
    EXPECT_EQ(ubKnown.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    EXPECT_TRUE(ubKnown.requiresIncompleteEvent);
    ASSERT_EQ(ubKnown.data.size(), 1u);
    EXPECT_EQ(ubKnown.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(ubKnown.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    context.physicalCoreCount = 6;
    const auto reserved = ConvertRawWithContext(
        170, {0x400, 0x800, (32ULL << 4) | (16ULL << 16) | (256ULL << 32), 16ULL | (3ULL << 16)}, context);
    EXPECT_EQ(reserved.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    EXPECT_TRUE(reserved.requiresIncompleteEvent);
    ASSERT_EQ(reserved.data.size(), 1u);
    EXPECT_EQ(reserved.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L0C);
}

TEST(UbMemory, CompactGmToUbPadsWholeBurstGroupOnce)
{
    const auto r = ConvertUbRaw(
        84, {0x100, 0x100000, (3ULL << 4) | (48ULL << 25), 64 | (48ULL << 40)},
        UbDmaState(DmaLoopDirection::GM_TO_UBUF));
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[1].layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
    EXPECT_EQ(r.data[1].layout.range.bytes, 160u);
}

TEST(UbMemory, CompactUbToGmReadsAlignmentTailButWritesOnlyValidBytes)
{
    const auto r = ConvertUbRaw(
        83, {0x100000, 0x100, (3ULL << 4) | (48ULL << 25), 48 | (48ULL << 40)},
        UbDmaState(DmaLoopDirection::UBUF_TO_GM));
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0].layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
    EXPECT_EQ(r.data[0].layout.range.bytes, 144u);
    EXPECT_EQ(r.data[1].layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
    EXPECT_EQ(r.data[1].layout.range.bytes, 160u);
}

TEST(UbMemory, DmaOuterLoopsUseUbSideOfEachStrideRegister)
{
    for (bool output : {false, true}) {
        const auto direction = output ? DmaLoopDirection::UBUF_TO_GM : DmaLoopDirection::GM_TO_UBUF;
        auto state = UbDmaState(direction);
        const size_t d = static_cast<size_t>(direction);
        state.dmaLoopSizes[d] = DmaLoopSizeParamField{direction, 2, 3};
        state.dmaLoopStrides[d][0] = DmaLoopStrideParamField{direction, 0, 256, 512};
        state.dmaLoopStrides[d][1] = DmaLoopStrideParamField{direction, 1, 1024, 2048};
        const auto r =
            ConvertUbRaw(output ? 83 : 84, {0x1000, 0x2000, (2ULL << 4) | (32ULL << 25), 64 | (96ULL << 40)}, state);
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        const auto& ub = r.data[1];
        ASSERT_EQ(ub.layoutKind, ACLSAN_MEM_LAYOUT_ND_AFFINE);
        EXPECT_EQ(ub.layout.ndAffine.dims[0], 2u);
        EXPECT_EQ(ub.layout.ndAffine.dims[1], 2u);
        EXPECT_EQ(ub.layout.ndAffine.dims[2], 3u);
        EXPECT_EQ(ub.layout.ndAffine.strides[0], 96);
        EXPECT_EQ(ub.layout.ndAffine.strides[1], output ? 256 : 512);
        EXPECT_EQ(ub.layout.ndAffine.strides[2], output ? 1024 : 2048);
    }
}

TEST(UbMemory, NdDmaPaddingWritesSameDestinationInBothValueModes)
{
    for (bool constant : {false, true}) {
        MemoryRegisterState state{};
        state.ndDmaPadCount = NdDmaPadCountParamField{{1, 0, 0, 0}, {2, 0, 0, 0}};
        state.ndDmaLoopStrides[0] = NdDmaLoopStrideParamField{0, 1, 1};
        state.ndDmaLoopStrides[1] = NdDmaLoopStrideParamField{1, 8, 16};
        const auto r = ConvertUbRaw(
            88,
            {0x100, 0x100000, (8ULL << 4) | (2ULL << 24) | (1ULL << 44),
             1 | (1ULL << 20) | (3ULL << 40) | (5ULL << 48) | (uint64_t(constant) << 56)},
            state);
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        const auto& gm = r.data[0].layout.ndAffine;
        EXPECT_EQ(gm.dims[0], 8u);
        EXPECT_EQ(gm.dims[1], 2u);
        const auto& ub = r.data[1].layout.ndAffine;
        EXPECT_EQ(ub.dims[0], 16u);
        EXPECT_EQ(ub.dims[1], 5u);
        EXPECT_EQ(ub.strides[0], 2);
        EXPECT_EQ(ub.strides[1], 32);
    }
}

TEST(UbMemory, NdDmaMissingPaddingRetainsGmReadAndMarksIncomplete)
{
    const auto r = ConvertUbRaw(87, {0x100, 0x100000, (1ULL << 4) | (1ULL << 24) | (1ULL << 44), 1 | (1ULL << 20)});
    EXPECT_EQ(r.status, MemoryCbdataStatus::MISSING_REGISTER_STATE);
    EXPECT_EQ(r.requiredRegisterInstructionId, 131u);
    EXPECT_TRUE(r.requiresIncompleteEvent);
    ASSERT_EQ(r.data.size(), 1u);
    EXPECT_EQ(r.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
}

TEST(UbMemory, PaddingOnSingletonAxisRequiresDestinationStride)
{
    MemoryRegisterState state{};
    state.ndDmaPadCount = NdDmaPadCountParamField{{1, 0, 0, 0}, {0, 0, 0, 0}};
    const auto r =
        ConvertUbRaw(87, {0x100, 0x100000, (1ULL << 4) | (1ULL << 24) | (1ULL << 44), 1 | (1ULL << 20)}, state);
    EXPECT_EQ(r.status, MemoryCbdataStatus::MISSING_REGISTER_STATE);
    EXPECT_EQ(r.requiredRegisterInstructionId, 133u);
    EXPECT_TRUE(r.requiresIncompleteEvent);
}

TEST(UbMemory, ZeroLengthsAndCountsNeverCreateUbAccesses)
{
    for (uint32_t id : {83u, 84u, 85u, 86u, 87u, 88u, 89u, 174u}) {
        const auto r = ConvertUbRaw(id, {0x100, 0x100000, 0, 0});
        EXPECT_EQ(r.status, MemoryCbdataStatus::NO_ACCESS) << id;
        EXPECT_TRUE(r.data.empty()) << id;
    }
}

TEST(UbMemory, DestinationOverflowPreservesKnownGmReadWithoutExactUbWrite)
{
    const auto r = ConvertUbRaw(
        84, {UINT64_MAX - 15, 0x100000, (1ULL << 4) | (32ULL << 25), 0}, UbDmaState(DmaLoopDirection::GM_TO_UBUF));
    EXPECT_EQ(r.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
    EXPECT_TRUE(r.requiresIncompleteEvent);
    ASSERT_EQ(r.data.size(), 1u);
    EXPECT_EQ(r.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
}

TEST(UbMemory, GmOverflowPreservesKnownUbEndpointAndMarksIncomplete)
{
    for (uint32_t id : {83u, 84u, 87u}) {
        SCOPED_TRACE(id);
        const bool output = id == 83;
        auto state = UbDmaState(output ? DmaLoopDirection::UBUF_TO_GM : DmaLoopDirection::GM_TO_UBUF);
        uint64_t config = (1ULL << 4) | (32ULL << 25);
        uint64_t config1 = 0;
        if (id == 87) {
            config = (32ULL << 4) | (1ULL << 24) | (1ULL << 44);
            config1 = 1 | (1ULL << 20);
            state.ndDmaPadCount = NdDmaPadCountParamField{};
            state.ndDmaLoopStrides[0] = NdDmaLoopStrideParamField{0, 1, 1};
        }
        const auto r = ConvertUbRaw(
            id, {output ? UINT64_MAX - 15 : 0x100, output ? 0x100 : UINT64_MAX - 15, config, config1}, state);
        EXPECT_EQ(r.status, MemoryCbdataStatus::PARTIAL_COVERAGE);
        EXPECT_TRUE(r.requiresIncompleteEvent);
        ASSERT_EQ(r.data.size(), 1u);
        EXPECT_EQ(r.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        EXPECT_EQ(r.data[0].address, 0x100u);
        EXPECT_EQ(r.data[0].accessMode, output ? ACLSAN_DEVICE_MEMORY_ACCESS_READ : ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    }
}

TEST(UbMemory, GmInternalRangesCannotSuppressNumericallyEqualUbAddresses)
{
    MemoryCbdataContext context{};
    context.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    context.parameterBase = 0x100;
    context.parameterBytes = 256;
    const auto r =
        MemoryFieldToCbdataConverter{context}.Convert(CopyUbufToUbufParamField{174, 0x100, 0x140, 1, 1, 0, 0});
    ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(r.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
}

TEST(UbMemory, UbCopyDecoderKeepsAllConfigFieldsAndFullAddressWidths)
{
    AclsanRawTraceRecord raw{};
    raw.instrId = 174;
    raw.args[0] = 0x123456789abcdef0;
    raw.args[1] = 0xfedcba9876543210;
    raw.args[2] = 0xabc5ULL | (0x1234ULL << 16) | (0x5678ULL << 32) | (0x9abcULL << 48);
    const auto d = dav3510::GetDeviceInstructionDecoder().decode(raw);
    ASSERT_TRUE(d);
    const auto& f = std::get<CopyUbufToUbufParamField>(d->params);
    EXPECT_EQ(f.dstAddr, raw.args[0]);
    EXPECT_EQ(f.srcAddr, raw.args[1]);
    EXPECT_EQ(f.burstNum, 0xabc5u);
    EXPECT_EQ(f.burstLen, 0x1234u);
    EXPECT_EQ(f.srcGap, 0x5678u);
    EXPECT_EQ(f.dstGap, 0x9abcu);
}

TEST(UbMemory, UbToUbUsesFullSixteenBitBurstCount)
{
    for (uint64_t count : {1ULL, 15ULL, 16ULL, 0x1001ULL, 0xffffULL}) {
        SCOPED_TRACE(count);
        const auto r = ConvertUbRaw(174, {0x100, 0x800, count | (1ULL << 16) | (1ULL << 32) | (2ULL << 48), 0});
        ASSERT_EQ(r.status, MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(r.data.size(), 2u);
        for (const auto& access : r.data) {
            if (count == 1) {
                ASSERT_EQ(access.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
                EXPECT_EQ(access.layout.range.bytes, 32u);
            } else {
                ASSERT_EQ(access.layoutKind, ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
                EXPECT_EQ(access.layout.blockRepeat.repeatTimes, count);
                EXPECT_EQ(access.layout.blockRepeat.blockSize, 32u);
            }
        }
    }
}
} // namespace
