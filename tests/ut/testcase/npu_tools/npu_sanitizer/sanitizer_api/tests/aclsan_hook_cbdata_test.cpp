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

#include "../../common/tests/plog_capture.h"
#include "aclsan_boundary_stub.h"
#include <cassert>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "acl_san/aclsan_api.h"
#include "acl_san/aclsan_cbdata.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "device_instr/common/instruction_id.h"
#include "device_instr/decoder_registry.h"
#include "aclsan_trace_buffer.h"
#include "aclsan_trace_runtime.h"
#include "../src/acl_san/aclsan_dispatch.cpp"
#include "../src/acl_san/aclsan_hook_aclrt.cpp"

namespace {

template <typename T, typename = void>
struct CanDispatchDeviceMemoryAccess : std::false_type {};

template <typename T>
struct CanDispatchDeviceMemoryAccess<
    T, std::void_t<decltype(aclsan::AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(std::declval<const T&>()))>>
    : std::true_type {};

static_assert(!CanDispatchDeviceMemoryAccess<aclsan::DeviceMemoryAccessDataList>::value);

struct CallbackCapture {
    AclsanCallbackDomain domain = ACLSAN_CB_DOMAIN_INVALID;
    AclsanCallbackId callbackId{};
    AclsanResourceData resource{};
    AclsanMemoryWriteData memoryWrite{};
    AclsanSynchronizeData synchronize{};
    AclsanLaunchData launch{};
    std::string launchFunctionName;
    uint32_t calls = 0;
};

CallbackCapture g_callbackCapture{};
bool g_callbackEnabled = true;
uint32_t g_deviceMemoryCallbackCount = 0;
uint32_t g_deviceStateCallbackCount = 0;
uint32_t g_deviceSyncCallbackCount = 0;
uint32_t g_decoderCallCount = 0;
std::array<AclsanDeviceMemoryAccessData, 16> g_deviceMemoryCallbacks{};
std::array<AclsanDeviceRegisterStateData, 16> g_deviceStateCallbacks{};
std::array<AclsanDeviceSyncData, 3> g_deviceSyncCallbacks{};
void* g_lastFreedAddress = nullptr;
int32_t g_lastSynchronizeTimeout = 0;
bool g_mallocOriginalAvailable = true;
bool g_freeOriginalAvailable = true;
uint32_t g_functionAttributeQueryCalls = 0;
int32_t g_currentDeviceId = 3;
aclError g_getDeviceResult = ACL_SUCCESS;
int32_t g_clearCallbackResult = 0;
int32_t g_registerMallocResult = 0;
size_t g_lastMallocSize = 0;
bool g_hostInputAllocation = false;
aclError g_hostInputMallocResult = ACL_SUCCESS;
aclError g_hostInputMemcpyResult = ACL_SUCCESS;
aclError g_hostWriteResult = ACL_SUCCESS;
std::array<uint8_t, 64> g_hostInputStorage{};

void ResetCapture()
{
    g_callbackCapture = {};
    g_callbackEnabled = true;
    g_deviceMemoryCallbackCount = 0;
    g_deviceStateCallbackCount = 0;
    g_deviceSyncCallbackCount = 0;
    g_deviceMemoryCallbacks = {};
    g_deviceStateCallbacks = {};
    g_deviceSyncCallbacks = {};
    g_lastFreedAddress = nullptr;
    g_lastSynchronizeTimeout = 0;
    g_currentDeviceId = 3;
    g_getDeviceResult = ACL_SUCCESS;
    g_clearCallbackResult = 0;
    g_registerMallocResult = 0;
    g_lastMallocSize = 0;
    g_functionAttributeQueryCalls = 0;
    g_hostInputAllocation = false;
    g_hostInputMallocResult = ACL_SUCCESS;
    g_hostInputMemcpyResult = ACL_SUCCESS;
    g_hostWriteResult = ACL_SUCCESS;
}

void* Address(uintptr_t value) { return reinterpret_cast<void*>(value); }

std::optional<aclsan::DecodedInstruction> CountDecoderCalls(const aclsan::AclsanRawTraceRecord&) noexcept
{
    ++g_decoderCallCount;
    return std::nullopt;
}

template <typename Action>
std::string CaptureDebugLogs(Action action)
{
    PlogCapture capture;
    action();
    return capture.Text();
}

aclError FakeAclrtMalloc(void** deviceAddress, size_t size, aclrtMemMallocPolicy policy)
{
    g_lastMallocSize = size;
    (void)policy;
    if (deviceAddress == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (g_hostInputAllocation && g_hostInputMallocResult != ACL_SUCCESS) {
        return g_hostInputMallocResult;
    }
    *deviceAddress = g_hostInputAllocation ? g_hostInputStorage.data() : Address(0x12340000U);
    return ACL_SUCCESS;
}

aclError FakeAclrtFree(void* deviceAddress)
{
    g_lastFreedAddress = deviceAddress;
    return ACL_SUCCESS;
}

aclError FakeAclrtSynchronizeStream(aclrtStream stream)
{
    return stream == Address(0x45670000U) ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM;
}

aclError FakeAclrtSynchronizeStreamWithTimeout(aclrtStream stream, int32_t timeout)
{
    g_lastSynchronizeTimeout = timeout;
    return stream == Address(0x45670000U) ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM;
}

aclError FakeAclrtGetDevice(int32_t* deviceId)
{
    if (g_getDeviceResult != ACL_SUCCESS) {
        return g_getDeviceResult;
    }
    if (deviceId == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *deviceId = g_currentDeviceId;
    return ACL_SUCCESS;
}

aclError FakeAclrtBinaryGetGlobal(aclrtBinHandle, const char*, void** address, size_t* bytes)
{
    static uint64_t global = 0;
    if (address == nullptr || bytes == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *address = &global;
    *bytes = sizeof(global);
    return ACL_SUCCESS;
}

aclError FakeAclrtGetFunctionAttribute(aclrtFuncHandle, aclrtFuncAttribute attrType, int64_t* attrValue)
{
    ++g_functionAttributeQueryCalls;
    if (attrValue == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    switch (attrType) {
        case ACL_FUNC_ATTR_KERNEL_TYPE:
            *attrValue = ACL_KERNEL_TYPE_MIX;
            return ACL_SUCCESS;
        case ACL_FUNC_ATTR_KERNEL_RATIO:
            *attrValue = 0x00010002;
            return ACL_SUCCESS;
        case ACL_FUNC_ATTR_KERNEL_SCHED_MODE:
            *attrValue = 1;
            return ACL_SUCCESS;
        default:
            return ACL_ERROR_INVALID_PARAM;
    }
}

aclError FakeAclrtLaunchKernelWithHostArgs(
    aclrtFuncHandle function, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, void*, size_t, aclrtPlaceHolderInfo*,
    size_t)
{
    return function == Address(0x12345678U) ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM;
}

const char* FakeAclrtGetSocName() { return "Ascend950PR_9589"; }

aclError FakeAclrtGetDeviceInfo(uint32_t, aclrtDevAttr attr, int64_t* value)
{
    if (value == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *value = attr == ACL_DEV_ATTR_CUBE_CORE_NUM ? 36 : 72;
    return ACL_SUCCESS;
}

void CheckCommonData(const AclsanCallbackCommonData& common, size_t expectedSize, const char* apiName)
{
    assert(common.version == ACLSAN_API_VERSION);
    assert(common.size == expectedSize);
    assert(std::strcmp(common.apiName, apiName) == 0);
    assert(common.result == ACL_SUCCESS);
    assert(common.correlationId == 0);
}

void TestMallocCallbackData()
{
    ResetCapture();
    void* deviceAddress = nullptr;

    assert(aclrtMallocHook(&deviceAddress, 64, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    assert(g_callbackCapture.calls == 1);
    assert(g_callbackCapture.domain == ACLSAN_CB_DOMAIN_RESOURCE);
    assert(g_callbackCapture.callbackId == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC);
    CheckCommonData(g_callbackCapture.resource.common, sizeof(AclsanResourceData), "aclrtMalloc");
    assert(g_callbackCapture.resource.ptr == deviceAddress);
    assert(g_callbackCapture.resource.bytes == 64);
    assert(g_callbackCapture.resource.memorySpace == ACLSAN_MEMORY_SPACE_DEVICE);
    assert(g_callbackCapture.resource.deviceId == 3);
    assert(g_callbackCapture.resource.resourceId == reinterpret_cast<uintptr_t>(deviceAddress));
}

void TestMallocCallbackDataRoundsSizeUpTo32Bytes()
{
    constexpr std::array<std::pair<size_t, uint64_t>, 4> kCases = {{{1, 32}, {31, 32}, {32, 32}, {33, 64}}};
    for (const auto& [requestedSize, expectedSize] : kCases) {
        ResetCapture();
        void* deviceAddress = nullptr;

        assert(aclrtMallocHook(&deviceAddress, requestedSize, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
        assert(g_lastMallocSize == requestedSize);
        assert(g_callbackCapture.calls == 1);
        assert(g_callbackCapture.resource.ptr == deviceAddress);
        assert(g_callbackCapture.resource.bytes == expectedSize);
    }
}

void TestMallocAlign32CallbackData()
{
    ResetCapture();
    void* deviceAddress = nullptr;

    assert(aclrtMallocAlign32Hook(&deviceAddress, 33, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    assert(g_callbackCapture.calls == 1);
    assert(g_callbackCapture.domain == ACLSAN_CB_DOMAIN_RESOURCE);
    assert(g_callbackCapture.callbackId == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC);
    CheckCommonData(g_callbackCapture.resource.common, sizeof(AclsanResourceData), "aclrtMallocAlign32");
    assert(g_callbackCapture.resource.ptr == deviceAddress);
    assert(g_callbackCapture.resource.bytes == 64);
    assert(g_callbackCapture.resource.memorySpace == ACLSAN_MEMORY_SPACE_DEVICE);
    assert(g_callbackCapture.resource.deviceId == 3);
}

void TestMallocPreservesOriginalRuntimeError()
{
    ResetCapture();
    assert(aclrtMallocHook(nullptr, 64, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_ERROR_INVALID_PARAM);
}

void TestMallocSkipsCallbackWhenGetDeviceFails()
{
    ResetCapture();
    g_getDeviceResult = ACL_ERROR_RT_INTERNAL_ERROR;
    void* deviceAddress = nullptr;

    assert(aclrtMallocHook(&deviceAddress, 64, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    assert(g_callbackCapture.calls == 0);
}

void TestMissingOriginalMallocAborts()
{
    ResetCapture();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        g_mallocOriginalAvailable = false;
        void* deviceAddress = nullptr;
        (void)aclrtMallocHook(&deviceAddress, 64, ACL_MEM_MALLOC_HUGE_FIRST);
        _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
}

void TestFreeCallbackData()
{
    ResetCapture();
    void* const deviceAddress = Address(0x12340000U);

    assert(aclrtFreeHook(deviceAddress) == ACL_SUCCESS);
    assert(g_lastFreedAddress == deviceAddress);
    assert(g_callbackCapture.calls == 1);
    assert(g_callbackCapture.domain == ACLSAN_CB_DOMAIN_RESOURCE);
    assert(g_callbackCapture.callbackId == ACLSAN_CBID_RESOURCE_MEMORY_FREE);
    CheckCommonData(g_callbackCapture.resource.common, sizeof(AclsanResourceData), "aclrtFree");
    assert(g_callbackCapture.resource.ptr == deviceAddress);
    assert(g_callbackCapture.resource.bytes == 0);
    assert(g_callbackCapture.resource.memorySpace == ACLSAN_MEMORY_SPACE_DEVICE);
    assert(g_callbackCapture.resource.deviceId == 3);
    assert(g_callbackCapture.resource.resourceId == reinterpret_cast<uintptr_t>(deviceAddress));
}

void TestMissingOriginalFreeAborts()
{
    ResetCapture();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        g_freeOriginalAvailable = false;
        (void)aclrtFreeHook(Address(0x12340000U));
        _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
}

void TestRuntimeHookRegistrationFailureAborts()
{
    ResetCapture();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        g_registerMallocResult = 1;
        aclsan::ApplyRuntimeHooks({ACL_RT_API_aclrtMalloc});
        _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
}

void TestRuntimeHookClearingFailureAborts()
{
    ResetCapture();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        g_clearCallbackResult = 1;
        aclsan::ApplyRuntimeHooks({});
        _exit(0);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
}

void TestSynchronizeStreamCallbackData()
{
    ResetCapture();
    void* const stream = Address(0x45670000U);

    assert(aclrtSynchronizeStreamHook(stream) == ACL_SUCCESS);
    assert(g_deviceMemoryCallbackCount == 0);
    assert(g_deviceSyncCallbackCount == 0);
    assert(g_callbackCapture.calls == 1);
    assert(g_callbackCapture.domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE);
    assert(g_callbackCapture.callbackId == ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END);
    CheckCommonData(g_callbackCapture.synchronize.common, sizeof(AclsanSynchronizeData), "aclrtSynchronizeStream");
    assert(g_callbackCapture.synchronize.stream == stream);
}

void TestSynchronizeStreamWithTimeoutCallbackData()
{
    ResetCapture();
    void* const stream = Address(0x45670000U);

    assert(aclrtSynchronizeStreamWithTimeoutHook(stream, 1234) == ACL_SUCCESS);
    assert(g_lastSynchronizeTimeout == 1234);
    assert(g_deviceMemoryCallbackCount == 0);
    assert(g_deviceSyncCallbackCount == 0);
    assert(g_callbackCapture.calls == 1);
    assert(g_callbackCapture.domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE);
    assert(g_callbackCapture.callbackId == ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END);
    CheckCommonData(
        g_callbackCapture.synchronize.common, sizeof(AclsanSynchronizeData), "aclrtSynchronizeStreamWithTimeout");
    assert(g_callbackCapture.synchronize.stream == stream);
}

void TestSetPaddingRecordsUpdateLaunchStateAndCallback()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord first{};
    first.record.instrId = 392;
    first.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    first.record.args[0] = UINT64_C(0x1111222233334444);
    first.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    first.blockId = 3;
    first.phyCoreId = 5;
    first.instrExecId = 1;
    first.launchId = 27;
    first.deviceId = 3;
    aclsan::ParsedTraceRecord second = first;
    second.record.args[0] = UINT64_C(0xfedcba9876543210);
    second.instrExecId = 2;

    const std::string logs = CaptureDebugLogs([&] { aclsan::DispatchTraceRecords({first, second}, *decoder); });

    assert(g_deviceMemoryCallbackCount == 0);
    assert(g_deviceStateCallbackCount == 2);
    assert(g_deviceSyncCallbackCount == 0);
    assert(g_deviceStateCallbacks[0].regId == ACLSAN_DEVICE_REGISTER_SET_PADDING);
    assert(g_deviceStateCallbacks[0].value == UINT64_C(0x1111222233334444));
    assert(g_deviceStateCallbacks[1].value == UINT64_C(0xfedcba9876543210));
    assert(
        logs.find("[raw] deviceId=3 phyCoreId=5 blockId=3 blockType=AIC  instrExecId=1 launchId=27  "
                  "type=AclsanRawTraceRecord pc=0x0 instrId=392 siteId=0 category=3 pipeline=0 "
                  "args=[0x1111222233334444,0x0,0x0,0x0,0x0]") != std::string::npos);
    assert(
        logs.find("[raw] deviceId=3 phyCoreId=5 blockId=3 blockType=AIC  instrExecId=2 launchId=27  "
                  "type=AclsanRawTraceRecord pc=0x0 instrId=392 siteId=0 category=3 pipeline=0 "
                  "args=[0xfedcba9876543210,0x0,0x0,0x0,0x0]") != std::string::npos);
    assert(logs.find("[param] type=SetPaddingParamField value=0x1111222233334444") != std::string::npos);
    assert(logs.find("[param] type=SetPaddingParamField value=0xfedcba9876543210") != std::string::npos);
    assert(
        logs.find("[register] action=update register=set_padding launchId=27 blockType=2 blockId=3 "
                  "value=0x1111222233334444") != std::string::npos);
    assert(
        logs.find("[register] action=update register=set_padding launchId=27 blockType=2 blockId=3 "
                  "value=0xfedcba9876543210") != std::string::npos);
}

void TestRegisterStateCallbackPreservesPackedValue()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord state{};
    state.record.instrId = 124;
    state.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    state.record.args[0] = UINT64_C(0xfedcba9887654321);
    state.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    state.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    state.blockId = 2;
    state.phyCoreId = 7;
    state.instrExecId = 3;
    state.launchId = 41;
    state.deviceId = 4;

    aclsan::DispatchTraceRecords({state}, *decoder);

    assert(g_deviceStateCallbackCount == 1);
    const auto& callback = g_deviceStateCallbacks[0];
    assert(callback.header.size == sizeof(AclsanDeviceRegisterStateData));
    assert(callback.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    assert(callback.header.launchId == 41);
    assert(callback.header.blockId == 2);
    assert(callback.regId == ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
    assert(callback.value == UINT64_C(0xfedcba9887654321));
}

void TestSetL12DDoesNotPublishPersistentRegisterState()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord record{};
    record.record.instrId = 149;
    record.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    record.record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    record.record.args[0] = 0x2000;
    record.record.args[1] = 1;
    record.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    record.blockId = 1;
    record.launchId = 42;

    aclsan::DispatchTraceRecords({record}, *decoder);

    assert(g_deviceStateCallbackCount == 0);
    assert(g_deviceMemoryCallbackCount == 0);
}

void TestRegisterStatePersistsAcrossTraceSnapshots()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    aclsan::dav3510::Dav3510RegisterStateManager registerState{42};

    aclsan::ParsedTraceRecord state{};
    state.record.instrId = 124;
    state.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    state.record.args[0] = 64;
    state.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    state.blockId = 3;
    state.launchId = 42;
    aclsan::DispatchTraceRecords({state}, *decoder, nullptr, &registerState);
    assert(g_deviceStateCallbackCount == 1);

    ResetCapture();
    aclsan::ParsedTraceRecord memory = state;
    memory.record = {};
    memory.record.instrId = 72;
    memory.record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    memory.record.args[0] = 0x2000;
    memory.record.args[1] = 0x4000;
    memory.record.args[3] = (UINT64_C(1) << 12U) | (UINT64_C(1) << 24U);
    memory.instrExecId = 1;
    aclsan::DispatchTraceRecords({memory}, *decoder, nullptr, &registerState);

    assert(g_deviceMemoryCallbackCount == 2);
    const auto& callback = g_deviceMemoryCallbacks[0];
    assert(callback.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    assert(callback.regDependencyMask0 == (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE));
    const std::vector<aclsan::InternalMemoryRange> readOnlyRanges{{reinterpret_cast<void*>(0x4000), 512}};
    ResetCapture();
    aclsan::DispatchTraceRecords({memory}, *decoder, nullptr, &registerState, &readOnlyRanges);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 1U);
    EXPECT_EQ(g_deviceMemoryCallbacks[0].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
    ResetCapture();
    memory.record.args[1] = 0x8000;
    aclsan::DispatchTraceRecords({memory}, *decoder, nullptr, &registerState, &readOnlyRanges);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 2U);
    EXPECT_EQ(g_deviceMemoryCallbacks[0].header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    EXPECT_EQ(g_deviceMemoryCallbacks[0].instructionId, 72U);
    EXPECT_EQ(g_deviceMemoryCallbacks[0].regDependencyMask0, UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE);
}

void TestUndefinedInstructionIdsSkipDecoder()
{
    ResetCapture();
    g_decoderCallCount = 0;
    const aclsan::DeviceInstructionDecoder decoder{"test", CountDecoderCalls};
    aclsan::ParsedTraceRecord unknown{};
    unknown.record.instrId = 0;
    aclsan::ParsedTraceRecord outOfRange{};
    outOfRange.record.instrId = UINT32_MAX;

    aclsan::DispatchTraceRecords({unknown, outOfRange}, decoder);

    assert(g_decoderCallCount == 0);
    EXPECT_EQ(g_deviceMemoryCallbackCount, 0U);
}

void ExpectL0CoverageMarker(const aclsan::ParsedTraceRecord& record)
{
    ASSERT_EQ(g_deviceMemoryCallbackCount, 1U);
    const auto& marker = g_deviceMemoryCallbacks[0];
    EXPECT_EQ(marker.instructionId, record.record.instrId);
    EXPECT_EQ(marker.header.launchId, record.launchId);
    EXPECT_EQ(marker.header.pc, record.record.pc);
    EXPECT_EQ(marker.header.blockId, record.blockId);
    EXPECT_EQ(marker.header.instrExecId, record.instrExecId);
    EXPECT_EQ(marker.header.flags, ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED | ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED);
    EXPECT_EQ(marker.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN);
    EXPECT_EQ(marker.accessMode, 0U);
    EXPECT_EQ(marker.layoutKind, 0U);
    EXPECT_EQ(marker.accessIndex, 0U);
    EXPECT_EQ(marker.accessCount, 1U);
}

void TestDefinedInstructionIdUsesDecoder()
{
    ResetCapture();
    g_decoderCallCount = 0;
    const aclsan::DeviceInstructionDecoder decoder{"test", CountDecoderCalls};
    const std::array newlyDefinedIds{
        aclsan::InstructionId::LoopSizeUbufToGm,    aclsan::InstructionId::Loop1StrideUbufToGm,
        aclsan::InstructionId::Loop2StrideUbufToGm, aclsan::InstructionId::LoopSizeGmToUbuf,
        aclsan::InstructionId::Loop1StrideGmToUbuf, aclsan::InstructionId::Loop2StrideGmToUbuf,
        aclsan::InstructionId::NdDmaPadCount,       aclsan::InstructionId::Loop3Param,
        aclsan::InstructionId::CopyCbufToFbuf,      aclsan::InstructionId::FixL0cToCbufF32,
        aclsan::InstructionId::FixL0cToCbufS32,     aclsan::InstructionId::FixL0cToUbufF32,
        aclsan::InstructionId::FixL0cToUbufS32,     aclsan::InstructionId::CopyUbufToCbuf,
        aclsan::InstructionId::LoopSizeGmToCbuf,    aclsan::InstructionId::Loop1StrideGmToCbuf,
        aclsan::InstructionId::Loop2StrideGmToCbuf,
    };
    for (const auto id : newlyDefinedIds) {
        ResetCapture();
        aclsan::ParsedTraceRecord record{};
        record.record.instrId = static_cast<uint32_t>(id);
        aclsan::DispatchTraceRecords({record}, decoder);
        EXPECT_EQ(g_deviceMemoryCallbackCount, 0U);
    }

    assert(g_decoderCallCount == newlyDefinedIds.size());
    const std::array<uint32_t, 4> fixpipeIds{386, 387, 390, 391};
    for (const auto id : fixpipeIds) {
        SCOPED_TRACE(id);
        ResetCapture();
        aclsan::ParsedTraceRecord record{};
        record.record.instrId = id;
        aclsan::DispatchTraceRecords({record}, decoder);
        EXPECT_EQ(g_deviceMemoryCallbackCount, 0U);
    }
    const std::array<uint32_t, 36> l0OperandIds{137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148,
                                                151, 152, 153, 154, 155, 156, 157, 422, 400, 401, 402, 403,
                                                404, 405, 406, 407, 408, 409, 410, 411, 412, 413, 414, 415};
    for (const auto id : l0OperandIds) {
        SCOPED_TRACE(id);
        ResetCapture();
        aclsan::ParsedTraceRecord record{};
        record.record.instrId = id;
        record.record.pc = 0x1234;
        record.launchId = 33;
        record.blockId = 2;
        record.instrExecId = 11;
        aclsan::DispatchTraceRecords({record}, decoder);
        // Failed L0 decoding must expose a coverage gap without inventing an access.
        ExpectL0CoverageMarker(record);
    }
    assert(g_decoderCallCount == newlyDefinedIds.size() + fixpipeIds.size() + l0OperandIds.size());
}

void TestCubeLoadAndUnsupportedModeReachMemoryCallback()
{
    ResetCapture();
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);
    aclsan::ParsedTraceRecord load{};
    load.record.instrId = 144;
    load.record.pipeline = ACLSAN_DEVICE_PIPE_MTE1;
    load.record.args[0] = 0x10000;
    load.record.args[1] = 0x200;
    load.record.args[2] = (UINT64_C(1) << 32) | (UINT64_C(1) << 40);
    load.record.args[3] = UINT64_C(1) | (UINT64_C(1) << 16);
    load.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    load.instrExecId = 11;
    load.launchId = 33;
    aclsan::DispatchTraceRecords({load}, *decoder);
    assert(g_deviceMemoryCallbackCount == 2);
    const auto& read = g_deviceMemoryCallbacks[0];
    const auto& write = g_deviceMemoryCallbacks[1];
    assert(read.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L1);
    assert(read.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(read.address == 0x200 && read.layout.range.bytes == 512);
    assert(write.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0A);
    assert(write.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    assert(write.address == 0x10000 && write.layout.range.bytes == 512);
    assert(read.header.instrExecId == 11 && write.header.instrExecId == 11);
    assert(read.accessCount == 2 && write.accessCount == 2);

    ResetCapture();
    load.record.args[4] = 1; // B32 transpose requires an even K step.
    aclsan::DispatchTraceRecords({load}, *decoder);
    ExpectL0CoverageMarker(load);
}

void TestNdDmaPadCountStatePreservesExactGmFootprint()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord base{};
    base.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    base.blockId = 4;
    base.phyCoreId = 6;
    base.launchId = 28;
    base.deviceId = 3;

    aclsan::ParsedTraceRecord padding = base;
    padding.record.instrId = 131;
    padding.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    padding.record.args[0] = UINT64_C(0x0807060504030201);
    padding.instrExecId = 1;

    aclsan::ParsedTraceRecord loop0Stride = base;
    loop0Stride.record.instrId = 132;
    loop0Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop0Stride.record.args[0] = (UINT64_C(1) << 20U) | 1;
    loop0Stride.instrExecId = 2;

    aclsan::ParsedTraceRecord loop1Stride = base;
    loop1Stride.record.instrId = 133;
    loop1Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop1Stride.record.args[0] = (UINT64_C(8) << 20U) | 16;
    loop1Stride.instrExecId = 3;

    aclsan::ParsedTraceRecord loop2Stride = base;
    loop2Stride.record.instrId = 134;
    loop2Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop2Stride.record.args[0] = 128;
    loop2Stride.instrExecId = 4;

    aclsan::ParsedTraceRecord loop3Stride = base;
    loop3Stride.record.instrId = 135;
    loop3Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop3Stride.record.args[0] = 1024;
    loop3Stride.instrExecId = 5;

    aclsan::ParsedTraceRecord loop4Stride = base;
    loop4Stride.record.instrId = 136;
    loop4Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop4Stride.record.args[0] = 16384;
    loop4Stride.instrExecId = 6;

    aclsan::ParsedTraceRecord memory = base;
    memory.record.instrId = 87;
    memory.record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    memory.record.args[0] = 0x2000;
    memory.record.args[1] = 0x4000;
    memory.record.args[2] = (UINT64_C(3) << 4U) | (UINT64_C(2) << 24U) | (UINT64_C(1) << 44U);
    memory.record.args[3] =
        UINT64_C(1) | (UINT64_C(1) << 20U) | (UINT64_C(2) << 40U) | (UINT64_C(3) << 48U) | (UINT64_C(1) << 56U);
    memory.instrExecId = 7;

    const std::string logs = CaptureDebugLogs([&] {
        aclsan::DispatchTraceRecords(
            {padding, loop0Stride, loop1Stride, loop2Stride, loop3Stride, loop4Stride, memory}, *decoder);
    });

    assert(g_deviceMemoryCallbackCount == 2);
    const auto& access = g_deviceMemoryCallbacks[0];
    assert(access.address == 0x4000);
    assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(access.layout.ndAffine.elementBytes == 1);
    assert(access.layout.ndAffine.dims[0] == 3);
    assert(access.layout.ndAffine.dims[1] == 2);
    assert(access.layout.ndAffine.strides[0] == 1);
    assert(access.layout.ndAffine.strides[1] == 8);
    const auto& local = g_deviceMemoryCallbacks[1];
    assert(local.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
    assert(local.address == 0x2000 && local.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    assert(local.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(local.layout.ndAffine.rank == 5 && local.layout.ndAffine.elementBytes == 1);
    const std::array<uint64_t, 5> paddedCounts{8, 5, 8, 12, 16};
    const std::array<int64_t, 5> dstStrides{1, 16, 128, 1024, 16384};
    for (size_t axis = 0; axis < paddedCounts.size(); ++axis) {
        assert(local.layout.ndAffine.dims[axis] == paddedCounts[axis]);
        assert(local.layout.ndAffine.strides[axis] == dstStrides[axis]);
    }
    assert(local.accessCount == 2 && local.header.instrExecId == access.header.instrExecId);
    assert(logs.find("type=NdDmaPadCountParamField left=[1,3,5,7] right=[2,4,6,8]") != std::string::npos);

    ResetCapture();
    const std::vector<aclsan::PreparedTraceLaunch::HostInput> internalInputs{{Address(0x4000), 32}};
    aclsan::DispatchTraceRecords(
        {padding, loop0Stride, loop1Stride, loop2Stride, loop3Stride, loop4Stride, memory}, *decoder, &internalInputs);
    assert(g_deviceMemoryCallbackCount == 1);
    assert(g_deviceMemoryCallbacks[0].memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
}

void TestDmaOuterLoopStateReachesMemoryCallback()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord base{};
    base.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    base.blockId = 5;
    base.phyCoreId = 7;
    base.launchId = 29;
    base.deviceId = 3;

    aclsan::ParsedTraceRecord loopSize = base;
    loopSize.record.instrId = 128;
    loopSize.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loopSize.record.args[0] = UINT64_C(2) | (UINT64_C(3) << 21U);
    loopSize.instrExecId = 1;

    aclsan::ParsedTraceRecord loop1Stride = base;
    loop1Stride.record.instrId = 129;
    loop1Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop1Stride.record.args[0] = UINT64_C(0x200) | (UINT64_C(0x20) << 40U);
    loop1Stride.instrExecId = 2;

    aclsan::ParsedTraceRecord loop2Stride = base;
    loop2Stride.record.instrId = 130;
    loop2Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop2Stride.record.args[0] = UINT64_C(0x1000) | (UINT64_C(0x40) << 40U);
    loop2Stride.instrExecId = 3;

    aclsan::ParsedTraceRecord memory = base;
    memory.record.instrId = 85;
    memory.record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    memory.record.args[0] = 0x2000;
    memory.record.args[1] = 0x6000;
    memory.record.args[2] = (UINT64_C(2) << 4U) | (UINT64_C(32) << 25U);
    memory.record.args[3] = UINT64_C(64) | (UINT64_C(96) << 40U);
    memory.instrExecId = 4;

    aclsan::DispatchTraceRecords({loopSize, loop1Stride, loop2Stride, memory}, *decoder);

    assert(g_deviceMemoryCallbackCount == 2);
    const auto& access = g_deviceMemoryCallbacks[0];
    assert(access.address == 0x6000);
    assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(access.layout.ndAffine.rank == 3);
    assert(access.layout.ndAffine.elementBytes == 32);
    assert(access.layout.ndAffine.dims[0] == 2 && access.layout.ndAffine.strides[0] == 64);
    assert(access.layout.ndAffine.dims[1] == 2 && access.layout.ndAffine.strides[1] == 0x200);
    assert(access.layout.ndAffine.dims[2] == 3 && access.layout.ndAffine.strides[2] == 0x1000);
    const auto& local = g_deviceMemoryCallbacks[1];
    assert(local.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
    assert(local.address == 0x2000 && local.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    assert(local.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(local.layout.ndAffine.elementBytes == 32);
    assert(local.layout.ndAffine.dims[0] == 2 && local.layout.ndAffine.strides[0] == 96);
    assert(local.layout.ndAffine.dims[1] == 2 && local.layout.ndAffine.strides[1] == 0x20);
    assert(local.layout.ndAffine.dims[2] == 3 && local.layout.ndAffine.strides[2] == 0x40);
    assert(local.accessCount == 2 && local.header.instrExecId == access.header.instrExecId);
}

void TestUbufToGmOuterLoopStateReachesMemoryCallback()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord base{};
    base.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    base.blockId = 6;
    base.phyCoreId = 8;
    base.launchId = 30;
    base.deviceId = 3;

    aclsan::ParsedTraceRecord loopSize = base;
    loopSize.record.instrId = 125;
    loopSize.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loopSize.record.args[0] = UINT64_C(2) | (UINT64_C(3) << 21U);

    aclsan::ParsedTraceRecord loop1Stride = base;
    loop1Stride.record.instrId = 126;
    loop1Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop1Stride.record.args[0] = UINT64_C(0x400) | (UINT64_C(0x20) << 40U);

    aclsan::ParsedTraceRecord loop2Stride = base;
    loop2Stride.record.instrId = 127;
    loop2Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop2Stride.record.args[0] = UINT64_C(0x2000) | (UINT64_C(0x40) << 40U);

    aclsan::ParsedTraceRecord memory = base;
    memory.record.instrId = 83;
    memory.record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    memory.record.args[0] = 0x7000;
    memory.record.args[1] = 0x2000;
    memory.record.args[2] = (UINT64_C(2) << 4U) | (UINT64_C(32) << 25U);
    memory.record.args[3] = UINT64_C(64) | (UINT64_C(96) << 40U);

    auto ctrl = base;
    ctrl.record.instrId = 159;
    ctrl.record.args[0] = 0;
    aclsan::DispatchTraceRecords({ctrl, loopSize, loop1Stride, loop2Stride, memory}, *decoder);

    assert(g_deviceMemoryCallbackCount == 2);
    const auto& access = g_deviceMemoryCallbacks[0];
    assert(access.address == 0x7000);
    assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(access.layout.ndAffine.rank == 3);
    assert(access.layout.ndAffine.elementBytes == 32);
    assert(access.layout.ndAffine.dims[0] == 2 && access.layout.ndAffine.strides[0] == 64);
    assert(access.layout.ndAffine.dims[1] == 2 && access.layout.ndAffine.strides[1] == 0x400);
    assert(access.layout.ndAffine.dims[2] == 3 && access.layout.ndAffine.strides[2] == 0x2000);
    const auto& local = g_deviceMemoryCallbacks[1];
    assert(local.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
    assert(local.address == 0x2000 && local.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(local.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(local.layout.ndAffine.elementBytes == 32);
    assert(local.layout.ndAffine.dims[0] == 2 && local.layout.ndAffine.strides[0] == 96);
    assert(local.layout.ndAffine.dims[1] == 2 && local.layout.ndAffine.strides[1] == 0x20);
    assert(local.layout.ndAffine.dims[2] == 3 && local.layout.ndAffine.strides[2] == 0x40);
    assert(local.accessCount == 2 && local.header.instrExecId == access.header.instrExecId);
}

void TestGmToL1OuterLoopStateReachesMemoryCallback()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord base{};
    base.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    base.blockId = 7;
    base.phyCoreId = 9;
    base.launchId = 31;
    base.deviceId = 3;

    aclsan::ParsedTraceRecord loopSize = base;
    loopSize.record.instrId = 394;
    loopSize.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loopSize.record.args[0] = UINT64_C(2) | (UINT64_C(3) << 21U);

    aclsan::ParsedTraceRecord loop1Stride = base;
    loop1Stride.record.instrId = 395;
    loop1Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop1Stride.record.args[0] = UINT64_C(0x400) | (UINT64_C(0x20) << 40U);

    aclsan::ParsedTraceRecord loop2Stride = base;
    loop2Stride.record.instrId = 396;
    loop2Stride.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop2Stride.record.args[0] = UINT64_C(0x2000) | (UINT64_C(0x40) << 40U);

    aclsan::ParsedTraceRecord memory = base;
    memory.record.instrId = 73;
    memory.record.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    memory.record.args[0] = 0x3000;
    memory.record.args[1] = 0x8000;
    memory.record.args[2] = (UINT64_C(2) << 4U) | (UINT64_C(2) << 25U);
    memory.record.args[3] = 3;

    aclsan::DispatchTraceRecords({loopSize, loop1Stride, loop2Stride, memory}, *decoder);

    assert(g_deviceMemoryCallbackCount == 2);
    const auto& access = g_deviceMemoryCallbacks[0];
    assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(access.address == 0x8000);
    assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    assert(access.header.sourceKind == ACLSAN_DEVICE_SOURCE_MTE2);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(access.layout.ndAffine.rank == 3);
    assert(access.layout.ndAffine.elementBytes == 64);
    assert(access.layout.ndAffine.dims[0] == 2 && access.layout.ndAffine.strides[0] == 96);
    assert(access.layout.ndAffine.dims[1] == 2 && access.layout.ndAffine.strides[1] == 0x400);
    assert(access.layout.ndAffine.dims[2] == 3 && access.layout.ndAffine.strides[2] == 0x2000);
    const auto& local = g_deviceMemoryCallbacks[1];
    assert(local.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L1);
    assert(local.address == 0x3000);
    assert(local.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    assert(local.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(local.layout.ndAffine.elementBytes == 64);
    assert(local.layout.ndAffine.dims[0] == 2 && local.layout.ndAffine.strides[0] == 64);
    assert(local.layout.ndAffine.dims[1] == 2 && local.layout.ndAffine.strides[1] == 0x20);
    assert(local.layout.ndAffine.dims[2] == 3 && local.layout.ndAffine.strides[2] == 0x40);
    assert(local.header.instrExecId == access.header.instrExecId);
}

void TestFixpipeLoop3StateReachesMemoryCallback()
{
    ResetCapture();
    const aclsan::DeviceInstructionDecoder* decoder =
        aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    assert(decoder != nullptr);

    aclsan::ParsedTraceRecord base{};
    base.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    base.blockId = 8;
    base.phyCoreId = 10;
    base.launchId = 32;
    base.deviceId = 3;

    aclsan::ParsedTraceRecord loop3 = base;
    loop3.record.instrId = 90;
    loop3.record.category = aclsan::DeviceInstructionCategory::RegisterState;
    loop3.record.args[0] = UINT64_C(2) | (UINT64_C(7) << 16U) | (UINT64_C(100) << 32U);

    aclsan::ParsedTraceRecord memory = base;
    memory.record.instrId = 91;
    memory.record.pipeline = ACLSAN_DEVICE_PIPE_FIXPIPE;
    memory.record.args[0] = 0xa000;
    memory.record.args[1] = 0x4000;
    memory.record.args[2] = (UINT64_C(32) << 4U) | (UINT64_C(3) << 16U) | (UINT64_C(40) << 32U);
    memory.record.args[3] = UINT64_C(1) << 43U;

    auto ctrl = base;
    ctrl.record.instrId = 159;
    ctrl.record.args[0] = 0;
    aclsan::DispatchTraceRecords({ctrl, loop3, memory}, *decoder);

    assert(g_deviceMemoryCallbackCount == 3);
    const auto& access = g_deviceMemoryCallbacks[0];
    assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    assert(access.address == 0xa000);
    assert(access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    assert(access.header.sourceKind == ACLSAN_DEVICE_SOURCE_FIXPIPE);
    assert(access.layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    assert(access.layout.ndAffine.rank == 2);
    assert(access.layout.ndAffine.elementBytes == 128);
    assert(access.layout.ndAffine.dims[0] == 3 && access.layout.ndAffine.strides[0] == 160);
    assert(access.layout.ndAffine.dims[1] == 2 && access.layout.ndAffine.strides[1] == 400);
    for (size_t i = 1; i < 3; ++i) {
        const auto& local = g_deviceMemoryCallbacks[i];
        assert(local.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0C);
        assert(local.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        assert(local.address == 0x4000 + (i - 1) * 7 * 64);
        assert(local.header.instrExecId == access.header.instrExecId);
    }
}

void TestDisabledCallbackIsNotInvoked()
{
    ResetCapture();
    g_callbackEnabled = false;
    const AclsanResourceData callbackData{};

    aclsan::AclsanCallbackDispatcher::DispatchResource(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, callbackData);
    assert(g_callbackCapture.calls == 0);
}

void TestLaunchCallbackData()
{
    ResetCapture();
    const aclrtBinHandle binary = Address(0x88770000U);
    const aclrtFuncHandle function = Address(0x12345678U);
    const aclrtStream stream = Address(0x45670000U);
    aclsan::RecordTraceBinaryLoadFromData(binary, false, 0, nullptr, 0);
    aclsan::RecordTraceBinaryFunctionLookup(binary, function, "mix_kernel");

    assert(aclrtLaunchKernelWithHostArgsHook(function, 8, stream, nullptr, nullptr, 0, nullptr, 0) == ACL_SUCCESS);
    assert(g_callbackCapture.calls == 1);
    assert(g_callbackCapture.domain == ACLSAN_CB_DOMAIN_LAUNCH);
    assert(g_callbackCapture.callbackId == ACLSAN_CBID_LAUNCH_KERNEL);
    CheckCommonData(g_callbackCapture.launch.common, sizeof(AclsanLaunchData), "aclrtLaunchKernelWithHostArgs");
    assert(g_callbackCapture.launch.launchId != 0);
    assert(g_callbackCapture.launch.function == function);
    assert(g_callbackCapture.launch.stream == stream);
    assert(g_callbackCapture.launch.numBlocks == 8);
    assert(g_callbackCapture.launchFunctionName == "mix_kernel");
    assert(aclrtLaunchKernelWithHostArgsHook(function, 17, stream, nullptr, nullptr, 0, nullptr, 0) == ACL_SUCCESS);
    assert(g_callbackCapture.launch.numBlocks == 17);
    assert(g_functionAttributeQueryCalls == 0);
}

aclError FakeGetParamCount(const void*, size_t* count)
{
    *count = 2;
    return ACL_SUCCESS;
}

aclError FakeGetParamInfo(const void*, size_t index, size_t* offset, size_t* size)
{
    assert(index == 1);
    *offset = 24;
    *size = sizeof(void*);
    return ACL_SUCCESS;
}

void TestArgsArrayUsesCallerPointerStorage()
{
    void* deviceBuffer = Address(0x1000);
    uint64_t value = 42;
    void* originalArgs[] = {&value};
    std::vector<void*> launchArgs;
    assert(BuildInstrumentedArgsArray(Address(0x12345678), deviceBuffer, 24, originalArgs, launchArgs) == ACL_SUCCESS);
    assert(launchArgs.size() == 2 && launchArgs[0] == &value);
    assert(originalArgs[0] == &value && value == 42);
    assert(launchArgs[1] == &deviceBuffer);
    deviceBuffer = Address(0x2000);
    void* hiddenPointer = nullptr;
    std::memcpy(&hiddenPointer, launchArgs[1], sizeof(hiddenPointer));
    assert(hiddenPointer == deviceBuffer);
    assert(
        BuildInstrumentedArgsArray(Address(0x12345678), deviceBuffer, 16, originalArgs, launchArgs) ==
        ACL_ERROR_FEATURE_UNSUPPORTED);
}

void TestTraceArgumentModes()
{
    ResetCapture();
    const auto binary = Address(0x88770001);
    const auto function = Address(0x12345679);
    const std::array<uint8_t, 8> image{0x7f, 'E', 'L', 'F', 1, 2, 3, 4};
    aclsan::RecordTraceBinaryLoadFromData(binary, true, 24, image.data(), image.size());
    aclsan::RecordTraceBinaryFunctionLookup(binary, function, "argument_modes");
    aclsan::PreparedTraceLaunch prepared;
    assert(
        aclsan::PrepareTraceLaunch(
            function, 1, nullptr, 0, nullptr, 0, aclsan::TraceArgumentMode::ARGS_ARRAY, prepared) == ACL_SUCCESS);
    assert(prepared.instrumented && prepared.traceArgumentOffset == 24);
    assert(prepared.arguments.empty() && prepared.placeholders.empty());
    assert(prepared.deviceBuffer != nullptr);
    aclsan::CompleteTraceLaunch(std::move(prepared), function, nullptr, ACL_ERROR_FAILURE);
    assert(g_lastFreedAddress == prepared.deviceBuffer);

    // 零参数 HostArgs 仍需生成带对齐填充及隐藏指针的连续参数区。
    assert(
        aclsan::PrepareTraceLaunch(
            function, 1, nullptr, 0, nullptr, 0, aclsan::TraceArgumentMode::HOST_ARGS, prepared) == ACL_SUCCESS);
    assert(prepared.arguments.size() == 24 + sizeof(void*));
    void* hiddenPointer = nullptr;
    std::memcpy(&hiddenPointer, prepared.arguments.data() + 24, sizeof(hiddenPointer));
    assert(hiddenPointer == prepared.deviceBuffer);
    aclsan::CompleteTraceLaunch(std::move(prepared), function, nullptr, ACL_ERROR_FAILURE);
    aclsan::RecordTraceBinaryUnload(binary);
}

} // namespace

namespace aclsan {

// 原 aclsan::InvokeCallback / acltoolGetOriginalRuntimeApi 的测试桩定义：
// 改为普通函数，经 llt/aclsan_boundary_stub 的 BoundaryGuard 安装，
// 避免与 aclsan_api.cpp 的真实实现符号冲突。
bool CaptureInvokeCallback(AclsanCallbackDomain domain, AclsanCallbackId callbackId, const void* callbackData) noexcept
{
    if (!g_callbackEnabled) {
        return true;
    }
    assert(callbackData != nullptr);
    g_callbackCapture.domain = domain;
    g_callbackCapture.callbackId = callbackId;
    ++g_callbackCapture.calls;
    if (domain == ACLSAN_CB_DOMAIN_RESOURCE) {
        g_callbackCapture.resource = *static_cast<const AclsanResourceData*>(callbackData);
    } else if (domain == ACLSAN_CB_DOMAIN_MEMORY) {
        g_callbackCapture.memoryWrite = *static_cast<const AclsanMemoryWriteData*>(callbackData);
    } else if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        g_callbackCapture.synchronize = *static_cast<const AclsanSynchronizeData*>(callbackData);
    } else if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && callbackId == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        assert(g_deviceMemoryCallbackCount < g_deviceMemoryCallbacks.size());
        g_deviceMemoryCallbacks[g_deviceMemoryCallbackCount] =
            *static_cast<const AclsanDeviceMemoryAccessData*>(callbackData);
        ++g_deviceMemoryCallbackCount;
    } else if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && callbackId == ACLSAN_CBID_DEVICE_STATE) {
        assert(g_deviceStateCallbackCount < g_deviceStateCallbacks.size());
        g_deviceStateCallbacks[g_deviceStateCallbackCount] =
            *static_cast<const AclsanDeviceRegisterStateData*>(callbackData);
        ++g_deviceStateCallbackCount;
    } else if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && callbackId == ACLSAN_CBID_DEVICE_SYNC) {
        assert(g_deviceSyncCallbackCount < g_deviceSyncCallbacks.size());
        g_deviceSyncCallbacks[g_deviceSyncCallbackCount] = *static_cast<const AclsanDeviceSyncData*>(callbackData);
        ++g_deviceSyncCallbackCount;
    } else if (domain == ACLSAN_CB_DOMAIN_LAUNCH && callbackId == ACLSAN_CBID_LAUNCH_KERNEL) {
        g_callbackCapture.launch = *static_cast<const AclsanLaunchData*>(callbackData);
        g_callbackCapture.launchFunctionName =
            g_callbackCapture.launch.functionName == nullptr ? "" : g_callbackCapture.launch.functionName;
    }
    return true;
}

} // namespace aclsan

namespace {
aclError FakeAclrtMemcpy(void* destination, size_t capacity, const void* source, size_t bytes, aclrtMemcpyKind)
{
    assert(bytes <= capacity);
    if (g_hostWriteResult != ACL_SUCCESS) {
        return g_hostWriteResult;
    }
    if (g_hostInputAllocation && g_hostInputMemcpyResult != ACL_SUCCESS) {
        return g_hostInputMemcpyResult;
    }
    if (destination == Address(0x12340000U)) {
        return ACL_SUCCESS;
    }
    if (bytes != 0) {
        std::memcpy(destination, source, bytes);
    }
    return ACL_SUCCESS;
}

aclError FakeAclrtMemset(void* destination, size_t capacity, int32_t value, size_t bytes)
{
    EXPECT_LE(bytes, capacity);
    if (g_hostWriteResult == ACL_SUCCESS) {
        std::memset(destination, value, bytes);
    }
    return g_hostWriteResult;
}

void* CaptureGetOriginalRuntimeApi(aclrtApiId apiId)
{
    switch (apiId) {
        case ACL_RT_API_aclrtMalloc:
            if (!g_mallocOriginalAvailable) {
                return nullptr;
            }
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtMalloc));
        case ACL_RT_API_aclrtMallocAlign32:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtMalloc));
        case ACL_RT_API_aclrtFree:
            if (!g_freeOriginalAvailable) {
                return nullptr;
            }
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtFree));
        case ACL_RT_API_aclrtSynchronizeStream:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtSynchronizeStream));
        case ACL_RT_API_aclrtSynchronizeStreamWithTimeout:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtSynchronizeStreamWithTimeout));
        case ACL_RT_API_aclrtMemset:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtMemset));
        case ACL_RT_API_aclrtMemcpy:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtMemcpy));
        case ACL_RT_API_aclrtGetDevice:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtGetDevice));
        case ACL_RT_API_aclrtBinaryGetGlobal:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtBinaryGetGlobal));
        case ACL_RT_API_aclrtGetFunctionAttribute:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtGetFunctionAttribute));
        case ACL_RT_API_aclrtLaunchKernelWithHostArgs:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtLaunchKernelWithHostArgs));
        case ACL_RT_API_aclrtGetSocName:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtGetSocName));
        case ACL_RT_API_aclrtGetDeviceInfo:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeAclrtGetDeviceInfo));
        case ACL_RT_API_aclrtFunctionGetParamCount:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeGetParamCount));
        case ACL_RT_API_aclrtFunctionGetParamInfo:
            return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&FakeGetParamInfo));
        default:
            return nullptr;
    }
}

} // namespace

// 原 13 个 acltoolRegister*/ClearCallback 内联桩已收敛到 llt/aclsan_boundary_stub
// （默认转发注入库真实实现；本用例在 TEST 内经 BoundaryGuard 安装受控版本）。

void TestHostInputMaterialization()
{
    ResetCapture();
    aclsan::PreparedTraceLaunch prepared;
    prepared.instrumented = true;
    prepared.traceArgumentOffset = 16;
    prepared.deviceBuffer = Address(0x12340000U);
    prepared.arguments.assign(40, 0);
    std::fill(prepared.arguments.begin() + 24, prepared.arguments.end(), 0xab);
    prepared.placeholders = {{8, 24}, {0, 24}};
    g_hostInputAllocation = true;
    ASSERT_EQ(aclsan::MaterializeTraceHostInputs(prepared), ACL_SUCCESS);
    ASSERT_EQ(prepared.arguments.size(), 24U);
    ASSERT_TRUE(prepared.placeholders.empty());
    ASSERT_EQ(prepared.hostInputs.size(), 1U);
    ASSERT_EQ(prepared.hostInputs[0].bytes, 32U);
    EXPECT_EQ(g_callbackCapture.calls, 0U);
    void* first = nullptr;
    void* second = nullptr;
    std::memcpy(&first, prepared.arguments.data(), sizeof(first));
    std::memcpy(&second, prepared.arguments.data() + 8, sizeof(second));
    ASSERT_EQ(first, g_hostInputStorage.data());
    ASSERT_EQ(first, second);
    for (size_t i = 0; i < 32; ++i) {
        ASSERT_EQ(g_hostInputStorage[i], i < 16 ? 0xab : 0);
    }
    aclsan::CompleteTraceLaunch(std::move(prepared), nullptr, nullptr, ACL_ERROR_FAILURE);
    ASSERT_EQ(g_lastFreedAddress, g_hostInputStorage.data());
    EXPECT_EQ(g_callbackCapture.calls, 0U);

    aclsan::PreparedTraceLaunch failed;
    failed.instrumented = true;
    failed.traceArgumentOffset = 16;
    failed.arguments.assign(40, 0xcd);
    failed.placeholders = {{0, 24}, {8, 16}};
    g_lastFreedAddress = nullptr;
    ASSERT_EQ(aclsan::MaterializeTraceHostInputs(failed), ACL_ERROR_INVALID_PARAM);
    ASSERT_TRUE(failed.hostInputs.empty());
    ASSERT_EQ(g_lastFreedAddress, g_hostInputStorage.data());

    aclsan::PreparedTraceLaunch mallocFailed;
    mallocFailed.instrumented = true;
    mallocFailed.traceArgumentOffset = 16;
    mallocFailed.arguments.assign(40, 0xef);
    mallocFailed.placeholders = {{0, 24}};
    g_lastFreedAddress = nullptr;
    g_hostInputMallocResult = ACL_ERROR_BAD_ALLOC;
    ASSERT_EQ(aclsan::MaterializeTraceHostInputs(mallocFailed), ACL_ERROR_BAD_ALLOC);
    ASSERT_TRUE(mallocFailed.hostInputs.empty());
    ASSERT_EQ(g_lastFreedAddress, nullptr);
    g_hostInputMallocResult = ACL_SUCCESS;

    aclsan::PreparedTraceLaunch copyFailed;
    copyFailed.instrumented = true;
    copyFailed.traceArgumentOffset = 16;
    copyFailed.arguments.assign(40, 0xef);
    copyFailed.placeholders = {{0, 24}};
    g_lastFreedAddress = nullptr;
    g_hostInputMemcpyResult = ACL_ERROR_FAILURE;
    ASSERT_EQ(aclsan::MaterializeTraceHostInputs(copyFailed), ACL_ERROR_FAILURE);
    ASSERT_TRUE(copyFailed.hostInputs.empty());
    ASSERT_EQ(g_lastFreedAddress, g_hostInputStorage.data());
    g_hostInputMemcpyResult = ACL_SUCCESS;
    g_hostInputAllocation = false;
    EXPECT_EQ(g_callbackCapture.calls, 0U);
}

void TestParameterAccessStaysInternal()
{
    ResetCapture();
    const auto* decoder = aclsan::FindDeviceInstructionDecoder(aclsan::SocVersion::DAV_3510);
    ASSERT_NE(decoder, nullptr);
    aclsan::ParsedTraceRecord record{};
    record.launchId = 101;
    record.record.instrId = static_cast<uint32_t>(aclsan::InstructionId::LdDevB64);
    record.record.category = aclsan::DeviceInstructionCategory::MemoryAccess;
    record.record.parameterBase = 0x4000;
    record.parameterBytes = 32;
    record.record.args[0] = 0x4018;
    aclsan::DispatchTraceRecords({record}, *decoder);
    EXPECT_EQ(g_deviceMemoryCallbackCount, 0U);
    EXPECT_EQ(g_callbackCapture.calls, 0U);
    record.record.args[0] = 0x4019;
    aclsan::DispatchTraceRecords({record}, *decoder);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 1U);
    EXPECT_EQ(g_deviceMemoryCallbacks[0].address, 0x4019U);
    // A later launch must not inherit the previous launch's parameter range.
    record.launchId = 102;
    record.record.parameterBase = 0x8000;
    record.record.args[0] = 0x4018;
    aclsan::DispatchTraceRecords({record}, *decoder);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 2U);
    EXPECT_EQ(g_deviceMemoryCallbacks[1].address, 0x4018U);
}

TEST(AclsanHookCbdata, Main)
{
    aclsan_test::Boundary boundary;
    boundary.getOriginalRuntimeApi = &CaptureGetOriginalRuntimeApi;
    boundary.invokeCallback = &aclsan::CaptureInvokeCallback;
    boundary.clearRuntimeCallback = [](aclrtApiId) { return g_clearCallbackResult; };
    boundary.registerRuntimeCallback = [](aclrtApiId id, void*) {
        return id == ACL_RT_API_aclrtMalloc ? g_registerMallocResult : 0;
    };
    const aclsan_test::BoundaryGuard boundaryGuard{boundary};
    TestParameterAccessStaysInternal();
    TestHostInputMaterialization();
    TestMallocCallbackData();
    TestMallocCallbackDataRoundsSizeUpTo32Bytes();
    TestMallocAlign32CallbackData();
    TestMallocPreservesOriginalRuntimeError();
    TestMallocSkipsCallbackWhenGetDeviceFails();
    TestMissingOriginalMallocAborts();
    TestFreeCallbackData();
    TestMissingOriginalFreeAborts();
    TestRuntimeHookRegistrationFailureAborts();
    TestRuntimeHookClearingFailureAborts();
    TestSynchronizeStreamCallbackData();
    TestSynchronizeStreamWithTimeoutCallbackData();
    TestSetPaddingRecordsUpdateLaunchStateAndCallback();
    TestRegisterStateCallbackPreservesPackedValue();
    TestSetL12DDoesNotPublishPersistentRegisterState();
    TestRegisterStatePersistsAcrossTraceSnapshots();
    TestUndefinedInstructionIdsSkipDecoder();
    TestDefinedInstructionIdUsesDecoder();
    TestCubeLoadAndUnsupportedModeReachMemoryCallback();
    TestNdDmaPadCountStatePreservesExactGmFootprint();
    TestDmaOuterLoopStateReachesMemoryCallback();
    TestUbufToGmOuterLoopStateReachesMemoryCallback();
    TestGmToL1OuterLoopStateReachesMemoryCallback();
    TestFixpipeLoop3StateReachesMemoryCallback();
    TestLaunchCallbackData();
    TestArgsArrayUsesCallerPointerStorage();
    TestTraceArgumentModes();
    TestDisabledCallbackIsNotInvoked();
}

TEST(AclsanHookCbdata, L1ToBtAndFbDispatchSourceReads)
{
    aclsan_test::Boundary boundary;
    boundary.invokeCallback = &aclsan::CaptureInvokeCallback;
    const aclsan_test::BoundaryGuard boundaryGuard{boundary};
    const auto& decoder = aclsan::dav3510::GetDeviceInstructionDecoder();
    for (uint32_t id : {167u, 423u, 424u, 425u, 426u}) {
        SCOPED_TRACE(id);
        ResetCapture();
        aclsan::ParsedTraceRecord parsed{};
        parsed.record.instrId = id;
        parsed.record.pipeline = id == 167 ? ACLSAN_DEVICE_PIPE_FIXPIPE : ACLSAN_DEVICE_PIPE_MTE1;
        parsed.record.pc = 0x5678;
        parsed.record.args[1] = 0x7ff00;
        parsed.record.args[2] = (1ULL << 4) | (2ULL << 16);
        parsed.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
        aclsan::DispatchTraceRecords({parsed}, decoder);
        ASSERT_EQ(g_deviceMemoryCallbackCount, 1u);
        const auto& access = g_deviceMemoryCallbacks[0];
        EXPECT_EQ(access.header.pc, 0x5678u);
        EXPECT_EQ(access.memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_L1);
        EXPECT_EQ(access.accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        EXPECT_EQ(access.address, 0x7ff00u);
        EXPECT_EQ(access.layout.range.bytes, id == 167 ? 128u : 64u);
    }
}

TEST(AclsanHookCbdata, AtomicCtrlReplayPersistsAcrossSnapshotsAndRestoresMode)
{
    aclsan_test::Boundary boundary;
    boundary.invokeCallback = &aclsan::CaptureInvokeCallback;
    const aclsan_test::BoundaryGuard boundaryGuard{boundary};
    ResetCapture();
    const auto& decoder = aclsan::dav3510::GetDeviceInstructionDecoder();
    aclsan::dav3510::Dav3510RegisterStateManager state(99);
    aclsan::ParsedTraceRecord base{};
    base.launchId = 99;
    base.phyCoreId = 2;
    base.deviceId = 1;
    base.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    auto ctrl = base;
    ctrl.record.instrId = 159;
    ctrl.record.args[0] = 64; // f32 加法
    auto loops = base;
    loops.record.instrId = 125; // set_loop_size_ubtoout
    loops.record.args[0] = 1 | (UINT64_C(1) << 21U);
    auto memory = base;
    memory.record.instrId = 83;
    memory.record.pipeline = ACLSAN_DEVICE_PIPE_MTE3;
    memory.record.args[0] = 0x4000;
    memory.record.args[1] = 0;
    memory.record.args[2] = (UINT64_C(1) << 4U) | (UINT64_C(32) << 25U);
    aclsan::DispatchTraceRecords({ctrl, loops}, decoder, nullptr, &state);
    EXPECT_EQ(g_deviceMemoryCallbackCount, 0U);
    EXPECT_EQ(g_deviceSyncCallbackCount, 0U);
    aclsan::DispatchTraceRecords({memory}, decoder, nullptr, &state);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 2U);
    EXPECT_EQ(g_deviceMemoryCallbacks[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    EXPECT_NE(g_deviceMemoryCallbacks[0].regDependencyMask0 & (UINT64_C(1) << ACLSAN_DEVICE_REGISTER_CTRL), 0U);
    ctrl.record.args[0] = UINT64_C(2) << 9U; // none，但保留旧的 min 位
    aclsan::DispatchTraceRecords({ctrl, memory}, decoder, nullptr, &state);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 4U);
    EXPECT_EQ(g_deviceMemoryCallbacks[2].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    ctrl.record.args[0] = 64; // set_ctrl(savedCtrl)
    aclsan::DispatchTraceRecords({ctrl, memory}, decoder, nullptr, &state);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 6U);
    EXPECT_EQ(g_deviceMemoryCallbacks[4].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    EXPECT_EQ(g_deviceMemoryCallbacks[0].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD);
    ctrl.record.args[0] = 64 | (UINT64_C(1) << 9U); // max 替换 add
    aclsan::DispatchTraceRecords({ctrl, memory}, decoder, nullptr, &state);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 8U);
    EXPECT_EQ(g_deviceMemoryCallbacks[6].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX);
    EXPECT_EQ(g_deviceMemoryCallbacks[6].header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    ctrl.record.args[0] = 64 | (UINT64_C(2) << 9U); // min 替换 max
    aclsan::DispatchTraceRecords({ctrl, memory}, decoder, nullptr, &state);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 10U);
    EXPECT_EQ(g_deviceMemoryCallbacks[8].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN);
    EXPECT_EQ(g_deviceMemoryCallbacks[8].header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    memory.phyCoreId = 3;
    loops.phyCoreId = 3;
    aclsan::DispatchTraceRecords({loops, memory}, decoder, nullptr, &state);
    ASSERT_EQ(g_deviceMemoryCallbackCount, 12U);
    EXPECT_EQ(g_deviceMemoryCallbacks[10].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    EXPECT_EQ(g_deviceMemoryCallbacks[10].header.flags, ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    EXPECT_EQ(g_deviceSyncCallbackCount, 0U);
    for (size_t i = 1; i < g_deviceMemoryCallbackCount; i += 2) {
        EXPECT_EQ(g_deviceMemoryCallbacks[i].memorySpace, ACLSAN_DEVICE_MEMORY_SPACE_UB);
        EXPECT_EQ(g_deviceMemoryCallbacks[i].accessMode, ACLSAN_DEVICE_MEMORY_ACCESS_READ);
        EXPECT_EQ(g_deviceMemoryCallbacks[i].accessCount, 2U);
    }
}

TEST(AclsanHookCbdata, HostWritesReportCompletionAndExcludeOtherCopyDirections)
{
    aclsan_test::Boundary boundary;
    boundary.getOriginalRuntimeApi = &CaptureGetOriginalRuntimeApi;
    boundary.invokeCallback = &aclsan::CaptureInvokeCallback;
    const aclsan_test::BoundaryGuard boundaryGuard{boundary};
    // -1 selects memset; the other cases exercise actual memcpy directions.
    for (const int operation :
         {-1, int(ACL_MEMCPY_HOST_TO_DEVICE), int(ACL_MEMCPY_DEVICE_TO_HOST), int(ACL_MEMCPY_DEVICE_TO_DEVICE),
          int(ACL_MEMCPY_HOST_TO_HOST)}) {
        for (int failure = 0; failure < 3; ++failure) {
            SCOPED_TRACE(::testing::Message() << "operation=" << operation << " failure=" << failure);
            ResetCapture();
            g_hostWriteResult = failure == 1 ? ACL_ERROR_INVALID_PARAM : ACL_SUCCESS;
            g_getDeviceResult = failure == 2 ? ACL_ERROR_FAILURE : ACL_SUCCESS;
            std::array<uint8_t, 8> destination{};
            const std::array<uint8_t, 4> source{7, 7, 7, 7};
            const auto result = operation == -1 ?
                                    aclrtMemsetHook(destination.data(), destination.size(), 7, source.size()) :
                                    aclrtMemcpyHook(
                                        destination.data(), destination.size(), source.data(), source.size(),
                                        static_cast<aclrtMemcpyKind>(operation));
            EXPECT_EQ(result, g_hostWriteResult);
            EXPECT_EQ(destination[0], failure == 1 ? 0 : 7);
            EXPECT_EQ(destination[4], 0); // capacity must not replace the actual write length.
            const bool reportsWrite = operation == -1 || operation == ACL_MEMCPY_HOST_TO_DEVICE;
            ASSERT_EQ(g_callbackCapture.calls, reportsWrite ? 1U : 0U);
            if (!reportsWrite) {
                continue;
            }
            EXPECT_EQ(g_callbackCapture.domain, ACLSAN_CB_DOMAIN_MEMORY);
            EXPECT_EQ(g_callbackCapture.callbackId, ACLSAN_CBID_MEMORY_WRITE_END);
            const auto& data = g_callbackCapture.memoryWrite;
            EXPECT_EQ(data.common.version, ACLSAN_API_VERSION);
            EXPECT_EQ(data.common.size, sizeof(data));
            EXPECT_EQ(data.common.result, failure == 2 ? ACL_ERROR_FAILURE : g_hostWriteResult);
            EXPECT_EQ(data.destination, destination.data());
            EXPECT_EQ(data.bytes, source.size());
            if (failure != 2) {
                EXPECT_EQ(data.deviceId, 3U);
            }
        }
    }
    ResetCapture();
}
