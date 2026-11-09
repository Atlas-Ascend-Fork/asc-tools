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

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_s8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 400,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_f16_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 401,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_bf16_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 402,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_f32_f32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 403,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_e4m3_e4m3(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 404,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_e4m3_e5m2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 405,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_e5m2_e4m3(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 406,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_e5m2_e5m2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 407,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e1m2_e1m2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 408,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e1m2_e2m1(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 409,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e2m1_e1m2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 410,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e2m1_e2m1(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 411,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e4m3_e4m3(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 412,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e4m3_e5m2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 413,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e5m2_e4m3(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 414,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_mad_mx_e5m2_e5m2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cc__ void* c, __ca__ void* a, __cb__ void* b, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_M), 415,
        reinterpret_cast<uint64_t>(c), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), config, 0UL);
}
