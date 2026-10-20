/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_PROCESSOR_DIAGNOSTIC_PIPELINE_NAME_H
#define NPU_TOOLS_NPU_CHECK_PROCESSOR_DIAGNOSTIC_PIPELINE_NAME_H

#include "acl_san/aclsan_cbdata_device.h"

#include <cstdint>

namespace npucheck::detail {

inline const char* DevicePipelineName(std::uint32_t pipeline) noexcept
{
    switch (pipeline) {
        case ACLSAN_DEVICE_PIPE_SCALAR:
            return "PIPE_S";
        case ACLSAN_DEVICE_PIPE_VECTOR:
            return "PIPE_V";
        case ACLSAN_DEVICE_PIPE_MATRIX:
            return "PIPE_M";
        case ACLSAN_DEVICE_PIPE_MTE1:
            return "PIPE_MTE1";
        case ACLSAN_DEVICE_PIPE_MTE2:
            return "PIPE_MTE2";
        case ACLSAN_DEVICE_PIPE_MTE3:
            return "PIPE_MTE3";
        case ACLSAN_DEVICE_PIPE_ALL:
            return "PIPE_ALL";
        case ACLSAN_DEVICE_PIPE_FIXPIPE:
            return "PIPE_FIX";
        default:
            return "PIPE_UNKNOWN";
    }
}

} // namespace npucheck::detail

#endif // NPU_TOOLS_NPU_CHECK_PROCESSOR_DIAGNOSTIC_PIPELINE_NAME_H
