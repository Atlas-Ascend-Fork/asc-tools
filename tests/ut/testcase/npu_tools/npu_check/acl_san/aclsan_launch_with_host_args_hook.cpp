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
#include "acl_san/aclsan_cbdata.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"

#include <cstdio>
#include <cstring>

namespace {

#define CHECK(expression)                                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return 1;                                                                     \
        }                                                                                 \
    } while (false)

struct LaunchArguments {
    aclrtFuncHandle function = nullptr;
    uint32_t blocks = 0;
    aclrtStream stream = nullptr;
    aclrtLaunchKernelCfg* config = nullptr;
    void* hostArgs = nullptr;
    size_t argsSize = 0;
    aclrtPlaceHolderInfo* placeholders = nullptr;
    size_t placeholderCount = 0;
};

LaunchArguments g_forwarded{};
int g_originalCalls = 0;
int g_sentinelCalls = 0;
int g_arrayCalls = 0;

aclError ArrayLaunch(void* function, uint32_t blocks, aclrtStream stream, aclrtLaunchKernelCfg* config, void** args)
{
    ++g_arrayCalls;
    g_forwarded = {function, blocks, stream, config, nullptr, 0, nullptr, 0};
    if (args == nullptr || *static_cast<void**>(args[0]) != function || *static_cast<uint32_t*>(args[1]) != 42) {
        return ACL_ERROR_INVALID_PARAM;
    }
    return 73;
}

aclError OriginalLaunch(
    aclrtFuncHandle function, uint32_t blocks, aclrtStream stream, aclrtLaunchKernelCfg* config, void* hostArgs,
    size_t argsSize, aclrtPlaceHolderInfo* placeholders, size_t placeholderCount)
{
    ++g_originalCalls;
    g_forwarded = {function, blocks, stream, config, hostArgs, argsSize, placeholders, placeholderCount};
    return 71;
}

aclError SentinelLaunch(
    aclrtFuncHandle, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, void*, size_t, aclrtPlaceHolderInfo*, size_t)
{
    ++g_sentinelCalls;
    return 72;
}

int g_launchCallbacks = 0;
uint32_t g_lastNumBlocks = 0;
void Callback(void*, AclsanCallbackDomain domain, AclsanCallbackId, const void* data)
{
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto& launch = *static_cast<const AclsanLaunchData*>(data);
        g_lastNumBlocks = launch.numBlocks;
        if (std::strcmp(launch.common.apiName, "aclrtLaunchKernelWithArgsArray") == 0 && launch.common.result == 73 &&
            launch.launchId != 0) {
            ++g_launchCallbacks;
        }
    }
}

} // namespace

int main()
{
    CHECK(RuntimeStubSetOriginFunction("aclrtLaunchKernelWithHostArgs", &OriginalLaunch) == ACL_SUCCESS);
    CHECK(RuntimeStubSetOriginFunction("aclrtLaunchKernelWithArgsArray", &ArrayLaunch) == ACL_SUCCESS);

    AclsanSubscriberHandle subscriber = nullptr;
    CHECK(aclsanSubscribe(&subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    CHECK(acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks(&SentinelLaunch) == ACL_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);

    int functionStorage = 0;
    int streamStorage = 0;
    int configStorage = 0;
    unsigned char hostArgsStorage[64]{};
    const auto function = reinterpret_cast<aclrtFuncHandle>(&functionStorage);
    const auto stream = reinterpret_cast<aclrtStream>(&streamStorage);
    auto* config = reinterpret_cast<aclrtLaunchKernelCfg*>(&configStorage);
    aclrtPlaceHolderInfo placeholders[] = {{0, 24}, {8, 32}, {16, 40}};

    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL) ==
        ACLSAN_STATUS_SUCCESS);
    uint32_t value = 42;
    void* pointer = function;
    void* arrayArgs[] = {&pointer, &value};
    CHECK(aclrtLaunchKernelWithArgsArray(function, 13, stream, config, arrayArgs) == 73);
    CHECK(g_launchCallbacks == 1);
    CHECK(g_lastNumBlocks == 13);
    CHECK(g_arrayCalls == 1);
    CHECK(g_originalCalls == 0);
    CHECK(g_forwarded.function == function && g_forwarded.blocks == 13);
    CHECK(g_forwarded.stream == stream && g_forwarded.config == config);
    CHECK(arrayArgs[0] == &pointer && arrayArgs[1] == &value);

    CHECK(acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks(&SentinelLaunch) == ACL_SUCCESS);
    CHECK(aclrtLaunchKernelWithHostArgs(function, 1, stream, config, nullptr, 0, nullptr, 0) == 72);
    CHECK(g_sentinelCalls == 1);
    CHECK(acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks(nullptr) == ACL_SUCCESS);
    CHECK(
        aclsanEnableCallback(0, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);

    CHECK(aclrtLaunchKernelWithHostArgs(function, 7, stream, config, hostArgsStorage, 64, placeholders, 3) == 71);
    CHECK(g_sentinelCalls == 1);
    CHECK(g_originalCalls == 1);
    CHECK(g_lastNumBlocks == 7);
    CHECK(g_arrayCalls == 1);
    CHECK(g_forwarded.function == function);
    CHECK(g_forwarded.blocks == 7);
    CHECK(g_forwarded.stream == stream);
    CHECK(g_forwarded.config == config);
    CHECK(g_forwarded.hostArgs == hostArgsStorage);
    CHECK(g_forwarded.argsSize == 64);
    CHECK(g_forwarded.placeholders == placeholders);
    CHECK(g_forwarded.placeholderCount == 3);

    CHECK(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
    CHECK(aclrtLaunchKernelWithHostArgs(function, 1, stream, config, hostArgsStorage, 8, placeholders, 1) == 71);
    CHECK(g_originalCalls == 2);
    CHECK(aclrtLaunchKernelWithArgsArray(function, 13, stream, config, arrayArgs) == 73);
    CHECK(g_launchCallbacks == 1);
    CHECK(g_lastNumBlocks == 7);
    CHECK(g_arrayCalls == 2);
    return 0;
}
