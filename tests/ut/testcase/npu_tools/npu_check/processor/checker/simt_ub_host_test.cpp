/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "aclsan_ub_layout_builder.h"
#include "aclsan_simt_ub_trace.h"
#include "dbi/ctrlbin/ctrlbin_bindings.h"
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace aclsan;
AclsanUbLaunchMetadata Metadata(uint64_t dynamic = 0x10001)
{
    return {0x5000, 0x3000, dynamic, 3, ACLSAN_UB_METADATA_COMPILER | ACLSAN_UB_METADATA_STATIC};
}
TEST(SimtUbHostTest, LayoutCalculatesCacheAndKeepsAlignmentPadding)
{
    AclsanUbLayout layout{};
    ASSERT_TRUE(BuildSimtUbLayout(SocVersion::DAV_3510, Metadata(), layout));
    EXPECT_EQ(layout.valid, 1U);
    EXPECT_EQ(layout.regions[0].end, 0x3000U);
    EXPECT_EQ(layout.regions[1].begin, 0x3000U);
    EXPECT_EQ(layout.regions[1].end, 0x13080U);
    EXPECT_EQ(layout.regions[2].begin, 0x13080U);
    EXPECT_EQ(layout.regions[2].end, 0x15080U);
    EXPECT_EQ(layout.regions[3].begin, 0x20000U);
    EXPECT_EQ(layout.regions[3].end, 0x40000U);
    ASSERT_TRUE(BuildSimtUbLayout(SocVersion::DAV_3510, Metadata(0x1b000), layout));
    EXPECT_EQ(layout.regions[2].end, layout.regions[3].begin);
}
TEST(SimtUbHostTest, CacheShrinksToRemainingSpaceAndRejectsLessThan32KiB)
{
    AclsanUbLayout layout{};
    auto metadata = Metadata(208895U);
    metadata.kernelVfType = 4;
    ASSERT_TRUE(BuildSimtUbLayout(SocVersion::DAV_3510, metadata, layout));
    EXPECT_EQ(layout.regions[1].end, 0x36000U);
    EXPECT_EQ(layout.regions[2].end, 0x38000U);
    EXPECT_EQ(layout.regions[3].begin, 0x38000U);
    EXPECT_EQ(layout.regions[3].end, 0x40000U);
    EXPECT_FALSE(BuildSimtUbLayout(SocVersion::DAV_3510, Metadata(208897U), layout));
    EXPECT_EQ(layout.valid, 0U);
    EXPECT_FALSE(BuildSimtUbLayout(SocVersion::DAV_3510, Metadata(UINT64_MAX), layout));
}
TEST(SimtUbHostTest, MissingOrInconsistentMetadataCannotBecomeValidLayouts)
{
    AclsanUbLayout layout{};
    auto metadata = Metadata();
    metadata.flags = 0;
    EXPECT_FALSE(BuildSimtUbLayout(SocVersion::DAV_3510, metadata, layout));
    metadata = Metadata();
    metadata.kernelVfType = 2;
    EXPECT_FALSE(BuildSimtUbLayout(SocVersion::DAV_3510, metadata, layout));
    metadata = Metadata();
    metadata.compilerAllocatedBytes += 8192;
    EXPECT_FALSE(BuildSimtUbLayout(SocVersion::DAV_3510, metadata, layout));
    metadata = Metadata();
    metadata.userStaticBytes = UINT64_MAX;
    EXPECT_FALSE(BuildSimtUbLayout(SocVersion::DAV_3510, metadata, layout));
    EXPECT_FALSE(BuildSimtUbLayout(static_cast<SocVersion>(255), Metadata(), layout));
    EXPECT_EQ(layout.valid, 0U);
    metadata = {8192, 0, 0, 3, ACLSAN_UB_METADATA_COMPILER | ACLSAN_UB_METADATA_STATIC};
    ASSERT_TRUE(BuildSimtUbLayout(SocVersion::DAV_3510, metadata, layout));
    EXPECT_EQ(layout.regions[0].begin, layout.regions[0].end);
    EXPECT_EQ(layout.regions[1].begin, layout.regions[1].end);
    EXPECT_EQ(layout.regions[2].begin, 0U);
    EXPECT_EQ(layout.regions[2].end, 8192U);
}
ParsedSimtTraceRecord Record(InstrType instruction)
{
    ParsedSimtTraceRecord parsed{};
    parsed.launchId = 17;
    parsed.deviceId = 2;
    parsed.phyCoreId = 36;
    parsed.instrExecId = 42;
    auto& raw = parsed.record;
    raw.instructionId = static_cast<uint32_t>(instruction);
    raw.base = 0x13078;
    raw.pc = 0x456;
    raw.scalarPc = 0x123;
    raw.invocationId = 3;
    raw.blockDimX = 32;
    raw.blockDimY = raw.blockDimZ = 1;
    raw.threadX = 7;
    return parsed;
}
TEST(SimtUbHostTest, StoresAtomicsAndReductionsPreserveThreadAndLaunchContext)
{
    AclsanUbLayout layout{};
    ASSERT_TRUE(BuildSimtUbLayout(SocVersion::DAV_3510, Metadata(), layout));
    for (const auto instruction :
         {InstrType::SIMT_STS_B8, InstrType::SIMT_STS_B16, InstrType::SIMT_STS_B32, InstrType::SIMT_STS_B64,
          InstrType::SIMT_STS_B128, InstrType::ATOM_ADD_S_U32, InstrType::ATOM_CAS_S_U32, InstrType::RED_S_BF16,
          InstrType::RED_S_FP32}) {
        auto parsed = Record(instruction);
        EXPECT_TRUE(IsSimtUbTraceRecord(parsed));
        const auto data = TranslateSimtUbTraceRecord(parsed, layout);
        ASSERT_TRUE(data.has_value());
        EXPECT_EQ(data->header.launchId, 17U);
        EXPECT_EQ(data->header.pc, 0x456U);
        EXPECT_EQ(data->header.instrExecId, 42U);
        EXPECT_EQ(data->simt.threadX, 7U);
        EXPECT_EQ(data->memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        EXPECT_NE(data->header.flags & ACLSAN_DEVICE_EVENT_FLAG_SIMT_UB, 0U);
        EXPECT_EQ(data->ubLayout.regions[1].end, 0x13080U);
    }
    auto parsed = Record(InstrType::SIMT_ST_B32);
    EXPECT_FALSE(IsSimtUbTraceRecord(parsed));                           // Opcode alone cannot establish UB.
    EXPECT_TRUE(TranslateSimtUbTraceRecord(parsed, layout).has_value()); // Collector established UB.
    EXPECT_FALSE(TranslateSimtUbTraceRecord(Record(InstrType::SIMT_LDG_B32), layout));
}
TEST(SimtUbHostTest, UbMemoryOwnerMatchesTheExecutingVectorCoreAndBlock)
{
    for (const auto instruction : {InstrType::SIMT_STS_B32, InstrType::ATOM_CAS_S_U32, InstrType::RED_S_FP32}) {
        for (uint32_t index = 0; index < 2; ++index) {
            auto parsed = Record(instruction);
            parsed.phyCoreId = ASCSAN_AIC_CORES_PER_DIE + index;
            parsed.record.blockId = 3 + index;
            const auto data = TranslateSimtUbTraceRecord(parsed, {});
            ASSERT_TRUE(data.has_value());
            EXPECT_EQ(data->header.executor.phyCoreId, parsed.phyCoreId);
            EXPECT_EQ(data->header.executor.blockId, parsed.record.blockId);
            EXPECT_EQ(data->header.executor.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
            EXPECT_EQ(data->memoryOwner.phyCoreId, parsed.phyCoreId);
            EXPECT_EQ(data->memoryOwner.blockId, parsed.record.blockId);
            EXPECT_EQ(data->memoryOwner.blockType, ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
        }
    }
}
TEST(SimtUbHostTest, AddressExpressionOverflowCannotWrapIntoAllocatedUb)
{
    auto parsed = Record(InstrType::SIMT_STS_B32);
    parsed.record.base = UINT64_MAX - 3;
    parsed.record.offset = 1;
    auto data = TranslateSimtUbTraceRecord(parsed, {});
    ASSERT_TRUE(data);
    EXPECT_EQ(data->address, UINT64_MAX);
    parsed.record.base = 0;
    parsed.record.offset = -1;
    data = TranslateSimtUbTraceRecord(parsed, {});
    ASSERT_TRUE(data);
    EXPECT_EQ(data->address, UINT64_MAX);
    parsed.record.base = 8;
    parsed.record.offset = -1;
    data = TranslateSimtUbTraceRecord(parsed, {});
    ASSERT_TRUE(data);
    EXPECT_EQ(data->address, 4U);
    parsed.record.reserved = 1;
    EXPECT_FALSE(TranslateSimtUbTraceRecord(parsed, {}));
    parsed.record.reserved = 0;
    parsed.record.threadX = 32;
    EXPECT_FALSE(TranslateSimtUbTraceRecord(parsed, {}));
}
} // namespace
