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

#include "aclsan_boundary_stub.h"
#include "injection/injection_hook.h"
#include "injection/runtime_stub_api.h"

#include <cstdint>

namespace {

aclError OriginalGetParamCount(const void* func, size_t* count)
{
    if (func != count) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *count = 3;
    return 81;
}

aclError OriginalGetParamInfo(const void* func, size_t index, size_t* offset, size_t* size)
{
    if (func != offset || index != 2) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *offset = 16;
    *size = 8;
    return 82;
}

aclError ReplacementGetParamCount(const void*, size_t*) { return 83; }
aclError ReplacementGetParamInfo(const void*, size_t, size_t*, size_t*) { return 84; }

int OriginalResetForce(int32_t deviceId) { return deviceId == 3 ? 113 : ACL_ERROR_INVALID_PARAM; }
int ReplacementResetForce(int32_t deviceId)
{
    const auto original =
        reinterpret_cast<aclrtResetDeviceForceFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtResetDeviceForce));
    return original(deviceId) + 1;
}

int gOriginalMallocCalls = 0;
int gReplacementMallocCalls = 0;
int gOriginalMallocAlign32Calls = 0;
int gReplacementMallocAlign32Calls = 0;
int gReplacementFreeCalls = 0;
int gReplacementMemcpyCalls = 0;
int gReplacementMemsetCalls = 0;
int gReplacementLaunchCalls = 0;
int gReplacementGetFuncBySymbolCalls = 0;
int gReplacementBinaryUnLoadCalls = 0;
int gReplacementSynchronizeStreamWithTimeoutCalls = 0;
int gOriginalGetDeviceCalls = 0;
int gReplacementGetDeviceCalls = 0;
int gReplacementBinaryGetGlobalCalls = 0;
int gReplacementGetFunctionAttributeCalls = 0;
int gReplacementGetSocNameCalls = 0;
int gOriginalGetDeviceInfoCalls = 0;
int gReplacementGetDeviceInfoCalls = 0;
int32_t gLastSynchronizeStreamTimeout = 0;

int OriginalMalloc(void**, std::size_t, aclrtMemMallocPolicy)
{
    ++gOriginalMallocCalls;
    return 17;
}

int OriginalMallocAlign32(void**, std::size_t, aclrtMemMallocPolicy)
{
    ++gOriginalMallocAlign32Calls;
    return 18;
}

int OriginalFree(void*) { return 0; }
int OriginalMemcpy(void*, std::size_t, const void*, std::size_t, aclrtMemcpyKind) { return 0; }
int OriginalMemset(void*, std::size_t, int, std::size_t) { return 0; }
int OriginalLaunch(void*, std::uint32_t, const void*, std::size_t, void*) { return 0; }
int OriginalGetDevice(int32_t* deviceId)
{
    ++gOriginalGetDeviceCalls;
    *deviceId = 4;
    return ACL_SUCCESS;
}

int OriginalBinaryGetGlobal(aclrtBinHandle, const char*, void** address, std::size_t* bytes)
{
    *address = reinterpret_cast<void*>(0x1234U);
    *bytes = 64;
    return ACL_SUCCESS;
}

int OriginalGetFunctionAttribute(aclrtFuncHandle, aclrtFuncAttribute, int64_t* value)
{
    *value = 32;
    return ACL_SUCCESS;
}

const char* OriginalGetSocName() { return "original-soc"; }

int OriginalGetDeviceInfo(uint32_t, aclrtDevAttr attr, int64_t* value)
{
    ++gOriginalGetDeviceInfoCalls;
    *value = attr == ACL_DEV_ATTR_CUBE_CORE_NUM ? 36 : 72;
    return ACL_SUCCESS;
}

int ReplacementMalloc(void** pointer, std::size_t size, aclrtMemMallocPolicy policy)
{
    ++gReplacementMallocCalls;
    const auto original = reinterpret_cast<aclrtMallocFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMalloc));
    return original == nullptr ? -1 : original(pointer, size, policy);
}

int ReplacementMallocAlign32(void** pointer, std::size_t size, aclrtMemMallocPolicy policy)
{
    ++gReplacementMallocAlign32Calls;
    const auto original =
        reinterpret_cast<aclrtMallocAlign32Func>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMallocAlign32));
    return original == nullptr ? -1 : original(pointer, size, policy);
}

int ReplacementFree(void*)
{
    ++gReplacementFreeCalls;
    return 21;
}

int ReplacementMemcpy(void*, std::size_t, const void*, std::size_t, aclrtMemcpyKind)
{
    ++gReplacementMemcpyCalls;
    return 22;
}

int ReplacementMemset(void*, std::size_t, int, std::size_t)
{
    ++gReplacementMemsetCalls;
    return 23;
}

int ReplacementLaunch(void*, uint32_t, const void*, std::size_t, void*)
{
    ++gReplacementLaunchCalls;
    return 24;
}

int ReplacementGetFuncBySymbol(const void* symbol, aclrtFuncHandle* funcHandle)
{
    ++gReplacementGetFuncBySymbolCalls;
    if (symbol == nullptr || funcHandle == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *funcHandle = const_cast<void*>(symbol);
    return 25;
}

int ReplacementBinaryUnLoad(aclrtBinHandle binHandle)
{
    ++gReplacementBinaryUnLoadCalls;
    return binHandle == nullptr ? ACL_ERROR_INVALID_PARAM : 26;
}

int ReplacementSynchronizeStreamWithTimeout(aclrtStream, int32_t timeout)
{
    ++gReplacementSynchronizeStreamWithTimeoutCalls;
    gLastSynchronizeStreamTimeout = timeout;
    return 27;
}

int ReplacementGetDevice(int32_t* deviceId)
{
    ++gReplacementGetDeviceCalls;
    *deviceId = 8;
    return ACL_SUCCESS;
}

int ReplacementBinaryGetGlobal(aclrtBinHandle, const char*, void** address, std::size_t* bytes)
{
    ++gReplacementBinaryGetGlobalCalls;
    *address = reinterpret_cast<void*>(0x5678U);
    *bytes = 128;
    return ACL_SUCCESS;
}

int ReplacementGetFunctionAttribute(aclrtFuncHandle, aclrtFuncAttribute, int64_t* value)
{
    ++gReplacementGetFunctionAttributeCalls;
    *value = 64;
    return ACL_SUCCESS;
}

const char* ReplacementGetSocName()
{
    ++gReplacementGetSocNameCalls;
    return "replacement-soc";
}

int ReplacementGetDeviceInfo(uint32_t, aclrtDevAttr attr, int64_t* value)
{
    ++gReplacementGetDeviceInfoCalls;
    *value = attr == ACL_DEV_ATTR_CUBE_CORE_NUM ? 18 : 36;
    return ACL_SUCCESS;
}

// acltoolHookInit 在统一大用例里经 --wrap 由边界桩层接管（默认桩 0，不装钩子）；
// 本用例需要真实安装钩子，__real_ 即源码编入的 injection_hook.cpp 真实实现。
extern "C" int32_t __real_acltoolHookInit(void);

int32_t RealHookInit() { return __real_acltoolHookInit(); }

} // namespace

TEST(AclInjectionHook, Main)
{
    aclsan_test::Boundary boundary;
    boundary.hookInit = &RealHookInit;
    const aclsan_test::BoundaryGuard boundaryGuard{boundary};

    static_assert(ACL_RT_API_aclrtGetFuncBySymbol == 14);
    static_assert(ACL_RT_API_aclrtBinaryUnLoad == 15);
    static_assert(ACL_RT_API_aclrtSynchronizeStreamWithTimeout == 16);
    static_assert(ACL_RT_API_aclrtGetDevice == 17);
    static_assert(ACL_RT_API_aclrtBinaryGetGlobal == 18);
    static_assert(ACL_RT_API_aclrtGetFunctionAttribute == 19);
    static_assert(ACL_RT_API_aclrtGetSocName == 20);
    static_assert(ACL_RT_API_aclrtGetDeviceInfo == 21);
    static_assert(ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs == 22);
    static_assert(ACL_RT_API_aclrtLaunchKernelWithArgsArray == 23);
    static_assert(ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray == 24);
    static_assert(ACL_RT_API_aclrtMallocAlign32 == 25);
    static_assert(ACL_RT_API_aclrtFunctionGetParamCount == 26);
    static_assert(ACL_RT_API_aclrtFunctionGetParamInfo == 27);
    static_assert(ACL_RT_API_aclrtMallocWithCfg == 28);
    static_assert(ACL_RT_API_aclrtBinaryLoadFromFile == 29);
    static_assert(ACL_RT_API_aclrtResetDeviceForce == 30);
    static_assert(ACL_RT_API_aclrtLaunchKernelWithConfig == 31);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtFunctionGetParamCount", &OriginalGetParamCount), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtFunctionGetParamInfo", &OriginalGetParamInfo), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtResetDeviceForce", &OriginalResetForce), ACL_SUCCESS);

    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtMalloc", &OriginalMalloc), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtMallocAlign32", &OriginalMallocAlign32), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtFree", &OriginalFree), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtMemcpy", &OriginalMemcpy), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtMemset", &OriginalMemset), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtLaunchKernel", &OriginalLaunch), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetDevice", &OriginalGetDevice), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtBinaryGetGlobal", &OriginalBinaryGetGlobal), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetFunctionAttribute", &OriginalGetFunctionAttribute), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetSocName", &OriginalGetSocName), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetDeviceInfo", &OriginalGetDeviceInfo), 0);

    ASSERT_EQ(reinterpret_cast<aclrtMallocFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMalloc)), &OriginalMalloc);
    ASSERT_EQ(
        reinterpret_cast<aclrtMallocAlign32Func>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMallocAlign32)),
        &OriginalMallocAlign32);
    ASSERT_EQ(
        reinterpret_cast<aclrtGetDeviceFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtGetDevice)),
        &OriginalGetDevice);
    ASSERT_EQ(
        reinterpret_cast<aclrtBinaryGetGlobalFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryGetGlobal)),
        &OriginalBinaryGetGlobal);
    ASSERT_EQ(
        reinterpret_cast<aclrtGetFunctionAttributeFunc>(
            acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtGetFunctionAttribute)),
        &OriginalGetFunctionAttribute);
    ASSERT_EQ(
        reinterpret_cast<aclrtGetSocNameFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtGetSocName)),
        &OriginalGetSocName);
    ASSERT_EQ(
        reinterpret_cast<aclrtGetDeviceInfoFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtGetDeviceInfo)),
        &OriginalGetDeviceInfo);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtMalloc), 0);

    aclrtApiFunc restored = nullptr;
    for (const char* name :
         {"aclrtFunctionGetParamInfo", "aclrtMallocWithCfg", "aclrtBinaryLoadFromFile", "aclrtResetDeviceForce",
          "aclrtLaunchKernelWithConfig"}) {
        SCOPED_TRACE(name);
        aclrtApiFunc origin = nullptr;
        ASSERT_EQ(aclrtApiInjectionGetFunc(name, &origin, nullptr), ACL_SUCCESS);
        ASSERT_NE(origin, nullptr);
        ASSERT_EQ(RuntimeStubClearOrigin(name), ACL_SUCCESS);
        ASSERT_EQ(acltoolHookInit(), ACL_ERROR_UNINITIALIZE);
        ASSERT_EQ(aclrtApiInjectionGetFunc("aclrtMalloc", nullptr, &restored), ACL_SUCCESS);
        ASSERT_EQ(restored, reinterpret_cast<aclrtApiFunc>(&OriginalMalloc));
        ASSERT_EQ(RuntimeStubSetOrigin(name, origin), ACL_SUCCESS);
    }
    ASSERT_EQ(acltoolHookInit(), 0);
    ASSERT_EQ(acltoolHookInit(), 0);
    ASSERT_EQ(aclrtResetDeviceForce(3), 113);
    ASSERT_EQ(acltoolRegisterAclrtResetDeviceForceCallbacks(&ReplacementResetForce), ACL_SUCCESS);
    ASSERT_EQ(aclrtResetDeviceForce(3), 114);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtResetDeviceForce), ACL_SUCCESS);
    ASSERT_EQ(aclrtResetDeviceForce(3), 113);

    const auto getCount = reinterpret_cast<aclrtFunctionGetParamCountFunc>(
        acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFunctionGetParamCount));
    const auto getInfo = reinterpret_cast<aclrtFunctionGetParamInfoFunc>(
        acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFunctionGetParamInfo));
    ASSERT_EQ(getCount, &OriginalGetParamCount);
    ASSERT_EQ(getInfo, &OriginalGetParamInfo);
    size_t count = 0;
    size_t offset = 0;
    size_t size = 0;
    ASSERT_EQ(aclrtFunctionGetParamCount(&count, &count), 81);
    ASSERT_EQ(count, 3);
    ASSERT_EQ(aclrtFunctionGetParamInfo(&offset, 2, &offset, &size), 82);
    ASSERT_EQ(offset, 16);
    ASSERT_EQ(size, 8);
    ASSERT_EQ(acltoolRegisterAclrtFunctionGetParamCountCallbacks(&ReplacementGetParamCount), 0);
    ASSERT_EQ(acltoolRegisterAclrtFunctionGetParamInfoCallbacks(&ReplacementGetParamInfo), 0);
    ASSERT_EQ(aclrtFunctionGetParamCount(&count, &count), 83);
    ASSERT_EQ(aclrtFunctionGetParamInfo(&offset, 2, &offset, &size), 84);
    ASSERT_EQ(getCount(&count, &count), 81);
    ASSERT_EQ(getInfo(&offset, 2, &offset, &size), 82);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtFunctionGetParamCount), 0);
    ASSERT_EQ(acltoolRegisterAclrtFunctionGetParamInfoCallbacks(nullptr), 0);
    ASSERT_EQ(aclrtFunctionGetParamCount(&count, &count), 81);
    ASSERT_EQ(aclrtFunctionGetParamInfo(&offset, 2, &offset, &size), 82);

    void* pointer = nullptr;
    ASSERT_EQ(aclrtMalloc(&pointer, 1, ACL_MEM_MALLOC_HUGE_FIRST), 17);
    ASSERT_EQ(aclrtMallocAlign32(&pointer, 1, ACL_MEM_MALLOC_HUGE_FIRST), 18);
    ASSERT_EQ(gOriginalMallocCalls, 1);
    ASSERT_EQ(gOriginalMallocAlign32Calls, 1);
    ASSERT_EQ(gReplacementMallocCalls, 0);
    ASSERT_EQ(gReplacementMallocAlign32Calls, 0);
    ASSERT_EQ(aclrtSetDevice(0), 0);
    ASSERT_EQ(aclrtSynchronizeStream(nullptr), 0);
    int32_t deviceId = -1;
    ASSERT_EQ(aclrtGetDevice(&deviceId), ACL_SUCCESS);
    ASSERT_EQ(deviceId, 4);
    ASSERT_EQ(gOriginalGetDeviceCalls, 1);

    ASSERT_EQ(acltoolRegisterAclrtMallocCallbacks(&ReplacementMalloc), 0);
    ASSERT_EQ(acltoolRegisterAclrtMallocAlign32Callbacks(&ReplacementMallocAlign32), 0);
    ASSERT_EQ(acltoolRegisterAclrtFreeCallbacks(&ReplacementFree), 0);
    ASSERT_EQ(acltoolRegisterAclrtMemcpyCallbacks(&ReplacementMemcpy), 0);
    ASSERT_EQ(acltoolRegisterAclrtMemsetCallbacks(&ReplacementMemset), 0);
    ASSERT_EQ(acltoolRegisterAclrtLaunchKernelCallbacks(&ReplacementLaunch), 0);
    ASSERT_EQ(acltoolRegisterAclrtGetFuncBySymbolCallbacks(&ReplacementGetFuncBySymbol), 0);
    ASSERT_EQ(acltoolRegisterAclrtBinaryUnLoadCallbacks(&ReplacementBinaryUnLoad), 0);
    ASSERT_EQ(acltoolRegisterAclrtSynchronizeStreamWithTimeoutCallbacks(&ReplacementSynchronizeStreamWithTimeout), 0);
    ASSERT_EQ(acltoolRegisterAclrtGetDeviceCallbacks(&ReplacementGetDevice), 0);
    ASSERT_EQ(acltoolRegisterAclrtBinaryGetGlobalCallbacks(&ReplacementBinaryGetGlobal), 0);
    ASSERT_EQ(acltoolRegisterAclrtGetFunctionAttributeCallbacks(&ReplacementGetFunctionAttribute), 0);
    ASSERT_EQ(acltoolRegisterAclrtGetSocNameCallbacks(&ReplacementGetSocName), 0);
    ASSERT_EQ(acltoolRegisterAclrtGetDeviceInfoCallbacks(&ReplacementGetDeviceInfo), 0);
    ASSERT_EQ(aclrtMalloc(&pointer, 1, ACL_MEM_MALLOC_HUGE_FIRST), 17);
    ASSERT_EQ(aclrtMallocAlign32(&pointer, 1, ACL_MEM_MALLOC_HUGE_FIRST), 18);
    ASSERT_EQ(aclrtFree(pointer), 21);
    ASSERT_EQ(aclrtMemcpy(nullptr, 0, nullptr, 0, ACL_MEMCPY_HOST_TO_HOST), 22);
    ASSERT_EQ(aclrtMemset(nullptr, 0, 0, 0), 23);
    ASSERT_EQ(aclrtLaunchKernel(nullptr, 0, nullptr, 0, nullptr), 24);
    int symbol = 0;
    aclrtFuncHandle functionHandle = nullptr;
    ASSERT_EQ(aclrtGetFuncBySymbol(&symbol, &functionHandle), 25);
    ASSERT_EQ(functionHandle, &symbol);
    ASSERT_EQ(aclrtBinaryUnLoad(functionHandle), 26);
    ASSERT_EQ(aclrtSynchronizeStreamWithTimeout(nullptr, 1234), 27);
    deviceId = -1;
    ASSERT_EQ(aclrtGetDevice(&deviceId), ACL_SUCCESS);
    ASSERT_EQ(deviceId, 8);
    void* globalAddress = nullptr;
    std::size_t globalBytes = 0;
    ASSERT_EQ(aclrtBinaryGetGlobal(nullptr, "global", &globalAddress, &globalBytes), ACL_SUCCESS);
    ASSERT_EQ(globalAddress, reinterpret_cast<void*>(0x5678U));
    ASSERT_EQ(globalBytes, 128);
    int64_t attribute = 0;
    ASSERT_EQ(aclrtGetFunctionAttribute(nullptr, ACL_FUNC_ATTR_KERNEL_TYPE, &attribute), ACL_SUCCESS);
    ASSERT_EQ(attribute, 64);
    ASSERT_STREQ(aclrtGetSocName(), "replacement-soc");
    int64_t coreCount = 0;
    ASSERT_EQ(aclrtGetDeviceInfo(0, ACL_DEV_ATTR_CUBE_CORE_NUM, &coreCount), ACL_SUCCESS);
    ASSERT_EQ(coreCount, 18);
    ASSERT_EQ(gReplacementMallocCalls, 1);
    ASSERT_EQ(gReplacementMallocAlign32Calls, 1);
    ASSERT_EQ(gOriginalMallocCalls, 2);
    ASSERT_EQ(gOriginalMallocAlign32Calls, 2);
    ASSERT_EQ(gReplacementFreeCalls, 1);
    ASSERT_EQ(gReplacementMemcpyCalls, 1);
    ASSERT_EQ(gReplacementMemsetCalls, 1);
    ASSERT_EQ(gReplacementLaunchCalls, 1);
    ASSERT_EQ(gReplacementGetFuncBySymbolCalls, 1);
    ASSERT_EQ(gReplacementBinaryUnLoadCalls, 1);
    ASSERT_EQ(gReplacementSynchronizeStreamWithTimeoutCalls, 1);
    ASSERT_EQ(gReplacementGetDeviceCalls, 1);
    ASSERT_EQ(gReplacementBinaryGetGlobalCalls, 1);
    ASSERT_EQ(gReplacementGetFunctionAttributeCalls, 1);
    ASSERT_EQ(gReplacementGetSocNameCalls, 1);
    ASSERT_EQ(gReplacementGetDeviceInfoCalls, 1);
    ASSERT_EQ(gLastSynchronizeStreamTimeout, 1234);

    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtMalloc), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtMalloc), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtMallocAlign32), 0);
    ASSERT_EQ(aclrtMalloc(&pointer, 1, ACL_MEM_MALLOC_HUGE_FIRST), 17);
    ASSERT_EQ(aclrtMallocAlign32(&pointer, 1, ACL_MEM_MALLOC_HUGE_FIRST), 18);
    ASSERT_EQ(gOriginalMallocCalls, 3);
    ASSERT_EQ(gOriginalMallocAlign32Calls, 3);
    ASSERT_EQ(gReplacementMallocCalls, 1);
    ASSERT_EQ(gReplacementMallocAlign32Calls, 1);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtGetFuncBySymbol), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtBinaryUnLoad), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtSynchronizeStreamWithTimeout), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtGetDevice), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtBinaryGetGlobal), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtGetFunctionAttribute), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtGetSocName), 0);
    ASSERT_EQ(acltoolClearCallback(ACL_RT_API_aclrtGetDeviceInfo), 0);
    deviceId = -1;
    ASSERT_EQ(aclrtGetDevice(&deviceId), ACL_SUCCESS);
    ASSERT_EQ(deviceId, 4);
    ASSERT_EQ(gOriginalGetDeviceCalls, 2);
    globalAddress = nullptr;
    globalBytes = 0;
    ASSERT_EQ(aclrtBinaryGetGlobal(nullptr, "global", &globalAddress, &globalBytes), ACL_SUCCESS);
    ASSERT_EQ(globalAddress, reinterpret_cast<void*>(0x1234U));
    ASSERT_EQ(globalBytes, 64);
    attribute = 0;
    ASSERT_EQ(aclrtGetFunctionAttribute(nullptr, ACL_FUNC_ATTR_KERNEL_TYPE, &attribute), ACL_SUCCESS);
    ASSERT_EQ(attribute, 32);
    ASSERT_STREQ(aclrtGetSocName(), "original-soc");
    coreCount = 0;
    ASSERT_EQ(aclrtGetDeviceInfo(0, ACL_DEV_ATTR_CUBE_CORE_NUM, &coreCount), ACL_SUCCESS);
    ASSERT_EQ(coreCount, 36);
    ASSERT_EQ(gOriginalGetDeviceInfoCalls, 1);
    ASSERT_EQ(acltoolClearCallback(static_cast<aclrtApiId>(ACL_RT_API_MAX)), ACL_ERROR_INVALID_PARAM);
    ASSERT_EQ(acltoolClearCallback(static_cast<aclrtApiId>(-1)), ACL_ERROR_INVALID_PARAM);
    ASSERT_EQ(acltoolGetOriginalRuntimeApi(static_cast<aclrtApiId>(ACL_RT_API_MAX)), nullptr);
    ASSERT_EQ(acltoolGetOriginalRuntimeApi(static_cast<aclrtApiId>(-1)), nullptr);

    ASSERT_EQ(
        reinterpret_cast<aclrtLaunchKernelFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtLaunchKernel)),
        &OriginalLaunch);
    for (const auto id :
         {ACL_RT_API_aclrtSynchronizeStream, ACL_RT_API_aclrtLaunchKernelWithHostArgs,
          ACL_RT_API_aclrtBinaryLoadFromData, ACL_RT_API_aclrtSynchronizeStreamWithTimeout}) {
        SCOPED_TRACE(static_cast<int32_t>(id));
        ASSERT_NE(acltoolGetOriginalRuntimeApi(id), nullptr);
    }
}
