/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "aclsan_trace_runtime.h"
#include "aclsan_trace_buffer.h"
#include "aclsan_ub_layout_builder.h"
#include "aclsan_simt_ub_access.h"
#include "dbi/ctrlbin/ctrlbin_bindings.h"
#include "acl_san/aclsan_api.h"
#include "acl_san/aclsan_cbdata_device.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace {
std::vector<uint64_t> dropped;
std::vector<AclsanSimtUbMemoryAccessData> writes;
std::vector<AclsanSimtUbMemoryAccessData> failures;
size_t ordinaryWrites = 0;
std::atomic<bool> failCopy{false};
void Callback(void*, AclsanCallbackDomain, AclsanCallbackId cbid, const void* raw)
{
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(raw);
    if (cbid == ACLSAN_CBID_DEVICE_MEMORY_ACCESS)
        ++ordinaryWrites;
    if (IsSimtUbMemcheckCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, cbid, raw)) {
        auto& events = (h.flags & ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) != 0 ? failures : writes;
        events.push_back(*static_cast<const AclsanSimtUbMemoryAccessData*>(raw));
        if ((h.flags & ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR) != 0)
            dropped.push_back(h.launchId);
    }
}
aclError Copy(void* dst, size_t maximum, const void* src, size_t size, aclrtMemcpyKind)
{
    if (failCopy || size > maximum)
        return ACL_ERROR_FAILURE;
    std::memcpy(dst, src, size);
    return ACL_SUCCESS;
}
aclError Free(void* pointer)
{
    std::free(pointer);
    return ACL_SUCCESS;
}
aclError Allocate(void** pointer, size_t bytes, aclrtMemMallocPolicy)
{
    return posix_memalign(pointer, alignof(aclsan::AclsanTraceBufferHeader), bytes) == 0 ? ACL_SUCCESS :
                                                                                           ACL_ERROR_BAD_ALLOC;
}
class SimtUbRuntimeTest : public testing::Test {
protected:
    AclsanSubscriberHandle subscriber = nullptr;
    void* lastBuffer = nullptr;
    uint64_t firstLaunchId = 0;
    uint64_t LaunchId(uint64_t ordinal) const { return firstLaunchId + ordinal - 1U; }
    void SetUp() override
    {
        aclsan::ResetTraceRuntimeState();
        dropped.clear();
        writes.clear();
        failures.clear();
        ordinaryWrites = 0;
        failCopy = false;
        ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtMemcpy", &Copy), ACL_SUCCESS);
        ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtFree", &Free), ACL_SUCCESS);
        ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtMalloc", &Allocate), ACL_SUCCESS);
        ASSERT_EQ(acltoolHookInit(), ACL_SUCCESS);
        ASSERT_EQ(aclrtSetDevice(0), ACL_SUCCESS);
        ASSERT_EQ(aclsanSubscribe(&subscriber, &Callback, nullptr), ACLSAN_STATUS_SUCCESS);
        ASSERT_EQ(
            aclsanEnableCallback(
                1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS),
            ACLSAN_STATUS_SUCCESS);
        ASSERT_EQ(
            aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS),
            ACLSAN_STATUS_SUCCESS);
    }
    void TearDown() override
    {
        EXPECT_EQ(aclsanUnsubscribe(subscriber), ACLSAN_STATUS_SUCCESS);
        aclsan::ResetTraceRuntimeState();
    }
    void Launch(
        uint64_t id, uint64_t dynamic = 0x10001, uint32_t vfType = 3, bool metadataValid = true, bool ubRecord = false,
        uint32_t blockCount = 1)
    {
        aclsan::PreparedTraceLaunch prepared{};
        const auto binary = reinterpret_cast<aclrtBinHandle>(0x100);
        const auto function = reinterpret_cast<aclrtFuncHandle>(0x200);
        aclsan::RecordTraceBinaryLoadFromData(binary, true, sizeof(void*), nullptr, 0);
        aclsan::RecordTraceBinaryFunctionLookup(binary, function, "SimtUbTest");
        ASSERT_EQ(
            aclsan::PrepareTraceLaunch(
                function, blockCount, nullptr, 0, nullptr, 0, aclsan::TraceArgumentMode::HOST_ARGS, prepared),
            ACL_SUCCESS);
        ASSERT_TRUE(prepared.instrumented);
        if (firstLaunchId == 0)
            firstLaunchId = prepared.launchId;
        ASSERT_EQ(prepared.launchId, LaunchId(id));
        prepared.ubMetadata = {
            0x5000, 0x3000, dynamic, vfType, ACLSAN_UB_METADATA_COMPILER | ACLSAN_UB_METADATA_STATIC};
        if (!metadataValid)
            prepared.ubMetadata.flags = 0;
        (void)aclsan::BuildSimtUbLayout(aclsan::SocVersion::DAV_3510, prepared.ubMetadata, prepared.ubLayout);
        // One ordinary scalar record, no UB records: missing layout still matters.
        auto* control = reinterpret_cast<aclsan::AclsanTraceControl*>(
            prepared.hostBuffer.data() + aclsan::TraceWriteControlOffset(0));
        control->write.writeSequence = 1;
        auto* record = reinterpret_cast<aclsan::AclsanRawTraceRecord*>(
            prepared.hostBuffer.data() + aclsan::TraceRecordOffset(0, 0));
        record->category = aclsan::DeviceInstructionCategory::Synchronization;
        record->instrId = 440;
        record->blockId = 0;
        if (ubRecord) {
            const auto& header = *reinterpret_cast<const aclsan::AclsanTraceBufferHeader*>(prepared.hostBuffer.data());
            const uint32_t coreId = aclsan::ASCSAN_AIC_CORES_PER_DIE;
            auto* core = reinterpret_cast<aclsan::AclsanSimtCoreHeader*>(
                prepared.hostBuffer.data() + header.simtSegmentOffset + sizeof(aclsan::AclsanSimtTraceHeader) +
                coreId * aclsan::ASCSAN_SIMT_CORE_BYTES);
            core->invocationId = 1;
            auto* lane = reinterpret_cast<aclsan::AclsanSimtThreadHeader*>(core + 1);
            lane->recordCount = 1;
            lane->lastInvocation = lane->completedInvocation = 1;
            lane->completedInvocations = 1;
            auto* raw = reinterpret_cast<aclsan::AclsanSimtRawTraceRecord*>(lane + 1);
            raw->pc = 0x1234;
            raw->base = 0x13078;
            raw->invocationId = 1;
            raw->instructionId = static_cast<uint32_t>(InstrType::SIMT_STS_B128);
            raw->blockDimX = raw->blockDimY = raw->blockDimZ = 1;
        }
        lastBuffer = prepared.deviceBuffer;
        ASSERT_NE(prepared.deviceBuffer, nullptr);
        std::memcpy(prepared.deviceBuffer, prepared.hostBuffer.data(), prepared.hostBuffer.size());
        aclsan::CompleteTraceLaunch(std::move(prepared), function, nullptr, ACL_SUCCESS);
    }
};
TEST_F(SimtUbRuntimeTest, MissingMetadataOnSimtLaunchMarksIncompleteWithoutUbRecords)
{
    Launch(1, 0x10001, 3, false);
    const auto result = aclsan::CollectTraceStream(nullptr);
    EXPECT_FALSE(result.Complete());
    EXPECT_GT(result.droppedRecordCount, 0U);
    ASSERT_EQ(dropped.size(), 1U);
    EXPECT_EQ(dropped[0], LaunchId(1));
    EXPECT_TRUE(writes.empty());
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_TRUE(IsSimtUbMemcheckCallback(
        ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, &failures[0]));
    EXPECT_EQ(ordinaryWrites, 0U);
}
TEST_F(SimtUbRuntimeTest, LaunchLayoutsAreCalculatedIndependently)
{
    Launch(1);
    Launch(2, 0x1b000);
    const auto result = aclsan::CollectTraceStream(nullptr);
    EXPECT_EQ(result.completeLaunchCount, 2U);
    EXPECT_TRUE(result.Complete());
    EXPECT_TRUE(dropped.empty());
}
TEST_F(SimtUbRuntimeTest, RepeatedUbWritesKeepBlockAndInvocationIdentityWithoutDuplicates)
{
    Launch(1, 0x10001, 3, true, true, 2);
    auto* buffer = static_cast<uint8_t*>(lastBuffer);
    const auto& header = *reinterpret_cast<const aclsan::AclsanTraceBufferHeader*>(buffer);
    for (uint32_t block = 0; block < 2; ++block) {
        const uint32_t coreId = aclsan::ASCSAN_AIC_CORES_PER_DIE + block;
        auto* core = reinterpret_cast<aclsan::AclsanSimtCoreHeader*>(
            buffer + header.simtSegmentOffset + sizeof(aclsan::AclsanSimtTraceHeader) +
            coreId * aclsan::ASCSAN_SIMT_CORE_BYTES);
        core->invocationId = 2;
        auto* lane = reinterpret_cast<aclsan::AclsanSimtThreadHeader*>(core + 1);
        lane->recordCount = 4;
        lane->lastInvocation = lane->completedInvocation = 2;
        lane->completedInvocations = 3;
        auto* records = reinterpret_cast<aclsan::AclsanSimtRawTraceRecord*>(lane + 1);
        for (uint32_t index = 0; index < 4; ++index) {
            records[index] = {};
            records[index].pc = 0x1234;
            records[index].base = 0x15080;
            records[index].invocationId = 1 + index / 2;
            records[index].blockId = block;
            records[index].instructionId = static_cast<uint32_t>(InstrType::SIMT_STS_B32);
            records[index].blockDimX = records[index].blockDimY = records[index].blockDimZ = 1;
        }
    }
    EXPECT_FALSE(aclsan::CollectTraceStream(nullptr, false).Complete());
    ASSERT_EQ(writes.size(), 8U);
    std::set<uint64_t> ids;
    size_t counts[2][2]{};
    for (const auto& access : writes) {
        ids.insert(access.header.instrExecId);
        ASSERT_LT(access.header.executor.blockId, 2U);
        const uint32_t invocation = static_cast<uint32_t>(access.simt.invocationId);
        ASSERT_GE(invocation, 1U);
        ASSERT_LE(invocation, 2U);
        ++counts[access.header.executor.blockId][invocation - 1];
        EXPECT_EQ(access.header.pc, 0x1234U);
    }
    EXPECT_EQ(ids.size(), 8U);
    for (const auto& block : counts)
        for (size_t count : block)
            EXPECT_EQ(count, 2U);
    EXPECT_FALSE(aclsan::CollectTraceStream(nullptr, false).Complete());
    EXPECT_EQ(writes.size(), 8U);
    EXPECT_TRUE(aclsan::CollectTraceStream(nullptr).Complete());
    EXPECT_EQ(writes.size(), 8U);
    EXPECT_TRUE(failures.empty());
}
TEST_F(SimtUbRuntimeTest, InvalidDynamicSizeMarksOnlyItsOwnLaunchIncomplete)
{
    Launch(1);
    Launch(2, UINT64_MAX);
    const auto result = aclsan::CollectTraceStream(nullptr);
    EXPECT_EQ(result.completeLaunchCount, 1U);
    EXPECT_FALSE(result.Complete());
    ASSERT_EQ(dropped.size(), 1U);
    EXPECT_EQ(dropped[0], LaunchId(2));
}
TEST_F(SimtUbRuntimeTest, NonSimtLaunchDoesNotRequireCacheBounds)
{
    Launch(1, 0, 2);
    EXPECT_TRUE(aclsan::CollectTraceStream(nullptr).Complete());
    EXPECT_TRUE(dropped.empty());
}
TEST_F(SimtUbRuntimeTest, UbWritesRetainLayoutsAcrossInterleavedLaunches)
{
    Launch(1, 0x10001, 3, true, true);
    Launch(2, 0x1b000, 3, true, true);
    EXPECT_TRUE(aclsan::CollectTraceStream(nullptr).Complete());
    ASSERT_EQ(writes.size(), 2U);
    EXPECT_EQ(ordinaryWrites, 0U);
    EXPECT_EQ(writes[0].header.launchId, LaunchId(1));
    EXPECT_EQ(writes[0].ubLayout.regions[1].end, 0x13080U);
    EXPECT_EQ(writes[1].header.launchId, LaunchId(2));
    EXPECT_EQ(writes[1].ubLayout.regions[1].end, 0x1e000U);
    EXPECT_EQ(writes[0].simt.threadX, 0U);
    EXPECT_EQ(writes[0].address, 0x13078U);
    EXPECT_EQ(writes[0].layout.range.bytes, 16U);
    EXPECT_TRUE(dropped.empty());
}
TEST_F(SimtUbRuntimeTest, FailedDeviceCopyEmitsIncompleteMarker)
{
    Launch(1);
    failCopy = true;
    EXPECT_FALSE(aclsan::CollectTraceStream(nullptr).Complete());
    ASSERT_EQ(dropped.size(), 1U);
    EXPECT_EQ(dropped[0], LaunchId(1));
}
} // namespace
