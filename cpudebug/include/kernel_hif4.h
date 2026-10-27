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
 * \file kernel_hif4.h
 * \brief
 */
#ifndef ASCENDC_HIF4_H
#define ASCENDC_HIF4_H
#include <cstdint>
#include "kernel_bf16.h"

namespace hifloat4 {
// FP4 (E2M1)
constexpr uint32_t FP4_E1M2_MAN_HIDE_BIT = 0x4;
constexpr uint32_t FP4_SIGN_INDEX = 3;
constexpr uint32_t FP4_T_POS_MAX = 0x7;
constexpr uint32_t FP4_T_NEG_MAX = 0xf;
constexpr uint32_t FP4_T_NAN = 0;
constexpr uint32_t FP4_E1M2_MAX_MAN = 0x3;
constexpr uint32_t FP4_E1M2_MAX_EXP = 0x1;
constexpr uint32_t FP4_E1M2_EXP_LEN = 1;
constexpr uint32_t FP4_E1M2_MAN_LEN = 2;
inline uint8_t FP4E1M2_CONSTRUCTOR(uint16_t s, uint16_t e, uint16_t m)
{
    return (((s) << FP4_SIGN_INDEX) | ((e) << FP4_E1M2_MAN_LEN) | ((m)&FP4_E1M2_MAX_MAN));
}

/*
 * @ingroup Fp4e1m2T
 * bit3:        1 bit SIGN      +---+---+----+
 * bit2-1:      1 bit EXP       | S | E | MM |
 * bit0:        2 bit MAN       +---+---+----+
 */
struct Fp4e1m2T {
    uint8_t val = 0;

public:
    Fp4e1m2T(void) : val(0x0u) {}
    Fp4e1m2T(const Fp4e1m2T& fp4) : val(fp4.val) {}
    Fp4e1m2T(const bfloat16::Bf16T src) : val(BfloatToFp4e1m2(src.val)) {}
    Fp4e1m2T& operator=(const Fp4e1m2T& fp4)
    {
        if (this != &fp4) {
            this->val = fp4.val;
        }
        return *this;
    }
    uint8_t BfloatToFp4e1m2(const uint16_t src) const;
    operator bfloat16::Bf16T() const;
};
} // namespace hifloat4
using hifloat4x2_t = hifloat4::Fp4e1m2T;
using hif4x2_t = hifloat4x2_t;
#endif
