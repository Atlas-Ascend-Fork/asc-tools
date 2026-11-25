/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime_api_handlers.h"
#include "npu_compute_log.h"
#include "acl_pti/initialization.h"

#include <array>
#include <cstddef>

namespace aclpti::handler {
namespace {
// Only the stable trampoline is installed in the injection library. A copy
// loaded by a racing call remains safe after the slot and session are cleared.
template <aclrtApiId Api, typename Function>
struct RuntimeEntry;

template <aclrtApiId Api, typename... Args>
struct RuntimeEntry<Api, aclError (*)(Args...)> {
    using Function = aclError (*)(Args...);
    static inline Function handler = nullptr;

    static aclError Invoke(Args... args)
    {
        initialization::CallbackAccess access;
        if (!access) {
            const auto original = reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(Api));
            return original == nullptr ? ACL_ERROR_INTERNAL_ERROR : original(args...);
        }
        const auto result = handler(args...);
        initialization::DeliverKernelCompletion();
        return result;
    }
};

std::array<bool, ACL_RT_API_MAX> registeredApis{};

template <aclrtApiId Api, typename Function>
bool RegisterRuntimeHandler(std::int32_t (*registerCallback)(Function), Function handler)
{
    if (handler == nullptr) {
        return false;
    }
    using Entry = RuntimeEntry<Api, Function>;
    Entry::handler = handler;
    const auto result = registerCallback(&Entry::Invoke);
    ASCTOOL_DEBUG("[aclpti] register runtime handler apiId=%d result=%d", static_cast<int>(Api), result);
    registeredApis[Api] = result == ACL_SUCCESS;
    return registeredApis[Api];
}
} // namespace

bool RegisterRuntimeApiHandlers() { return RegisterRuntimeApiHandlers(RuntimeHandlerMode::Kernel); }

bool RegisterRuntimeApiHandlers(RuntimeHandlerMode mode)
{
    const auto handlers =
        mode == RuntimeHandlerMode::Application ? BuildApplicationRuntimeHandlers() : BuildKernelRuntimeHandlers();
    bool registered =
        RegisterRuntimeHandler<ACL_RT_API_aclrtLaunchKernelWithHostArgs>(
            acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks, handlers.launchKernelWithHostArgs) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs>(
            acltoolRegisterAclrtLaunchSIMTKernelWithHostArgsCallbacks, handlers.launchSIMTKernelWithHostArgs) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtLaunchKernelWithArgsArray>(
            acltoolRegisterAclrtLaunchKernelWithArgsArrayCallbacks, handlers.launchKernelWithArgsArray) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray>(
            acltoolRegisterAclrtLaunchSIMTKernelWithArgsArrayCallbacks, handlers.launchSIMTKernelWithArgsArray) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtMemcpy>(
            acltoolRegisterAclrtMemcpyCallbacks, handlers.memoryCopyHandler) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtBinaryLoadFromData>(
            acltoolRegisterAclrtBinaryLoadFromDataCallbacks, handlers.binaryLoadFromData) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtBinaryLoadFromFile>(
            acltoolRegisterAclrtBinaryLoadFromFileCallbacks, handlers.binaryLoadFromFile) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtBinaryGetFunction>(
            acltoolRegisterAclrtBinaryGetFunctionCallbacks, handlers.binaryGetFunction) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtMalloc>(acltoolRegisterAclrtMallocCallbacks, handlers.malloc) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtMallocAlign32>(
            acltoolRegisterAclrtMallocAlign32Callbacks, handlers.mallocAlign32) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtMallocWithCfg>(
            acltoolRegisterAclrtMallocWithCfgCallbacks, handlers.mallocWithCfg) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtMemset>(
            acltoolRegisterAclrtMemsetCallbacks, handlers.memorySetHandler) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtFree>(acltoolRegisterAclrtFreeCallbacks, handlers.free) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtCreateStream>(
            acltoolRegisterAclrtCreateStreamCallbacks, handlers.createStream) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtDestroyStream>(
            acltoolRegisterAclrtDestroyStreamCallbacks, handlers.destroyStream) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtSetDevice>(acltoolRegisterAclrtSetDeviceCallbacks, handlers.setDevice) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtResetDevice>(
            acltoolRegisterAclrtResetDeviceCallbacks, handlers.resetDevice) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtSynchronizeStream>(
            acltoolRegisterAclrtSynchronizeStreamCallbacks, handlers.synchronizeStream) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtBinaryGetFunctionByEntry>(
            acltoolRegisterAclrtBinaryGetFunctionByEntryCallbacks, handlers.binaryGetFunctionByEntry) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtLaunchKernel>(
            acltoolRegisterAclrtLaunchKernelCallbacks, handlers.launchKernel) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtGetFuncBySymbol>(
            acltoolRegisterAclrtGetFuncBySymbolCallbacks, handlers.getFuncBySymbol) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtBinaryUnLoad>(
            acltoolRegisterAclrtBinaryUnLoadCallbacks, handlers.binaryUnLoad) &&
        RegisterRuntimeHandler<ACL_RT_API_aclrtResetDeviceForce>(
            acltoolRegisterAclrtResetDeviceForceCallbacks, handlers.resetDeviceForce);
    if (registered && mode == RuntimeHandlerMode::Application) {
        registered = RegisterRuntimeHandler<ACL_RT_API_aclrtLaunchKernelWithConfig>(
            acltoolRegisterAclrtLaunchKernelWithConfigCallbacks, handlers.launchKernelWithConfig);
    }
    if (!registered) {
        ASCTOOL_ERROR("[aclpti] runtime handler registration failed code=%u", NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    ASCTOOL_DEBUG("[aclpti] runtime handler registration complete");
    return true;
}

void ClearRuntimeApiHandlers() noexcept
{
    for (size_t index = 0; index < registeredApis.size(); ++index) {
        if (registeredApis[index]) {
            (void)acltoolClearCallback(static_cast<aclrtApiId>(index));
            registeredApis[index] = false;
        }
    }
}
} // namespace aclpti::handler
