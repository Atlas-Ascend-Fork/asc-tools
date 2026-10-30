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

#include "acl_san/aclsan_api.h"
#include "device_instr/common/instruction_id.h"
#include "aclsan_memory_cbdata.h"

#include <cassert>
#include <array>
#include <cstdint>
#include <limits>
#include <algorithm>

namespace {

// This suite specifies the pre-existing GM address sets. Cube's complete
// multi-space results and incomplete modes are tested in cube_memory_test.cpp.
class GmProjectionConverter {
public:
    GmProjectionConverter(aclsan::MemoryCbdataContext c, aclsan::MemoryRegisterState s = {})
        : converter_(c, WithAtomicDisabled(s))
    {}
    static aclsan::MemoryRegisterState WithAtomicDisabled(aclsan::MemoryRegisterState s)
    {
        s.ctrl = aclsan::CtrlRegisterParamField{0};
        if (!s.ndDmaPadCount) {
            s.ndDmaPadCount = aclsan::NdDmaPadCountParamField{};
        }
        return s;
    }
    aclsan::MemoryCbdataResult Convert(const aclsan::MemoryInstructionField& f) const
    {
        auto r = converter_.Convert(f);
        r.data.erase(
            std::remove_if(
                r.data.begin(), r.data.end(),
                [](const auto& d) { return d.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM; }),
            r.data.end());
        if (r.status == aclsan::MemoryCbdataStatus::PARTIAL_COVERAGE && !r.data.empty()) {
            r.status = aclsan::MemoryCbdataStatus::SUCCESS;
        }
        for (size_t i = 0; i < r.data.size(); ++i) {
            r.data[i].accessIndex = i;
            r.data[i].accessCount = r.data.size();
        }
        return r;
    }

private:
    aclsan::MemoryFieldToCbdataConverter converter_;
};

using aclsan::CopyGmToUbufAlignV2ParamField;
using aclsan::InstructionId;

uint32_t RawInstructionId(InstructionId instruction) { return static_cast<uint32_t>(instruction); }

aclsan::MemoryRegisterState SingletonDmaState(aclsan::DmaLoopDirection direction)
{
    aclsan::MemoryRegisterState state{};
    state.dmaLoopSizes[static_cast<std::size_t>(direction)] = aclsan::DmaLoopSizeParamField{direction, 1, 1};
    return state;
}

void TestEmptyFieldProducesNoCbdata()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 16;

    const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::NO_ACCESS);
    assert(result.data.empty());
}

void TestFieldAndContextProduceCbdata()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 16;
    field.srcAddr = 0x1000;
    field.burstNum = 3;
    field.burstLen = 32;
    field.burstSrcStride = 64;
    aclsan::MemoryCbdataContext context{};
    context.pc = 0x2000;
    context.instrExecId = 41;
    context.serialNo = 40;
    context.siteId = 7;
    context.coreId = 2;
    context.blockId = 3;
    context.pipeline = ACLSAN_DEVICE_PIPE_MTE2;

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_UBUF);
    const auto result = GmProjectionConverter{context, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    const AclsanDeviceMemoryAccessData& access = result.data.front();
    assert(access.header.pc == context.pc);
    assert(access.header.instrExecId == context.instrExecId);
    assert(access.header.serialNo == context.serialNo);
    assert(access.header.siteId == context.siteId);
    assert(access.header.phyCoreId == context.coreId);
    assert(access.header.blockId == context.blockId);
    assert(access.header.pipeline == context.pipeline);
    assert(access.address == field.srcAddr);
    assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(access.accessIndex == 0);
    assert(access.accessCount == 1);
    assert(access.dataBits == field.dataBits);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(access.layout.blockRepeat.blockNum == 1);
    assert(access.layout.blockRepeat.blockSize == field.burstLen);
    assert(access.layout.blockRepeat.repeatTimes == field.burstNum);
    assert(access.layout.blockRepeat.repeatStride == static_cast<int64_t>(field.burstSrcStride));
}

void TestCubeAndMultiFieldsUseDecodedDataBits()
{
    aclsan::CopyGmToCbufAlignV2ParamField cubeField{};
    cubeField.instrId = RawInstructionId(InstructionId::CopyGmToCbufAlignV2B16);
    cubeField.dataBits = 16;
    cubeField.srcAddr = 0x2000;
    cubeField.burstNum = 1;
    cubeField.burstLen = 8;
    const auto cubeState = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto cubeResult = GmProjectionConverter{{}, cubeState}.Convert(aclsan::MemoryInstructionField{cubeField});
    assert(cubeResult.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(cubeResult.data.size() == 1);
    assert(cubeResult.data.front().dataBits == cubeField.dataBits);

    aclsan::CopyGmToCbufMultiNd2NzParamField multiField{};
    multiField.instrId = RawInstructionId(InstructionId::CopyGmToCbufMultiNd2NzB16);
    multiField.dataBits = 16;
    multiField.srcAddr = 0x3000;
    multiField.nValue = 1;
    multiField.dValue = 1;
    aclsan::MemoryRegisterState state{};
    state.mte2Nz = aclsan::Mte2NzParamField{1};
    const auto multiResult = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{multiField});
    assert(multiResult.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(multiResult.data.size() == 1);
    assert(multiResult.data.front().dataBits == multiField.dataBits);
}

void TestNdDmaFieldUsesDecodedDataBits()
{
    aclsan::NdDmaOutToUbufParamField field{};
    field.dataBits = 16;
    field.srcAddr = 0x4000;
    field.loop0Size = 1;
    field.loop1Size = 1;
    field.loop2Size = 1;
    field.loop3Size = 1;
    field.loop4Size = 1;

    aclsan::MemoryRegisterState state{};
    // Singleton NDDMA dimensions do not consume their stride registers.
    const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().dataBits == field.dataBits);
    assert(result.data.front().address == field.srcAddr);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(result.data.front().layout.ndAffine.rank == 5);
    assert(result.data.front().layout.ndAffine.elementBytes == 2);
}

void TestOneFieldCanProduceMultipleCbdataRecords()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.dstAddr = 0x8000;
    field.nSize = 48;
    field.mSize = 4;
    field.loopDstStride = 64;
    field.quantPre = 24;

    const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 2);
    assert(result.data[0].address == field.dstAddr);
    assert(result.data[0].accessIndex == 0);
    assert(result.data[0].accessCount == 2);
    assert(result.data[0].layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data[0].layout.range.bytes == 128);
    assert(result.data[1].address == field.dstAddr + 64);
    assert(result.data[1].accessIndex == 1);
    assert(result.data[1].accessCount == 2);
    assert(result.data[1].layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data[1].layout.range.bytes == 64);
}

void TestInvalidFieldIsRejected()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.srcAddr = 0x1000;
    field.burstNum = 1;
    field.burstLen = 32;

    const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);
    assert(result.data.empty());
}

void TestUnsupportedDataBitsIsRejected()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 64;
    field.srcAddr = 0x1000;
    field.burstNum = 1;
    field.burstLen = 32;

    const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);
    assert(result.data.empty());
}

void TestNdDmaPaddingDoesNotExpandGmReadFootprint()
{
    for (const bool constantPaddingMode : {false, true}) {
        aclsan::NdDmaOutToUbufParamField field{};
        field.instrId = RawInstructionId(InstructionId::NdDmaOutToUbufB16);
        field.dataBits = 16;
        field.srcAddr = 0x4000;
        field.loop0Size = 3;
        field.loop1Size = 2;
        field.loop2Size = 1;
        field.loop3Size = 1;
        field.loop4Size = 1;
        field.loop0LeftPaddingCount = 2;
        field.loop0RightPaddingCount = 7;
        field.paddingMode = constantPaddingMode;

        aclsan::MemoryRegisterState state{};
        state.ndDmaPadCount = aclsan::NdDmaPadCountParamField{{3, 4, 5, 6}, {8, 9, 10, 11}};
        state.ndDmaLoopStrides[0] = aclsan::NdDmaLoopStrideParamField{0, 1};
        state.ndDmaLoopStrides[1] = aclsan::NdDmaLoopStrideParamField{1, 8};
        state.ndDmaLoopStrides[2] = aclsan::NdDmaLoopStrideParamField{2, 0};
        state.ndDmaLoopStrides[3] = aclsan::NdDmaLoopStrideParamField{3, 0};
        state.ndDmaLoopStrides[4] = aclsan::NdDmaLoopStrideParamField{4, 0};
        const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
        assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
        assert(result.data.size() == 1);
        const auto& access = result.data.front();
        assert(access.address == field.srcAddr);
        assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
        assert(access.layout.ndAffine.rank == 5);
        assert(access.layout.ndAffine.elementBytes == 2);
        assert(access.layout.ndAffine.dims[0] == 3);
        assert(access.layout.ndAffine.dims[1] == 2);
        assert(access.layout.ndAffine.strides[0] == 2);
        assert(access.layout.ndAffine.strides[1] == 16);
    }
}

void TestCopyGmToCbufV2UsesUnifiedConverter()
{
    aclsan::CopyGmToCbufV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToCbufV2);
    field.srcAddr = 0x5000;
    field.burstNum = 2;
    field.burstLen = 3;
    field.srcStride = 5;

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    const auto& access = result.data.front();
    assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(access.header.sourceKind == ACLSAN_DEVICE_SOURCE_MTE2);
    assert(access.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    assert(access.dataBits == 0);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(access.layout.blockRepeat.blockSize == 96);
    assert(access.layout.blockRepeat.repeatStride == 160);
}

void TestCopyGmToCbufV2ModelsPaddingSourceFootprint()
{
    constexpr std::array<uint64_t, 5> kSourceBytes{1, 2, 4, 8, 16};
    for (uint8_t mode = 1; mode <= 5; ++mode) {
        aclsan::CopyGmToCbufV2ParamField field{};
        field.instrId = RawInstructionId(InstructionId::CopyGmToCbufV2);
        field.srcAddr = 0x5400;
        field.burstNum = 3;
        field.burstLen = 1;
        field.padFunctionMode = mode;

        const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
        const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
        assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
        assert(result.data.size() == 1);
        const auto& access = result.data.front();
        assert(access.layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
        assert(access.layout.range.bytes == 3 * kSourceBytes[mode - 1]);
    }

    for (uint8_t mode = 6; mode <= 8; ++mode) {
        aclsan::CopyGmToCbufV2ParamField field{};
        field.instrId = RawInstructionId(InstructionId::CopyGmToCbufV2);
        field.srcAddr = 0x5500;
        field.burstNum = 2;
        field.burstLen = 2;
        field.padFunctionMode = mode;
        field.srcStride = 3;

        const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
        const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
        assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
        assert(result.data.size() == 1);
        const auto& access = result.data.front();
        assert(access.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
        assert(access.layout.blockRepeat.blockSize == 64);
        assert(access.layout.blockRepeat.repeatStride == 96);
    }

    aclsan::CopyGmToCbufV2ParamField invalid{};
    invalid.instrId = RawInstructionId(InstructionId::CopyGmToCbufV2);
    invalid.burstNum = 1;
    invalid.burstLen = 2;
    invalid.padFunctionMode = 1;
    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{invalid});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);
    invalid.burstLen = 1;
    invalid.padFunctionMode = 9;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{invalid});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);
}

void TestContiguousBurstStrideProducesOneRange()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 16;
    field.srcAddr = 0x5800;
    field.burstNum = 3;
    field.burstLen = 32;
    field.burstSrcStride = 32;

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_UBUF);
    const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data.front().layout.range.bytes == 96);
}

void TestAddressExtentOverflowIsRejected()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 16;
    field.srcAddr = UINT64_MAX - 31;
    field.burstNum = 2;
    field.burstLen = 32;
    field.burstSrcStride = 32;

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_UBUF);
    const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::PARTIAL_COVERAGE);
    assert(result.data.empty());
    assert(result.requiresIncompleteEvent);
}

void TestDmaOuterLoopsProduceAffineLayout()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 16;
    field.srcAddr = 0x6000;
    field.burstNum = 3;
    field.burstLen = 32;
    field.burstSrcStride = 64;

    aclsan::MemoryRegisterState state{};
    constexpr auto direction = aclsan::DmaLoopDirection::GM_TO_UBUF;
    const auto directionIndex = static_cast<std::size_t>(direction);
    state.dmaLoopSizes[directionIndex] = aclsan::DmaLoopSizeParamField{direction, 2, 4};
    state.dmaLoopStrides[directionIndex][0] = aclsan::DmaLoopStrideParamField{direction, 0, 0x200, 0x20};
    state.dmaLoopStrides[directionIndex][1] = aclsan::DmaLoopStrideParamField{direction, 1, 0x1000, 0x40};

    const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    const auto& layout = result.data.front().layout.ndAffine;
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(layout.rank == 3);
    assert(layout.elementBytes == 32);
    assert(layout.dims[0] == 3 && layout.strides[0] == 64);
    assert(layout.dims[1] == 2 && layout.strides[1] == 0x200);
    assert(layout.dims[2] == 4 && layout.strides[2] == 0x1000);
}

void TestDmaOuterLoopsUseTheGmStrideForEveryDirection()
{
    {
        aclsan::CopyUbufToGmAlignV2ParamField field{};
        field.instrId = RawInstructionId(InstructionId::CopyUbufToGmAlignV2);
        field.dstAddr = 0x7000;
        field.burstNum = 2;
        field.burstLen = 16;
        field.dstStride = 64;

        aclsan::MemoryRegisterState state{};
        constexpr auto direction = aclsan::DmaLoopDirection::UBUF_TO_GM;
        const auto directionIndex = static_cast<std::size_t>(direction);
        state.dmaLoopSizes[directionIndex] = aclsan::DmaLoopSizeParamField{direction, 2, 3};
        state.dmaLoopStrides[directionIndex][0] = aclsan::DmaLoopStrideParamField{direction, 0, 0x20, 0x300};
        state.dmaLoopStrides[directionIndex][1] = aclsan::DmaLoopStrideParamField{direction, 1, 0x40, 0x1000};

        const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
        assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
        const auto& access = result.data.front();
        assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
        assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
        assert(access.layout.ndAffine.dims[0] == 2 && access.layout.ndAffine.strides[0] == 64);
        assert(access.layout.ndAffine.dims[1] == 2 && access.layout.ndAffine.strides[1] == 0x300);
        assert(access.layout.ndAffine.dims[2] == 3 && access.layout.ndAffine.strides[2] == 0x1000);
    }

    {
        aclsan::CopyGmToCbufV2ParamField field{};
        field.instrId = RawInstructionId(InstructionId::CopyGmToCbufV2);
        field.srcAddr = 0x8000;
        field.burstNum = 2;
        field.burstLen = 3;
        field.srcStride = 5;

        aclsan::MemoryRegisterState state{};
        constexpr auto direction = aclsan::DmaLoopDirection::GM_TO_CBUF;
        const auto directionIndex = static_cast<std::size_t>(direction);
        state.dmaLoopSizes[directionIndex] = aclsan::DmaLoopSizeParamField{direction, 2, 3};
        state.dmaLoopStrides[directionIndex][0] = aclsan::DmaLoopStrideParamField{direction, 0, 0x400, 0x20};
        state.dmaLoopStrides[directionIndex][1] = aclsan::DmaLoopStrideParamField{direction, 1, 0x2000, 0x40};

        const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
        assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
        const auto& access = result.data.front();
        assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
        assert(access.layout.ndAffine.elementBytes == 96);
        assert(access.layout.ndAffine.dims[0] == 2 && access.layout.ndAffine.strides[0] == 160);
        assert(access.layout.ndAffine.dims[1] == 2 && access.layout.ndAffine.strides[1] == 0x400);
        assert(access.layout.ndAffine.dims[2] == 3 && access.layout.ndAffine.strides[2] == 0x2000);
    }
}

void TestDmaOuterLoopStateDistinguishesDefaultZeroAndMissingStride()
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 16;
    field.srcAddr = 0x9000;
    field.burstNum = 1;
    field.burstLen = 32;

    aclsan::MemoryRegisterState state{};
    constexpr auto direction = aclsan::DmaLoopDirection::GM_TO_UBUF;
    const auto directionIndex = static_cast<std::size_t>(direction);
    state.dmaLoopSizes[directionIndex] = aclsan::DmaLoopSizeParamField{direction, 2, 1};
    auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::Loop1StrideGmToUbuf));

    state.dmaLoopSizes[directionIndex] = aclsan::DmaLoopSizeParamField{direction, 0, 1};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::NO_ACCESS);

    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::LoopSizeGmToUbuf));
}

void TestFixpipeConversionModesConsumeIndependentLoop3State()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.dstAddr = 0xa000;
    field.nSize = 32;
    field.mSize = 3;
    field.loopDstStride = 40;
    field.quantPre = 0;
    field.nz2ndEnable = true;

    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::Loop3Param));

    aclsan::MemoryRegisterState state{};
    state.loop3 = aclsan::Loop3ParamField{2, 7, 100};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    const auto& rowMajor = result.data.front();
    assert(rowMajor.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(rowMajor.layout.ndAffine.elementBytes == 128);
    assert(rowMajor.layout.ndAffine.dims[0] == 3 && rowMajor.layout.ndAffine.strides[0] == 160);
    assert(rowMajor.layout.ndAffine.dims[1] == 2 && rowMajor.layout.ndAffine.strides[1] == 400);

    field.nz2ndEnable = false;
    field.nz2dnEnable = true;
    field.nSize = 5;
    field.mSize = 32;
    state.loop3 = aclsan::Loop3ParamField{3, 9, 200};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::ChannelPara));

    state.channelPara = aclsan::ChannelParaParamField{1ULL << 48};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    const auto& columnMajor = result.data.front();
    assert(columnMajor.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(columnMajor.layout.blockRepeat.blockSize == 128);
    assert(columnMajor.layout.blockRepeat.repeatTimes == 15);
    assert(columnMajor.layout.blockRepeat.repeatStride == 160);

    state.loop3 = aclsan::Loop3ParamField{0, 9, 200};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::NO_ACCESS);
}

void TestFixpipeZeroDestinationStrideStillWritesGm()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.dstAddr = 0xa800;
    field.nSize = 48;
    field.mSize = 2;
    field.loopDstStride = 0;
    field.quantPre = 24;

    const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 2);
    assert(result.data[0].address == field.dstAddr);
    assert(result.data[0].layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data[0].layout.range.bytes == 64);
    assert(result.data[1].address == field.dstAddr);
    assert(result.data[1].layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data[1].layout.range.bytes == 32);
}

void TestFixpipeUnprovenLowLevelModeIsRejected()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.nSize = 16;
    field.mSize = 2;
    field.loopDstStride = 32;
    field.quantPre = 0;
    field.loopEnhanceEnable = true;

    const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);
}

void TestFixpipeDumpTensorC0PaddingUsesRawNzFootprint()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.dstAddr = 0xac00;
    field.nSize = 32;
    field.mSize = 16;
    field.loopDstStride = 256;
    field.loopSrtStride = 16;
    field.c0PadEnable = true;

    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().dataBits == 32);
    assert(result.data.front().address == field.dstAddr);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data.front().layout.range.bytes == 2048);

    field.nSize = 16;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().layout.range.bytes == 1024);

    field.quantPre = 1;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);
}

void TestMultiStateDistinguishesMissingAndObservedZero()
{
    aclsan::CopyGmToCbufMultiNd2NzParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToCbufMultiNd2NzB16);
    field.dataBits = 16;
    field.srcAddr = 0xb000;
    field.nValue = 2;
    field.dValue = 16;
    field.loop1SrcStride = 32;
    field.loop4SrcStride = 128;

    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::SetMte2NzPara));

    aclsan::MemoryRegisterState state{};
    state.mte2Nz = aclsan::Mte2NzParamField{0, 3, 4, 5};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::NO_ACCESS);
}

void TestLargeMultiAccessRemainsNdAffineInsteadOfTruncatingRepeatCount()
{
    aclsan::CopyGmToCbufMultiDn2NzParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToCbufMultiDn2NzB16);
    field.dataBits = 16;
    field.srcAddr = 0xb800;
    field.nValue = 1;
    field.dValue = (UINT32_C(1) << 20U) - 1U;
    field.loop1SrcStride = 4;
    field.loop4SrcStride = static_cast<uint64_t>(field.dValue) * field.loop1SrcStride;

    aclsan::MemoryRegisterState state{};
    state.mte2Nz = aclsan::Mte2NzParamField{UINT16_MAX};
    const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(result.data.front().layout.ndAffine.rank == 2);
}

void TestNdDmaRequiresOnlyActiveStrideStates()
{
    aclsan::NdDmaOutToUbufParamField field{};
    field.instrId = RawInstructionId(InstructionId::NdDmaOutToUbufB8);
    field.dataBits = 8;
    field.srcAddr = 0xc000;
    field.loop0Size = 1;
    field.loop1Size = 2;
    field.loop2Size = 1;
    field.loop3Size = 1;
    field.loop4Size = 1;

    aclsan::MemoryRegisterState state{};
    auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::NdDmaLoop1Stride));
    state.ndDmaLoopStrides[1] = aclsan::NdDmaLoopStrideParamField{1, 8};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);

    field.loop4Size = 0;
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::NO_ACCESS);
}

void TestLoadGmToCbuf2DV2UsesModeZeroFractalSize()
{
    aclsan::LoadGmToCbuf2DV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::LoadGmToCbuf2DV2);
    field.srcAddr = 0x100000;
    field.mStartPosition = 2;
    field.kStartPosition = 1;
    field.mStep = 2;
    field.kStep = 2;
    aclsan::MemoryRegisterState state{};
    state.mte2Source = aclsan::Mte2SourceParamField{4};

    const auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    const auto& access = result.data.front();
    assert(access.address == field.srcAddr + 6 * 512);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(access.layout.blockRepeat.blockSize == 2 * 512);
    assert(access.layout.blockRepeat.repeatTimes == 2);
    assert(access.layout.blockRepeat.repeatStride == 4 * 512);
}

void TestLoadGmToCbuf2DV2SkipsNonzeroModes()
{
    aclsan::LoadGmToCbuf2DV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::LoadGmToCbuf2DV2);
    field.srcAddr = 0x100000;
    field.mStep = 1;
    field.kStep = 1;

    for (uint8_t mode = 1; mode <= 7; ++mode) {
        field.decompMode = mode;
        const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
        assert(result.status == aclsan::MemoryCbdataStatus::PARTIAL_COVERAGE);
        assert(result.data.empty());
        assert(result.requiredRegisterInstructionId == 0);
    }
}

void TestLoadGmToCbuf2DV2SupportsNegativeAndZeroSourceStride()
{
    aclsan::LoadGmToCbuf2DV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::LoadGmToCbuf2DV2);
    field.srcAddr = 0x100000;
    field.mStartPosition = 2;
    field.kStartPosition = 3;
    field.mStep = 2;
    field.kStep = 3;
    aclsan::MemoryRegisterState state{};
    state.mte2Source = aclsan::Mte2SourceParamField{-4};
    auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().address == field.srcAddr + 6 * 512);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(result.data.front().layout.blockRepeat.blockSize == 2 * 512);
    assert(result.data.front().layout.blockRepeat.repeatTimes == 3);
    assert(result.data.front().layout.blockRepeat.repeatStride == 4 * 512);

    state.mte2Source = aclsan::Mte2SourceParamField{0};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().address == field.srcAddr + 2 * 512);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data.front().layout.range.bytes == 2 * 512);
}

void TestLoadGmToCbuf2DV2RejectsAddressOverflow()
{
    aclsan::LoadGmToCbuf2DV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::LoadGmToCbuf2DV2);
    field.mStep = 1;
    field.kStep = 1;
    field.decompMode = 0;
    aclsan::MemoryRegisterState state{};
    state.mte2Source = aclsan::Mte2SourceParamField{1};
    field.kStep = 2;
    state.mte2Source = aclsan::Mte2SourceParamField{-1};
    auto result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);

    field.kStep = 1;
    field.mStartPosition = 1;
    field.srcAddr = std::numeric_limits<uint64_t>::max();
    state.mte2Source = aclsan::Mte2SourceParamField{0};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);

    field.srcAddr = 0;
    field.mStartPosition = 0;
    state.mte2Source = aclsan::Mte2SourceParamField{std::numeric_limits<int64_t>::min()};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
}

void TestLoadGmToCbuf2DV2HandlesMissingStateEmptyStepsAndTailOverflow()
{
    aclsan::LoadGmToCbuf2DV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::LoadGmToCbuf2DV2);
    field.srcAddr = 0x1000;
    field.mStep = 1;
    field.kStep = 1;

    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::Mte2SrcPara));

    aclsan::MemoryRegisterState state{};
    state.mte2Source = aclsan::Mte2SourceParamField{1};
    field.mStep = 0;
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::NO_ACCESS);
    field.mStep = 1;
    field.kStep = 0;
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::NO_ACCESS);

    field.kStep = 2;
    field.srcAddr = std::numeric_limits<uint64_t>::max() - 511;
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
}

void TestFixpipePacked4ProducesExactByteRanges()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.dstAddr = 0x1000;
    field.nSize = 128;
    field.mSize = 16;
    field.loopDstStride = 1200;
    field.quantPre = 25;

    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 2);
    assert(result.data[0].dataBits == 4 && result.data[0].address == 0x1000);
    assert(result.data[0].layoutKind == ACLSAN_MEM_LAYOUT_RANGE && result.data[0].layout.range.bytes == 512);
    assert(result.data[1].address == 0x1258 && result.data[1].layout.range.bytes == 512);
    for (std::size_t index = 0; index < result.data.size(); ++index) {
        assert(result.data[index].accessIndex == static_cast<uint32_t>(index));
        assert(result.data[index].accessCount == result.data.size());
    }

    field.quantPre = 21;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);

    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.quantPre = 25;
    field.nSize = 48;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);

    field.nSize = 64;
    field.quantPre = 25;
    field.splitEnable = true;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);
}

void TestFixpipeB8ChannelMergeUsesThirtyTwoChannelGroups()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.dstAddr = 0x1800;
    field.nSize = 48;
    field.mSize = 16;
    field.loopDstStride = 1024;
    field.quantPre = 24;

    const auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 2);
    assert(result.data[0].dataBits == 8 && result.data[0].address == 0x1800);
    assert(result.data[0].layoutKind == ACLSAN_MEM_LAYOUT_RANGE && result.data[0].layout.range.bytes == 512);
    assert(result.data[1].dataBits == 8 && result.data[1].address == 0x1c00);
    assert(result.data[1].layoutKind == ACLSAN_MEM_LAYOUT_RANGE && result.data[1].layout.range.bytes == 256);

    aclsan::MemoryRegisterState state{};
    state.loop3 = aclsan::Loop3ParamField{2, 16, 512};
    field.nSize = 16;
    field.mSize = 16;
    field.loopDstStride = 32;
    field.nz2ndEnable = true;
    auto converted = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(converted.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(converted.data.size() == 1);
    assert(converted.data.front().layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(converted.data.front().layout.blockRepeat.blockSize == 16);
    assert(converted.data.front().layout.blockRepeat.repeatTimes == 32);
    assert(converted.data.front().layout.blockRepeat.repeatStride == 32);

    field.nz2ndEnable = false;
    field.nz2dnEnable = true;
    state.channelPara = aclsan::ChannelParaParamField{1ULL << 48};
    converted = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(converted.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(converted.data.size() == 1);
    assert(converted.data.front().layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(converted.data.front().layout.blockRepeat.blockSize == 16);
    assert(converted.data.front().layout.blockRepeat.repeatTimes == 32);
    assert(converted.data.front().layout.blockRepeat.repeatStride == 32);
}

void TestFixpipeNzRejectsUnsupportedNSizeRemainders()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutF32);
    field.dstAddr = 0x1000;
    field.mSize = 16;
    field.loopDstStride = 128;

    field.splitEnable = true;
    field.nSize = 9;
    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);

    field.nSize = 8;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data.front().layout.range.bytes == 512);

    field.splitEnable = false;
    field.nSize = 17;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::INVALID_FIELD);

    field.nSize = 16;
    result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 1);
    assert(result.data.front().layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(result.data.front().layout.range.bytes == 1024);
}

void TestFixpipePacked4ConversionModeUsesLoop3State()
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = RawInstructionId(InstructionId::FixL0cToOutS32);
    field.dstAddr = 0x2000;
    field.nSize = 5;
    field.mSize = 2;
    field.loopDstStride = 7;
    field.quantPre = 21;
    field.nz2ndEnable = true;

    auto result = GmProjectionConverter{{}}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    assert(result.requiredRegisterInstructionId == RawInstructionId(InstructionId::Loop3Param));

    aclsan::MemoryRegisterState state{};
    state.loop3 = aclsan::Loop3ParamField{2, 16, 20};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 4);
    assert(result.data[0].address == 0x2000 && result.data[0].layout.range.bytes == 3);
    assert(result.data[1].address == 0x200a && result.data[1].layout.range.bytes == 3);
    assert(result.data[2].address == 0x2003 && result.data[2].layout.range.bytes == 3);
    assert(result.data[3].address == 0x200d && result.data[3].layout.range.bytes == 3);

    field.nz2ndEnable = false;
    field.nz2dnEnable = true;
    state.channelPara = aclsan::ChannelParaParamField{1ULL << 48};
    result = GmProjectionConverter{{}, state}.Convert(aclsan::MemoryInstructionField{field});
    assert(result.status == aclsan::MemoryCbdataStatus::SUCCESS);
    assert(result.data.size() == 10);
    assert(result.data.front().address == 0x2000 && result.data.front().layout.range.bytes == 1);
    assert(result.data.back().address == 0x2018 && result.data.back().layout.range.bytes == 1);
}

} // namespace

TEST(AclsanMemoryCbdata, Main)
{
    TestEmptyFieldProducesNoCbdata();
    TestFieldAndContextProduceCbdata();
    TestCubeAndMultiFieldsUseDecodedDataBits();
    TestNdDmaFieldUsesDecodedDataBits();
    TestOneFieldCanProduceMultipleCbdataRecords();
    TestInvalidFieldIsRejected();
    TestUnsupportedDataBitsIsRejected();
    TestNdDmaPaddingDoesNotExpandGmReadFootprint();
    TestCopyGmToCbufV2UsesUnifiedConverter();
    TestCopyGmToCbufV2ModelsPaddingSourceFootprint();
    TestContiguousBurstStrideProducesOneRange();
    TestAddressExtentOverflowIsRejected();
    TestDmaOuterLoopsProduceAffineLayout();
    TestDmaOuterLoopsUseTheGmStrideForEveryDirection();
    TestDmaOuterLoopStateDistinguishesDefaultZeroAndMissingStride();
    TestFixpipeConversionModesConsumeIndependentLoop3State();
    TestFixpipeZeroDestinationStrideStillWritesGm();
    TestFixpipeUnprovenLowLevelModeIsRejected();
    TestFixpipeDumpTensorC0PaddingUsesRawNzFootprint();
    TestMultiStateDistinguishesMissingAndObservedZero();
    TestLargeMultiAccessRemainsNdAffineInsteadOfTruncatingRepeatCount();
    TestNdDmaRequiresOnlyActiveStrideStates();
    TestLoadGmToCbuf2DV2UsesModeZeroFractalSize();
    TestLoadGmToCbuf2DV2SkipsNonzeroModes();
    TestLoadGmToCbuf2DV2SupportsNegativeAndZeroSourceStride();
    TestLoadGmToCbuf2DV2RejectsAddressOverflow();
    TestLoadGmToCbuf2DV2HandlesMissingStateEmptyStepsAndTailOverflow();
    TestFixpipePacked4ProducesExactByteRanges();
    TestFixpipeB8ChannelMergeUsesThirtyTwoChannelGroups();
    TestFixpipeNzRejectsUnsupportedNSizeRemainders();
    TestFixpipePacked4ConversionModeUsesLoop3State();
}

TEST(AclsanMemoryCbdata, ScalarDevRejectsInconsistentFields)
{
    aclsan::ScalarDevParamField field{64, 32, 0x1000, 0};
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::INVALID_FIELD);
    field.instrId = 63;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::INVALID_FIELD);
    field.instrId = 72;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::INVALID_FIELD);
}

TEST(AclsanMemoryCbdata, InternalParameterRangeSuppressesOnlyFullyContainedAccesses)
{
    aclsan::MemoryCbdataContext context{};
    context.parameterBase = 0x4000;
    context.parameterBytes = 32;
    for (uint32_t id : {64U, 68U}) { // ST_DEV.b64 and LD_DEV.b64.
        for (uint64_t address : {0x4000UL, 0x4018UL}) {
            const auto result =
                aclsan::MemoryFieldToCbdataConverter{context}.Convert(aclsan::ScalarDevParamField{id, 64, address, 0});
            EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::NO_ACCESS);
            EXPECT_TRUE(result.data.empty());
        }
        for (uint64_t address : {0x3fffUL, 0x4019UL, 0x4020UL, 0x8000UL}) {
            const auto result =
                aclsan::MemoryFieldToCbdataConverter{context}.Convert(aclsan::ScalarDevParamField{id, 64, address, 0});
            ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
            ASSERT_EQ(result.data.size(), 1U);
            EXPECT_EQ(result.data.front().address, address);
        }
    }
    context.parameterBytes = 0;
    EXPECT_EQ(
        aclsan::MemoryFieldToCbdataConverter{context}.Convert(aclsan::ScalarDevParamField{68, 64, 0x4000, 0}).status,
        aclsan::MemoryCbdataStatus::SUCCESS);
    context.parameterBytes = UINT64_MAX;
    EXPECT_EQ(
        aclsan::MemoryFieldToCbdataConverter{context}.Convert(aclsan::ScalarDevParamField{68, 64, 0x4000, 0}).status,
        aclsan::MemoryCbdataStatus::SUCCESS);
}

TEST(AclsanMemoryCbdata, InternalRangeUsesEntireStridedFootprintAndLaunchContext)
{
    CopyGmToUbufAlignV2ParamField field{};
    field.instrId = RawInstructionId(InstructionId::CopyGmToUbufAlignV2B16);
    field.dataBits = 16;
    field.srcAddr = 0x4000;
    field.burstNum = 3;
    field.burstLen = 32;
    field.burstSrcStride = 64;
    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_UBUF);
    const auto original = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    ASSERT_EQ(original.status, aclsan::MemoryCbdataStatus::SUCCESS);
    aclsan::MemoryCbdataContext context{};
    context.parameterBase = 0x4000;
    context.parameterBytes = 256;
    const auto contained = aclsan::MemoryFieldToCbdataConverter{context, state}.Convert(field);
    EXPECT_EQ(contained.status, aclsan::MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(contained.data.size(), 1u);
    EXPECT_EQ(contained.data.front().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    context.parameterBytes = 32;
    const auto partial = aclsan::MemoryFieldToCbdataConverter{context, state}.Convert(field);
    EXPECT_EQ(partial.status, aclsan::MemoryCbdataStatus::SUCCESS);
    const auto withoutContext = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    EXPECT_EQ(withoutContext.status, aclsan::MemoryCbdataStatus::SUCCESS);
}

TEST(AclsanMemoryCbdata, ScalarDevAddressOverflowIsExplicit)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    for (uint32_t id = 64; id <= 71; ++id) {
        const uint32_t bits = 64U >> ((id - 64) % 4);
        for (const auto& field :
             {aclsan::ScalarDevParamField{id, bits, UINT64_MAX, 1}, aclsan::ScalarDevParamField{id, bits, 0, -1},
              aclsan::ScalarDevParamField{id, bits, 0, INT64_MIN}}) {
            const auto result = converter.Convert(field);
            EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
            EXPECT_TRUE(result.data.empty());
        }
    }
}

TEST(AclsanMemoryCbdata, ScalarPreloadDoesNotProduceMemoryAccessCbdata)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    const std::array<uint32_t, 2> instructionIds{
        RawInstructionId(InstructionId::DcPreload), RawInstructionId(InstructionId::DcPreloadI)};
    for (const uint32_t id : instructionIds) {
        const auto result = converter.Convert(aclsan::ScalarPreloadParamField{id, UINT64_MAX, INT64_MAX});
        EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::NO_ACCESS);
        EXPECT_TRUE(result.data.empty());
    }

    const auto invalid = converter.Convert(aclsan::ScalarPreloadParamField{0, 0x1000, 0});
    EXPECT_EQ(invalid.status, aclsan::MemoryCbdataStatus::INVALID_FIELD);
    EXPECT_TRUE(invalid.data.empty());
}

TEST(AclsanMemoryCbdata, ScalarGmUsesWidthDirectionOffsetFamilyPairAndPost)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    for (uint32_t id = 24; id <= 55; ++id) {
        const uint32_t bits = 64U >> ((id - 24U) % 4U);
        const bool isStore = id <= 43;
        const bool isPair = (id >= 32 && id <= 35) || (id >= 52 && id <= 55);
        const bool hasRegisterOffset = (id >= 28 && id <= 31) || (id >= 40 && id <= 43) || (id >= 48 && id <= 51);
        const std::array<uint64_t, 2> postModes{0UL, isPair ? 0UL : 1UL};
        for (uint64_t post : postModes) {
            SCOPED_TRACE(::testing::Message() << "id=" << id << " post=" << post);
            const aclsan::ScalarGmParamField field{
                id, bits, 0x1000, 3, post, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
            const auto result = converter.Convert(field);
            ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
            ASSERT_EQ(result.data.size(), 1U);
            const auto& access = result.data.front();
            const uint64_t scaledOffset = hasRegisterOffset ? 3U * (bits / 8U) : 3U;
            EXPECT_EQ(access.address, post == 1U ? 0x1000U : 0x1000U + scaledOffset);
            EXPECT_EQ(
                access.accessMode, isStore ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ);
            EXPECT_EQ(access.dataBits, bits);
            EXPECT_EQ(access.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
            EXPECT_EQ(access.layout.range.bytes, (bits / 8U) * (isPair ? 2U : 1U));
            EXPECT_EQ(access.header.sourceKind, isStore ? ACLSAN_DEVICE_SOURCE_ST : ACLSAN_DEVICE_SOURCE_LD);
        }
    }

    const aclsan::ScalarGmParamField pairWithPost{
        32, 64, 0x1000, 3, 1, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
    EXPECT_EQ(converter.Convert(pairWithPost).status, aclsan::MemoryCbdataStatus::INVALID_FIELD);
}

TEST(AclsanMemoryCbdata, ScalarGmValidatesAddressContextAndBounds)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    aclsan::ScalarGmParamField field{
        24, 64, UINT64_C(0x8000000200001000), 0, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
    auto result = converter.Convert(field);
    ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(result.data.size(), 1U);
    EXPECT_EQ(result.data.front().address, UINT64_C(0x200001000));

    field.dataBits = 32;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::INVALID_FIELD);
    field.dataBits = 64;
    field.post = 2;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::INVALID_FIELD);
    field.post = 0;
    field.addressContext = 0;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::MISSING_ADDRESS_CONTEXT);
    field.addressContext = aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;
    field.addr = UINT64_MAX;
    field.offset = 1;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);

    field = {32, 64, UINT64_C(0xfffffffffff8), 0, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
}

TEST(AclsanMemoryCbdata, ParameterFilteringUsesResolvedScalarAddressAndPairWidth)
{
    aclsan::MemoryCbdataContext context{};
    context.parameterBase = 0x4000;
    context.parameterBytes = 32;
    const aclsan::MemoryFieldToCbdataConverter converter{context};
    aclsan::ScalarGmParamField field{RawInstructionId(InstructionId::LdB64Reg), 64, 0x4000, 3, 0, UINT64_C(0x100000000),
                                     aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::NO_ACCESS);
    field.offset = 4;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::SUCCESS);
    field.post = 1;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::NO_ACCESS);
    field.post = 0;
    field.instrId = RawInstructionId(InstructionId::LdpB64);
    field.offset = 16;
    EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::NO_ACCESS);
    field.offset = 17;
    const auto crossing = converter.Convert(field);
    ASSERT_EQ(crossing.status, aclsan::MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(crossing.data.size(), 1U);
    EXPECT_EQ(crossing.data[0].layout.range.bytes, 16U);
}

TEST(AclsanMemoryCbdata, ScalarAtomicWithoutConfigurationPublishesFlaggedOrdinaryWrite)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    struct Case {
        uint32_t id;
        uint32_t bits;
    };
    const Case cases[] = {
        {56, 32}, {57, 16}, {58, 8}, {59, 32}, {60, 16}, {61, 8},
    };
    for (const auto& test : cases) {
        for (uint64_t post : {0UL, 1UL}) {
            SCOPED_TRACE(::testing::Message() << "id=" << test.id << " post=" << post);
            const aclsan::ScalarAtomicParamField field{
                test.id, test.bits, 0x1000, 3, post, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
            const auto result = converter.Convert(field);
            ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
            EXPECT_FALSE(result.requiresIncompleteEvent);
            ASSERT_EQ(result.data.size(), 1U);
            const auto& access = result.data.front();
            EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
            EXPECT_EQ(
                access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT | ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN);
            EXPECT_EQ(access.address, 0x1000U + (post == 1 ? 0U : 3U * (test.id <= 58 ? test.bits / 8U : 1U)));
            EXPECT_EQ(access.dataBits, test.bits);
            EXPECT_EQ(access.layout.range.bytes, test.bits / 8U);
        }
    }
}

TEST(AclsanMemoryCbdata, ScalarAtomicValidatesAddressBeforeOrdinaryWriteFallback)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    for (uint32_t id = 56; id <= 61; ++id) {
        const uint32_t bits = 32U >> ((id - 56) % 3);
        aclsan::ScalarAtomicParamField field{id,
                                             bits,
                                             UINT64_C(0x8000000200001000),
                                             0,
                                             0,
                                             UINT64_C(0x100000000),
                                             aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
        auto result = converter.Convert(field);
        ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(result.data.size(), 1U);
        for (uint64_t local : {UINT64_C(0x100080040), UINT64_C(0x100100040)}) {
            field.addr = local;
            result = converter.Convert(field);
            EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::UNSUPPORTED_ADDRESS_SPACE);
            EXPECT_TRUE(result.data.empty());
        }
        field.addr = UINT64_C(0x100000040);
        EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::INVALID_ADDRESS_SPACE);
        // Classification must happen after offset calculation: cross bit 24 into GM.
        field.addr = UINT64_C(0x100fffffc);
        field.offset = id <= 58 ? 4 / (bits / 8) : 4;
        result = converter.Convert(field);
        ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
        ASSERT_EQ(result.data.size(), 1U);
        field.addressContext = 0;
        EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::MISSING_ADDRESS_CONTEXT);
        field.addressContext = aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1 + 1;
        EXPECT_EQ(converter.Convert(field).status, aclsan::MemoryCbdataStatus::MISSING_ADDRESS_CONTEXT);
    }
    aclsan::ScalarAtomicParamField tail{
        56, 32, UINT64_C(0xfffffffffffe), 0, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1};
    EXPECT_EQ(converter.Convert(tail).status, aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
    // A zero SYS_VA_BASE is valid when the protocol marker is present.
    tail.sysVaBase = 0;
    tail.addr = 0x1000000;
    EXPECT_EQ(converter.Convert(tail).status, aclsan::MemoryCbdataStatus::SUCCESS);
    tail.addr = 0x1000004;
    tail.offset = -1;
    const auto negativeOffset = converter.Convert(tail);
    EXPECT_EQ(negativeOffset.status, aclsan::MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(negativeOffset.data.size(), 1U);
    EXPECT_EQ(negativeOffset.data[0].address, 0x1000000U);
}

TEST(AclsanMemoryCbdata, ScalarAtomicRejectsInconsistentFieldsAndPost)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    for (uint32_t id : {55U, 62U}) {
        EXPECT_EQ(
            converter
                .Convert(aclsan::ScalarAtomicParamField{
                    id, 32, 0x1000, 0, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1})
                .status,
            aclsan::MemoryCbdataStatus::INVALID_FIELD);
    }
    // Width must match the instruction family.
    EXPECT_EQ(
        converter
            .Convert(aclsan::ScalarAtomicParamField{
                56, 16, 0x1000, 0, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1})
            .status,
        aclsan::MemoryCbdataStatus::INVALID_FIELD);
    EXPECT_EQ(
        converter
            .Convert(aclsan::ScalarAtomicParamField{
                61, 32, 0x1000, 0, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1})
            .status,
        aclsan::MemoryCbdataStatus::INVALID_FIELD);
    // Only post == 0 and post == 1 are supported.
    for (uint64_t post : {2UL, UINT64_MAX}) {
        EXPECT_EQ(
            converter
                .Convert(aclsan::ScalarAtomicParamField{
                    58, 8, 0x1000, 0, post, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1})
                .status,
            aclsan::MemoryCbdataStatus::INVALID_FIELD);
    }
}

TEST(AclsanMemoryCbdata, ReadOnlyBinaryFilteringPreservesWritesCrossBoundaryReadsAndAtomicFallback)
{
    const std::vector<aclsan::InternalMemoryRange> ranges{{reinterpret_cast<void*>(0x4000), 32}};
    aclsan::MemoryCbdataContext context{};
    context.readOnlyBinaryRanges = &ranges;
    const aclsan::MemoryFieldToCbdataConverter converter{context};
    for (uint64_t address : {0x4000UL, 0x4018UL}) {
        const auto result = converter.Convert(aclsan::ScalarDevParamField{68, 64, address, 0});
        EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::NO_ACCESS);
        EXPECT_TRUE(result.data.empty());
        const auto write = converter.Convert(aclsan::ScalarDevParamField{64, 64, address, 0});
        ASSERT_EQ(write.data.size(), 1U);
        EXPECT_EQ(write.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    }
    for (uint64_t address : {0x3fffUL, 0x4019UL, 0x4020UL, 0x8000UL}) {
        const auto result = converter.Convert(aclsan::ScalarDevParamField{68, 64, address, 0});
        ASSERT_EQ(result.data.size(), 1U);
        EXPECT_EQ(result.data[0].address, address);
        EXPECT_EQ(result.data[0].layout.range.bytes, 8U);
    }
    const auto atomic = converter.Convert(aclsan::ScalarAtomicParamField{
        56, 32, 0x4000, 0, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1});
    EXPECT_EQ(atomic.status, aclsan::MemoryCbdataStatus::SUCCESS);
    ASSERT_EQ(atomic.data.size(), 1U);
    EXPECT_EQ(atomic.data[0].address, 0x4000U);
    EXPECT_EQ(atomic.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    EXPECT_EQ(
        aclsan::MemoryFieldToCbdataConverter{{}}.Convert(aclsan::ScalarDevParamField{68, 64, 0x4000, 0}).status,
        aclsan::MemoryCbdataStatus::SUCCESS);
}

TEST(AclsanMemoryCbdata, ScalarAtomicAddressOverflowIsExplicit)
{
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    for (uint32_t id = 56; id <= 61; ++id) {
        const uint32_t bits = 32U >> ((id - 56) % 3);
        for (const auto& field :
             {aclsan::ScalarAtomicParamField{
                  id, bits, UINT64_MAX, 1, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1},
              aclsan::ScalarAtomicParamField{
                  id, bits, 0, -1, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1},
              aclsan::ScalarAtomicParamField{
                  id, bits, 0, INT64_MIN, 0, UINT64_C(0x100000000), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1}}) {
            const auto result = converter.Convert(field);
            EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
            EXPECT_TRUE(result.data.empty());
        }
    }
}

TEST(AclsanMemoryCbdata, LocalConversionPreservesMissingRegisterCause)
{
    aclsan::Img2ColParamField field{};
    field.instrId = 153;
    field.mExtension = 16;
    field.kExtension = 16;
    aclsan::MemoryRegisterState state{};
    auto result = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    EXPECT_EQ(result.requiredRegisterInstructionId, RawInstructionId(InstructionId::SetFmatrix));
    EXPECT_FALSE(result.requiresIncompleteEvent);
    EXPECT_TRUE(result.data.empty());

    state.fmatrix[0] = aclsan::FmatrixParamField{};
    result = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::MISSING_REGISTER_STATE);
    EXPECT_EQ(result.requiredRegisterInstructionId, RawInstructionId(InstructionId::SetL3dRpt));
    EXPECT_TRUE(result.data.empty());
}

TEST(AclsanMemoryCbdata, GmOutputWithoutCtrlPublishesExactOrdinaryWrite)
{
    aclsan::CopyUbufToGmAlignV2ParamField field{};
    field.instrId = 83;
    field.dstAddr = 0x1000;
    field.burstNum = 1;
    field.burstLen = 32;
    const auto result =
        aclsan::MemoryFieldToCbdataConverter{{}, SingletonDmaState(aclsan::DmaLoopDirection::UBUF_TO_GM)}.Convert(
            field);
    ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
    EXPECT_FALSE(result.requiresIncompleteEvent);
    ASSERT_EQ(result.data.size(), 2U);
    const auto& access = result.data.front();
    EXPECT_EQ(access.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    EXPECT_EQ(access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    EXPECT_EQ(access.address, 0x1000U);
    EXPECT_EQ(access.layout.range.bytes, 32U);
    EXPECT_EQ(result.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
    EXPECT_EQ(result.data[1].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
}

TEST(AclsanMemoryCbdata, GmDmaAtomicConfigurationFreezesOnlyOutputAccesses)
{
    aclsan::CopyUbufToGmAlignV2ParamField field{};
    field.instrId = 83;
    field.srcAddr = 0x200;
    field.dstAddr = 0x1000;
    field.burstNum = 2;
    field.burstLen = 32;
    auto state = SingletonDmaState(aclsan::DmaLoopDirection::UBUF_TO_GM);
    constexpr uint32_t bits[] = {0, 32, 16, 16, 32, 8, 16};
    for (uint64_t type = 0; type <= 6; ++type) {
        state.ctrl = aclsan::CtrlRegisterParamField{type << 6U};
        const auto result = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
        ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
        EXPECT_FALSE(result.requiresIncompleteEvent);
        ASSERT_GE(result.data.size(), 1U);
        for (const auto& access : result.data) {
            if (access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
                EXPECT_EQ(
                    access.accessMode,
                    type ? ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD : ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
                EXPECT_EQ(access.dataBits, bits[type]);
            } else {
                EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
            }
        }
    }
    state.ctrl = aclsan::CtrlRegisterParamField{1U << 6U};
    const auto enabled = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    state.ctrl = aclsan::CtrlRegisterParamField{2U << 9U}; // none 会保留 min 操作位
    const auto disabled = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    ASSERT_EQ(disabled.status, aclsan::MemoryCbdataStatus::SUCCESS);
    EXPECT_EQ(enabled.data.front().accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    EXPECT_EQ(disabled.data.front().accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
}

TEST(AclsanMemoryCbdata, UnsupportedAtomicModesCannotBecomeExactWrites)
{
    aclsan::CopyUbufToGmAlignV2ParamField field{};
    field.instrId = 83;
    field.dstAddr = 0x1000;
    field.burstNum = 1;
    field.burstLen = 32;
    auto state = SingletonDmaState(aclsan::DmaLoopDirection::UBUF_TO_GM);
    for (uint64_t ctrl : {uint64_t((1U << 6U) | (3U << 9U)), uint64_t(7U << 6U)}) {
        state.ctrl = aclsan::CtrlRegisterParamField{ctrl};
        const auto result = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
        ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::PARTIAL_COVERAGE);
        EXPECT_TRUE(result.requiresIncompleteEvent);
        for (const auto& access : result.data) {
            EXPECT_NE(access.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
            EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        }
    }
}

TEST(AclsanMemoryCbdata, FixpipeAtomicChangesAllOutputSegmentsAndPreservesL0cRead)
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = 91;
    field.dstAddr = 0x1000;
    field.nSize = 16;
    field.mSize = 16;
    field.loopDstStride = 256;
    field.loopSrtStride = 16;
    aclsan::MemoryRegisterState state{};
    state.ctrl = aclsan::CtrlRegisterParamField{1U << 6U};
    const auto result = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
    ASSERT_GE(result.data.size(), 2U);
    for (const auto& access : result.data) {
        EXPECT_EQ(
            access.accessMode, access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ?
                                   ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD :
                                   ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    }
    state.ctrl = aclsan::CtrlRegisterParamField{5U << 6U};
    const auto mismatch = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
    EXPECT_EQ(mismatch.status, aclsan::MemoryCbdataStatus::PARTIAL_COVERAGE);
}

TEST(AclsanMemoryCbdata, MissingCtrlPreservesFixpipeSourceReadAndExactGmWrite)
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = 91;
    field.dstAddr = 0x1000;
    field.nSize = 16;
    field.mSize = 16;
    field.loopDstStride = 256;
    field.loopSrtStride = 16;
    const auto result = aclsan::MemoryFieldToCbdataConverter{{}}.Convert(field);
    ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
    EXPECT_FALSE(result.requiresIncompleteEvent);
    ASSERT_EQ(result.data.size(), 2U);
    EXPECT_EQ(result.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    EXPECT_EQ(result.data[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    EXPECT_EQ(result.data[0].header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    EXPECT_EQ(result.data[1].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L0C);
    EXPECT_EQ(result.data[1].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    EXPECT_EQ(result.data[1].header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
}

TEST(AclsanMemoryCbdata, FixpipeAtomicCoversS32AndQuantizedOutputSegments)
{
    struct Case {
        uint32_t id;
        uint8_t quantPre;
        uint64_t atomicType;
        uint32_t bits;
    };
    for (const auto& test :
         {Case{92, 0, 4, 32}, Case{91, 1, 2, 16}, Case{92, 10, 3, 16}, Case{91, 24, 5, 8}, Case{92, 8, 5, 8}}) {
        SCOPED_TRACE(::testing::Message() << "id=" << test.id << " quantPre=" << unsigned(test.quantPre));
        aclsan::FixL0cToOutParamField field{};
        field.instrId = test.id;
        field.quantPre = test.quantPre;
        field.dstAddr = 0x8000;
        field.nSize = 48;
        field.mSize = 4;
        field.loopDstStride = 64;
        field.loopSrtStride = 16;
        aclsan::MemoryRegisterState state{};
        state.ctrl = aclsan::CtrlRegisterParamField{test.atomicType << 6U};
        const auto result = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
        ASSERT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
        size_t gmCount = 0;
        for (const auto& access : result.data) {
            if (access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
                ++gmCount;
                EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
                EXPECT_EQ(access.dataBits, test.bits);
                EXPECT_EQ(access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
                EXPECT_EQ(access.header.version, ACLSAN_API_VERSION);
            } else {
                EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
            }
        }
        EXPECT_GT(gmCount, 0U);
        if (test.quantPre == 24) {
            EXPECT_EQ(gmCount, 2U);
        }
    }
}

TEST(AclsanMemoryCbdata, AddMaxMinAllProduceAtomicWritesForDmaAndFixpipe)
{
    for (uint64_t operation = 0; operation <= 2; ++operation) {
        SCOPED_TRACE(operation);
        auto state = SingletonDmaState(aclsan::DmaLoopDirection::UBUF_TO_GM);
        state.ctrl = aclsan::CtrlRegisterParamField{(1U << 6U) | (operation << 9U)};
        aclsan::CopyUbufToGmAlignV2ParamField dma{};
        dma.instrId = 83;
        dma.dstAddr = 0x1000;
        dma.burstNum = 1;
        dma.burstLen = 32;
        aclsan::FixL0cToOutParamField fix{};
        fix.instrId = 91;
        fix.dstAddr = 0x1000;
        fix.nSize = 16;
        fix.mSize = 16;
        fix.loopDstStride = 256;
        fix.loopSrtStride = 16;
        for (const auto& field : {aclsan::MemoryInstructionField{dma}, aclsan::MemoryInstructionField{fix}}) {
            const auto result = aclsan::MemoryFieldToCbdataConverter{{}, state}.Convert(field);
            EXPECT_EQ(result.status, aclsan::MemoryCbdataStatus::SUCCESS);
            ASSERT_FALSE(result.data.empty());
            for (const auto& access : result.data) {
                EXPECT_EQ(access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
                EXPECT_EQ(
                    access.accessMode, access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ?
                                           (operation == 0 ? ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD :
                                            operation == 1 ? ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX :
                                                             ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN) :
                                           ACLSAN_DEVICE_MEMORY_ACCESS_READ);
            }
        }
    }
}
