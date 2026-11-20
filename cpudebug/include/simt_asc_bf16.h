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
 * \file simt_asc_bf16.h
 * \brief SIMT bfloat16 conversion functions
 */
#ifndef ASCENDC_SIMT_ASC_BF16_H
#define ASCENDC_SIMT_ASC_BF16_H

#include "simt_internal_functions.h"

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

#endif // ASCENDC_SIMT_ASC_BF16_H
