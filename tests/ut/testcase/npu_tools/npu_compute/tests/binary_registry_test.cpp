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

#include "acl_pti/profiling/binary_instrumenter.h"
#include "acl_pti/profiling/binary_registry.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace aclpti::profiling;
namespace {
int binaryTokens[32];
int functionTokens[32];
int loads = 0;
int failLoad = -1;
void* failLookup = nullptr;
void* failUnload = nullptr;
bool failTools = false;
bool failBinary = false;
bool failName = false;
std::vector<void*> unloaded;
const void* images[32]{};
std::size_t imageSizes[32]{};

bool ValidImage(aclrtBinHandle binary)
{
    const auto index = static_cast<int*>(binary) - binaryTokens;
    return imageSizes[index] >= 3 && std::memcmp(images[index], "ELF", 3) == 0;
}

aclError Load(const void* data, std::size_t size, const aclrtBinaryLoadOptions*, aclrtBinHandle* result)
{
    const int index = loads++;
    if (index == failLoad) {
        return ACL_ERROR_RT_FAILURE;
    }
    *result = &binaryTokens[index];
    images[index] = data;
    imageSizes[index] = size;
    return ACL_SUCCESS;
}
aclError Unload(aclrtBinHandle binary)
{
    if (!ValidImage(binary)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (binary == failUnload) {
        return ACL_ERROR_RT_FAILURE;
    }
    unloaded.push_back(binary);
    return ACL_SUCCESS;
}
aclError Lookup(aclrtBinHandle binary, const char*, aclrtFuncHandle* result)
{
    if (!ValidImage(binary)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (binary == failLookup) {
        return ACL_ERROR_RT_FAILURE;
    }
    *result = &functionTokens[static_cast<int*>(binary) - binaryTokens];
    return ACL_SUCCESS;
}
aclError Entry(aclrtBinHandle binary, std::uint64_t, aclrtFuncHandle* result)
{
    return Lookup(binary, "kernel", result);
}
} // namespace

extern "C" void* acltoolGetOriginalRuntimeApi(aclrtApiId id)
{
    switch (id) {
        case ACL_RT_API_aclrtBinaryLoadFromData:
            return reinterpret_cast<void*>(&Load);
        case ACL_RT_API_aclrtBinaryUnLoad:
            return reinterpret_cast<void*>(&Unload);
        case ACL_RT_API_aclrtBinaryGetFunction:
            return reinterpret_cast<void*>(&Lookup);
        case ACL_RT_API_aclrtBinaryGetFunctionByEntry:
            return reinterpret_cast<void*>(&Entry);
        default:
            return nullptr;
    }
}
extern "C" aclError BinaryRegistryTestFunctionGetBinary(aclrtFuncHandle function, aclrtBinHandle* binary) __asm__(
    "aclrtFunctionGetBinary");

extern "C" aclError BinaryRegistryTestFunctionGetBinary(aclrtFuncHandle function, aclrtBinHandle* binary)
{
    if (failBinary)
        return ACL_ERROR_RT_FAILURE;
    *binary = &binaryTokens[static_cast<int*>(function) - functionTokens];
    return ACL_SUCCESS;
}
extern "C" aclError BinaryRegistryTestGetFunctionName(aclrtFuncHandle, uint32_t, char*) __asm__("aclrtGetFunctionName");

extern "C" aclError BinaryRegistryTestGetFunctionName(aclrtFuncHandle, uint32_t, char* name)
{
    if (failName)
        return ACL_ERROR_RT_FAILURE;
    std::strcpy(name, "kernel");
    return ACL_SUCCESS;
}
namespace aclpti::profiling {
bool InstrumentKernelEnd(const void*, std::size_t, std::vector<char>& output, std::uint32_t* traceArgumentOffset)
{
    if (traceArgumentOffset != nullptr) {
        *traceArgumentOffset = 56;
    }
    output = {'E', 'L', 'F'};
    return !failTools;
}
} // namespace aclpti::profiling

TEST(NpuComputeBinaryRegistry, RegistersCompanionsAndPreservesOriginalsOnFailure)
{
    loads = 0;
    failLoad = -1;
    failLookup = nullptr;
    failUnload = nullptr;
    failTools = false;
    failBinary = false;
    failName = false;
    unloaded.clear();
    BinaryRegistry registry;
    const char input[] = "ELF";
    aclrtBinHandle a = nullptr;
    aclrtFuncHandle f = nullptr;
    ASSERT_TRUE(Load(input, sizeof(input), nullptr, &a) == ACL_SUCCESS);
    failTools = true;
    ASSERT_TRUE(registry.RegisterBinary(input, sizeof(input), nullptr, a) == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(unloaded.empty()); // Profiling failure must not release the application's original binary.
    failTools = false;
    failLoad = loads;
    ASSERT_TRUE(registry.RegisterBinary(input, sizeof(input), nullptr, a) == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(unloaded.empty());
    failLoad = -1;
    const int companionIndex = loads;
    ASSERT_TRUE(registry.RegisterBinary(input, sizeof(input), nullptr, a) == ACLPTI_SUCCESS);

    const char* filePath = "/tmp/npu_compute_binary_registry_test.bin";
    {
        std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file.write(input, sizeof(input));
        ASSERT_TRUE(file.good());
    }
    aclrtBinHandle fileBinary = nullptr;
    ASSERT_TRUE(Load(input, sizeof(input), nullptr, &fileBinary) == ACL_SUCCESS);
    ASSERT_TRUE(registry.RegisterBinaryFromFile(filePath, nullptr, fileBinary) == ACLPTI_SUCCESS);
    ASSERT_TRUE(fileBinary != nullptr);
    std::remove(filePath);

    ASSERT_TRUE(Lookup(a, "kernel", &f) == ACL_SUCCESS);
    failBinary = true;
    ASSERT_TRUE(registry.RegisterSymbolFunction(f) == ACLPTI_ERROR_PROFILING_FAILED);
    failBinary = false;
    failName = true;
    ASSERT_TRUE(registry.RegisterSymbolFunction(f) == ACLPTI_ERROR_PROFILING_FAILED);
    failName = false;
    ASSERT_TRUE(registry.RegisterSymbolFunction(f) == ACLPTI_SUCCESS);
    ASSERT_TRUE(registry.FindInstrumentedFunction(f) == &functionTokens[companionIndex]);
    std::uint32_t traceOffset = 0;
    ASSERT_TRUE(registry.GetInstrumentedTraceArgumentOffset(registry.FindInstrumentedFunction(f), traceOffset));
    ASSERT_TRUE(traceOffset == 56);
    ASSERT_TRUE(!registry.GetInstrumentedTraceArgumentOffset(f, traceOffset));
    ASSERT_TRUE(traceOffset == 0);

    aclrtBinHandle b = nullptr;
    aclrtFuncHandle g = nullptr;
    ASSERT_TRUE(Load(input, sizeof(input), nullptr, &b) == ACL_SUCCESS);
    ASSERT_TRUE(registry.RegisterBinary(input, sizeof(input), nullptr, b) == ACLPTI_SUCCESS);
    ASSERT_TRUE(Entry(b, 42, &g) == ACL_SUCCESS);
    ASSERT_TRUE(registry.RegisterSymbolFunction(g) == ACLPTI_SUCCESS);
    ASSERT_TRUE(registry.RegisterBinaryFunction(b, std::uint64_t(42), g) == ACLPTI_SUCCESS);
    ASSERT_TRUE(registry.FindInstrumentedFunction(f) != registry.FindInstrumentedFunction(g));

    failLookup = &binaryTokens[companionIndex];
    ASSERT_TRUE(registry.RegisterBinaryFunction(a, "kernel", f) == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(f != nullptr && registry.FindInstrumentedFunction(f) == nullptr);
    ASSERT_TRUE(registry.RegisterBinaryFunction(a, std::uint64_t(42), f) == ACLPTI_ERROR_PROFILING_FAILED);
    failLookup = nullptr;
    ASSERT_TRUE(registry.RegisterBinaryFunction(a, "kernel", f) == ACLPTI_SUCCESS);
    failUnload = &binaryTokens[companionIndex];
    {
        BinaryRegistry::UnloadContext context;
        ASSERT_TRUE(registry.PrepareBinaryUnload(a, context) == ACLPTI_ERROR_PROFILING_FAILED);
    }
    ASSERT_TRUE(registry.FindInstrumentedFunction(f) != nullptr);
    failUnload = nullptr;
    {
        BinaryRegistry::UnloadContext context;
        ASSERT_TRUE(registry.PrepareBinaryUnload(a, context) == ACLPTI_SUCCESS);
        ASSERT_TRUE(Unload(a) == ACL_SUCCESS);
        ASSERT_TRUE(registry.CompleteBinaryUnload(context) == ACLPTI_SUCCESS);
    }
    ASSERT_TRUE(registry.FindInstrumentedFunction(f) == nullptr);
    ASSERT_TRUE(!registry.GetInstrumentedTraceArgumentOffset(&functionTokens[companionIndex], traceOffset));
    failUnload = b;
    {
        BinaryRegistry::UnloadContext context;
        ASSERT_TRUE(registry.PrepareBinaryUnload(b, context) == ACLPTI_SUCCESS);
        ASSERT_TRUE(Unload(b) == ACL_ERROR_RT_FAILURE);
        // Destruction releases the lock without removing the original record.
    }
    ASSERT_TRUE(registry.FindInstrumentedFunction(g) == nullptr);
    const auto count = unloaded.size();
    failUnload = nullptr;
    {
        BinaryRegistry::UnloadContext context;
        ASSERT_TRUE(registry.PrepareBinaryUnload(b, context) == ACLPTI_SUCCESS);
        ASSERT_TRUE(Unload(b) == ACL_SUCCESS);
        ASSERT_TRUE(registry.CompleteBinaryUnload(context) == ACLPTI_SUCCESS);
    }
    ASSERT_TRUE(unloaded.size() == count + 1);
}
