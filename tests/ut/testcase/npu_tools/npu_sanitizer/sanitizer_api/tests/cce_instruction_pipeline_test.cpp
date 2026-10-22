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

#include "device_instr/common/instruction_id.h"
#include "device_instr/decoder_registry.h"
#include "aclsan_device_data.h"
#include "aclsan_memory_cbdata.h"

#include <cassert>
#include <cstdint>
#include <limits>

namespace {

void TestCallbackUsesRawRecordPipeline()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = static_cast<uint32_t>(aclsan::InstructionId::SetFlag);
    record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    record.args[0] = ACLSAN_DEVICE_PIPE_SCALAR;
    record.args[1] = ACLSAN_DEVICE_PIPE_MTE2;
    record.args[2] = 7;

    aclsan::ParsedTraceRecord parsed{};
    parsed.record = record;
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    const auto decoded = decoder->decode(record);
    assert(decoded.has_value());
    const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
    assert(callback.has_value());
    const auto* sync = std::get_if<AclsanDeviceSyncData>(&*callback);
    assert(sync != nullptr);
    assert(sync->header.pipeline == record.pipeline);
}

void TestMemoryCallbackUsesRawRecordPipeline()
{
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = static_cast<uint32_t>(aclsan::InstructionId::CopyGmToCbufV2);
    record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    record.args[0] = 0x2000;
    record.args[1] = 0x1000;
    record.args[2] = (UINT64_C(1) << 4U) | (UINT64_C(1) << 25U);

    aclsan::ParsedTraceRecord parsed{};
    parsed.record = record;
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    const auto decoded = decoder->decode(record);
    assert(decoded.has_value());
    const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
    assert(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    assert(accesses != nullptr);
    assert(!accesses->empty());
    for (const AclsanDeviceMemoryAccessData& access : *accesses) {
        assert(access.header.pipeline == record.pipeline);
    }
}

void TestLocalOnlyInstructionReturnsEmptyGmAccessList()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = static_cast<uint32_t>(aclsan::InstructionId::CopyUbufToCbuf);
    const auto decoded = decoder->decode(record);
    assert(decoded.has_value());
    assert(decoded->kind == aclsan::DeviceInstructionKind::LocalMemoryTransfer);
    aclsan::ParsedTraceRecord parsed{};
    parsed.record = record;
    const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
    assert(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    assert(accesses != nullptr);
    assert(accesses->empty());
}

void TestUnknownInstructionReturnsNoDecodedResult()
{
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    aclsan::AclsanRawTraceRecord record{};
    record.instrId = UINT32_MAX;
    assert(!decoder->decode(record).has_value());
}

} // namespace

TEST(CceInstructionPipeline, UbCopyProducesReadAndWriteWithSameExecutionIdentity)
{
    aclsan::ParsedTraceRecord parsed{};
    parsed.record.instrId = 174;
    parsed.record.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    parsed.record.args[0] = 0x100;
    parsed.record.args[1] = 0x400;
    parsed.record.args[2] = 2ULL | (1ULL << 16);
    parsed.instrExecId = 11;
    parsed.launchId = 9;
    parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    parsed.phyCoreId = 4;
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    const auto decoded = decoder->decode(parsed.record);
    ASSERT_TRUE(decoded);
    const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
    ASSERT_TRUE(callback);
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    ASSERT_NE(accesses, nullptr);
    ASSERT_EQ(accesses->size(), 2u);
    for (size_t i = 0; i < accesses->size(); ++i) {
        const auto& a = (*accesses)[i];
        EXPECT_EQ(a.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        EXPECT_EQ(a.accessMode, i == 0 ? ACLSAN_DEVICE_MEMORY_ACCESS_READ : ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
        EXPECT_EQ(a.header.instrExecId, 11u);
        EXPECT_EQ(a.header.phyCoreId, 4u);
        EXPECT_EQ(a.header.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
        EXPECT_EQ(a.header.pipeline, ACLSAN_DEVICE_PIPE_VECTOR);
        EXPECT_EQ(a.header.sourceKind, ACLSAN_DEVICE_SOURCE_VECTOR);
        EXPECT_EQ(a.instructionId, 174u);
        ASSERT_EQ(a.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
        EXPECT_EQ(a.layout.range.bytes, 64u);
        EXPECT_EQ(a.accessIndex, i);
        EXPECT_EQ(a.accessCount, 2u);
    }
}

TEST(CceInstructionPipeline, Main)
{
    TestCallbackUsesRawRecordPipeline();
    TestMemoryCallbackUsesRawRecordPipeline();
    TestLocalOnlyInstructionReturnsEmptyGmAccessList();
    TestUnknownInstructionReturnsNoDecodedResult();
}

TEST(CceInstructionPipeline, ScalarDevWidthsAndSignedByteOffsets)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    for (uint32_t id = 64; id <= 71; ++id) {
        for (int64_t offset : {int64_t{0}, int64_t{3}, int64_t{-3}}) {
            SCOPED_TRACE(::testing::Message() << "id=" << id << " offset=" << offset);
            aclsan::ParsedTraceRecord parsed{};
            parsed.record.instrId = id;
            parsed.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
            parsed.record.pc = 0x1234;
            parsed.record.siteId = 17;
            parsed.record.args[0] = 0x1000;
            parsed.record.args[1] = static_cast<uint64_t>(offset);
            parsed.record.args[2] = 1; // No post-index flag for ST_DEV/LD_DEV.
            parsed.launchId = 7;
            parsed.deviceId = 2;
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded.has_value());
            const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
            ASSERT_TRUE(callback.has_value());
            const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
            ASSERT_NE(accesses, nullptr);
            ASSERT_EQ(accesses->size(), 1U);
            const auto& access = accesses->front();
            EXPECT_EQ(access.address, static_cast<uint64_t>(0x1000 + offset));
            EXPECT_EQ(access.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
            EXPECT_EQ(
                access.accessMode, id < 68 ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ);
            EXPECT_EQ(access.dataBits, 64U >> ((id - 64) % 4));
            EXPECT_EQ(access.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
            EXPECT_EQ(access.layout.range.bytes, 8U >> ((id - 64) % 4));
            EXPECT_EQ(access.header.sourceKind, id < 68 ? ACLSAN_DEVICE_SOURCE_ST : ACLSAN_DEVICE_SOURCE_LD);
            EXPECT_EQ(access.header.pipeline, ACLSAN_DEVICE_PIPE_SCALAR);
            EXPECT_EQ(access.header.pc, parsed.record.pc);
            EXPECT_EQ(access.header.siteId, parsed.record.siteId);
            EXPECT_EQ(access.header.launchId, parsed.launchId);
            EXPECT_EQ(access.header.deviceId, parsed.deviceId);
            EXPECT_EQ(access.accessIndex, 0U);
            EXPECT_EQ(access.accessCount, 1U);
        }
    }
}

TEST(CceInstructionPipeline, ScalarDevRejectsAddressOverflow)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    struct Case {
        uint64_t address;
        int64_t offset;
    };
    const Case cases[] = {
        {UINT64_MAX, 1},
        {0, -1},
        {0, INT64_MIN},
        {UINT64_MAX - 3, 0},
    };
    for (uint32_t id : {64U, 68U}) {
        for (const auto& test : cases) {
            aclsan::ParsedTraceRecord parsed{};
            parsed.record.instrId = id;
            parsed.record.args[0] = test.address;
            parsed.record.args[1] = static_cast<uint64_t>(test.offset);
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded.has_value());
            EXPECT_FALSE(aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded).has_value());
        }
    }
}

TEST(CceInstructionPipeline, ScalarDevExplicitGmAndMinimumOffset)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    for (uint32_t id = 64; id <= 71; ++id) {
        aclsan::ParsedTraceRecord parsed{};
        parsed.record.instrId = id;
        parsed.record.args[0] = (UINT64_C(1) << 63) + 0x10;
        parsed.record.args[1] = static_cast<uint64_t>(INT64_MIN);
        const auto decoded = decoder->decode(parsed.record);
        ASSERT_TRUE(decoded.has_value());
        const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
        ASSERT_TRUE(callback.has_value());
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
        ASSERT_NE(accesses, nullptr);
        ASSERT_EQ(accesses->size(), 1U);
        EXPECT_EQ(accesses->front().address, 0x10U);
        EXPECT_EQ(accesses->front().memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
    }
}

TEST(CceInstructionPipeline, ScalarAtomicWithoutConfigurationPublishesOrdinaryWriteFallback)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
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
            aclsan::ParsedTraceRecord parsed{};
            parsed.record.instrId = test.id;
            parsed.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
            parsed.record.args[0] = 0x1000;
            parsed.record.args[1] = static_cast<uint64_t>(int64_t{3});
            parsed.record.args[2] = post;
            parsed.record.args[3] = UINT64_C(0x100000000);
            parsed.record.args[4] = aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded.has_value());
            const auto* field = std::get_if<aclsan::ScalarAtomicParamField>(&decoded->params);
            ASSERT_NE(field, nullptr);
            EXPECT_EQ(field->dataBits, test.bits);
            EXPECT_EQ(field->offset, 3);
            EXPECT_EQ(field->post, post);
            const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
            ASSERT_TRUE(callback.has_value());
            const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
            ASSERT_NE(accesses, nullptr);
            ASSERT_EQ(accesses->size(), 1U);
            const auto& access = accesses->front();
            EXPECT_EQ(access.address, 0x1000U + (post == 1 ? 0U : 3U * (test.id <= 58 ? test.bits / 8U : 1U)));
            EXPECT_EQ(access.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
            EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
            EXPECT_EQ(access.header.version, ACLSAN_API_VERSION);
            EXPECT_EQ(
                access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT | ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN);
            EXPECT_EQ(access.instructionId, test.id);
            EXPECT_EQ(access.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
            EXPECT_EQ(access.layout.range.bytes, test.bits / 8U);
            EXPECT_EQ(access.header.pipeline, ACLSAN_DEVICE_PIPE_SCALAR);
        }
    }
}

TEST(CceInstructionPipeline, ScalarAtomicRejectsUnsupportedPostAndOverflow)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    struct Case {
        uint64_t address;
        int64_t offset;
        uint64_t post;
    };
    const Case rejected[] = {
        {0x1000, 1, 2}, {0x1000, 1, UINT64_MAX}, {UINT64_MAX, 1, 0}, {0, -1, 0}, {0, INT64_MIN, 0},
    };
    for (uint32_t id = 56; id <= 61; ++id) {
        for (const auto& test : rejected) {
            SCOPED_TRACE(::testing::Message() << "id=" << id << " post=" << test.post);
            aclsan::ParsedTraceRecord parsed{};
            parsed.record.instrId = id;
            parsed.record.args[0] = test.address;
            parsed.record.args[1] = static_cast<uint64_t>(test.offset);
            parsed.record.args[2] = test.post;
            parsed.record.args[3] = UINT64_C(0x100000000);
            parsed.record.args[4] = aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded.has_value());
            EXPECT_FALSE(aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded).has_value());
        }
    }
}

TEST(CceInstructionPipeline, UnsupportedAtomicDestinationPreservesSourceAndSignalsIncompleteCollection)
{
    aclsan::FixL0cToOutParamField field{};
    field.instrId = 91;
    field.dstAddr = 0x1000;
    field.nSize = 16;
    field.mSize = 16;
    field.loopDstStride = 256;
    field.loopSrtStride = 16;
    const aclsan::DecodedInstruction decoded{aclsan::DeviceInstructionKind::FixL0cToOut, field};
    aclsan::ParsedTraceRecord parsed{};
    parsed.record.instrId = field.instrId;
    parsed.record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    parsed.record.pc = 0x200;
    for (uint64_t ctrl : {UINT64_C(0x640), UINT64_C(0x1c0), UINT64_C(0x140)}) {
        SCOPED_TRACE(ctrl);
        aclsan::MemoryRegisterState state{};
        state.ctrl = aclsan::CtrlRegisterParamField{ctrl};
        const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, decoded, state);
        ASSERT_TRUE(callback.has_value());
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
        ASSERT_NE(accesses, nullptr);
        ASSERT_EQ(accesses->size(), 2U);
        const auto& source = accesses->front();
        EXPECT_EQ(source.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L0C);
        EXPECT_EQ(source.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        EXPECT_EQ(source.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
        const auto& incomplete = accesses->back();
        EXPECT_EQ(incomplete.accessMode, 0U);
        EXPECT_EQ(incomplete.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN);
        EXPECT_EQ(incomplete.header.flags, ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED);
        EXPECT_EQ(incomplete.header.version, ACLSAN_API_VERSION);
        EXPECT_EQ(incomplete.header.pc, parsed.record.pc);
        EXPECT_EQ(incomplete.instructionId, field.instrId);
        EXPECT_EQ(incomplete.layoutKind, 0U);
        for (size_t i = 0; i < accesses->size(); ++i) {
            EXPECT_EQ((*accesses)[i].accessIndex, i);
            EXPECT_EQ((*accesses)[i].accessCount, accesses->size());
        }
    }
}
