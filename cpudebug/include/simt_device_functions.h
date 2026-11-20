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
 * \file simt_device_functions.h
 * \brief SIMT device functions
 */
#ifndef ASCENDC_SIMT_DEVICE_FUNCTIONS_H
#define ASCENDC_SIMT_DEVICE_FUNCTIONS_H

#include "stub_def.h"

static inline int __ffs(int x)
{
    if (x == 0) {
        return 0;
    }
    int lsb = x & (~x + 1);
    return __builtin_ctz(lsb) + 1;
}

static inline int __clz(int x)
{
    int32_t count = 0;
    for (int i = 0; i < 32; i++) {              // 32 : int bit-length
        int32_t tmp = 1 << (32 - 1 - i);        // 32 : int bit-length
        if (((tmp & x) >> (32 - 1 - i)) == 0) { // 32 : int bit-length
            count += 1;
        } else {
            break;
        }
    }
    return count;
}

static inline unsigned long long __umul64hi(unsigned long long x, unsigned long long y)
{
    uint32_t a0 = (uint32_t)x;
    uint32_t a1 = x >> 32; // 32 : uint bit-length
    uint32_t b0 = (uint32_t)y;
    uint32_t b1 = y >> 32; // 32 : uint bit-length

    uint64_t lo0 = (uint64_t)a0 * b0;
    uint64_t m1 = (uint64_t)a0 * b1;
    uint64_t m2 = (uint64_t)a1 * b0;
    uint64_t hi1 = (uint64_t)a1 * b1;

    uint64_t mid = m1 + m2;
    uint64_t carry = mid < m1;

    uint64_t mid_hi = mid >> 32; // 32 : uint bit-length
    uint64_t mid_lo = (uint32_t)mid;

    uint64_t t = mid_lo + (lo0 >> 32); // 32 : uint bit-length

    return hi1 + mid_hi + (carry << 32) + (t >> 32); // 32 : uint bit-length
}

static inline int __popc(unsigned int x)
{
    int count = 0;
    while (x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

static inline unsigned int __byte_perm(unsigned int x, unsigned int y, unsigned int s)
{
    uint64_t combined = (static_cast<uint64_t>(y) << 32) | x; // 32 : uint bit-length
    uint32_t selector0 = s & 0x7;
    uint32_t selector1 = (s >> 4) & 0x7;
    uint32_t selector2 = (s >> 8) & 0x7;
    uint32_t selector3 = (s >> 12) & 0x7;
    uint32_t byte0 = (combined >> (selector0 * 8)) & 0xFF;
    uint32_t byte1 = (combined >> (selector1 * 8)) & 0xFF;
    uint32_t byte2 = (combined >> (selector2 * 8)) & 0xFF;
    uint32_t byte3 = (combined >> (selector3 * 8)) & 0xFF;
    // 8/16/24 : bit offsets of the 2nd/3rd/4th bytes in uint32
    return byte0 | (byte1 << 8) | (byte2 << 16) | (byte3 << 24);
}

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

#endif // ASCENDC_SIMT_DEVICE_FUNCTIONS_H
