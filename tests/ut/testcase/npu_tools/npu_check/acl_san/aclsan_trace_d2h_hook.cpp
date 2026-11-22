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
#include "kernel_argument_elf_fixture.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"
#include "dbi/trace_buffer_abi.h"
#include "securec.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {

#define CHECK(expression)                                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return 1;                                                                     \
        }                                                                                 \
    } while (false)

template <size_t N>
bool StringEquals(const char* value, const char (&expected)[N])
{
    return value != nullptr && std::strncmp(value, expected, N) == 0;
}

struct CapturedRecord {
    uint64_t launchId;
    uint64_t instrExecId;
    uint32_t deviceId;
    uint32_t phyCoreId;
    uint32_t blockId;
    uint32_t blockType;
    uint64_t pc;
    uint32_t srcPipe;
    uint32_t dstPipe;
    uint64_t objectId;
};

struct CapturedMemoryAccess {
    uint64_t address;
    uint64_t bytes;
    uint32_t memorySpace;
    uint32_t accessMode;
    uint32_t layoutKind;
    uint32_t rank;
    uint64_t firstCount;
    uint64_t secondCount;
    uint32_t flags;
};

constexpr uint32_t SCALAR_ATOMIC_TYPES[] = {0, 1, 2, 3, 4, 0, 1, 2, 4};
std::vector<CapturedRecord> g_records;
std::vector<CapturedMemoryAccess> g_memoryAccesses;
size_t g_mallocCalls = 0;
std::atomic<size_t> g_freeCalls{0};
std::atomic<size_t> g_d2hCalls{0};
size_t g_deviceInfoCalls = 0;
bool g_failLaunch = false;
aclError g_syncResult = ACL_SUCCESS;
bool g_writeRecord = true;
bool g_writeMemoryRecord = false;
bool g_writeMultiMemoryRecords = false;
bool g_writeScalarAtomicRecords = false;
uint32_t g_expectedHiddenOffset = 16;
size_t g_expectedLaunchArgumentBytes = 24;
uint32_t g_expectedPlaceholderDataOffset = 0;
uint32_t g_expectedPaddingBegin = 0;
bool g_checkPadding = false;
bool g_expectMaterializedHostInput = false;
size_t g_binaryLoadCalls = 0;
bool g_loadFromFile = false;
std::vector<std::string> g_patchedPaths;

aclError OriginalFileLoad(const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binary)
{
    g_patchedPaths.emplace_back(path);
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> image{std::istreambuf_iterator<char>(input), {}};
    // Simulate File delegating to Data: the outer hook must remain the only DBI owner.
    return aclrtBinaryLoadFromData(image.data(), image.size(), options, binary);
}

const auto kOffset16Binary = reinterpret_cast<aclrtBinHandle>(0x161);
const auto kOffset8Binary = reinterpret_cast<aclrtBinHandle>(0x81);
const auto kOffset24Binary = reinterpret_cast<aclrtBinHandle>(0x241);
const auto kFunction = reinterpret_cast<aclrtFuncHandle>(0x101);
const auto kOneArgumentFunction = reinterpret_cast<aclrtFuncHandle>(0x102);
const auto kZeroArgumentFunction = reinterpret_cast<aclrtFuncHandle>(0x103);
const auto kThreeArgumentFunction = reinterpret_cast<aclrtFuncHandle>(0x104);

aclError OriginalBinaryLoad(const void*, size_t, const aclrtBinaryLoadOptions*, aclrtBinHandle* binary)
{
    if (binary == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    const aclrtBinHandle binaries[] = {kOffset16Binary, kOffset8Binary, kOffset24Binary};
    if (g_binaryLoadCalls >= std::size(binaries)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *binary = binaries[g_binaryLoadCalls++];
    return ACL_SUCCESS;
}

aclError OriginalBinaryGetFunction(aclrtBinHandle binary, const char* name, aclrtFuncHandle* function)
{
    if (name == nullptr || function == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if ((binary == kOffset16Binary && StringEquals(name, "Offset16")) ||
        (binary == kOffset8Binary && StringEquals(name, "Offset8"))) {
        *function = kFunction;
        return ACL_SUCCESS;
    }
    if (binary != kOffset24Binary) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (StringEquals(name, "OneArgument")) {
        *function = kOneArgumentFunction;
    } else if (StringEquals(name, "ZeroArgument")) {
        *function = kZeroArgumentFunction;
    } else if (StringEquals(name, "ThreeArguments")) {
        *function = kThreeArgumentFunction;
    } else {
        return ACL_ERROR_INVALID_PARAM;
    }
    return ACL_SUCCESS;
}

bool LoadInstrumentedFunction(uint32_t argumentSize, const char* name, aclrtFuncHandle* function)
{
    const std::vector<uint8_t> image = aclsan::test::MakeKernelArgumentSizeElf(argumentSize);
    aclrtBinaryLoadOptions options{};
    aclrtBinHandle binary = nullptr;
    if (g_loadFromFile) {
        char path[] = "/tmp/aclsan-d2h-file-XXXXXX";
        const int fd = mkstemp(path);
        if (fd < 0)
            return false;
        const auto bytes = write(fd, image.data(), image.size());
        (void)close(fd);
        const bool loaded = bytes == static_cast<ssize_t>(image.size()) &&
                            aclrtBinaryLoadFromFile(path, &options, &binary) == ACL_SUCCESS;
        (void)unlink(path);
        return loaded && aclrtBinaryGetFunction(binary, name, function) == ACL_SUCCESS;
    }
    return aclrtBinaryLoadFromData(image.data(), image.size(), &options, &binary) == ACL_SUCCESS &&
           aclrtBinaryGetFunction(binary, name, function) == ACL_SUCCESS;
}

aclError OriginalMalloc(void** pointer, size_t bytes, aclrtMemMallocPolicy)
{
    ++g_mallocCalls;
    *pointer = nullptr;
    return posix_memalign(pointer, alignof(aclsan::AclsanTraceBufferHeader), bytes) == 0 ? ACL_SUCCESS :
                                                                                           ACL_ERROR_BAD_ALLOC;
}

aclError OriginalFree(void* pointer)
{
    ++g_freeCalls;
    std::free(pointer);
    return ACL_SUCCESS;
}

aclError OriginalMemcpy(void* dst, size_t dstMax, const void* src, size_t bytes, aclrtMemcpyKind kind)
{
    if (dst == nullptr || src == nullptr || bytes > dstMax) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (kind == ACL_MEMCPY_DEVICE_TO_HOST && bytes == 2 * sizeof(aclsan::AclsanRawTraceRecord)) {
        ++g_d2hCalls;
    }
    return bytes == 0 || memcpy_s(dst, dstMax, src, bytes) == EOK ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM;
}

aclError OriginalLaunch(
    aclrtFuncHandle, uint32_t blocks, aclrtStream, aclrtLaunchKernelCfg*, void* hostArgs, size_t argsSize,
    aclrtPlaceHolderInfo* placeholders, size_t placeholderCount)
{
    if (g_failLaunch) {
        return ACL_ERROR_FAILURE;
    }
    if (hostArgs == nullptr || argsSize != g_expectedLaunchArgumentBytes || blocks != 2 ||
        g_expectedHiddenOffset > argsSize || sizeof(void*) > argsSize - g_expectedHiddenOffset) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (g_expectedPlaceholderDataOffset != 0 && (placeholderCount != 1 || placeholders == nullptr ||
                                                 placeholders[0].dataOffset != g_expectedPlaceholderDataOffset)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (g_expectMaterializedHostInput) {
        if (placeholderCount != 0 || placeholders != nullptr) {
            return ACL_ERROR_INVALID_PARAM;
        }
        void* input = nullptr;
        if (argsSize < sizeof(input) || memcpy_s(&input, sizeof(input), hostArgs, sizeof(input)) != EOK) {
            return ACL_ERROR_INVALID_PARAM;
        }
        if (input == nullptr || *static_cast<const uint64_t*>(input) != 2U) {
            return ACL_ERROR_INVALID_PARAM;
        }
    }

    void* deviceBuffer = nullptr;
    if (g_checkPadding) {
        for (uint32_t offset = g_expectedPaddingBegin; offset < g_expectedHiddenOffset; ++offset) {
            if (static_cast<const uint8_t*>(hostArgs)[offset] != 0) {
                return ACL_ERROR_INVALID_PARAM;
            }
        }
    }
    if (memcpy_s(
            &deviceBuffer, sizeof(deviceBuffer), static_cast<const uint8_t*>(hostArgs) + g_expectedHiddenOffset,
            sizeof(deviceBuffer)) != EOK) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (deviceBuffer == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (!g_writeRecord) {
        return ACL_SUCCESS;
    }

    auto* bytes = static_cast<uint8_t*>(deviceBuffer);
    auto* header = reinterpret_cast<aclsan::AclsanTraceBufferHeader*>(bytes);
    if (header->magic != aclsan::ASCSAN_TRACE_BUFFER_MAGIC || header->blockCount != blocks ||
        header->physicalCoreCount != aclsan::ASCSAN_PHYSICAL_CORE_SLOT_COUNT) {
        return ACL_ERROR_INVALID_PARAM;
    }
    constexpr uint32_t phyCoreId = aclsan::ASCSAN_AIC_CORES_PER_DIE;
    auto* writeSequence = bytes + aclsan::TraceWriteControlOffset(phyCoreId);
    auto* record = reinterpret_cast<aclsan::AclsanRawTraceRecord*>(bytes + aclsan::TraceRecordOffset(phyCoreId, 0));
    constexpr uint32_t RECORD_COUNT = 2;
    if (g_writeScalarAtomicRecords) {
        for (size_t i = 0; i < std::size(SCALAR_ATOMIC_TYPES); ++i) {
            auto& atomic = record[i];
            atomic.pc = 0x2000 + 4 * i;
            atomic.instrId = 427 + i;
            atomic.category = aclsan::DeviceInstructionCategory::MemoryAccess;
            atomic.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
            atomic.blockId = 1;
            atomic.args[i < 4 ? 1 : 0] = 0x2001000;
            atomic.args[i < 4 ? 4 : 2] = SCALAR_ATOMIC_TYPES[i];
            atomic.atomicMetadata = 0x80;
        }
        auto& repeated = record[std::size(SCALAR_ATOMIC_TYPES)];
        repeated = record[1];
        repeated.pc = 0x4000;
        repeated.args[4] = 4;
        auto& store = record[std::size(SCALAR_ATOMIC_TYPES) + 1];
        store.pc = 0x2000;
        store.instrId = 56;
        store.category = aclsan::DeviceInstructionCategory::MemoryAccess;
        store.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        store.blockId = 1;
        store.args[0] = 0x2001000;
        store.args[3] = UINT64_C(0x100000000);
        store.atomicMetadata = 0x1122334f;
        const uint32_t count = std::size(SCALAR_ATOMIC_TYPES) + 2;
        std::memcpy(writeSequence, &count, sizeof(count));
        return ACL_SUCCESS;
    }
    record->pc = 0x1234;
    if (g_writeMultiMemoryRecords) {
        record->args[0] = 2;
        record->instrId = 399;
        record->category = aclsan::DeviceInstructionCategory::RegisterState;
        record->pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        record->blockId = 1;

        auto* multi = record + 1;
        multi->pc = 0x1238;
        multi->args[1] = 0x130000000000ULL;
        multi->args[2] = (64ULL << 4U) | (4ULL << 48U);
        multi->args[3] = 8ULL | (512ULL << 21U);
        multi->instrId = 78;
        multi->siteId = 39;
        multi->category = aclsan::DeviceInstructionCategory::MemoryAccess;
        multi->pipeline = ACLSAN_DEVICE_PIPE_MTE2;
        multi->blockId = 1;
        std::memcpy(writeSequence, &RECORD_COUNT, sizeof(RECORD_COUNT));
        return ACL_SUCCESS;
    }
    if (g_writeMemoryRecord) {
        // DMA loops must be initialized before the GM-to-UB access.
        auto* state = record;
        state->instrId = 128;
        state->args[0] = 1ULL | (1ULL << 21U);
        state->category = aclsan::DeviceInstructionCategory::RegisterState;
        state->pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        state->blockId = 1;
        record++;
        record->pc = 0x1234;
        record->args[0] = 0;
        record->args[1] = 0x120000025000ULL;
        record->args[2] = (1ULL << 4) | (8256ULL << 25);
        record->args[3] = 0;
        record->args[4] = 0;
        record->instrId = 86;
        record->siteId = 38;
        record->category = aclsan::DeviceInstructionCategory::MemoryAccess;
        record->pipeline = ACLSAN_DEVICE_PIPE_MTE2;
    } else {
        record->args[0] = 2;
        record->args[1] = 3;
        record->args[2] = 7;
        record->args[3] = 0;
        record->args[4] = 0;
        record->instrId = 440;
        record->siteId = 37;
        record->category = aclsan::DeviceInstructionCategory::Synchronization;
        record->pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    }
    record->blockId = 1;
    record->atomicMetadata = 0;
    if (!g_writeMemoryRecord) {
        auto* second = record + 1;
        second->pc = 0x2234;
        second->args[0] = 2;
        second->args[1] = 3;
        second->args[2] = 8;
        second->args[3] = 0;
        second->args[4] = 0;
        second->instrId = 440;
        second->siteId = 38;
        second->category = aclsan::DeviceInstructionCategory::Synchronization;
        second->pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
        second->blockId = 1;
        second->atomicMetadata = 0;
    }
    std::memcpy(writeSequence, &RECORD_COUNT, sizeof(RECORD_COUNT));
    return ACL_SUCCESS;
}

aclError OriginalSync(aclrtStream) { return g_syncResult; }

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
    ++g_deviceInfoCalls;
    if (attr == ACL_DEV_ATTR_CUBE_CORE_NUM) {
        *value = 36;
        return ACL_SUCCESS;
    }
    if (attr == ACL_DEV_ATTR_VECTOR_CORE_NUM) {
        *value = aclsan::ASCSAN_PHYSICAL_CORE_SLOT_COUNT - 36U;
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
        const auto* access = static_cast<const AclsanDeviceMemoryAccessData*>(data);
        uint32_t rank = 0;
        uint64_t firstCount = 0;
        uint64_t secondCount = 0;
        uint64_t bytes = 0;
        if (access->layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE) {
            rank = access->layout.ndAffine.rank;
            firstCount = access->layout.ndAffine.dims[0];
            secondCount = access->layout.ndAffine.dims[1];
            bytes = access->layout.ndAffine.elementBytes;
        } else if (access->layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT) {
            bytes = access->layout.blockRepeat.blockSize;
        } else if (access->layoutKind == ACLSAN_MEM_LAYOUT_RANGE) {
            bytes = access->layout.range.bytes;
        }
        g_memoryAccesses.push_back(
            {access->address, bytes, access->memorySpace, access->accessMode, access->layoutKind, rank, firstCount,
             secondCount, access->header.flags});
        return;
    }
    if (id == ACLSAN_CBID_DEVICE_SYNC) {
        const auto* sync = static_cast<const AclsanDeviceSyncData*>(data);
        g_records.push_back(
            {sync->header.launchId, sync->header.instrExecId, sync->header.deviceId, sync->header.executor.phyCoreId,
             sync->header.executor.blockId, sync->header.executor.blockType, sync->header.pc, sync->srcPipe,
             sync->dstPipe, sync->objectId});
    }
}

} // namespace

aclError g_arrayCountStatus = ACL_SUCCESS;
bool g_zeroMetadataCount = false;
bool g_badHiddenSize = false;
size_t g_arrayInfoCalls = 0;

aclError OriginalGetParamCount(const void* function, size_t* count)
{
    *count =
        g_zeroMetadataCount ? 0 : (function == kZeroArgumentFunction ? 1 : (function == kOneArgumentFunction ? 2 : 4));
    return g_arrayCountStatus;
}

aclError g_arrayMetadataStatus = ACL_SUCCESS;
bool g_badHiddenOffset = false;
size_t g_arrayOriginalCalls = 0;

aclError OriginalGetParamInfo(const void* function, size_t index, size_t* offset, size_t* size)
{
    size_t count = 0;
    OriginalGetParamCount(function, &count);
    ++g_arrayInfoCalls;
    if (count == 0 || index != count - 1) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *offset = g_badHiddenOffset ? 16 : 24;
    *size = g_badHiddenSize ? 4 : sizeof(uint64_t);
    return g_arrayMetadataStatus;
}

aclError OriginalArrayLaunch(
    void* function, uint32_t blocks, aclrtStream stream, aclrtLaunchKernelCfg* config, void** args)
{
    ++g_arrayOriginalCalls;
    size_t count = 0;
    OriginalGetParamCount(function, &count);
    uint64_t packed[4]{};
    if (args == nullptr || count == 0 || count > std::size(packed)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    for (size_t index = 0; index + 1 < count; ++index) {
        if (args[index] == nullptr) {
            return 75;
        }
        if (memcpy_s(&packed[index], sizeof(packed[index]), args[index], sizeof(packed[index])) != EOK) {
            return ACL_ERROR_INVALID_PARAM;
        }
        if (packed[index] != (count == 2 ? 0x1234 : index + 1)) {
            return ACL_ERROR_INVALID_PARAM;
        }
    }
    if (args[count - 1] == nullptr || memcpy_s(&packed[3], sizeof(packed[3]), args[count - 1], sizeof(void*)) != EOK) {
        return ACL_ERROR_INVALID_PARAM;
    }
    return OriginalLaunch(function, blocks, stream, config, packed, sizeof(packed), nullptr, 0);
}

int main(int argc, char** argv)
{
    g_loadFromFile = argc == 2 && StringEquals(argv[1], "--file");
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromFile", &OriginalFileLoad) == ACL_SUCCESS);
    setenv("NPU_CHECK_TRACE_RECORDS_PER_BLOCK", "2", 1);
    CHECK(RuntimeStubSetSocName("Ascend950PR_9589") == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtMalloc", &OriginalMalloc) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtFree", &OriginalFree) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtMemcpy", &OriginalMemcpy) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtLaunchKernelWithHostArgs", &OriginalLaunch) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &OriginalSync) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtGetDevice", &OriginalGetDevice) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtGetDeviceInfo", &OriginalGetDeviceInfo) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &OriginalBinaryLoad) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryGetFunction", &OriginalBinaryGetFunction) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtLaunchKernelWithArgsArray", &OriginalArrayLaunch) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtFunctionGetParamCount", &OriginalGetParamCount) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtFunctionGetParamInfo", &OriginalGetParamInfo) == ACL_SUCCESS);
    CHECK(acltoolHookInit() == ACL_SUCCESS);

    AclsanSubscriberHandle subscriber = nullptr;
    CHECK(aclsanSubscribe(&subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);

    int ordinaryFunctionStorage = 0;
    int streamStorage1 = 0;
    int streamStorage2 = 0;
    aclrtFuncHandle function = nullptr;
    const auto ordinaryFunction = reinterpret_cast<aclrtFuncHandle>(&ordinaryFunctionStorage);
    const auto stream1 = reinterpret_cast<aclrtStream>(&streamStorage1);
    const auto stream2 = reinterpret_cast<aclrtStream>(&streamStorage2);
    CHECK(LoadInstrumentedFunction(16, "Offset16", &function));
    CHECK(function == kFunction);

    uint64_t arguments[2] = {1, 2};
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    // FIFO slots use the fixed physical topology, even when cores are disabled.
    CHECK(g_deviceInfoCalls == 0);

    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream2, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream2) == ACL_SUCCESS);
    CHECK(g_records.size() == 4);
    std::stable_sort(g_records.begin(), g_records.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.launchId < rhs.launchId;
    });
    CHECK(g_records[2].launchId == 2 && g_records[3].launchId == 2);
    constexpr uint64_t slotIdentity = static_cast<uint64_t>(aclsan::ASCSAN_AIC_CORES_PER_DIE) << 32U;
    CHECK(g_records[0].launchId == 1);
    CHECK(g_records[0].instrExecId == (slotIdentity | 1U));
    CHECK(g_records[0].deviceId == 3);
    CHECK(g_records[0].phyCoreId == aclsan::ASCSAN_AIC_CORES_PER_DIE);
    CHECK(g_records[0].blockId == 1);
    CHECK(g_records[0].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
    CHECK(g_records[0].pc == 0x1234);
    CHECK(g_records[0].srcPipe == 2);
    CHECK(g_records[0].dstPipe == 3);
    CHECK(g_records[0].objectId == 7);
    CHECK(g_records[1].launchId == 1);
    CHECK(g_records[1].instrExecId == (slotIdentity | 2U));
    CHECK(g_records[1].deviceId == 3);
    CHECK(g_records[1].phyCoreId == aclsan::ASCSAN_AIC_CORES_PER_DIE);
    CHECK(g_records[1].blockId == 1);
    CHECK(g_records[1].blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR);
    CHECK(g_records[1].pc == 0x2234);
    CHECK(g_records[1].objectId == 8);

    g_writeMemoryRecord = true;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_memoryAccesses.size() == 2);
    CHECK(g_memoryAccesses[0].address == 0x120000025000ULL);
    CHECK(g_memoryAccesses[0].bytes == 8256);
    CHECK(g_memoryAccesses[0].memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    CHECK(g_memoryAccesses[0].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    CHECK(g_memoryAccesses[1].address == 0);
    CHECK(g_memoryAccesses[1].bytes == 8256);
    CHECK(g_memoryAccesses[1].memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
    CHECK(g_memoryAccesses[1].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    g_writeMemoryRecord = false;

    g_memoryAccesses.clear();
    g_writeMultiMemoryRecords = true;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_memoryAccesses.size() == 2);
    CHECK(g_memoryAccesses[0].address == 0x130000000000ULL);
    CHECK(g_memoryAccesses[0].memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM);
    CHECK(g_memoryAccesses[0].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    CHECK(g_memoryAccesses[0].layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    CHECK(g_memoryAccesses[0].rank == 2);
    CHECK(g_memoryAccesses[0].firstCount == 4);
    CHECK(g_memoryAccesses[0].secondCount == 2);
    CHECK(g_memoryAccesses[1].address == 0);
    CHECK(g_memoryAccesses[1].memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L1);
    CHECK(g_memoryAccesses[1].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    CHECK(g_memoryAccesses[1].layoutKind == ACLSAN_MEM_LAYOUT_ND_AFFINE);
    CHECK(g_memoryAccesses[1].rank == 3);
    g_writeMultiMemoryRecords = false;

    g_memoryAccesses.clear();
    g_writeScalarAtomicRecords = true;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    g_writeScalarAtomicRecords = false;
    constexpr uint32_t atomicModes[] = {
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_CAS, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_EXCH,
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN,
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX};
    CHECK(g_memoryAccesses.size() == std::size(SCALAR_ATOMIC_TYPES) + 3);
    for (size_t i = 0; i < std::size(SCALAR_ATOMIC_TYPES); ++i) {
        CHECK(g_memoryAccesses[i].address == 0x2001000);
        CHECK(g_memoryAccesses[i].accessMode == atomicModes[SCALAR_ATOMIC_TYPES[i]]);
        CHECK(g_memoryAccesses[i].flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
    }
    CHECK(g_memoryAccesses[9].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX);
    CHECK(g_memoryAccesses[10].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ);
    CHECK(g_memoryAccesses[11].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE);
    CHECK((g_memoryAccesses[10].flags & ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN) != 0);
    CHECK((g_memoryAccesses[11].flags & ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN) != 0);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_memoryAccesses.size() == std::size(SCALAR_ATOMIC_TYPES) + 3);

    const size_t recordsBeforeTimedOutSync = g_records.size();
    const size_t d2hBeforeTimedOutSync = g_d2hCalls;
    const size_t freesBeforeTimedOutSync = g_freeCalls;
    g_syncResult = ACL_ERROR_RT_STREAM_SYNC_TIMEOUT;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_ERROR_RT_STREAM_SYNC_TIMEOUT);
    // The kernel may still be using the trace buffer after a timeout.
    CHECK(g_freeCalls == freesBeforeTimedOutSync);
    g_syncResult = ACL_SUCCESS;
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_records.size() == recordsBeforeTimedOutSync + 2);
    CHECK(g_d2hCalls == d2hBeforeTimedOutSync + 1);
    // A timed-out stream may still be using the injected buffer. Retain it until
    // a successful synchronization, without dispatching the same records twice.
    CHECK(g_freeCalls == freesBeforeTimedOutSync);
    g_syncResult = ACL_SUCCESS;
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_freeCalls == freesBeforeTimedOutSync + 1);
    CHECK(g_records.size() == recordsBeforeTimedOutSync + 2);

    const size_t recordsBeforeDbiCompletion = g_records.size();
    const size_t d2hBeforeDbiCompletion = g_d2hCalls;
    const size_t freesBeforeDbiCompletion = g_freeCalls;
    g_syncResult = ACL_ERROR_RT_AICORE_EXCEPTION;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_ERROR_RT_AICORE_EXCEPTION);
    CHECK(g_records.size() == recordsBeforeDbiCompletion + 2);
    CHECK(g_d2hCalls == d2hBeforeDbiCompletion + 1);
    CHECK(g_freeCalls == freesBeforeDbiCompletion);
    g_syncResult = ACL_SUCCESS;
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_freeCalls == freesBeforeDbiCompletion + 1);

    g_syncResult = ACL_ERROR_RT_AICORE_EXCEPTION;
    g_writeRecord = false;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_ERROR_RT_AICORE_EXCEPTION);
    g_syncResult = ACL_SUCCESS;
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    g_writeRecord = true;

    const size_t recordsBeforeGenericFailure = g_records.size();
    const size_t d2hBeforeGenericFailure = g_d2hCalls;
    const size_t freesBeforeGenericFailure = g_freeCalls;
    g_syncResult = ACL_ERROR_FAILURE;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_ERROR_FAILURE);
    CHECK(g_freeCalls == freesBeforeGenericFailure);
    g_syncResult = ACL_SUCCESS;
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_records.size() == recordsBeforeGenericFailure + 2);
    CHECK(g_d2hCalls == d2hBeforeGenericFailure + 1);
    CHECK(g_freeCalls == freesBeforeGenericFailure + 1);

    const size_t freesBeforeFailedLaunch = g_freeCalls;
    g_failLaunch = true;
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), nullptr, 0) ==
        ACL_ERROR_FAILURE);
    CHECK(g_freeCalls == freesBeforeFailedLaunch + 1);
    g_failLaunch = false;

    CHECK(LoadInstrumentedFunction(8, "Offset8", &function));
    CHECK(function == kFunction);
    g_expectedHiddenOffset = 8;
    g_expectedLaunchArgumentBytes = 16;
    g_expectedPlaceholderDataOffset = 0;
    g_expectMaterializedHostInput = true;
    aclrtPlaceHolderInfo placeholder{0, 8};
    CHECK(
        aclrtLaunchKernelWithHostArgs(function, 2, stream1, nullptr, arguments, sizeof(arguments), &placeholder, 1) ==
        ACL_SUCCESS);
    CHECK(placeholder.dataOffset == 8);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    g_expectMaterializedHostInput = false;

    aclrtFuncHandle oneArgumentFunction = nullptr;
    aclrtFuncHandle zeroArgumentFunction = nullptr;
    aclrtFuncHandle threeArgumentFunction = nullptr;
    CHECK(LoadInstrumentedFunction(24, "OneArgument", &oneArgumentFunction));
    CHECK(oneArgumentFunction == kOneArgumentFunction);
    CHECK(aclrtBinaryGetFunction(kOffset24Binary, "ZeroArgument", &zeroArgumentFunction) == ACL_SUCCESS);
    CHECK(aclrtBinaryGetFunction(kOffset24Binary, "ThreeArguments", &threeArgumentFunction) == ACL_SUCCESS);
    g_writeRecord = false;
    g_expectedHiddenOffset = 24;
    g_expectedLaunchArgumentBytes = 32;
    g_expectedPlaceholderDataOffset = 0;
    g_checkPadding = true;
    uint64_t oneArgument = 0x1234;
    g_expectedPaddingBegin = 8;
    CHECK(
        aclrtLaunchKernelWithHostArgs(
            oneArgumentFunction, 2, stream1, nullptr, &oneArgument, sizeof(oneArgument), nullptr, 0) == ACL_SUCCESS);
    g_expectedPaddingBegin = 0;
    CHECK(
        aclrtLaunchKernelWithHostArgs(zeroArgumentFunction, 2, stream1, nullptr, nullptr, 0, nullptr, 0) ==
        ACL_SUCCESS);
    uint64_t threeArguments[3] = {1, 2, 3};
    g_expectedPaddingBegin = 24;
    CHECK(
        aclrtLaunchKernelWithHostArgs(
            threeArgumentFunction, 2, stream1, nullptr, threeArguments, sizeof(threeArguments), nullptr, 0) ==
        ACL_SUCCESS);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    g_checkPadding = false;

    void* ordinaryArgs = arguments;
    CHECK(
        aclrtLaunchKernelWithHostArgs(
            ordinaryFunction, 2, stream1, nullptr, ordinaryArgs, sizeof(arguments), &placeholder, 1) ==
        ACL_ERROR_INVALID_PARAM);
    CHECK(g_mallocCalls == g_freeCalls);

    void* oneArray[] = {&oneArgument};
    void* threeArray[] = {&threeArguments[0], &threeArguments[1], &threeArguments[2]};
    const size_t infoCallsBeforeArray = g_arrayInfoCalls;
    const size_t recordsBeforeArray = g_records.size();
    const size_t freesBeforeArray = g_freeCalls;
    g_writeRecord = true;
    CHECK(aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, oneArray) == ACL_SUCCESS);
    CHECK(aclrtLaunchKernelWithArgsArray(zeroArgumentFunction, 2, stream1, nullptr, nullptr) == ACL_SUCCESS);
    CHECK(aclrtLaunchKernelWithArgsArray(threeArgumentFunction, 2, stream1, nullptr, threeArray) == ACL_SUCCESS);
    CHECK(oneArray[0] == &oneArgument && oneArgument == 0x1234);
    CHECK(threeArray[0] == &threeArguments[0] && threeArray[2] == &threeArguments[2]);
    CHECK(g_freeCalls == freesBeforeArray);
    CHECK(aclrtSynchronizeStream(stream1) == ACL_SUCCESS);
    CHECK(g_records.size() == recordsBeforeArray + 6);
    CHECK(g_freeCalls == freesBeforeArray + 3);
    CHECK(g_arrayOriginalCalls == 3);
    CHECK(g_arrayInfoCalls == infoCallsBeforeArray + 3);

    g_failLaunch = true;
    CHECK(aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, oneArray) == ACL_ERROR_FAILURE);
    g_failLaunch = false;
    CHECK(g_arrayOriginalCalls == 4);
    CHECK(g_mallocCalls == g_freeCalls);
    const size_t infoCallsBeforeFailure = g_arrayInfoCalls;
    g_arrayCountStatus = 73;
    CHECK(aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, oneArray) == 73);
    g_arrayCountStatus = ACL_SUCCESS;
    g_zeroMetadataCount = true;
    CHECK(
        aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, oneArray) ==
        ACL_ERROR_FEATURE_UNSUPPORTED);
    g_zeroMetadataCount = false;
    CHECK(g_arrayInfoCalls == infoCallsBeforeFailure);
    g_badHiddenSize = true;
    CHECK(
        aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, oneArray) ==
        ACL_ERROR_FEATURE_UNSUPPORTED);
    g_badHiddenSize = false;
    g_arrayMetadataStatus = 74;
    CHECK(aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, oneArray) == 74);
    g_arrayMetadataStatus = ACL_SUCCESS;
    g_badHiddenOffset = true;
    CHECK(
        aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, oneArray) ==
        ACL_ERROR_FEATURE_UNSUPPORTED);
    g_badHiddenOffset = false;
    CHECK(aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, nullptr) == ACL_ERROR_INVALID_PARAM);
    void* nullArray[] = {nullptr};
    CHECK(aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, stream1, nullptr, nullArray) == 75);
    CHECK(g_arrayOriginalCalls == 5);
    CHECK(g_mallocCalls == g_freeCalls);

    if (g_loadFromFile) {
        CHECK(g_patchedPaths.size() == 3);
        for (const auto& path : g_patchedPaths)
            CHECK(access(path.c_str(), F_OK) == 0);
        for (auto binary : {kOffset16Binary, kOffset8Binary, kOffset24Binary}) {
            CHECK(aclrtBinaryUnLoad(binary) == ACL_SUCCESS);
        }
        for (const auto& path : g_patchedPaths)
            CHECK(access(path.c_str(), F_OK) != 0);
    }
    CHECK(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
    return 0;
}
