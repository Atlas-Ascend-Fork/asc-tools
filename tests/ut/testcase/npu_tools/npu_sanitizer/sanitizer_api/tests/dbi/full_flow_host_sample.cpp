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
#include "acl_san/aclsan_cbdata_device.h"
#include "dbi/trace_buffer_abi.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"
#include "kernel_argument_elf_fixture.h"

#include <acl/acl_rt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#define CHECK(expression)                                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return 1;                                                                     \
        }                                                                                 \
    } while (false)

const auto kBinary = reinterpret_cast<aclrtBinHandle>(0x31);
const auto kFunction = reinterpret_cast<aclrtFuncHandle>(0x41);
const auto kOrdinaryBinary = reinterpret_cast<aclrtBinHandle>(0x32);
const auto kOrdinaryFunction = reinterpret_cast<aclrtFuncHandle>(0x42);
const auto kStream = reinterpret_cast<aclrtStream>(0x51);

struct CapturedRecord {
    uint32_t blockId;
    uint32_t blockType;
    uint32_t phyCoreId;
    uint32_t pipeline;
    uint32_t siteId;
    uint64_t pc;
};

struct SampleState {
    size_t allocations = 0;
    size_t frees = 0;
    size_t h2dCalls = 0;
    size_t d2hCalls = 0;
    std::vector<CapturedRecord> callbacks;
};

struct RecordFixture {
    uint64_t pc;
    uint32_t pipeline;
    uint32_t siteId;
    uint32_t instrId;
    uint64_t srcPipe;
    uint64_t dstPipe;
    uint64_t eventId;
};

constexpr RecordFixture kRecords[] = {
    {0x1000, ACLSAN_DEVICE_PIPE_SCALAR, 10, 440, 2, 3, 100},
    {0x2000, ACLSAN_DEVICE_PIPE_SCALAR, 20, 442, 3, 2, 200},
};

SampleState g_state;

aclError OriginalBinaryLoad(const void* data, size_t length, const aclrtBinaryLoadOptions*, aclrtBinHandle* binary)
{
    if (data == nullptr || length == 0 || binary == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *binary = kBinary;
    std::puts("[intercept] binary_load");
    std::puts("[dbi] patched=yes backend=simulated");
    return ACL_SUCCESS;
}

aclError OriginalBinaryGetFunction(aclrtBinHandle binary, const char* name, aclrtFuncHandle* function)
{
    if (name == nullptr || function == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (binary == kBinary && std::strcmp(name, "FullFlowKernel") == 0) {
        *function = kFunction;
        return ACL_SUCCESS;
    }
    if (binary == kOrdinaryBinary && std::strcmp(name, "OrdinaryKernel") == 0) {
        *function = kOrdinaryFunction;
        return ACL_SUCCESS;
    }
    return ACL_ERROR_INVALID_PARAM;
}

aclError OriginalMalloc(void** pointer, size_t bytes, aclrtMemMallocPolicy)
{
    if (pointer == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    ++g_state.allocations;
    *pointer = std::malloc(bytes);
    return *pointer == nullptr ? ACL_ERROR_BAD_ALLOC : ACL_SUCCESS;
}

aclError OriginalFree(void* pointer)
{
    ++g_state.frees;
    std::free(pointer);
    return ACL_SUCCESS;
}

aclError OriginalMemcpy(void* dst, size_t dstMax, const void* src, size_t bytes, aclrtMemcpyKind kind)
{
    if (dst == nullptr || src == nullptr || bytes > dstMax) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (kind == ACL_MEMCPY_HOST_TO_DEVICE) {
        ++g_state.h2dCalls;
    } else if (kind == ACL_MEMCPY_DEVICE_TO_HOST) {
        ++g_state.d2hCalls;
    }
    std::memcpy(dst, src, bytes);
    return ACL_SUCCESS;
}

aclError OriginalLaunch(
    aclrtFuncHandle function, uint32_t blocks, aclrtStream stream, aclrtLaunchKernelCfg*, void* hostArgs,
    size_t argsSize, aclrtPlaceHolderInfo*, size_t)
{
    if (function == kOrdinaryFunction) {
        return stream == kStream && hostArgs != nullptr && argsSize == 16 ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM;
    }
    if (function != kFunction || stream != kStream || blocks != 2 || hostArgs == nullptr || argsSize != 24) {
        std::fprintf(stderr, "unexpected launch arguments: blocks=%u args_size=%zu\n", blocks, argsSize);
        return ACL_ERROR_INVALID_PARAM;
    }
    void* traceBuffer = nullptr;
    std::memcpy(&traceBuffer, static_cast<const unsigned char*>(hostArgs) + 16, sizeof(traceBuffer));
    if (traceBuffer == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    std::puts("[hook] function instrumented=yes");
    std::puts("[hook] launch trace_buffer_injected=yes");

    auto* bytes = static_cast<uint8_t*>(traceBuffer);
    auto* header = reinterpret_cast<aclsan::AclsanTraceBufferHeader*>(bytes);
    if (header->magic != aclsan::ASCSAN_TRACE_BUFFER_MAGIC || header->blockCount != blocks ||
        header->physicalCoreSlotCount != aclsan::ASCSAN_PHYSICAL_CORE_SLOT_COUNT) {
        return ACL_ERROR_INVALID_PARAM;
    }

    constexpr uint32_t blockIds[] = {0U, 1U};
    constexpr uint32_t phyCoreIds[] = {0U, aclsan::ASCSAN_AIC_CORES_PER_DIE};
    for (uint32_t recordIndex = 0; recordIndex < 2; ++recordIndex) {
        auto* slice = reinterpret_cast<aclsan::AclsanTraceSliceHeader*>(
            bytes + sizeof(*header) +
            static_cast<size_t>(phyCoreIds[recordIndex]) * aclsan::ASCSAN_TRACE_BYTES_PER_CORE);
        auto* record =
            reinterpret_cast<aclsan::AclsanRawTraceRecord*>(reinterpret_cast<uint8_t*>(slice) + sizeof(*slice));
        record->pc = kRecords[recordIndex].pc;
        record->args[0] = kRecords[recordIndex].srcPipe;
        record->args[1] = kRecords[recordIndex].dstPipe;
        record->args[2] = kRecords[recordIndex].eventId;
        record->args[3] = 0;
        record->args[4] = 0;
        record->instrId = kRecords[recordIndex].instrId;
        record->siteId = kRecords[recordIndex].siteId;
        record->category = aclsan::DeviceInstructionCategory::Synchronization;
        record->pipeline = static_cast<uint16_t>(kRecords[recordIndex].pipeline);
        record->blockId = blockIds[recordIndex];
        record->reserved = 0;
        slice->phyCoreId = phyCoreIds[recordIndex];
        slice->recordCount = 1;
    }
    std::puts("[device] records=2");
    return ACL_SUCCESS;
}

aclError OriginalSynchronize(aclrtStream stream) { return stream == kStream ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM; }

aclError OriginalUnload(aclrtBinHandle binary) { return binary == kBinary ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM; }

aclError OriginalGetDevice(int32_t* deviceId)
{
    if (deviceId == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *deviceId = 3;
    return ACL_SUCCESS;
}

aclError OriginalGetDeviceInfo(uint32_t deviceId, aclrtDevAttr attr, int64_t* value)
{
    if (deviceId != 3U || value == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (attr == ACL_DEV_ATTR_CUBE_CORE_NUM) {
        *value = 2;
        return ACL_SUCCESS;
    }
    if (attr == ACL_DEV_ATTR_VECTOR_CORE_NUM) {
        *value = 4;
        return ACL_SUCCESS;
    }
    return ACL_ERROR_INVALID_PARAM;
}

void Callback(void*, AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    if (domain != ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION || data == nullptr) {
        return;
    }
    if (id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        const auto* memory = static_cast<const AclsanDeviceMemoryAccessData*>(data);
        g_state.callbacks.push_back(
            {memory->header.blockId, memory->header.blockType, memory->header.phyCoreId, memory->header.pipeline,
             memory->header.siteId, memory->header.pc});
    } else if (id == ACLSAN_CBID_DEVICE_SYNC) {
        const auto* sync = static_cast<const AclsanDeviceSyncData*>(data);
        g_state.callbacks.push_back(
            {sync->header.blockId, sync->header.blockType, sync->header.phyCoreId, ACLSAN_DEVICE_PIPE_SCALAR, 0,
             sync->header.pc});
    }
}

struct Cleanup {
    AclsanSubscriberHandle subscriber = nullptr;
    aclrtBinHandle binary = nullptr;

    ~Cleanup()
    {
        if (binary != nullptr) {
            (void)aclrtBinaryUnLoad(binary);
        }
        if (subscriber != nullptr) {
            (void)aclsanUnsubscribe(subscriber);
        }
    }
};

} // namespace

int main()
{
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &OriginalBinaryLoad) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryGetFunction", &OriginalBinaryGetFunction) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtMalloc", &OriginalMalloc) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtFree", &OriginalFree) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtMemcpy", &OriginalMemcpy) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtLaunchKernelWithHostArgs", &OriginalLaunch) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &OriginalSynchronize) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryUnLoad", &OriginalUnload) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtGetDevice", &OriginalGetDevice) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtGetDeviceInfo", &OriginalGetDeviceInfo) == ACL_SUCCESS);
    CHECK(acltoolHookInit() == ACL_SUCCESS);

    Cleanup cleanup;
    CHECK(aclsanSubscribe(&cleanup.subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(
            1, cleanup.subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, cleanup.subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);

    const std::vector<uint8_t> image = aclsan::test::MakeKernelArgumentSizeElf(16);
    aclrtBinaryLoadOptions options{};
    CHECK(aclrtBinaryLoadFromData(image.data(), image.size(), &options, &cleanup.binary) == ACL_SUCCESS);
    uint64_t arguments[2] = {1, 2};
    aclrtFuncHandle ordinaryFunction = nullptr;
    CHECK(aclrtBinaryGetFunction(kOrdinaryBinary, "OrdinaryKernel", &ordinaryFunction) == ACL_SUCCESS);
    CHECK(
        aclrtLaunchKernelWithHostArgs(
            ordinaryFunction, 2, kStream, nullptr, arguments, sizeof(arguments), nullptr, 0) == ACL_SUCCESS);
    CHECK(g_state.allocations == 0);
    CHECK(g_state.h2dCalls == 0);

    aclrtFuncHandle function = nullptr;
    CHECK(aclrtBinaryGetFunction(cleanup.binary, "FullFlowKernel", &function) == ACL_SUCCESS);

    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, kStream, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(g_state.h2dCalls == 1);
    CHECK(g_state.d2hCalls == 0);
    CHECK(g_state.callbacks.empty());

    CHECK(aclrtSynchronizeStream(kStream) == ACL_SUCCESS);
    CHECK(g_state.d2hCalls == 1);
    CHECK(g_state.callbacks.size() == 2);
    CHECK(g_state.callbacks[0].blockId == 0);
    CHECK(g_state.callbacks[0].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE);
    CHECK(g_state.callbacks[0].phyCoreId == 0);
    CHECK(g_state.callbacks[0].pipeline == ACLSAN_DEVICE_PIPE_SCALAR);
    CHECK(g_state.callbacks[0].siteId == 0);
    CHECK(g_state.callbacks[0].pc == 0x1000);
    CHECK(g_state.callbacks[1].blockId == 1);
    CHECK(g_state.callbacks[1].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
    CHECK(g_state.callbacks[1].phyCoreId == aclsan::ASCSAN_AIC_CORES_PER_DIE);
    CHECK(g_state.callbacks[1].pipeline == ACLSAN_DEVICE_PIPE_SCALAR);
    CHECK(g_state.callbacks[1].siteId == 0);
    CHECK(g_state.callbacks[1].pc == 0x2000);
    CHECK(g_state.allocations == g_state.frees);

    std::puts("[d2h] copies=1");
    std::puts("[callback] records=2");
    std::puts("[verify] kernel_result=simulated trace_records=pass resources=balanced");
    std::puts("FULL_FLOW_SAMPLE_PASS");
    return 0;
}
