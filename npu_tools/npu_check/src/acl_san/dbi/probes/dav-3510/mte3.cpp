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

// MOV_UB_TO_L1, API ID 173.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_ubuf_to_cbuf(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __cbuf__ void* dst, __ubuf__ void* src, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE3), 173,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config, 0UL, 0UL);
}

// MOV_UB_TO_OUT_ALIGN_V2, API ID 83.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_check_probe_copy_ubuf_to_gm_align_v2(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __gm__ void* dst, __ubuf__ void* src, uint64_t config0,
    uint64_t config1)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_MTE3), 83,
        reinterpret_cast<uint64_t>(dst), reinterpret_cast<uint64_t>(src), config0, config1, 0UL);
}
