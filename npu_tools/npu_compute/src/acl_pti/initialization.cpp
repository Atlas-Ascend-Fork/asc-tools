/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "initialization.h"

#include "acl_pti/profiling/replay_runtime.h"
#include "acl_pti/handler/runtime_api_handlers.h"
#include "injection/injection_hook.h"

#include <mutex>

namespace aclpti::initialization {

aclptiResult InitializeDependencies()
{
    static std::mutex initializationMutex;
    static bool initialized = false;

    std::lock_guard<std::mutex> lock(initializationMutex);
    if (initialized) {
        ASCTOOL_DEBUG("[aclpti] dependencies already initialized");
        return ACLPTI_SUCCESS;
    }

    ASCTOOL_DEBUG("[aclpti] initialize dependencies");
    const int hookResult = acltoolHookInit();
    ASCTOOL_DEBUG("[aclpti] hook install result=%d", hookResult);
    if (hookResult != 0) {
        ASCTOOL_ERROR(
            "[aclpti] hook initialization failed: result=%d"
            " code=%u",
            hookResult, static_cast<unsigned>(static_cast<uint32_t>(hookResult)));
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }

    const aclptiResult replayResult = profiling::GetReplayRuntime().Initialize();
    if (replayResult != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] replay runtime initialization failed"
            " code=%u",
            static_cast<unsigned>(static_cast<uint32_t>(replayResult)));
        return replayResult;
    }
    if (!handler::RegisterRuntimeApiHandlers()) {
        (void)profiling::GetReplayRuntime().StopProfiling();
        ASCTOOL_ERROR(
            "[aclpti] runtime handler registration failed"
            " code=%u",
            static_cast<unsigned>(1));
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }
    initialized = true;
    ASCTOOL_DEBUG("[aclpti] dependencies initialized");
    return ACLPTI_SUCCESS;
}

} // namespace aclpti::initialization
