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
#include <npubit.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
inline bool GetWarpEnvironment(NpubitPlatformType& platform, NpubitHwContext& hardware)
{
    const char* value = std::getenv("NPU_COMPUTE_NPUBIT_PLATFORM");
    if (value == nullptr)
        return false;
    if (std::strcmp(value, "V100") == 0)
        platform = V100;
    else if (std::strcmp(value, "V121") == 0)
        platform = V121;
    else if (std::strcmp(value, "V220") == 0)
        platform = V220;
    else
        return false;
    constexpr std::uint32_t kV100DieCount = 2;
    constexpr std::uint32_t kV100CoresPerDie = 18;
    const char* dies = std::getenv("NPU_COMPUTE_NPUBIT_DIE_COUNT");
    const char* preferred = std::getenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE");
    const char* legacy = std::getenv("NPU_COMPUTE_NPUBIT_AICORE_PER_DIE");
    const char* cores = preferred != nullptr ? preferred : legacy;
    std::uint32_t selectedDies = kV100DieCount;
    std::uint32_t selectedCores = kV100CoresPerDie;
    std::uint32_t legacyCores = 0;
    if ((platform != V100 && (dies == nullptr || cores == nullptr)) ||
        (dies != nullptr && !ParsePositiveWarpDecimal(dies, selectedDies)) ||
        (cores != nullptr && !ParsePositiveWarpDecimal(cores, selectedCores))) {
        return false;
    }
    if (preferred != nullptr && legacy != nullptr &&
        (!ParsePositiveWarpDecimal(legacy, legacyCores) || selectedCores != legacyCores))
        return false;
    hardware.die_count = selectedDies;
    hardware.aicore_per_die = selectedCores;
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
