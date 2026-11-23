/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#pragma once
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string_view>
namespace aclpti::profiling {
template <typename Uint>
inline bool ParsePositiveWarpDecimal(std::string_view text, Uint& result)
{
    constexpr Uint kDecimalBase = 10;
    Uint parsed = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        const auto value = static_cast<Uint>(static_cast<int>(digit) - static_cast<int>('0'));
        if (parsed > (std::numeric_limits<Uint>::max() - value) / kDecimalBase) {
            return false;
        }
        parsed = parsed * kDecimalBase + value;
    }
    if (parsed == 0) {
        return false;
    }
    result = parsed;
    return true;
}
// Total allocation in bytes, not the capacity of one subcore slice.
// Slice layout and raw trace decoding remain owned by the npubit SDK.
inline bool GetWarpTraceBufferBytes(std::size_t& bytes)
{
    const char* text = std::getenv("NPU_COMPUTE_NPUBIT_TRACE_BUFFER_BYTES");
    if (text == nullptr) {
        return true;
    }
    std::size_t parsed = 0;
    if (!ParsePositiveWarpDecimal(text, parsed) || parsed == std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    bytes = parsed;
    return true;
}
} // namespace aclpti::profiling
