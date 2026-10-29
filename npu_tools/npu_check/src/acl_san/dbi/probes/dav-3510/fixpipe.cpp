/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "trace_record.h"

// MOV_L1_TO_FB, API ID 167.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_copy_cbuf_to_fbuf(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __fbuf__ void* dst, __cbuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_FIX), 167,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

// FIX_L0C_TO_L1.f32, API ID 168.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_copy_matrix_cc_to_cbuf_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __cc__ float* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_FIX), 168,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// FIX_L0C_TO_L1.s32, API ID 169.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_copy_matrix_cc_to_cbuf_s32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __cc__ int32_t* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_FIX), 169,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// FIX_L0C_TO_OUT.f32, API ID 91.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_copy_matrix_cc_to_gm_f32_a5(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __gm__ void* dst, __cc__ float* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_FIX), 91,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// FIX_L0C_TO_OUT.s32, API ID 92.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_copy_matrix_cc_to_gm_s32_a5(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __gm__ void* dst, __cc__ int32_t* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_FIX), 92,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// FIX_L0C_TO_UB.f32, API ID 170.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_copy_matrix_cc_to_ubuf_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __cc__ float* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_FIX), 170,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// FIX_L0C_TO_UB.s32, API ID 171.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_copy_matrix_cc_to_ubuf_s32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __cc__ int32_t* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_FIX), 171,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}
