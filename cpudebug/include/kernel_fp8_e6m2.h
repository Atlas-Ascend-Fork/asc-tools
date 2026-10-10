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
 * \file kernel_fp8_e6m2.h
 * \brief
 */
#ifndef ASCENDC_FP8_E6M2_H
#define ASCENDC_FP8_E6M2_H
#include <cstdint>

namespace float8_e6m2 {
/*
 * @ingroup Fp8e6m2T
 * Unsigned FP8 E6M2: no sign bit, no zero, no denormal, no Inf.
 * bit7-2:      6 bit EXP (bias=48)  +------+----+
 * bit0-1:      2 bit MAN            |EEEEEE| MM |
 *                                   +------+----+
 * Normal value = (1 + M/4) * 2^(E-48), E in [0, 63], M in [0, 3]
 * Min = 0x00 (E=0, M=0)  -> 1 * 2^(-48)
 * Max = 0xFE (E=63, M=10) -> 1.5 * 2^15
 * NaN = 0xFF (E=63, M=11)
 */
struct Fp8e6m2T {
    uint8_t val;

public:
    constexpr Fp8e6m2T(void) : val(0x0u) {}
    Fp8e6m2T(const Fp8e6m2T& fp8) : val(fp8.val) {}
    Fp8e6m2T(const float src) : val(FloatToFp8e6m2(src)) {}
    Fp8e6m2T& operator=(const Fp8e6m2T& fp8)
    {
        if (this != &fp8) {
            this->val = fp8.val;
        }
        return *this;
    }
    uint8_t FloatToFp8e6m2(const float src) const;
    operator float() const;
    float ToFloat() const;
};
} // namespace float8_e6m2
using fp8_e6m2_t = float8_e6m2::Fp8e6m2T;
#endif // ASCENDC_FP8_E6M2_H
