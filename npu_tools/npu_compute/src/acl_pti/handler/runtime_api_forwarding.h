/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_RUNTIME_API_FORWARDING_H
#define NPU_COMPUTE_RUNTIME_API_FORWARDING_H

#include "runtime_api_handlers.h"
#include "acl_pti/callback/dispatcher.h"
#include "aclpti/aclpti_runtime_api.h"
#include "npu_compute_log.h"
#include <utility>

namespace aclpti::handler {
template <typename CallPolicy, typename Function, typename Params, typename Operation>
aclError InvokeRuntimeApi(
    aclptiCallbackId cbid, aclrtApiId apiId, const char* apiName, Params& params, Operation&& operation)
{
    callback::GetDispatcher().Dispatch(ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_ENTER, &params, ACL_SUCCESS);
    const auto original = reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(apiId));
    aclError result;
    if (original == nullptr) {
        result = ACL_ERROR_INTERNAL_ERROR;
        ASCTOOL_ERROR(
            "[aclpti] error operation=original_lookup status=%d api=%s code=%u", result, apiName,
            static_cast<unsigned>(static_cast<uint32_t>(result)));
    } else {
        result = std::forward<Operation>(operation)(original);
        if (result != ACL_SUCCESS) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=runtime_call status=%d api=%s code=%u", result, apiName,
                static_cast<unsigned>(static_cast<uint32_t>(result)));
        }
    }
    CallPolicy::Complete(result);
    callback::GetDispatcher().Dispatch(ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_EXIT, &params, result);
    return result;
}

template <typename CallPolicy>
aclError ForwardAclrtLaunchKernelWithHostArgs(
    aclrtFuncHandle funcHandle, std::uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void* hostArgs,
    std::size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, std::size_t placeHolderNum)
{
    aclptiAclrtLaunchKernelWithHostArgsParams params{funcHandle, numBlocks, stream,           cfg,
                                                     hostArgs,   argsSize,  placeHolderArray, placeHolderNum};
    return InvokeRuntimeApi<CallPolicy, aclrtLaunchKernelWithHostArgsFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, ACL_RT_API_aclrtLaunchKernelWithHostArgs,
        "aclrtLaunchKernelWithHostArgs", params, [&params](aclrtLaunchKernelWithHostArgsFunc original) {
            return original(
                params.funcHandle, params.numBlocks, params.stream, params.cfg, params.hostArgs, params.argsSize,
                params.placeHolderArray, params.placeHolderNum);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtLaunchSIMTKernelWithHostArgs(
    void* func, dim3 gridDim, dim3 blockDim, std::size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void* hostArgs, std::size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, std::size_t placeHolderNum)
{
    aclptiAclrtLaunchSIMTKernelWithHostArgsParams params{func, gridDim,  blockDim, dynUbufSize,      stream,
                                                         cfg,  hostArgs, argsSize, placeHolderArray, placeHolderNum};
    return InvokeRuntimeApi<CallPolicy, aclrtLaunchSIMTKernelWithHostArgsFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs, ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs,
        "aclrtLaunchSIMTKernelWithHostArgs", params, [&params](aclrtLaunchSIMTKernelWithHostArgsFunc original) {
            return original(
                params.func, params.gridDim, params.blockDim, params.dynUbufSize, params.stream, params.cfg,
                params.hostArgs, params.argsSize, params.placeHolderArray, params.placeHolderNum);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtLaunchKernelWithArgsArray(
    void* func, std::uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg, void** args)
{
    aclptiAclrtLaunchKernelWithArgsArrayParams params{func, numBlocks, stream, cfg, args};
    return InvokeRuntimeApi<CallPolicy, aclrtLaunchKernelWithArgsArrayFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, ACL_RT_API_aclrtLaunchKernelWithArgsArray,
        "aclrtLaunchKernelWithArgsArray", params, [&params](aclrtLaunchKernelWithArgsArrayFunc original) {
            return original(params.func, params.numBlocks, params.stream, params.cfg, params.args);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtLaunchSIMTKernelWithArgsArray(
    void* func, dim3 gridDim, dim3 blockDim, std::size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    void** args)
{
    aclptiAclrtLaunchSIMTKernelWithArgsArrayParams params{func, gridDim, blockDim, dynUbufSize, stream, cfg, args};
    return InvokeRuntimeApi<CallPolicy, aclrtLaunchSIMTKernelWithArgsArrayFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray, ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray,
        "aclrtLaunchSIMTKernelWithArgsArray", params, [&params](aclrtLaunchSIMTKernelWithArgsArrayFunc original) {
            return original(
                params.func, params.gridDim, params.blockDim, params.dynUbufSize, params.stream, params.cfg,
                params.args);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtLaunchKernelWithConfig(
    aclrtFuncHandle funcHandle, std::uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
    aclrtArgsHandle argsHandle, void* reserve)
{
    aclptiAclrtLaunchKernelWithConfigParams params{funcHandle, numBlocks, stream, cfg, argsHandle, reserve};
    return InvokeRuntimeApi<CallPolicy, aclrtLaunchKernelWithConfigFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithConfig, ACL_RT_API_aclrtLaunchKernelWithConfig,
        "aclrtLaunchKernelWithConfig", params, [&params](aclrtLaunchKernelWithConfigFunc original) {
            return original(
                params.funcHandle, params.numBlocks, params.stream, params.cfg, params.argsHandle, params.reserve);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtMemcpy(
    void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind kind)
{
    aclptiAclrtMemcpyParams params{destination, destinationSize, source, count, kind};
    return InvokeRuntimeApi<CallPolicy, aclrtMemcpyFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMemcpy, ACL_RT_API_aclrtMemcpy, "aclrtMemcpy", params,
        [&params](aclrtMemcpyFunc original) {
            return original(params.dst, params.destMax, params.src, params.count, params.kind);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtBinaryLoadFromData(
    const void* data, std::size_t length, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle)
{
    aclptiAclrtBinaryLoadFromDataParams params{data, length, options, binHandle};
    return InvokeRuntimeApi<CallPolicy, aclrtBinaryLoadFromDataFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromData, ACL_RT_API_aclrtBinaryLoadFromData, "aclrtBinaryLoadFromData",
        params, [&params](aclrtBinaryLoadFromDataFunc original) {
            return original(params.data, params.length, params.options, params.binHandle);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtBinaryLoadFromFile(const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle)
{
    aclptiAclrtBinaryLoadFromFileParams params{path, options, binHandle};
    return InvokeRuntimeApi<CallPolicy, aclrtBinaryLoadFromFileFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryLoadFromFile, ACL_RT_API_aclrtBinaryLoadFromFile, "aclrtBinaryLoadFromFile",
        params, [&params](aclrtBinaryLoadFromFileFunc original) {
            return original(params.path, params.options, params.binHandle);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtBinaryGetFunction(
    const aclrtBinHandle binHandle, const char* kernelName, aclrtFuncHandle* funcHandle)
{
    aclptiAclrtBinaryGetFunctionParams params{binHandle, kernelName, funcHandle};
    return InvokeRuntimeApi<CallPolicy, aclrtBinaryGetFunctionFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction, ACL_RT_API_aclrtBinaryGetFunction, "aclrtBinaryGetFunction", params,
        [&params](aclrtBinaryGetFunctionFunc original) {
            return original(params.binHandle, params.kernelName, params.funcHandle);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtMalloc(void** devPtr, std::size_t size, aclrtMemMallocPolicy policy)
{
    aclptiAclrtMallocParams params{devPtr, size, policy};
    return InvokeRuntimeApi<CallPolicy, aclrtMallocFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMalloc, ACL_RT_API_aclrtMalloc, "aclrtMalloc", params,
        [&params](aclrtMallocFunc original) { return original(params.devPtr, params.size, params.policy); });
}

template <typename CallPolicy>
aclError ForwardAclrtMallocAlign32(void** devPtr, std::size_t size, aclrtMemMallocPolicy policy)
{
    aclptiAclrtMallocAlign32Params params{devPtr, size, policy};
    return InvokeRuntimeApi<CallPolicy, aclrtMallocAlign32Func>(
        ACLPTI_RUNTIME_CBID_aclrtMallocAlign32, ACL_RT_API_aclrtMallocAlign32, "aclrtMallocAlign32", params,
        [&params](aclrtMallocAlign32Func original) { return original(params.devPtr, params.size, params.policy); });
}

template <typename CallPolicy>
aclError ForwardAclrtMallocWithCfg(void** devPtr, std::size_t size, aclrtMemMallocPolicy policy, aclrtMallocConfig* cfg)
{
    aclptiAclrtMallocWithCfgParams params{devPtr, size, policy, cfg};
    return InvokeRuntimeApi<CallPolicy, aclrtMallocWithCfgFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMallocWithCfg, ACL_RT_API_aclrtMallocWithCfg, "aclrtMallocWithCfg", params,
        [&params](aclrtMallocWithCfgFunc original) {
            return original(params.devPtr, params.size, params.policy, params.cfg);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtMemset(void* devPtr, std::size_t maxCount, std::int32_t value, std::size_t count)
{
    aclptiAclrtMemsetParams params{devPtr, maxCount, value, count};
    return InvokeRuntimeApi<CallPolicy, aclrtMemsetFunc>(
        ACLPTI_RUNTIME_CBID_aclrtMemset, ACL_RT_API_aclrtMemset, "aclrtMemset", params,
        [&params](aclrtMemsetFunc original) {
            return original(params.devPtr, params.maxCount, params.value, params.count);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtFree(void* devPtr)
{
    aclptiAclrtFreeParams params{devPtr};
    return InvokeRuntimeApi<CallPolicy, aclrtFreeFunc>(
        ACLPTI_RUNTIME_CBID_aclrtFree, ACL_RT_API_aclrtFree, "aclrtFree", params,
        [&params](aclrtFreeFunc original) { return original(params.devPtr); });
}

template <typename CallPolicy>
aclError ForwardAclrtCreateStream(aclrtStream* stream)
{
    aclptiAclrtCreateStreamParams params{stream};
    return InvokeRuntimeApi<CallPolicy, aclrtCreateStreamFunc>(
        ACLPTI_RUNTIME_CBID_aclrtCreateStream, ACL_RT_API_aclrtCreateStream, "aclrtCreateStream", params,
        [&params](aclrtCreateStreamFunc original) { return original(params.stream); });
}

template <typename CallPolicy>
aclError ForwardAclrtDestroyStream(aclrtStream stream)
{
    aclptiAclrtDestroyStreamParams params{stream};
    return InvokeRuntimeApi<CallPolicy, aclrtDestroyStreamFunc>(
        ACLPTI_RUNTIME_CBID_aclrtDestroyStream, ACL_RT_API_aclrtDestroyStream, "aclrtDestroyStream", params,
        [&params](aclrtDestroyStreamFunc original) { return original(params.stream); });
}

template <typename CallPolicy>
aclError ForwardAclrtSetDevice(std::int32_t deviceId)
{
    aclptiAclrtSetDeviceParams params{deviceId};
    return InvokeRuntimeApi<CallPolicy, aclrtSetDeviceFunc>(
        ACLPTI_RUNTIME_CBID_aclrtSetDevice, ACL_RT_API_aclrtSetDevice, "aclrtSetDevice", params,
        [&params](aclrtSetDeviceFunc original) { return original(params.deviceId); });
}

template <typename CallPolicy>
aclError ForwardAclrtResetDevice(std::int32_t deviceId)
{
    aclptiAclrtResetDeviceParams params{deviceId};
    return InvokeRuntimeApi<CallPolicy, aclrtResetDeviceFunc>(
        ACLPTI_RUNTIME_CBID_aclrtResetDevice, ACL_RT_API_aclrtResetDevice, "aclrtResetDevice", params,
        [&params](aclrtResetDeviceFunc original) { return original(params.deviceId); });
}

template <typename CallPolicy>
aclError ForwardAclrtSynchronizeStream(aclrtStream stream)
{
    aclptiAclrtSynchronizeStreamParams params{stream};
    return InvokeRuntimeApi<CallPolicy, aclrtSynchronizeStreamFunc>(
        ACLPTI_RUNTIME_CBID_aclrtSynchronizeStream, ACL_RT_API_aclrtSynchronizeStream, "aclrtSynchronizeStream", params,
        [&params](aclrtSynchronizeStreamFunc original) { return original(params.stream); });
}

template <typename CallPolicy>
aclError ForwardAclrtBinaryGetFunctionByEntry(
    aclrtBinHandle binHandle, std::uint64_t funcEntry, aclrtFuncHandle* funcHandle)
{
    aclptiAclrtBinaryGetFunctionByEntryParams params{binHandle, funcEntry, funcHandle};
    return InvokeRuntimeApi<CallPolicy, aclrtBinaryGetFunctionByEntryFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry, ACL_RT_API_aclrtBinaryGetFunctionByEntry,
        "aclrtBinaryGetFunctionByEntry", params, [&params](aclrtBinaryGetFunctionByEntryFunc original) {
            return original(params.binHandle, params.funcEntry, params.funcHandle);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtLaunchKernel(
    aclrtFuncHandle function, std::uint32_t blockCount, const void* argsData, std::size_t argsSize, aclrtStream stream)
{
    aclptiAclrtLaunchKernelParams params{function, blockCount, argsData, argsSize, stream};
    return InvokeRuntimeApi<CallPolicy, aclrtLaunchKernelFunc>(
        ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACL_RT_API_aclrtLaunchKernel, "aclrtLaunchKernel", params,
        [&params](aclrtLaunchKernelFunc original) {
            return original(params.funcHandle, params.numBlocks, params.argsData, params.argsSize, params.stream);
        });
}

template <typename CallPolicy>
aclError ForwardAclrtGetFuncBySymbol(const void* symbol, aclrtFuncHandle* funcHandle)
{
    aclptiAclrtGetFuncBySymbolParams params{symbol, funcHandle};
    return InvokeRuntimeApi<CallPolicy, aclrtGetFuncBySymbolFunc>(
        ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol, ACL_RT_API_aclrtGetFuncBySymbol, "aclrtGetFuncBySymbol", params,
        [&params](aclrtGetFuncBySymbolFunc original) { return original(params.symbol, params.funcHandle); });
}

template <typename CallPolicy>
aclError ForwardAclrtBinaryUnLoad(aclrtBinHandle binHandle)
{
    aclptiAclrtBinaryUnLoadParams params{binHandle};
    return InvokeRuntimeApi<CallPolicy, aclrtBinaryUnLoadFunc>(
        ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad, ACL_RT_API_aclrtBinaryUnLoad, "aclrtBinaryUnLoad", params,
        [&params](aclrtBinaryUnLoadFunc original) { return original(params.binHandle); });
}

inline aclError ForwardResetDeviceForce(int32_t deviceId)
{
    const auto original =
        reinterpret_cast<aclrtResetDeviceForceFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtResetDeviceForce));
    return original == nullptr ? ACL_ERROR_INTERNAL_ERROR : original(deviceId);
}

template <typename CallPolicy>
RuntimeHandlerSet MakeForwardingHandlers()
{
    RuntimeHandlerSet handlers;
    handlers.launchKernelWithHostArgs = &ForwardAclrtLaunchKernelWithHostArgs<CallPolicy>;
    handlers.launchSIMTKernelWithHostArgs = &ForwardAclrtLaunchSIMTKernelWithHostArgs<CallPolicy>;
    handlers.launchKernelWithArgsArray = &ForwardAclrtLaunchKernelWithArgsArray<CallPolicy>;
    handlers.launchSIMTKernelWithArgsArray = &ForwardAclrtLaunchSIMTKernelWithArgsArray<CallPolicy>;
    handlers.memcpy = &ForwardAclrtMemcpy<CallPolicy>;
    handlers.binaryLoadFromData = &ForwardAclrtBinaryLoadFromData<CallPolicy>;
    handlers.binaryLoadFromFile = &ForwardAclrtBinaryLoadFromFile<CallPolicy>;
    handlers.binaryGetFunction = &ForwardAclrtBinaryGetFunction<CallPolicy>;
    handlers.malloc = &ForwardAclrtMalloc<CallPolicy>;
    handlers.mallocAlign32 = &ForwardAclrtMallocAlign32<CallPolicy>;
    handlers.mallocWithCfg = &ForwardAclrtMallocWithCfg<CallPolicy>;
    handlers.memset = &ForwardAclrtMemset<CallPolicy>;
    handlers.free = &ForwardAclrtFree<CallPolicy>;
    handlers.createStream = &ForwardAclrtCreateStream<CallPolicy>;
    handlers.destroyStream = &ForwardAclrtDestroyStream<CallPolicy>;
    handlers.setDevice = &ForwardAclrtSetDevice<CallPolicy>;
    handlers.resetDevice = &ForwardAclrtResetDevice<CallPolicy>;
    handlers.synchronizeStream = &ForwardAclrtSynchronizeStream<CallPolicy>;
    handlers.binaryGetFunctionByEntry = &ForwardAclrtBinaryGetFunctionByEntry<CallPolicy>;
    handlers.launchKernel = &ForwardAclrtLaunchKernel<CallPolicy>;
    handlers.launchKernelWithConfig = &ForwardAclrtLaunchKernelWithConfig<CallPolicy>;
    handlers.getFuncBySymbol = &ForwardAclrtGetFuncBySymbol<CallPolicy>;
    handlers.binaryUnLoad = &ForwardAclrtBinaryUnLoad<CallPolicy>;
    handlers.resetDeviceForce = &ForwardResetDeviceForce;
    return handlers;
}
} // namespace aclpti::handler
#endif
