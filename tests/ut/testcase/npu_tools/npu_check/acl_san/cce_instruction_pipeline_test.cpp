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

void ExpectOnlyIncompleteScalarAccess(
    const aclsan::ParsedTraceRecord& parsed, const aclsan::DecodedInstruction& decoded)
{
    const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, decoded);
    ASSERT_TRUE(callback.has_value());
    const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
    ASSERT_NE(accesses, nullptr);
    ASSERT_EQ(accesses->size(), 1U);
    const auto& marker = accesses->front();
    EXPECT_EQ(marker.header.flags, ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED);
    EXPECT_EQ(marker.header.pc, parsed.record.pc);
    EXPECT_EQ(marker.header.launchId, parsed.launchId);
    EXPECT_EQ(marker.header.instrExecId, parsed.instrExecId);
    EXPECT_EQ(marker.header.pipeline, ACLSAN_DEVICE_PIPE_SCALAR);
    EXPECT_EQ(marker.instructionId, parsed.record.instrId);
    EXPECT_EQ(marker.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN);
    EXPECT_EQ(marker.accessMode, 0U);
    EXPECT_EQ(marker.layoutKind, 0U);
    EXPECT_EQ(marker.accessIndex, 0U);
    EXPECT_EQ(marker.accessCount, 1U);
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
        EXPECT_EQ(a.header.executor.phyCoreId, 4u);
        EXPECT_EQ(a.header.executor.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
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

TEST(CceInstructionPipeline, ScalarDevAddressOverflowPublishesOnlyIncompleteMarker)
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
            parsed.launchId = 9;
            parsed.instrExecId = 11;
            parsed.record.pc = 0x200;
            parsed.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
            parsed.record.instrId = id;
            parsed.record.args[0] = test.address;
            parsed.record.args[1] = static_cast<uint64_t>(test.offset);
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded.has_value());
            ExpectOnlyIncompleteScalarAccess(parsed, *decoded);
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

TEST(CceInstructionPipeline, ScalarAtomicZeroConfigurationPublishesAtomicAdd)
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
            for (size_t i = 0; i < accesses->size(); ++i) {
                const auto& access = accesses->at(i);
                EXPECT_EQ(access.address, 0x1000U + (post == 1 ? 0U : 3U * (test.id <= 58 ? test.bits / 8U : 1U)));
                EXPECT_EQ(access.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
                EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
                EXPECT_EQ(access.accessIndex, i);
                EXPECT_EQ(access.accessCount, 1U);
                EXPECT_EQ(access.header.version, ACLSAN_API_VERSION);
                EXPECT_EQ(access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
                EXPECT_EQ(access.instructionId, test.id);
                EXPECT_EQ(access.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
                EXPECT_EQ(access.layout.range.bytes, test.bits / 8U);
                EXPECT_EQ(access.header.pipeline, ACLSAN_DEVICE_PIPE_SCALAR);
            }
        }
    }
}

TEST(CceInstructionPipeline, ScalarAtomAndRedPreserveRegisterOrderAndOperationType)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    for (const uint32_t id : {427U, 431U}) {
        aclsan::ParsedTraceRecord parsed{};
        parsed.record.instrId = id;
        parsed.record.args[0] = id == 427 ? 0x11 : 0x2001000;
        parsed.record.args[1] = id == 427 ? 0x2001000 : 0x22;
        parsed.record.args[2] = id == 427 ? 0x22 : 1;
        parsed.record.args[3] = id == 427 ? 0x33 : 0;
        parsed.record.args[4] = id == 427 ? 1 : 4;
        parsed.record.atomicMetadata = 0x80;
        const auto decoded = decoder->decode(parsed.record);
        ASSERT_TRUE(decoded);
        if (id == 427) {
            const auto* field = std::get_if<aclsan::ScalarAtomParamField>(&decoded->params);
            ASSERT_NE(field, nullptr);
            EXPECT_EQ(field->dstValue, 0x11U);
            EXPECT_EQ(field->srcAddr, 0x2001000U);
            EXPECT_EQ(field->srcOperand, 0x22U);
            EXPECT_EQ(field->srcOperandCas, 0x33U);
            EXPECT_EQ(field->atomicType, 1U);
            EXPECT_EQ(field->sysVaBase, UINT64_C(0x100000000));
        } else {
            const auto* field = std::get_if<aclsan::ScalarRedParamField>(&decoded->params);
            ASSERT_NE(field, nullptr);
            EXPECT_EQ(field->srcAddr, 0x2001000U);
            EXPECT_EQ(field->srcOperand, 0x22U);
            EXPECT_EQ(field->atomicType, 1U);
            EXPECT_EQ(field->sysVaBase, UINT64_C(0x100000000));
        }
    }
}

TEST(CceInstructionPipeline, ScalarAtomAndRedUseCapturedAddressWindow)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    const aclsan::MemoryFieldToCbdataConverter converter{{}};
    struct Case {
        uint32_t metadata;
        aclsan::MemoryCbdataStatus status;
    };
    for (uint32_t id : {427U, 431U}) {
        for (const Case test :
             {Case{0x80, aclsan::MemoryCbdataStatus::UNSUPPORTED_ADDRESS_SPACE},
              {0x100, aclsan::MemoryCbdataStatus::SUCCESS},
              {0, aclsan::MemoryCbdataStatus::SUCCESS}}) {
            SCOPED_TRACE(::testing::Message() << "id=" << id << " metadata=" << test.metadata);
            aclsan::AclsanRawTraceRecord record{};
            record.instrId = id;
            record.args[id == 427 ? 1 : 0] = UINT64_C(0x100080040);
            record.args[4] = 0;
            record.atomicMetadata = test.metadata;
            const auto decoded = decoder->decode(record);
            ASSERT_TRUE(decoded);
            const auto result = id == 427 ? converter.Convert(std::get<aclsan::ScalarAtomParamField>(decoded->params)) :
                                            converter.Convert(std::get<aclsan::ScalarRedParamField>(decoded->params));
            EXPECT_EQ(result.status, test.status);
            if (test.status == aclsan::MemoryCbdataStatus::SUCCESS) {
                ASSERT_EQ(result.data.size(), 1U);
                EXPECT_EQ(result.data[0].address, UINT64_C(0x100080040));
                EXPECT_EQ(result.data[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
            }
        }
    }
}

TEST(CceInstructionPipeline, ScalarAtomAndRedProduceReadAndWriteForAllTypes)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    const uint32_t widths[] = {32, 32, 64, 64, 32, 32, 16, 16, 32};
    for (uint32_t id = 427; id <= 435; ++id) {
        SCOPED_TRACE(id);
        aclsan::ParsedTraceRecord parsed{};
        parsed.launchId = 45;
        parsed.instrExecId = id;
        parsed.record.instrId = id;
        parsed.record.pc = 0x400 + id * 4;
        parsed.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        parsed.record.args[0] = id <= 430 ? 0x42 : 0x2001000;
        parsed.record.args[1] = id <= 430 ? 0x2001000 : 0x1234;
        parsed.record.args[2] = id <= 430 ? 0x1234 : UINT64_MAX;
        parsed.record.args[3] = id <= 430 ? 0x5678 : 0;
        parsed.record.args[4] = id <= 430 ? UINT64_MAX : 0;
        parsed.record.atomicMetadata = 0x80;
        const auto decoded = decoder->decode(parsed.record);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_TRUE(aclsan::IsDefinedInstructionId(id));
        if (id <= 430) {
            EXPECT_EQ(decoded->kind, aclsan::DeviceInstructionKind::ScalarAtom);
            const auto* field = std::get_if<aclsan::ScalarAtomParamField>(&decoded->params);
            ASSERT_NE(field, nullptr);
            EXPECT_EQ(field->dstValue, parsed.record.args[0]);
            EXPECT_EQ(field->srcAddr, parsed.record.args[1]);
            EXPECT_EQ(field->srcOperand, parsed.record.args[2]);
            EXPECT_EQ(field->srcOperandCas, parsed.record.args[3]);
            EXPECT_EQ(field->sysVaBase, UINT64_C(0x100000000));
        } else {
            EXPECT_EQ(decoded->kind, aclsan::DeviceInstructionKind::ScalarRed);
            const auto* field = std::get_if<aclsan::ScalarRedParamField>(&decoded->params);
            ASSERT_NE(field, nullptr);
            EXPECT_EQ(field->srcAddr, parsed.record.args[0]);
            EXPECT_EQ(field->srcOperand, parsed.record.args[1]);
            EXPECT_EQ(field->sysVaBase, UINT64_C(0x100000000));
        }
        const auto callback = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
        ASSERT_TRUE(callback.has_value());
        const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*callback);
        ASSERT_NE(accesses, nullptr);
        ASSERT_EQ(accesses->size(), 2U);
        for (size_t i = 0; i < accesses->size(); ++i) {
            const auto& access = accesses->at(i);
            EXPECT_EQ(access.instructionId, id);
            EXPECT_EQ(access.address, 0x2001000U);
            EXPECT_EQ(access.dataBits, widths[id - 427]);
            EXPECT_EQ(access.layoutKind, ACLSAN_MEM_LAYOUT_RANGE);
            EXPECT_EQ(access.layout.range.bytes, widths[id - 427] / 8);
            EXPECT_EQ(access.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_GM);
            EXPECT_EQ(access.accessIndex, i);
            EXPECT_EQ(access.accessCount, 2U);
            EXPECT_EQ(access.accessMode, i == 0 ? ACLSAN_DEVICE_MEMORY_ACCESS_READ : ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
            EXPECT_EQ(
                access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT | ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN);
            EXPECT_EQ(access.header.launchId, parsed.launchId);
            EXPECT_EQ(access.header.instrExecId, parsed.instrExecId);
            EXPECT_EQ(access.header.pc, parsed.record.pc);
        }
    }
}

TEST(CceInstructionPipeline, ScalarAtomicInvalidPostAndOverflowPublishOnlyIncompleteMarker)
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
            parsed.launchId = 9;
            parsed.instrExecId = 11;
            parsed.record.pc = 0x200;
            parsed.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
            parsed.record.instrId = id;
            parsed.record.args[0] = test.address;
            parsed.record.args[1] = static_cast<uint64_t>(test.offset);
            parsed.record.args[2] = test.post;
            parsed.record.args[3] = UINT64_C(0x100000000);
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded.has_value());
            ExpectOnlyIncompleteScalarAccess(parsed, *decoded);
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

TEST(CceInstructionPipeline, ScalarAtomRedUsesBishengTypeAsOneAtomicAccess)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    struct Operation {
        uint32_t type;
        uint32_t mode;
    };
    for (uint32_t id = 427; id <= 435; ++id) {
        for (const Operation op :
             {Operation{0, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_CAS},
              {1, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_EXCH},
              {2, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD},
              {3, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN},
              {4, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX}}) {
            SCOPED_TRACE(::testing::Message() << "id=" << id << " mode=" << op.mode);
            aclsan::ParsedTraceRecord parsed{};
            parsed.record.instrId = id;
            parsed.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
            parsed.record.args[0] = id <= 430 ? 0x42 : 0x1000;
            parsed.record.args[1] = id <= 430 ? 0x1000 : 17;
            parsed.record.args[2] = id <= 430 ? 17 : op.type;
            parsed.record.args[3] = id <= 430 && op.mode == ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_CAS ? 19 : 0;
            parsed.record.args[4] = id <= 430 ? op.type : 0;
            parsed.record.atomicMetadata = 0x80;
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded);
            const auto translated = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
            ASSERT_TRUE(translated);
            const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*translated);
            ASSERT_NE(accesses, nullptr);
            ASSERT_EQ(accesses->size(), 1U);
            const auto& access = accesses->front();
            EXPECT_EQ(access.accessMode, op.mode);
            EXPECT_EQ(access.address, 0x1000U);
            EXPECT_EQ(access.header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
            EXPECT_EQ(access.accessIndex, 0U);
            EXPECT_EQ(access.accessCount, 1U);
        }
    }
}

TEST(CceInstructionPipeline, ScalarAtomRedUnsupportedOperationKeepsCoverageUnknown)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    for (uint32_t id = 427; id <= 435; ++id) {
        for (uint64_t type : {UINT64_C(5), UINT64_MAX}) {
            SCOPED_TRACE(::testing::Message() << "id=" << id << " type=" << type);
            aclsan::ParsedTraceRecord parsed{};
            parsed.record.instrId = id;
            parsed.record.args[0] = id <= 430 ? 0x42 : 0x1000;
            parsed.record.args[1] = id <= 430 ? 0x1000 : 0;
            parsed.record.args[id <= 430 ? 4 : 2] = type;
            parsed.record.atomicMetadata = 0x80;
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded);
            const auto translated = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
            ASSERT_TRUE(translated);
            const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*translated);
            ASSERT_NE(accesses, nullptr);
            ASSERT_EQ(accesses->size(), 2U);
            for (const auto& access : *accesses)
                EXPECT_NE(access.header.flags & ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN, 0U);
        }
    }
}

TEST(CceInstructionPipeline, ScalarAtomicPreservesCapturedConfigurationAndAddress)
{
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    for (uint32_t id = 56; id <= 61; ++id) {
        for (uint32_t cfg : {0U, 1U, 2U, 4U, 0x24U, 0x70U}) {
            aclsan::ParsedTraceRecord parsed{};
            parsed.record.instrId = id;
            parsed.record.args[0] = 0x1000;
            parsed.record.args[1] = static_cast<uint64_t>(int64_t{-3});
            parsed.record.args[3] = UINT64_C(0x100000000);
            parsed.record.atomicMetadata = cfg;
            const auto decoded = decoder->decode(parsed.record);
            ASSERT_TRUE(decoded);
            const auto* field = std::get_if<aclsan::ScalarAtomicParamField>(&decoded->params);
            ASSERT_NE(field, nullptr);
            EXPECT_EQ(field->atomicConfig, cfg);
            const auto translated = aclsan::TranslateDecodedTraceToCallbackData(parsed, *decoded);
            ASSERT_TRUE(translated);
            const auto* accesses = std::get_if<aclsan::DeviceMemoryAccessDataList>(&*translated);
            ASSERT_NE(accesses, nullptr);
            ASSERT_FALSE(accesses->empty());
            if ((cfg & 0xfU) == 0 || cfg == 1 || cfg == 4 || cfg == 0x24) {
                ASSERT_EQ(accesses->size(), 1U);
                EXPECT_EQ(accesses->front().accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
                EXPECT_EQ(accesses->front().header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
            }
            const uint32_t bytes = 4U >> ((id - 56) % 3);
            for (const auto& access : *accesses) {
                EXPECT_EQ(access.address, 0x1000U - 3U * (id <= 58 ? bytes : 1U));
                EXPECT_EQ(access.layout.range.bytes, bytes);
            }
        }
    }
}
