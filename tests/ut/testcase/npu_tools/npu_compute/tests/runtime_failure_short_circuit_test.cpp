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
#include <string>

#include <unistd.h>

namespace {

constexpr int kOriginalFailure = -31;

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

bool gFailNextMalloc = false;
bool gFailNextFree = false;
bool gFailNextMemcpy = false;
bool gFailNextMemset = false;
bool gFailNextLaunch = false;
bool gFailNextStart = false;
std::size_t gMallocCalls = 0;
std::size_t gFreeCalls = 0;
std::size_t gMemcpyCalls = 0;
std::size_t gMemsetCalls = 0;
std::size_t gLaunchCalls = 0;
std::size_t gStartCalls = 0;
std::size_t gShutdownCalls = 0;
bool gFailShutdown = false;
aclError gExitStatus = ACL_SUCCESS;
std::size_t gShutdownAtExit = 0;
aclptiResult CountShutdown(void*)
{
    ++gShutdownCalls;
    return gFailShutdown ? ACLPTI_ERROR_PROFILING_FAILED : ACLPTI_SUCCESS;
}
void RuntimeCallback(void*, aclptiCallbackDomain, aclptiCallbackId, const aclptiCallbackData* data)
{
    if (data->callbackSite == ACLPTI_API_EXIT) {
        gExitStatus = data->retval;
        gShutdownAtExit = gShutdownCalls;
    }
}
std::size_t gGetDeviceCalls = 0;
std::uint8_t* gKernelValue = nullptr;

int RealMalloc(void** pointer, std::size_t size, aclrtMemMallocPolicy)
{
    ++gMallocCalls;
    if (gFailNextMalloc) {
        gFailNextMalloc = false;
        return kOriginalFailure;
    }
    *pointer = std::malloc(size);
    return *pointer == nullptr ? -1 : 0;
}

int RealFree(void* pointer)
{
    ++gFreeCalls;
    if (gFailNextFree) {
        gFailNextFree = false;
        return kOriginalFailure;
    }
    std::free(pointer);
    return 0;
}

int RealMemcpy(void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind)
{
    ++gMemcpyCalls;
    if (gFailNextMemcpy) {
        gFailNextMemcpy = false;
        return kOriginalFailure;
    }
    if (destination == nullptr || source == nullptr || count > destinationSize) {
        return -1;
    }
    std::memmove(destination, source, count);
    return 0;
}

int RealMemset(void* destination, std::size_t destinationSize, int value, std::size_t count)
{
    ++gMemsetCalls;
    if (gFailNextMemset) {
        gFailNextMemset = false;
        return kOriginalFailure;
    }
    if (destination == nullptr || count > destinationSize) {
        return -1;
    }
    std::memset(destination, value, count);
    return 0;
}

int RealLaunch(void*, uint32_t, const void*, std::size_t, void*)
{
    ++gLaunchCalls;
    if (gFailNextLaunch) {
        gFailNextLaunch = false;
        return kOriginalFailure;
    }
    if (gKernelValue != nullptr) {
        ++*gKernelValue;
    }
    return 0;
}

int RealSynchronize(void*) { return 0; }

int gBinaryTokens[16];
int gBinaryLoads = 0;
int gBinaryQueries = 0;
int gBinaryUnloads = 0;
int gFailBinaryLoad = -1;
void* gFailBinaryQuery = nullptr;
void* gFailBinaryUnload = nullptr;
aclError RealBinaryLoad(const void*, std::size_t, const aclrtBinaryLoadOptions*, aclrtBinHandle* result)
{
    const int index = gBinaryLoads++;
    if (index == gFailBinaryLoad)
        return kOriginalFailure;
    *result = &gBinaryTokens[index];
    return ACL_SUCCESS;
}
aclError RealBinaryFunction(aclrtBinHandle binary, const char*, aclrtFuncHandle* result)
{
    ++gBinaryQueries;
    if (binary == gFailBinaryQuery)
        return kOriginalFailure;
    *result = binary;
    return ACL_SUCCESS;
}
aclError RealBinaryEntry(aclrtBinHandle binary, std::uint64_t, aclrtFuncHandle* result)
{
    return RealBinaryFunction(binary, "kernel", result);
}
aclError RealBinaryUnload(aclrtBinHandle binary)
{
    if (binary == gFailBinaryUnload)
        return kOriginalFailure;
    ++gBinaryUnloads;
    return ACL_SUCCESS;
}

} // namespace

std::int32_t MsprofStart(uint32_t, const void*, uint32_t)
{
    ++gStartCalls;
    if (gFailNextStart) {
        gFailNextStart = false;
        return kOriginalFailure;
    }
    return 0;
}

std::int32_t MsprofStop(uint32_t, const void*, uint32_t) { return 0; }

std::int32_t MsprofRegisterDataCallback(uint32_t, void* callback) { return callback == nullptr ? -1 : 0; }

extern "C" aclError aclrtGetDevice(std::int32_t* deviceId)
{
    ++gGetDeviceCalls;
    if (deviceId == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *deviceId = 0;
    return ACL_SUCCESS;
}

class NpuComputeRuntimeFailure : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeRuntimeFailure, PreservesFailureAndStopsProfiling)
{
    NPU_COMPUTE_ISOLATE_TEST();
    const auto& scenario = GetParam();
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMalloc", &RealMalloc) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtFree", &RealFree) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemcpy", &RealMemcpy) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtMemset", &RealMemset) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtLaunchKernel", &RealLaunch) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &RealSynchronize) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryLoadFromData", &RealBinaryLoad) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryGetFunction", &RealBinaryFunction) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryGetFunctionByEntry", &RealBinaryEntry) == 0);
    ASSERT_TRUE(RuntimeStubSetOriginFunction("aclrtBinaryUnLoad", &RealBinaryUnload) == 0);

    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&subscriber, &RuntimeCallback, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(subscriber != nullptr);
    for (aclptiCallbackId cbid = 0; cbid < ACLPTI_RUNTIME_CBID_SIZE; ++cbid) {
        ASSERT_TRUE(aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid) == ACLPTI_SUCCESS);
    }
    ASSERT_TRUE(aclrtSetDevice(0) == ACL_SUCCESS);
    const char* sections[] = {"PipeUtilization"};
    aclptiRangeProfilerSetConfigParams config{sections, 1};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);

    ASSERT_TRUE(aclptiRegisterDataModuleShutdownCallback(&CountShutdown, nullptr) == ACLPTI_SUCCESS);
    if (scenario != "Default") {
        if (scenario.rfind("binary-", 0) == 0) {
            const char image[] = "ELF";
            aclrtBinHandle binary = nullptr;
            aclrtFuncHandle function = nullptr;
            if (scenario == "binary-disabled") {
                ASSERT_TRUE(aclrtBinaryLoadFromData(image, sizeof(image), nullptr, &binary) == ACL_SUCCESS);
                ASSERT_TRUE(gBinaryLoads == 1);
                ASSERT_TRUE(aclrtBinaryGetFunction(binary, "kernel", &function) == ACL_SUCCESS);
                ASSERT_TRUE(aclrtBinaryGetFunctionByEntry(binary, 42, &function) == ACL_SUCCESS);
                ASSERT_TRUE(gBinaryQueries == 2 && gShutdownCalls == 0);
                ASSERT_TRUE(aclrtBinaryUnLoad(binary) == ACL_SUCCESS);
                return;
            }
            config.collectPipeline = true;
            ASSERT_TRUE(aclptiRangeProfilerSetConfig(&config) == ACLPTI_SUCCESS);
            aclError status;
            if (scenario == "binary-load" || scenario == "binary-companion-load") {
                gFailBinaryLoad = scenario == "binary-load" ? 0 : 1;
                status = aclrtBinaryLoadFromData(image, sizeof(image), nullptr, &binary);
            } else {
                ASSERT_TRUE(aclrtBinaryLoadFromData(image, sizeof(image), nullptr, &binary) == ACL_SUCCESS);
                ASSERT_TRUE(gBinaryLoads == 2);
                if (scenario == "binary-unload" || scenario == "binary-companion-unload") {
                    gFailBinaryUnload = scenario == "binary-unload" ? binary : &gBinaryTokens[1];
                    status = aclrtBinaryUnLoad(binary);
                } else {
                    gFailBinaryQuery = scenario.find("companion") != std::string::npos ? &gBinaryTokens[1] : binary;
                    status = scenario.find("entry") != std::string::npos ?
                                 aclrtBinaryGetFunctionByEntry(binary, 42, &function) :
                                 aclrtBinaryGetFunction(binary, "kernel", &function);
                }
            }
            const bool auxiliary = scenario == "binary-companion-load" || scenario == "binary-companion-name" ||
                                   scenario == "binary-companion-entry" || scenario == "binary-companion-unload";
            ASSERT_TRUE(status == (auxiliary ? ACL_ERROR_PROFILING_FAILURE : kOriginalFailure));
            if (auxiliary)
                ASSERT_TRUE(binary == &gBinaryTokens[0]);
            if (scenario == "binary-companion-name" || scenario == "binary-companion-entry") {
                ASSERT_TRUE(function == binary); // Preserve the application's successful original lookup.
            }
            ASSERT_TRUE(gExitStatus == status && gShutdownAtExit == 1);
            ASSERT_TRUE(gShutdownCalls == 1);
            gFailBinaryUnload = nullptr;
            gFailBinaryQuery = nullptr;
            if (binary != nullptr)
                ASSERT_TRUE(aclrtBinaryUnLoad(binary) == ACL_SUCCESS);
            const int previousLoads = gBinaryLoads;
            ASSERT_TRUE(aclrtBinaryLoadFromData(image, sizeof(image), nullptr, &binary) == ACL_SUCCESS);
            ASSERT_TRUE(gBinaryLoads == previousLoads + 1);
            const int previousQueries = gBinaryQueries;
            ASSERT_TRUE(aclrtBinaryGetFunction(binary, "kernel", &function) == ACL_SUCCESS);
            ASSERT_TRUE(aclrtBinaryGetFunctionByEntry(binary, 42, &function) == ACL_SUCCESS);
            ASSERT_TRUE(gBinaryQueries == previousQueries + 2);
            ASSERT_TRUE(aclrtBinaryUnLoad(binary) == ACL_SUCCESS);
            ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
            ASSERT_TRUE(gLaunchCalls == 1 && gStartCalls == 0 && gShutdownCalls == 1);
            return;
        }
        void* pointer = nullptr;
        ASSERT_TRUE(aclrtMalloc(&pointer, 8, ACL_MEM_MALLOC_HUGE_FIRST) == ACL_SUCCESS);
        aclError status = ACL_SUCCESS;
        if (scenario == "malloc" || scenario == "shutdown-failure") {
            gFailShutdown = scenario == "shutdown-failure";
            gFailNextMalloc = true;
            void* failed = nullptr;
            status = aclrtMalloc(&failed, 8, ACL_MEM_MALLOC_HUGE_FIRST);
        } else if (scenario == "free") {
            gFailNextFree = true;
            status = aclrtFree(pointer);
        } else if (scenario == "memcpy") {
            gFailNextMemcpy = true;
            status = aclrtMemcpy(pointer, 8, pointer, 8, ACL_MEMCPY_HOST_TO_DEVICE);
        } else if (scenario == "memset") {
            gFailNextMemset = true;
            status = aclrtMemset(pointer, 8, 0, 8);
        } else if (scenario == "launch") {
            gFailNextLaunch = true;
            status = aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr);
        } else if (scenario == "missing") {
            ASSERT_TRUE(RuntimeStubClearOrigin("aclrtSynchronizeStream") == ACL_SUCCESS);
            ASSERT_TRUE(setenv("NPU_COMPUTE_DEBUG", "1", 1) == 0);
            std::string log;
            ASSERT_TRUE(CaptureStderr([&] { status = aclrtSynchronizeStream(nullptr); }, &log));
            ASSERT_TRUE(log.find("operation=original_lookup") != std::string::npos);
            ASSERT_TRUE(log.find("operation=runtime_call") == std::string::npos);
            ASSERT_TRUE(unsetenv("NPU_COMPUTE_DEBUG") == 0);
        } else {
            FAIL() << "Unknown failure scenario: " << scenario;
        }
        ASSERT_TRUE(status == (scenario == "missing" ? ACL_ERROR_INTERNAL_ERROR : kOriginalFailure));
        ASSERT_TRUE(gExitStatus == status && gShutdownAtExit == 1);
        ASSERT_TRUE(gShutdownCalls == 1);
        ASSERT_TRUE(gStartCalls == 0);
        const auto launches = gLaunchCalls;
        ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
        ASSERT_TRUE(gLaunchCalls == launches + 1 && gStartCalls == 0 && gShutdownCalls == 1);
        ASSERT_TRUE(aclrtFree(pointer) == ACL_SUCCESS);
        return;
    }

    void* allocation = nullptr;
    ASSERT_TRUE(aclrtMalloc(&allocation, 8, ACL_MEM_MALLOC_HUGE_FIRST) == 0);
    ASSERT_TRUE(allocation != nullptr);
    const std::uint8_t value = 7;
    const std::uint8_t initialValue = 3;
    ASSERT_TRUE(
        aclrtMemcpy(allocation, 8, &initialValue, sizeof(initialValue), ACL_MEMCPY_HOST_TO_DEVICE) == ACL_SUCCESS);

    gFailNextStart = true;
    gKernelValue = static_cast<std::uint8_t*>(allocation);
    ASSERT_TRUE(setenv("NPU_COMPUTE_DEBUG", "1", 1) == 0);
    const std::size_t launchesBeforeStartFailure = gLaunchCalls;
    const std::size_t startsBeforeStartFailure = gStartCalls;
    aclError profilingStatus = ACL_ERROR_INTERNAL_ERROR;
    std::string profilingLog;
    ASSERT_TRUE(
        CaptureStderr([&] { profilingStatus = aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr); }, &profilingLog));
    ASSERT_TRUE(profilingStatus == ACL_ERROR_INTERNAL_ERROR);
    ASSERT_TRUE(*gKernelValue == initialValue);
    ASSERT_TRUE(gLaunchCalls == launchesBeforeStartFailure + 1);
    ASSERT_TRUE(gStartCalls == startsBeforeStartFailure + 1);
    ASSERT_TRUE(gGetDeviceCalls == 1);
    ASSERT_TRUE(profilingLog.find("[aclpti] error operation=prof_start") != std::string::npos);
    ASSERT_TRUE(profilingLog.find("operation=runtime_call") != std::string::npos);
    ASSERT_TRUE(profilingLog.find("api=aclrtLaunchKernel") != std::string::npos);
    ASSERT_TRUE(profilingLog.find("operation=original_call") == std::string::npos);
    ASSERT_TRUE(profilingLog.find("status=-31") != std::string::npos);
    ASSERT_TRUE(profilingLog.find("replay=0") != std::string::npos);
    ASSERT_TRUE(profilingLog.find("round=0") != std::string::npos);
    ASSERT_TRUE(profilingLog.find("domain=") == std::string::npos);
    ASSERT_TRUE(unsetenv("NPU_COMPUTE_DEBUG") == 0);

    const std::size_t launchesBeforeRetry = gLaunchCalls;
    const std::size_t startsBeforeRetry = gStartCalls;
    ASSERT_TRUE(aclrtLaunchKernel(nullptr, 1, nullptr, 0, nullptr) == ACL_ERROR_PROFILING_FAILURE);
    ASSERT_TRUE(gLaunchCalls == launchesBeforeRetry + 1);
    ASSERT_TRUE(gStartCalls == startsBeforeRetry);
    ASSERT_TRUE(gGetDeviceCalls == 1);
    ASSERT_TRUE(*gKernelValue == initialValue + 1);
    gKernelValue = nullptr;

    gFailNextFree = true;
    const std::size_t freeCallsBeforeFailure = gFreeCalls;
    ASSERT_TRUE(aclrtFree(allocation) == kOriginalFailure);
    ASSERT_TRUE(gFreeCalls == freeCallsBeforeFailure + 1);
    ASSERT_TRUE(aclrtFree(allocation) == 0);
    ASSERT_TRUE(gFreeCalls == freeCallsBeforeFailure + 3);
    return;
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios, NpuComputeRuntimeFailure,
    testing::Values(
        "Default", "malloc", "free", "memcpy", "memset", "launch", "missing", "shutdown-failure", "binary-disabled",
        "binary-load", "binary-companion-load", "binary-name", "binary-entry", "binary-companion-name",
        "binary-companion-entry", "binary-unload", "binary-companion-unload"),
    [](const testing::TestParamInfo<std::string>& info) {
        std::string name = info.param;
        for (char& character : name) {
            if (character == '-')
                character = '_';
        }
        return name;
    });
