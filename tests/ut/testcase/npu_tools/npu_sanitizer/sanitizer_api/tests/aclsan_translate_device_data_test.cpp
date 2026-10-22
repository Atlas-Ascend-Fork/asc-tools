/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include <gtest/gtest.h>

#include "../../common/tests/plog_capture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
#include <algorithm>
#include <unistd.h>

#include "aclsan_device_data.h"
#include "aclsan_device_data_log.h"
#include "aclsan_register_dependency.h"
#include "device_instr/decoder_registry.h"

namespace {

constexpr uint32_t COPY_GM_TO_CBUF_ALIGN_V2_B16_ID = 75;
constexpr uint32_t COPY_GM_TO_CBUF_MULTI_ND2NZ_B16_ID = 78;
constexpr uint32_t COPY_GM_TO_CBUF_V2_ID = 73;
constexpr uint32_t COPY_UBUF_TO_GM_ALIGN_V2_ID = 83;
constexpr uint32_t LOAD_GM_TO_CBUF_2D_V2_ID = 72;
constexpr uint32_t ND_DMA_OUT_TO_UBUF_B16_ID = 88;
constexpr uint32_t FIX_L0C_TO_OUT_F32_ID = 91;
constexpr uint64_t C0_SIZE = 32;
constexpr uint32_t SET_FLAG_ID = 440;
constexpr uint32_t SET_FLAG_I_ID = 441;
constexpr uint32_t WAIT_FLAG_ID = 442;
constexpr uint32_t WAIT_FLAG_I_ID = 443;
constexpr uint32_t GET_BUF_ID = 448;
constexpr uint32_t GET_BUF_I_ID = 449;
constexpr uint32_t RLS_BUF_ID = 450;
constexpr uint32_t RLS_BUF_I_ID = 451;
constexpr uint32_t SET_FLAG_V_ID = 456;
constexpr uint32_t SET_FLAG_IV_ID = 457;
constexpr uint32_t WAIT_FLAG_V_ID = 458;
constexpr uint32_t WAIT_FLAG_IV_ID = 459;
constexpr uint32_t GET_BUF_V_ID = 460;
constexpr uint32_t GET_BUF_IV_ID = 461;
constexpr uint32_t RLS_BUF_V_ID = 462;
constexpr uint32_t RLS_BUF_IV_ID = 463;
constexpr uint32_t DC_PRELOAD_ID = 62;
constexpr uint32_t DC_PRELOAD_I_ID = 63;
constexpr uint32_t ST_B64_IMM_ID = 24;
constexpr uint32_t LDP_B32_ID = 53;

static_assert(std::is_same_v<aclsan::DeviceMemoryAccessDataList, std::vector<AclsanDeviceMemoryAccessData>>);

std::optional<aclsan::DecodedInstruction> DecodeRecord(const aclsan::AclsanRawTraceRecord& record)
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    return decoder->decode(record);
}

std::optional<aclsan::DeviceInstructionParamField> TranslateRecord(const aclsan::AclsanRawTraceRecord& record)
{
    const std::optional<aclsan::DecodedInstruction> decoded = DecodeRecord(record);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return decoded->params;
}

aclsan::ParsedTraceRecord MakeParsedTraceRecord(
    uint64_t instrExecId, uint32_t blockId, uint32_t blockType, uint32_t phyCoreId, uint32_t deviceId,
    uint64_t launchId = 0)
{
    aclsan::ParsedTraceRecord parsed{};
    parsed.instrExecId = instrExecId;
    parsed.launchId = launchId;
    parsed.blockId = blockId;
    parsed.blockType = blockType;
    parsed.phyCoreId = phyCoreId;
    parsed.deviceId = deviceId;
    return parsed;
}

std::optional<aclsan::DeviceCallbackData> TranslateRecordToCallbackData(
    const aclsan::AclsanRawTraceRecord& record, aclsan::ParsedTraceRecord parsed,
    const aclsan::MemoryRegisterState& registerState = {})
{
    const std::optional<aclsan::DecodedInstruction> decoded = DecodeRecord(record);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    parsed.record = record;
    auto state = registerState;
    state.ctrl = aclsan::CtrlRegisterParamField{0};
    auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded, state);
    // Existing cases below assert GM conversion and sync behavior. Multi-space
    // translation and unsupported modes are exercised by the Cube integration tests.
    if (callback) {
        if (auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback)) {
            accesses->erase(
                std::remove_if(
                    accesses->begin(), accesses->end(),
                    [](const auto& d) {
                        return d.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM &&
                               (d.header.flags & ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) == 0;
                    }),
                accesses->end());
            for (size_t i = 0; i < accesses->size(); ++i) {
                (*accesses)[i].accessIndex = i;
                (*accesses)[i].accessCount = accesses->size();
            }
        }
    }
    return callback;
}

aclsan::MemoryRegisterState SingletonDmaState(aclsan::DmaLoopDirection direction)
{
    aclsan::MemoryRegisterState state{};
    state.dmaLoopSizes[static_cast<std::size_t>(direction)] = aclsan::DmaLoopSizeParamField{direction, 1, 1};
    return state;
}

void TestTranslateMovOutToL1AlignV2()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = COPY_GM_TO_CBUF_ALIGN_V2_B16_ID;
    record.args[0] = 0x1000;
    record.args[1] = 0x2000;
    record.args[2] = (5ULL << 0) | (0x12345ULL << 4) | (0x23456ULL << 25) | (0x1AULL << 46) | (0x15ULL << 52) |
                     (1ULL << 58) | (5ULL << 60);
    record.args[3] = 0x123456789ULL | (0x34567ULL << 40);

    const auto translated = TranslateRecord(record);

    assert(translated.has_value());
    const auto* params = std::get_if<aclsan::CopyGmToCbufAlignV2ParamField>(&*translated);
    assert(params != nullptr);
    assert(params->instrId == record.instrId);
    assert(params->dstAddr == record.args[0]);
    assert(params->srcAddr == record.args[1]);
    assert(params->sid == 5);
    assert(params->burstNum == 0x12345);
    assert(params->burstLen == 0x23456);
    assert(params->leftPaddingCount == 0x1A);
    assert(params->rightPaddingCount == 0x15);
    assert(params->dataSelectBit);
    assert(params->l2CacheControl == 5);
    assert(params->burstSrcStride == 0x123456789ULL);
    assert(params->burstDstStride == 0x34567);
}

void TestTranslateSetAndWaitFlag()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = SET_FLAG_ID;
    record.args[0] = 2;
    record.args[1] = 3;
    record.args[2] = 7;
    const auto setFlag = TranslateRecord(record);
    assert(setFlag.has_value());
    const auto* setParams = std::get_if<aclsan::FlagParamField>(&*setFlag);
    assert(setParams != nullptr);
    assert(setParams->srcPipe == 2);
    assert(setParams->dstPipe == 3);
    assert(setParams->eventId == 7);

    record.instrId = WAIT_FLAG_ID;
    const auto waitFlag = TranslateRecord(record);
    assert(waitFlag.has_value());
    const auto* waitParams = std::get_if<aclsan::FlagParamField>(&*waitFlag);
    assert(waitParams != nullptr);
    assert(waitParams->instrId == WAIT_FLAG_ID);
}

void TestTranslateAllFlagVariantsToCorrectActions()
{
    aclsan::AclsanRawTraceRecord record{};
    record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    record.args[0] = 4;
    record.args[1] = 1;
    record.args[2] = 5;
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(1, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 2, 3);

    for (uint32_t instructionId : {SET_FLAG_ID, SET_FLAG_I_ID, SET_FLAG_V_ID, SET_FLAG_IV_ID}) {
        record.instrId = instructionId;
        const auto callback = TranslateRecordToCallbackData(record, parsed);
        assert(callback.has_value());
        const auto* sync = std::get_if<AclsanDeviceSyncData>(&*callback);
        assert(sync != nullptr);
        assert(sync->action == ACLSAN_DEVICE_SYNC_ACTION_SET);
        assert(sync->srcPipe == 4);
        assert(sync->dstPipe == 1);
        assert(sync->objectId == 5);
    }

    for (uint32_t instructionId : {WAIT_FLAG_ID, WAIT_FLAG_I_ID, WAIT_FLAG_V_ID, WAIT_FLAG_IV_ID}) {
        record.instrId = instructionId;
        const auto callback = TranslateRecordToCallbackData(record, parsed);
        assert(callback.has_value());
        const auto* sync = std::get_if<AclsanDeviceSyncData>(&*callback);
        assert(sync != nullptr);
        assert(sync->action == ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    }
}

void TestTranslateAllBufferVariantsToCorrectActions()
{
    aclsan::AclsanRawTraceRecord record{};
    record.pc = 0x2000;
    record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    record.args[0] = ACLSAN_DEVICE_PIPE_VECTOR;
    record.args[1] = 0x12345678;
    record.args[2] = 3;
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(6, 2, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 4, 3);

    const uint32_t getInstructionIds[] = {GET_BUF_ID, GET_BUF_I_ID, GET_BUF_V_ID, GET_BUF_IV_ID};
    for (uint32_t instructionId : getInstructionIds) {
        record.instrId = instructionId;
        const auto callback = TranslateRecordToCallbackData(record, parsed);
        assert(callback.has_value());
        const auto* sync = std::get_if<AclsanDeviceSyncData>(&*callback);
        assert(sync != nullptr);
        assert(sync->header.pc == record.pc);
        assert(sync->header.instrExecId == parsed.instrExecId);
        assert(sync->header.blockId == parsed.blockId);
        assert(sync->header.blockType == parsed.blockType);
        assert(sync->header.phyCoreId == parsed.phyCoreId);
        assert(sync->syncKind == ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF);
        assert(sync->action == ACLSAN_DEVICE_SYNC_ACTION_GET);
        assert(sync->scope == ACLSAN_DEVICE_SYNC_SCOPE_PIPE);
        assert(sync->srcPipe == 0);
        assert(sync->dstPipe == ACLSAN_DEVICE_PIPE_VECTOR);
        assert(sync->mode == 3);
        assert(sync->objectId == 0x12345678);
    }

    const uint32_t releaseInstructionIds[] = {RLS_BUF_ID, RLS_BUF_I_ID, RLS_BUF_V_ID, RLS_BUF_IV_ID};
    for (uint32_t instructionId : releaseInstructionIds) {
        record.instrId = instructionId;
        const auto callback = TranslateRecordToCallbackData(record, parsed);
        assert(callback.has_value());
        const auto* sync = std::get_if<AclsanDeviceSyncData>(&*callback);
        assert(sync != nullptr);
        assert(sync->syncKind == ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF);
        assert(sync->action == ACLSAN_DEVICE_SYNC_ACTION_RELEASE);
        assert(sync->dstPipe == ACLSAN_DEVICE_PIPE_VECTOR);
        assert(sync->mode == 3);
        assert(sync->objectId == 0x12345678);
    }
}

void TestTranslateCopyUbufToGmAlignV2()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = COPY_UBUF_TO_GM_ALIGN_V2_ID;
    record.args[0] = 0x3000;
    record.args[1] = 0x4000;
    record.args[2] = (5ULL << 0) | (0x12345ULL << 4) | (0x23456ULL << 25) | (5ULL << 60);
    record.args[3] = 0x123456789ULL | (0x34567ULL << 40);

    const auto translated = TranslateRecord(record);

    assert(translated.has_value());
    const auto* params = std::get_if<aclsan::CopyUbufToGmAlignV2ParamField>(&*translated);
    assert(params != nullptr);
    assert(params->instrId == record.instrId);
    assert(params->dstAddr == record.args[0]);
    assert(params->srcAddr == record.args[1]);
    assert(params->sid == 5);
    assert(params->burstNum == 0x12345);
    assert(params->burstLen == 0x23456);
    assert(params->l2CacheControl == 5);
    assert(params->dstStride == 0x123456789ULL);
    assert(params->srcStride == 0x34567);
}

void AssertRangeLayout(const AclsanDeviceMemoryAccessData& access, uint64_t bytes)
{
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(access.layout.range.bytes == bytes);
}

void AssertLinearBlockRepeatLayout(
    const AclsanDeviceMemoryAccessData& access, uint32_t repeatTimes, uint32_t blockSize, int64_t repeatStride)
{
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
    assert(access.layout.blockRepeat.blockNum == 1);
    assert(access.layout.blockRepeat.blockSize == blockSize);
    assert(access.layout.blockRepeat.blockStride == 0);
    assert(access.layout.blockRepeat.repeatTimes == repeatTimes);
    assert(access.layout.blockRepeat.repeatStride == repeatStride);
}

aclsan::AclsanRawTraceRecord MakePaddedCopyGmToCbufAlignV2Record(
    uint64_t sourceStride, uint32_t destinationStride, uint32_t burstNum = 3)
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = COPY_GM_TO_CBUF_ALIGN_V2_B16_ID;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = (static_cast<uint64_t>(burstNum) << 4) | (64ULL << 25) | (2ULL << 46) | (1ULL << 52);
    record.args[3] = sourceStride | (static_cast<uint64_t>(destinationStride) << 40);
    return record;
}

void TestMakesRangeLayoutForAtMostOneBurst()
{
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(1, 1, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 0, 3);

    const auto emptyCallback = TranslateRecordToCallbackData(MakePaddedCopyGmToCbufAlignV2Record(96, 96, 0), parsed);
    assert(emptyCallback.has_value());
    const auto* emptyAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*emptyCallback);
    assert(emptyAccesses != nullptr);
    assert(emptyAccesses->empty());

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto callback = TranslateRecordToCallbackData(MakePaddedCopyGmToCbufAlignV2Record(96, 96, 1), parsed, state);
    assert(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    assert(accesses != nullptr);
    assert(accesses->size() == 1);
    AssertRangeLayout(accesses->front(), 64);
}

void TestMakesRangeLayoutsFromContinuousParamFieldData()
{
    const aclsan::AclsanRawTraceRecord record = MakePaddedCopyGmToCbufAlignV2Record(64, 70);
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(1, 1, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 0, 3);

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto callback = TranslateRecordToCallbackData(record, parsed, state);

    assert(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    assert(accesses != nullptr);
    assert(accesses->size() == 1);
    AssertRangeLayout(accesses->front(), 192);
}

void TestMakesBlockRepeatOnlyForNonContinuousSource()
{
    const aclsan::AclsanRawTraceRecord record = MakePaddedCopyGmToCbufAlignV2Record(96, 70);
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(1, 1, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 0, 3);

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto callback = TranslateRecordToCallbackData(record, parsed, state);

    assert(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    assert(accesses != nullptr);
    assert(accesses->size() == 1);
    AssertLinearBlockRepeatLayout(accesses->front(), 3, 64, 96);
}

void TestIgnoresNonGmDestinationStride()
{
    const aclsan::AclsanRawTraceRecord record = MakePaddedCopyGmToCbufAlignV2Record(64, 96);
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(1, 1, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 0, 3);

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto callback = TranslateRecordToCallbackData(record, parsed, state);

    assert(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    assert(accesses != nullptr);
    assert(accesses->size() == 1);
    AssertRangeLayout(accesses->front(), 192);
}

void TestTranslateRawTraceToCallbackData()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = COPY_GM_TO_CBUF_ALIGN_V2_B16_ID;
    record.pc = 0x100;
    record.siteId = 2;
    record.args[0] = 0x200040;
    record.args[1] = 0x100040;
    record.args[2] = (2ULL << 4) | (64ULL << 25);
    record.args[3] = 64ULL | (64ULL << 40);
    const aclsan::ParsedTraceRecord parsed =
        MakeParsedTraceRecord(1002, 1, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 0, 3, 17);

    const auto gmToCbufState = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto memoryCallback = TranslateRecordToCallbackData(record, parsed, gmToCbufState);
    assert(memoryCallback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*memoryCallback);
    assert(accesses != nullptr);
    assert(accesses->size() == 1);
    const AclsanDeviceMemoryAccessData& source = (*accesses)[0];
    assert(source.header.pc == record.pc);
    assert(source.header.siteId == record.siteId);
    assert(source.header.blockId == parsed.blockId);
    assert(source.header.blockType == parsed.blockType);
    assert(source.header.launchId == parsed.launchId);
    assert(source.header.instrExecId == parsed.instrExecId);
    assert(source.header.serialNo == 0);
    assert(source.header.deviceId == parsed.deviceId);
    assert(source.header.phyCoreId == parsed.phyCoreId);
    assert(source.address == record.args[1]);
    assert(source.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(source.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(source.accessIndex == 0);
    assert(source.accessCount == 1);
    AssertRangeLayout(source, 128);

    record.instrId = COPY_UBUF_TO_GM_ALIGN_V2_ID;
    record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    record.args[0] = 0x300040;
    record.args[1] = 0x400040;
    const auto ubToGmState = SingletonDmaState(aclsan::DmaLoopDirection::UBUF_TO_GM);
    const auto ubToGmCallback = TranslateRecordToCallbackData(record, parsed, ubToGmState);
    assert(ubToGmCallback.has_value());
    const auto* ubToGmAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*ubToGmCallback);
    assert(ubToGmAccesses != nullptr);
    assert(ubToGmAccesses->size() == 1);
    const AclsanDeviceMemoryAccessData& gmDestination = (*ubToGmAccesses)[0];
    assert(gmDestination.address == record.args[0]);
    assert(gmDestination.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(gmDestination.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    assert(gmDestination.accessIndex == 0);
    assert(gmDestination.header.pc == record.pc);
    assert(gmDestination.header.instrExecId == parsed.instrExecId);
    assert(gmDestination.header.serialNo == 0);

    record.instrId = SET_FLAG_ID;
    record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    record.args[0] = 2;
    record.args[1] = 3;
    record.args[2] = 7;
    const auto setCallback = TranslateRecordToCallbackData(record, parsed);
    assert(setCallback.has_value());
    const auto* setSync = std::get_if<AclsanDeviceSyncData>(&*setCallback);
    assert(setSync != nullptr);
    assert(setSync->header.version == ACLSAN_API_VERSION);
    assert(setSync->header.size == sizeof(AclsanDeviceSyncData));
    assert(setSync->header.launchId == parsed.launchId);
    assert(setSync->header.sourceKind == ACLSAN_DEVICE_SOURCE_SET_WAIT_FLAG);
    assert(setSync->action == ACLSAN_DEVICE_SYNC_ACTION_SET);
    assert(setSync->srcPipe == 2);
    assert(setSync->dstPipe == 3);
    assert(setSync->objectId == 7);
    assert(setSync->header.instrExecId == parsed.instrExecId);
    assert(setSync->header.serialNo == 0);
    assert(setSync->header.blockId == parsed.blockId);
    assert(setSync->header.blockType == parsed.blockType);
    assert(setSync->header.phyCoreId == parsed.phyCoreId);

    record.instrId = WAIT_FLAG_ID;
    const auto waitCallback = TranslateRecordToCallbackData(record, parsed);
    assert(waitCallback.has_value());
    const auto* waitSync = std::get_if<AclsanDeviceSyncData>(&*waitCallback);
    assert(waitSync != nullptr);
    assert(waitSync->action == ACLSAN_DEVICE_SYNC_ACTION_WAIT);
    assert(waitSync->header.sourceKind == ACLSAN_DEVICE_SOURCE_SET_WAIT_FLAG);
}

void TestTranslateMultiAndFixpipeToGmCbdata()
{
    aclsan::AclsanRawTraceRecord multi{};
    multi.instrId = COPY_GM_TO_CBUF_MULTI_ND2NZ_B16_ID;
    multi.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    multi.args[1] = 0x5000;
    multi.args[2] = (64ULL << 4U) | (4ULL << 48U);
    multi.args[3] = 8ULL | (512ULL << 21U);
    const aclsan::ParsedTraceRecord parsed =
        MakeParsedTraceRecord(20, 3, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 19, 2, 9);
    aclsan::MemoryRegisterState multiState{};
    multiState.mte2Nz = aclsan::Mte2NzParamField{2, 0, 0, 0};

    const auto multiCallback = TranslateRecordToCallbackData(multi, parsed, multiState);
    assert(multiCallback.has_value());
    const auto* multiAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*multiCallback);
    assert(multiAccesses != nullptr);
    assert(multiAccesses->size() == 1);
    const auto& multiAccess = multiAccesses->front();
    assert(multiAccess.address == multi.args[1]);
    assert(multiAccess.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(multiAccess.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(multiAccess.layout.ndAffine.rank == 2);
    assert(multiAccess.layout.ndAffine.elementBytes == 16);
    assert(multiAccess.layout.ndAffine.dims[0] == 4);
    assert(multiAccess.layout.ndAffine.dims[1] == 2);
    assert(multiAccess.layout.ndAffine.strides[0] == 64);
    assert(multiAccess.layout.ndAffine.strides[1] == 512);

    aclsan::AclsanRawTraceRecord fixpipe{};
    fixpipe.instrId = FIX_L0C_TO_OUT_F32_ID;
    fixpipe.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    fixpipe.args[0] = 0x8000;
    fixpipe.args[2] = (48ULL << 4U) | (4ULL << 16U) | (64ULL << 32U);
    fixpipe.args[3] = 24ULL << 34U;

    const auto fixpipeCallback = TranslateRecordToCallbackData(fixpipe, parsed);
    assert(fixpipeCallback.has_value());
    const auto* fixpipeAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*fixpipeCallback);
    assert(fixpipeAccesses != nullptr);
    assert(fixpipeAccesses->size() == 2);
    assert((*fixpipeAccesses)[0].address == 0x8000);
    assert((*fixpipeAccesses)[0].layout.range.bytes == 128);
    assert((*fixpipeAccesses)[1].address == 0x8040);
    assert((*fixpipeAccesses)[1].layout.range.bytes == 64);
}

std::string CaptureTranslateDebugLogs(
    const aclsan::AclsanRawTraceRecord& record, const aclsan::ParsedTraceRecord& parsed)
{
    PlogCapture capture;
    const auto translated = TranslateRecordToCallbackData(record, parsed);
    assert(translated.has_value());
    return capture.Text();
}

std::string CaptureCallbackDataDebugLogs(const aclsan::DeviceCallbackData& callbackData)
{
    PlogCapture capture;
    aclsan::LogCallbackData(callbackData);
    return capture.Text();
}

void TestLogsEveryMemoryAccessInVariableLengthList()
{
    aclsan::DeviceMemoryAccessDataList accesses(3);
    for (std::size_t index = 0; index < accesses.size(); ++index) {
        accesses[index].address = 0x1000 + index;
        accesses[index].accessIndex = static_cast<uint32_t>(index);
        accesses[index].accessCount = static_cast<uint32_t>(accesses.size());
    }
    accesses[0].header.deviceId = 2;
    accesses[0].header.phyCoreId = 1;
    accesses[0].header.blockId = 3;
    accesses[0].header.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE;
    accesses[0].header.instrExecId = 4;
    accesses[0].header.launchId = 5;

    const std::string logs = CaptureCallbackDataDebugLogs(aclsan::DeviceCallbackData{accesses});

    assert(logs.find("[cbdata] type=AclsanDeviceMemoryAccessData index=0") != std::string::npos);
    assert(
        logs.find("deviceId=2 phyCoreId=1 blockId=3 blockType=AICORE instrExecId=4 launchId=5") != std::string::npos);
    assert(logs.find("index=0 address=0x1000") != std::string::npos);
    assert(logs.find("index=1 address=0x1001") != std::string::npos);
    assert(logs.find("index=2 address=0x1002") != std::string::npos);
}

void TestTranslateDebugLogsShowSyncConversion()
{
    aclsan::AclsanRawTraceRecord record{};
    record.pc = 0x1234;
    record.instrId = SET_FLAG_ID;
    record.siteId = 5;
    record.category = aclsan::DeviceInstructionCategory::Synchronization;
    record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    record.args[0] = 2;
    record.args[1] = 3;
    record.args[2] = 7;
    const aclsan::ParsedTraceRecord parsed =
        MakeParsedTraceRecord(9, 4, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 6, 3, 12);

    const std::string logs = CaptureTranslateDebugLogs(record, parsed);
    assert(logs.find("[raw]") != std::string::npos);
    if (logs.find("[raw] deviceId=3 phyCoreId=6 blockId=4 blockType=AIC  instrExecId=9 launchId=12  "
                  "type=AclsanRawTraceRecord pc=0x1234") == std::string::npos) {
        std::abort();
    }
    assert(logs.find("instrId=440") != std::string::npos);
    assert(logs.find("siteId=5 category=2 pipeline=" + std::to_string(ACLSAN_DEVICE_PIPE_SCALAR)) != std::string::npos);
    assert(logs.find("args=[0x2,0x3,0x7,0x0,0x0]") != std::string::npos);
    assert(logs.find("instrExecId=9") != std::string::npos);
    assert(logs.find("[param]") != std::string::npos);
    assert(logs.find("type=FlagParamField") != std::string::npos);
    assert(logs.find("srcPipe=2 dstPipe=3 eventId=7") != std::string::npos);
    assert(logs.find("[cbdata]") != std::string::npos);
    assert(logs.find("type=AclsanDeviceSyncData") != std::string::npos);
    if (logs.find("[cbdata] deviceId=3 phyCoreId=6 blockId=4 blockType=AIC  instrExecId=9 launchId=12  "
                  "type=AclsanDeviceSyncData pc=0x1234") == std::string::npos) {
        std::abort();
    }
    assert(logs.find("action=1") != std::string::npos);
    assert(logs.find("objectId=7") != std::string::npos);
}

void TestTranslateDebugLogsShowBufferConversion()
{
    aclsan::AclsanRawTraceRecord record{};
    record.pc = 0x2345;
    record.instrId = GET_BUF_V_ID;
    record.category = aclsan::DeviceInstructionCategory::Synchronization;
    record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    record.args[0] = ACLSAN_DEVICE_PIPE_VECTOR;
    record.args[1] = 0x12345678;
    record.args[2] = 3;
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(10, 5, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 7, 3);

    const std::string logs = CaptureTranslateDebugLogs(record, parsed);
    assert(logs.find("[param] type=SyncBufParamField") != std::string::npos);
    assert(
        logs.find("pipe=" + std::to_string(ACLSAN_DEVICE_PIPE_VECTOR) + " bufId=0x12345678 mode=3") !=
        std::string::npos);
    assert(
        logs.find("[cbdata] deviceId=3 phyCoreId=7 blockId=5 blockType=AIV  instrExecId=10 launchId=0  "
                  "type=AclsanDeviceSyncData") != std::string::npos);
    assert(
        logs.find(
            "syncKind=2 action=3 scope=1 srcPipe=0 dstPipe=" + std::to_string(ACLSAN_DEVICE_PIPE_VECTOR) + " mode=3") !=
        std::string::npos);
    assert(logs.find("objectId=305419896") != std::string::npos);
}

void TestTranslateDebugLogsShowUbufToGmConversion()
{
    aclsan::AclsanRawTraceRecord record{};
    record.pc = 0x5678;
    record.instrId = COPY_UBUF_TO_GM_ALIGN_V2_ID;
    record.siteId = 4;
    record.category = aclsan::DeviceInstructionCategory::MemoryAccess;
    record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    record.args[0] = 0x3000;
    record.args[1] = 0x4000;
    record.args[2] = (2ULL << 4) | (64ULL << 25);
    record.args[3] = 0x1234;
    const aclsan::ParsedTraceRecord parsed =
        MakeParsedTraceRecord(11, 3, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR, 5, 3, 13);

    const std::string logs = CaptureTranslateDebugLogs(record, parsed);
    assert(
        logs.find("[raw] deviceId=3 phyCoreId=5 blockId=3 blockType=AIV  instrExecId=11 launchId=13  "
                  "type=AclsanRawTraceRecord") != std::string::npos);
    assert(
        logs.find("instrId=83 siteId=4 category=1 pipeline=" + std::to_string(ACLSAN_DEVICE_PIPE_MTE3)) !=
        std::string::npos);
    assert(logs.find("[param] type=CopyUbufToGmAlignV2ParamField") != std::string::npos);
    assert(logs.find("burstNum=2 burstLen=64") != std::string::npos);
    assert(logs.find("[cbdata] type=AclsanDeviceMemoryAccessData index=0") != std::string::npos);
    assert(logs.find("deviceId=3 phyCoreId=5 blockId=3 blockType=AIV instrExecId=11 launchId=13") != std::string::npos);
}

void TestTranslateCopyGmToCbufV2ToCallbackData()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = COPY_GM_TO_CBUF_V2_ID;
    record.pc = 0x6000;
    record.siteId = 8;
    record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    record.args[0] = 0x3000;
    record.args[1] = 0x2000;
    record.args[2] = (7ULL << 4) | (11ULL << 25);
    record.args[3] = 11ULL | (11ULL << 40);
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(12, 6, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 4, 3);

    const auto state = SingletonDmaState(aclsan::DmaLoopDirection::GM_TO_CBUF);
    const auto callback = TranslateRecordToCallbackData(record, parsed, state);

    assert(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    assert(accesses != nullptr);
    assert(accesses->size() == 1);
    const AclsanDeviceMemoryAccessData& source = accesses->front();
    assert(source.header.version == ACLSAN_API_VERSION);
    assert(source.header.size == sizeof(AclsanDeviceMemoryAccessData));
    assert(source.header.pc == record.pc);
    assert(source.header.siteId == record.siteId);
    assert(source.header.sourceKind == ACLSAN_DEVICE_SOURCE_MTE2);
    assert(source.header.pipeline == ACLSAN_DEVICE_PIPE_MTE2);
    assert(source.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    assert(source.address == record.args[1]);
    assert(source.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(source.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(source.accessIndex == 0);
    assert(source.accessCount == 1);
    assert(source.dataBits == 0);
    assert(source.header.serialNo == 0);
    AssertRangeLayout(source, 7 * 11 * C0_SIZE);
}

void AssertSingleGmAccess(
    const AclsanDeviceMemoryAccessData& access, uint64_t address, AclsanDeviceMemoryAccessMode accessMode,
    uint32_t dataBits)
{
    assert(access.address == address);
    assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(access.accessMode == accessMode);
    assert(access.accessIndex == 0);
    assert(access.accessCount == 1);
    assert(access.header.serialNo == 0);
    assert(access.dataBits == dataBits);
}

void TestTranslateStateDependentDmaFieldsToCallbackData()
{
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(13, 7, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 5, 3);
    aclsan::AclsanRawTraceRecord record{};
    record.args[0] = 0x7000;
    record.args[1] = 0x6000;

    record.instrId = LOAD_GM_TO_CBUF_2D_V2_ID;
    record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    record.args[2] = 0;
    record.args[3] = (2ULL << 12U) | (3ULL << 24U);
    aclsan::MemoryRegisterState load2DState{};
    load2DState.mte2Source = aclsan::Mte2SourceParamField{4};
    const auto load2DCallback = TranslateRecordToCallbackData(record, parsed, load2DState);
    assert(load2DCallback.has_value());
    const auto* load2DAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*load2DCallback);
    assert(load2DAccesses != nullptr);
    assert(load2DAccesses->size() == 1);
    AssertSingleGmAccess(load2DAccesses->front(), record.args[1], ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0);
    assert(load2DAccesses->front().layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);

    record.args[2] = 2ULL << 32U;
    record.args[3] = (1ULL << 12U) | (3ULL << 24U) | (2ULL << 40U);
    const auto skippedLoad2DCallback = TranslateRecordToCallbackData(record, parsed);
    assert(!skippedLoad2DCallback.has_value());

    record.args[3] = (1ULL << 12U) | (3ULL << 24U);
    load2DState.mte2Source = aclsan::Mte2SourceParamField{-1};
    const auto negativeStrideLoad2DCallback = TranslateRecordToCallbackData(record, parsed, load2DState);
    assert(negativeStrideLoad2DCallback.has_value());
    const auto* negativeStrideLoad2DAccesses =
        std::get_if<aclsan::DeviceMemoryAccessDataList>(&*negativeStrideLoad2DCallback);
    assert(negativeStrideLoad2DAccesses != nullptr);
    assert(negativeStrideLoad2DAccesses->size() == 1);
    const auto& negativeStrideLoad2DAccess = negativeStrideLoad2DAccesses->front();
    AssertSingleGmAccess(negativeStrideLoad2DAccess, record.args[1], ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0);
    assert(negativeStrideLoad2DAccess.layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
    assert(negativeStrideLoad2DAccess.layout.range.bytes == 3 * 512);

    record.instrId = ND_DMA_OUT_TO_UBUF_B16_ID;
    record.args[2] = (3ULL << 4U) | (2ULL << 24U) | (1ULL << 44U);
    record.args[3] = 1ULL | (1ULL << 20U);
    aclsan::MemoryRegisterState ndDmaState{};
    ndDmaState.ndDmaPadCount = aclsan::NdDmaPadCountParamField{};
    ndDmaState.ndDmaLoopStrides[0] = aclsan::NdDmaLoopStrideParamField{0, 1};
    ndDmaState.ndDmaLoopStrides[1] = aclsan::NdDmaLoopStrideParamField{1, 8};
    ndDmaState.ndDmaLoopStrides[2] = aclsan::NdDmaLoopStrideParamField{2, 0};
    ndDmaState.ndDmaLoopStrides[3] = aclsan::NdDmaLoopStrideParamField{3, 0};
    ndDmaState.ndDmaLoopStrides[4] = aclsan::NdDmaLoopStrideParamField{4, 0};
    const auto ndDmaCallback = TranslateRecordToCallbackData(record, parsed, ndDmaState);
    assert(ndDmaCallback.has_value());
    const auto* ndDmaAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*ndDmaCallback);
    assert(ndDmaAccesses != nullptr);
    assert(ndDmaAccesses->size() == 1);
    AssertSingleGmAccess(ndDmaAccesses->front(), record.args[1], ACLSAN_DEVICE_MEMORY_ACCESS_READ, 16);
    assert(ndDmaAccesses->front().layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(ndDmaAccesses->front().layout.ndAffine.rank == 5);
    assert(ndDmaAccesses->front().layout.ndAffine.elementBytes == 2);
    assert(ndDmaAccesses->front().layout.ndAffine.dims[0] == 3);
    assert(ndDmaAccesses->front().layout.ndAffine.dims[1] == 2);
    assert(ndDmaAccesses->front().layout.ndAffine.strides[0] == 2);
    assert(ndDmaAccesses->front().layout.ndAffine.strides[1] == 16);

    record.instrId = FIX_L0C_TO_OUT_F32_ID;
    record.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    record.args[2] = (16ULL << 4U) | (4ULL << 16U) | (64ULL << 32U);
    record.args[3] = 0;
    const auto fixCallback = TranslateRecordToCallbackData(record, parsed);
    assert(fixCallback.has_value());
    const auto* fixAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*fixCallback);
    assert(fixAccesses != nullptr);
    assert(fixAccesses->size() == 1);
    AssertSingleGmAccess(fixAccesses->front(), record.args[0], ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 32);
    AssertRangeLayout(fixAccesses->front(), 256);
}

void TestMissingRegisterStateProducesEstimatedDependencyEvent()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = LOAD_GM_TO_CBUF_2D_V2_ID;
    record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    record.pc = 0x1234;
    record.args[1] = 0x8000;
    record.args[3] = (2ULL << 12U) | (3ULL << 24U);
    const auto parsed = MakeParsedTraceRecord(21, 4, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 2, 1, 9);

    const auto estimated = TranslateRecordToCallbackData(record, parsed);
    assert(estimated.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*estimated);
    assert(accesses != nullptr);
    assert(accesses->size() == 1);
    const auto& access = accesses->front();
    assert(access.header.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED);
    assert(access.header.size == sizeof(AclsanDeviceMemoryAccessData));
    assert(access.instructionId == LOAD_GM_TO_CBUF_2D_V2_ID);
    assert(access.regDependencyMask0 == (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE));
    assert(access.regDependencyMask1 == 0);

    aclsan::MemoryRegisterState state{};
    state.mte2Source = aclsan::Mte2SourceParamField{0};
    const auto exact = TranslateRecordToCallbackData(record, parsed, state);
    assert(exact.has_value());
    const auto* exactAccesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*exact);
    assert(exactAccesses != nullptr);
    assert(exactAccesses->size() == 1);
    assert(exactAccesses->front().header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    assert(exactAccesses->front().regDependencyMask0 == (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE));
}

void TestConditionalRegisterDependencies()
{
    aclsan::MemoryRegisterState state{};
    aclsan::CopyGmToCbufV2ParamField copy{};
    copy.burstNum = 1;
    copy.burstLen = 1;
    copy.padFunctionMode = 2;
    const auto missingLoopAndPadding = aclsan::ResolveRegisterDependencies(aclsan::MemoryInstructionField{copy}, state);
    assert(
        missingLoopAndPadding.mask0 == ((UINT64_C(1) << ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_CBUF) |
                                        (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_SET_PADDING)));

    copy.padFunctionMode = 0;
    const auto withoutPadding = aclsan::ResolveRegisterDependencies(aclsan::MemoryInstructionField{copy}, state);
    assert(withoutPadding.mask0 == (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_GM_TO_CBUF));

    copy.burstNum = 0;
    copy.padFunctionMode = 2;
    const auto noAccess = aclsan::ResolveRegisterDependencies(aclsan::MemoryInstructionField{copy}, state);
    assert(noAccess.mask0 == 0);
}

void TestTranslatesLocalMemoryTransfersToOwnedAccesses()
{
    constexpr uint32_t AIC_CORE = 5;
    constexpr uint32_t AIV_CORE = 29; // 18 AICs per die + 2 * AIC_CORE + subBlockId 1.
    constexpr uint32_t AIC_BLOCK = 2;
    constexpr uint32_t AIV_BLOCK = 5;
    const auto parsed = MakeParsedTraceRecord(14, AIC_BLOCK, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, AIC_CORE, 3);
    constexpr uint32_t LOCAL_INSTRUCTION_IDS[] = {158, 167, 168, 169, 170, 171, 173};
    for (const uint64_t instructionId : LOCAL_INSTRUCTION_IDS) {
        SCOPED_TRACE(instructionId);
        auto trace = parsed;
        trace.physicalCoreCount = 108;
        auto& record = trace.record;
        record.instrId = instructionId;
        record.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
        record.args[0] = 0x2000;
        record.args[1] = 0x1000;
        record.args[2] = (16ULL << 4) | (16ULL << 16) | (256ULL << 32);
        record.args[3] = 16;
        if (instructionId == 158 || instructionId == 173) {
            record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
            record.args[2] = (1ULL << 4) | (1ULL << 16);
            record.args[3] = 0;
            if (instructionId == 158) {
                record.args[2] |= 1; // Select the second AIV for the UB destination.
            } else {
                trace.phyCoreId = AIV_CORE;
                trace.blockId = AIV_BLOCK;
                trace.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
            }
        } else if (instructionId == 170 || instructionId == 171) {
            record.args[3] |= 1ULL << 18; // FIXPIPE subBlockId 1.
        }

        const auto decoded = DecodeRecord(record);
        ASSERT_TRUE(decoded.has_value());
        aclsan::MemoryRegisterState state{};
        state.ctrl = aclsan::CtrlRegisterParamField{0};
        // Verify the complete callback; the legacy GM-only helper filters out local accesses.
        const auto callback = aclsan::TranslateDecodedTraceToCallbackData(trace, *decoded, state);
        ASSERT_TRUE(callback.has_value());
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
        ASSERT_NE(accesses, nullptr);
        ASSERT_EQ(accesses->size(), instructionId == 167 ? 1U : 2U);

        const uint32_t sourceSpace = instructionId == 173                         ? ACLSAN_DEVICE_MEMORY_SPACE_UB :
                                     instructionId == 158 || instructionId == 167 ? ACLSAN_DEVICE_MEMORY_SPACE_L1 :
                                                                                    ACLSAN_DEVICE_MEMORY_SPACE_L0C;
        const uint32_t destinationSpace = instructionId == 158 || instructionId == 170 || instructionId == 171 ?
                                              ACLSAN_DEVICE_MEMORY_SPACE_UB :
                                              ACLSAN_DEVICE_MEMORY_SPACE_L1;
        for (const auto& access : *accesses) {
            const bool read = access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ;
            const bool ub = access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB;
            EXPECT_EQ(access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
            EXPECT_EQ(access.instructionId, instructionId);
            EXPECT_EQ(access.header.instrExecId, trace.instrExecId);
            EXPECT_EQ(access.header.pipeline, record.pipeline);
            EXPECT_EQ(access.memorySpace, read ? sourceSpace : destinationSpace);
            EXPECT_EQ(access.address, read ? record.args[1] : record.args[0]);
            EXPECT_EQ(access.header.phyCoreId, ub ? AIV_CORE : AIC_CORE);
            EXPECT_EQ(access.header.blockId, ub ? AIV_BLOCK : AIC_BLOCK);
            EXPECT_EQ(
                access.header.blockType,
                ub ? ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR : ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
        }
        EXPECT_EQ(
            std::count_if(
                accesses->begin(), accesses->end(),
                [](const auto& access) { return access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ; }),
            1);
        EXPECT_EQ(
            std::count_if(
                accesses->begin(), accesses->end(),
                [](const auto& access) { return access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE; }),
            instructionId == 167 ? 0 : 1);
    }

    aclsan::AclsanRawTraceRecord logged{};
    logged.instrId = 167;
    logged.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    logged.args[0] = 0x2000;
    logged.args[1] = 0x1000;
    logged.args[2] = UINT64_C(0x1122334455667788);
    logged.args[3] = UINT64_C(0x99aabbccddeeff00);
    const std::string logs = CaptureTranslateDebugLogs(logged, parsed);
    assert(logs.find("type=LocalMemoryTransferParamField instrId=167") != std::string::npos);
    assert(logs.find("config0=0x1122334455667788 config1=0x99aabbccddeeff00 kind=0 localOnly=1") != std::string::npos);
    assert(logs.find("unsupported") == std::string::npos);
}

void TestKeepsScalarPreloadInTraceWithoutMemoryAccessCbdata()
{
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(15, 2, ACLSAN_DEVICE_BLOCK_TYPE_AICORE, 4, 3);
    for (const uint32_t instructionId : {DC_PRELOAD_ID, DC_PRELOAD_I_ID}) {
        aclsan::AclsanRawTraceRecord record{};
        record.instrId = instructionId;
        record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        record.args[0] = UINT64_C(0x123456789abcdef0);
        record.args[1] = UINT64_C(0xffffffffffffffff);

        const auto callback = TranslateRecordToCallbackData(record, parsed);
        ASSERT_TRUE(callback.has_value());
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
        ASSERT_NE(accesses, nullptr);
        EXPECT_TRUE(accesses->empty());

        const std::string logs = CaptureTranslateDebugLogs(record, parsed);
        EXPECT_NE(logs.find("type=ScalarPreloadParamField"), std::string::npos);
        EXPECT_EQ(logs.find("DEVICE_MEMORY_ACCESS"), std::string::npos);
    }
}

void TestTranslatesScalarGmInstructionsToMemoryAccessCbdata()
{
    const aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(16, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 2, 3);
    struct Case {
        uint32_t instructionId;
        uint32_t accessMode;
        uint32_t dataBits;
        uint64_t bytes;
    };
    for (const Case& test :
         {Case{ST_B64_IMM_ID, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 64, 8},
          Case{LDP_B32_ID, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 32, 8}}) {
        aclsan::AclsanRawTraceRecord record{};
        record.instrId = test.instructionId;
        record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        record.args[0] = UINT64_C(0x8000000200001000);
        record.args[1] = 8;
        record.args[2] = 0;
        record.args[3] = UINT64_C(0x100000000);
        record.args[4] = aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;

        const auto callback = TranslateRecordToCallbackData(record, parsed);
        ASSERT_TRUE(callback.has_value());
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
        ASSERT_NE(accesses, nullptr);
        ASSERT_EQ(accesses->size(), 1U);
        EXPECT_EQ(accesses->front().address, UINT64_C(0x200001008));
        EXPECT_EQ(accesses->front().accessMode, test.accessMode);
        EXPECT_EQ(accesses->front().dataBits, test.dataBits);
        EXPECT_EQ(accesses->front().layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
        EXPECT_EQ(accesses->front().layout.range.bytes, test.bytes);

        const std::string logs = CaptureTranslateDebugLogs(record, parsed);
        EXPECT_NE(logs.find("type=ScalarGmParamField"), std::string::npos);
        EXPECT_NE(logs.find("type=AclsanDeviceMemoryAccessData"), std::string::npos);
    }
}

void TestRejectsUnsupportedCallbackParamField()
{
    aclsan::ParsedTraceRecord parsed{};
    const aclsan::DecodedInstruction decoded{
        aclsan::DeviceInstructionKind::CopyGmToCbufMulti, aclsan::CopyGmToCbufMultiDn2NzParamField{}};

    const auto result = aclsan::TranslateDecodedTraceToCallbackData(parsed, decoded);
    assert(!result.has_value());
}

void TestRejectsUnknownInstruction()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = UINT32_MAX;
    assert(!TranslateRecord(record).has_value());
}

} // namespace

TEST(AclsanTranslateDeviceData, Main)
{
    TestTranslateMovOutToL1AlignV2();
    TestTranslateSetAndWaitFlag();
    TestTranslateAllFlagVariantsToCorrectActions();
    TestTranslateAllBufferVariantsToCorrectActions();
    TestTranslateCopyUbufToGmAlignV2();
    TestMakesRangeLayoutForAtMostOneBurst();
    TestMakesRangeLayoutsFromContinuousParamFieldData();
    TestMakesBlockRepeatOnlyForNonContinuousSource();
    TestIgnoresNonGmDestinationStride();
    TestTranslateRawTraceToCallbackData();
    TestLogsEveryMemoryAccessInVariableLengthList();
    TestTranslateMultiAndFixpipeToGmCbdata();
    TestTranslateDebugLogsShowSyncConversion();
    TestTranslateDebugLogsShowBufferConversion();
    TestTranslateDebugLogsShowUbufToGmConversion();
    TestTranslateCopyGmToCbufV2ToCallbackData();
    TestTranslateStateDependentDmaFieldsToCallbackData();
    TestMissingRegisterStateProducesEstimatedDependencyEvent();
    TestConditionalRegisterDependencies();
    TestTranslatesLocalMemoryTransfersToOwnedAccesses();
    TestKeepsScalarPreloadInTraceWithoutMemoryAccessCbdata();
    TestTranslatesScalarGmInstructionsToMemoryAccessCbdata();
    TestRejectsUnsupportedCallbackParamField();
    TestRejectsUnknownInstruction();
}

TEST(AclsanTranslateDeviceData, ScalarAddressClassificationSkipsKnownLocalSpacesButPreservesFailures)
{
    for (const uint32_t instructionId : {26U, 44U}) {
        SCOPED_TRACE(instructionId);
        aclsan::ParsedTraceRecord parsed = MakeParsedTraceRecord(17, 0, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE, 2, 3);
        auto& record = parsed.record;
        record.instrId = instructionId;
        record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        record.args[3] = UINT64_C(0x100000000);
        record.args[4] = aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;

        // Scalar LD/ST instructions also access UB and compiler-private stack windows.
        for (const uint64_t address : {UINT64_C(0x100080040), UINT64_C(0x100100040)}) {
            SCOPED_TRACE(address);
            record.args[0] = address;
            const auto decoded = DecodeRecord(record);
            ASSERT_TRUE(decoded.has_value());
            PlogCapture capture;
            const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
            ASSERT_TRUE(callback.has_value());
            const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
            ASSERT_NE(accesses, nullptr);
            EXPECT_TRUE(accesses->empty());
            EXPECT_NE(
                capture.Text().find("skipped memory access in known UB/PRIVATE address space"), std::string::npos);
            EXPECT_EQ(capture.Text().find("incomplete memory access"), std::string::npos);
        }

        record.args[0] = UINT64_C(0x8000000200001000);
        auto decoded = DecodeRecord(record);
        ASSERT_TRUE(decoded.has_value());
        const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
        ASSERT_TRUE(callback.has_value());
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
        ASSERT_NE(accesses, nullptr);
        ASSERT_EQ(accesses->size(), 1U);
        EXPECT_EQ(accesses->front().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
        EXPECT_EQ(accesses->front().address, UINT64_C(0x200001000));
        EXPECT_EQ(
            accesses->front().accessMode,
            instructionId == 26U ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        EXPECT_EQ(accesses->front().layout.range.bytes, instructionId == 26U ? 2U : 8U);

        // Unknown address encodings and missing mapping context remain conversion errors.
        record.args[0] = UINT64_C(0x100000040);
        decoded = DecodeRecord(record);
        ASSERT_TRUE(decoded.has_value());
        {
            PlogCapture capture;
            EXPECT_FALSE(aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded).has_value());
            EXPECT_NE(capture.Text().find("incomplete memory access status=INVALID_ADDRESS_SPACE"), std::string::npos);
        }
        record.args[0] = UINT64_C(0x8000000200001000);
        record.args[4] = 0;
        decoded = DecodeRecord(record);
        ASSERT_TRUE(decoded.has_value());
        {
            PlogCapture capture;
            EXPECT_FALSE(aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded).has_value());
            EXPECT_NE(
                capture.Text().find("incomplete memory access status=MISSING_ADDRESS_CONTEXT"), std::string::npos);
        }
    }
}
