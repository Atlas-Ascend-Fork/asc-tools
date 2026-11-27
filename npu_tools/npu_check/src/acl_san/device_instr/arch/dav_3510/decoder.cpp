/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_cbdata_device.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "device_instr/arch/dav_3510/dma_layout.h"
#include "device_instr/common/bit_range.h"
#include "device_instr/common/instruction_id.h"
#include "dbi/trace_buffer_abi.h"

#include <array>

namespace aclsan::dav3510 {
namespace {

constexpr uint32_t DATA_BITS_B8 = 8;
constexpr uint32_t DATA_BITS_B16 = 16;
constexpr uint32_t DATA_BITS_B32 = 32;
constexpr uint32_t DATA_BITS_B64 = 64;
constexpr uint32_t CROSS_CORE_MODE_SHIFT = 4U;
constexpr uint32_t CROSS_CORE_MODE_MASK = 0x3U;
constexpr uint32_t CROSS_CORE_FLAG_ID_SHIFT = 8U;
constexpr uint32_t CROSS_CORE_FLAG_ID_MASK = 0xFU;
constexpr uint32_t CROSS_CORE_RECEIVER_FLAG_MODE = 4U;
constexpr uint64_t BOOLEAN_MAX_VALUE = 1U;

constexpr uint32_t RawId(InstructionId instructionId) noexcept { return static_cast<uint32_t>(instructionId); }

DecodedInstruction DecodeVtranspose(const AclsanRawTraceRecord& record) noexcept
{
    return {DeviceInstructionKind::Vtranspose, VtransposeParamField{record.instrId, record.args[0], record.args[1]}};
}

DecodedInstruction DecodeVbs32(const AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    return {
        DeviceInstructionKind::Vbs32,
        Vbs32ParamField{record.instrId, dataBits, record.args[0], record.args[1], record.args[2], record.args[3]}};
}

DecodedInstruction DecodeVmrgsort(const AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    return {
        DeviceInstructionKind::Vmrgsort,
        VmrgsortParamField{record.instrId, dataBits, record.args[0], record.args[1], record.args[2], record.args[3]}};
}

std::optional<DecodedInstruction> DecodeVnchwconv(const AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    if (record.args[0] >= VA_REGISTER_COUNT || record.args[1] >= VA_REGISTER_COUNT || (record.args[0] & 1U) != 0 ||
        (record.args[1] & 1U) != 0) {
        return std::nullopt;
    }
    VnchwconvParamField f{};
    f.instrId = record.instrId;
    f.dataBits = dataBits;
    f.dstVaIndex = static_cast<uint32_t>(record.args[0]);
    f.srcVaIndex = static_cast<uint32_t>(record.args[1]);
    f.config = record.args[2];
    if (dataBits == DATA_BITS_B8) {
        if (record.args[3] > 1 || record.args[4] > 1) {
            return std::nullopt;
        }
        f.dstHighHalf = record.args[3] != 0;
        f.srcHighHalf = record.args[4] != 0;
    }
    return DecodedInstruction{DeviceInstructionKind::Vnchwconv, f};
}

template <auto DecodeFamily, uint32_t dataBits>
std::optional<DecodedInstruction> DecodeVectorUb(const AclsanRawTraceRecord& record) noexcept
{
    return DecodeFamily(record, dataBits);
}

std::optional<DecodedInstruction> DecodeMoveVa(const AclsanRawTraceRecord& record) noexcept
{
    if (record.args[0] >= VA_REGISTER_COUNT || record.args[1] >= VA_ENTRY_COUNT || (record.args[1] & 1U) != 0) {
        return std::nullopt;
    }
    return DecodedInstruction{
        DeviceInstructionKind::MoveVa, MoveVaParamField{
                                           static_cast<uint32_t>(record.args[0]), static_cast<uint32_t>(record.args[1]),
                                           record.args[2], record.args[3]}};
}

std::optional<DecodedInstruction> DecodeLdva(const AclsanRawTraceRecord& record) noexcept
{
    if (record.args[0] >= VA_REGISTER_COUNT || record.args[2] > 1) {
        return std::nullopt;
    }
    return DecodedInstruction{
        DeviceInstructionKind::Ldva,
        LdvaParamField{static_cast<uint32_t>(record.args[0]), record.args[1], record.args[2] != 0}};
}

template <typename ParamField>
ParamField DecodeMovAlignV2Params(const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    ParamField params{};
    params.instrId = record.instrId;
    params.dataBits = dataBits;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.sid = static_cast<uint8_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::SID));
    params.burstNum = static_cast<uint32_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::BURST_NUM));
    params.burstLen = static_cast<uint32_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::BURST_LEN));
    params.leftPaddingCount =
        static_cast<uint8_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::LEFT_PADDING_COUNT));
    params.rightPaddingCount =
        static_cast<uint8_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::RIGHT_PADDING_COUNT));
    params.dataSelectBit = ExtractBitRange(record.args[2], MovAlignV2Layout::DATA_SELECT_BIT) != 0;
    params.l2CacheControl = static_cast<uint8_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::L2_CACHE_CONTROL));
    params.burstSrcStride = ExtractBitRange(record.args[3], MovAlignV2Layout::BURST_SRC_STRIDE);
    params.burstDstStride = static_cast<uint32_t>(ExtractBitRange(record.args[3], MovAlignV2Layout::BURST_DST_STRIDE));

    return params;
}

template <typename ParamField>
std::optional<DecodedInstruction> DecodeMovAlignV2(
    const aclsan::AclsanRawTraceRecord& record, DeviceInstructionKind kind, uint32_t dataBits) noexcept
{
    const ParamField params = DecodeMovAlignV2Params<ParamField>(record, dataBits);
    return DecodedInstruction{kind, params};
}

std::optional<DecodedInstruction> DecodeCopyUbufToGmAlignV2(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    CopyUbufToGmAlignV2ParamField params{};
    params.instrId = record.instrId;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.sid = static_cast<uint8_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::SID));
    params.burstNum = static_cast<uint32_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::BURST_NUM));
    params.burstLen = static_cast<uint32_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::BURST_LEN));
    params.l2CacheControl = static_cast<uint8_t>(ExtractBitRange(record.args[2], MovAlignV2Layout::L2_CACHE_CONTROL));
    params.dstStride = ExtractBitRange(record.args[3], MovAlignV2Layout::BURST_SRC_STRIDE);
    params.srcStride = static_cast<uint32_t>(ExtractBitRange(record.args[3], MovAlignV2Layout::BURST_DST_STRIDE));

    return DecodedInstruction{DeviceInstructionKind::CopyUbufToGmAlignV2, params};
}

std::optional<DecodedInstruction> DecodeCopyUbufToUbuf(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    CopyUbufToUbufParamField params{};
    params.instrId = record.instrId;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.burstNum = static_cast<uint16_t>(ExtractBitRange(record.args[2], CopyUbufToUbufLayout::BURST_NUM));
    params.burstLen = static_cast<uint16_t>(ExtractBitRange(record.args[2], CopyUbufToUbufLayout::BURST_LEN));
    params.srcGap = static_cast<uint16_t>(ExtractBitRange(record.args[2], CopyUbufToUbufLayout::SRC_GAP));
    params.dstGap = static_cast<uint16_t>(ExtractBitRange(record.args[2], CopyUbufToUbufLayout::DST_GAP));

    return DecodedInstruction{DeviceInstructionKind::CopyUbufToUbuf, params};
}

template <typename ParamField>
DecodedInstruction DecodeCopyGmToCbufMulti(const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    ParamField params{};
    params.instrId = record.instrId;
    params.dataBits = dataBits;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.sid = static_cast<uint8_t>(ExtractBitRange(record.args[2], CopyGmToCbufMultiLayout::SID));
    params.loop1SrcStride = ExtractBitRange(record.args[2], CopyGmToCbufMultiLayout::LOOP1_SRC_STRIDE);
    params.l2CacheControl =
        static_cast<uint8_t>(ExtractBitRange(record.args[2], CopyGmToCbufMultiLayout::L2_CACHE_CONTROL));
    params.nValue = static_cast<uint16_t>(ExtractBitRange(record.args[2], CopyGmToCbufMultiLayout::N_VALUE));
    params.dValue = static_cast<uint32_t>(ExtractBitRange(record.args[3], CopyGmToCbufMultiLayout::D_VALUE));
    params.loop4SrcStride = ExtractBitRange(record.args[3], CopyGmToCbufMultiLayout::LOOP4_SRC_STRIDE);
    params.smallC0Enable = ExtractBitRange(record.args[3], CopyGmToCbufMultiLayout::SMALL_C0_ENABLE) != 0;

    return DecodedInstruction{DeviceInstructionKind::CopyGmToCbufMulti, params};
}

std::optional<DecodedInstruction> DecodeCopyGmToCbufV2(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    CopyGmToCbufV2ParamField params{};
    params.instrId = record.instrId;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.sid = static_cast<uint8_t>(ExtractBitRange(record.args[2], CopyGmToCbufV2Layout::SID));
    params.burstNum = static_cast<uint32_t>(ExtractBitRange(record.args[2], CopyGmToCbufV2Layout::BURST_NUM));
    params.burstLen = static_cast<uint32_t>(ExtractBitRange(record.args[2], CopyGmToCbufV2Layout::BURST_LEN));
    params.padFunctionMode =
        static_cast<uint8_t>(ExtractBitRange(record.args[2], CopyGmToCbufV2Layout::PAD_FUNCTION_MODE));
    params.l2CacheControl =
        static_cast<uint8_t>(ExtractBitRange(record.args[2], CopyGmToCbufV2Layout::L2_CACHE_CONTROL));
    params.srcStride = ExtractBitRange(record.args[3], CopyGmToCbufV2Layout::SRC_STRIDE);
    params.dstStride = static_cast<uint32_t>(ExtractBitRange(record.args[3], CopyGmToCbufV2Layout::DST_STRIDE));

    return DecodedInstruction{DeviceInstructionKind::CopyGmToCbufV2, params};
}

DecodedInstruction DecodeLoadGmToCbuf2DV2(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    LoadGmToCbuf2DV2ParamField params{};
    params.instrId = record.instrId;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.mStartPosition =
        static_cast<uint32_t>(ExtractBitRange(record.args[2], LoadGmToCbuf2DV2Layout::M_START_POSITION));
    params.kStartPosition =
        static_cast<uint32_t>(ExtractBitRange(record.args[2], LoadGmToCbuf2DV2Layout::K_START_POSITION));
    params.dstStride = static_cast<uint16_t>(ExtractBitRange(record.args[3], LoadGmToCbuf2DV2Layout::DST_STRIDE));
    params.mStep = static_cast<uint16_t>(ExtractBitRange(record.args[3], LoadGmToCbuf2DV2Layout::M_STEP));
    params.kStep = static_cast<uint16_t>(ExtractBitRange(record.args[3], LoadGmToCbuf2DV2Layout::K_STEP));
    params.sid = static_cast<uint8_t>(ExtractBitRange(record.args[3], LoadGmToCbuf2DV2Layout::SID));
    params.decompMode = static_cast<uint8_t>(ExtractBitRange(record.args[3], LoadGmToCbuf2DV2Layout::DECOMP_MODE));
    params.l2CacheControl =
        static_cast<uint8_t>(ExtractBitRange(record.args[3], LoadGmToCbuf2DV2Layout::L2_CACHE_CONTROL));

    return DecodedInstruction{DeviceInstructionKind::LoadGmToCbuf2DV2, params};
}

DecodedInstruction DecodeNdDmaOutToUbuf(const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    NdDmaOutToUbufParamField params{};
    params.instrId = record.instrId;
    params.dataBits = dataBits;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.sid = static_cast<uint8_t>(ExtractBitRange(record.args[2], NdDmaLayout::SID));
    params.loop0Size = static_cast<uint32_t>(ExtractBitRange(record.args[2], NdDmaLayout::LOOP0_SIZE));
    params.loop1Size = static_cast<uint32_t>(ExtractBitRange(record.args[2], NdDmaLayout::LOOP1_SIZE));
    params.loop2Size = static_cast<uint32_t>(ExtractBitRange(record.args[2], NdDmaLayout::LOOP2_SIZE));
    params.loop3Size = static_cast<uint32_t>(ExtractBitRange(record.args[3], NdDmaLayout::LOOP3_SIZE));
    params.loop4Size = static_cast<uint32_t>(ExtractBitRange(record.args[3], NdDmaLayout::LOOP4_SIZE));
    params.loop0LeftPaddingCount =
        static_cast<uint8_t>(ExtractBitRange(record.args[3], NdDmaLayout::LOOP0_LEFT_PADDING_COUNT));
    params.loop0RightPaddingCount =
        static_cast<uint8_t>(ExtractBitRange(record.args[3], NdDmaLayout::LOOP0_RIGHT_PADDING_COUNT));
    params.paddingMode = ExtractBitRange(record.args[3], NdDmaLayout::PADDING_MODE) != 0;
    params.l2CacheControl = static_cast<uint8_t>(ExtractBitRange(record.args[3], NdDmaLayout::L2_CACHE_CONTROL));

    return DecodedInstruction{DeviceInstructionKind::NdDmaOutToUbuf, params};
}

DecodedInstruction DecodeSetL12D(const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    SetL12DParamField params{};
    params.instrId = record.instrId;
    params.dataBits = dataBits;
    params.dstAddr = record.args[0];
    params.repeatTimes = static_cast<uint16_t>(ExtractBitRange(record.args[1], SetL12DLayout::REPEAT_TIMES));
    params.blockNum = static_cast<uint16_t>(ExtractBitRange(record.args[1], SetL12DLayout::BLOCK_NUM));
    params.repeatGap = static_cast<uint16_t>(ExtractBitRange(record.args[1], SetL12DLayout::REPEAT_GAP));

    return DecodedInstruction{DeviceInstructionKind::SetL12D, params};
}

DecodedInstruction DecodeSetPadding(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    const SetPaddingParamField params{record.args[0]};
    return DecodedInstruction{DeviceInstructionKind::SetPadding, params};
}

DecodedInstruction DecodeMte2SourceParam(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    const int64_t stride = static_cast<int32_t>(static_cast<uint32_t>(record.args[0]));
    return DecodedInstruction{DeviceInstructionKind::Mte2SourceParam, Mte2SourceParamField{stride}};
}

DecodedInstruction DecodeNdDmaPadCount(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    NdDmaPadCountParamField params{};
    constexpr uint32_t PADDING_BITS_PER_LOOP = 16;
    constexpr uint32_t RIGHT_PADDING_OFFSET = 8;
    constexpr uint64_t PADDING_MASK = 0xFF;
    for (uint32_t loopIndex = 0; loopIndex < params.leftPaddingCounts.size(); ++loopIndex) {
        const uint32_t offset = loopIndex * PADDING_BITS_PER_LOOP;
        params.leftPaddingCounts[loopIndex] = static_cast<uint8_t>((record.args[0] >> offset) & PADDING_MASK);
        params.rightPaddingCounts[loopIndex] =
            static_cast<uint8_t>((record.args[0] >> (offset + RIGHT_PADDING_OFFSET)) & PADDING_MASK);
    }
    return DecodedInstruction{DeviceInstructionKind::NdDmaPadCount, params};
}

DecodedInstruction DecodeNdDmaLoopStride(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    const uint32_t first = RawId(InstructionId::NdDmaLoop0Stride);
    const auto loopIndex = record.instrId - first;
    const uint64_t stride = (record.args[0] >> 20U) & ((1ULL << 40U) - 1U);
    return DecodedInstruction{
        DeviceInstructionKind::NdDmaLoopStride,
        NdDmaLoopStrideParamField{loopIndex, stride, static_cast<uint32_t>(record.args[0] & 0xfffffU)}};
}

DecodedInstruction DecodeMte2NzParam(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    const uint64_t config = record.args[0];
    const Mte2NzParamField params{
        static_cast<uint16_t>(config), static_cast<uint16_t>(config >> 16U), static_cast<uint16_t>(config >> 32U),
        static_cast<uint16_t>(config >> 48U)};
    return DecodedInstruction{DeviceInstructionKind::Mte2NzParam, params};
}

DecodedInstruction DecodeLoop3Param(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    const uint64_t config = record.args[0];
    const Loop3ParamField params{
        static_cast<uint16_t>(config), static_cast<uint16_t>(config >> 16U), static_cast<uint32_t>(config >> 32U)};
    return DecodedInstruction{DeviceInstructionKind::Loop3Param, params};
}

DecodedInstruction DecodeDmaLoopSize(const aclsan::AclsanRawTraceRecord& record, DmaLoopDirection direction) noexcept
{
    constexpr uint64_t LOOP1_SIZE_MASK = (1ULL << 21U) - 1U;
    constexpr uint64_t LOOP2_SIZE_MASK = (1ULL << 42U) - 1U;
    const DmaLoopSizeParamField params{
        direction, static_cast<uint32_t>(record.args[0] & LOOP1_SIZE_MASK), (record.args[0] >> 21U) & LOOP2_SIZE_MASK};
    return DecodedInstruction{DeviceInstructionKind::DmaLoopSize, params};
}

DecodedInstruction DecodeDmaLoopStride(
    const aclsan::AclsanRawTraceRecord& record, DmaLoopDirection direction, uint32_t loopIndex) noexcept
{
    constexpr uint64_t GM_STRIDE_MASK = (1ULL << 40U) - 1U;
    constexpr uint64_t LOCAL_STRIDE_MASK = (1ULL << 21U) - 1U;
    const uint64_t lowStride = record.args[0] & GM_STRIDE_MASK;
    const uint64_t highStride = (record.args[0] >> 40U) & LOCAL_STRIDE_MASK;
    const bool sourceIsGm = direction != DmaLoopDirection::UBUF_TO_GM;
    const DmaLoopStrideParamField params{
        direction, loopIndex, sourceIsGm ? lowStride : highStride, sourceIsGm ? highStride : lowStride};
    return DecodedInstruction{DeviceInstructionKind::DmaLoopStride, params};
}

DecodedInstruction DecodeLocalMemoryTransfer(
    const aclsan::AclsanRawTraceRecord& record, LocalMemoryTransferKind kind) noexcept
{
    LocalMemoryTransferParamField params{};
    params.instrId = static_cast<uint32_t>(record.instrId);
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.config0 = record.args[2];
    params.config1 = record.args[3];
    params.kind = kind;
    if (kind == LocalMemoryTransferKind::CopyCbufToFbuf || kind == LocalMemoryTransferKind::CopyCbufToBt) {
        params.nBurst = static_cast<uint16_t>(ExtractBitRange(record.args[2], LocalMemoryTransferLayout::N_BURST));
        params.lenBurst = static_cast<uint16_t>(ExtractBitRange(record.args[2], LocalMemoryTransferLayout::LEN_BURST));
    }
    if (kind == LocalMemoryTransferKind::CopyCbufToFbuf) {
        params.srcStride =
            static_cast<uint16_t>(ExtractBitRange(record.args[2], LocalMemoryTransferLayout::SRC_STRIDE));
        params.dstStride =
            static_cast<uint16_t>(ExtractBitRange(record.args[2], LocalMemoryTransferLayout::DST_STRIDE));
        params.dstMemBlock =
            static_cast<uint8_t>(ExtractBitRange(record.args[0], LocalMemoryTransferLayout::DST_MEM_BLOCK));
    } else if (kind == LocalMemoryTransferKind::CopyCbufToBt) {
        params.srcGap = static_cast<uint16_t>(ExtractBitRange(record.args[2], LocalMemoryTransferLayout::SRC_STRIDE));
        params.dstGap = static_cast<uint16_t>(ExtractBitRange(record.args[2], LocalMemoryTransferLayout::DST_STRIDE));
        params.cvtEnable = ExtractBitRange(record.args[2], LocalMemoryTransferLayout::CVT_ENABLE) != 0;
    }
    if (kind == LocalMemoryTransferKind::CopyCbufToUbuf) {
        params.subBlockId = ExtractBitRange(record.args[2], LocalMemoryTransferLayout::SUB_BLOCK_ID) != 0;
    } else if (kind == LocalMemoryTransferKind::FixL0cToUbufF32 || kind == LocalMemoryTransferKind::FixL0cToUbufS32) {
        params.dualDstCtl =
            static_cast<uint8_t>(ExtractBitRange(record.args[3], LocalMemoryTransferLayout::DUAL_DST_CONTROL));
        params.subBlockId = ExtractBitRange(record.args[3], LocalMemoryTransferLayout::SUB_BLOCK_CONTROL) != 0;
    }
    return DecodedInstruction{DeviceInstructionKind::LocalMemoryTransfer, params};
}

template <typename ParamField>
DecodedInstruction DecodeFixL0c(
    const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits, DeviceInstructionKind kind) noexcept
{
    ParamField params{};
    params.instrId = static_cast<uint32_t>(record.instrId);
    params.dataBits = dataBits;
    params.dstAddr = record.args[0];
    params.srcAddr = record.args[1];
    params.sid = static_cast<uint8_t>(ExtractBitRange(record.args[2], FixL0cToOutLayout::SID));
    params.nSize = static_cast<uint16_t>(ExtractBitRange(record.args[2], FixL0cToOutLayout::N_SIZE));
    params.mSize = static_cast<uint16_t>(ExtractBitRange(record.args[2], FixL0cToOutLayout::M_SIZE));
    params.loopDstStride = static_cast<uint32_t>(ExtractBitRange(record.args[2], FixL0cToOutLayout::LOOP_DST_STRIDE));
    params.loopSrtStride = static_cast<uint16_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::LOOP_SRT_STRIDE));
    params.l2CacheControl = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::L2_CACHE_CONTROL));
    params.clipReluPre = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::CLIP_RELU_PRE));
    params.unitFlag = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::UNIT_FLAG));
    const uint8_t quantPreHigh =
        static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::QUANT_PRE_HIGH));
    const uint8_t quantPreLow = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::QUANT_PRE_LOW));
    params.quantPre = static_cast<uint8_t>((static_cast<unsigned int>(quantPreHigh) << 5U) | quantPreLow);
    params.reluPre = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::RELU_PRE));
    params.splitEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::SPLIT_ENABLE) != 0;
    params.nz2ndEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::NZ2ND_ENABLE) != 0;
    params.quantPost = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::QUANT_POST));
    params.reluPost = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::RELU_POST));
    params.clipReluPost = ExtractBitRange(record.args[3], FixL0cToOutLayout::CLIP_RELU_POST) != 0;
    params.loopEnhanceEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::LOOP_ENHANCE_ENABLE) != 0;
    params.eltwiseOp = static_cast<uint8_t>(ExtractBitRange(record.args[3], FixL0cToOutLayout::ELTWISE_OP));
    params.eltwiseAntqEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::ELTWISE_ANTQ_ENABLE) != 0;
    params.loopEnhanceMergeEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::LOOP_ENHANCE_MERGE_ENABLE) != 0;
    params.c0PadEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::C0_PAD_ENABLE) != 0;
    params.winoPostEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::WINO_POST_ENABLE) != 0;
    params.brcbEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::BRCB_ENABLE) != 0;
    params.nz2dnEnable = ExtractBitRange(record.args[3], FixL0cToOutLayout::NZ2DN_ENABLE) != 0;

    return DecodedInstruction{kind, params};
}

std::optional<DecodedInstruction> DecodeFlag(
    const aclsan::AclsanRawTraceRecord& record, DeviceInstructionKind kind) noexcept
{
    const FlagParamField params{
        record.instrId, static_cast<uint32_t>(record.args[0]), static_cast<uint32_t>(record.args[1]), record.args[2]};
    return DecodedInstruction{kind, params};
}

std::optional<DecodedInstruction> DecodeHardwareFlag(
    const aclsan::AclsanRawTraceRecord& record, DeviceInstructionKind kind) noexcept
{
    if (record.args[0] > UINT32_MAX || record.args[1] > UINT32_MAX || record.args[4] > BOOLEAN_MAX_VALUE) {
        return std::nullopt;
    }
    const HardwareFlagParamField params{
        record.instrId,
        static_cast<uint32_t>(record.args[0]),
        static_cast<uint32_t>(record.args[1]),
        record.args[2],
        record.args[3],
        record.args[4] != 0};
    return DecodedInstruction{kind, params};
}

std::optional<DecodedInstruction> DecodeBuffer(
    const aclsan::AclsanRawTraceRecord& record, DeviceInstructionKind kind) noexcept
{
    if (record.args[0] > UINT32_MAX || record.args[2] > UINT8_MAX) {
        return std::nullopt;
    }
    const SyncBufParamField params{
        record.instrId, static_cast<uint32_t>(record.args[0]), record.args[1], static_cast<uint8_t>(record.args[2])};
    return DecodedInstruction{kind, params};
}

DecodedInstruction DecodeScalarDev(
    const aclsan::AclsanRawTraceRecord& record, DeviceInstructionKind kind, uint32_t dataBits) noexcept
{
    const ScalarDevParamField params{record.instrId, dataBits, record.args[0], static_cast<int64_t>(record.args[1])};
    return DecodedInstruction{kind, params};
}

DecodedInstruction DecodeScalarPreload(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    const int64_t offset = record.instrId == RawId(InstructionId::DcPreloadI) ?
                               static_cast<int64_t>(static_cast<int16_t>(record.args[1])) :
                               static_cast<int64_t>(record.args[1]);
    return DecodedInstruction{
        DeviceInstructionKind::ScalarPreload, ScalarPreloadParamField{record.instrId, record.args[0], offset}};
}

DecodedInstruction DecodeScalarAtomic(const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    const ScalarAtomicParamField params{
        record.instrId, dataBits,       record.args[0],       static_cast<int64_t>(record.args[1]),
        record.args[2], record.args[3], record.atomicMetadata};
    return DecodedInstruction{DeviceInstructionKind::ScalarAtomic, params};
}

uint64_t DecodeScalarAtomRedSysVaBase(uint32_t metadata) noexcept
{
    return static_cast<uint64_t>(metadata & ASCSAN_SCALAR_SYS_VA_WINDOW_MASK) << ASCSAN_SCALAR_SYS_VA_WINDOW_SHIFT;
}

DecodedInstruction DecodeScalarAtom(const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    const uint64_t sysVaBase = DecodeScalarAtomRedSysVaBase(record.atomicMetadata);
    const ScalarAtomParamField params{record.instrId, dataBits,       record.args[0], record.args[1],
                                      record.args[2], record.args[3], sysVaBase,      record.args[4]};
    return DecodedInstruction{DeviceInstructionKind::ScalarAtom, params};
}

DecodedInstruction DecodeScalarRed(const aclsan::AclsanRawTraceRecord& record, uint32_t dataBits) noexcept
{
    const uint64_t sysVaBase = DecodeScalarAtomRedSysVaBase(record.atomicMetadata);
    const ScalarRedParamField params{record.instrId, dataBits,  record.args[0],
                                     record.args[1], sysVaBase, record.args[2]};
    return DecodedInstruction{DeviceInstructionKind::ScalarRed, params};
}

DecodedInstruction DecodeScalarGm(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    constexpr uint32_t WIDTHS_PER_FAMILY = 4;
    const uint32_t dataBits = DATA_BITS_B64 >> ((record.instrId - RawId(InstructionId::StB64Imm)) % WIDTHS_PER_FAMILY);
    const ScalarGmParamField params{record.instrId, dataBits,      record.args[0], static_cast<int64_t>(record.args[1]),
                                    record.args[2], record.args[3]};
    return DecodedInstruction{DeviceInstructionKind::ScalarGm, params};
}

std::optional<DecodedInstruction> DecodeCrossCore(
    const aclsan::AclsanRawTraceRecord& record, DeviceInstructionKind kind) noexcept
{
    uint32_t pipe = 0;
    uint32_t mode = 0;
    uint64_t flagId = 0;
    switch (static_cast<InstructionId>(record.instrId)) {
        case InstructionId::FftsCrossCoreSync:
        case InstructionId::FftsCrossCoreSyncV:
            pipe = static_cast<uint32_t>(record.args[0]);
            mode = static_cast<uint32_t>((record.args[1] >> CROSS_CORE_MODE_SHIFT) & CROSS_CORE_MODE_MASK);
            flagId = (record.args[1] >> CROSS_CORE_FLAG_ID_SHIFT) & CROSS_CORE_FLAG_ID_MASK;
            break;
        case InstructionId::WaitFlagDev:
        case InstructionId::WaitFlagDevI:
            pipe = static_cast<uint32_t>(record.args[0]);
            flagId = record.args[1];
            break;
        case InstructionId::WaitFlagDevV:
        case InstructionId::WaitFlagDevIV:
            pipe = ACLSAN_DEVICE_PIPE_VECTOR;
            flagId = record.args[0];
            break;
        case InstructionId::SetIntraBlock:
        case InstructionId::SetIntraBlockI:
        case InstructionId::SetIntraBlockV:
        case InstructionId::SetIntraBlockIV:
            pipe = static_cast<uint32_t>(record.args[0]);
            mode = CROSS_CORE_RECEIVER_FLAG_MODE;
            flagId = record.args[1];
            break;
        case InstructionId::WaitIntraBlock:
        case InstructionId::WaitIntraBlockI:
        case InstructionId::WaitIntraBlockV:
        case InstructionId::WaitIntraBlockIV:
            pipe = static_cast<uint32_t>(record.args[0]);
            flagId = record.args[1];
            break;
        default:
            return std::nullopt;
    }
    return DecodedInstruction{kind, CrossCoreParamField{record.instrId, pipe, mode, flagId}};
}

using DecodeHandler = std::optional<DecodedInstruction> (*)(const aclsan::AclsanRawTraceRecord&) noexcept;
using DecodeTable = std::array<DecodeHandler, RawId(InstructionId::PushPb) + 1U>;

template <typename... Ids>
constexpr void RegisterDecoders(DecodeTable& table, DecodeHandler handler, Ids... ids) noexcept
{
    ((table[RawId(ids)] = handler), ...);
}

constexpr DecodeTable MakeDecodeTable() noexcept
{
    DecodeTable table{};
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodedInstruction{
                DeviceInstructionKind::PushPb,
                PushPbParamField{{record.args[0], record.args[1], record.args[2], record.args[3]}}};
        },
        InstructionId::PushPb);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodedInstruction{DeviceInstructionKind::Vf, VfParamField{record.args[0], record.args[1]}};
        },
        InstructionId::Vf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtomic(record, DATA_BITS_B32);
        },
        InstructionId::StAtomicB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtomic(record, DATA_BITS_B16);
        },
        InstructionId::StAtomicB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtomic(record, DATA_BITS_B8);
        },
        InstructionId::StAtomicB8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtomic(record, DATA_BITS_B32);
        },
        InstructionId::StiAtomicB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtomic(record, DATA_BITS_B16);
        },
        InstructionId::StiAtomicB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtomic(record, DATA_BITS_B8);
        },
        InstructionId::StiAtomicB8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtom(record, DATA_BITS_B32);
        },
        InstructionId::ScalarAtomU32, InstructionId::ScalarAtomS32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarAtom(record, DATA_BITS_B64);
        },
        InstructionId::ScalarAtomU64, InstructionId::ScalarAtomS64);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarRed(record, DATA_BITS_B32);
        },
        InstructionId::ScalarRedU32, InstructionId::ScalarRedS32, InstructionId::ScalarRedF32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarRed(record, DATA_BITS_B16);
        },
        InstructionId::ScalarRedF16, InstructionId::ScalarRedBf16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarPreload(record);
        },
        InstructionId::DcPreload, InstructionId::DcPreloadI);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::StDev, DATA_BITS_B64);
        },
        InstructionId::StDevB64);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::StDev, DATA_BITS_B32);
        },
        InstructionId::StDevB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::StDev, DATA_BITS_B16);
        },
        InstructionId::StDevB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::StDev, DATA_BITS_B8);
        },
        InstructionId::StDevB8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::LdDev, DATA_BITS_B64);
        },
        InstructionId::LdDevB64);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::LdDev, DATA_BITS_B32);
        },
        InstructionId::LdDevB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::LdDev, DATA_BITS_B16);
        },
        InstructionId::LdDevB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeScalarDev(record, DeviceInstructionKind::LdDev, DATA_BITS_B8);
        },
        InstructionId::LdDevB8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLoadGmToCbuf2DV2(record);
        },
        InstructionId::LoadGmToCbuf2DV2);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyGmToCbufV2(record);
        },
        InstructionId::CopyGmToCbufV2);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMovAlignV2<CopyGmToCbufAlignV2ParamField>(
                record, DeviceInstructionKind::CopyGmToCbufAlignV2, DATA_BITS_B8);
        },
        InstructionId::CopyGmToCbufAlignV2B8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMovAlignV2<CopyGmToCbufAlignV2ParamField>(
                record, DeviceInstructionKind::CopyGmToCbufAlignV2, DATA_BITS_B16);
        },
        InstructionId::CopyGmToCbufAlignV2B16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMovAlignV2<CopyGmToCbufAlignV2ParamField>(
                record, DeviceInstructionKind::CopyGmToCbufAlignV2, DATA_BITS_B32);
        },
        InstructionId::CopyGmToCbufAlignV2B32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyGmToCbufMulti<CopyGmToCbufMultiNd2NzParamField>(record, DATA_BITS_B8);
        },
        InstructionId::CopyGmToCbufMultiNd2NzB8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyGmToCbufMulti<CopyGmToCbufMultiNd2NzParamField>(record, DATA_BITS_B16);
        },
        InstructionId::CopyGmToCbufMultiNd2NzB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyGmToCbufMulti<CopyGmToCbufMultiNd2NzParamField>(record, DATA_BITS_B32);
        },
        InstructionId::CopyGmToCbufMultiNd2NzB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyGmToCbufMulti<CopyGmToCbufMultiDn2NzParamField>(record, DATA_BITS_B8);
        },
        InstructionId::CopyGmToCbufMultiDn2NzB8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyGmToCbufMulti<CopyGmToCbufMultiDn2NzParamField>(record, DATA_BITS_B16);
        },
        InstructionId::CopyGmToCbufMultiDn2NzB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyGmToCbufMulti<CopyGmToCbufMultiDn2NzParamField>(record, DATA_BITS_B32);
        },
        InstructionId::CopyGmToCbufMultiDn2NzB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyUbufToGmAlignV2(record);
        },
        InstructionId::CopyUbufToGmAlignV2);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCopyUbufToUbuf(record);
        },
        InstructionId::CopyUbufToUbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMovAlignV2<CopyGmToUbufAlignV2ParamField>(
                record, DeviceInstructionKind::CopyGmToUbufAlignV2, DATA_BITS_B8);
        },
        InstructionId::CopyGmToUbufAlignV2B8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMovAlignV2<CopyGmToUbufAlignV2ParamField>(
                record, DeviceInstructionKind::CopyGmToUbufAlignV2, DATA_BITS_B16);
        },
        InstructionId::CopyGmToUbufAlignV2B16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMovAlignV2<CopyGmToUbufAlignV2ParamField>(
                record, DeviceInstructionKind::CopyGmToUbufAlignV2, DATA_BITS_B32);
        },
        InstructionId::CopyGmToUbufAlignV2B32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeNdDmaOutToUbuf(record, DATA_BITS_B8);
        },
        InstructionId::NdDmaOutToUbufB8);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeNdDmaOutToUbuf(record, DATA_BITS_B16);
        },
        InstructionId::NdDmaOutToUbufB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeNdDmaOutToUbuf(record, DATA_BITS_B32);
        },
        InstructionId::NdDmaOutToUbufB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodedInstruction{DeviceInstructionKind::ChannelPara, ChannelParaParamField{record.args[0]}};
        },
        InstructionId::ChannelPara);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodedInstruction{DeviceInstructionKind::SetFpc, FpcParamField{record.args[0]}};
        },
        InstructionId::SetFpc);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLoop3Param(record);
        },
        InstructionId::Loop3Param);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMte2SourceParam(record);
        },
        InstructionId::Mte2SrcPara);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeNdDmaPadCount(record);
        },
        InstructionId::NdDmaPadCount);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeNdDmaLoopStride(record);
        },
        InstructionId::NdDmaLoop0Stride, InstructionId::NdDmaLoop1Stride, InstructionId::NdDmaLoop2Stride,
        InstructionId::NdDmaLoop3Stride, InstructionId::NdDmaLoop4Stride);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeMte2NzParam(record);
        },
        InstructionId::SetMte2NzPara);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopSize(record, DmaLoopDirection::UBUF_TO_GM);
        },
        InstructionId::LoopSizeUbufToGm);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopStride(record, DmaLoopDirection::UBUF_TO_GM, 0);
        },
        InstructionId::Loop1StrideUbufToGm);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopStride(record, DmaLoopDirection::UBUF_TO_GM, 1);
        },
        InstructionId::Loop2StrideUbufToGm);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopSize(record, DmaLoopDirection::GM_TO_UBUF);
        },
        InstructionId::LoopSizeGmToUbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopStride(record, DmaLoopDirection::GM_TO_UBUF, 0);
        },
        InstructionId::Loop1StrideGmToUbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopStride(record, DmaLoopDirection::GM_TO_UBUF, 1);
        },
        InstructionId::Loop2StrideGmToUbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeFixL0c<FixL0cToOutParamField>(record, DATA_BITS_B32, DeviceInstructionKind::FixL0cToOut);
        },
        InstructionId::FixL0cToOutF32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeFixL0c<FixL0cToOutParamField>(record, DATA_BITS_B32, DeviceInstructionKind::FixL0cToOut);
        },
        InstructionId::FixL0cToOutS32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::CopyCbufToFbuf);
        },
        InstructionId::CopyCbufToFbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::CopyCbufToBt);
        },
        InstructionId::CopyCbufToBtF32, InstructionId::CopyCbufToBtS32, InstructionId::CopyCbufToBtF16,
        InstructionId::CopyCbufToBtBf16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::FixL0cToCbufF32);
        },
        InstructionId::FixL0cToCbufF32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::FixL0cToCbufS32);
        },
        InstructionId::FixL0cToCbufS32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::FixL0cToUbufF32);
        },
        InstructionId::FixL0cToUbufF32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::FixL0cToUbufS32);
        },
        InstructionId::FixL0cToUbufS32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::CopyUbufToCbuf);
        },
        InstructionId::CopyUbufToCbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeSetL12D(record, DATA_BITS_B16);
        },
        InstructionId::SetL12DB16);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeSetL12D(record, DATA_BITS_B32);
        },
        InstructionId::SetL12DB32);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            if (record.args[0] > 1) {
                return std::nullopt;
            }
            return DecodedInstruction{
                DeviceInstructionKind::MoveMask,
                VectorMaskWriteParamField{static_cast<uint32_t>(record.args[0]), record.args[1]}};
        },
        InstructionId::SetVectorMask);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodedInstruction{DeviceInstructionKind::SetCtrl, CtrlRegisterParamField{record.args[0]}};
        },
        InstructionId::SetCtrl);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeSetPadding(record);
        },
        InstructionId::SetPadding);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopSize(record, DmaLoopDirection::GM_TO_CBUF);
        },
        InstructionId::LoopSizeGmToCbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopStride(record, DmaLoopDirection::GM_TO_CBUF, 0);
        },
        InstructionId::Loop1StrideGmToCbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeDmaLoopStride(record, DmaLoopDirection::GM_TO_CBUF, 1);
        },
        InstructionId::Loop2StrideGmToCbuf);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            if (record.args[0] > UINT32_MAX)
                return std::nullopt;
            return DecodedInstruction{
                DeviceInstructionKind::PipeBarrier,
                PipeBarrierParamField{record.instrId, static_cast<uint32_t>(record.args[0])}};
        },
        InstructionId::PipeBarrier);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeFlag(record, DeviceInstructionKind::SetFlag);
        },
        InstructionId::SetFlag, InstructionId::SetFlagI, InstructionId::SetFlagV, InstructionId::SetFlagIV);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeFlag(record, DeviceInstructionKind::WaitFlag);
        },
        InstructionId::WaitFlag, InstructionId::WaitFlagI, InstructionId::WaitFlagV, InstructionId::WaitFlagIV);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCrossCore(record, DeviceInstructionKind::CrossCoreSet);
        },
        InstructionId::FftsCrossCoreSync, InstructionId::FftsCrossCoreSyncV);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCrossCore(record, DeviceInstructionKind::IntraBlockSet);
        },
        InstructionId::SetIntraBlock, InstructionId::SetIntraBlockI, InstructionId::SetIntraBlockV,
        InstructionId::SetIntraBlockIV);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCrossCore(record, DeviceInstructionKind::CrossCoreWait);
        },
        InstructionId::WaitFlagDev, InstructionId::WaitFlagDevI, InstructionId::WaitFlagDevV,
        InstructionId::WaitFlagDevIV);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeCrossCore(record, DeviceInstructionKind::IntraBlockWait);
        },
        InstructionId::WaitIntraBlock, InstructionId::WaitIntraBlockI, InstructionId::WaitIntraBlockV,
        InstructionId::WaitIntraBlockIV);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeHardwareFlag(record, DeviceInstructionKind::HardwareSetFlag);
        },
        InstructionId::HSetFlag, InstructionId::HSetFlagI);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeHardwareFlag(record, DeviceInstructionKind::HardwareWaitFlag);
        },
        InstructionId::HWaitFlag, InstructionId::HWaitFlagI);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeBuffer(record, DeviceInstructionKind::GetBuf);
        },
        InstructionId::GetBuf, InstructionId::GetBufI, InstructionId::GetBufV, InstructionId::GetBufIV);
    RegisterDecoders(
        table,
        [](const aclsan::AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeBuffer(record, DeviceInstructionKind::RlsBuf);
        },
        InstructionId::RlsBuf, InstructionId::RlsBufI, InstructionId::RlsBufV, InstructionId::RlsBufIV);
    return table;
}

std::optional<DecodedInstruction> DecodeLoadL1Mx(const AclsanRawTraceRecord& record) noexcept
{
    const uint64_t config0 = record.args[2];
    const uint64_t config1 = record.args[3];
    LoadL1MxParamField field{};
    field.instrId = record.instrId;
    field.srcAddr = record.args[1];
    field.dstAddr = record.args[0];
    field.xStart = static_cast<uint16_t>(ExtractBitRange(config0, LoadL1MxLayout::X_START));
    field.yStart = static_cast<uint16_t>(ExtractBitRange(config0, LoadL1MxLayout::Y_START));
    field.xStep = static_cast<uint8_t>(ExtractBitRange(config0, LoadL1MxLayout::X_STEP));
    field.yStep = static_cast<uint8_t>(ExtractBitRange(config0, LoadL1MxLayout::Y_STEP));
    field.srcStride = static_cast<uint16_t>(ExtractBitRange(config1, LoadL1MxLayout::SRC_STRIDE));
    field.dstStride = static_cast<uint16_t>(ExtractBitRange(config1, LoadL1MxLayout::DST_STRIDE));
    field.hasUnknownConfig = ExtractBitRange(config0, LoadL1MxLayout::UNKNOWN_CONFIG0) != 0 ||
                             ExtractBitRange(config1, LoadL1MxLayout::UNKNOWN_CONFIG1) != 0;
    return DecodedInstruction{DeviceInstructionKind::LoadL1Mx, field};
}

constexpr auto DECODE_TABLE = [] {
    auto table = MakeDecodeTable();
    RegisterDecoders(table, DecodeLoadL1Mx, InstructionId::LoadCbufToCaMx, InstructionId::LoadCbufToCbMx);
    RegisterDecoders(
        table,
        [](const AclsanRawTraceRecord& record) noexcept -> std::optional<DecodedInstruction> {
            return DecodeVtranspose(record);
        },
        InstructionId::VtransposeB16);
    RegisterDecoders(table, DecodeVectorUb<DecodeVnchwconv, DATA_BITS_B8>, InstructionId::ScatterVnchwconvB8);
    RegisterDecoders(table, DecodeVectorUb<DecodeVnchwconv, DATA_BITS_B16>, InstructionId::ScatterVnchwconvB16);
    RegisterDecoders(table, DecodeVectorUb<DecodeVnchwconv, DATA_BITS_B32>, InstructionId::ScatterVnchwconvB32);
    RegisterDecoders(table, DecodeVectorUb<DecodeVbs32, DATA_BITS_B16>, InstructionId::Vbs32F16);
    RegisterDecoders(table, DecodeVectorUb<DecodeVbs32, DATA_BITS_B32>, InstructionId::Vbs32F32);
    RegisterDecoders(table, DecodeVectorUb<DecodeVmrgsort, DATA_BITS_B16>, InstructionId::VmrgsortF16);
    RegisterDecoders(table, DecodeVectorUb<DecodeVmrgsort, DATA_BITS_B32>, InstructionId::VmrgsortF32);
    RegisterDecoders(table, DecodeMoveVa, InstructionId::MoveVa);
    RegisterDecoders(table, DecodeLdva, InstructionId::Ldva);
    return table;
}();

std::optional<DecodedInstruction> DecodeRegisteredInstruction(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    if (record.instrId >= DECODE_TABLE.size()) {
        return std::nullopt;
    }
    const auto decode = DECODE_TABLE[record.instrId];
    return decode != nullptr ? decode(record) : std::nullopt;
}

std::optional<DecodedInstruction> Decode(const aclsan::AclsanRawTraceRecord& record) noexcept
{
    if (record.instrId >= RawId(InstructionId::StB64Imm) && record.instrId <= RawId(InstructionId::LdpB8)) {
        return DecodeScalarGm(record);
    }
    if (record.instrId == RawId(InstructionId::SetFmatrix) || record.instrId == RawId(InstructionId::SetFmatrixB)) {
        const uint64_t config = record.args[0];
        FmatrixParamField fields{};
        fields.instrId = record.instrId;
        fields.width = static_cast<uint16_t>(ExtractBitRange(config, FmatrixLayout::WIDTH));
        fields.height = static_cast<uint16_t>(ExtractBitRange(config, FmatrixLayout::HEIGHT));
        fields.paddingLeft = static_cast<uint8_t>(ExtractBitRange(config, FmatrixLayout::PADDING_LEFT));
        fields.paddingRight = static_cast<uint8_t>(ExtractBitRange(config, FmatrixLayout::PADDING_RIGHT));
        fields.paddingTop = static_cast<uint8_t>(ExtractBitRange(config, FmatrixLayout::PADDING_TOP));
        fields.paddingBottom = static_cast<uint8_t>(ExtractBitRange(config, FmatrixLayout::PADDING_BOTTOM));
        return DecodedInstruction{DeviceInstructionKind::Img2ColRegister, fields};
    }
    if (record.instrId == RawId(InstructionId::SetL3dRpt) || record.instrId == RawId(InstructionId::SetL3dRptB)) {
        const uint64_t config = record.args[0];
        L3dRptParamField fields{};
        fields.instrId = record.instrId;
        fields.repeatStride = static_cast<uint16_t>(ExtractBitRange(config, L3dRptLayout::REPEAT_STRIDE));
        fields.repeatTimes = static_cast<uint8_t>(ExtractBitRange(config, L3dRptLayout::REPEAT_TIMES));
        fields.repeatAlongK = ExtractBitRange(config, L3dRptLayout::REPEAT_ALONG_K) != 0;
        fields.dstStride = static_cast<uint8_t>(ExtractBitRange(config, L3dRptLayout::DST_STRIDE));
        fields.dstOffset = static_cast<uint8_t>(ExtractBitRange(config, L3dRptLayout::DST_OFFSET));
        return DecodedInstruction{DeviceInstructionKind::Img2ColRegister, fields};
    }
    if (record.instrId >= RawId(InstructionId::LoadCbufToCaB4) &&
        record.instrId <= RawId(InstructionId::LoadCbufToCbB32)) {
        LoadCbufToL0ParamField f{};
        f.instrId = record.instrId;
        f.dstAddr = record.args[0];
        f.srcAddr = record.args[1];
        f.mStartPosition = static_cast<uint16_t>(ExtractBitRange(record.args[2], LoadL1ToL0Layout::M_START_POSITION));
        f.kStartPosition = static_cast<uint16_t>(ExtractBitRange(record.args[2], LoadL1ToL0Layout::K_START_POSITION));
        f.mStep = static_cast<uint8_t>(ExtractBitRange(record.args[2], LoadL1ToL0Layout::M_STEP));
        f.kStep = static_cast<uint8_t>(ExtractBitRange(record.args[2], LoadL1ToL0Layout::K_STEP));
        f.srcStride = static_cast<uint16_t>(record.args[3]);
        f.dstStride = static_cast<uint16_t>(ExtractBitRange(record.args[3], LoadL1ToL0Layout::DST_STRIDE));
        f.transpose = record.args[4] != 0;
        return DecodedInstruction{DeviceInstructionKind::LoadL1ToL0, f};
    }
    if (record.instrId >= RawId(InstructionId::LoadCbufToCbTransposeB8) &&
        record.instrId <= RawId(InstructionId::LoadCbufToCbTransposeB4)) {
        LoadCbufToCbTransposeParamField f{};
        f.instrId = record.instrId;
        f.dstAddr = record.args[0];
        f.srcAddr = record.args[1];
        f.indexId = static_cast<uint16_t>(ExtractBitRange(record.args[2], LoadL1TransposeLayout::INDEX_ID));
        f.repeat = static_cast<uint8_t>(ExtractBitRange(record.args[2], LoadL1TransposeLayout::REPEAT));
        f.srcStride = static_cast<uint16_t>(ExtractBitRange(record.args[2], LoadL1TransposeLayout::SRC_STRIDE));
        f.dstGap = static_cast<uint16_t>(ExtractBitRange(record.args[2], LoadL1TransposeLayout::DST_GAP));
        f.dstFracGap = static_cast<uint16_t>(ExtractBitRange(record.args[3], LoadL1TransposeLayout::DST_FRAC_GAP));
        f.srcFracGap = static_cast<uint16_t>(ExtractBitRange(record.args[3], LoadL1TransposeLayout::SRC_FRAC_GAP));
        f.decrement = ExtractBitRange(record.args[2], LoadL1TransposeLayout::DECREMENT) != 0;
        return DecodedInstruction{DeviceInstructionKind::LoadL1Transpose, f};
    }
    if ((record.instrId >= RawId(InstructionId::Img2ColCbufToCaB8) &&
         record.instrId <= RawId(InstructionId::Img2ColCbufToCbB32)) ||
        record.instrId == RawId(InstructionId::Img2ColCbufToCaB32)) {
        Img2ColParamField f{};
        f.instrId = record.instrId;
        f.dstAddr = record.args[0];
        f.srcAddr = static_cast<uint32_t>(record.args[1]);
        f.kExtension = static_cast<uint16_t>(ExtractBitRange(record.args[2], Img2ColLayout::K_EXTENSION));
        f.mExtension = static_cast<uint16_t>(ExtractBitRange(record.args[2], Img2ColLayout::M_EXTENSION));
        f.kStartPoint = static_cast<uint16_t>(ExtractBitRange(record.args[2], Img2ColLayout::K_START_POINT));
        f.mStartPoint = static_cast<uint16_t>(ExtractBitRange(record.args[2], Img2ColLayout::M_START_POINT));
        f.strideWidth = static_cast<uint8_t>(ExtractBitRange(record.args[3], Img2ColLayout::STRIDE_WIDTH));
        f.strideHeight = static_cast<uint8_t>(ExtractBitRange(record.args[3], Img2ColLayout::STRIDE_HEIGHT));
        f.filterWidth = static_cast<uint8_t>(ExtractBitRange(record.args[3], Img2ColLayout::FILTER_WIDTH));
        f.filterHeight = static_cast<uint8_t>(ExtractBitRange(record.args[3], Img2ColLayout::FILTER_HEIGHT));
        f.dilationFilterWidth =
            static_cast<uint8_t>(ExtractBitRange(record.args[3], Img2ColLayout::DILATION_FILTER_WIDTH));
        f.dilationFilterHeight =
            static_cast<uint8_t>(ExtractBitRange(record.args[3], Img2ColLayout::DILATION_FILTER_HEIGHT));
        f.filterSizeWidth = ExtractBitRange(record.args[3], Img2ColLayout::FILTER_SIZE_WIDTH) != 0;
        f.filterSizeHeight = ExtractBitRange(record.args[3], Img2ColLayout::FILTER_SIZE_HEIGHT) != 0;
        f.transpose = ExtractBitRange(record.args[3], Img2ColLayout::TRANSPOSE) != 0;
        f.fMatrixControl = ExtractBitRange(record.args[3], Img2ColLayout::FMATRIX_CONTROL) != 0;
        f.channelSize = static_cast<uint16_t>(ExtractBitRange(record.args[3], Img2ColLayout::CHANNEL_SIZE));
        return DecodedInstruction{DeviceInstructionKind::Img2Col, f};
    }
    if (record.instrId >= RawId(InstructionId::MadS8) && record.instrId <= RawId(InstructionId::MadMxE5m2E5m2)) {
        const uint64_t config = record.args[3];
        return DecodedInstruction{
            DeviceInstructionKind::Mmad,
            MmadParamField{
                record.instrId, static_cast<uint32_t>(record.args[0]), record.args[1], record.args[2],
                static_cast<uint16_t>(ExtractBitRange(config, MmadLayout::M_SIZE)),
                static_cast<uint16_t>(ExtractBitRange(config, MmadLayout::K_SIZE)),
                static_cast<uint16_t>(ExtractBitRange(config, MmadLayout::N_SIZE)),
                static_cast<uint8_t>(ExtractBitRange(config, MmadLayout::UNIT_FLAG)),
                ExtractBitRange(config, MmadLayout::DISABLE_GEMV) != 0,
                ExtractBitRange(config, MmadLayout::CMATRIX_SOURCE) != 0,
                ExtractBitRange(config, MmadLayout::CMATRIX_INIT_VAL) != 0}};
    }
    if (record.instrId == RawId(InstructionId::CopyCbufToUbuf)) {
        return DecodeLocalMemoryTransfer(record, LocalMemoryTransferKind::CopyCbufToUbuf);
    }
    return DecodeRegisteredInstruction(record);
}

} // namespace

const DeviceInstructionDecoder& GetDeviceInstructionDecoder() noexcept
{
    static const DeviceInstructionDecoder DECODER{"dav_3510", Decode};
    return DECODER;
}

} // namespace aclsan::dav3510
