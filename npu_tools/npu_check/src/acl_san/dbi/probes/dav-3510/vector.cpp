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

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_ubuf_to_ubuf(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __ubuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 174,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_scatter_vnchwconv_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, uint64_t src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 175, dst, src,
        config, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_scatter_vnchwconv_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, uint64_t src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 176, dst, src,
        config, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_scatter_vnchwconv_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, uint64_t src, uint64_t config,
    uint64_t dstHighHalf, uint64_t srcHighHalf)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 177, dst, src,
        config, dstHighHalf, srcHighHalf);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_vtranspose(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __ubuf__ void* src)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 178,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_ldva(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t dst, uint64_t src, uint64_t h)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 417, dst, src,
        h, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_vbs32_f16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __ubuf__ void* src0, __ubuf__ void* src1,
    uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 418,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src0), reinterpret_cast<uint64_t>(src1), config,
        0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_vbs32_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __ubuf__ void* src0, __ubuf__ void* src1,
    uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 419,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src0), reinterpret_cast<uint64_t>(src1), config,
        0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_vmrgsort4_f16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __ubuf__ void* src, uint64_t xm, uint64_t xt)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 420,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), xm, xt, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_vmrgsort4_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __ubuf__ void* dst, __ubuf__ void* src, uint64_t xm, uint64_t xt)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_V), 421,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), xm, xt, 0UL);
}
