/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file simt_internal_functions.h
 * \brief internal SIMT conversion helpers
 */
#ifndef ASCENDC_SIMT_INTERNAL_FUNCTIONS_H
#define ASCENDC_SIMT_INTERNAL_FUNCTIONS_H

#include <cmath>
#include <cfenv>
#include "stub_fun.h"
#include "kernel_simt_cpu.h"
#include "kernel_fp16.h"
#include "kernel_bf16.h"
#include "kernel_vectorized.h"
#include "kernel_operator_common_intf.h"

#define __launch_bounds__(x)

namespace {
constexpr int16_t HALF_MAX_EXP = 31;
const float HALF_SUBNORMAL_THRESHOLD = std::pow(2, -14);
} // namespace

template <typename T, typename U>
constexpr uint32_t GetRoundBitNum()
{
    if constexpr (std::is_same<T, half>::value && std::is_same<U, float>::value) {
        return bfloat16::FP32_MAN_LEN - static_cast<uint32_t>(Fp16BasicParam::K_FP16_MAN_LEN);
    } else if constexpr (std::is_same<T, bfloat16_t>::value && std::is_same<U, float>::value) {
        return bfloat16::FP32_MAN_LEN - bfloat16::BF16_MAN_LEN;
    }
    return 0;
}

template <typename T>
constexpr uint32_t GetMantissaLen()
{
    if constexpr (std::is_same<T, half>::value) {
        return static_cast<uint32_t>(Fp16BasicParam::K_FP16_MAN_LEN);
    } else if constexpr (std::is_same<T, bfloat16_t>::value) {
        return bfloat16::BF16_MAN_LEN;
    }
    return 0;
}

template <typename T, typename U, ROUND rnd>
void HandleRound(uint32_t sign, int16_t& exp, uint32_t& mantissa, uint32_t round_part)
{
    constexpr uint32_t round_bit_num = GetRoundBitNum<T, U>();
    constexpr uint32_t round_carry = 1U << round_bit_num;
    constexpr uint32_t round_bit_map = round_carry - 1;
    constexpr uint32_t round_first_bit = 1U << (round_bit_num - 1);
    constexpr uint32_t round_left_bit = round_first_bit - 1;

    if constexpr (rnd == ROUND::CAST_RINT) {
        if ((round_part & round_first_bit) != 0) {
            if ((round_part & round_left_bit) != 0) {
                mantissa += 1;
            } else if ((mantissa & 1) == 1) {
                mantissa += 1;
            }
        }
    } else if constexpr (rnd == ROUND::CAST_FLOOR) {
        if ((sign == 1) && (round_part != 0)) {
            mantissa += 1;
        }
    } else if constexpr (rnd == ROUND::CAST_CEIL) {
        if ((sign == 0) && (round_part != 0)) {
            mantissa += 1;
        }
    } else if constexpr (rnd == ROUND::CAST_ROUND) {
        if ((round_part & round_first_bit) != 0) {
            mantissa += 1;
        }
    } else if constexpr (rnd == ROUND::CAST_ODD) {
        if ((round_part != 0) && ((mantissa & 1) == 0)) {
            mantissa += 1;
        }
    }

    if ((mantissa & (1U << GetMantissaLen<T>())) != 0) {
        exp += 1;
    }
}

template <
    ROUND rnd = ROUND::CAST_RINT, RoundingSaturation sat = RoundingSaturation::RS_DISABLE_VALUE, typename SRC_TYPE>
float CastIntegralToFloat(SRC_TYPE src)
{
    if constexpr (
        rnd == ROUND::CAST_RINT || rnd == ROUND::CAST_FLOOR || rnd == ROUND::CAST_CEIL || rnd == ROUND::CAST_TRUNC) {
        fenv_t env;
        std::fegetenv(&env);
        constexpr int32_t round = [] {
            if constexpr (rnd == ROUND::CAST_RINT) {
                return FE_TONEAREST;
            } else if constexpr (rnd == ROUND::CAST_FLOOR) {
                return FE_DOWNWARD;
            } else if constexpr (rnd == ROUND::CAST_CEIL) {
                return FE_UPWARD;
            } else if constexpr (rnd == ROUND::CAST_TRUNC) {
                return FE_TOWARDZERO;
            }
        }();
        std::fesetround(round);
        float res = static_cast<float>(src);
        std::fesetenv(&env);
        return res;
    }

    float f = static_cast<float>(src);
    SRC_TYPE tmp = static_cast<SRC_TYPE>(f);
    if (src == tmp) {
        return f;
    }

    float f_up = 0;
    float f_down = 0;
    if (src < tmp) {
        f_up = f;
        f_down = std::nextafter(f, -INFINITY);
    } else {
        f_up = std::nextafter(f, INFINITY);
        f_down = f;
    }

    if constexpr (rnd == ROUND::CAST_ROUND) {
        SRC_TYPE src_up = static_cast<SRC_TYPE>(f_up);
        SRC_TYPE src_down = static_cast<SRC_TYPE>(f_down);
        if (src_up - src == src - src_down) {
            return src > 0 ? f_up : f_down;
        } else if (src_up - src < src - src_down) {
            return f_up;
        } else {
            return f_down;
        }
    } else if constexpr (rnd == ROUND::CAST_ODD) {
        uint32_t f_bits = *reinterpret_cast<uint32_t*>(&f_up);
        return (f_bits & 1) == 0 ? f_up : f_down;
    }

    return static_cast<float>(src);
}

template <
    ROUND rnd = ROUND::CAST_RINT, RoundingSaturation sat = RoundingSaturation::RS_DISABLE_VALUE, typename SRC_TYPE>
float __cvt_float(SRC_TYPE src)
{
    static_assert(
        std::is_same_v<SRC_TYPE, int32_t> || std::is_same_v<SRC_TYPE, uint32_t> || std::is_same_v<SRC_TYPE, uint64_t> ||
            std::is_same_v<SRC_TYPE, int64_t> || std::is_same_v<SRC_TYPE, half> || std::is_same_v<SRC_TYPE, float> ||
            std::is_same_v<SRC_TYPE, bfloat16_t>,
        "src type can only be int32_t/uint32_t/int64_t/uint64_t and half/float/bfloat_t");
    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");

    if constexpr (std::is_same_v<SRC_TYPE, half> || std::is_same_v<SRC_TYPE, bfloat16_t>) {
        if (__isnan(src)) {
            return NAN;
        }
        if (__isinf(src)) {
            return copysignf(INFINITY, src);
        }
        return src.ToFloat();
    } else if constexpr (std::is_same_v<SRC_TYPE, float>) {
        if constexpr (rnd == ROUND::CAST_RINT) {
            return rintf(src);
        } else if constexpr (rnd == ROUND::CAST_ROUND) {
            return roundf(src);
        } else if constexpr (rnd == ROUND::CAST_FLOOR) {
            return floorf(src);
        } else if constexpr (rnd == ROUND::CAST_CEIL) {
            return ceilf(src);
        } else if constexpr (rnd == ROUND::CAST_TRUNC) {
            return truncf(src);
        } else {
            return src;
        }
    }
    if constexpr (std::is_integral<SRC_TYPE>::value) {
        return CastIntegralToFloat<rnd, sat>(src);
    }
    return 0.0f;
}

template <RoundingSaturation rst, typename T, typename U>
bool HandleOverflow(uint16_t sign, int32_t exp, U& res)
{
    int64_t ctrl48 = AscendC::GetCtrlSpr<48, 48>();
    int64_t ctrl60 = AscendC::GetCtrlSpr<60, 60>();
    if constexpr (std::is_same_v<T, half>) {
        if (exp < 0) { // underflow
            res = sign << static_cast<uint16_t>(Fp16BasicParam::K_FP16_SIGN_INDEX);
        } else if (exp >= HALF_MAX_EXP) { // overflow
            if (ctrl60 == 0) {
                if constexpr (rst == RoundingSaturation::RS_DISABLE_VALUE) {
                    res = (sign << static_cast<uint16_t>(Fp16BasicParam::K_FP16_SIGN_INDEX)) |
                          static_cast<uint16_t>(Fp16BasicParam::K_FP16_EXP_MASK);
                } else {
                    res = (sign << static_cast<uint16_t>(Fp16BasicParam::K_FP16_SIGN_INDEX)) |
                          static_cast<uint16_t>(Fp16BasicParam::K_FP16_MAX);
                }
            } else if (ctrl60 == 1) {
                if (ctrl48 == 1) {
                    res = (sign << static_cast<uint16_t>(Fp16BasicParam::K_FP16_SIGN_INDEX)) |
                          static_cast<uint16_t>(Fp16BasicParam::K_FP16_EXP_MASK);
                } else {
                    res = (sign << static_cast<uint16_t>(Fp16BasicParam::K_FP16_SIGN_INDEX)) |
                          static_cast<uint16_t>(Fp16BasicParam::K_FP16_MAX);
                }
            }
        } else {
            return false;
        }
        return true;
    } else if constexpr (std::is_same_v<T, bfloat16_t>) {
        if (exp == static_cast<int32_t>(Fp32BasicParam::K_FP32_MAX_EXP)) {
            if (ctrl48 == 1) {
                res = (sign << bfloat16::BF16_SIGN_INDEX) | bfloat16::BF16_INFINITY;
            } else {
                res = (sign << bfloat16::BF16_SIGN_INDEX) | bfloat16::BF16_ABS_MAX;
            }
            return true;
        }
        return false;
    }
    return false;
}

template <RoundingSaturation rst>
bool use_saturation()
{
    int64_t ctrl60 = AscendC::GetCtrlSpr<60, 60>();
    if (ctrl60 == 0)
        return (rst == RoundingSaturation::RS_ENABLE_VALUE);
    return (AscendC::GetCtrlSpr<48, 48>()) == 0; // 48: ctrl[48]
}

template <ROUND rnd, RoundingSaturation rst, typename SRC_TYPE>
int32_t __cvt_int32_t(SRC_TYPE x)
{
    static_assert(
        std::is_same_v<SRC_TYPE, half> || std::is_same_v<SRC_TYPE, float> || std::is_same_v<SRC_TYPE, bfloat16_t>,
        "src type can only be half/float/bfloat_t");

    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");

    static_assert(rst == RoundingSaturation::RS_ENABLE_VALUE, "sat type can only be: RS_ENABLE");
    if (__isnan(x)) {
        return 0;
    }
    if (__isinf(x)) {
        if (x > SRC_TYPE{0}) {
            return INT32_MAX;
        } else {
            return INT32_MIN;
        }
    }
    if constexpr (std::is_same_v<SRC_TYPE, float>) {
        return static_cast<int32_t>(__cvt_float<rnd>(x));
    } else if constexpr (std::is_same_v<SRC_TYPE, half> || std::is_same_v<SRC_TYPE, bfloat16_t>) {
        float f = __cvt_float<rnd>(x);
        return static_cast<int32_t>(__cvt_float<rnd>(f));
    }
}

template <ROUND rnd, RoundingSaturation rst, typename SRC_TYPE>
uint32_t __cvt_uint32_t(SRC_TYPE x)
{
    static_assert(
        std::is_same_v<SRC_TYPE, half> || std::is_same_v<SRC_TYPE, float> || std::is_same_v<SRC_TYPE, bfloat16_t>,
        "src type can only be half/float/bfloat_t");

    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");

    static_assert(rst == RoundingSaturation::RS_ENABLE_VALUE, "sat type can only be: RS_ENABLE");
    if (__isnan(x) || x < SRC_TYPE{0}) {
        return 0;
    }
    if (__isinf(x) && x > SRC_TYPE{0}) {
        return UINT32_MAX;
    }
    if constexpr (std::is_same_v<SRC_TYPE, float>) {
        return static_cast<uint32_t>(__cvt_float<rnd>(x));
    } else if constexpr (std::is_same_v<SRC_TYPE, half> || std::is_same_v<SRC_TYPE, bfloat16_t>) {
        float f = __cvt_float<rnd>(x);
        return static_cast<uint32_t>(__cvt_float<rnd>(f));
    }
}

template <ROUND rnd, RoundingSaturation rst, typename SRC_TYPE>
int64_t __cvt_int64_t(SRC_TYPE x)
{
    static_assert(std::is_same_v<SRC_TYPE, float>, "src type can only be float");
    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");
    static_assert(rst == RoundingSaturation::RS_ENABLE_VALUE, "sat type can only be: RS_ENABLE");
    if (__isnan(x)) {
        return 0;
    }
    if (__isinf(x)) {
        if (x > SRC_TYPE{0}) {
            return INT64_MAX;
        } else {
            return INT64_MIN;
        }
    }
    if constexpr (std::is_same_v<SRC_TYPE, float>) {
        return static_cast<int64_t>(__cvt_float<rnd>(x));
    }
}

template <ROUND rnd, RoundingSaturation rst, typename SRC_TYPE>
uint64_t __cvt_uint64_t(SRC_TYPE x)
{
    static_assert(std::is_same_v<SRC_TYPE, float>, "src type can only be float");
    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");
    static_assert(rst == RoundingSaturation::RS_ENABLE_VALUE, "sat type can only be: RS_ENABLE");
    if constexpr (std::is_same_v<SRC_TYPE, float>) {
        if (__isnan(x) || x < SRC_TYPE{0}) {
            return 0;
        }
        if (__isinf(x) && x > SRC_TYPE{0}) {
            return UINT64_MAX;
        }
        return static_cast<uint64_t>(__cvt_float<rnd>(x));
    }
}

template <
    ROUND rnd = ROUND::CAST_RINT, RoundingSaturation rst = RoundingSaturation::RS_DISABLE_VALUE, typename SRC_TYPE>
float2 __cvt_float2(SRC_TYPE src)
{
    static_assert(
        std::is_same_v<SRC_TYPE, half2> || std::is_same_v<SRC_TYPE, bfloat16x2_t> ||
            std::is_same_v<SRC_TYPE, float8_e4m3x2_t> || std::is_same_v<SRC_TYPE, float8_e5m2x2_t>,
        "src type can only be half2/bfloat16x2_t/float8_e4m3x2_t/float8_e5m2x2_t");
    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");
    float2 res = {src.x.ToFloat(), src.y.ToFloat()};
    if constexpr (std::is_same_v<SRC_TYPE, half2> || std::is_same_v<SRC_TYPE, bfloat16x2_t>) {
        res = {__cvt_float(src.x), __cvt_float(src.y)};
    }
    return res;
}

#endif // ASCENDC_SIMT_INTERNAL_FUNCTIONS_H
