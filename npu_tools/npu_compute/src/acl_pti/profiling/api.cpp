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
#include "aclpti/aclpti_range_profiler.h"

#include "acl_pti/profiling/replay_runtime.h"

extern "C" ACLPTI_EXPORT aclptiResult aclptiRangeProfilerSetConfig(aclptiRangeProfilerSetConfigParams* pParams)
{
    const aclptiResult result = aclpti::profiling::GetReplayRuntime().SetConfig(pParams);
    ASCTOOL_DEBUG("[aclpti] range config result=%d", static_cast<int>(result));
    if (result != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] range config failed: result=%d"
            " code=%u",
            static_cast<int>(result), static_cast<unsigned>(static_cast<uint32_t>(result)));
    }
    return result;
}
