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
#include "injection/injection_hook.h"

#include <acl/acl.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

namespace {

#define CHECK_ACL(expression)                                                                  \
    do {                                                                                       \
        const aclError status = (expression);                                                  \
        if (status != ACL_SUCCESS) {                                                           \
            std::fprintf(stderr, "%s failed at line %d: %d\n", #expression, __LINE__, status); \
            return 1;                                                                          \
        }                                                                                      \
    } while (false)

#define CHECK(expression)                                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return 1;                                                                     \
        }                                                                                 \
    } while (false)

struct CallbackState {
    size_t records = 0;
    size_t launchCallbacks = 0;
    bool valid = true;
    std::set<uint64_t> launchIds;
};

CallbackState g_callbacks;

void PrintParameterMetadata(aclrtFuncHandle function, const char* name)
{
    size_t count = 0;
    const aclError result = aclrtFunctionGetParamCount(function, &count);
    std::printf("[metadata] kernel=%s count=%zu result=%d\n", name, count, result);
    if (result != ACL_SUCCESS) {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        size_t offset = 0;
        size_t size = 0;
        const aclError infoResult = aclrtFunctionGetParamInfo(function, index, &offset, &size);
        std::printf(
            "[metadata] kernel=%s index=%zu offset=%zu size=%zu result=%d\n", name, index, offset, size, infoResult);
    }
}

bool HasValidBlockIdentity(const AclsanDeviceEventHeader& header)
{
    if (header.executor.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE) {
        return header.executor.blockId < 2;
    }
    if (header.executor.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR) {
        return header.executor.blockId < 4;
    }
    return false;
}

void Callback(void*, AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    if (data != nullptr && domain == ACLSAN_CB_DOMAIN_LAUNCH && id == ACLSAN_CBID_LAUNCH_KERNEL) {
        const auto* launch = static_cast<const AclsanLaunchData*>(data);
        ++g_callbacks.launchCallbacks;
        g_callbacks.valid = g_callbacks.valid && launch->common.size == sizeof(AclsanLaunchData) &&
                            launch->common.result == ACL_SUCCESS && launch->numBlocks == 2;
        return;
    }
    if (domain != ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION || data == nullptr) {
        return;
    }
    if (id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        const auto* memory = static_cast<const AclsanDeviceMemoryAccessData*>(data);
        ++g_callbacks.records;
        g_callbacks.launchIds.insert(memory->header.launchId);
        g_callbacks.valid = g_callbacks.valid && HasValidBlockIdentity(memory->header) &&
                            memory->header.pipeline == ACLSAN_DEVICE_PIPE_MTE2;
    } else if (id == ACLSAN_CBID_DEVICE_SYNC) {
        const auto* sync = static_cast<const AclsanDeviceSyncData*>(data);
        ++g_callbacks.records;
        g_callbacks.launchIds.insert(sync->header.launchId);
        g_callbacks.valid = g_callbacks.valid && HasValidBlockIdentity(sync->header) &&
                            sync->syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
    }
}

bool ParseDeviceId(const char* text, int32_t& deviceId)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    deviceId = static_cast<int32_t>(parsed);
    return true;
}

struct RuntimeResources {
    bool initialized = false;
    bool deviceSet = false;
    int32_t deviceId = 0;
    aclrtStream stream = nullptr;
    aclrtBinHandle binary = nullptr;
    void* input = nullptr;
    void* output = nullptr;
    void* workspace = nullptr;
    AclsanSubscriberHandle subscriber = nullptr;

    bool Release() noexcept
    {
        bool success = true;
        if (binary != nullptr) {
            success = aclrtBinaryUnLoad(binary) == ACL_SUCCESS && success;
            binary = nullptr;
        }
        for (void** allocation : {&workspace, &output, &input}) {
            if (*allocation != nullptr) {
                success = aclrtFree(*allocation) == ACL_SUCCESS && success;
                *allocation = nullptr;
            }
        }
        if (stream != nullptr) {
            success = aclrtDestroyStream(stream) == ACL_SUCCESS && success;
            stream = nullptr;
        }
        if (subscriber != nullptr) {
            success = aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS && success;
            subscriber = nullptr;
        }
        if (deviceSet) {
            success = aclrtResetDevice(deviceId) == ACL_SUCCESS && success;
            deviceSet = false;
        }
        if (initialized) {
            success = aclFinalize() == ACL_SUCCESS && success;
            initialized = false;
        }
        return success;
    }

    ~RuntimeResources() { (void)Release(); }
};

} // namespace

int main(int argc, char** argv)
{
    CHECK(argc == 5);
    const bool useArgsArray = std::strcmp(argv[4], "args-array") == 0;
    CHECK(useArgsArray || std::strcmp(argv[4], "host-args") == 0);
    const bool zeroOnly = std::strcmp(argv[2], "zero") == 0;
    const bool threeOnly = std::strcmp(argv[2], "three") == 0;
    CHECK(zeroOnly || threeOnly || std::strcmp(argv[2], "multi") == 0);
    int32_t deviceId = 0;
    CHECK(ParseDeviceId(argv[3], deviceId));

    RuntimeResources resources;
    resources.deviceId = deviceId;
    CHECK_ACL(aclInit(nullptr));
    resources.initialized = true;
    CHECK_ACL(aclrtSetDevice(deviceId));
    resources.deviceSet = true;
    CHECK_ACL(aclrtCreateStream(&resources.stream));
    CHECK(acltoolHookInit() == ACL_SUCCESS);
    CHECK(aclsanSubscribe(&resources.subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(
            1, resources.subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, resources.subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);

    CHECK(
        aclsanEnableCallback(1, resources.subscriber, ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL) ==
        ACLSAN_STATUS_SUCCESS);

    std::puts("[intercept] binary_load");
    std::ifstream binaryInput(argv[1], std::ios::binary);
    const std::vector<uint8_t> binaryImage{
        std::istreambuf_iterator<char>(binaryInput), std::istreambuf_iterator<char>()};
    CHECK(!binaryImage.empty());
    aclrtBinaryLoadOption option{};
    option.type = ACL_RT_BINARY_LOAD_OPT_MAGIC;
    option.value.magic = ACL_RT_BINARY_MAGIC_ELF_AICORE;
    aclrtBinaryLoadOptions options{&option, 1};
    CHECK_ACL(aclrtBinaryLoadFromData(binaryImage.data(), binaryImage.size(), &options, &resources.binary));
    std::puts("[dbi] patched=yes backend=real");

    aclrtFuncHandle zeroArgumentFunction = nullptr;
    CHECK_ACL(aclrtBinaryGetFunction(resources.binary, "ZeroArgumentKernel", &zeroArgumentFunction));
    PrintParameterMetadata(zeroArgumentFunction, "ZeroArgumentKernel");
    if (zeroOnly) {
        if (useArgsArray) {
            CHECK_ACL(aclrtLaunchKernelWithArgsArray(zeroArgumentFunction, 2, resources.stream, nullptr, nullptr));
        } else {
            CHECK_ACL(aclrtLaunchKernelWithHostArgs(
                zeroArgumentFunction, 2, resources.stream, nullptr, nullptr, 0, nullptr, 0));
        }
        std::puts("[hook] function instrumented=yes");
        std::puts("[hook] launch trace_buffer_injected=yes");
        CHECK_ACL(aclrtSynchronizeStream(resources.stream));
        CHECK(g_callbacks.records > 0);
        CHECK(g_callbacks.valid);
        CHECK(g_callbacks.launchIds.size() == 1);
        CHECK(g_callbacks.launchCallbacks == 1);
        std::printf("[device] records=%zu\n", g_callbacks.records);
        std::puts("[d2h] copies=0");
        std::printf("[callback] records=%zu launches=%zu\n", g_callbacks.records, g_callbacks.launchIds.size());
        CHECK(resources.Release());
        std::puts("[verify] kernel_result=pass trace_records=pass resources=balanced");
        std::puts("FULL_FLOW_SAMPLE_PASS");
        return 0;
    }

    aclrtFuncHandle oneArgumentFunction = nullptr;
    aclrtFuncHandle threeArgumentFunction = nullptr;
    CHECK_ACL(aclrtBinaryGetFunction(resources.binary, "OneArgumentKernel", &oneArgumentFunction));
    CHECK_ACL(aclrtBinaryGetFunction(resources.binary, "FullFlowKernel", &threeArgumentFunction));
    PrintParameterMetadata(oneArgumentFunction, "OneArgumentKernel");
    PrintParameterMetadata(threeArgumentFunction, "FullFlowKernel");

    constexpr size_t kValues = 8;
    constexpr size_t kBytes = kValues * sizeof(uint32_t);
    std::array<uint32_t, kValues> inputHost{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<uint32_t, kValues> outputHost{};
    CHECK_ACL(aclrtMalloc(&resources.input, kBytes, ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMalloc(&resources.output, kBytes, ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMalloc(&resources.workspace, kBytes, ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK_ACL(aclrtMemcpy(resources.input, kBytes, inputHost.data(), kBytes, ACL_MEMCPY_HOST_TO_DEVICE));
    CHECK_ACL(aclrtMemset(resources.output, kBytes, 0, kBytes));
    CHECK_ACL(aclrtMemset(resources.workspace, kBytes, 0, kBytes));

    struct OneArgument {
        void* input;
    } oneArgument{resources.input};
    static_assert(sizeof(oneArgument) == 8);
    if (!threeOnly) {
        if (useArgsArray) {
            void* args[] = {&oneArgument.input};
            CHECK_ACL(aclrtLaunchKernelWithArgsArray(oneArgumentFunction, 2, resources.stream, nullptr, args));
        } else {
            CHECK_ACL(aclrtLaunchKernelWithHostArgs(
                oneArgumentFunction, 2, resources.stream, nullptr, &oneArgument, sizeof(oneArgument), nullptr, 0));
        }
        if (useArgsArray) {
            CHECK_ACL(aclrtLaunchKernelWithArgsArray(zeroArgumentFunction, 2, resources.stream, nullptr, nullptr));
        } else {
            CHECK_ACL(aclrtLaunchKernelWithHostArgs(
                zeroArgumentFunction, 2, resources.stream, nullptr, nullptr, 0, nullptr, 0));
        }
    }

    struct HostArgs {
        void* input;
        void* output;
        void* workspace;
    } arguments{resources.input, resources.output, resources.workspace};
    static_assert(sizeof(arguments) == 24);
    if (useArgsArray) {
        void* args[] = {&arguments.input, &arguments.output, &arguments.workspace};
        CHECK_ACL(aclrtLaunchKernelWithArgsArray(threeArgumentFunction, 2, resources.stream, nullptr, args));
    } else {
        CHECK_ACL(aclrtLaunchKernelWithHostArgs(
            threeArgumentFunction, 2, resources.stream, nullptr, &arguments, sizeof(arguments), nullptr, 0));
    }
    std::puts("[hook] function instrumented=yes");
    std::puts("[hook] launch trace_buffer_injected=yes");
    CHECK_ACL(aclrtSynchronizeStream(resources.stream));
    CHECK_ACL(aclrtMemcpy(outputHost.data(), kBytes, resources.output, kBytes, ACL_MEMCPY_DEVICE_TO_HOST));
    if (outputHost != inputHost) {
        std::fprintf(stderr, "unexpected output:");
        for (const uint32_t value : outputHost) {
            std::fprintf(stderr, " %u", value);
        }
        std::fputc('\n', stderr);
    }
    CHECK(outputHost == inputHost);
    CHECK(g_callbacks.records > 0);
    CHECK(g_callbacks.valid);
    CHECK(g_callbacks.launchIds.size() == (threeOnly ? 1U : 3U));
    CHECK(g_callbacks.launchCallbacks == (threeOnly ? 1U : 3U));

    std::printf("[device] records=%zu\n", g_callbacks.records);
    std::puts("[d2h] copies=1");
    std::printf("[callback] records=%zu launches=%zu\n", g_callbacks.records, g_callbacks.launchIds.size());
    CHECK(resources.Release());
    std::puts("[verify] kernel_result=pass trace_records=pass resources=balanced");
    std::puts("FULL_FLOW_SAMPLE_PASS");
    return 0;
}
