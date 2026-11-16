/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMORY_ACCESS_MODE_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMORY_ACCESS_MODE_H

#include "acl_san/aclsan_cbdata_device.h"

namespace npucheck {

// Preserve the raw callback mode for VF pipeline classification and diagnostics.
// Unknown values stay unknown so existing validation still rejects them.
constexpr uint32_t NormalizeMemoryAccessMode(uint32_t mode) noexcept
{
    switch (mode) {
        case ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ:
        case ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ:
            return ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        case ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE:
        case ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE:
            return ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        default:
            return mode;
    }
}

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_MEMORY_ACCESS_MODE_H
