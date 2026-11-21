/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "runtime_api_forwarding.h"
#include "acl_pti/profiling/application_profiler.h"
#include <cstdint>

namespace aclpti::handler {
namespace {
struct ApplicationCallPolicy {
    static void Complete(aclError) {}
};

aclError ApplicationSetDevice(int32_t deviceId)
{
    aclptiAclrtSetDeviceParams params{deviceId};
    return InvokeRuntimeApi<ApplicationCallPolicy, aclrtSetDeviceFunc>(
        ACLPTI_RUNTIME_CBID_aclrtSetDevice, ACL_RT_API_aclrtSetDevice, "aclrtSetDevice", params,
        [&params](aclrtSetDeviceFunc original) {
            return profiling::GetApplicationProfiler().SetDevice(original, params.deviceId);
        });
}

aclError ApplicationResetDevice(int32_t deviceId)
{
    aclptiAclrtResetDeviceParams params{deviceId};
    return InvokeRuntimeApi<ApplicationCallPolicy, aclrtResetDeviceFunc>(
        ACLPTI_RUNTIME_CBID_aclrtResetDevice, ACL_RT_API_aclrtResetDevice, "aclrtResetDevice", params,
        [&params](aclrtResetDeviceFunc original) {
            return profiling::GetApplicationProfiler().ResetDevice(original, params.deviceId, false);
        });
}

aclError ApplicationResetDeviceForce(int32_t deviceId)
{
    const auto original =
        reinterpret_cast<aclrtResetDeviceForceFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtResetDeviceForce));
    if (original == nullptr) {
        return ACL_ERROR_INTERNAL_ERROR;
    }
    return profiling::GetApplicationProfiler().ResetDevice(original, deviceId, true);
}
} // namespace

RuntimeHandlerSet BuildApplicationRuntimeHandlers()
{
    auto handlers = MakeForwardingHandlers<ApplicationCallPolicy>();
    handlers.setDevice = &ApplicationSetDevice;
    handlers.resetDevice = &ApplicationResetDevice;
    handlers.resetDeviceForce = &ApplicationResetDeviceForce;
    return handlers;
}
} // namespace aclpti::handler
