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
 * \file simt_asc_fp8.h
 * \brief SIMT fp8 and hifloat8 conversion functions
 */
#ifndef ASCENDC_SIMT_ASC_FP8_H
#define ASCENDC_SIMT_ASC_FP8_H

#include "simt_internal_functions.h"

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

#endif // ASCENDC_SIMT_ASC_FP8_H
