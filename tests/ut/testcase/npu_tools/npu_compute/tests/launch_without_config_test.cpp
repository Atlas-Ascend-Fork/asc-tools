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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::size_t g_launch_calls = 0;
std::size_t g_start_calls = 0;
std::size_t g_stop_calls = 0;

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

int RealMemcpy(void* destination, std::size_t destination_size, const void* source, std::size_t count, aclrtMemcpyKind)
{
    if (destination == nullptr || source == nullptr || count > destination_size) {
        return -1;
    }
    std::memmove(destination, source, count);
    return 0;
}

int RealMemset(void* destination, std::size_t destination_size, int value, std::size_t count)
{
    if (destination == nullptr || count > destination_size) {
        return -1;
    }
    std::memset(destination, value, count);
    return 0;
}

int RealLaunch(void*, uint32_t, const void*, std::size_t, void*)
{
    ++g_launch_calls;
    return 0;
}

int RealSynchronize(void*) { return 0; }

int ProfilerStart(uint32_t, const void* config, uint32_t length)
{
    ++g_start_calls;
    if (config == nullptr || length != sizeof(MsprofConfig)) {
        return -1;
    }
    return 0;
}

int ProfilerStop(uint32_t, const void* config, uint32_t length)
{
    ++g_stop_calls;
    return config != nullptr && length == sizeof(MsprofConfig) ? 0 : -1;
}
int RegisterRawData(uint32_t, MsprofRawDataCallback callback) { return callback == nullptr ? -1 : 0; }

} // namespace

std::int32_t MsprofStart(uint32_t data_type, const void* config, uint32_t length)
{
    return ProfilerStart(data_type, config, length);
}

std::int32_t MsprofStop(uint32_t data_type, const void* config, uint32_t length)
{
    return ProfilerStop(data_type, config, length);
}

std::int32_t MsprofRegisterDataCallback(uint32_t type, void* function)
{
    return RegisterRawData(type, reinterpret_cast<MsprofRawDataCallback>(function));
}

TEST(NpuComputeLaunchWithoutConfig, LaunchesWithoutStartingProfiling)
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
    ASSERT_TRUE(subscriber != nullptr);
    ASSERT_TRUE(aclrtSetDevice(0) == ACL_SUCCESS);

    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == 0);
    ASSERT_TRUE(g_launch_calls == 1);
    ASSERT_TRUE(g_start_calls == 0);
    ASSERT_TRUE(g_stop_calls == 0);
}
