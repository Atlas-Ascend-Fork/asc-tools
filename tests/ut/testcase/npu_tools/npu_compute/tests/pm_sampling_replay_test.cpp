/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/profiling/range_profiler.h"
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include "acl_pti/profiling/binary_registry.h"
#include "acl_pti/profiling/replay_memory.h"
#include "injection/runtime_stub_api.h"
#include "acl_pti/data/profiling_data_manager.h"
#include <cstdio>
#include <string>
#include <cstdlib>

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
            return 1;                                            \
        }                                                        \
    } while (0)
namespace {
int original, companion, binary, companionBinary;
int shadowValue = 0;
bool addressFails = false, syncFails = false, stopFails = false;
bool collecting = false;
bool warmupSyncFails = false;
std::string calls;
std::vector<unsigned> modes;
int Device(int* id)
{
    *id = 0;
    return 0;
}
int Sync(aclrtStream)
{
    calls += 'S';
    return (syncFails && collecting) || (warmupSyncFails && calls.find('L') != std::string::npos) ? -1 : 0;
}
int Load(const void*, size_t, const aclrtBinaryLoadOptions*, aclrtBinHandle* h)
{
    *h = &companionBinary;
    return 0;
}
int Function(aclrtBinHandle, const char*, aclrtFuncHandle* h)
{
    *h = &companion;
    return 0;
}
int Allocate(void** address, std::size_t, aclrtMemMallocPolicy)
{
    *address = &shadowValue;
    return ACL_SUCCESS;
}
int Free(void*) { return ACL_SUCCESS; }
int Copy(void* destination, std::size_t, const void* source, std::size_t count, aclrtMemcpyKind)
{
    std::copy_n(static_cast<const unsigned char*>(source), count, static_cast<unsigned char*>(destination));
    return ACL_SUCCESS;
}
} // namespace
extern "C" __attribute__((visibility("default"))) aclError
SamplingGetAddress(const void*, void**, void**) __asm__("aclrtGetFunctionAddr");
extern "C" __attribute__((visibility("default"))) aclError SamplingGetAddress(const void*, void** aic, void** aiv)
{
    *aic = nullptr;
    *aiv = reinterpret_cast<void*>(0x100100);
    return addressFails ? -1 : 0;
}
extern "C" int32_t MsprofStart(uint32_t, const void* config, uint32_t)
{
    collecting = true;
    calls += 'B';
    const auto& attr = static_cast<const MsprofConfig*>(config)->configInfo.attrs[0];
    modes.push_back(attr.id == PROF_CONFIG_ATTR_INSTR ? attr.value.instrMode : 0);
    return 0;
}
extern "C" int32_t MsprofStop(uint32_t, const void*, uint32_t)
{
    collecting = false;
    calls += 'E';
    return stopFails ? -1 : 0;
}
extern "C" int32_t MsprofRegisterDataCallback(uint32_t, void*) { return 0; }

int main()
{
    using aclpti::profiling::BinaryRegistry;
    using aclpti::profiling::RangeProfiler;
    using aclpti::profiling::ReplayMemory;
    CHECK(RuntimeStubSetOriginFunction("aclrtGetDevice", &Device) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &Sync) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &Load) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtBinaryGetFunction", &Function) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtMalloc", &Allocate) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtFree", &Free) == 0);
    CHECK(RuntimeStubSetOriginFunction("aclrtMemcpy", &Copy) == 0);
    // Every case starts a fresh data-manager collection and must drain it on failure too.
    constexpr int kMappingFailureScenario = 8;
    constexpr int kSamplingOnlyMappingFailureScenario = kMappingFailureScenario + 1;
    for (int scenario = 0; scenario <= kSamplingOnlyMappingFailureScenario + 3; ++scenario) {
        RangeProfiler profiler;
        BinaryRegistry registry;
        ReplayMemory memory;
        calls.clear();
        collecting = false;
        const bool offsetReadFailure = scenario == kSamplingOnlyMappingFailureScenario + 3;
        const bool warmupLaunchFailure = scenario == kSamplingOnlyMappingFailureScenario + 1;
        warmupSyncFails = scenario == kSamplingOnlyMappingFailureScenario + 2;
        const bool warmupFailure = warmupLaunchFailure || warmupSyncFails;
        unsigned warmups = 0;
        modes.clear();
        addressFails = scenario == 4;
        syncFails = scenario == 6;
        stopFails = scenario == 7;
        aclptiRangeProfilerSetConfigParams config{};
        const char* section = "Memory";
        config.collectPcSampling = true;
        const bool unavailableSnapshot = scenario == 3 || scenario == 4;
        const bool mapFailure = scenario == kMappingFailureScenario || scenario == kSamplingOnlyMappingFailureScenario;
        const bool samplingFailure = mapFailure || unavailableSnapshot;
        const bool shadowMemory =
            scenario == 0 || scenario == kMappingFailureScenario || scenario == kSamplingOnlyMappingFailureScenario;
        constexpr int kKernelOutput = 42;
        int deviceOutput = kKernelOutput;
        void* deviceAddress = &deviceOutput;
        if (shadowMemory) {
            shadowValue = kKernelOutput;
            CHECK(
                memory.MirrorMalloc(
                    &deviceAddress, sizeof(deviceOutput), ACL_MEM_MALLOC_NORMAL_ONLY, ACL_RT_API_aclrtMalloc) ==
                ACLPTI_SUCCESS);
        }
        CHECK(unsetenv("NPU_COMPUTE_TEST_PM_MAP_FAILURE") == 0);
        if (mapFailure) {
            CHECK(setenv("NPU_COMPUTE_TEST_PM_MAP_FAILURE", "1", 1) == 0);
        }
        config.collectPipeline = scenario == 2 || scenario == kMappingFailureScenario;
        if (scenario == 1 || scenario == kMappingFailureScenario) {
            config.sections = &section;
            config.numSections = 1;
        }
        CHECK(profiler.SetConfig(&config) == ACLPTI_SUCCESS);
        std::shared_ptr<const aclptiProfilingDataResult> result;
        CHECK(
            aclpti::data::ProfilingDataManager::Instance().Initialize(
                [&](std::shared_ptr<const aclptiProfilingDataResult> value) {
                    result = std::move(value);
                    return ACLPTI_SUCCESS;
                }) == ACLPTI_SUCCESS);
        CHECK(profiler.Initialize() == ACLPTI_SUCCESS);
        const char elf[] = "ELF";
        if (scenario != 3) {
            CHECK(registry.RegisterBinary(elf, sizeof(elf), nullptr, &binary, true) == ACLPTI_SUCCESS);
            CHECK(registry.RegisterBinaryFunction(&binary, "kernel", &original) == ACLPTI_SUCCESS);
        }
        const auto status = profiler.ReplayKernel(
            memory, registry, &original,
            [&](aclrtFuncHandle h, bool instrumented) {
                calls += 'L';
                if (!collecting) {
                    if (!instrumented || h != &companion || (scenario == 0 && deviceOutput != kKernelOutput)) {
                        return ACL_ERROR_INVALID_PARAM;
                    }
                    ++warmups;
                    deviceOutput = kKernelOutput + 1;
                    return warmupLaunchFailure ? ACL_ERROR_RT_FAILURE : ACL_SUCCESS;
                }
                if (scenario == 0 && deviceOutput != kKernelOutput) {
                    return ACL_ERROR_INVALID_PARAM;
                }
                deviceOutput = kKernelOutput + (scenario == 0 ? 1 : 0);
                if (h != (instrumented ? &companion : &original)) {
                    return ACL_ERROR_INVALID_PARAM;
                }
                if (modes.back() == PROF_COMPUTE_PC_SAMPLING && scenario != 3 && !instrumented) {
                    return ACL_ERROR_INVALID_PARAM;
                }
                if (modes.back() == PROF_COMPUTE_PC_SAMPLING) {
                    MsprofRawData raw{};
                    raw.type = PC_SAMPLING_DATA_TYPE;
                    raw.deviceId = 0;
                    raw.chunkSize = 1;
                    raw.chunk[0] = 0x5a;
                    raw.isLastChunk = true;
                    if (aclpti::data::ProfilingDataManager::Instance().GetRawDataCallback()(&raw) != 0) {
                        return ACL_ERROR_PROFILING_FAILURE;
                    }
                }
                if (scenario == 5) {
                    return ACL_ERROR_RT_FAILURE;
                }
                return ACL_SUCCESS;
            },
            nullptr,
            [&](aclrtFuncHandle, std::uint64_t& offset) {
                if (offsetReadFailure) {
                    return ACLPTI_ERROR_PROFILING_FAILED;
                }
                offset = 0x80;
                return ACLPTI_SUCCESS;
            });
        CHECK((status == ACLPTI_SUCCESS) == (scenario < 5 || mapFailure));
        if (samplingFailure) {
            // Skipping sampling must preserve the last launch's output, including
            // when only PmSampling was selected and no replay was performed.
            CHECK(deviceOutput == kKernelOutput);
            if (shadowMemory) {
                CHECK(memory.MirrorFree(deviceAddress) == ACLPTI_SUCCESS);
            }
        }
        CHECK(calls.front() == 'S');
        if (!unavailableSnapshot && scenario != kSamplingOnlyMappingFailureScenario && !warmupFailure) {
            CHECK(calls.back() == 'E');
        }
        if (!samplingFailure && !warmupFailure) {
            CHECK(modes.back() == PROF_COMPUTE_PC_SAMPLING);
        }
        if (scenario == 0) {
            CHECK(warmups == 5);
            CHECK(calls == "SLSLSLSLSLSBLSE");
            CHECK(deviceOutput == kKernelOutput + 1);
            CHECK(memory.MirrorFree(deviceAddress) == ACLPTI_SUCCESS);
        }
        if (scenario == 1) {
            CHECK(modes.front() == 0 && modes.size() > 1);
        }
        if (scenario == 2) {
            CHECK(modes == std::vector<unsigned>({PROF_COMPUTE_BIU_PERF, PROF_COMPUTE_PC_SAMPLING}));
        }
        if (scenario == kMappingFailureScenario) {
            CHECK(modes.front() == 0 && modes.back() == PROF_COMPUTE_BIU_PERF);
            CHECK(std::count(calls.begin(), calls.end(), 'L') == static_cast<int>(modes.size()));
        }
        if (scenario == kSamplingOnlyMappingFailureScenario) {
            CHECK(calls == "S" && modes.empty());
        }
        if (unavailableSnapshot) {
            CHECK(calls == "S");
        }
        CHECK(profiler.Shutdown() == ACLPTI_SUCCESS);
        CHECK(result != nullptr);
        CHECK((result->status == ACLPTI_SUCCESS) == (scenario < 3));
        if (warmupFailure) {
            CHECK(warmups == 1 && modes.empty());
            CHECK(result->status != ACLPTI_SUCCESS);
            CHECK(result->pcSamplingData.empty());
            continue;
        }
        CHECK(warmups == (samplingFailure ? 0U : 5U));
        CHECK(result->pcSamplingData.empty() == samplingFailure);
        CHECK(result->pmSamplingContexts.size() == (unavailableSnapshot ? 0U : 1U));
        if (!result->pmSamplingContexts.empty()) {
            const auto& context = *result->pmSamplingContexts.begin()->second;
            if (mapFailure) {
                CHECK(!context.instructionMapError.empty());
                CHECK(context.originalElf && context.companionElf);
                CHECK(result->pcSamplingData.empty());
                continue;
            }
            CHECK(result->pcSamplingData.size() == 1);
            CHECK(result->pcSamplingData[0].replayId == result->pmSamplingContexts.begin()->first);
            CHECK(result->pcSamplingData[0].bytes == std::vector<uint8_t>({0x5a}));
            CHECK(context.elfEntry == 0x180 && context.aivRuntimeEntry == 0x100100);
            CHECK(context.originalElf && context.companionElf);
            CHECK(context.originalElf.get() != context.companionElf.get());
            CHECK(context.pcOffset == ((scenario == 5 || scenario == 6 || offsetReadFailure) ? 0U : 0x80U));
            CHECK(context.companionElf->back() == 'S');
            CHECK(context.businessInstructions->count(0x100) == 1);
            CHECK(context.businessInstructions->at(0x100) == 0);
            CHECK(context.businessInstructions->count(0x180) == 0);
            CHECK(context.simtInstructions->count(0x100) == 1);
        }
    }
    CHECK(unsetenv("NPU_COMPUTE_TEST_PM_MAP_FAILURE") == 0);
    std::puts("PASS: PmSampling replay selection, instruction modes, failure stop/release");
}
