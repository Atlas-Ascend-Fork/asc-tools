/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_RUNTIME_HOOK_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_RUNTIME_HOOK_H

#include "acl_san/aclsan_api.h"
#include "npu_tool_log.h"
#include "injection/injection_hook.h"

#include <cstdlib>
#include <set>

namespace aclsan {

[[noreturn]] inline void AbortHookFailure(const char* hookName, const char* stage, const char* reason) noexcept
{
    ASCTOOL_ERROR("[FATAL] npu-check internal failure: hook=%s stage=%s reason=%s", hookName, stage, reason);
    std::abort();
}

template <typename Function>
Function GetOriginalRuntimeFunction(aclrtApiId apiId, const char* hookName) noexcept
{
    const auto function = reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(apiId));
    if (function == nullptr) {
        AbortHookFailure(hookName, "call_original_aclrt", "acltoolGetOriginalRuntimeApi returned nullptr");
    }
    return function;
}

void ApplyRuntimeHooks(const std::set<aclrtApiId>& requiredHooks) noexcept;
void CollectRetiredTraceVariants() noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_RUNTIME_HOOK_H
