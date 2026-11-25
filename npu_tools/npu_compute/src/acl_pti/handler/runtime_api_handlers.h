/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_RUNTIME_API_HANDLERS_H
#define NPU_COMPUTE_RUNTIME_API_HANDLERS_H

#include "injection/injection_hook.h"

namespace aclpti::handler {
enum class RuntimeHandlerMode { Kernel, Application };

struct RuntimeHandlerSet {
    aclrtLaunchKernelWithHostArgsFunc launchKernelWithHostArgs = nullptr;
    aclrtLaunchSIMTKernelWithHostArgsFunc launchSIMTKernelWithHostArgs = nullptr;
    aclrtLaunchKernelWithArgsArrayFunc launchKernelWithArgsArray = nullptr;
    aclrtLaunchSIMTKernelWithArgsArrayFunc launchSIMTKernelWithArgsArray = nullptr;
    aclrtMemcpyFunc memoryCopyHandler = nullptr;
    aclrtBinaryLoadFromDataFunc binaryLoadFromData = nullptr;
    aclrtBinaryLoadFromFileFunc binaryLoadFromFile = nullptr;
    aclrtBinaryGetFunctionFunc binaryGetFunction = nullptr;
    aclrtMallocFunc malloc = nullptr;
    aclrtMallocAlign32Func mallocAlign32 = nullptr;
    aclrtMallocWithCfgFunc mallocWithCfg = nullptr;
    aclrtMemsetFunc memorySetHandler = nullptr;
    aclrtFreeFunc free = nullptr;
    aclrtCreateStreamFunc createStream = nullptr;
    aclrtDestroyStreamFunc destroyStream = nullptr;
    aclrtSetDeviceFunc setDevice = nullptr;
    aclrtResetDeviceFunc resetDevice = nullptr;
    aclrtSynchronizeStreamFunc synchronizeStream = nullptr;
    aclrtBinaryGetFunctionByEntryFunc binaryGetFunctionByEntry = nullptr;
    aclrtLaunchKernelFunc launchKernel = nullptr;
    aclrtLaunchKernelWithConfigFunc launchKernelWithConfig = nullptr;
    aclrtGetFuncBySymbolFunc getFuncBySymbol = nullptr;
    aclrtBinaryUnLoadFunc binaryUnLoad = nullptr;
    aclrtResetDeviceForceFunc resetDeviceForce = nullptr;
};

RuntimeHandlerSet BuildKernelRuntimeHandlers();
RuntimeHandlerSet BuildApplicationRuntimeHandlers();
bool RegisterRuntimeApiHandlers();
bool RegisterRuntimeApiHandlers(RuntimeHandlerMode mode);
void ClearRuntimeApiHandlers() noexcept;

} // namespace aclpti::handler
#endif
