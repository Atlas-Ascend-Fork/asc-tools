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

// MOV_OUT_TO_L1_ALIGN_V2.b8, API ID 74.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_gm_to_cbuf_align_v2_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 74,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_ALIGN_V2.b16, API ID 75.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_gm_to_cbuf_align_v2_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 75,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_ALIGN_V2.b32, API ID 76.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_gm_to_cbuf_align_v2_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 76,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_MULTI_DN2NZ.b8, API ID 80.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_copy_gm_to_cbuf_multi_dn2nz_d_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 80,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_MULTI_DN2NZ.b16, API ID 81.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_copy_gm_to_cbuf_multi_dn2nz_d_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 81,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_MULTI_DN2NZ.b32, API ID 82.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_copy_gm_to_cbuf_multi_dn2nz_d_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 82,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_MULTI_ND2NZ.b8, API ID 77.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_copy_gm_to_cbuf_multi_nd2nz_d_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 77,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_MULTI_ND2NZ.b16, API ID 78.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_copy_gm_to_cbuf_multi_nd2nz_d_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 78,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_MULTI_ND2NZ.b32, API ID 79.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_copy_gm_to_cbuf_multi_nd2nz_d_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 79,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_L1_V2, API ID 73.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_gm_to_cbuf_v2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 73,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_UB_ALIGN_V2.b8, API ID 84.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_gm_to_ubuf_align_v2_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 84,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_UB_ALIGN_V2.b16, API ID 85.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_gm_to_ubuf_align_v2_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 85,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_OUT_TO_UB_ALIGN_V2.b32, API ID 86.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_gm_to_ubuf_align_v2_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 86,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// LOAD_OUT_TO_L1_2DV2, API ID 72.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_gm_to_cbuf_2dv2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __gm__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 72,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// ND_DMA_OUT_TO_UB.b8, API ID 87.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_nd_copy_gm_to_ubuf_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __gm__ void* src, uint64_t config,
    uint64_t secConfig)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 87,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, secConfig, 0UL);
}

// ND_DMA_OUT_TO_UB.b16, API ID 88.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_nd_copy_gm_to_ubuf_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __gm__ void* src, uint64_t config,
    uint64_t secConfig)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 88,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, secConfig, 0UL);
}

// ND_DMA_OUT_TO_UB.b32, API ID 89.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_nd_copy_gm_to_ubuf_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __gm__ void* src, uint64_t config,
    uint64_t secConfig)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE2), 89,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, secConfig, 0UL);
}

// SET_L1_2D.b16, API ID 149.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_set_l1_2d_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_MTE2), 149,
        reinterpret_cast<uint64_t>(dst), config, 0UL, 0UL, 0UL);
}

// SET_L1_2D.b32, API ID 150.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_set_l1_2d_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_MTE2), 150,
        reinterpret_cast<uint64_t>(dst), config, 0UL, 0UL, 0UL);
}
