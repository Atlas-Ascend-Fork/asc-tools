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

#include <algorithm>
#include <functional>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr aclptiCacheControl kInvalidCacheControl = static_cast<aclptiCacheControl>(2);

template <typename Function>
bool CaptureStderr(Function function, std::string* output)
{
    FILE* capture = std::tmpfile();
    if (capture == nullptr) {
        return false;
    }

    const int savedStderr = dup(STDERR_FILENO);
    if (savedStderr < 0) {
        std::fclose(capture);
        return false;
    }

    bool success = std::fflush(stderr) == 0 && dup2(fileno(capture), STDERR_FILENO) >= 0;
    if (success) {
        function();
        success = std::fflush(stderr) == 0;
    }
    success = dup2(savedStderr, STDERR_FILENO) >= 0 && success;
    close(savedStderr);

    if (success) {
        std::rewind(capture);
        char buffer[256];
        std::size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), capture)) != 0) {
            output->append(buffer, count);
        }
        success = std::ferror(capture) == 0;
    }

    success = std::fclose(capture) == 0 && success;
    return success;
}

struct KernelArgs {
    uint8_t* value;
};

struct ObservedConfig {
    std::uint32_t devNums;
    std::uint32_t deviceId;
    std::uint64_t profSwitch;
    std::uint32_t primaryAttr;
    std::uint32_t instrMode;
    std::uint32_t blockMode;
    std::array<std::uint32_t, COMPUTE_AICORE_METRICS_NUM> pmuEvents;
};

std::size_t g_flush_calls = 0;
bool g_flush_failure = false;
bool g_partial_flush_failure = false;
bool g_none = false;
bool g_start_failure = false;
bool g_missing_flush = false;
std::size_t g_restore_calls = 0;
int RealFlush(void* reserved)
{
    if (reserved != nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    ++g_flush_calls;
    if (g_restore_calls != g_flush_calls) {
        return ACL_ERROR_INVALID_PARAM;
    }
    return g_flush_failure || (g_partial_flush_failure && g_flush_calls % 2 == 1) ? ACL_ERROR_RT_FAILURE : ACL_SUCCESS;
}

std::size_t g_malloc_calls = 0;
std::size_t g_free_calls = 0;
std::size_t g_memcpy_calls = 0;
std::size_t g_launch_calls = 0;
std::size_t g_launch_with_host_args_calls = 0;
std::size_t g_sync_calls = 0;
std::size_t g_set_device_calls = 0;
std::size_t g_reset_device_calls = 0;
std::size_t g_get_device_calls = 0;
std::int32_t g_current_device = -1;
std::size_t g_soc_name_calls = 0;
std::size_t g_start_calls = 0;
std::size_t g_stop_calls = 0;
uint32_t g_start_data_type = 0;
uint32_t g_stop_data_type = 0;
uint32_t g_callback_type = 0;
std::vector<uint8_t> g_kernel_inputs;
std::vector<void*> g_launch_handles;
std::size_t g_binary_loads = 0;
std::vector<void*> g_unloaded_binaries;
int g_binary_tokens[4];
int g_function_tokens[4];

int RealBinaryLoad(const void*, std::size_t, const aclrtBinaryLoadOptions*, aclrtBinHandle* binary)
{
    *binary = &g_binary_tokens[g_binary_loads++];
    return ACL_SUCCESS;
}

int RealBinaryFunction(aclrtBinHandle binary, const char* name, aclrtFuncHandle* function)
{
    if (std::strcmp(name, "sample") != 0) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *function = &g_function_tokens[static_cast<int*>(binary) - g_binary_tokens];
    return ACL_SUCCESS;
}

int RealBinaryEntry(aclrtBinHandle binary, std::uint64_t entry, aclrtFuncHandle* function)
{
    if (entry != 42) {
        return ACL_ERROR_INVALID_PARAM;
    }
    return RealBinaryFunction(binary, "sample", function);
}

int RealBinaryUnload(aclrtBinHandle binary)
{
    g_unloaded_binaries.push_back(binary);
    return ACL_SUCCESS;
}
std::vector<ObservedConfig> g_configs;
const void* g_expected_args_data = nullptr;
const void* g_started_config = nullptr;
MsprofRawDataCallback g_registered_callback = nullptr;

int RealMalloc(void** pointer, std::size_t size, aclrtMemMallocPolicy)
{
    ++g_malloc_calls;
    *pointer = std::malloc(size);
    return *pointer == nullptr ? -1 : 0;
}

int RealFree(void* pointer)
{
    ++g_free_calls;
    std::free(pointer);
    return 0;
}

int RealMemcpy(
    void* destination, std::size_t destination_size, const void* source, std::size_t count, aclrtMemcpyKind kind)
{
    if (kind == ACL_MEMCPY_DEVICE_TO_DEVICE) {
        ++g_restore_calls;
    }
    ++g_memcpy_calls;
    if (destination == nullptr || source == nullptr || count > destination_size) {
        return -1;
    }
    const auto* first = static_cast<const unsigned char*>(source);
    auto* output = static_cast<unsigned char*>(destination);
    if (std::less<const void*>{}(source, destination)) {
        std::copy_backward(first, first + count, output + count);
    } else {
        std::copy_n(first, count, output);
    }
    return 0;
}

int RealMemset(void* destination, std::size_t destination_size, int value, std::size_t count)
{
    if (destination == nullptr || count > destination_size) {
        return -1;
    }
    std::fill_n(static_cast<unsigned char*>(destination), count, static_cast<unsigned char>(value));
    return 0;
}

std::size_t g_parameter_query_calls = 0;
std::size_t g_instrumented_launch_calls = 0;

aclError MissingParameterCount(const void*, std::size_t*)
{
    ++g_parameter_query_calls;
    return 107000; // Runtime rejects legacy kernels without parameter layout metadata.
}

aclError MissingParameterInfo(const void*, std::size_t, std::size_t*, std::size_t*)
{
    ++g_parameter_query_calls;
    return 107000;
}

int RealLaunch(void* function, uint32_t, const void* args_data, std::size_t args_size, void*)
{
    ++g_launch_calls;
    g_launch_handles.push_back(function);
    if (args_data == nullptr) {
        return -1;
    }
    const bool instrumented = function == &g_function_tokens[1] || function == &g_function_tokens[3];
    if (instrumented) {
        // The instrumentation stub places the hidden pointer at byte 56, independent of argsSize.
        if (args_size != 56 + sizeof(void*) || args_data == g_expected_args_data ||
            std::memcmp(args_data, g_expected_args_data, sizeof(KernelArgs)) != 0) {
            return -1;
        }
        const auto* bytes = static_cast<const unsigned char*>(args_data);
        if (!std::all_of(bytes + sizeof(KernelArgs), bytes + 56, [](unsigned char byte) { return byte == 0; })) {
            return -1;
        }
        void* hidden = reinterpret_cast<void*>(1);
        std::copy_n(
            reinterpret_cast<const unsigned char*>(bytes + 56), sizeof(hidden),
            reinterpret_cast<unsigned char*>(&hidden));
        if (hidden != nullptr) {
            return -1;
        }
        ++g_instrumented_launch_calls;
    } else if (args_data != g_expected_args_data || args_size != sizeof(KernelArgs)) {
        return -1;
    }
    KernelArgs args{};
    std::copy_n(
        reinterpret_cast<const unsigned char*>(args_data), sizeof(args), reinterpret_cast<unsigned char*>(&args));
    g_kernel_inputs.push_back(*args.value);
    ++*args.value;
    return 0;
}

int RealLaunchWithHostArgs(
    aclrtFuncHandle function, uint32_t blocks, aclrtStream stream, aclrtLaunchKernelCfg*, void* args, std::size_t size,
    aclrtPlaceHolderInfo*, std::size_t)
{
    ++g_launch_with_host_args_calls;
    if (args != nullptr) {
        return RealLaunch(function, blocks, args, size, stream);
    }
    return 0;
}

int RealSynchronize(void*)
{
    ++g_sync_calls;
    return 0;
}

int RealSetDevice(std::int32_t deviceId)
{
    ++g_set_device_calls;
    if (deviceId < 0) {
        return -1;
    }
    g_current_device = deviceId;
    return 0;
}

int RealResetDevice(std::int32_t deviceId)
{
    ++g_reset_device_calls;
    if (deviceId < 0 || deviceId != g_current_device) {
        return -1;
    }
    g_current_device = -1;
    return 0;
}

int ProfilerStart(uint32_t, const void* config, uint32_t length)
{
    ++g_start_calls;
    if (g_start_failure) {
        return -1;
    }
    if (!g_none && !g_missing_flush && g_flush_calls != g_start_calls) {
        return -1;
    }
    if (config == nullptr || length != sizeof(MsprofConfig)) {
        return -1;
    }
    const auto* msprofConfig = static_cast<const MsprofConfig*>(config);
    const bool pipeline = msprofConfig->configInfo.attrs != nullptr &&
                          msprofConfig->configInfo.attrs[0].id == PROF_CONFIG_ATTR_INSTR &&
                          msprofConfig->configInfo.attrs[0].value.instrMode == PROF_COMPUTE_BIU_PERF;
    if (msprofConfig->configInfo.attrs == nullptr || msprofConfig->configInfo.numAttrs != (pipeline ? 1U : 2U) ||
        (!pipeline && msprofConfig->configInfo.attrs[1].id != PROF_CONFIG_ATTR_TASK_BLOCK)) {
        return -1;
    }
    ObservedConfig observed{
        msprofConfig->devNums,
        msprofConfig->devIdList[0],
        msprofConfig->profSwitch,
        msprofConfig->configInfo.attrs[0].id,
        0,
        pipeline ? 0 : msprofConfig->configInfo.attrs[1].value.taskBlockMode,
        {},
    };
    if (observed.primaryAttr == PROF_CONFIG_ATTR_AICORE_METRICS) {
        std::copy_n(
            reinterpret_cast<const unsigned char*>(msprofConfig->configInfo.attrs[0].value.aicoreMetrics),
            sizeof(observed.pmuEvents), reinterpret_cast<unsigned char*>(observed.pmuEvents.data()));
    } else if (observed.primaryAttr == PROF_CONFIG_ATTR_INSTR) {
        observed.instrMode = msprofConfig->configInfo.attrs[0].value.instrMode;
    } else {
        return -1;
    }
    g_configs.push_back(observed);
    g_started_config = config;
    return 0;
}

int ProfilerStop(uint32_t, const void* config, uint32_t length)
{
    ++g_stop_calls;
    if (config == nullptr || config != g_started_config || length != sizeof(MsprofConfig)) {
        return -1;
    }
    g_started_config = nullptr;
    return 0;
}

int RegisterRawData(uint32_t, MsprofRawDataCallback callback)
{
    g_registered_callback = callback;
    return callback == nullptr ? -1 : 0;
}

std::array<uint32_t, COMPUTE_AICORE_METRICS_NUM> ExpectedPmus(std::initializer_list<uint32_t> events)
{
    std::array<uint32_t, COMPUTE_AICORE_METRICS_NUM> result{};
    result.fill(MSPROF_INVALID_AICORE_METRIC);
    std::copy(events.begin(), events.end(), result.begin());
    return result;
}

} // namespace

std::int32_t MsprofStart(uint32_t data_type, const void* config, uint32_t length)
{
    g_start_data_type = data_type;
    return ProfilerStart(data_type, config, length);
}

std::int32_t MsprofStop(uint32_t data_type, const void* config, uint32_t length)
{
    g_stop_data_type = data_type;
    return ProfilerStop(data_type, config, length);
}

std::int32_t MsprofRegisterDataCallback(uint32_t type, void* function)
{
    g_callback_type = type;
    return RegisterRawData(type, reinterpret_cast<MsprofRawDataCallback>(function));
}

extern "C" aclError aclrtGetDevice(std::int32_t* deviceId)
{
    ++g_get_device_calls;
    if (deviceId == nullptr || g_current_device < 0) {
        return ACL_ERROR_RT_CONTEXT_NULL;
    }
    *deviceId = g_current_device;
    return ACL_SUCCESS;
}

extern "C" aclError ReplayTestFunctionGetBinary(aclrtFuncHandle function, aclrtBinHandle* binary) __asm__(
    "aclrtFunctionGetBinary");

extern "C" aclError ReplayTestFunctionGetBinary(aclrtFuncHandle function, aclrtBinHandle* binary)
{
    if (function != &g_function_tokens[0])
        return ACL_ERROR_INVALID_PARAM;
    *binary = &g_binary_tokens[0];
    return ACL_SUCCESS;
}

extern "C" aclError ReplayTestGetFunctionName(aclrtFuncHandle, uint32_t, char*) __asm__("aclrtGetFunctionName");

extern "C" aclError ReplayTestGetFunctionName(aclrtFuncHandle, uint32_t, char* name)
{
    constexpr char kSampleName[] = "sample";
    std::copy_n(kSampleName, sizeof(kSampleName), name);
    return ACL_SUCCESS;
}

extern "C" aclError ReplayTestGetFunctionAddr(const void*, void**, void**) __asm__("aclrtGetFunctionAddr");
extern "C" aclError ReplayTestGetFunctionAddr(const void*, void** aic, void** aiv)
{
    *aic = nullptr;
    *aiv = reinterpret_cast<void*>(0x100100);
    return ACL_SUCCESS;
}

class NpuComputeReplay : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeReplay, ReplaysAndShortCircuitsFailure)
{
    NPU_COMPUTE_ISOLATE_TEST();
    const auto& mode = GetParam();
    const bool useShrinkBlock = mode == "shrink";
    const bool useDebugLog = mode == "debug";
    const bool useHostArgs = mode == "host";
    const bool useSymbol = mode == "symbol";
    g_flush_failure = mode == "flush_failure";
    g_partial_flush_failure = mode == "partial_flush_failure";
    g_start_failure = mode == "start_failure";
    g_none = mode == "none";
    g_missing_flush = mode == "missing_flush";
    if (useDebugLog) {
        ASSERT_TRUE(setenv("NPU_COMPUTE_DEBUG", "1", 1) == 0);
    } else {
        ASSERT_TRUE(unsetenv("NPU_COMPUTE_DEBUG") == 0);
    }
    const aclptiBlockResultMode blockResult = useShrinkBlock ? ACLPTI_BLOCK_RESULT_SHRINK : ACLPTI_BLOCK_RESULT_ALL;
    const std::uint32_t expectedBlockMode = useShrinkBlock ? PROF_COMPUTE_BLOCK_SHRINK : PROF_COMPUTE_ALL_BLOCK;
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtDeviceL2CacheFlush", &RealFlush) == 0);
    if (g_missing_flush) {
        ASSERT_TRUE(RuntimeStubClearOrigin("aclrtDeviceL2CacheFlush") == 0);
    }
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMalloc", &RealMalloc) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMallocAlign32", &RealMalloc) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFree", &RealFree) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemcpy", &RealMemcpy) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemset", &RealMemset) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchKernel", &RealLaunch) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &RealBinaryLoad) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryGetFunction", &RealBinaryFunction) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryGetFunctionByEntry", &RealBinaryEntry) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryUnLoad", &RealBinaryUnload) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchKernelWithHostArgs", &RealLaunchWithHostArgs) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFunctionGetParamCount", &MissingParameterCount) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFunctionGetParamInfo", &MissingParameterInfo) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtSetDevice", &RealSetDevice) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtResetDevice", &RealResetDevice) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &RealSynchronize) == 0);

    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(subscriber != nullptr);
    ASSERT_TRUE(g_registered_callback != nullptr);
    ASSERT_TRUE(g_callback_type == 0);
    ASSERT_TRUE(reinterpret_cast<aclrtMallocFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMalloc)) == &RealMalloc);
    ASSERT_TRUE(
        reinterpret_cast<aclrtMallocAlign32Func>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMallocAlign32)) ==
        &RealMalloc);
    ASSERT_TRUE(reinterpret_cast<aclrtFreeFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFree)) == &RealFree);
    ASSERT_TRUE(reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy)) == &RealMemcpy);
    ASSERT_TRUE(reinterpret_cast<aclrtMemsetFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemset)) == &RealMemset);
    ASSERT_TRUE(
        reinterpret_cast<aclrtLaunchKernelFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtLaunchKernel)) ==
        &RealLaunch);
    ASSERT_TRUE(
        reinterpret_cast<aclrtLaunchKernelWithHostArgsFunc>(
            acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtLaunchKernelWithHostArgs)) == &RealLaunchWithHostArgs);
    ASSERT_TRUE(
        reinterpret_cast<aclrtSynchronizeStreamFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtSynchronizeStream)) ==
        &RealSynchronize);
    ASSERT_TRUE(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtSetDevice) != nullptr);
    ASSERT_TRUE(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtLaunchKernelWithHostArgs) != nullptr);

    aclptiSubscribeHandle repeated_subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&repeated_subscriber, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(repeated_subscriber == subscriber);

    ASSERT_TRUE(aclptiActivityEnable(subscriber, ACLPTI_ACTIVITY_KIND_FULL, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(aclptiActivityEnable(subscriber, ACLPTI_ACTIVITY_KIND_FULL, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(
        aclptiActivityEnable(subscriber, ACLPTI_ACTIVITY_KIND_INVALID, nullptr) == ACLPTI_ERROR_INVALID_PARAMETER);
    aclptiActivityConfig invalid_activity_config{1};
    ASSERT_TRUE(
        aclptiActivityEnable(subscriber, ACLPTI_ACTIVITY_KIND_FULL, &invalid_activity_config) ==
        ACLPTI_ERROR_INVALID_PARAMETER);
    ASSERT_TRUE(aclptiActivityDisable(subscriber, ACLPTI_ACTIVITY_KIND_FULL, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(aclptiActivityDisable(subscriber, ACLPTI_ACTIVITY_KIND_FULL, nullptr) == ACLPTI_SUCCESS);
    const char* sections[] = {"PipeUtilization", "ResourceConflictRatio"};
    aclptiRangeProfilerSetConfigParams config{sections, std::size(sections), blockResult, true, true};
    auto invalidCacheConfig = config;
    invalidCacheConfig.cacheControl = kInvalidCacheControl;
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&invalidCacheConfig) == ACLPTI_ERROR_INVALID_PARAMETER);
    if (!g_none) {
        config.cacheControl = ACLPTI_CACHE_CONTROL_ALL;
    }
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);
    aclptiRangeProfilerSetConfigParams invalidConfig{nullptr, 0, ACLPTI_BLOCK_RESULT_ALL, false, false};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&invalidConfig) == ACLPTI_ERROR_INVALID_PARAMETER);

    const char binaryImage[] = "sample ELF (toolchain stub input)";
    aclrtBinHandle binary = nullptr;
    ASSERT_TRUE(aclrtBinaryLoadFromData(binaryImage, sizeof(binaryImage), nullptr, &binary) == ACL_SUCCESS);
    ASSERT_TRUE(binary == &g_binary_tokens[0]);
    ASSERT_TRUE(g_binary_loads == 2);
    aclrtFuncHandle namedFunction = nullptr;
    if (useSymbol) {
        // Like Bisheng's generated host stub: symbol lookup is the only function query.
        ASSERT_TRUE(aclrtGetFuncBySymbol(&g_function_tokens[0], &namedFunction) == ACL_SUCCESS);
    } else {
        ASSERT_TRUE(aclrtBinaryGetFunction(binary, "sample", &namedFunction) == ACL_SUCCESS);
    }
    ASSERT_TRUE(namedFunction == &g_function_tokens[0]);
    // The second binary deliberately reuses Entry 42; it must retain its own mapping.
    aclrtBinHandle entryBinary = nullptr;
    ASSERT_TRUE(aclrtBinaryLoadFromData(binaryImage, sizeof(binaryImage), nullptr, &entryBinary) == ACL_SUCCESS);
    aclrtFuncHandle entryFunction = nullptr;
    ASSERT_TRUE(aclrtBinaryGetFunctionByEntry(entryBinary, 42, &entryFunction) == ACL_SUCCESS);
    ASSERT_TRUE(entryFunction == &g_function_tokens[2]);

    void* alignedAllocation = nullptr;
    ASSERT_TRUE(aclrtMallocAlign32(&alignedAllocation, 1, ACL_MEM_MALLOC_HUGE_FIRST) == 0);
    uint8_t alignedValue = 3;
    ASSERT_TRUE(aclrtMemcpy(alignedAllocation, 1, &alignedValue, 1, ACL_MEMCPY_HOST_TO_DEVICE) == 0);
    ASSERT_TRUE(aclrtFree(alignedAllocation) == 0);

    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);
    void* allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 1, ACL_MEM_MALLOC_HUGE_FIRST) == 0);
    uint8_t initial_value = 5;
    ASSERT_TRUE(aclrtMemcpy(allocation, 1, &initial_value, 1, ACL_MEMCPY_HOST_TO_DEVICE) == 0);

    KernelArgs args{static_cast<uint8_t*>(allocation)};
    g_expected_args_data = &args;

    ASSERT_TRUE(aclrtSetDevice(7) == 0);
    ASSERT_TRUE(g_set_device_calls == 1);
    std::string profilingLog;
    aclError firstLaunchStatus = ACL_ERROR_RT_FAILURE;
    ASSERT_TRUE(CaptureStderr(
        [&] {
            auto function = useShrinkBlock ? entryFunction : namedFunction;
            firstLaunchStatus = useHostArgs ? aclrtLaunchKernelWithHostArgs(
                                                  function, 1, nullptr, nullptr, &args, sizeof(args), nullptr, 0) :
                                              aclrtLaunchKernel(function, 1, &args, sizeof(args), nullptr);
        },
        &profilingLog));
    ASSERT_TRUE(profilingLog.find("[INFO] cache-control") == std::string::npos);
    if (g_start_failure) {
        ASSERT_TRUE(firstLaunchStatus != ACL_SUCCESS);
        ASSERT_TRUE(g_flush_calls == 1 && g_start_calls == 1 && g_launch_calls == 1 && g_stop_calls == 0);
        ASSERT_TRUE(profilingLog.find("cache-control") == std::string::npos);
        ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
        return;
    }
    ASSERT_TRUE(firstLaunchStatus == ACL_SUCCESS);
    ASSERT_TRUE(g_parameter_query_calls == 0);
    ASSERT_TRUE(g_instrumented_launch_calls == 2);
    ASSERT_TRUE(g_flush_calls == (g_none || g_missing_flush ? 0U : 5U));
    if (g_none) {
        ASSERT_TRUE(profilingLog.find("cache-control") == std::string::npos);
    } else if (g_missing_flush) {
        ASSERT_TRUE(profilingLog.find("cache-control") == std::string::npos);
    } else {
        ASSERT_TRUE(
            (profilingLog.find("[WARNING] cache-control=all") != std::string::npos) ==
            (g_flush_failure || g_partial_flush_failure));
        if (g_flush_failure || g_partial_flush_failure) {
            ASSERT_TRUE(profilingLog.find("L2 flush failed; continue profiling") != std::string::npos);
        }
    }
    ASSERT_TRUE(g_start_calls == 5);
    ASSERT_TRUE(g_stop_calls == 5);
    ASSERT_TRUE(g_start_data_type == 8);
    ASSERT_TRUE(g_stop_data_type == 8);
    ASSERT_TRUE(g_launch_calls == 6);
    void* expectedOriginal = useShrinkBlock ? &g_function_tokens[2] : &g_function_tokens[0];
    void* expectedPatched = useShrinkBlock ? &g_function_tokens[3] : &g_function_tokens[1];
    ASSERT_TRUE(
        (g_launch_handles == std::vector<void*>{
                                 expectedOriginal, expectedOriginal, expectedOriginal, expectedOriginal,
                                 expectedPatched, expectedPatched}));
    ASSERT_TRUE(aclrtBinaryGetFunctionByEntry(entryBinary, 43, &entryFunction) == ACL_ERROR_INVALID_PARAM);
    ASSERT_TRUE(aclrtBinaryUnLoad(binary) == ACL_SUCCESS);
    ASSERT_TRUE(aclrtBinaryUnLoad(entryBinary) == ACL_SUCCESS);
    ASSERT_TRUE(g_unloaded_binaries.size() == 4);
    ASSERT_TRUE(g_sync_calls == 6);
    ASSERT_TRUE(g_get_device_calls == 1);
    ASSERT_TRUE(g_soc_name_calls == 0);
    ASSERT_TRUE((g_kernel_inputs == std::vector<std::uint8_t>{5, 5, 5, 5, 5, 5}));
    ASSERT_TRUE(*static_cast<std::uint8_t*>(allocation) == 6);

    ASSERT_TRUE(g_configs.size() == 5);
    // Each PMU replay uses all ten hardware slots before starting the next round.
    const auto expected_first = ExpectedPmus({0, 1, 10, 36, 52, 53, 514, 515, 810, 1281});
    const auto expected_second = ExpectedPmus({1794, 1812, 1813, 11, 12, 13, 14, 15, 1344, 1366});
    const auto expected_third = ExpectedPmus({1376, 1377, 1378, 1379});
    ASSERT_TRUE(g_configs[0].pmuEvents == expected_first);
    ASSERT_TRUE(g_configs[0].pmuEvents[0] == 0);
    ASSERT_TRUE(g_configs[0].pmuEvents[COMPUTE_AICORE_METRICS_NUM - 1] == 1281);
    ASSERT_TRUE(g_configs[0].profSwitch == (PROF_TASK_TIME_MASK | PROF_AICORE_METRICS_MASK));
    ASSERT_TRUE(g_configs[0].primaryAttr == PROF_CONFIG_ATTR_AICORE_METRICS);
    ASSERT_TRUE(g_configs[0].blockMode == expectedBlockMode);
    ASSERT_TRUE(g_configs[0].devNums == 1);
    ASSERT_TRUE(g_configs[0].deviceId == 7);
    ASSERT_TRUE(g_configs[1].pmuEvents == expected_second);
    ASSERT_TRUE(g_configs[1].pmuEvents[0] == 1794);
    ASSERT_TRUE(g_configs[1].pmuEvents[COMPUTE_AICORE_METRICS_NUM - 1] == 1366);
    ASSERT_TRUE(g_configs[1].profSwitch == (PROF_TASK_TIME_MASK | PROF_AICORE_METRICS_MASK));
    ASSERT_TRUE(g_configs[1].primaryAttr == PROF_CONFIG_ATTR_AICORE_METRICS);
    ASSERT_TRUE(g_configs[1].blockMode == expectedBlockMode);
    ASSERT_TRUE(g_configs[1].devNums == 1);
    ASSERT_TRUE(g_configs[1].deviceId == 7);
    ASSERT_TRUE(g_configs[2].pmuEvents == expected_third);
    ASSERT_TRUE(g_configs[2].pmuEvents[0] == 1376);
    ASSERT_TRUE(g_configs[2].pmuEvents[4] == MSPROF_INVALID_AICORE_METRIC);
    ASSERT_TRUE(g_configs[2].profSwitch == (PROF_TASK_TIME_MASK | PROF_AICORE_METRICS_MASK));
    ASSERT_TRUE(g_configs[2].primaryAttr == PROF_CONFIG_ATTR_AICORE_METRICS);
    ASSERT_TRUE(g_configs[2].blockMode == expectedBlockMode);
    ASSERT_TRUE(g_configs[2].devNums == 1);
    ASSERT_TRUE(g_configs[2].deviceId == 7);
    ASSERT_TRUE(g_configs[3].profSwitch == (PROF_TASK_TIME_MASK | PROF_INSTR_MASK));
    ASSERT_TRUE(g_configs[3].primaryAttr == PROF_CONFIG_ATTR_INSTR);
    ASSERT_TRUE(g_configs[3].instrMode == PROF_COMPUTE_BIU_PERF);
    ASSERT_TRUE(g_configs[3].blockMode == 0);
    ASSERT_TRUE(g_configs[4].profSwitch == (PROF_TASK_TIME_MASK | PROF_INSTR_MASK));
    ASSERT_TRUE(g_configs[4].primaryAttr == PROF_CONFIG_ATTR_INSTR);
    ASSERT_TRUE(g_configs[4].instrMode == PROF_COMPUTE_PC_SAMPLING);
    ASSERT_TRUE(g_configs[4].blockMode == expectedBlockMode);

    if (useDebugLog) {
        ASSERT_TRUE(profilingLog.find("[aclpti] msprof config round=0 collectionType=8") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("profSwitch=0x804") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("devNums=1") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("devId[0]=7") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("dumpPath=\"\" dumpPathLength=0") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("sampleConfig=\"\" sampleConfigLength=0") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("configInfo.attrs=") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("configInfo.numAttrs=2") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("attr[0] id=0 aicoreMetrics[0]=0") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("aicoreMetrics[9]=1281") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("round=1 attr[0] id=0 aicoreMetrics[0]=1794") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("attr[1] id=2 taskBlockMode=1") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("attr[0] id=1 instrMode=1") != std::string::npos);
        ASSERT_TRUE(profilingLog.find("attr[0] id=1 instrMode=2") != std::string::npos);
    } else {
        ASSERT_TRUE(profilingLog.find("msprof config round=") == std::string::npos);
    }

    const std::size_t starts_after_first_launch = g_start_calls;
    const std::size_t syncs_after_first_launch = g_sync_calls;
    initial_value = 10;
    ASSERT_TRUE(
        aclrtMemcpy(allocation, 1, &initial_value, 1, ACL_MEMCPY_HOST_TO_DEVICE) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, &args, sizeof(args), nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(g_start_calls == starts_after_first_launch);
    ASSERT_TRUE(g_sync_calls == syncs_after_first_launch);
    ASSERT_TRUE(g_launch_calls == 7);
    ASSERT_TRUE(g_get_device_calls == 1);
    ASSERT_TRUE(g_kernel_inputs.back() == 10);
    ASSERT_TRUE(*static_cast<std::uint8_t*>(allocation) == 11);

    const std::size_t starts_before_host_args = g_start_calls;
    const std::size_t syncs_before_host_args = g_sync_calls;
    ASSERT_TRUE(
        aclrtLaunchKernelWithHostArgs(nullptr, 1, nullptr, nullptr, nullptr, 0, nullptr, 0) ==
        ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(g_launch_with_host_args_calls == (useHostArgs ? 7U : 1U));
    ASSERT_TRUE(g_get_device_calls == 1);
    ASSERT_TRUE(g_start_calls == starts_before_host_args);
    ASSERT_TRUE(g_sync_calls == syncs_before_host_args);

    ASSERT_TRUE(aclrtResetDevice(7) == 0);
    ASSERT_TRUE(g_reset_device_calls == 1);
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, &args, sizeof(args), nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(g_get_device_calls == 1);
    ASSERT_TRUE(g_start_calls == starts_before_host_args);
    ASSERT_TRUE(g_sync_calls == syncs_before_host_args);
    ASSERT_TRUE(aclrtFree(allocation) == 0);
    ASSERT_TRUE(g_malloc_calls == 4);
    ASSERT_TRUE(g_free_calls == 4);
}

INSTANTIATE_TEST_SUITE_P(
    Modes, NpuComputeReplay,
    testing::Values(
        "Default", "symbol", "debug", "shrink", "host", "flush_failure", "partial_flush_failure", "missing_flush",
        "none", "start_failure"),
    [](const testing::TestParamInfo<std::string>& info) { return info.param; });
