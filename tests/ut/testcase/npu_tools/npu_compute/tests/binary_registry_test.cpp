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
#include <algorithm>

#include "acl_pti/profiling/binary_instrumenter.h"
#include "acl_pti/profiling/binary_registry.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using aclpti::profiling::BinaryRegistry;
using aclpti::profiling::PmSamplingBinarySnapshot;
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
bool failAddress = false;
bool invalidFallbackElf = false;
bool mixedSymbols = false;
bool simtBinary = false;
bool requirePhysicalName = false;
int instrumentationCalls = 0;
int samplingInstrumentationCalls = 0;
int snapshotAddressCalls = 0;
std::uint64_t lastEntry = 0;
std::vector<char> fallbackElf(64, 0);
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
aclError Lookup(aclrtBinHandle binary, const char* name, aclrtFuncHandle* result)
{
    if (!ValidImage(binary)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    if (binary == failLookup) {
        return ACL_ERROR_RT_FAILURE;
    }
    if (requirePhysicalName && std::strcmp(name, "kernel_mix_aic") != 0) {
        return ACL_ERROR_RT_FAILURE;
    }
    *result = &functionTokens[static_cast<int*>(binary) - binaryTokens];
    return ACL_SUCCESS;
}
aclError Entry(aclrtBinHandle binary, std::uint64_t entry, aclrtFuncHandle* result)
{
    lastEntry = entry;
    return Lookup(binary, "kernel", result);
}
} // namespace

extern "C" void* acltoolGetOriginalRuntimeApi(aclrtApiId id)
{
    switch (id) {
        case ACL_RT_API_aclrtMemcpy:
            return reinterpret_cast<void*>(
                +[](void* target, std::size_t max, const void* source, std::size_t size, aclrtMemcpyKind) -> aclError {
                    if (size > max) {
                        return ACL_ERROR_INVALID_PARAM;
                    }
                    std::copy_n(
                        reinterpret_cast<const unsigned char*>(source), size, reinterpret_cast<unsigned char*>(target));
                    return ACL_SUCCESS;
                });
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
    constexpr char kKernelName[] = "kernel";
    std::copy_n(kKernelName, sizeof(kKernelName), name);
    return ACL_SUCCESS;
}
extern "C" __attribute__((visibility("default"))) aclError
BinaryRegistryTestGetFunctionAddr(const void*, void**, void**) __asm__("aclrtGetFunctionAddr");
extern "C" __attribute__((visibility("default"))) aclError
BinaryRegistryTestGetFunctionAddr(const void*, void** aic, void** aiv)
{
    ++snapshotAddressCalls;
    if (failAddress) {
        return ACL_ERROR_RT_FAILURE;
    }
    *aic = nullptr;
    *aiv = reinterpret_cast<void*>(0x12000000100ULL);
    return ACL_SUCCESS;
}
extern "C" __attribute__((visibility("default"))) aclError
BinaryRegistryTestGetBinaryAddress(aclrtBinHandle, void**, std::size_t*) __asm__("aclrtBinaryGetDevAddress");
extern "C" __attribute__((visibility("default"))) aclError
BinaryRegistryTestGetBinaryAddress(aclrtBinHandle, void** address, std::size_t* size)
{
    std::copy_n(
        reinterpret_cast<const unsigned char*>(
            invalidFallbackElf ? "BAD!" :
                                 "\x7f"
                                 "ELF"),
        4, reinterpret_cast<unsigned char*>(fallbackElf.data()));
    *address = fallbackElf.data();
    *size = fallbackElf.size();
    return ACL_SUCCESS;
}
namespace aclpti::profiling {
bool InstrumentKernelEnd(
    const void*, std::size_t, std::vector<char>& output, std::uint32_t* traceArgumentOffset,
    PmSamplingInstructionMap* pmSamplingMap)
{
    ++instrumentationCalls;
    if (pmSamplingMap != nullptr) {
        pmSamplingMap->business = {{0x100, 0}};
        if (simtBinary) {
            pmSamplingMap->simt = {0x100};
        }
        pmSamplingMap->functionEntries =
            mixedSymbols ? std::map<std::string, std::uint64_t>{{"kernel_mix_aic", 0x100}, {"kernel_mix_aiv", 0x200}} :
                           std::map<std::string, std::uint64_t>{{"kernel", 0x100}};
        pmSamplingMap->relocatedEntries = {{0, 0x100}};
    }
    if (traceArgumentOffset != nullptr) {
        *traceArgumentOffset = 56;
    }
    output = {'E', 'L', 'F'};
    return !failTools;
}

bool InstrumentSimtSampling(
    const void*, std::size_t, std::vector<char>& output, std::uint32_t* probeArgumentOffset,
    PmSamplingInstructionMap* pmSamplingMap)
{
    ++samplingInstrumentationCalls;
    if (probeArgumentOffset != nullptr) {
        *probeArgumentOffset = 64;
    }
    if (pmSamplingMap != nullptr) {
        pmSamplingMap->business = {{0x180, 0x100}};
        pmSamplingMap->simt = {0x180};
        pmSamplingMap->functionEntries =
            mixedSymbols ? std::map<std::string, std::uint64_t>{{"kernel_mix_aic", 0x180}, {"kernel_mix_aiv", 0x280}} :
                           std::map<std::string, std::uint64_t>{{"kernel", 0x180}};
        pmSamplingMap->relocatedEntries = {{0x100, 0x180}};
    }
    output = {'E', 'L', 'F', 'S', 'I', 'M', 'T'};
    return !failTools;
}

bool BuildOriginalPmSamplingInstructionMap(const void*, std::size_t, PmSamplingInstructionMap& output)
{
    output = {};
    output.business = {{0x100, 0}};
    output.functionEntries =
        mixedSymbols ? std::map<std::string, std::uint64_t>{{"kernel_mix_aic", 0x100}, {"kernel_mix_aiv", 0x200}} :
                       std::map<std::string, std::uint64_t>{{"kernel", 0x100}};
    output.relocatedEntries = {{0, 0x100}};
    if (simtBinary) {
        output.simt = {0x100};
    }
    return true;
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

    // PmSampling owns both ELF copies, the complete map and launch-time addresses.
    aclrtBinHandle samplingBinary = nullptr;
    aclrtFuncHandle samplingFunction = nullptr;
    ASSERT_TRUE(Load(input, sizeof(input), nullptr, &samplingBinary) == ACL_SUCCESS);
    const auto toolsBefore = instrumentationCalls;
    lastEntry = 0;
    ASSERT_TRUE(registry.RegisterBinary(input, sizeof(input), nullptr, samplingBinary, true, true) == ACLPTI_SUCCESS);
    ASSERT_TRUE(registry.RegisterBinary(input, sizeof(input), nullptr, samplingBinary, true, true) == ACLPTI_SUCCESS);
    ASSERT_TRUE(registry.RegisterBinary(input, sizeof(input), nullptr, samplingBinary, false, true) == ACLPTI_SUCCESS);
    ASSERT_TRUE(instrumentationCalls == toolsBefore + 1); // Only the Pipeline companion is instrumented.
    ASSERT_TRUE(Lookup(samplingBinary, "kernel", &samplingFunction) == ACL_SUCCESS);
    ASSERT_TRUE(registry.RegisterBinaryFunction(samplingBinary, "kernel", samplingFunction) == ACLPTI_SUCCESS);
    ASSERT_TRUE(registry.RegisterBinaryFunction(samplingBinary, std::uint64_t(0), samplingFunction) == ACLPTI_SUCCESS);
    ASSERT_TRUE(lastEntry == 0x100);
    PmSamplingBinarySnapshot snapshot;
    failAddress = true;
    ASSERT_TRUE(!registry.GetPmSamplingSnapshot(samplingFunction, snapshot));
    ASSERT_TRUE(!snapshot.image);
    failAddress = false;
    ASSERT_TRUE(registry.GetPmSamplingSnapshot(samplingFunction, snapshot));
    ASSERT_TRUE(
        snapshot.image && snapshot.elfEntry == 0x100 && snapshot.aicRuntimeEntry == 0 &&
        snapshot.aivRuntimeEntry == 0x12000000100ULL);
    ASSERT_TRUE(snapshot.image->instructions.business.at(0x100) == 0);
    ASSERT_TRUE(snapshot.image->instructions.business.count(0x104) == 0);
    // Runtime's ACLNN all-kernel entry is a tiling key; the original function name
    // supplies the ELF entry used to map the Pipeline companion.
    ASSERT_EQ(registry.RegisterBinaryFunction(samplingBinary, std::uint64_t(1), samplingFunction), ACLPTI_SUCCESS);
    ASSERT_EQ(lastEntry, 1U);
    ASSERT_TRUE(registry.GetPmSamplingSnapshot(samplingFunction, snapshot));
    ASSERT_EQ(snapshot.elfEntry, 0x100U);
    failName = true;
    ASSERT_EQ(
        registry.RegisterBinaryFunction(samplingBinary, std::uint64_t(2), samplingFunction),
        ACLPTI_ERROR_PROFILING_FAILED);
    failName = false;
    ASSERT_EQ(registry.RegisterBinaryFunction(samplingBinary, std::uint64_t(1), samplingFunction), ACLPTI_SUCCESS);
    ASSERT_TRUE(std::memcmp(snapshot.image->originalElf.data(), input, sizeof(input)) == 0);
    {
        BinaryRegistry::UnloadContext context;
        ASSERT_TRUE(registry.PrepareBinaryUnload(samplingBinary, context) == ACLPTI_SUCCESS);
        ASSERT_TRUE(Unload(samplingBinary) == ACL_SUCCESS);
        ASSERT_TRUE(registry.CompleteBinaryUnload(context) == ACLPTI_SUCCESS);
    }
    const auto addressCalls = snapshotAddressCalls;
    ASSERT_TRUE(snapshot.image->instructions.business.at(0x100) == 0);
    ASSERT_TRUE(std::memcmp(snapshot.image->originalElf.data(), input, sizeof(input)) == 0);
    ASSERT_TRUE(snapshotAddressCalls == addressCalls); // Workers read snapshots without a Runtime call.
    PmSamplingBinarySnapshot missing;
    ASSERT_TRUE(!registry.GetPmSamplingSnapshot(samplingFunction, missing));

    // A binary loaded before hooks can only be registered from a copied complete ELF.
    aclrtBinHandle preloaded = nullptr;
    aclrtFuncHandle preloadedFunction = nullptr;
    ASSERT_TRUE(Load(input, sizeof(input), nullptr, &preloaded) == ACL_SUCCESS);
    ASSERT_TRUE(Lookup(preloaded, "kernel", &preloadedFunction) == ACL_SUCCESS);
    invalidFallbackElf = true;
    ASSERT_TRUE(registry.RegisterSymbolFunction(preloadedFunction, true, false) == ACLPTI_ERROR_PROFILING_FAILED);
    invalidFallbackElf = false;
    ASSERT_TRUE(registry.RegisterSymbolFunction(preloadedFunction, true, false) == ACLPTI_SUCCESS);
    ASSERT_TRUE(registry.GetPmSamplingSnapshot(preloadedFunction, missing));
}

TEST(NpuComputeBinaryRegistry, ResolvesMixedLogicalNamesAndPhysicalCompanions)
{
    loads = 0;
    failLoad = -1;
    failLookup = nullptr;
    failUnload = nullptr;
    failTools = false;
    failBinary = false;
    failName = false;
    failAddress = false;
    mixedSymbols = true;
    for (bool physicalNameRequired : {false, true}) {
        requirePhysicalName = false;
        BinaryRegistry registry;
        const char input[] = "ELF";
        aclrtBinHandle binary = nullptr;
        aclrtFuncHandle function = nullptr;
        ASSERT_EQ(Load(input, sizeof(input), nullptr, &binary), ACL_SUCCESS);
        ASSERT_EQ(registry.RegisterBinary(input, sizeof(input), nullptr, binary, true, true), ACLPTI_SUCCESS);
        ASSERT_EQ(Lookup(binary, "kernel", &function), ACL_SUCCESS);
        requirePhysicalName = physicalNameRequired;
        ASSERT_EQ(registry.RegisterBinaryFunction(binary, "kernel", function), ACLPTI_SUCCESS);
        ASSERT_NE(registry.FindInstrumentedFunction(function), nullptr);
        PmSamplingBinarySnapshot snapshot;
        ASSERT_TRUE(registry.GetPmSamplingSnapshot(function, snapshot));
        ASSERT_EQ(snapshot.elfEntry, 0x100U);
    }
    mixedSymbols = false;
    requirePhysicalName = false;
}

TEST(NpuComputeBinaryRegistry, RegistersIndependentPmSamplingCompanion)
{
    BinaryRegistry registry;
    simtBinary = true;
    const char input[] = "ELF";
    aclrtBinHandle binary = nullptr;
    aclrtFuncHandle original = nullptr;
    ASSERT_EQ(Load(input, sizeof(input), nullptr, &binary), ACL_SUCCESS);
    const auto toolsBefore = instrumentationCalls;
    const auto samplingToolsBefore = samplingInstrumentationCalls;
    const auto loadsBefore = loads;
    ASSERT_EQ(registry.RegisterBinary(input, sizeof(input), nullptr, binary, true, false), ACLPTI_SUCCESS);
    EXPECT_EQ(instrumentationCalls, toolsBefore);
    EXPECT_EQ(samplingInstrumentationCalls, samplingToolsBefore + 1);
    EXPECT_EQ(loads, loadsBefore + 1);
    ASSERT_EQ(Lookup(binary, "kernel", &original), ACL_SUCCESS);
    ASSERT_EQ(registry.RegisterBinaryFunction(binary, "kernel", original), ACLPTI_SUCCESS);
    const auto sampling = registry.FindPmSamplingFunction(original);
    ASSERT_NE(sampling, nullptr);
    ASSERT_NE(sampling, original);
    ASSERT_TRUE(registry.NeedsPmSamplingProbeBuffer(sampling));
    ASSERT_EQ(registry.FindInstrumentedFunction(original), nullptr);
    PmSamplingBinarySnapshot snapshot;
    ASSERT_TRUE(registry.GetPmSamplingSnapshot(original, snapshot));
    ASSERT_EQ(snapshot.image->instructions.simt, std::set<std::uint64_t>{0x100});
    ASSERT_EQ(snapshot.image->instructions.business.count(0x184), 0U);
    ASSERT_EQ(snapshot.image->companionElf, (std::vector<char>{'E', 'L', 'F', 'S', 'I', 'M', 'T'}));
    const auto unloadsBefore = unloaded.size();
    {
        BinaryRegistry::UnloadContext context;
        ASSERT_EQ(registry.PrepareBinaryUnload(binary, context), ACLPTI_SUCCESS);
        ASSERT_EQ(Unload(binary), ACL_SUCCESS);
        ASSERT_EQ(registry.CompleteBinaryUnload(context), ACLPTI_SUCCESS);
    }
    ASSERT_EQ(unloaded.size(), unloadsBefore + 2);
    simtBinary = false;
}

TEST(NpuComputeBinaryRegistry, FallsBackToOriginalSamplingWhenSimtProbeInsertionFails)
{
    loads = 0;
    failLoad = -1;
    failLookup = nullptr;
    failTools = true;
    failBinary = false;
    failName = false;
    failAddress = false;
    mixedSymbols = false;
    simtBinary = true;
    BinaryRegistry registry;
    const char input[] = "ELF";
    aclrtBinHandle binary = nullptr;
    aclrtFuncHandle original = nullptr;
    ASSERT_EQ(Load(input, sizeof(input), nullptr, &binary), ACL_SUCCESS);
    ASSERT_EQ(registry.RegisterBinary(input, sizeof(input), nullptr, binary, true, false), ACLPTI_SUCCESS);
    ASSERT_EQ(Lookup(binary, "kernel", &original), ACL_SUCCESS);
    ASSERT_EQ(registry.RegisterBinaryFunction(binary, "kernel", original), ACLPTI_SUCCESS);
    EXPECT_EQ(registry.FindPmSamplingFunction(original), original);
    EXPECT_FALSE(registry.NeedsPmSamplingProbeBuffer(original));
    PmSamplingBinarySnapshot snapshot;
    ASSERT_TRUE(registry.GetPmSamplingSnapshot(original, snapshot));
    EXPECT_TRUE(snapshot.error.empty());
    EXPECT_FALSE(snapshot.companionError.empty());
    EXPECT_EQ(snapshot.elfEntry, 0x100U);
    EXPECT_EQ(snapshot.image->companionElf, snapshot.image->originalElf);
    failTools = false;
    simtBinary = false;
}

TEST(NpuComputeBinaryRegistry, KeepsSamplingAndPipelineCompanionsIndependent)
{
    BinaryRegistry registry;
    simtBinary = true;
    mixedSymbols = false;
    const char input[] = "ELF";
    aclrtBinHandle binary = nullptr;
    aclrtFuncHandle original = nullptr;
    ASSERT_EQ(Load(input, sizeof(input), nullptr, &binary), ACL_SUCCESS);
    const auto pipelineToolsBefore = instrumentationCalls;
    const auto samplingToolsBefore = samplingInstrumentationCalls;
    const auto loadsBefore = loads;
    ASSERT_EQ(registry.RegisterBinary(input, sizeof(input), nullptr, binary, true, true), ACLPTI_SUCCESS);
    EXPECT_EQ(instrumentationCalls, pipelineToolsBefore + 1);
    EXPECT_EQ(samplingInstrumentationCalls, samplingToolsBefore + 1);
    EXPECT_EQ(loads, loadsBefore + 2);
    ASSERT_EQ(Lookup(binary, "kernel", &original), ACL_SUCCESS);
    ASSERT_EQ(registry.RegisterBinaryFunction(binary, "kernel", original), ACLPTI_SUCCESS);
    const auto sampling = registry.FindPmSamplingFunction(original);
    const auto pipeline = registry.FindInstrumentedFunction(original);
    ASSERT_NE(sampling, nullptr);
    ASSERT_NE(pipeline, nullptr);
    EXPECT_NE(sampling, pipeline);
    EXPECT_TRUE(registry.NeedsPmSamplingProbeBuffer(sampling));
    EXPECT_FALSE(registry.NeedsPmSamplingProbeBuffer(pipeline));
    std::uint32_t offset = 0;
    ASSERT_TRUE(registry.GetInstrumentedTraceArgumentOffset(sampling, offset));
    EXPECT_EQ(offset, 64U);
    ASSERT_TRUE(registry.GetInstrumentedTraceArgumentOffset(pipeline, offset));
    EXPECT_EQ(offset, 56U);
    PmSamplingBinarySnapshot snapshot;
    ASSERT_TRUE(registry.GetPmSamplingSnapshot(original, snapshot));
    EXPECT_EQ(snapshot.elfEntry, 0x180U);
    EXPECT_EQ(snapshot.image->instructions.business.at(0x100), 0U);
    {
        BinaryRegistry::UnloadContext context;
        ASSERT_EQ(registry.PrepareBinaryUnload(binary, context), ACLPTI_SUCCESS);
        ASSERT_EQ(Unload(binary), ACL_SUCCESS);
        ASSERT_EQ(registry.CompleteBinaryUnload(context), ACLPTI_SUCCESS);
    }
    simtBinary = false;
}
