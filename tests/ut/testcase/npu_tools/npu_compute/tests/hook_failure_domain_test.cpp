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
#include <new>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

bool gFailNextHostAllocation = false;

} // namespace

void* operator new(std::size_t size)
{
    if (gFailNextHostAllocation) {
        gFailNextHostAllocation = false;
        throw std::bad_alloc();
    }
    void* pointer = std::malloc(size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void operator delete(void* pointer) noexcept { std::free(pointer); }

void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

constexpr aclError kMallocFailure = -41;
constexpr aclError kFreeFailure = -42;
constexpr aclError kSyncFailure = -43;
constexpr aclError kMemcpyFailure = -44;

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

std::size_t gMallocCalls = 0;
std::size_t gFreeCalls = 0;
std::size_t gMemcpyCalls = 0;
std::size_t gLaunchCalls = 0;
std::size_t gSyncCalls = 0;
std::size_t gStartCalls = 0;
std::size_t gShutdownCalls = 0;
std::size_t gFailMallocCall = 0;
std::size_t gFailFreeCall = 0;
std::size_t gFailMemcpyCall = 0;
std::size_t gFailSyncCall = 0;

aclError RealMalloc(void** pointer, std::size_t size, aclrtMemMallocPolicy)
{
    ++gMallocCalls;
    if (gMallocCalls == gFailMallocCall) {
        return kMallocFailure;
    }
    *pointer = std::malloc(size);
    return *pointer == nullptr ? ACL_ERROR_BAD_ALLOC : ACL_SUCCESS;
}

aclError RealFree(void* pointer)
{
    ++gFreeCalls;
    if (gFreeCalls == gFailFreeCall) {
        return kFreeFailure;
    }
    std::free(pointer);
    return ACL_SUCCESS;
}

aclError RealMemcpy(
    void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind)
{
    ++gMemcpyCalls;
    if (gMemcpyCalls == gFailMemcpyCall) {
        return kMemcpyFailure;
    }
    if (destination == nullptr || source == nullptr || count > destinationSize) {
        return ACL_ERROR_INVALID_PARAM;
    }
    std::memmove(destination, source, count);
    return ACL_SUCCESS;
}

aclError RealMemset(void* destination, std::size_t destinationSize, std::int32_t value, std::size_t count)
{
    if (destination == nullptr || count > destinationSize) {
        return ACL_ERROR_INVALID_PARAM;
    }
    std::memset(destination, value, count);
    return ACL_SUCCESS;
}

aclError RealLaunch(void*, std::uint32_t, const void*, std::size_t, void*)
{
    ++gLaunchCalls;
    return ACL_SUCCESS;
}

aclError RealSynchronize(void*)
{
    ++gSyncCalls;
    return gSyncCalls == gFailSyncCall ? kSyncFailure : ACL_SUCCESS;
}

aclptiResult FailingShutdownCallback(void*) { return ACLPTI_ERROR_INTERNAL; }

aclptiResult CountingShutdownCallback(void* userData)
{
    if (userData != nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    ++gShutdownCalls;
    return ACLPTI_SUCCESS;
}

void Initialize()
{
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMalloc", &RealMalloc) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFree", &RealFree) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemcpy", &RealMemcpy) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemset", &RealMemset) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchKernel", &RealLaunch) == ACL_SUCCESS);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &RealSynchronize) == ACL_SUCCESS);
    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(subscriber != nullptr);
    const char* sections[] = {"PipeUtilization"};
    aclptiRangeProfilerSetConfigParams config{sections, 1};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);
}

TEST(NpuComputeHookFailure, ShadowMallocStopsProfiling)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountingShutdownCallback, nullptr) == ACLPTI_SUCCESS);
    gFailMallocCall = 2;
    void* allocation = nullptr;
    const std::size_t freeCallsBeforeMalloc = gFreeCalls;
    aclError status = ACL_ERROR_INTERNAL_ERROR;
    std::string logText;
    ASSERT_TRUE(CaptureStderr([&] { status = aclrtMalloc(&allocation, 16, ACL_MEM_MALLOC_HUGE_FIRST); }, &logText));
    ASSERT_TRUE(status == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(allocation != nullptr);
    ASSERT_TRUE(gMallocCalls == 2);
    ASSERT_TRUE(gFreeCalls == freeCallsBeforeMalloc);
    ASSERT_TRUE(logText.find("[aclpti] error operation=shadow_malloc") != std::string::npos);
    ASSERT_TRUE(logText.find("status=-41") != std::string::npos);
    ASSERT_TRUE(logText.find("rollback_status=") == std::string::npos);
    ASSERT_TRUE(logText.find("domain=") == std::string::npos);
    ASSERT_TRUE(logText.find("consistency=") == std::string::npos);
    ASSERT_TRUE(gShutdownCalls == 1);

    const std::size_t startsBeforeLaunch = gStartCalls;
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(gLaunchCalls == 1);
    ASSERT_TRUE(gStartCalls == startsBeforeLaunch);
    ASSERT_TRUE(gShutdownCalls == 1);

    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
    ASSERT_TRUE(gFreeCalls == freeCallsBeforeMalloc + 1);
}

TEST(NpuComputeHookFailure, ReplaySynchronizeFailure)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    gFailSyncCall = 2;
    const std::size_t memcpyCallsBeforeLaunch = gMemcpyCalls;
    aclError launchStatus = ACL_ERROR_INTERNAL_ERROR;
    std::string logText;
    ASSERT_TRUE(CaptureStderr([&] { launchStatus = aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr); }, &logText));

    ASSERT_TRUE(launchStatus == ACL_ERROR_INTERNAL_ERROR);
    ASSERT_TRUE(logText.find("[aclpti] error operation=replay_sync") != std::string::npos);
    ASSERT_TRUE(logText.find("status=-43") != std::string::npos);
    ASSERT_TRUE(logText.find("domain=") == std::string::npos);
    ASSERT_TRUE(gLaunchCalls == 2);
    ASSERT_TRUE(gStartCalls == 1);
    ASSERT_TRUE(gSyncCalls == 2);
    ASSERT_TRUE(gMemcpyCalls == memcpyCallsBeforeLaunch);
    const std::size_t startsBeforeRetry = gStartCalls;
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(gLaunchCalls == 3);
    ASSERT_TRUE(gStartCalls == startsBeforeRetry);
}

TEST(NpuComputeHookFailure, InitialSynchronizeFailure)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    gFailSyncCall = 1;
    aclError launchStatus = ACL_SUCCESS;
    std::string logText;
    ASSERT_TRUE(CaptureStderr([&] { launchStatus = aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr); }, &logText));

    ASSERT_TRUE(launchStatus == ACL_ERROR_INTERNAL_ERROR);
    ASSERT_TRUE(logText.find("[aclpti] error operation=replay_initial_sync") != std::string::npos);
    ASSERT_TRUE(logText.find("status=-43") != std::string::npos);
    ASSERT_TRUE(gLaunchCalls == 1);
    ASSERT_TRUE(gStartCalls == 0);
    ASSERT_TRUE(gSyncCalls == 1);
}

TEST(NpuComputeHookFailure, ReplayInitialSynchronize)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_SUCCESS);
    ASSERT_TRUE(gLaunchCalls >= 2);
    ASSERT_TRUE(gSyncCalls == gLaunchCalls);
    ASSERT_TRUE(gStartCalls + 1 == gLaunchCalls);
    ASSERT_TRUE(gMemcpyCalls == 0);
}

TEST(NpuComputeHookFailure, ProfilingFlowFailure)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&FailingShutdownCallback, nullptr) == ACLPTI_SUCCESS);
    aclError status = ACL_ERROR_INTERNAL_ERROR;
    std::string logText;
    ASSERT_TRUE(CaptureStderr([&] { status = aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr); }, &logText));
    ASSERT_TRUE(status == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(logText.find("[aclpti] error operation=data_shutdown") != std::string::npos);
    ASSERT_TRUE(logText.find("status=8") != std::string::npos);
    ASSERT_TRUE(logText.find("domain=") == std::string::npos);
}

TEST(NpuComputeHookFailure, ShadowFreeRetry)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    void* allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 16, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    ASSERT_TRUE(gMallocCalls == 2);
    gFailFreeCall = 2;
    aclError status = ACL_ERROR_INTERNAL_ERROR;
    bool threwBadAlloc = false;
    std::string logText;
    ASSERT_TRUE(CaptureStderr(
        [&] {
            gFailNextHostAllocation = true;
            try {
                status = aclrtFree(allocation);
            } catch (const std::bad_alloc&) {
                threwBadAlloc = true;
            }
            gFailNextHostAllocation = false;
        },
        &logText));
    ASSERT_TRUE(!threwBadAlloc);
    ASSERT_TRUE(status == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(logText.find("[aclpti] error operation=shadow_free") != std::string::npos);
    ASSERT_TRUE(logText.find("status=-42") != std::string::npos);
    ASSERT_TRUE(logText.find("domain=") == std::string::npos);

    void* cleanupAllocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&cleanupAllocation, 16, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(aclrtFree(cleanupAllocation) == ACL_SUCCESS);
    ASSERT_TRUE(gFreeCalls == 4);
}

TEST(NpuComputeHookFailure, NullFreeDoesNotStopProfiling)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountingShutdownCallback, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(aclrtFree(nullptr) == ACL_SUCCESS);
    ASSERT_TRUE(gFreeCalls == 1);
    ASSERT_TRUE(gShutdownCalls == 0);

    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_SUCCESS);
    ASSERT_TRUE(gStartCalls == 2);
    ASSERT_TRUE(gShutdownCalls == 1);
}

TEST(NpuComputeHookFailure, MissingMemcpyMirrorStopsProfiling)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountingShutdownCallback, nullptr) == ACLPTI_SUCCESS);
    void* allocation = nullptr;
    ASSERT_TRUE(RealMalloc(&allocation, 16, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    std::uint8_t value = 9;
    ASSERT_TRUE(
        aclrtMemcpy(allocation, 16, &value, sizeof(value), ACL_MEMCPY_HOST_TO_DEVICE) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(*static_cast<std::uint8_t*>(allocation) == value);
    ASSERT_TRUE(gShutdownCalls == 1);

    const std::size_t startsBeforeLaunch = gStartCalls;
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(gStartCalls == startsBeforeLaunch);
    ASSERT_TRUE(gShutdownCalls == 1);
    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
}

TEST(NpuComputeHookFailure, MissingMemsetMirrorStopsProfiling)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountingShutdownCallback, nullptr) == ACLPTI_SUCCESS);
    void* allocation = nullptr;
    ASSERT_TRUE(RealMalloc(&allocation, 16, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    ASSERT_TRUE(aclrtMemset(allocation, 16, 5, 16) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(*static_cast<std::uint8_t*>(allocation) == 5);
    ASSERT_TRUE(gShutdownCalls == 1);

    const std::size_t startsBeforeLaunch = gStartCalls;
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(gStartCalls == startsBeforeLaunch);
    ASSERT_TRUE(gShutdownCalls == 1);
    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
}

TEST(NpuComputeHookFailure, MallocMissingFreeStopsProfiling)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountingShutdownCallback, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(RuntimeStubClearOrigin("aclrtFree") == ACL_SUCCESS);
    void* allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 16, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(allocation != nullptr);
    ASSERT_TRUE(gMallocCalls == 1);
    ASSERT_TRUE(gShutdownCalls == 1);

    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFree", &RealFree) == ACL_SUCCESS);
    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
    ASSERT_TRUE(gFreeCalls == 1);
}

TEST(NpuComputeHookFailure, ShadowMetadataAllocationFailure)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountingShutdownCallback, nullptr) == ACLPTI_SUCCESS);
    void* allocation = nullptr;
    aclError status = ACL_ERROR_INTERNAL_ERROR;
    bool threwBadAlloc = false;
    gFailNextHostAllocation = true;
    try {
        status = aclrtMalloc(&allocation, 16, ACL_MEM_MALLOC_HUGE_FIRST);
    } catch (const std::bad_alloc&) {
        threwBadAlloc = true;
    }
    gFailNextHostAllocation = false;

    ASSERT_TRUE(!threwBadAlloc);
    ASSERT_TRUE(status == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(allocation != nullptr);
    ASSERT_TRUE(gMallocCalls == 2);
    ASSERT_TRUE(gFreeCalls == 1);
    ASSERT_TRUE(gShutdownCalls == 1);
    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
    ASSERT_TRUE(gFreeCalls == 2);
}

TEST(NpuComputeHookFailure, ShadowMemcpyStopsProfiling)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", "1", 1), 0);
    ASSERT_NO_FATAL_FAILURE(Initialize());
    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountingShutdownCallback, nullptr) == ACLPTI_SUCCESS);

    void* allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 16, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
    std::uint8_t value = 7;
    gFailMemcpyCall = gMemcpyCalls + 2;
    aclError status = ACL_ERROR_INTERNAL_ERROR;
    std::string logText;
    ASSERT_TRUE(CaptureStderr(
        [&] { status = aclrtMemcpy(allocation, 16, &value, sizeof(value), ACL_MEMCPY_HOST_TO_DEVICE); }, &logText));
    ASSERT_TRUE(status == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(logText.find("[aclpti] error operation=shadow_memcpy status=-44") != std::string::npos);
    ASSERT_TRUE(gShutdownCalls == 1);

    const std::size_t startsBeforeLaunch = gStartCalls;
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(gLaunchCalls == 1);
    ASSERT_TRUE(gStartCalls == startsBeforeLaunch);
    ASSERT_TRUE(gShutdownCalls == 1);
    ASSERT_TRUE(aclrtFree(allocation) == ACL_SUCCESS);
}

} // namespace

std::int32_t MsprofStart(std::uint32_t, const void*, std::uint32_t)
{
    ++gStartCalls;
    return 0;
}

std::int32_t MsprofStop(std::uint32_t, const void*, std::uint32_t) { return 0; }

std::int32_t MsprofRegisterDataCallback(std::uint32_t, void* callback) { return callback == nullptr ? -1 : 0; }

extern "C" aclError aclrtGetDevice(std::int32_t* deviceId)
{
    if (deviceId == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *deviceId = 0;
    return ACL_SUCCESS;
}
