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
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"
#include "profiling/prof_api.h"

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

struct NormalLaunch {
    void* func;
    uint32_t numBlocks;
    aclrtStream stream;
    aclrtLaunchKernelCfg* cfg;
    void** args;
};

struct SimtLaunch {
    void* func;
    dim3 gridDim;
    dim3 blockDim;
    std::size_t dynUbufSize;
    aclrtStream stream;
    aclrtLaunchKernelCfg* cfg;
    void** args;
};

std::vector<NormalLaunch> gNormalLaunches;
std::vector<SimtLaunch> gSimtLaunches;
std::size_t gStartCalls = 0;
std::size_t gStopCalls = 0;
std::size_t gSyncCalls = 0;

aclError RealNormalLaunch(void* func, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void** args)
{
    gNormalLaunches.push_back({func, numBlocks, stream, cfg, args});
    return ACL_SUCCESS;
}

aclError RealSimtLaunch(
    void* func, dim3 gridDim, dim3 blockDim, std::size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void** args)
{
    gSimtLaunches.push_back({func, gridDim, blockDim, dynUbufSize, stream, cfg, args});
    return ACL_SUCCESS;
}

aclError RealSynchronize(aclrtStream)
{
    ++gSyncCalls;
    return ACL_SUCCESS;
}

void CheckNormalReplay()
{
    int functionToken = 0;
    std::uint32_t firstArg = 11;
    std::uint64_t secondArg = 12;
    void* args[] = {&firstArg, &secondArg};
    aclrtLaunchKernelCfg cfg{};
    const auto stream = reinterpret_cast<aclrtStream>(0x10);

    ASSERT_TRUE(aclrtLaunchKernelWithArgsArray(&functionToken, 8, stream, &cfg, args) == ACL_SUCCESS);
    ASSERT_TRUE(gNormalLaunches.size() == 3);
    for (const auto& launch : gNormalLaunches) {
        ASSERT_TRUE(launch.func == &functionToken);
        ASSERT_TRUE(launch.numBlocks == 8);
        ASSERT_TRUE(launch.stream == stream);
        ASSERT_TRUE(launch.cfg == &cfg);
        ASSERT_TRUE(launch.args == args);
    }
}

void CheckSimtReplay()
{
    int functionToken = 0;
    std::uint32_t firstArg = 11;
    std::uint64_t secondArg = 12;
    void* args[] = {&firstArg, &secondArg};
    dim3 gridDim{2, 3, 4};
    dim3 blockDim{5, 6, 7};
    aclrtLaunchKernelCfg cfg{};
    const auto stream = reinterpret_cast<aclrtStream>(0x20);

    ASSERT_TRUE(
        aclrtLaunchSIMTKernelWithArgsArray(&functionToken, gridDim, blockDim, 4096, stream, &cfg, args) == ACL_SUCCESS);
    ASSERT_TRUE(gSimtLaunches.size() == 3);
    for (const auto& launch : gSimtLaunches) {
        ASSERT_TRUE(launch.func == &functionToken);
        ASSERT_TRUE(launch.gridDim.x == 2 && launch.gridDim.y == 3 && launch.gridDim.z == 4);
        ASSERT_TRUE(launch.blockDim.x == 5 && launch.blockDim.y == 6 && launch.blockDim.z == 7);
        ASSERT_TRUE(launch.dynUbufSize == 4096);
        ASSERT_TRUE(launch.stream == stream);
        ASSERT_TRUE(launch.cfg == &cfg);
        ASSERT_TRUE(launch.args == args);
    }
}

} // namespace

std::int32_t MsprofStart(uint32_t, const void*, uint32_t)
{
    ++gStartCalls;
    return 0;
}

std::int32_t MsprofStop(uint32_t, const void*, uint32_t)
{
    ++gStopCalls;
    return 0;
}

std::int32_t MsprofRegisterDataCallback(uint32_t, void* callback) { return callback == nullptr ? -1 : 0; }

class NpuComputeArgsArrayReplay : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeArgsArrayReplay, PreservesArgumentsAcrossReplay)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchKernelWithArgsArray", &RealNormalLaunch) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchSIMTKernelWithArgsArray", &RealSimtLaunch) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &RealSynchronize) == ACL_SUCCESS);

    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(subscriber != nullptr);
    ASSERT_TRUE(
        reinterpret_cast<aclrtLaunchKernelWithArgsArrayFunc>(
            acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtLaunchKernelWithArgsArray)) == &RealNormalLaunch);
    ASSERT_TRUE(
        reinterpret_cast<aclrtLaunchSIMTKernelWithArgsArrayFunc>(
            acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray)) == &RealSimtLaunch);
    ASSERT_TRUE(aclrtSetDevice(7) == ACL_SUCCESS);

    const char* sections[] = {"PipeUtilization"};
    aclptiRangeProfilerSetConfigParams config{sections, 1};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);

    if (GetParam() == "normal") {
        ASSERT_NO_FATAL_FAILURE(CheckNormalReplay());
    } else {
        ASSERT_NO_FATAL_FAILURE(CheckSimtReplay());
    }
    ASSERT_TRUE(gStartCalls == 2);
    ASSERT_TRUE(gStopCalls == 2);
    ASSERT_TRUE(gSyncCalls == 3);
}

INSTANTIATE_TEST_SUITE_P(
    Modes, NpuComputeArgsArrayReplay, testing::Values("normal", "simt"),
    [](const testing::TestParamInfo<std::string>& info) { return info.param; });
