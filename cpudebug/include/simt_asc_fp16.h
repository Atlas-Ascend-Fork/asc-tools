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
 * \file simt_asc_fp16.h
 * \brief SIMT half-precision conversion functions
 */
#ifndef ASCENDC_SIMT_ASC_FP16_H
#define ASCENDC_SIMT_ASC_FP16_H

#include "simt_internal_functions.h"

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

#endif // ASCENDC_SIMT_ASC_FP16_H
