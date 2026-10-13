/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file kernel_utils_ceil_oom_que.h
 * \brief
 */
#ifndef ASCENDC_MODULE_UTILS_CEIL_OOM_QUE_H
#define ASCENDC_MODULE_UTILS_CEIL_OOM_QUE_H

#define USE_ISA_INS 1
#define GM_ADDR __gm__ uint8_t*
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
#define UB_ADDR __ubuf__ uint8_t*
#define SSBUF_ADDR __ssbuf__ uint32_t*
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#include "kernel_macros.h"
#include "kernel_log.h"
#include "kernel_event.h"
#if defined(ASCENDC_CPU_DEBUG) && ASCENDC_CPU_DEBUG == 1
#include <set>
#include <map>
#include <sstream>
#include <thread>
#include <iomanip>
#include "stub_def.h"
#include "stub_fun.h"
#endif // ASCENDC_CPU_DEBUG

#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 5102) || (__NPU_ARCH__ == 2103) || (__NPU_ARCH__ == 3003) || \
                              (__NPU_ARCH__ == 3103) || (__NPU_ARCH__ == 3113) || (__NPU_ARCH__ == 3510))

#if !defined(ASCENDC_CPU_DEBUG)
using fp4x2_e2m1_t = float4_e2m1x2_t;
using fp4x2_e1m2_t = float4_e1m2x2_t;
using fp8_e5m2_t = float8_e5m2_t;
using fp8_e4m3fn_t = float8_e4m3_t;
using fp8_e8m0_t = float8_e8m0_t;
#endif
#endif

#include <cstdint>
#ifndef TILING_KEY_VAR
#if defined(ASCENDC_CPU_DEBUG)
extern uint64_t g_tilingKey;
#else
#if __NPU_ARCH__ == 2002
[[block_local]] uint64_t g_tilingKey;
#else
[[workgroup_local]] __gm__ uint64_t g_tilingKey;
#endif
#endif
#define TILING_KEY_VAR g_tilingKey
#endif

namespace AscendC {

__aicore__ constexpr inline uint32_t DivCeil(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

__aicore__ constexpr inline uint32_t AlignUp(uint32_t a, uint32_t b) { return DivCeil(a, b) * b; }

template <bool b>
struct BoolInst {
    using Type = BoolInst<b>;
    static constexpr bool value = b;
};

using TrueType = BoolInst<true>;
using FalseType = BoolInst<false>;

template <typename T, typename U>
struct IsSameType : public FalseType {};

template <typename T>
struct IsSameType<T, T> : public TrueType {};

template <typename... Arg>
struct Tuple {};

template <typename T, typename U, typename... Args>
__aicore__ constexpr bool SupportType()
{
    if constexpr (sizeof...(Args) > 0) {
        return IsSameType<T, U>::value || SupportType<T, Args...>();
    }
    return IsSameType<T, U>::value;
}

} // namespace AscendC

#endif // ASCENDC_MODULE_UTILS_CEIL_OOM_QUE_H
