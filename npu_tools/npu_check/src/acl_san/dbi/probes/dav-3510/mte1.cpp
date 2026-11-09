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

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_ca_mx_2dv2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 151, dst,
        reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_cb_mx_2dv2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 152, dst,
        reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// MOV_L1_TO_BT.bf16, API ID 426.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_cbuf_to_bt_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, __cbuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 426, dst,
        reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

// MOV_L1_TO_BT.f16, API ID 425.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_cbuf_to_bt_f16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, __cbuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 425, dst,
        reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

// MOV_L1_TO_BT.s32, API ID 424.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_cbuf_to_bt_s32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, __cbuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 424, dst,
        reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

// MOV_L1_TO_BT.f32, API ID 423.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_cbuf_to_bt_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, __cbuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 423, dst,
        reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

// MOV_L1_TO_UB, API ID 158.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_cbuf_to_ubuf(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __cbuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 158,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

// LOAD_L1_TO_L0A_3DV2.b8, API ID 154.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_img2colv2_cbuf_to_ca_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ca__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 154,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// LOAD_L1_TO_L0A_3DV2.b16, API ID 153.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_img2colv2_cbuf_to_ca_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ca__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 153,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// LOAD_L1_TO_L0A_3DV2.b32, API ID 422.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_img2colv2_cbuf_to_ca_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ca__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 422,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// LOAD_L1_TO_L0B_3DV2.b8, API ID 156.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_img2colv2_cbuf_to_cb_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 156,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// LOAD_L1_TO_L0B_3DV2.b16, API ID 155.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_img2colv2_cbuf_to_cb_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 155,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// LOAD_L1_TO_L0B_3DV2.b32, API ID 157.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_img2colv2_cbuf_to_cb_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 157,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}

// LOAD_L1_TO_L0A_2DV2.b8, API ID 143.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_ca_2dv2_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ca__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 143,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0A_2DV2.b16, API ID 142.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_ca_2dv2_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ca__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 142,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0A_2DV2.b32, API ID 144.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_ca_2dv2_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ca__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 144,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0A_2DV2.b4, API ID 141.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_ca_2dv2_b4(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ca__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 141,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0B_2DV2.b4, API ID 146.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_cb_2dv2_b4(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 146,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0B_2DV2.b8, API ID 147.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_cb_2dv2_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 147,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0B_2DV2.b16, API ID 145.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_cb_2dv2_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 145,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0B_2DV2.b32, API ID 148.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_load_cbuf_to_cb_2dv2_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config0,
    uint64_t config1, uint64_t transpose)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 148,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, transpose);
}

// LOAD_L1_TO_L0B_2D_TRANSPOSE.b4, API ID 140.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_load_cbuf_to_cb_2d_transpose_b4(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config,
    uint64_t fracStride)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 140,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, fracStride, 0UL);
}

// LOAD_L1_TO_L0B_2D_TRANSPOSE.b8, API ID 137.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_load_cbuf_to_cb_2d_transpose_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config,
    uint64_t fracStride)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 137,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, fracStride, 0UL);
}

// LOAD_L1_TO_L0B_2D_TRANSPOSE.b16, API ID 138.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_load_cbuf_to_cb_2d_transpose_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config,
    uint64_t fracStride)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 138,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, fracStride, 0UL);
}

// LOAD_L1_TO_L0B_2D_TRANSPOSE.b32, API ID 139.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void
__npu_check_probe_load_cbuf_to_cb_2d_transpose_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cb__ void* dst, __cbuf__ void* src, uint64_t config,
    uint64_t fracStride)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE1), 139,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, fracStride, 0UL);
}
