/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute/npu_compute.h"
#include "runtime/compute_service.h"

#include "acl_pti/initialization.h"

#include <cstdlib>
#include <cstring>

extern "C" NPU_COMPUTE_EXPORT int acltoolInitialize()
{
    try {
        const auto* mode = std::getenv("NPU_COMPUTE_REPLAY_MODE");
        if (mode != nullptr && std::strcmp(mode, "application") == 0) {
            return aclpti::initialization::RegisterApplicationLifecycle(
                       &npucompute::ComputeService::InitializeFromAcl, &npucompute::ComputeService::FinalizeFromAcl) ==
                           ACLPTI_SUCCESS ?
                       0 :
                       -1;
        }
        return npucompute::ComputeService::Instance().Initialize();
    } catch (...) {
        return -1;
    }
}

extern "C" NPU_COMPUTE_EXPORT int acltoolShutdown()
{
    try {
        return npucompute::ComputeService::Instance().Shutdown();
    } catch (...) {
        return -1;
    }
}
