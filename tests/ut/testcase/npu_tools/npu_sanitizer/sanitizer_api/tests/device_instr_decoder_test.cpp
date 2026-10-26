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

#include "device_instr/decoder_registry.h"
#include "device_instr/soc_version.h"

#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <variant>

namespace {

constexpr uint64_t Pack(uint64_t value, uint32_t begin) noexcept { return value << begin; }

template <typename ParamField>
aclsan::DecodedInstruction DecodeAndAssertDataBits(
    const aclsan::DeviceInstructionDecoder& decoder, uint32_t instructionId, uint32_t expectedDataBits)
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = instructionId;
    record.args[2] = Pack(2, 4) | Pack(32, 25);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder.decode(record);
    assert(decoded.has_value());
    const auto* params = std::get_if<ParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->dataBits == expectedDataBits);
    return *decoded;
}

void TestSocVersionMappingsSelectDav3510Decoder()
{
    const auto& mappings = aclsan::GetSocVersionMappings();
    assert(mappings.size() == 35U);
    for (const auto& mapping : mappings) {
        const std::optional<aclsan::SocVersion> version = aclsan::ResolveSocVersion(mapping.first.c_str());
        assert(version.has_value());
        assert(*version == aclsan::SocVersion::DAV_3510);
        const aclsan::DeviceInstructionDecoder* decoder = aclsan::FindDeviceInstructionDecoder(*version);
        assert(decoder != nullptr);
        assert(std::strcmp(decoder->architecture, "dav_3510") == 0);
    }

    assert(!aclsan::ResolveSocVersion(nullptr).has_value());
    assert(!aclsan::ResolveSocVersion("").has_value());
    assert(!aclsan::ResolveSocVersion("UnknownSoc").has_value());
}

void TestDecodesDav3510CopyInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 75;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] =
        Pack(5, 0) | Pack(3, 4) | Pack(64, 25) | Pack(0x1a, 46) | Pack(0x15, 52) | Pack(1, 58) | Pack(5, 60);
    record.args[3] = 0x1234ULL | (0x20ULL << 40U);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::CopyGmToCbufAlignV2);
    const auto* params = std::get_if<aclsan::CopyGmToCbufAlignV2ParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 75);
    assert(params->dataBits == 16);
    assert(params->dstAddr == 0x2000);
    assert(params->srcAddr == 0x1000);
    assert(params->sid == 5);
    assert(params->burstNum == 3);
    assert(params->burstLen == 64);
    assert(params->leftPaddingCount == 0x1a);
    assert(params->rightPaddingCount == 0x15);
    assert(params->dataSelectBit);
    assert(params->l2CacheControl == 5);
    assert(params->burstSrcStride == 0x1234);
    assert(params->burstDstStride == 0x20);

    record.instrId = 85;
    const std::optional<aclsan::DecodedInstruction> gmToUbuf = decoder->decode(record);
    assert(gmToUbuf.has_value());
    assert(gmToUbuf->kind == aclsan::DeviceInstructionKind::CopyGmToUbufAlignV2);
    const auto* gmToUbufParams = std::get_if<aclsan::CopyGmToUbufAlignV2ParamField>(&gmToUbuf->params);
    assert(gmToUbufParams != nullptr);
    assert(gmToUbufParams->instrId == 85);
    assert(gmToUbufParams->dataBits == 16);
    assert(gmToUbufParams->sid == 5);
    assert(gmToUbufParams->burstNum == 3);
    assert(gmToUbufParams->burstLen == 64);
    assert(gmToUbufParams->leftPaddingCount == 0x1a);
    assert(gmToUbufParams->rightPaddingCount == 0x15);
    assert(gmToUbufParams->dataSelectBit);
    assert(gmToUbufParams->l2CacheControl == 5);
    assert(gmToUbufParams->burstSrcStride == 0x1234);
    assert(gmToUbufParams->burstDstStride == 0x20);
}

void TestDecodesMovemask93AsCompleteVectorMask()
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 93;
    record.args[0] = UINT64_C(0x1122334455667788);
    record.args[1] = UINT64_C(0x99aabbccddeeff00);

    const auto decoded = decoder->decode(record);
    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::MoveMask);
    const auto* params = std::get_if<aclsan::VectorMaskParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->vectorMask0 == UINT64_C(0x99aabbccddeeff00));
    assert(params->vectorMask1 == UINT64_C(0x1122334455667788));
}

void TestDecodesInclusiveMovAlignV2Ranges()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 85;
    record.args[2] = Pack(0xF, 0) | Pack(1, 59);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    const auto* params = std::get_if<aclsan::CopyGmToUbufAlignV2ParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->sid == 0xF);
    assert(!params->dataSelectBit);
}

void TestDecodesDav3510CopyUbufToGmAlignV2Instruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 83;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = Pack(6, 0) | Pack(2, 4) | Pack(32, 25) | Pack(3, 60);
    record.args[3] = Pack(0x1234567, 0) | Pack(0x54321, 40);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::CopyUbufToGmAlignV2);
    const auto* params = std::get_if<aclsan::CopyUbufToGmAlignV2ParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 83);
    assert(params->dstAddr == 0x2000);
    assert(params->srcAddr == 0x1000);
    assert(params->sid == 6);
    assert(params->burstNum == 2);
    assert(params->burstLen == 32);
    assert(params->l2CacheControl == 3);
    assert(params->dstStride == 0x1234567);
    assert(params->srcStride == 0x54321);
}

template <typename ParamField>
void AssertCopyGmToCbufMultiParams(
    const aclsan::DecodedInstruction& decoded, uint32_t instructionId, uint32_t expectedDataBits)
{
    assert(decoded.kind == aclsan::DeviceInstructionKind::CopyGmToCbufMulti);
    const auto* params = std::get_if<ParamField>(&decoded.params);
    assert(params != nullptr);
    assert(params->instrId == instructionId);
    assert(params->dataBits == expectedDataBits);
    assert(params->dstAddr == 0x2000);
    assert(params->srcAddr == 0x1000);
    assert(params->sid == 5);
    assert(params->loop1SrcStride == 0x12345);
    assert(params->l2CacheControl == 5);
    assert(params->nValue == 0x3456);
    assert(params->dValue == 0x54321);
    assert(params->loop4SrcStride == 0x23456);
    assert(params->smallC0Enable);
}

void TestDecodesDav3510CopyGmToCbufMultiInstructions()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = Pack(5, 0) | Pack(0x12345, 4) | Pack(5, 44) | Pack(0x3456, 48);
    record.args[3] = Pack(0x54321, 0) | Pack(0x23456, 21) | Pack(1, 61);

    record.instrId = 77;
    const std::optional<aclsan::DecodedInstruction> nd2Nz = decoder->decode(record);
    assert(nd2Nz.has_value());
    AssertCopyGmToCbufMultiParams<aclsan::CopyGmToCbufMultiNd2NzParamField>(*nd2Nz, 77, 8);

    record.instrId = 80;
    const std::optional<aclsan::DecodedInstruction> dn2Nz = decoder->decode(record);
    assert(dn2Nz.has_value());
    AssertCopyGmToCbufMultiParams<aclsan::CopyGmToCbufMultiDn2NzParamField>(*dn2Nz, 80, 8);
}

void TestDecodesDav3510CopyGmToCbufV2Instruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 73;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = Pack(3, 0) | Pack(7, 4) | Pack(11, 25) | Pack(6, 56) | Pack(4, 60);
    constexpr uint64_t SRC_STRIDE = (UINT64_C(1) << 35) | UINT64_C(0x1f);
    record.args[3] = Pack(SRC_STRIDE, 0) | Pack(0xabcd, 40);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::CopyGmToCbufV2);
    const auto* params = std::get_if<aclsan::CopyGmToCbufV2ParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 73);
    assert(params->dstAddr == 0x2000);
    assert(params->srcAddr == 0x1000);
    assert(params->sid == 3);
    assert(params->burstNum == 7);
    assert(params->burstLen == 11);
    assert(params->padFunctionMode == 6);
    assert(params->l2CacheControl == 4);
    assert(params->srcStride == SRC_STRIDE);
    assert(params->dstStride == 0xabcd);
}

void TestDecodesDav3510LoadGmToCbuf2DV2Instruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 72;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = Pack(0x12345678, 0) | Pack(0x1abcdef0, 32);
    record.args[3] = Pack(0x345, 0) | Pack(0x678, 12) | Pack(0x5ab, 24) | Pack(6, 36) | Pack(3, 40) | Pack(7, 60);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::LoadGmToCbuf2DV2);
    const auto* params = std::get_if<aclsan::LoadGmToCbuf2DV2ParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 72);
    assert(params->dstAddr == 0x2000);
    assert(params->srcAddr == 0x1000);
    assert(params->mStartPosition == 0x12345678);
    assert(params->kStartPosition == 0x1abcdef0);
    assert(params->dstStride == 0x345);
    assert(params->mStep == 0x678);
    assert(params->kStep == 0x5ab);
    assert(params->sid == 6);
    assert(params->decompMode == 3);
    assert(params->l2CacheControl == 7);
}

void TestDecodesDav3510NdDmaOutToUbufInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 87;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = Pack(6, 0) | Pack(0x12345, 4) | Pack(0x23456, 24) | Pack(0x34567, 44);
    record.args[3] = Pack(0x45678, 0) | Pack(0x56789, 20) | Pack(0x67, 40) | Pack(0x78, 48) | Pack(1, 56) | Pack(5, 60);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::NdDmaOutToUbuf);
    const auto* params = std::get_if<aclsan::NdDmaOutToUbufParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 87);
    assert(params->dataBits == 8);
    assert(params->dstAddr == 0x2000);
    assert(params->srcAddr == 0x1000);
    assert(params->sid == 6);
    assert(params->loop0Size == 0x12345);
    assert(params->loop1Size == 0x23456);
    assert(params->loop2Size == 0x34567);
    assert(params->loop3Size == 0x45678);
    assert(params->loop4Size == 0x56789);
    assert(params->loop0LeftPaddingCount == 0x67);
    assert(params->loop0RightPaddingCount == 0x78);
    assert(params->paddingMode);
    assert(params->l2CacheControl == 5);
}

void TestDecodesDav3510NdDmaPadCountInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 131;
    record.args[0] = Pack(0x11, 0) | Pack(0x12, 8) | Pack(0x21, 16) | Pack(0x22, 24) | Pack(0x31, 32) | Pack(0x32, 40) |
                     Pack(0x41, 48) | Pack(0x42, 56);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::NdDmaPadCount);
    const auto* params = std::get_if<aclsan::NdDmaPadCountParamField>(&decoded->params);
    assert(params != nullptr);
    assert((params->leftPaddingCounts == std::array<uint8_t, 4>{0x11, 0x21, 0x31, 0x41}));
    assert((params->rightPaddingCounts == std::array<uint8_t, 4>{0x12, 0x22, 0x32, 0x42}));
}

void TestDecodesDav3510RegisterFieldsWithoutChangingMoverFields()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord source{};
    source.instrId = 124;
    source.args[0] = UINT32_C(0xfffffff0);
    const auto sourceDecoded = decoder->decode(source);
    assert(sourceDecoded.has_value());
    const auto* sourceParams = std::get_if<aclsan::Mte2SourceParamField>(&sourceDecoded->params);
    assert(sourceParams != nullptr && sourceParams->srcStride == -16);

    aclsan::AclsanRawTraceRecord nz{};
    nz.instrId = 399;
    nz.args[0] = Pack(1, 0) | Pack(2, 16) | Pack(3, 32) | Pack(4, 48);
    const auto nzDecoded = decoder->decode(nz);
    assert(nzDecoded.has_value());
    const auto* nzParams = std::get_if<aclsan::Mte2NzParamField>(&nzDecoded->params);
    assert(nzParams != nullptr);
    assert(
        nzParams->matrixNum == 1 && nzParams->loop2DstStride == 2 && nzParams->loop3DstStride == 3 &&
        nzParams->loop4DstStride == 4);

    aclsan::AclsanRawTraceRecord loop3{};
    loop3.instrId = 90;
    loop3.args[0] = Pack(5, 0) | Pack(6, 16) | Pack(UINT32_C(0x12345678), 32);
    const auto loop3Decoded = decoder->decode(loop3);
    assert(loop3Decoded.has_value());
    const auto* loop3Params = std::get_if<aclsan::Loop3ParamField>(&loop3Decoded->params);
    assert(loop3Params != nullptr);
    assert(
        loop3Params->loopCount == 5 && loop3Params->srcStride == 6 && loop3Params->dstStride == UINT32_C(0x12345678));
}

void TestDecodesDav3510DmaOuterLoopFields()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord size{};
    size.instrId = 128;
    constexpr uint64_t kLoop2Size = (UINT64_C(1) << 35U) | 9U;
    size.args[0] = Pack(7, 0) | Pack(kLoop2Size, 21);
    const auto sizeDecoded = decoder->decode(size);
    assert(sizeDecoded.has_value());
    const auto* sizeParams = std::get_if<aclsan::DmaLoopSizeParamField>(&sizeDecoded->params);
    assert(sizeParams != nullptr);
    assert(sizeParams->direction == aclsan::DmaLoopDirection::GM_TO_UBUF);
    assert(sizeParams->loop1Size == 7 && sizeParams->loop2Size == kLoop2Size);

    aclsan::AclsanRawTraceRecord outToUbufStride{};
    outToUbufStride.instrId = 129;
    outToUbufStride.args[0] = Pack(UINT64_C(0x123456789a), 0) | Pack(0x1abcde, 40);
    const auto outToUbufDecoded = decoder->decode(outToUbufStride);
    assert(outToUbufDecoded.has_value());
    const auto* outToUbufParams = std::get_if<aclsan::DmaLoopStrideParamField>(&outToUbufDecoded->params);
    assert(outToUbufParams != nullptr);
    assert(outToUbufParams->srcStride == UINT64_C(0x123456789a));
    assert(outToUbufParams->dstStride == 0x1abcde);

    aclsan::AclsanRawTraceRecord ubToOutStride{};
    ubToOutStride.instrId = 126;
    ubToOutStride.args[0] = Pack(UINT64_C(0x23456789ab), 0) | Pack(0x12345, 40);
    const auto ubToOutDecoded = decoder->decode(ubToOutStride);
    assert(ubToOutDecoded.has_value());
    const auto* ubToOutParams = std::get_if<aclsan::DmaLoopStrideParamField>(&ubToOutDecoded->params);
    assert(ubToOutParams != nullptr);
    assert(ubToOutParams->srcStride == 0x12345);
    assert(ubToOutParams->dstStride == UINT64_C(0x23456789ab));
}

void TestDecodesDav3510SetL12DInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 149;
    record.args[0] = 0x2000;
    record.args[1] = Pack(0x1234, 0) | Pack(0x2345, 16) | Pack(0x3456, 32);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::SetL12D);
    const auto* params = std::get_if<aclsan::SetL12DParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 149);
    assert(params->dataBits == 16);
    assert(params->dstAddr == 0x2000);
    assert(params->repeatTimes == 0x1234);
    assert(params->blockNum == 0x2345);
    assert(params->repeatGap == 0x3456);
}

void TestDecodesDav3510SetPaddingInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 392;
    record.args[0] = UINT64_C(0xfedcba9876543210);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::SetPadding);
    const auto* params = std::get_if<aclsan::SetPaddingParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->value == UINT64_C(0xfedcba9876543210));
}

void TestDecodesDav3510FixL0cToOutInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 91;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = Pack(7, 0) | Pack(0x5bc, 4) | Pack(0x5ef0, 16) | Pack(0x12345678, 32);
    record.args[3] = Pack(0x5876, 0) | Pack(5, 16) | Pack(1, 29) | Pack(1, 30) | Pack(1, 32) | Pack(0xb, 34) |
                     Pack(3, 39) | Pack(1, 42) | Pack(1, 43) | Pack(0xc, 44) | Pack(3, 49) | Pack(1, 52) | Pack(1, 53) |
                     Pack(3, 54) | Pack(1, 57) | Pack(1, 58) | Pack(1, 59) | Pack(1, 60) | Pack(1, 61) | Pack(1, 62);

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::FixL0cToOut);
    const auto* params = std::get_if<aclsan::FixL0cToOutParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 91);
    assert(params->dataBits == 32);
    assert(params->dstAddr == 0x2000);
    assert(params->srcAddr == 0x1000);
    assert(params->sid == 7);
    assert(params->nSize == 0x5bc);
    assert(params->mSize == 0x5ef0);
    assert(params->loopDstStride == 0x12345678);
    assert(params->loopSrtStride == 0x5876);
    assert(params->l2CacheControl == 5);
    assert(params->clipReluPre == 1);
    assert(params->unitFlag == 1);
    assert(params->quantPre == 0x2b);
    assert(params->reluPre == 3);
    assert(params->splitEnable);
    assert(params->nz2ndEnable);
    assert(params->quantPost == 0xc);
    assert(params->reluPost == 3);
    assert(params->clipReluPost);
    assert(params->loopEnhanceEnable);
    assert(params->eltwiseOp == 3);
    assert(params->eltwiseAntqEnable);
    assert(params->loopEnhanceMergeEnable);
    assert(params->c0PadEnable);
    assert(params->winoPostEnable);
    assert(params->brcbEnable);
    assert(params->nz2dnEnable);
}

void TestDecodesDav3510FixpipeQuantPreHighBit()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    for (uint8_t quantPre = 31; quantPre <= 36; ++quantPre) {
        aclsan::AclsanRawTraceRecord record{};
        record.instrId = 91;
        record.args[3] = Pack(quantPre >> 5U, 29) | Pack(quantPre & 0x1fU, 34);

        const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

        assert(decoded.has_value());
        const auto* params = std::get_if<aclsan::FixL0cToOutParamField>(&decoded->params);
        assert(params != nullptr);
        assert(params->quantPre == quantPre);
    }
}

void TestDecodesDav3510LocalMemoryTransfersWithoutLosingRawConfig()
{
    struct ExpectedLocalTransfer {
        uint64_t instructionId;
        aclsan::LocalMemoryTransferKind kind;
    };
    constexpr ExpectedLocalTransfer EXPECTED[] = {
        {167, aclsan::LocalMemoryTransferKind::CopyCbufToFbuf},
        {423, aclsan::LocalMemoryTransferKind::CopyCbufToBt},
        {424, aclsan::LocalMemoryTransferKind::CopyCbufToBt},
        {425, aclsan::LocalMemoryTransferKind::CopyCbufToBt},
        {426, aclsan::LocalMemoryTransferKind::CopyCbufToBt},
        {168, aclsan::LocalMemoryTransferKind::FixL0cToCbufF32},
        {169, aclsan::LocalMemoryTransferKind::FixL0cToCbufS32},
        {170, aclsan::LocalMemoryTransferKind::FixL0cToUbufF32},
        {171, aclsan::LocalMemoryTransferKind::FixL0cToUbufS32},
        {173, aclsan::LocalMemoryTransferKind::CopyUbufToCbuf},
    };
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    for (const ExpectedLocalTransfer& expected : EXPECTED) {
        aclsan::AclsanRawTraceRecord record{};
        record.instrId = expected.instructionId;
        record.args[0] = UINT64_C(0x123456789abcdef0);
        record.args[1] = UINT64_C(0x0fedcba987654321);
        record.args[2] = UINT64_C(0xa5a55a5a11223344);
        record.args[3] = UINT64_C(0x55aa33cc77889900);

        const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

        assert(decoded.has_value());
        assert(decoded->kind == aclsan::DeviceInstructionKind::LocalMemoryTransfer);
        const auto* params = std::get_if<aclsan::LocalMemoryTransferParamField>(&decoded->params);
        assert(params != nullptr);
        assert(params->instrId == expected.instructionId);
        assert(params->dstAddr == record.args[0]);
        assert(params->srcAddr == record.args[1]);
        assert(params->config0 == record.args[2]);
        assert(params->config1 == record.args[3]);
        assert(params->kind == expected.kind);
        if (expected.kind == aclsan::LocalMemoryTransferKind::CopyCbufToFbuf ||
            expected.kind == aclsan::LocalMemoryTransferKind::CopyCbufToBt) {
            assert(params->nBurst == 0x334u);
            assert(params->lenBurst == 0x1122u);
        }
        if (expected.kind == aclsan::LocalMemoryTransferKind::CopyCbufToFbuf) {
            assert(params->srcStride == 0x5a5au);
            assert(params->dstStride == 0xa5a5u);
            assert(params->dstMemBlock == 0xcu);
        }
        if (expected.kind == aclsan::LocalMemoryTransferKind::CopyCbufToBt) {
            assert(params->srcGap == 0x5a5au);
            assert(params->dstGap == 0xa5a5u);
            assert(!params->cvtEnable);
            record.args[2] |= UINT64_C(1) << 3;
            const auto converted = decoder->decode(record);
            assert(converted.has_value());
            assert(std::get<aclsan::LocalMemoryTransferParamField>(converted->params).cvtEnable);
        }
    }
}

void TestDecodesDav3510S4FixpipeUsesSourceIntrinsicId()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    for (const uint64_t instructionId : {UINT64_C(91), UINT64_C(92)}) {
        aclsan::AclsanRawTraceRecord record{};
        record.instrId = instructionId;
        record.args[0] = 0x2000;
        record.args[1] = 0x1000;
        record.args[2] = Pack(3, 0) | Pack(32, 4) | Pack(16, 16) | Pack(64, 32);
        const uint8_t quantPre = instructionId == 91 ? 25 : 21;
        record.args[3] = Pack(48, 0) | Pack(2, 16) | Pack(1, 32) | Pack(quantPre, 34) | Pack(1, 59);

        const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

        assert(decoded.has_value());
        const auto* params = std::get_if<aclsan::FixL0cToOutParamField>(&decoded->params);
        assert(params != nullptr);
        assert(params->instrId == instructionId);
        assert(params->dataBits == 32);
        assert(params->dstAddr == 0x2000 && params->srcAddr == 0x1000);
        assert(params->sid == 3 && params->nSize == 32 && params->mSize == 16);
        assert(params->loopDstStride == 64 && params->loopSrtStride == 48);
        assert(params->l2CacheControl == 2 && params->unitFlag == 1);
        assert(params->quantPre == quantPre && params->c0PadEnable);
    }
}

void TestDecodesDav3510SyncInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 440;
    record.args[0] = 2;
    record.args[1] = 3;
    record.args[2] = 7;

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::SetFlag);
    const auto* params = std::get_if<aclsan::FlagParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->srcPipe == 2);
    assert(params->dstPipe == 3);
    assert(params->eventId == 7);
}

void TestDecodesDav3510ScalarGmInstructions()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    for (uint32_t instructionId = 24; instructionId <= 55; ++instructionId) {
        aclsan::AclsanRawTraceRecord record{};
        record.instrId = instructionId;
        record.args[0] = UINT64_C(0x8000000200001000);
        record.args[1] = static_cast<uint64_t>(-8);
        record.args[2] = 1;
        record.args[3] = UINT64_C(0x100000000);
        record.args[4] = aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;

        const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

        assert(decoded.has_value());
        assert(decoded->kind == aclsan::DeviceInstructionKind::ScalarGm);
        const auto* params = std::get_if<aclsan::ScalarGmParamField>(&decoded->params);
        assert(params != nullptr);
        assert(params->instrId == instructionId);
        assert(params->dataBits == (64U >> ((instructionId - 24U) % 4U)));
        assert(params->addr == record.args[0]);
        assert(params->offset == -8);
        assert(params->post == 1);
        assert(params->sysVaBase == record.args[3]);
        assert(params->addressContext == record.args[4]);
    }
}

void TestDecodesDav3510BufferInstruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 448;
    record.args[0] = 5;
    record.args[1] = 0x12345678;
    record.args[2] = 3;

    const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);

    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::GetBuf);
    const auto* params = std::get_if<aclsan::SyncBufParamField>(&decoded->params);
    assert(params != nullptr);
    assert(params->instrId == 448);
    assert(params->pipe == 5);
    assert(params->bufId == 0x12345678);
    assert(params->mode == 3);

    record.args[0] = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    assert(!decoder->decode(record).has_value());
    record.args[0] = 5;
    record.args[2] = static_cast<uint64_t>(std::numeric_limits<uint8_t>::max()) + 1;
    assert(!decoder->decode(record).has_value());
}

void TestRejectsUnknownDav3510Instruction()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::AclsanRawTraceRecord record{};
    record.instrId = UINT32_MAX;
    assert(!decoder->decode(record).has_value());
}

void TestDecodesDav3510DataBits()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    DecodeAndAssertDataBits<aclsan::CopyGmToCbufAlignV2ParamField>(*decoder, 74, 8);
    DecodeAndAssertDataBits<aclsan::CopyGmToCbufAlignV2ParamField>(*decoder, 75, 16);
    DecodeAndAssertDataBits<aclsan::CopyGmToCbufAlignV2ParamField>(*decoder, 76, 32);

    DecodeAndAssertDataBits<aclsan::CopyGmToCbufMultiNd2NzParamField>(*decoder, 77, 8);
    DecodeAndAssertDataBits<aclsan::CopyGmToCbufMultiNd2NzParamField>(*decoder, 78, 16);
    DecodeAndAssertDataBits<aclsan::CopyGmToCbufMultiNd2NzParamField>(*decoder, 79, 32);
    DecodeAndAssertDataBits<aclsan::CopyGmToCbufMultiDn2NzParamField>(*decoder, 80, 8);
    DecodeAndAssertDataBits<aclsan::CopyGmToCbufMultiDn2NzParamField>(*decoder, 81, 16);
    DecodeAndAssertDataBits<aclsan::CopyGmToCbufMultiDn2NzParamField>(*decoder, 82, 32);
    DecodeAndAssertDataBits<aclsan::CopyGmToUbufAlignV2ParamField>(*decoder, 84, 8);
    DecodeAndAssertDataBits<aclsan::CopyGmToUbufAlignV2ParamField>(*decoder, 85, 16);
    DecodeAndAssertDataBits<aclsan::CopyGmToUbufAlignV2ParamField>(*decoder, 86, 32);
    DecodeAndAssertDataBits<aclsan::NdDmaOutToUbufParamField>(*decoder, 87, 8);
    DecodeAndAssertDataBits<aclsan::NdDmaOutToUbufParamField>(*decoder, 88, 16);
    DecodeAndAssertDataBits<aclsan::NdDmaOutToUbufParamField>(*decoder, 89, 32);
    DecodeAndAssertDataBits<aclsan::FixL0cToOutParamField>(*decoder, 91, 32);
    DecodeAndAssertDataBits<aclsan::FixL0cToOutParamField>(*decoder, 92, 32);
    DecodeAndAssertDataBits<aclsan::SetL12DParamField>(*decoder, 149, 16);
    DecodeAndAssertDataBits<aclsan::SetL12DParamField>(*decoder, 150, 32);
}

void TestClassifiesCurrentDav3510InstructionSet()
{
    struct ExpectedInstruction {
        uint32_t instructionId;
        aclsan::DeviceInstructionKind kind;
    };
    const ExpectedInstruction expectedInstructions[] = {
        {24, aclsan::DeviceInstructionKind::ScalarGm},
        {43, aclsan::DeviceInstructionKind::ScalarGm},
        {44, aclsan::DeviceInstructionKind::ScalarGm},
        {55, aclsan::DeviceInstructionKind::ScalarGm},
        {72, aclsan::DeviceInstructionKind::LoadGmToCbuf2DV2},
        {73, aclsan::DeviceInstructionKind::CopyGmToCbufV2},
        {77, aclsan::DeviceInstructionKind::CopyGmToCbufMulti},
        {78, aclsan::DeviceInstructionKind::CopyGmToCbufMulti},
        {79, aclsan::DeviceInstructionKind::CopyGmToCbufMulti},
        {80, aclsan::DeviceInstructionKind::CopyGmToCbufMulti},
        {81, aclsan::DeviceInstructionKind::CopyGmToCbufMulti},
        {82, aclsan::DeviceInstructionKind::CopyGmToCbufMulti},
        {83, aclsan::DeviceInstructionKind::CopyUbufToGmAlignV2},
        {84, aclsan::DeviceInstructionKind::CopyGmToUbufAlignV2},
        {85, aclsan::DeviceInstructionKind::CopyGmToUbufAlignV2},
        {86, aclsan::DeviceInstructionKind::CopyGmToUbufAlignV2},
        {87, aclsan::DeviceInstructionKind::NdDmaOutToUbuf},
        {88, aclsan::DeviceInstructionKind::NdDmaOutToUbuf},
        {89, aclsan::DeviceInstructionKind::NdDmaOutToUbuf},
        {90, aclsan::DeviceInstructionKind::Loop3Param},
        {124, aclsan::DeviceInstructionKind::Mte2SourceParam},
        {125, aclsan::DeviceInstructionKind::DmaLoopSize},
        {126, aclsan::DeviceInstructionKind::DmaLoopStride},
        {127, aclsan::DeviceInstructionKind::DmaLoopStride},
        {128, aclsan::DeviceInstructionKind::DmaLoopSize},
        {129, aclsan::DeviceInstructionKind::DmaLoopStride},
        {130, aclsan::DeviceInstructionKind::DmaLoopStride},
        {131, aclsan::DeviceInstructionKind::NdDmaPadCount},
        {132, aclsan::DeviceInstructionKind::NdDmaLoopStride},
        {133, aclsan::DeviceInstructionKind::NdDmaLoopStride},
        {134, aclsan::DeviceInstructionKind::NdDmaLoopStride},
        {135, aclsan::DeviceInstructionKind::NdDmaLoopStride},
        {136, aclsan::DeviceInstructionKind::NdDmaLoopStride},
        {399, aclsan::DeviceInstructionKind::Mte2NzParam},
        {91, aclsan::DeviceInstructionKind::FixL0cToOut},
        {92, aclsan::DeviceInstructionKind::FixL0cToOut},
        {93, aclsan::DeviceInstructionKind::MoveMask},
        {167, aclsan::DeviceInstructionKind::LocalMemoryTransfer},
        {168, aclsan::DeviceInstructionKind::LocalMemoryTransfer},
        {169, aclsan::DeviceInstructionKind::LocalMemoryTransfer},
        {170, aclsan::DeviceInstructionKind::LocalMemoryTransfer},
        {171, aclsan::DeviceInstructionKind::LocalMemoryTransfer},
        {173, aclsan::DeviceInstructionKind::LocalMemoryTransfer},
        {149, aclsan::DeviceInstructionKind::SetL12D},
        {150, aclsan::DeviceInstructionKind::SetL12D},
        {392, aclsan::DeviceInstructionKind::SetPadding},
        {394, aclsan::DeviceInstructionKind::DmaLoopSize},
        {395, aclsan::DeviceInstructionKind::DmaLoopStride},
        {396, aclsan::DeviceInstructionKind::DmaLoopStride},
        {440, aclsan::DeviceInstructionKind::SetFlag},
        {441, aclsan::DeviceInstructionKind::SetFlag},
        {442, aclsan::DeviceInstructionKind::WaitFlag},
        {443, aclsan::DeviceInstructionKind::WaitFlag},
        {448, aclsan::DeviceInstructionKind::GetBuf},
        {449, aclsan::DeviceInstructionKind::GetBuf},
        {450, aclsan::DeviceInstructionKind::RlsBuf},
        {451, aclsan::DeviceInstructionKind::RlsBuf},
        {456, aclsan::DeviceInstructionKind::SetFlag},
        {457, aclsan::DeviceInstructionKind::SetFlag},
        {458, aclsan::DeviceInstructionKind::WaitFlag},
        {459, aclsan::DeviceInstructionKind::WaitFlag},
        {460, aclsan::DeviceInstructionKind::GetBuf},
        {461, aclsan::DeviceInstructionKind::GetBuf},
        {462, aclsan::DeviceInstructionKind::RlsBuf},
        {463, aclsan::DeviceInstructionKind::RlsBuf},
    };

    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    for (const ExpectedInstruction& expected : expectedInstructions) {
        aclsan::AclsanRawTraceRecord record{};
        record.instrId = expected.instructionId;
        record.args[0] = 1;
        record.args[1] = 2;
        record.args[2] = expected.instructionId == 1 ? 32 : (2ULL << 4U) | (64ULL << 25U);
        if ((expected.instructionId >= 448 && expected.instructionId <= 451) ||
            (expected.instructionId >= 460 && expected.instructionId <= 463)) {
            record.args[2] = 3;
        }
        const std::optional<aclsan::DecodedInstruction> decoded = decoder->decode(record);
        assert(decoded.has_value());
        assert(decoded->kind == expected.kind);
    }
}

} // namespace

TEST(DeviceInstrDecoder, DecodesFullCtrlRegisterWrites)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = 159;
    record.args[0] = 0xFEDCBA9876543FC0ULL;
    const auto decoded = decoder->decode(record);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->kind, aclsan::DeviceInstructionKind::SetCtrl);
    const auto* params = std::get_if<aclsan::CtrlRegisterParamField>(&decoded->params);
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(params->value, record.args[0]);
}

TEST(DeviceInstrDecoder, Main)
{
    TestSocVersionMappingsSelectDav3510Decoder();
    TestDecodesDav3510CopyInstruction();
    TestDecodesMovemask93AsCompleteVectorMask();
    TestDecodesInclusiveMovAlignV2Ranges();
    TestDecodesDav3510CopyUbufToGmAlignV2Instruction();
    TestDecodesDav3510CopyGmToCbufMultiInstructions();
    TestDecodesDav3510CopyGmToCbufV2Instruction();
    TestDecodesDav3510LoadGmToCbuf2DV2Instruction();
    TestDecodesDav3510NdDmaOutToUbufInstruction();
    TestDecodesDav3510NdDmaPadCountInstruction();
    TestDecodesDav3510RegisterFieldsWithoutChangingMoverFields();
    TestDecodesDav3510DmaOuterLoopFields();
    TestDecodesDav3510SetL12DInstruction();
    TestDecodesDav3510SetPaddingInstruction();
    TestDecodesDav3510FixL0cToOutInstruction();
    TestDecodesDav3510FixpipeQuantPreHighBit();
    TestDecodesDav3510LocalMemoryTransfersWithoutLosingRawConfig();
    TestDecodesDav3510S4FixpipeUsesSourceIntrinsicId();
    TestDecodesDav3510ScalarGmInstructions();
    TestDecodesDav3510SyncInstruction();
    TestDecodesDav3510BufferInstruction();
    TestRejectsUnknownDav3510Instruction();
    TestDecodesDav3510DataBits();
    TestClassifiesCurrentDav3510InstructionSet();
}
