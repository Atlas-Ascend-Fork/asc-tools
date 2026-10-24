/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "isolated_gtest.h"

#include "aclpti/aclpti.h"
#include "injection/runtime_stub_api.h"
#include "profiling/prof_api.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct KernelArgs {
    uint8_t* values;
    std::size_t count;
};

std::size_t gLaunchCalls = 0;
std::size_t gStartCalls = 0;
std::vector<std::array<uint8_t, 2>> gInputs;

int RealMalloc(void** pointer, std::size_t size, aclrtMemMallocPolicy)
{
    *pointer = std::malloc(size);
    return *pointer == nullptr ? -1 : 0;
}

int RealFree(void* pointer)
{
    std::free(pointer);
    return 0;
}

int RealMemcpy(void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind)
{
    if (destination == nullptr || source == nullptr || count > destinationSize) {
        return -1;
    }
    std::memmove(destination, source, count);
    return 0;
}

int RealMemset(void* destination, std::size_t destinationSize, int value, std::size_t count)
{
    if (destination == nullptr || count > destinationSize) {
        return -1;
    }
    std::memset(destination, value, count);
    return 0;
}

int RealLaunch(void*, uint32_t, const void* argsData, std::size_t argsSize, void*)
{
    ++gLaunchCalls;
    if (argsData == nullptr || argsSize != sizeof(KernelArgs)) {
        return -1;
    }
    const auto* args = static_cast<const KernelArgs*>(argsData);
    const uint8_t second = args->count == 2 ? args->values[1] : 0;
    gInputs.push_back({args->values[0], second});
    for (std::size_t index = 0; index < args->count; ++index) {
        ++args->values[index];
    }
    return 0;
}

int RealSynchronize(void*) { return 0; }
int ProfilerStart(uint32_t, const void*, uint32_t length)
{
    ++gStartCalls;
    return length == sizeof(MsprofConfig) ? 0 : -1;
}
int ProfilerStop(uint32_t, const void*, uint32_t length) { return length == sizeof(MsprofConfig) ? 0 : -1; }
int RegisterRawData(uint32_t, MsprofRawDataCallback callback) { return callback == nullptr ? -1 : 0; }

} // namespace

std::int32_t MsprofStart(uint32_t dataType, const void* config, uint32_t length)
{
    return ProfilerStart(dataType, config, length);
}

std::int32_t MsprofStop(uint32_t dataType, const void* config, uint32_t length)
{
    return ProfilerStop(dataType, config, length);
}

std::int32_t MsprofRegisterDataCallback(uint32_t type, void* function)
{
    return RegisterRawData(type, reinterpret_cast<MsprofRawDataCallback>(function));
}

class NpuComputeShadowMemory : public testing::TestWithParam<bool> {};

TEST_P(NpuComputeShadowMemory, MirrorsMemoryAndRestoresInputsAcrossReplay)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMalloc", &RealMalloc) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFree", &RealFree) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemcpy", &RealMemcpy) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemset", &RealMemset) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchKernel", &RealLaunch) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &RealSynchronize) == 0);
    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(aclrtSetDevice(0) == ACL_SUCCESS);
    const char* sections[] = {"PipeUtilization", "ResourceConflictRatio"};
    aclptiRangeProfilerSetConfigParams config{sections, 2};
    config.cacheControl = GetParam() ? ACLPTI_CACHE_CONTROL_NONE : ACLPTI_CACHE_CONTROL_ALL;
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);

    void* source = nullptr;
    void* destination = nullptr;
    ASSERT_TRUE(aclrtMalloc(&source, 4, ACL_MEM_MALLOC_HUGE_FIRST) == 0);
    ASSERT_TRUE(aclrtMalloc(&destination, 4, ACL_MEM_MALLOC_HUGE_FIRST) == 0);
    const std::array<uint8_t, 4> sourceData{1, 2, 3, 4};
    ASSERT_TRUE(aclrtMemcpy(source, 4, sourceData.data(), sourceData.size(), ACL_MEMCPY_HOST_TO_DEVICE) == 0);
    const std::array<uint8_t, 4> currentSourceData{5, 6, 7, 8};
    ASSERT_TRUE(
        RealMemcpy(source, 4, currentSourceData.data(), currentSourceData.size(), ACL_MEMCPY_HOST_TO_DEVICE) == 0);
    ASSERT_TRUE(aclrtMemset(destination, 4, 0, 4) == 0);
    ASSERT_TRUE(
        aclrtMemcpy(
            static_cast<uint8_t*>(destination) + 2, 2, static_cast<uint8_t*>(source) + 1, 2,
            ACL_MEMCPY_DEVICE_TO_DEVICE) == 0);
    KernelArgs offsetArgs{static_cast<uint8_t*>(destination) + 2, 2};
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, &offsetArgs, sizeof(offsetArgs), nullptr) == 0);
    ASSERT_TRUE(gLaunchCalls == 4);
    ASSERT_TRUE((gInputs == std::vector<std::array<uint8_t, 2>>{{6, 7}, {6, 7}, {6, 7}, {6, 7}}));
    ASSERT_TRUE(offsetArgs.values[0] == 7);
    ASSERT_TRUE(offsetArgs.values[1] == 8);
    ASSERT_TRUE(aclrtFree(source) == 0);
    ASSERT_TRUE(aclrtFree(destination) == 0);
}

INSTANTIATE_TEST_SUITE_P(Modes, NpuComputeShadowMemory, testing::Bool());
