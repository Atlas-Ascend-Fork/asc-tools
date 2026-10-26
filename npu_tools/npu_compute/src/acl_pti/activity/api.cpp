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
#include "aclpti/aclpti_activity.h"

#include "control.h"

extern "C" ACLPTI_EXPORT aclptiResult
aclptiActivityEnable(aclptiSubscribeHandle subscriber, aclptiActivityKind kind, aclptiActivityConfig* pActivityConfig)
{
    const aclptiResult result = aclpti::activity::Enable(subscriber, kind, pActivityConfig);
    ASCTOOL_DEBUG("[aclpti] activity enable kind=%d result=%d", static_cast<int>(kind), static_cast<int>(result));
    if (result != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] activity operation failed: result=%d"
            " code=%u",
            static_cast<int>(result), static_cast<unsigned>(static_cast<uint32_t>(result)));
    }
    return result;
}

extern "C" ACLPTI_EXPORT aclptiResult
aclptiActivityDisable(aclptiSubscribeHandle subscriber, aclptiActivityKind kind, aclptiActivityConfig* pActivityConfig)
{
    const aclptiResult result = aclpti::activity::Disable(subscriber, kind, pActivityConfig);
    ASCTOOL_DEBUG("[aclpti] activity disable kind=%d result=%d", static_cast<int>(kind), static_cast<int>(result));
    if (result != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] activity operation failed: result=%d"
            " code=%u",
            static_cast<int>(result), static_cast<unsigned>(static_cast<uint32_t>(result)));
    }
    return result;
}
