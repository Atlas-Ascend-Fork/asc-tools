/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_RUNTIME_API_H
#define NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_RUNTIME_API_H

#include "aclpti/aclpti_callback.h"

#include "acl/acl_rt.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aclptiRuntimeCallbackId {
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs = 0,
    ACLPTI_RUNTIME_CBID_aclrtMemcpy = 1,
    ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromData = 2,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction = 3,
    ACLPTI_RUNTIME_CBID_aclrtMalloc = 4,
    ACLPTI_RUNTIME_CBID_aclrtMemset = 5,
    ACLPTI_RUNTIME_CBID_aclrtFree = 6,
    ACLPTI_RUNTIME_CBID_aclrtCreateStream = 7,
    ACLPTI_RUNTIME_CBID_aclrtDestroyStream = 8,
    ACLPTI_RUNTIME_CBID_aclrtSetDevice = 9,
    ACLPTI_RUNTIME_CBID_aclrtResetDevice = 10,
    ACLPTI_RUNTIME_CBID_aclrtSynchronizeStream = 11,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry = 12,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernel = 13,
    ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol = 14,
    ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad = 15,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs = 16,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray = 17,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray = 18,
    ACLPTI_RUNTIME_CBID_aclrtMallocAlign32 = 19,
    ACLPTI_RUNTIME_CBID_aclrtMallocWithCfg = 20,
    ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromFile = 21,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithConfig = 22,
    ACLPTI_RUNTIME_CBID_SIZE = 23,
} aclptiRuntimeCallbackId;

typedef struct aclptiAclrtLaunchKernelWithHostArgsParams {
    aclrtFuncHandle funcHandle;
    uint32_t numBlocks;
    aclrtStream stream;
    aclrtLaunchKernelCfg* cfg;
    void* hostArgs;
    size_t argsSize;
    aclrtPlaceHolderInfo* placeHolderArray;
    size_t placeHolderNum;
} aclptiAclrtLaunchKernelWithHostArgsParams;

typedef struct aclptiAclrtLaunchSIMTKernelWithHostArgsParams {
    void* func;
    dim3 gridDim;
    dim3 blockDim;
    size_t dynUbufSize;
    aclrtStream stream;
    aclrtLaunchKernelCfg* cfg;
    void* hostArgs;
    size_t argsSize;
    aclrtPlaceHolderInfo* placeHolderArray;
    size_t placeHolderNum;
} aclptiAclrtLaunchSIMTKernelWithHostArgsParams;

typedef struct aclptiAclrtLaunchKernelWithArgsArrayParams {
    void* func;
    uint32_t numBlocks;
    aclrtStream stream;
    aclrtLaunchKernelCfg* cfg;
    void** args;
} aclptiAclrtLaunchKernelWithArgsArrayParams;

typedef struct aclptiAclrtLaunchSIMTKernelWithArgsArrayParams {
    void* func;
    dim3 gridDim;
    dim3 blockDim;
    size_t dynUbufSize;
    aclrtStream stream;
    aclrtLaunchKernelCfg* cfg;
    void** args;
} aclptiAclrtLaunchSIMTKernelWithArgsArrayParams;

typedef struct aclptiAclrtMemcpyParams {
    void* dst;
    size_t destMax;
    const void* src;
    size_t count;
    aclrtMemcpyKind kind;
} aclptiAclrtMemcpyParams;

typedef struct aclptiAclrtBinaryLoadFromDataParams {
    const void* data;
    size_t length;
    const aclrtBinaryLoadOptions* options;
    aclrtBinHandle* binHandle;
} aclptiAclrtBinaryLoadFromDataParams;

typedef struct aclptiAclrtBinaryLoadFromFileParams {
    const char* path;
    aclrtBinaryLoadOptions* options;
    aclrtBinHandle* binHandle;
} aclptiAclrtBinaryLoadFromFileParams;

typedef struct aclptiAclrtBinaryGetFunctionParams {
    aclrtBinHandle binHandle;
    const char* kernelName;
    aclrtFuncHandle* funcHandle;
} aclptiAclrtBinaryGetFunctionParams;

typedef struct aclptiAclrtMallocParams {
    void** devPtr;
    size_t size;
    aclrtMemMallocPolicy policy;
} aclptiAclrtMallocParams;

typedef struct aclptiAclrtMallocAlign32Params {
    void** devPtr;
    size_t size;
    aclrtMemMallocPolicy policy;
} aclptiAclrtMallocAlign32Params;

typedef struct aclptiAclrtMallocWithCfgParams {
    void** devPtr;
    size_t size;
    aclrtMemMallocPolicy policy;
    aclrtMallocConfig* cfg;
} aclptiAclrtMallocWithCfgParams;

typedef struct aclptiAclrtMemsetParams {
    void* devPtr;
    size_t maxCount;
    int32_t value;
    size_t count;
} aclptiAclrtMemsetParams;

typedef struct aclptiAclrtFreeParams {
    void* devPtr;
} aclptiAclrtFreeParams;

typedef struct aclptiAclrtCreateStreamParams {
    aclrtStream* stream;
} aclptiAclrtCreateStreamParams;

typedef struct aclptiAclrtDestroyStreamParams {
    aclrtStream stream;
} aclptiAclrtDestroyStreamParams;

typedef struct aclptiAclrtSetDeviceParams {
    int32_t deviceId;
} aclptiAclrtSetDeviceParams;

typedef struct aclptiAclrtResetDeviceParams {
    int32_t deviceId;
} aclptiAclrtResetDeviceParams;

typedef struct aclptiAclrtSynchronizeStreamParams {
    aclrtStream stream;
} aclptiAclrtSynchronizeStreamParams;

typedef struct aclptiAclrtBinaryGetFunctionByEntryParams {
    aclrtBinHandle binHandle;
    uint64_t funcEntry;
    aclrtFuncHandle* funcHandle;
} aclptiAclrtBinaryGetFunctionByEntryParams;

typedef struct aclptiAclrtLaunchKernelParams {
    aclrtFuncHandle funcHandle;
    uint32_t numBlocks;
    const void* argsData;
    size_t argsSize;
    aclrtStream stream;
} aclptiAclrtLaunchKernelParams;

typedef struct aclptiAclrtLaunchKernelWithConfigParams {
    aclrtFuncHandle funcHandle;
    uint32_t numBlocks;
    aclrtStream stream;
    aclrtLaunchKernelCfg* cfg;
    aclrtArgsHandle argsHandle;
    void* reserve;
} aclptiAclrtLaunchKernelWithConfigParams;

typedef struct aclptiAclrtGetFuncBySymbolParams {
    const void* symbol;
    aclrtFuncHandle* funcHandle;
} aclptiAclrtGetFuncBySymbolParams;

typedef struct aclptiAclrtBinaryUnLoadParams {
    aclrtBinHandle binHandle;
} aclptiAclrtBinaryUnLoadParams;

#ifdef __cplusplus
}
#endif

#endif // NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_RUNTIME_API_H
