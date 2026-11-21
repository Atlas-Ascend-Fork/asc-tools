/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "simt_memory_record.h"
#include "aclsan_trace_buffer.h"
#include "aclsan_simt_trace.h"
#include "dbi/ctrlbin/ctrlbin_bindings.h"
#include "securec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace npucheck {
namespace {

TEST(SimtMemoryRecordTest, ResolvesSignedElementOffset)
{
    SimtMemoryAccessRecord record{};
    record.base = 0x1000;
    record.offset = -2;
    record.elementBytes = 16;
    record.accessBytes = 16;

    const auto address = ResolveSimtAddress(record);
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(*address, 0xfe0U);
    const auto range = ResolveSimtAccessRange(record);
    ASSERT_TRUE(range.has_value());
    EXPECT_EQ(range->first, 0xfe0U);
    EXPECT_EQ(range->second, 0xff0U);
}

TEST(SimtMemoryRecordTest, RejectsOffsetAndAddressOverflow)
{
    SimtMemoryAccessRecord record{};
    record.base = 0x1000;
    record.offset = std::numeric_limits<int64_t>::min();
    record.elementBytes = 16;
    record.accessBytes = 16;
    EXPECT_FALSE(ResolveSimtAddress(record).has_value());

    record.base = std::numeric_limits<uint64_t>::max() - 3;
    record.offset = 0;
    record.elementBytes = 1;
    record.accessBytes = 8;
    EXPECT_FALSE(ResolveSimtAccessRange(record).has_value());
}

TEST(SimtMemoryRecordTest, RejectsZeroWidth)
{
    SimtMemoryAccessRecord record{};
    record.base = 0x1000;
    record.elementBytes = 4;
    record.accessBytes = 0;
    EXPECT_FALSE(ResolveSimtAccessRange(record).has_value());
}

} // namespace
} // namespace npucheck

namespace aclsan {
namespace {

template <typename T>
bool ReadTraceValue(const std::vector<uint8_t>& buffer, size_t offset, T& value)
{
    return offset <= buffer.size() && sizeof(value) <= buffer.size() - offset &&
           memcpy_s(&value, sizeof(value), buffer.data() + offset, sizeof(value)) == EOK;
}

template <typename T>
bool WriteTraceValue(std::vector<uint8_t>& buffer, size_t offset, const T& value)
{
    return offset <= buffer.size() && sizeof(value) <= buffer.size() - offset &&
           memcpy_s(buffer.data() + offset, buffer.size() - offset, &value, sizeof(value)) == EOK;
}

} // namespace

TEST(SimtTraceTest, ParsesLaneAccessAndDetectsIncompleteTrace)
{
    std::vector<uint8_t> buffer;
    std::string error;
    ASSERT_TRUE(InitializeTraceBuffer(buffer, ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, error, true));
    AclsanTraceBufferHeader header{};
    ASSERT_TRUE(ReadTraceValue(buffer, 0, header));
    const uint32_t coreId = ASCSAN_AIC_CORES_PER_DIE; // First AIV physical slot.
    const size_t coreOffset =
        header.simtSegmentOffset + sizeof(AclsanSimtTraceHeader) + coreId * ASCSAN_SIMT_CORE_BYTES;
    AclsanSimtCoreHeader core{};
    core.invocationId = 1;
    ASSERT_TRUE(WriteTraceValue(buffer, coreOffset, core));
    const size_t threadOffset = coreOffset + sizeof(core) + ASCSAN_SIMT_THREAD_BYTES;
    AclsanSimtThreadHeader lane0{};
    lane0.lastInvocation = 1;
    lane0.completedInvocation = 1;
    lane0.completedInvocations = 1;
    ASSERT_TRUE(WriteTraceValue(buffer, coreOffset + sizeof(core), lane0));
    AclsanSimtThreadHeader thread{};
    thread.recordCount = 1;
    thread.lastInvocation = 1;
    thread.completedInvocation = 1;
    thread.completedInvocations = 1;
    ASSERT_TRUE(WriteTraceValue(buffer, threadOffset, thread));
    AclsanSimtRawTraceRecord raw{};
    raw.pc = 0x120;
    raw.base = 0x1020;
    raw.offset = -2;
    raw.invocationId = 1;
    raw.instructionId = static_cast<uint32_t>(InstrType::SIMT_LDG_B128);
    raw.blockDimX = 2;
    raw.blockDimY = 1;
    raw.blockDimZ = 1;
    raw.threadX = 1;
    ASSERT_TRUE(WriteTraceValue(buffer, threadOffset + sizeof(thread), raw));
    auto parsed = ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 2, true);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.droppedRecords, 0U);
    auto converted = TranslateSimtTraceRecord(parsed.records[0]);
    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(converted->address, 0x1000U);
    EXPECT_EQ(converted->layout.range.bytes, 16U);
    EXPECT_EQ(converted->simt.threadX, 1U);
    EXPECT_EQ(converted->header.deviceId, 2U);
    thread.overflowCount = 3;
    thread.completedInvocation = 0;
    ASSERT_TRUE(WriteTraceValue(buffer, threadOffset, thread));
    parsed = ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 2, true);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.droppedRecords, 4U);

    thread.overflowCount = 0;
    thread.completedInvocation = 1;
    lane0.lastInvocation = 0;
    lane0.completedInvocation = 0;
    lane0.completedInvocations = 0;
    ASSERT_TRUE(WriteTraceValue(buffer, threadOffset, thread));
    ASSERT_TRUE(WriteTraceValue(buffer, coreOffset + sizeof(core), lane0));
    parsed = ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 2, true);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_GT(parsed.droppedRecords, 0U);

    core.invocationId = 3;
    lane0.lastInvocation = lane0.completedInvocation = 3;
    lane0.completedInvocations = 0x5U;
    thread.lastInvocation = thread.completedInvocation = 3;
    thread.completedInvocations = 0x5U;
    raw.invocationId = 3;
    ASSERT_TRUE(WriteTraceValue(buffer, coreOffset, core));
    ASSERT_TRUE(WriteTraceValue(buffer, coreOffset + sizeof(core), lane0));
    ASSERT_TRUE(WriteTraceValue(buffer, threadOffset, thread));
    ASSERT_TRUE(WriteTraceValue(buffer, threadOffset + sizeof(thread), raw));
    parsed = ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 2, true);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_GT(parsed.droppedRecords, 0U);
}

TEST(SimtTraceTest, RetainsPriorRecordsButRequiresCurrentInvocationCoverage)
{
    std::vector<uint8_t> buffer;
    std::string error;
    ASSERT_TRUE(InitializeTraceBuffer(buffer, ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, error, true));
    AclsanTraceBufferHeader header{};
    ASSERT_TRUE(ReadTraceValue(buffer, 0, header));
    const uint32_t coreId = ASCSAN_AIC_CORES_PER_DIE;
    const size_t coreOffset =
        header.simtSegmentOffset + sizeof(AclsanSimtTraceHeader) + coreId * ASCSAN_SIMT_CORE_BYTES;
    AclsanSimtCoreHeader core{};
    core.invocationId = 2;
    ASSERT_TRUE(WriteTraceValue(buffer, coreOffset, core));

    const size_t laneOffset = coreOffset + sizeof(AclsanSimtCoreHeader);
    AclsanSimtThreadHeader lane{};
    lane.recordCount = 1;
    lane.lastInvocation = 1;
    lane.completedInvocation = 1;
    lane.completedInvocations = 1;
    ASSERT_TRUE(WriteTraceValue(buffer, laneOffset, lane));

    AclsanSimtRawTraceRecord record{};
    record.base = 0x1000;
    record.invocationId = 1;
    record.instructionId = static_cast<uint32_t>(InstrType::SIMT_STG_B32);
    record.blockId = 0;
    record.threadX = 0;
    record.blockDimX = record.blockDimY = record.blockDimZ = 1;
    ASSERT_TRUE(WriteTraceValue(buffer, laneOffset + sizeof(lane), record));

    const auto parsed =
        ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 2, true);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_GT(parsed.droppedRecords, 0U);

    // A completion from invocation 1 cannot cover a missing lane in invocation 2.
    lane.lastInvocation = lane.completedInvocation = 2;
    lane.completedInvocations = 3;
    record.invocationId = 2;
    record.threadX = 1;
    record.blockDimX = 2;
    const size_t nextLaneOffset = laneOffset + ASCSAN_SIMT_THREAD_BYTES;
    ASSERT_TRUE(WriteTraceValue(buffer, nextLaneOffset, lane));
    ASSERT_TRUE(WriteTraceValue(buffer, nextLaneOffset + sizeof(lane), record));
    const auto missingLane =
        ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 2, true);
    ASSERT_TRUE(missingLane.ok) << missingLane.error;
    ASSERT_EQ(missingLane.records.size(), 2U);
    EXPECT_GT(missingLane.droppedRecords, 0U);
}

TEST(SimtTraceTest, TranslatesGmAccessWidthsAndRejectsUnknownInstructions)
{
    const struct {
        InstrType instruction;
        uint32_t width;
        uint32_t mode;
    } cases[] = {
        {InstrType::SIMT_LDG_U8, 1, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LDG_S8, 1, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LDG_U16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LDG_S16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LDG_B32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LDG_B64, 8, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LDG_B128, 16, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_STG_B8, 1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::SIMT_STG_B16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::SIMT_STG_B32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::SIMT_STG_B64, 8, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::SIMT_STG_B128, 16, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::SIMT_LD_U8, 1, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LD_S8, 1, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LD_U16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LD_S16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_LD_B32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_READ},
        {InstrType::SIMT_ST_B8, 1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::SIMT_ST_B16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::SIMT_ST_B32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::RED_G_BF16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::RED_G_U32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::RED_G_FP16, 2, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
        {InstrType::ATOM_ADD_G_U64, 8, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD},
        {InstrType::ATOM_MIN_G_U32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN},
        {InstrType::ATOM_MAX_G_S32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX},
        {InstrType::ATOM_CAS_G_U32, 4, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE},
    };
    for (const auto& test : cases) {
        ParsedSimtTraceRecord parsed{};
        parsed.record.base = 0x1000;
        parsed.record.offset = -1;
        parsed.record.instructionId = static_cast<uint32_t>(test.instruction);
        const auto access = TranslateSimtTraceRecord(parsed);
        ASSERT_TRUE(access.has_value()) << static_cast<uint32_t>(test.instruction);
        EXPECT_EQ(access->address, 0x1000U - test.width);
        EXPECT_EQ(access->layout.range.bytes, test.width);
        EXPECT_EQ(access->accessMode, test.mode);
    }
    ParsedSimtTraceRecord unknown{};
    EXPECT_FALSE(TranslateSimtTraceRecord(unknown).has_value());
}

TEST(SimtTraceTest, RejectsMissingOrTruncatedSidecar)
{
    std::vector<uint8_t> buffer;
    std::string error;
    ASSERT_TRUE(InitializeTraceBuffer(buffer, ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, error, true));
    AclsanTraceBufferHeader header{};
    ASSERT_TRUE(ReadTraceValue(buffer, 0, header));
    const auto simtOffset = header.simtSegmentOffset;
    header.simtSegmentOffset = 0;
    ASSERT_TRUE(WriteTraceValue(buffer, 0, header));
    EXPECT_FALSE(ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 0, true).ok);
    header.simtSegmentOffset = simtOffset;
    ASSERT_TRUE(WriteTraceValue(buffer, 0, header));
    buffer.pop_back();
    EXPECT_FALSE(ParseSimtTraceBuffer(buffer.data(), buffer.size(), ASCSAN_PHYSICAL_CORE_SLOT_COUNT, 1, 9, 0, true).ok);
}
} // namespace aclsan
