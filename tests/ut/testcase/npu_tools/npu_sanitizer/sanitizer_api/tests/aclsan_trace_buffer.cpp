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

#include "acl_san/aclsan_cbdata_device.h"
#include "aclsan_trace_buffer.h"
#include "dbi/trace_buffer_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {

#define CHECK(expression)                                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return false;                                                                 \
        }                                                                                 \
    } while (false)

using aclsan::AclsanRawTraceRecord;
using aclsan::AclsanTraceBufferHeader;
using aclsan::AclsanTraceSliceHeader;
using aclsan::DeviceInstructionCategory;

static_assert(aclsan::ASCSAN_TRACE_BUFFER_MAGIC == 0x41534353414E3038ULL);
static_assert(sizeof(AclsanTraceBufferHeader) == 32);
static_assert(std::is_same_v<aclsan::AclsanTraceSegmentHeader, AclsanTraceBufferHeader>);
static_assert(offsetof(AclsanTraceBufferHeader, magic) == 0);
static_assert(offsetof(AclsanTraceBufferHeader, launchId) == 8);
static_assert(offsetof(AclsanTraceBufferHeader, segmentBytes) == 16);
static_assert(offsetof(AclsanTraceBufferHeader, blockCount) == 24);
static_assert(offsetof(AclsanTraceBufferHeader, physicalCoreCount) == 28);
static_assert(sizeof(AclsanTraceSliceHeader) == 16);
static_assert(sizeof(AclsanRawTraceRecord) == 80);
static_assert(aclsan::ASCSAN_TRACE_BYTES_PER_CORE == 1024U * 1024U);
static_assert(aclsan::ASCSAN_TRACE_RECORDS_PER_CORE == 13107U);
static_assert(
    aclsan::ASCSAN_TRACE_BYTES_PER_CORE - sizeof(AclsanTraceSliceHeader) -
        aclsan::ASCSAN_TRACE_RECORDS_PER_CORE * sizeof(AclsanRawTraceRecord) ==
    0U);
static_assert(sizeof(AclsanTraceSliceHeader) % alignof(AclsanRawTraceRecord) == 0);
static_assert(offsetof(AclsanRawTraceRecord, pc) == 0);
static_assert(offsetof(AclsanRawTraceRecord, args) == 8);
static_assert(sizeof(((AclsanRawTraceRecord*)nullptr)->args) == 5 * sizeof(uint64_t));
static_assert(offsetof(AclsanRawTraceRecord, instrId) == 48);
static_assert(std::is_same_v<decltype(AclsanRawTraceRecord{}.instrId), uint32_t>);
static_assert(offsetof(AclsanRawTraceRecord, siteId) == 52);
static_assert(sizeof(DeviceInstructionCategory) == sizeof(uint16_t));
static_assert(offsetof(AclsanRawTraceRecord, category) == 56);
static_assert(offsetof(AclsanRawTraceRecord, pipeline) == 58);
static_assert(offsetof(AclsanRawTraceRecord, blockId) == 60);
static_assert(offsetof(AclsanRawTraceRecord, reserved) == 64);
static_assert(offsetof(AclsanRawTraceRecord, parameterBase) == 72);

AclsanTraceSliceHeader* SliceAt(std::vector<uint8_t>& buffer, uint32_t sliceIndex)
{
    auto* header = reinterpret_cast<AclsanTraceBufferHeader*>(buffer.data());
    const size_t sliceOffset = sizeof(*header) + static_cast<size_t>(sliceIndex) * aclsan::ASCSAN_TRACE_BYTES_PER_CORE;
    return reinterpret_cast<AclsanTraceSliceHeader*>(buffer.data() + sliceOffset);
}

bool PutRecord(
    std::vector<uint8_t>& buffer, uint32_t sliceIndex, uint32_t index, uint32_t blockId, uint64_t pc, uint32_t pipeline,
    uint32_t phyCoreId)
{
    auto* slice = SliceAt(buffer, sliceIndex);
    CHECK(slice != nullptr);
    auto* record = reinterpret_cast<AclsanRawTraceRecord*>(
        reinterpret_cast<uint8_t*>(slice) + sizeof(*slice) + static_cast<size_t>(index) * sizeof(AclsanRawTraceRecord));
    record->pc = pc;
    for (uint32_t arg = 0; arg < 5; ++arg) {
        record->args[arg] = pc + arg + 1;
    }
    record->instrId = pc + 6;
    record->siteId = static_cast<uint32_t>(pc);
    record->category = DeviceInstructionCategory::MemoryAccess;
    record->pipeline = static_cast<uint16_t>(pipeline);
    record->blockId = blockId;
    record->reserved = 0;
    record->parameterBase = 0x10000 + pc;
    slice->phyCoreId = phyCoreId;
    slice->recordCount = index + 1;
    return true;
}

bool InitializesFixedPhysicalCoreSlices()
{
    std::vector<uint8_t> first;
    std::vector<uint8_t> second;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(first, 12, 1, 17, error));
    CHECK(aclsan::InitializeTraceBuffer(second, 12, 4096, 18, error));
    CHECK(first.size() == second.size());

    const auto* header = reinterpret_cast<const AclsanTraceBufferHeader*>(first.data());
    CHECK(header->magic == aclsan::ASCSAN_TRACE_BUFFER_MAGIC);
    CHECK(header->launchId == 17);
    CHECK(header->segmentBytes == first.size());
    CHECK(header->blockCount == 1);
    CHECK(header->physicalCoreCount == 12);
    CHECK(
        first.size() ==
        sizeof(*header) + static_cast<size_t>(header->physicalCoreCount) * aclsan::ASCSAN_TRACE_BYTES_PER_CORE);
    CHECK(
        reinterpret_cast<const uint8_t*>(SliceAt(first, 1)) - reinterpret_cast<const uint8_t*>(SliceAt(first, 0)) ==
        static_cast<ptrdiff_t>(aclsan::ASCSAN_TRACE_BYTES_PER_CORE));
    for (uint32_t sliceIndex = 0; sliceIndex < header->physicalCoreCount; ++sliceIndex) {
        const auto* slice = SliceAt(first, sliceIndex);
        CHECK(slice != nullptr);
        CHECK(slice->recordCount == 0);
        CHECK(slice->overflowCount == 0);
        CHECK(slice->phyCoreId == 0);
        CHECK(slice->reserved == 0);
    }

    std::vector<uint8_t> larger;
    CHECK(aclsan::InitializeTraceBuffer(larger, 18, 1, 19, error));
    CHECK(larger.size() == sizeof(*header) + 18U * aclsan::ASCSAN_TRACE_BYTES_PER_CORE);
    return true;
}

bool ParsesOneSegmentFromLargerStorage()
{
    std::vector<uint8_t> storage;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(storage, 12, 1, 20, error));
    const size_t segmentBytes = storage.size();
    storage.resize(segmentBytes + 64U, 0xA5U);

    const auto parsed = aclsan::ParseTraceBuffer(storage.data(), storage.size(), 12, 1, 20, 0);
    CHECK(parsed.ok);
    CHECK(parsed.records.empty());
    CHECK(reinterpret_cast<const AclsanTraceBufferHeader*>(storage.data())->segmentBytes == segmentBytes);
    return true;
}

bool ParsesMultipleLogicalBlocksInOnePhysicalSlice()
{
    std::vector<uint8_t> buffer;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(buffer, 12, 20, 19, error));
    constexpr uint32_t blockIds[] = {7, 7, 11, 11, 7};
    for (uint32_t index = 0; index < 5; ++index) {
        CHECK(PutRecord(buffer, 2, index, blockIds[index], 0x100 + index, ACLSAN_DEVICE_PIPE_MTE2, 2));
    }

    constexpr uint32_t deviceId = 3;
    const auto parsed = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 12, 20, 19, deviceId);
    CHECK(parsed.ok);
    CHECK(parsed.records.size() == 5);
    for (uint32_t index = 0; index < 5; ++index) {
        CHECK(parsed.records[index].blockId == blockIds[index]);
        CHECK(parsed.records[index].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
        CHECK(parsed.records[index].phyCoreId == 2);
        CHECK(parsed.records[index].instrExecId == 2U * aclsan::ASCSAN_TRACE_RECORDS_PER_CORE + index + 1U);
        CHECK(parsed.records[index].deviceId == deviceId);
        CHECK(parsed.records[index].launchId == 19);
        CHECK(parsed.records[index].record.parameterBase == 0x10100 + index);
        CHECK(parsed.records[index].record.category == DeviceInstructionCategory::MemoryAccess);
        CHECK(parsed.records[index].record.pipeline == ACLSAN_DEVICE_PIPE_MTE2);
    }
    return true;
}

bool ParsesTwoPartPhysicalCoreTopology()
{
    std::vector<uint8_t> buffer;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(buffer, 12, 2, 21, error));
    CHECK(PutRecord(buffer, 1, 0, 1, 0x200, ACLSAN_DEVICE_PIPE_MTE2, 1));
    CHECK(PutRecord(buffer, 2, 0, 1, 0x300, ACLSAN_DEVICE_PIPE_MTE3, 2));
    CHECK(PutRecord(buffer, 7, 0, 0, 0x400, ACLSAN_DEVICE_PIPE_MTE2, 7));
    CHECK(PutRecord(buffer, 8, 0, 2, 0x500, ACLSAN_DEVICE_PIPE_MTE3, 8));

    const auto parsed = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 12, 2, 21, 0);
    CHECK(parsed.ok);
    CHECK(parsed.records.size() == 4);
    CHECK(parsed.records[0].blockId == 1);
    CHECK(parsed.records[0].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
    CHECK(parsed.records[0].phyCoreId == 1);
    CHECK(parsed.records[0].instrExecId == aclsan::ASCSAN_TRACE_RECORDS_PER_CORE + 1U);
    CHECK(parsed.records[1].blockId == 1);
    CHECK(parsed.records[1].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
    CHECK(parsed.records[1].phyCoreId == 2);
    CHECK(parsed.records[1].instrExecId == 2U * aclsan::ASCSAN_TRACE_RECORDS_PER_CORE + 1U);
    CHECK(parsed.records[2].blockId == 0);
    CHECK(parsed.records[2].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
    CHECK(parsed.records[2].phyCoreId == 7);
    CHECK(parsed.records[3].blockId == 2);
    CHECK(parsed.records[3].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
    CHECK(parsed.records[3].phyCoreId == 8);
    return true;
}

bool ClassifiesAllDav3510PhysicalCores()
{
    constexpr uint32_t physicalCoreCount = 108;
    const uint32_t coresPerDie = physicalCoreCount / aclsan::ASCSAN_PHYSICAL_CORE_PART_COUNT;
    for (uint32_t phyCoreId = 0; phyCoreId < physicalCoreCount; ++phyCoreId) {
        const bool expectedAic = phyCoreId % coresPerDie < coresPerDie / aclsan::ASCSAN_AIC_CORE_RATIO_DENOMINATOR;
        CHECK(aclsan::IsAicPhysicalCore(phyCoreId, physicalCoreCount) == expectedAic);
    }
    CHECK(aclsan::IsAicPhysicalCore(0, physicalCoreCount));
    CHECK(aclsan::IsAicPhysicalCore(2, physicalCoreCount));
    CHECK(!aclsan::IsAicPhysicalCore(18, physicalCoreCount));
    CHECK(!aclsan::IsAicPhysicalCore(53, physicalCoreCount));
    CHECK(aclsan::IsAicPhysicalCore(54, physicalCoreCount));
    CHECK(aclsan::IsAicPhysicalCore(56, physicalCoreCount));
    CHECK(!aclsan::IsAicPhysicalCore(72, physicalCoreCount));
    CHECK(!aclsan::IsAicPhysicalCore(physicalCoreCount, physicalCoreCount));
    CHECK(!aclsan::IsAicPhysicalCore(2, 10));
    return true;
}

bool SupportsFullAicBlockCountRange()
{
    constexpr uint32_t maxBlockCount = std::numeric_limits<uint32_t>::max();
    std::vector<uint8_t> buffer;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(buffer, 12, maxBlockCount, 22, error));
    CHECK(PutRecord(buffer, 0, 0, maxBlockCount - 1U, 0x450, ACLSAN_DEVICE_PIPE_MTE2, 0));

    const auto parsed = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 12, maxBlockCount, 22, 0);
    CHECK(parsed.ok);
    CHECK(parsed.records.size() == 1);
    CHECK(parsed.records[0].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
    CHECK(parsed.records[0].blockId == maxBlockCount - 1U);
    return true;
}

bool RejectsUnrepresentableLogicalBlockIds()
{
    constexpr uint32_t blockCount = std::numeric_limits<uint32_t>::max();
    CHECK(aclsan::IsTraceBlockIdValid(blockCount - 1U, true, blockCount));
    CHECK(!aclsan::IsTraceBlockIdValid(blockCount, true, blockCount));
    CHECK(aclsan::IsTraceBlockIdValid(blockCount, false, blockCount));
    CHECK(!aclsan::IsTraceBlockIdValid(static_cast<uint64_t>(blockCount) + 1U, false, blockCount));
    return true;
}

bool ReportsOverflowAndKeepsRecords()
{
    std::vector<uint8_t> buffer;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(buffer, 12, 1, 23, error));
    CHECK(PutRecord(buffer, 0, 0, 0, 0x500, ACLSAN_DEVICE_PIPE_FIXPIPE, 0));
    auto* slice = SliceAt(buffer, 0);
    slice->overflowCount = 9;

    const auto parsed = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 12, 1, 23, 0);
    CHECK(parsed.ok);
    CHECK(parsed.overflowCount == 9);
    CHECK(parsed.records.size() == 1);
    return true;
}

bool DoesNotValidateLogicalBlockPlacement()
{
    std::vector<uint8_t> buffer;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(buffer, 12, 2, 29, error));
    CHECK(PutRecord(buffer, 1, 0, 0, 0x900, ACLSAN_DEVICE_PIPE_MTE2, 1));
    CHECK(PutRecord(buffer, 1, 1, 1, 0x901, ACLSAN_DEVICE_PIPE_MTE2, 1));
    CHECK(PutRecord(buffer, 0, 0, 1, 0xA00, ACLSAN_DEVICE_PIPE_MTE2, 0));

    const auto parsed = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 12, 2, 29, 0);
    CHECK(parsed.ok);
    CHECK(parsed.records.size() == 3);
    CHECK(parsed.records[0].blockId == 1);
    CHECK(parsed.records[0].instrExecId == 1);
    CHECK(parsed.records[1].blockId == 0);
    CHECK(parsed.records[1].instrExecId == aclsan::ASCSAN_TRACE_RECORDS_PER_CORE + 1U);
    CHECK(parsed.records[2].blockId == 1);
    CHECK(parsed.records[2].instrExecId == aclsan::ASCSAN_TRACE_RECORDS_PER_CORE + 2U);
    return true;
}

bool RejectsMalformedBuffers()
{
    std::vector<uint8_t> buffer;
    std::string error;
    CHECK(aclsan::InitializeTraceBuffer(buffer, 12, 2, 29, error));

    auto corrupt = buffer;
    reinterpret_cast<AclsanTraceBufferHeader*>(corrupt.data())->magic ^= 1;
    CHECK(!aclsan::ParseTraceBuffer(corrupt.data(), corrupt.size(), 12, 2, 29, 0).ok);
    CHECK(!aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 12, 2, 30, 0).ok);
    CHECK(!aclsan::ParseTraceBuffer(buffer.data(), buffer.size() - 1, 12, 2, 29, 0).ok);

    corrupt = buffer;
    reinterpret_cast<AclsanTraceBufferHeader*>(corrupt.data())->physicalCoreCount = 18;
    CHECK(!aclsan::ParseTraceBuffer(corrupt.data(), corrupt.size(), 12, 2, 29, 0).ok);

    corrupt = buffer;
    reinterpret_cast<AclsanTraceBufferHeader*>(corrupt.data())->segmentBytes -= 1U;
    CHECK(!aclsan::ParseTraceBuffer(corrupt.data(), corrupt.size(), 12, 2, 29, 0).ok);

    corrupt = buffer;
    reinterpret_cast<AclsanTraceSliceHeader*>(corrupt.data() + sizeof(AclsanTraceBufferHeader))->recordCount =
        aclsan::ASCSAN_TRACE_RECORDS_PER_CORE + 1U;
    CHECK(!aclsan::ParseTraceBuffer(corrupt.data(), corrupt.size(), 12, 2, 29, 0).ok);

    corrupt = buffer;
    CHECK(PutRecord(corrupt, 1, 0, 0, 0x600, ACLSAN_DEVICE_PIPE_MTE2, 2));
    CHECK(!aclsan::ParseTraceBuffer(corrupt.data(), corrupt.size(), 12, 2, 29, 0).ok);

    corrupt = buffer;
    CHECK(PutRecord(corrupt, 1, 0, 4, 0x700, ACLSAN_DEVICE_PIPE_MTE2, 1));
    CHECK(!aclsan::ParseTraceBuffer(corrupt.data(), corrupt.size(), 12, 2, 29, 0).ok);

    corrupt = buffer;
    CHECK(PutRecord(corrupt, 2, 0, 4, 0x800, ACLSAN_DEVICE_PIPE_MTE3, 2));
    CHECK(!aclsan::ParseTraceBuffer(corrupt.data(), corrupt.size(), 12, 2, 29, 0).ok);

    return true;
}

bool RejectsInvalidShape()
{
    std::vector<uint8_t> buffer;
    std::string error;
    CHECK(!aclsan::InitializeTraceBuffer(buffer, 0, 1, 1, error));
    CHECK(!aclsan::InitializeTraceBuffer(buffer, 10, 1, 1, error));
    CHECK(!aclsan::InitializeTraceBuffer(buffer, 12, 0, 1, error));

    size_t bytes = 0;
    CHECK(!aclsan::TraceBufferBytes(0, &bytes));
    CHECK(!aclsan::TraceBufferBytes(10, &bytes));
    CHECK(!aclsan::TraceBufferBytes(12, nullptr));
    return true;
}

} // namespace

TEST(AclsanTraceBuffer, InstructionIdsAreUniqueAcrossCoresAndLogicalBlocks)
{
    std::vector<uint8_t> buffer;
    std::string error;
    ASSERT_TRUE(aclsan::InitializeTraceBuffer(buffer, 6, 2, 31, error));
    ASSERT_TRUE(PutRecord(buffer, 0, 0, 0, 0x100, ACLSAN_DEVICE_PIPE_MTE1, 0));
    ASSERT_TRUE(PutRecord(buffer, 1, 0, 0, 0x200, ACLSAN_DEVICE_PIPE_MTE3, 1));
    ASSERT_TRUE(PutRecord(buffer, 1, 1, 1, 0x200, ACLSAN_DEVICE_PIPE_MTE3, 1));
    ASSERT_TRUE(PutRecord(buffer, 2, 0, 1, 0x200, ACLSAN_DEVICE_PIPE_MTE3, 2));
    const auto parsed = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 6, 2, 31, 0);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.records.size(), 4u);
    for (size_t i = 0; i < parsed.records.size(); ++i) {
        EXPECT_NE(parsed.records[i].instrExecId, 0u);
        for (size_t j = 0; j < i; ++j) {
            EXPECT_NE(parsed.records[i].instrExecId, parsed.records[j].instrExecId);
        }
    }
    EXPECT_LT(parsed.records[1].instrExecId, parsed.records[2].instrExecId);
}

TEST(AclsanTraceBuffer, InstructionIdsStayStableAcrossGrowingSnapshots)
{
    std::vector<uint8_t> buffer;
    std::string error;
    ASSERT_TRUE(aclsan::InitializeTraceBuffer(buffer, 6, 2, 31, error));
    ASSERT_TRUE(PutRecord(buffer, 2, 0, 1, 0x200, ACLSAN_DEVICE_PIPE_MTE3, 2));
    const auto first = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 6, 2, 31, 0);
    ASSERT_TRUE(first.ok) << first.error;
    ASSERT_EQ(first.records.size(), 1u);

    ASSERT_TRUE(PutRecord(buffer, 0, 0, 0, 0x100, ACLSAN_DEVICE_PIPE_MTE1, 0));
    ASSERT_TRUE(PutRecord(buffer, 1, 0, 0, 0x200, ACLSAN_DEVICE_PIPE_MTE3, 1));
    ASSERT_TRUE(PutRecord(buffer, 2, 1, 1, 0x204, ACLSAN_DEVICE_PIPE_MTE3, 2));
    const auto next = aclsan::ParseTraceBuffer(buffer.data(), buffer.size(), 6, 2, 31, 0);
    ASSERT_TRUE(next.ok) << next.error;
    ASSERT_EQ(next.records.size(), 4u);
    EXPECT_EQ(next.records[2].instrExecId, first.records[0].instrExecId);
    EXPECT_LT(next.records[2].instrExecId, next.records[3].instrExecId);
}

TEST(AclsanTraceBuffer, Main)
{
    ASSERT_TRUE(InitializesFixedPhysicalCoreSlices());
    ASSERT_TRUE(ParsesOneSegmentFromLargerStorage());
    ASSERT_TRUE(ParsesMultipleLogicalBlocksInOnePhysicalSlice());
    ASSERT_TRUE(ParsesTwoPartPhysicalCoreTopology());
    ASSERT_TRUE(ClassifiesAllDav3510PhysicalCores());
    ASSERT_TRUE(SupportsFullAicBlockCountRange());
    ASSERT_TRUE(RejectsUnrepresentableLogicalBlockIds());
    ASSERT_TRUE(ReportsOverflowAndKeepsRecords());
    ASSERT_TRUE(DoesNotValidateLogicalBlockPlacement());
    ASSERT_TRUE(RejectsMalformedBuffers());
    ASSERT_TRUE(RejectsInvalidShape());
}
