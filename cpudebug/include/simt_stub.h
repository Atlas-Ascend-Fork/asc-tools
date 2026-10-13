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
 * \file simt_stub.h
 * \brief
 */
#ifndef ASCENDC_SIMT_STUB_H
#define ASCENDC_SIMT_STUB_H

#if defined(ASCENDC_CPU_DEBUG)
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
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

template <ROUND rnd, RoundingSaturation rst>
half __cvt_half(float x)
{
    float conform_flag = 0;
    if ((x > 0) && x < HALF_SUBNORMAL_THRESHOLD) {
        x += HALF_SUBNORMAL_THRESHOLD;
        conform_flag = -1;
    }
    if ((x < 0) && (x > (-1 * HALF_SUBNORMAL_THRESHOLD))) {
        x -= HALF_SUBNORMAL_THRESHOLD;
        conform_flag = 1;
    }

    uint32_t f_bits = *reinterpret_cast<uint32_t*>(&x);
    uint16_t sign = bfloat16::Fp32ExtracSign(f_bits);
    uint32_t exp = bfloat16::Fp32ExtracExp(f_bits);
    uint32_t mantissa = f_bits & FP32_MAN_MASK;
    uint16_t res = 0;
    // handle INF / NaN
    if (exp == static_cast<uint32_t>(Fp32BasicParam::K_FP32_MAX_EXP)) {
        if (use_saturation<rst>()) {
            if (mantissa != 0) {
                return half(0);
            } else {
                // Inf/-Inf
                uint16_t half_max = static_cast<uint16_t>(Fp16BasicParam::K_FP16_MAX);
                half tmp = *reinterpret_cast<half*>(&half_max);
                return (sign == 0) ? tmp : (half)0 - tmp;
            }
        } else {
            res = (sign << static_cast<uint32_t>(Fp16BasicParam::K_FP16_SIGN_INDEX)) |
                  (mantissa ? static_cast<uint16_t>(Fp16BasicParam::K_FP16_ABS_MAX) :
                              static_cast<uint16_t>(Fp16BasicParam::K_FP16_EXP_MASK));
            return *reinterpret_cast<half*>(&res);
        }
    }

    int16_t half_exp =
        static_cast<int16_t>(exp) - (bfloat16::FP32_EXP_BIAS - static_cast<uint16_t>(Fp16BasicParam::K_FP16_EXP_BIAS));
    if (HandleOverflow<rst, half>(sign, half_exp, res)) {
        return *reinterpret_cast<half*>(&res);
    }

    uint32_t round_bit_num = bfloat16::FP32_MAN_LEN - static_cast<uint32_t>(Fp16BasicParam::K_FP16_MAN_LEN);
    uint32_t half_mantissa = mantissa >> round_bit_num;
    uint32_t round_part = mantissa & ((1 << round_bit_num) - 1);

    HandleRound<half, float, rnd>(sign, half_exp, half_mantissa, round_part);
    res = (sign << static_cast<uint16_t>(Fp16BasicParam::K_FP16_EXP_BIAS)) |
          (half_exp << static_cast<uint16_t>(Fp16BasicParam::K_FP16_MAN_LEN)) |
          (half_mantissa & static_cast<uint32_t>(Fp16BasicParam::K_FP16_MAN_MASK));
    if (HandleOverflow<rst, half>(sign, half_exp, res)) {
        return *reinterpret_cast<half*>(&res);
    }
    half tmp = *reinterpret_cast<half*>(&res);
    tmp += half(conform_flag * HALF_SUBNORMAL_THRESHOLD);
    return tmp;
}

template <ROUND rnd, RoundingSaturation rst>
bfloat16_t __cvt_bfloat16_t(float x)
{
    uint32_t f_bits = *reinterpret_cast<uint32_t*>(&x);
    uint16_t sign = bfloat16::Fp32ExtracSign(f_bits);
    uint32_t exp = bfloat16::Fp32ExtracExp(f_bits);
    uint32_t mantissa = f_bits & FP32_MAN_MASK;
    uint16_t res = 0;
    // handle INF / NaN
    if (exp == static_cast<int32_t>(Fp32BasicParam::K_FP32_MAX_EXP)) {
        bool use_saturation = false;
        int64_t ctrl60 = AscendC::GetCtrlSpr<60, 60>();
        if (ctrl60 == 0) {
            use_saturation = (rst == RoundingSaturation::RS_ENABLE_VALUE);
        } else {
            int64_t ctrl48 = AscendC::GetCtrlSpr<48, 48>();
            use_saturation = (ctrl48 == 0);
        }
        if (use_saturation) { // NaN→0，Inf→max
            if (mantissa != 0) {
                return bfloat16_t(0);
            } else {
                // Inf/-Inf
                uint16_t bf16_max = static_cast<uint16_t>(0x7F7F);
                bfloat16_t tmp = *reinterpret_cast<bfloat16_t*>(&bf16_max);
                return (sign == 0) ? tmp : (bfloat16_t)0 - tmp;
            }
        } else {
            res = mantissa == 0 ? ((sign << bfloat16::BF16_SIGN_INDEX) | bfloat16::BF16_EXP_MASK) : bfloat16::BF16_NAN;
            return *reinterpret_cast<bfloat16_t*>(&res);
        }
    }
    if (exp == 0 && mantissa == 0) {
        res = sign << bfloat16::BF16_SIGN_INDEX;
        return *reinterpret_cast<bfloat16_t*>(&res);
    }

    int16_t bf16_exp = static_cast<int16_t>(exp);
    uint32_t round_bit_num = bfloat16::FP32_MAN_LEN - bfloat16::BF16_MAN_LEN;
    uint32_t bf16_mantissa = mantissa >> round_bit_num;
    uint32_t round_part = mantissa & ((1 << round_bit_num) - 1);

    HandleRound<bfloat16_t, float, rnd>(sign, bf16_exp, bf16_mantissa, round_part);
    res = (sign << bfloat16::BF16_SIGN_INDEX) | (bf16_exp << bfloat16::BF16_MAN_LEN) |
          (bf16_mantissa & bfloat16::BF16_MAN_MASK);
    (void)HandleOverflow<rst, bfloat16_t>(sign, bf16_exp, res);

    return *reinterpret_cast<bfloat16_t*>(&res);
}

template <
    ROUND rnd = ROUND::CAST_RINT, RoundingSaturation rst = RoundingSaturation::RS_DISABLE_VALUE, typename SRC_TYPE>
half __cvt_half(SRC_TYPE src)
{
    int64_t ctrl48 = AscendC::GetCtrlSpr<48, 48>();
    int64_t ctrl60 = AscendC::GetCtrlSpr<60, 60>();

    auto make_half = [](uint16_t bits) { return *reinterpret_cast<half*>(&bits); };

    if constexpr (std::is_same_v<SRC_TYPE, half>) {
        if (__isnan(src)) {
            return ctrl48 == 1 ? make_half(static_cast<uint16_t>(Fp16BasicParam::K_FP16_ABS_MAX)) : half(0);
        }
        if (__isinf(src)) {
            if (ctrl48 == 1) {
                return src > (half)0 ? make_half(static_cast<uint16_t>(Fp16BasicParam::K_FP16_EXP_MASK)) :
                                       make_half(0xFC00);
            }
            half tmp = make_half(static_cast<uint16_t>(Fp16BasicParam::K_FP16_MAX));
            return src > (half)0 ? tmp : (half)0 - tmp;
        }
        float tmp = __cvt_float<rnd, rst>(src);
        tmp = __cvt_float<rnd, rst>(tmp);
        return __cvt_half<rnd, rst>(tmp);
    }

    if constexpr (std::is_same_v<SRC_TYPE, bfloat16_t>) {
        if (__isnan(src)) {
            uint16_t half_nan = static_cast<uint16_t>(Fp16BasicParam::K_FP16_ABS_MAX);
            if (ctrl60 == 0) {
                return rst == RoundingSaturation::RS_DISABLE_VALUE ? make_half(half_nan) : half(0);
            }
            return ctrl48 == 1 ? make_half(half_nan) : half(0);
        }
        float temp = __cvt_float<rnd, rst>(src);
        return __cvt_half<rnd, rst>(temp);
    }

    return half(0);
}

template <
    ROUND rnd = ROUND::CAST_RINT, RoundingSaturation rst = RoundingSaturation::RS_DISABLE_VALUE, typename SRC_TYPE>
bfloat16_t __cvt_bfloat16_t(SRC_TYPE src)
{
    int64_t ctrl48 = AscendC::GetCtrlSpr<48, 48>();
    auto make_bf16 = [](uint16_t bits) { return *reinterpret_cast<bfloat16_t*>(&bits); };

    if (__isnan(src)) {
        if constexpr (std::is_same_v<SRC_TYPE, bfloat16_t> || std::is_same_v<SRC_TYPE, half>) {
            return ctrl48 == 1 ? make_bf16(bfloat16::BF16_NAN) : bfloat16_t(0);
        }
    }

    auto handle_inf = [&](auto zero_val) {
        if (ctrl48 == 1) {
            return src > zero_val ? make_bf16(static_cast<uint16_t>(bfloat16::BF16_EXP_MASK)) : make_bf16(0xFF80);
        }
        bfloat16_t tmp = make_bf16(0x7F7F);
        return src > zero_val ? tmp : (bfloat16_t)0 - tmp;
    };

    if constexpr (std::is_same_v<SRC_TYPE, half>) {
        if (__isinf(src))
            return handle_inf((half)0);
        float temp = __cvt_float<rnd, rst>(src);
        return __cvt_bfloat16_t<rnd, rst>(temp);
    }

    if constexpr (std::is_same_v<SRC_TYPE, bfloat16_t>) {
        if (__isinf(src))
            return handle_inf((bfloat16_t)0);
        float temp = __cvt_float<rnd, rst>(src);
        temp = __cvt_float<rnd, rst>(temp);
        return __cvt_bfloat16_t<rnd, rst>(temp);
    }

    return bfloat16_t(0);
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

template <ROUND rnd, RoundingSaturation rst>
half __cvt_half(int32_t x)
{
    float temp = __cvt_float<rnd, rst>(x);
    return __cvt_half<rnd, rst>(temp);
}

template <ROUND rnd, RoundingSaturation rst>
half __cvt_half(uint32_t x)
{
    float temp = __cvt_float<rnd, rst>(x);
    return __cvt_half<rnd, rst>(temp);
}

template <ROUND rnd, RoundingSaturation rst>
bfloat16_t __cvt_bfloat16_t(int32_t x)
{
    float temp = __cvt_float<rnd, rst>(x);
    return __cvt_bfloat16_t<rnd, rst>(temp);
}

template <ROUND rnd, RoundingSaturation rst>
bfloat16_t __cvt_bfloat16_t(uint32_t x)
{
    float temp = __cvt_float<rnd, rst>(x);
    return __cvt_bfloat16_t<rnd, rst>(temp);
}

template <ROUND rnd = ROUND::CAST_ROUND, RoundingSaturation rst = RoundingSaturation::RS_DISABLE_VALUE>
float2 __cvt_float2(hifloat8x2_t src)
{
    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");
    float2 res{src.x.ToFloat(), src.y.ToFloat()};
    return res;
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

template <
    ROUND rnd = ROUND::CAST_RINT, RoundingSaturation rst = RoundingSaturation::RS_DISABLE_VALUE, typename SRC_TYPE>
bfloat16x2_t __cvt_bfloat16x2_t(SRC_TYPE src)
{
    static_assert(std::is_same_v<SRC_TYPE, float2>, "stc type can only be float2");
    static_assert(
        (rnd == ROUND::CAST_RINT) || (rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_FLOOR) ||
            (rnd == ROUND::CAST_CEIL) || (rnd == ROUND::CAST_TRUNC),
        "rnd type can only be: ROUND_R, ROUND_A, ROUND_F, ROUND_C,ROUND_Z");
    bfloat16x2_t tmp;
    tmp.x = __cvt_bfloat16_t<rnd, rst>(src.x);
    tmp.y = __cvt_bfloat16_t<rnd, rst>(src.y);
    return tmp;
}

template <
    ROUND rnd = ROUND::CAST_RINT, RoundingSaturation rst = RoundingSaturation::RS_DISABLE_VALUE, typename SRC_TYPE>
half2 __cvt_half2(SRC_TYPE src)
{
    int64_t ctrl48 = AscendC::GetCtrlSpr<48, 48>();
    half2 res;
    if constexpr (std::is_same_v<SRC_TYPE, hifloat8x2_t>) {
        if (ctrl48 == 1) {
            auto ConvertHif8ToHalf = [](hifloat8_t hif8) -> half {
                uint8_t hif8_bits = *reinterpret_cast<uint8_t*>(&hif8);
                // hifloat8 nan: 0x80 -> half nan: 0x7FFF
                if (hif8_bits == 0x80) {
                    uint16_t half_nan = 0x7FFF;
                    return *reinterpret_cast<half*>(&half_nan);
                }
                // hifloat8 inf: 0x6F -> half inf: 0x7C00
                if (hif8_bits == 0x6F) {
                    uint16_t half_inf = 0x7C00;
                    return *reinterpret_cast<half*>(&half_inf);
                }
                // hifloat8 -inf: 0xEF -> half -inf: 0xFC00
                if (hif8_bits == 0xEF) {
                    uint16_t half_neg_inf = 0xFC00;
                    return *reinterpret_cast<half*>(&half_neg_inf);
                }
                return half(hif8.ToFloat());
            };
            res = {ConvertHif8ToHalf(src.x), ConvertHif8ToHalf(src.y)};
        } else {
            auto ConvertHif8ToHalfSat = [](hifloat8_t hif8) -> half {
                uint8_t hif8_bits = *reinterpret_cast<uint8_t*>(&hif8);
                // hifloat8 nan: 0x80 -> half 0
                if (hif8_bits == 0x80) {
                    return half(0);
                }
                // hifloat8 inf: 0x6F -> half max: 0x7BFF
                if (hif8_bits == 0x6F) {
                    uint16_t half_max = 0x7BFF;
                    return *reinterpret_cast<half*>(&half_max);
                }
                // hifloat8 -inf: 0xEF -> half min: 0xFBFF
                if (hif8_bits == 0xEF) {
                    uint16_t half_min = 0xFBFF;
                    return *reinterpret_cast<half*>(&half_min);
                }
                return half(hif8.ToFloat());
            };
            res = {ConvertHif8ToHalfSat(src.x), ConvertHif8ToHalfSat(src.y)};
        }
    } else {
        res = {__cvt_half<rnd, rst>(src.x), __cvt_half<rnd, rst>(src.y)};
    }
    return res;
}

template <ROUND rnd, RoundingSaturation rst, typename SRC_TYPE>
hifloat8x2_t __cvt_hifloat8x2_t(SRC_TYPE src)
{
    static_assert(
        std::is_same_v<SRC_TYPE, float2> || std::is_same_v<SRC_TYPE, half2>, "src type can only be float2/half2");
    static_assert((rnd == ROUND::CAST_ROUND) || (rnd == ROUND::CAST_HYBRID), "rnd type can only be: ROUND_A, ROUND_H");

    auto make_hif8 = [](uint8_t bits) { return *reinterpret_cast<hifloat8_t*>(&bits); };

    auto ConvertFloatToHiF8 = [&](float f) -> hifloat8_t {
        uint32_t f_bits = *reinterpret_cast<uint32_t*>(&f);
        uint16_t sign = (f_bits >> 31) & 0x1;
        bool is_overflow = (sign == 0 && f_bits >= 0x47200000) || (sign == 1 && f_bits >= 0xC7200000);

        if ((f_bits >= 0x47000000) && (f_bits < 0x47200000))
            return make_hif8(0x6E);
        if ((f_bits >= 0xC7000000) && (f_bits < 0xC7200000))
            return make_hif8(0xEE);

        if (__isnan(f) || __isinf(f) || is_overflow) {
            if (use_saturation<rst>()) {
                if (__isnan(f))
                    return hifloat8_t(0.0f);
                return make_hif8(sign == 1 ? 0xEE : 0x6E);
            }
            if (__isnan(f))
                return make_hif8(0x80);
            return make_hif8(sign == 1 ? 0xEF : 0x6F);
        }
        return hifloat8_t(f);
    };

    if constexpr (std::is_same_v<SRC_TYPE, float2>) {
        return {ConvertFloatToHiF8(src.x), ConvertFloatToHiF8(src.y)};
    } else {
        float2 tmp{src.x.ToFloat(), src.y.ToFloat()};
        return {ConvertFloatToHiF8(tmp.x), ConvertFloatToHiF8(tmp.y)};
    }
}

template <ROUND rnd, RoundingSaturation rst, typename SRC_TYPE>
float8_e4m3x2_t __cvt_float8_e4m3x2_t(SRC_TYPE src)
{
    static_assert(std::is_same_v<SRC_TYPE, float2>, "src type can only be float2");
    static_assert(rnd == ROUND::CAST_RINT, "rnd type can only be: ROUND_R");

    auto make_fp8 = [](uint8_t bits) { return *reinterpret_cast<fp8_e4m3fn_t*>(&bits); };

    auto ConvertFloatToFp8E4M3 = [&](float f) -> fp8_e4m3fn_t {
        uint32_t f_bits = *reinterpret_cast<uint32_t*>(&f);
        uint16_t exp = (f_bits >> 23) & 0xFF;
        uint32_t mantissa = f_bits & 0x7FFFFF;
        uint16_t sign = (f_bits >> 31) & 0x1;
        bool is_overflow = (exp > 0x87) || ((exp == 0x87) && (mantissa > 0x680000));

        if (__isnan(f) || __isinf(f) || is_overflow) {
            if (use_saturation<rst>()) {
                if (__isnan(f)) {
                    int64_t ctrl50 = AscendC::GetCtrlSpr<50, 50>();
                    return ctrl50 == 0 ? fp8_e4m3fn_t(0.0f) : make_fp8(0x7F);
                }
                return make_fp8(sign == 1 ? 0xFE : 0x7E);
            }
            return make_fp8(0x7F);
        }
        return fp8_e4m3fn_t(f);
    };

    return {ConvertFloatToFp8E4M3(src.x), ConvertFloatToFp8E4M3(src.y)};
}

template <ROUND rnd, RoundingSaturation rst, typename SRC_TYPE>
float8_e5m2x2_t __cvt_float8_e5m2x2_t(SRC_TYPE src)
{
    static_assert(std::is_same_v<SRC_TYPE, float2>, "src type can only be float2");
    static_assert(rnd == ROUND::CAST_RINT, "rnd type can only be: ROUND_R");

    auto make_fp8 = [](uint8_t bits) { return *reinterpret_cast<fp8_e5m2_t*>(&bits); };

    auto ConvertFloatToFp8E5M2 = [&](float f) -> fp8_e5m2_t {
        uint32_t f_bits = *reinterpret_cast<uint32_t*>(&f);
        uint16_t exp = (f_bits >> 23) & 0xFF;
        uint32_t mantissa = f_bits & 0x7FFFFF;
        uint16_t sign = (f_bits >> 31) & 0x1;
        bool is_overflow = (exp > 0x8E) || ((exp == 0x8E) && (mantissa >= 0x700000));

        if (__isnan(f) || __isinf(f) || is_overflow) {
            if (use_saturation<rst>()) {
                if (__isnan(f)) {
                    int64_t ctrl50 = AscendC::GetCtrlSpr<50, 50>();
                    return ctrl50 == 0 ? fp8_e5m2_t(0.0f) : make_fp8(0x7F);
                }
                return make_fp8(sign == 1 ? 0xFB : 0x7B);
            }
            if (__isnan(f))
                return make_fp8(0x7F);
            return make_fp8(sign == 1 ? 0xFC : 0x7C);
        }
        return fp8_e5m2_t(f);
    };

    return {ConvertFloatToFp8E5M2(src.x), ConvertFloatToFp8E5M2(src.y)};
}

static half2 __float22half2_rz(float2 const x)
{
    half2 res;
    res.x = __cvt_half<ROUND::CAST_TRUNC, RoundingSaturation::RS_DISABLE_VALUE>(x.x);
    res.y = __cvt_half<ROUND::CAST_TRUNC, RoundingSaturation::RS_DISABLE_VALUE>(x.y);
    return res;
}

static float2 __half22float2(half2 const x)
{
    float2 res;
    res.x = __cvt_float<ROUND::CAST_RINT, RoundingSaturation::RS_DISABLE_VALUE>(x.x);
    res.y = __cvt_float<ROUND::CAST_RINT, RoundingSaturation::RS_DISABLE_VALUE>(x.y);
    return res;
}

namespace cce {
template <auto funcPtr, typename... Args>
void async_invoke(const dim3& dim, Args&&... args)
{
    g_threadDimX = dim.x;
    g_threadDimY = dim.y;
    g_threadDimZ = dim.z;

    blockDim.x = g_threadDimX;
    blockDim.y = g_threadDimY;
    blockDim.z = g_threadDimZ;

    blockIdx.x = block_idx;

    gridDim.x = block_num;

    AscendC::Simt::ThreadBlock& threadBlock = AscendC::Simt::ThreadBlock::GetBlockInstance();
    const uint32_t threadNum = g_threadDimX * g_threadDimY * g_threadDimZ;
    threadBlock.Init(threadNum);
    auto func = [&args...]() { funcPtr(args...); };
    for (uint32_t i = 0; i < threadNum; i++) {
        threadBlock.Schedule(func, i);
    }
    threadBlock.FinishJobs();
}
} // namespace cce

enum L1CacheType : uint32_t { NON_CACHEABLE = 0, CACHEABLE = 1 };
enum class LD_L2CacheType : uint32_t { L2_CACHE_HINT_NORMAL_FV = 0 };
enum class ST_L2CacheType : uint32_t { L2_CACHE_HINT_NORMAL_FV = 0 };

template <
    LD_L2CacheType L2Cache = LD_L2CacheType::L2_CACHE_HINT_NORMAL_FV,
    L1CacheType L1CacheType = L1CacheType::NON_CACHEABLE, typename T>
T __ldg(__gm__ T* address)
{
    return *address;
}
template <
    ST_L2CacheType L2Cache = ST_L2CacheType::L2_CACHE_HINT_NORMAL_FV,
    L1CacheType L1CacheType = L1CacheType::NON_CACHEABLE, typename T>
void __stg(__gm__ T* address, T val)
{
    *address = val;
}

constexpr int32_t warpSize = 32;

#endif
#endif

#endif
