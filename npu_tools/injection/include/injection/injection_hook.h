/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef INJECTION_INJECTION_HOOK_H_
#define INJECTION_INJECTION_HOOK_H_

#include <stddef.h>
#include <stdint.h>

#include "acl/acl_rt.h"
#include "injection/export.h"
#include "profiling/prof_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef aclError (*aclrtSetDeviceFunc)(int32_t deviceId);
typedef aclError (*aclrtGetDeviceFunc)(int32_t* deviceId);
typedef aclError (*aclrtResetDeviceFunc)(int32_t deviceId);
typedef aclError (*aclrtResetDeviceForceFunc)(int32_t deviceId);
typedef aclError (*aclrtCreateStreamFunc)(aclrtStream* stream);
typedef aclError (*aclrtDestroyStreamFunc)(aclrtStream stream);
typedef aclError (*aclrtMallocFunc)(void** devPtr, size_t size, aclrtMemMallocPolicy policy);
typedef aclError (*aclrtMallocAlign32Func)(void** devPtr, size_t size, aclrtMemMallocPolicy policy);
typedef aclError (*aclrtMallocWithCfgFunc)(
    void** devPtr, size_t size, aclrtMemMallocPolicy policy, aclrtMallocConfig* cfg);
typedef aclError (*aclrtBinaryLoadFromFileFunc)(
    const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binary);
typedef aclError (*aclrtFreeFunc)(void* devPtr);
typedef aclError (*aclrtMemcpyFunc)(void* dst, size_t destMax, const void* src, size_t count, aclrtMemcpyKind kind);
typedef aclError (*aclrtMemsetFunc)(void* devPtr, size_t maxCount, int32_t value, size_t count);
typedef aclError (*aclrtSynchronizeStreamFunc)(aclrtStream stream);
typedef aclError (*aclrtSynchronizeStreamWithTimeoutFunc)(aclrtStream stream, int32_t timeout);
typedef aclError (*aclrtBinaryLoadFromDataFunc)(
    const void* data, size_t length, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle);
typedef aclError (*aclrtBinaryGetFunctionFunc)(
    const aclrtBinHandle binHandle, const char* kernelName, aclrtFuncHandle* funcHandle);
typedef aclError (*aclrtBinaryGetFunctionByEntryFunc)(
    aclrtBinHandle binHandle, uint64_t funcEntry, aclrtFuncHandle* funcHandle);
typedef aclError (*aclrtLaunchKernelWithHostArgsFunc)(
    aclrtFuncHandle funcHandle, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void* hostArgs,
    size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, size_t placeHolderNum);
typedef aclError (*aclrtLaunchSIMTKernelWithHostArgsFunc)(
    void* func, dim3 gridDim, dim3 blockDim, size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void* hostArgs, size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, size_t placeHolderNum);
typedef aclError (*aclrtLaunchKernelWithArgsArrayFunc)(
    void* func, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void** args);
typedef aclError (*aclrtLaunchSIMTKernelWithArgsArrayFunc)(
    void* func, dim3 gridDim, dim3 blockDim, size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void** args);
typedef aclError (*aclrtLaunchKernelFunc)(
    aclrtFuncHandle funcHandle, uint32_t numBlocks, const void* argsData, size_t argsSize, aclrtStream stream);
typedef aclError (*aclrtLaunchKernelWithConfigFunc)(
    aclrtFuncHandle funcHandle, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    aclrtArgsHandle argsHandle, void* reserve);
typedef aclError (*aclrtGetFuncBySymbolFunc)(const void* symbol, aclrtFuncHandle* funcHandle);
typedef aclError (*aclrtBinaryUnLoadFunc)(aclrtBinHandle binHandle);
typedef aclError (*aclrtBinaryGetGlobalFunc)(aclrtBinHandle binHandle, const char* name, void** address, size_t* bytes);
typedef aclError (*aclrtGetFunctionAttributeFunc)(
    aclrtFuncHandle funcHandle, aclrtFuncAttribute attrType, int64_t* attrValue);
typedef const char* (*aclrtGetSocNameFunc)(void);
typedef aclError (*aclrtGetDeviceInfoFunc)(uint32_t deviceId, aclrtDevAttr attr, int64_t* value);
typedef aclError (*aclrtFunctionGetParamCountFunc)(const void* func, size_t* paramCount);
typedef aclError (*aclrtFunctionGetParamInfoFunc)(
    const void* func, size_t paramIndex, size_t* paramOffset, size_t* paramSize);

typedef enum {
    ACL_RT_API_aclrtLaunchKernelWithHostArgs = 0,
    ACL_RT_API_aclrtMemcpy = 1,
    ACL_RT_API_aclrtBinaryLoadFromData = 2,
    ACL_RT_API_aclrtBinaryGetFunction = 3,
    ACL_RT_API_aclrtMalloc = 4,
    ACL_RT_API_aclrtMemset = 5,
    ACL_RT_API_aclrtFree = 6,
    ACL_RT_API_aclrtCreateStream = 7,
    ACL_RT_API_aclrtDestroyStream = 8,
    ACL_RT_API_aclrtSetDevice = 9,
    ACL_RT_API_aclrtResetDevice = 10,
    ACL_RT_API_aclrtSynchronizeStream = 11,
    ACL_RT_API_aclrtBinaryGetFunctionByEntry = 12,
    ACL_RT_API_aclrtLaunchKernel = 13,
    ACL_RT_API_aclrtGetFuncBySymbol = 14,
    ACL_RT_API_aclrtBinaryUnLoad = 15,
    ACL_RT_API_aclrtSynchronizeStreamWithTimeout = 16,
    ACL_RT_API_aclrtGetDevice = 17,
    ACL_RT_API_aclrtBinaryGetGlobal = 18,
    ACL_RT_API_aclrtGetFunctionAttribute = 19,
    ACL_RT_API_aclrtGetSocName = 20,
    ACL_RT_API_aclrtGetDeviceInfo = 21,
    ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs = 22,
    ACL_RT_API_aclrtLaunchKernelWithArgsArray = 23,
    ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray = 24,
    ACL_RT_API_aclrtMallocAlign32 = 25,
    ACL_RT_API_aclrtFunctionGetParamCount = 26,
    ACL_RT_API_aclrtFunctionGetParamInfo = 27,
    ACL_RT_API_aclrtMallocWithCfg = 28,
    ACL_RT_API_aclrtBinaryLoadFromFile = 29,
    ACL_RT_API_aclrtResetDeviceForce = 30,
    ACL_RT_API_aclrtLaunchKernelWithConfig = 31,
    ACL_RT_API_MAX
} aclrtApiId;

ACL_TOOL_INJECTION_EXPORT int32_t acltoolUploaderInit(MsprofRawDataCallback uploader);
ACL_TOOL_INJECTION_EXPORT int32_t acltoolHookInit(void);
ACL_TOOL_INJECTION_EXPORT int32_t acltoolClearCallback(aclrtApiId id);
ACL_TOOL_INJECTION_EXPORT void* acltoolGetOriginalRuntimeApi(aclrtApiId id);

#define ACL_TOOL_INJECTION_DECLARE_REGISTRATION(exportName, apiName) \
    ACL_TOOL_INJECTION_EXPORT int32_t acltoolRegister##exportName##Callbacks(apiName##Func callback)

ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtSetDevice, aclrtSetDevice);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtGetDevice, aclrtGetDevice);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtResetDevice, aclrtResetDevice);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtResetDeviceForce, aclrtResetDeviceForce);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtCreateStream, aclrtCreateStream);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtDestroyStream, aclrtDestroyStream);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtMalloc, aclrtMalloc);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtMallocAlign32, aclrtMallocAlign32);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtMallocWithCfg, aclrtMallocWithCfg);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtBinaryLoadFromFile, aclrtBinaryLoadFromFile);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtFree, aclrtFree);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtMemcpy, aclrtMemcpy);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtMemset, aclrtMemset);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtSynchronizeStream, aclrtSynchronizeStream);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtSynchronizeStreamWithTimeout, aclrtSynchronizeStreamWithTimeout);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtBinaryLoadFromData, aclrtBinaryLoadFromData);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtBinaryGetFunction, aclrtBinaryGetFunction);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtBinaryGetFunctionByEntry, aclrtBinaryGetFunctionByEntry);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtLaunchKernelWithHostArgs, aclrtLaunchKernelWithHostArgs);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtLaunchSIMTKernelWithHostArgs, aclrtLaunchSIMTKernelWithHostArgs);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtLaunchKernelWithArgsArray, aclrtLaunchKernelWithArgsArray);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtLaunchSIMTKernelWithArgsArray, aclrtLaunchSIMTKernelWithArgsArray);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtLaunchKernel, aclrtLaunchKernel);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtLaunchKernelWithConfig, aclrtLaunchKernelWithConfig);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtGetFuncBySymbol, aclrtGetFuncBySymbol);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtBinaryUnLoad, aclrtBinaryUnLoad);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtBinaryGetGlobal, aclrtBinaryGetGlobal);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtGetFunctionAttribute, aclrtGetFunctionAttribute);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtGetSocName, aclrtGetSocName);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtGetDeviceInfo, aclrtGetDeviceInfo);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtFunctionGetParamCount, aclrtFunctionGetParamCount);
ACL_TOOL_INJECTION_DECLARE_REGISTRATION(AclrtFunctionGetParamInfo, aclrtFunctionGetParamInfo);

#undef ACL_TOOL_INJECTION_DECLARE_REGISTRATION

#ifdef __cplusplus
}
#endif

#endif // INJECTION_INJECTION_HOOK_H_
