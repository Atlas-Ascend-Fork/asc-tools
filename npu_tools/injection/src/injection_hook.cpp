/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "injection/injection_hook.h"

#include "injection_log.h"
#include "profiling/prof_api.h"

#include <mutex>
#include <pthread.h>
#include <array>

namespace {

struct aclApiTable {
    void* hook[ACL_RT_API_MAX];
    void* origin[ACL_RT_API_MAX];
    void* callback[ACL_RT_API_MAX];
    pthread_rwlock_t lock;
};

constexpr uint32_t kMsprofDataCallbackType = 0U;

constexpr const char* kRuntimeApiNames[ACL_RT_API_MAX] = {
    "aclrtLaunchKernelWithHostArgs",
    "aclrtMemcpy",
    "aclrtBinaryLoadFromData",
    "aclrtBinaryGetFunction",
    "aclrtMalloc",
    "aclrtMemset",
    "aclrtFree",
    "aclrtCreateStream",
    "aclrtDestroyStream",
    "aclrtSetDevice",
    "aclrtResetDevice",
    "aclrtSynchronizeStream",
    "aclrtBinaryGetFunctionByEntry",
    "aclrtLaunchKernel",
    "aclrtGetFuncBySymbol",
    "aclrtBinaryUnLoad",
    "aclrtSynchronizeStreamWithTimeout",
    "aclrtGetDevice",
    "aclrtBinaryGetGlobal",
    "aclrtGetFunctionAttribute",
    "aclrtGetSocName",
    "aclrtGetDeviceInfo",
    "aclrtLaunchSIMTKernelWithHostArgs",
    "aclrtLaunchKernelWithArgsArray",
    "aclrtLaunchSIMTKernelWithArgsArray",
    "aclrtMallocAlign32",
    "aclrtFunctionGetParamCount",
    "aclrtFunctionGetParamInfo",
    "aclrtMallocWithCfg",
    "aclrtBinaryLoadFromFile",
};

std::mutex g_initMutex;
bool g_hookInstalled = false;

template <typename T>
void* FunctionToAddress(T func)
{
    return reinterpret_cast<void*>(func);
}

template <typename T>
T AddressToFunction(void* address)
{
    return reinterpret_cast<T>(address);
}

bool IsValidApiId(aclrtApiId id)
{
    const auto value = static_cast<int32_t>(id);
    return value >= 0 && value < static_cast<int32_t>(ACL_RT_API_MAX);
}

extern aclApiTable g_aclApiTable;

const char* FindRuntimeApiName(aclrtApiId id)
{
    if (!IsValidApiId(id)) {
        return nullptr;
    }
    return kRuntimeApiNames[static_cast<size_t>(id)];
}

bool SaveOriginalRuntimeEntry(aclrtApiId id, void* origin)
{
    if (!IsValidApiId(id)) {
        return false;
    }
    const size_t index = static_cast<size_t>(id);
    pthread_rwlock_wrlock(&g_aclApiTable.lock);
    g_aclApiTable.origin[index] = origin;
    pthread_rwlock_unlock(&g_aclApiTable.lock);
    return true;
}

int32_t RegisterCallback(aclrtApiId id, void* callback)
{
    const char* name = FindRuntimeApiName(id);
    if (!IsValidApiId(id)) {
        ASCTOOL_ERROR(
            "[tool_injection] register callback failed: name=%s id=%d"
            " code=%u",
            name == nullptr ? "unknown" : name, static_cast<int>(id), static_cast<unsigned>(1));
        return ACL_ERROR_INVALID_PARAM;
    }
    const size_t index = static_cast<size_t>(id);
    pthread_rwlock_wrlock(&g_aclApiTable.lock);
    const bool replaced = g_aclApiTable.callback[index] != nullptr;
    g_aclApiTable.callback[index] = callback;
    pthread_rwlock_unlock(&g_aclApiTable.lock);
    ASCTOOL_DEBUG(
        "[tool_injection] %s callback: name=%s id=%d callback=%p",
        callback == nullptr ? "clear" : (replaced ? "replace" : "register"), name == nullptr ? "unknown" : name,
        static_cast<int>(id), callback);
    return ACL_SUCCESS;
}

template <typename Function>
Function GetDispatchTarget(aclrtApiId id)
{
    const char* name = FindRuntimeApiName(id);
    void* targetAddress = nullptr;
    const char* source = "origin";
    if (IsValidApiId(id)) {
        const size_t index = static_cast<size_t>(id);
        pthread_rwlock_rdlock(&g_aclApiTable.lock);
        targetAddress = g_aclApiTable.callback[index];
        if (targetAddress == nullptr) {
            targetAddress = g_aclApiTable.origin[index];
        } else {
            source = "callback";
        }
        pthread_rwlock_unlock(&g_aclApiTable.lock);
    }
    const Function target = AddressToFunction<Function>(targetAddress);
    ASCTOOL_DEBUG(
        "[tool_injection] dispatch runtime api: name=%s id=%d source=%s target=%p", name == nullptr ? "unknown" : name,
        static_cast<int>(id), source, FunctionToAddress(target));
    return target;
}

void LogMissingCallback(const char* name, aclrtApiId id)
{
    ASCTOOL_ERROR(
        "[tool_injection] hook call failed: callback and origin are unavailable name=%s id=%d"
        " code=%u",
        name == nullptr ? "unknown" : name, static_cast<int>(id), static_cast<unsigned>(1));
}

void LogHookResult(const char* name, aclrtApiId id, aclError result)
{
    ASCTOOL_DEBUG(
        "[tool_injection] hook return: name=%s id=%d result=%d", name == nullptr ? "unknown" : name,
        static_cast<int>(id), static_cast<int>(result));
}

void LogHookPointerResult(const char* name, aclrtApiId id, const void* result)
{
    ASCTOOL_DEBUG("[tool_injection] hook return: name=%s id=%d result=%p", name, static_cast<int>(id), result);
}

} // namespace

extern "C" aclError aclrtSetDeviceHook(int32_t deviceId)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtSetDevice;
    const auto callback = GetDispatchTarget<aclrtSetDeviceFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtSetDevice", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(deviceId);
    LogHookResult("aclrtSetDevice", id, result);
    return result;
}

extern "C" aclError aclrtGetDeviceHook(int32_t* deviceId)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtGetDevice;
    const auto callback = GetDispatchTarget<aclrtGetDeviceFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtGetDevice", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(deviceId);
    LogHookResult("aclrtGetDevice", id, result);
    return result;
}

extern "C" aclError aclrtResetDeviceHook(int32_t deviceId)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtResetDevice;
    const auto callback = GetDispatchTarget<aclrtResetDeviceFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtResetDevice", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(deviceId);
    LogHookResult("aclrtResetDevice", id, result);
    return result;
}

extern "C" aclError aclrtCreateStreamHook(aclrtStream* stream)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtCreateStream;
    const auto callback = GetDispatchTarget<aclrtCreateStreamFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtCreateStream", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(stream);
    LogHookResult("aclrtCreateStream", id, result);
    return result;
}

extern "C" aclError aclrtDestroyStreamHook(aclrtStream stream)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtDestroyStream;
    const auto callback = GetDispatchTarget<aclrtDestroyStreamFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtDestroyStream", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(stream);
    LogHookResult("aclrtDestroyStream", id, result);
    return result;
}

extern "C" aclError aclrtMallocHook(void** devPtr, size_t size, aclrtMemMallocPolicy policy)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtMalloc;
    const auto callback = GetDispatchTarget<aclrtMallocFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtMalloc", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(devPtr, size, policy);
    LogHookResult("aclrtMalloc", id, result);
    return result;
}

extern "C" aclError aclrtMallocAlign32Hook(void** devPtr, size_t size, aclrtMemMallocPolicy policy)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtMallocAlign32;
    const auto callback = GetDispatchTarget<aclrtMallocAlign32Func>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtMallocAlign32", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(devPtr, size, policy);
    LogHookResult("aclrtMallocAlign32", id, result);
    return result;
}

extern "C" aclError aclrtFreeHook(void* devPtr)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtFree;
    const auto callback = GetDispatchTarget<aclrtFreeFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtFree", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(devPtr);
    LogHookResult("aclrtFree", id, result);
    return result;
}

extern "C" aclError aclrtMallocWithCfgHook(
    void** ptr, size_t bytes, aclrtMemMallocPolicy policy, aclrtMallocConfig* cfg)
{
    constexpr auto id = ACL_RT_API_aclrtMallocWithCfg;
    const auto callback = GetDispatchTarget<aclrtMallocWithCfgFunc>(id);
    if (callback == nullptr) {
        return ACL_ERROR_UNINITIALIZE;
    }
    const auto result = callback(ptr, bytes, policy, cfg);
    LogHookResult("aclrtMallocWithCfg", id, result);
    return result;
}

extern "C" aclError aclrtBinaryLoadFromFileHook(
    const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binary)
{
    constexpr auto id = ACL_RT_API_aclrtBinaryLoadFromFile;
    const auto callback = GetDispatchTarget<aclrtBinaryLoadFromFileFunc>(id);
    if (callback == nullptr) {
        return ACL_ERROR_UNINITIALIZE;
    }
    const auto result = callback(path, options, binary);
    LogHookResult("aclrtBinaryLoadFromFile", id, result);
    return result;
}

extern "C" aclError aclrtMemcpyHook(void* dst, size_t destMax, const void* src, size_t count, aclrtMemcpyKind kind)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtMemcpy;
    const auto callback = GetDispatchTarget<aclrtMemcpyFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtMemcpy", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(dst, destMax, src, count, kind);
    LogHookResult("aclrtMemcpy", id, result);
    return result;
}

extern "C" aclError aclrtMemsetHook(void* devPtr, size_t maxCount, int32_t value, size_t count)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtMemset;
    const auto callback = GetDispatchTarget<aclrtMemsetFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtMemset", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(devPtr, maxCount, value, count);
    LogHookResult("aclrtMemset", id, result);
    return result;
}

extern "C" aclError aclrtSynchronizeStreamHook(aclrtStream stream)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtSynchronizeStream;
    const auto callback = GetDispatchTarget<aclrtSynchronizeStreamFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtSynchronizeStream", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(stream);
    LogHookResult("aclrtSynchronizeStream", id, result);
    return result;
}

extern "C" aclError aclrtSynchronizeStreamWithTimeoutHook(aclrtStream stream, int32_t timeout)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtSynchronizeStreamWithTimeout;
    const auto callback = GetDispatchTarget<aclrtSynchronizeStreamWithTimeoutFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtSynchronizeStreamWithTimeout", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(stream, timeout);
    LogHookResult("aclrtSynchronizeStreamWithTimeout", id, result);
    return result;
}

extern "C" aclError aclrtBinaryLoadFromDataHook(
    const void* binaryData, size_t length, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtBinaryLoadFromData;
    const auto callback = GetDispatchTarget<aclrtBinaryLoadFromDataFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtBinaryLoadFromData", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(binaryData, length, options, binHandle);
    LogHookResult("aclrtBinaryLoadFromData", id, result);
    return result;
}

extern "C" aclError aclrtBinaryGetFunctionHook(
    const aclrtBinHandle binHandle, const char* kernelName, aclrtFuncHandle* funcHandle)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtBinaryGetFunction;
    const auto callback = GetDispatchTarget<aclrtBinaryGetFunctionFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtBinaryGetFunction", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(binHandle, kernelName, funcHandle);
    LogHookResult("aclrtBinaryGetFunction", id, result);
    return result;
}

extern "C" aclError aclrtBinaryGetFunctionByEntryHook(
    aclrtBinHandle binHandle, uint64_t funcEntry, aclrtFuncHandle* funcHandle)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtBinaryGetFunctionByEntry;
    const auto callback = GetDispatchTarget<aclrtBinaryGetFunctionByEntryFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtBinaryGetFunctionByEntry", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(binHandle, funcEntry, funcHandle);
    LogHookResult("aclrtBinaryGetFunctionByEntry", id, result);
    return result;
}

extern "C" aclError aclrtLaunchKernelWithHostArgsHook(
    aclrtFuncHandle funcHandle, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void* hostArgs,
    size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, size_t placeHolderNum)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtLaunchKernelWithHostArgs;
    const auto callback = GetDispatchTarget<aclrtLaunchKernelWithHostArgsFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtLaunchKernelWithHostArgs", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result =
        callback(funcHandle, numBlocks, stream, cfg, hostArgs, argsSize, placeHolderArray, placeHolderNum);
    LogHookResult("aclrtLaunchKernelWithHostArgs", id, result);
    return result;
}

extern "C" aclError aclrtLaunchSIMTKernelWithHostArgsHook(
    void* func, dim3 gridDim, dim3 blockDim, size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void* hostArgs, size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, size_t placeHolderNum)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs;
    const auto callback = GetDispatchTarget<aclrtLaunchSIMTKernelWithHostArgsFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtLaunchSIMTKernelWithHostArgs", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(
        func, gridDim, blockDim, dynUbufSize, stream, cfg, hostArgs, argsSize, placeHolderArray, placeHolderNum);
    LogHookResult("aclrtLaunchSIMTKernelWithHostArgs", id, result);
    return result;
}

extern "C" aclError aclrtLaunchKernelWithArgsArrayHook(
    void* func, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void** args)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtLaunchKernelWithArgsArray;
    const auto callback = GetDispatchTarget<aclrtLaunchKernelWithArgsArrayFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtLaunchKernelWithArgsArray", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(func, numBlocks, stream, cfg, args);
    LogHookResult("aclrtLaunchKernelWithArgsArray", id, result);
    return result;
}

extern "C" aclError aclrtLaunchSIMTKernelWithArgsArrayHook(
    void* func, dim3 gridDim, dim3 blockDim, size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void** args)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray;
    const auto callback = GetDispatchTarget<aclrtLaunchSIMTKernelWithArgsArrayFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtLaunchSIMTKernelWithArgsArray", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(func, gridDim, blockDim, dynUbufSize, stream, cfg, args);
    LogHookResult("aclrtLaunchSIMTKernelWithArgsArray", id, result);
    return result;
}

extern "C" aclError aclrtLaunchKernelHook(
    aclrtFuncHandle funcHandle, uint32_t numBlocks, const void* argsData, size_t argsSize, aclrtStream stream)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtLaunchKernel;
    const auto callback = GetDispatchTarget<aclrtLaunchKernelFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtLaunchKernel", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(funcHandle, numBlocks, argsData, argsSize, stream);
    LogHookResult("aclrtLaunchKernel", id, result);
    return result;
}

extern "C" aclError aclrtGetFuncBySymbolHook(const void* symbol, aclrtFuncHandle* funcHandle)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtGetFuncBySymbol;
    const auto callback = GetDispatchTarget<aclrtGetFuncBySymbolFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtGetFuncBySymbol", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(symbol, funcHandle);
    LogHookResult("aclrtGetFuncBySymbol", id, result);
    return result;
}

extern "C" aclError aclrtBinaryUnLoadHook(aclrtBinHandle binHandle)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtBinaryUnLoad;
    const auto callback = GetDispatchTarget<aclrtBinaryUnLoadFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtBinaryUnLoad", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(binHandle);
    LogHookResult("aclrtBinaryUnLoad", id, result);
    return result;
}

extern "C" aclError aclrtBinaryGetGlobalHook(aclrtBinHandle binHandle, const char* name, void** address, size_t* bytes)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtBinaryGetGlobal;
    const auto callback = GetDispatchTarget<aclrtBinaryGetGlobalFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtBinaryGetGlobal", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(binHandle, name, address, bytes);
    LogHookResult("aclrtBinaryGetGlobal", id, result);
    return result;
}

extern "C" aclError aclrtGetFunctionAttributeHook(
    aclrtFuncHandle funcHandle, aclrtFuncAttribute attrType, int64_t* attrValue)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtGetFunctionAttribute;
    const auto callback = GetDispatchTarget<aclrtGetFunctionAttributeFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtGetFunctionAttribute", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(funcHandle, attrType, attrValue);
    LogHookResult("aclrtGetFunctionAttribute", id, result);
    return result;
}

extern "C" aclError aclrtFunctionGetParamCountHook(const void* func, size_t* paramCount)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtFunctionGetParamCount;
    const auto callback = GetDispatchTarget<aclrtFunctionGetParamCountFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtFunctionGetParamCount", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(func, paramCount);
    LogHookResult("aclrtFunctionGetParamCount", id, result);
    return result;
}

extern "C" aclError aclrtFunctionGetParamInfoHook(
    const void* func, size_t paramIndex, size_t* paramOffset, size_t* paramSize)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtFunctionGetParamInfo;
    const auto callback = GetDispatchTarget<aclrtFunctionGetParamInfoFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtFunctionGetParamInfo", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(func, paramIndex, paramOffset, paramSize);
    LogHookResult("aclrtFunctionGetParamInfo", id, result);
    return result;
}

extern "C" const char* aclrtGetSocNameHook()
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtGetSocName;
    const auto callback = GetDispatchTarget<aclrtGetSocNameFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtGetSocName", id);
        return nullptr;
    }
    const char* result = callback();
    LogHookPointerResult("aclrtGetSocName", id, result);
    return result;
}

extern "C" aclError aclrtGetDeviceInfoHook(uint32_t deviceId, aclrtDevAttr attr, int64_t* value)
{
    constexpr aclrtApiId id = ACL_RT_API_aclrtGetDeviceInfo;
    const auto callback = GetDispatchTarget<aclrtGetDeviceInfoFunc>(id);
    if (callback == nullptr) {
        LogMissingCallback("aclrtGetDeviceInfo", id);
        return ACL_ERROR_UNINITIALIZE;
    }
    const aclError result = callback(deviceId, attr, value);
    LogHookResult("aclrtGetDeviceInfo", id, result);
    return result;
}

namespace {

aclApiTable g_aclApiTable = {
    {
        FunctionToAddress(&aclrtLaunchKernelWithHostArgsHook),
        FunctionToAddress(&aclrtMemcpyHook),
        FunctionToAddress(&aclrtBinaryLoadFromDataHook),
        FunctionToAddress(&aclrtBinaryGetFunctionHook),
        FunctionToAddress(&aclrtMallocHook),
        FunctionToAddress(&aclrtMemsetHook),
        FunctionToAddress(&aclrtFreeHook),
        FunctionToAddress(&aclrtCreateStreamHook),
        FunctionToAddress(&aclrtDestroyStreamHook),
        FunctionToAddress(&aclrtSetDeviceHook),
        FunctionToAddress(&aclrtResetDeviceHook),
        FunctionToAddress(&aclrtSynchronizeStreamHook),
        FunctionToAddress(&aclrtBinaryGetFunctionByEntryHook),
        FunctionToAddress(&aclrtLaunchKernelHook),
        FunctionToAddress(&aclrtGetFuncBySymbolHook),
        FunctionToAddress(&aclrtBinaryUnLoadHook),
        FunctionToAddress(&aclrtSynchronizeStreamWithTimeoutHook),
        FunctionToAddress(&aclrtGetDeviceHook),
        FunctionToAddress(&aclrtBinaryGetGlobalHook),
        FunctionToAddress(&aclrtGetFunctionAttributeHook),
        FunctionToAddress(&aclrtGetSocNameHook),
        FunctionToAddress(&aclrtGetDeviceInfoHook),
        FunctionToAddress(&aclrtLaunchSIMTKernelWithHostArgsHook),
        FunctionToAddress(&aclrtLaunchKernelWithArgsArrayHook),
        FunctionToAddress(&aclrtLaunchSIMTKernelWithArgsArrayHook),
        FunctionToAddress(&aclrtMallocAlign32Hook),
        FunctionToAddress(&aclrtFunctionGetParamCountHook),
        FunctionToAddress(&aclrtFunctionGetParamInfoHook),
        FunctionToAddress(&aclrtMallocWithCfgHook),
        FunctionToAddress(&aclrtBinaryLoadFromFileHook),
    },
    {},
    {},
    PTHREAD_RWLOCK_INITIALIZER};

} // namespace

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolUploaderInit(MsprofRawDataCallback uploader)
{
    if (uploader == nullptr) {
        ASCTOOL_ERROR(
            "[tool_injection] uploader registration failed: null uploader"
            " code=%u",
            static_cast<unsigned>(1));
        return ACL_ERROR_INVALID_PARAM;
    }
    const int32_t result = MsprofRegisterDataCallback(kMsprofDataCallbackType, FunctionToAddress(uploader));
    ASCTOOL_DEBUG(
        "[tool_injection] uploader registration: type=%u uploader=%p result=%d", kMsprofDataCallbackType,
        FunctionToAddress(uploader), result);
    if (result != 0) {
        ASCTOOL_ERROR(
            "[tool_injection] uploader registration failed: result=%d"
            " code=%u",
            result, static_cast<unsigned>(static_cast<uint32_t>(result)));
    }
    return result;
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolHookInit(void)
{
    std::lock_guard<std::mutex> initLock(g_initMutex);
    ASCTOOL_DEBUG("[tool_injection] hook initialization started");
    if (g_hookInstalled) {
        ASCTOOL_DEBUG("[tool_injection] hook initialization skipped: already installed");
        return ACL_SUCCESS;
    }

    std::array<aclrtApiFunc, ACL_RT_API_MAX> previous{};
    auto rollback = [&previous]() {
        for (size_t i = previous.size(); i != 0; --i) {
            if (previous[i - 1]) {
                (void)aclrtApiInjectionSetFunc(FindRuntimeApiName(static_cast<aclrtApiId>(i - 1)), previous[i - 1]);
            }
        }
    };
    for (int32_t value = 0; value < static_cast<int32_t>(ACL_RT_API_MAX); ++value) {
        const auto id = static_cast<aclrtApiId>(value);
        const size_t index = static_cast<size_t>(id);
        const char* name = FindRuntimeApiName(id);
        const aclrtApiFunc hook = AddressToFunction<aclrtApiFunc>(g_aclApiTable.hook[index]);
        if (name == nullptr || hook == nullptr) {
            rollback();
            ASCTOOL_ERROR(
                "[tool_injection] hook initialization failed: invalid entry id=%d name=%s hook=%p"
                " code=%u",
                static_cast<int>(id), name == nullptr ? "unknown" : name, FunctionToAddress(hook),
                static_cast<unsigned>(1));
            return ACL_ERROR_INTERNAL_ERROR;
        }
        aclrtApiFunc origin = nullptr;
        aclrtApiFunc current = nullptr;
        aclError ret = aclrtApiInjectionGetFunc(name, &origin, &current);
        if ((id == ACL_RT_API_aclrtMallocWithCfg || id == ACL_RT_API_aclrtBinaryLoadFromFile) &&
            (ret != ACL_SUCCESS || origin == nullptr || current == nullptr)) {
            continue;
        }
        ASCTOOL_DEBUG(
            "[tool_injection] get runtime entry: name=%s id=%d result=%d origin=%p current=%p", name,
            static_cast<int>(id), static_cast<int>(ret), FunctionToAddress(origin), FunctionToAddress(current));
        if (ret != ACL_SUCCESS) {
            rollback();
            ASCTOOL_ERROR(
                "[tool_injection] hook initialization failed: get runtime entry name=%s result=%d"
                " code=%u",
                name, static_cast<int>(ret), static_cast<unsigned>(static_cast<uint32_t>(ret)));
            return ret;
        }
        if (origin == nullptr || current == nullptr) {
            rollback();
            ASCTOOL_ERROR(
                "[tool_injection] hook initialization failed: empty runtime entry name=%s"
                " code=%u",
                name, static_cast<unsigned>(1));
            return ACL_ERROR_UNINITIALIZE;
        }
        if (!SaveOriginalRuntimeEntry(id, FunctionToAddress(origin))) {
            rollback();
            ASCTOOL_ERROR(
                "[tool_injection] hook initialization failed: save origin entry name=%s"
                " code=%u",
                name, static_cast<unsigned>(1));
            return ACL_ERROR_INTERNAL_ERROR;
        }

        ret = aclrtApiInjectionSetFunc(name, hook);
        ASCTOOL_DEBUG(
            "[tool_injection] set runtime hook: name=%s id=%d hook=%p result=%d", name, static_cast<int>(id),
            FunctionToAddress(hook), static_cast<int>(ret));
        if (ret != ACL_SUCCESS) {
            rollback();
            ASCTOOL_ERROR(
                "[tool_injection] hook initialization failed: set runtime hook name=%s result=%d"
                " code=%u",
                name, static_cast<int>(ret), static_cast<unsigned>(static_cast<uint32_t>(ret)));
            return ret;
        }
        previous[index] = current;
    }

    g_hookInstalled = true;
    ASCTOOL_DEBUG("[tool_injection] hook initialization completed: installed=%d", static_cast<int>(ACL_RT_API_MAX));
    return ACL_SUCCESS;
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolClearCallback(aclrtApiId id)
{
    return RegisterCallback(id, nullptr);
}

extern "C" ACL_TOOL_INJECTION_EXPORT void* acltoolGetOriginalRuntimeApi(aclrtApiId id)
{
    if (!IsValidApiId(id)) {
        ASCTOOL_ERROR(
            "[tool_injection] get original runtime api failed: invalid id=%d"
            " code=%u",
            static_cast<int>(id), static_cast<unsigned>(1));
        return nullptr;
    }

    const char* name = FindRuntimeApiName(id);
    if (name == nullptr) {
        ASCTOOL_ERROR(
            "[tool_injection] get original runtime api failed: unknown id=%d"
            " code=%u",
            static_cast<int>(id), static_cast<unsigned>(1));
        return nullptr;
    }

    aclrtApiFunc origin = nullptr;
    const aclError result = aclrtApiInjectionGetFunc(name, &origin, nullptr);
    ASCTOOL_DEBUG(
        "[tool_injection] get original runtime api: name=%s id=%d result=%d origin=%p", name, static_cast<int>(id),
        static_cast<int>(result), FunctionToAddress(origin));
    if (result != ACL_SUCCESS) {
        return nullptr;
    }
    return FunctionToAddress(origin);
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtSetDeviceCallbacks(aclrtSetDeviceFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtSetDevice, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtGetDeviceCallbacks(aclrtGetDeviceFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtGetDevice, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtResetDeviceCallbacks(aclrtResetDeviceFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtResetDevice, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtCreateStreamCallbacks(aclrtCreateStreamFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtCreateStream, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtDestroyStreamCallbacks(aclrtDestroyStreamFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtDestroyStream, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtMallocCallbacks(aclrtMallocFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtMalloc, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtMallocAlign32Callbacks(aclrtMallocAlign32Func callback)
{
    return RegisterCallback(ACL_RT_API_aclrtMallocAlign32, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtFreeCallbacks(aclrtFreeFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtFree, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtMemcpyCallbacks(aclrtMemcpyFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtMemcpy, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtMemsetCallbacks(aclrtMemsetFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtMemset, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtSynchronizeStreamCallbacks(aclrtSynchronizeStreamFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtSynchronizeStream, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtSynchronizeStreamWithTimeoutCallbacks(aclrtSynchronizeStreamWithTimeoutFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtSynchronizeStreamWithTimeout, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtBinaryLoadFromDataCallbacks(aclrtBinaryLoadFromDataFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtBinaryLoadFromData, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtBinaryGetFunctionCallbacks(aclrtBinaryGetFunctionFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtBinaryGetFunction, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtBinaryGetFunctionByEntryCallbacks(aclrtBinaryGetFunctionByEntryFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtBinaryGetFunctionByEntry, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks(aclrtLaunchKernelWithHostArgsFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtLaunchKernelWithHostArgs, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtLaunchSIMTKernelWithHostArgsCallbacks(aclrtLaunchSIMTKernelWithHostArgsFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtLaunchKernelWithArgsArrayCallbacks(aclrtLaunchKernelWithArgsArrayFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtLaunchKernelWithArgsArray, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtLaunchSIMTKernelWithArgsArrayCallbacks(aclrtLaunchSIMTKernelWithArgsArrayFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtLaunchKernelCallbacks(aclrtLaunchKernelFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtLaunchKernel, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtGetFuncBySymbolCallbacks(aclrtGetFuncBySymbolFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtGetFuncBySymbol, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtBinaryUnLoadCallbacks(aclrtBinaryUnLoadFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtBinaryUnLoad, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtBinaryGetGlobalCallbacks(aclrtBinaryGetGlobalFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtBinaryGetGlobal, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtGetFunctionAttributeCallbacks(aclrtGetFunctionAttributeFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtGetFunctionAttribute, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtGetSocNameCallbacks(aclrtGetSocNameFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtGetSocName, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtGetDeviceInfoCallbacks(aclrtGetDeviceInfoFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtGetDeviceInfo, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtFunctionGetParamCountCallbacks(aclrtFunctionGetParamCountFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtFunctionGetParamCount, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtFunctionGetParamInfoCallbacks(aclrtFunctionGetParamInfoFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtFunctionGetParamInfo, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegisterAclrtMallocWithCfgCallbacks(aclrtMallocWithCfgFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtMallocWithCfg, FunctionToAddress(callback));
}

extern "C" ACL_TOOL_INJECTION_EXPORT int32_t
acltoolRegisterAclrtBinaryLoadFromFileCallbacks(aclrtBinaryLoadFromFileFunc callback)
{
    return RegisterCallback(ACL_RT_API_aclrtBinaryLoadFromFile, FunctionToAddress(callback));
}
