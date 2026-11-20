/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_UB_LAUNCH_CONFIG_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_UB_LAUNCH_CONFIG_H

#include "acl/acl_rt.h"

#include <cstddef>
#include <cstdint>

namespace aclsan {

inline bool ReadDynamicUbBytes(const aclrtLaunchKernelCfg* config, uint64_t& bytes) noexcept
{
    bytes = 0;
    if (config == nullptr) {
        return true;
    }
    if (config->numAttrs != 0 && config->attrs == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < config->numAttrs; ++i) {
        if (config->attrs[i].id != ACL_RT_LAUNCH_KERNEL_ATTR_DYN_UBUF_SIZE) {
            continue;
        }
        bytes = config->attrs[i].value.dynUBufSize;
    }
    return true;
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_UB_LAUNCH_CONFIG_H
