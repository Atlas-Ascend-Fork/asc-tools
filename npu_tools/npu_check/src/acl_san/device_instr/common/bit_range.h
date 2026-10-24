/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_BIT_RANGE_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_BIT_RANGE_H

#include "npu_tool_log.h"

#include <cstdint>
#include <limits>

namespace aclsan {

struct BitRange {
    uint8_t begin = 0;
    uint8_t end = 0;
};

inline uint64_t ExtractBitRange(uint64_t value, BitRange range) noexcept
{
    if (range.begin > range.end || range.end >= std::numeric_limits<uint64_t>::digits) {
        ASCTOOL_ERROR(
            "ExtractBitRange failed: invalid range begin=%u end=%u, expected 0 <= begin <= end < 64",
            static_cast<unsigned int>(range.begin), static_cast<unsigned int>(range.end));
        return 0;
    }
    const uint8_t width = range.end - range.begin + 1;
    const uint64_t mask = width == std::numeric_limits<uint64_t>::digits ? std::numeric_limits<uint64_t>::max() :
                                                                           (UINT64_C(1) << width) - UINT64_C(1);
    return (value >> range.begin) & mask;
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_BIT_RANGE_H
