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

// SET/WAIT FLAG instructions.

// SET_FLAG, API ID 440.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_flag(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 440,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, 0UL, 0UL);
}

// SET_FLAGI, API ID 441.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_flagi(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 441,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, 0UL, 0UL);
}

// WAIT_FLAG, API ID 442.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flag(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 442,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, 0UL, 0UL);
}

// WAIT_FLAGI, API ID 443.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flagi(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 443,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, 0UL, 0UL);
}

// WAIT_FLAG_DEV, API ID 445.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flag_dev_pipe(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, int64_t flagId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 445,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(flagId), 0UL, 0UL, 0UL);
}

// WAIT_FLAG_DEVI, API ID 446.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flag_devi_pipe(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint8_t flagId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 446,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(flagId), 0UL, 0UL, 0UL);
}

// FFTS_CROSS_CORE_SYNC, API ID 447.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_ffts_cross_core_sync(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 447,
        static_cast<uint64_t>(pipe), config, 0UL, 0UL, 0UL);
}

// SET_FLAG_V, API ID 456.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_flag_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t dstPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 456,
        static_cast<uint64_t>(PIPE_V), static_cast<uint64_t>(dstPipe), eventId, 0UL, 0UL);
}

// SET_FLAGI_V, API ID 457.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_flagi_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t dstPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 457,
        static_cast<uint64_t>(PIPE_V), static_cast<uint64_t>(dstPipe), eventId, 0UL, 0UL);
}

// WAIT_FLAG_V, API ID 458.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flag_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 458,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(PIPE_V), eventId, 0UL, 0UL);
}

// WAIT_FLAGI_V, API ID 459.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flagi_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, uint64_t eventId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 459,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(PIPE_V), eventId, 0UL, 0UL);
}

// WAIT_FLAG_DEV_V, API ID 469.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flag_dev_pipe_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, int64_t flagId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 469,
        static_cast<uint64_t>(flagId), 0UL, 0UL, 0UL, 0UL);
}

// WAIT_FLAG_DEVI_V, API ID 470.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_flag_devi_pipe_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint8_t flagId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 470,
        static_cast<uint64_t>(flagId), 0UL, 0UL, 0UL, 0UL);
}

// HSET_FLAG, API ID 471.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_hset_flag(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId, mem_t memory,
    bool isVirtual)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 471,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, static_cast<uint64_t>(memory),
        static_cast<uint64_t>(isVirtual));
}

// HSET_FLAGI, API ID 472.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_hset_flagi(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId, mem_t memory,
    bool isVirtual)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 472,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, static_cast<uint64_t>(memory),
        static_cast<uint64_t>(isVirtual));
}

// HWAIT_FLAG, API ID 473.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_hwait_flag(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId, mem_t memory,
    bool isVirtual)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 473,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, static_cast<uint64_t>(memory),
        static_cast<uint64_t>(isVirtual));
}

// HWAIT_FLAGI, API ID 474.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_hwait_flagi(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t srcPipe, pipe_t dstPipe, uint64_t eventId, mem_t memory,
    bool isVirtual)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 474,
        static_cast<uint64_t>(srcPipe), static_cast<uint64_t>(dstPipe), eventId, static_cast<uint64_t>(memory),
        static_cast<uint64_t>(isVirtual));
}

// GET/RLS BUF instructions.

// GET_BUF, API ID 448.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_get_buf(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint8_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 448,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(bufId), static_cast<uint64_t>(mode), 0UL, 0UL);
}

// GET_BUFI, API ID 449.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_get_bufi(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint64_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 449,
        static_cast<uint64_t>(pipe), bufId, static_cast<uint64_t>(mode), 0UL, 0UL);
}

// RLS_BUF, API ID 450.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_rls_buf(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint8_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 450,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(bufId), static_cast<uint64_t>(mode), 0UL, 0UL);
}

// RLS_BUFI, API ID 451.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_rls_bufi(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint64_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 451,
        static_cast<uint64_t>(pipe), bufId, static_cast<uint64_t>(mode), 0UL, 0UL);
}

// GET_BUF_V, API ID 460.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_get_buf_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint8_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 460,
        static_cast<uint64_t>(PIPE_V), static_cast<uint64_t>(bufId), static_cast<uint64_t>(mode), 0UL, 0UL);
}

// GET_BUFI_V, API ID 461.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_get_bufi_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 461,
        static_cast<uint64_t>(PIPE_V), bufId, static_cast<uint64_t>(mode), 0UL, 0UL);
}

// RLS_BUF_V, API ID 462.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_rls_buf_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint8_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 462,
        static_cast<uint64_t>(PIPE_V), static_cast<uint64_t>(bufId), static_cast<uint64_t>(mode), 0UL, 0UL);
}

// RLS_BUFI_V, API ID 463.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_rls_bufi_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t bufId, bool mode)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 463,
        static_cast<uint64_t>(PIPE_V), bufId, static_cast<uint64_t>(mode), 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_pipe_barrier(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 439,
        static_cast<uint64_t>(pipe), 0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_intra_block(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint64_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 452,
        static_cast<uint64_t>(pipe), syncId, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_intra_blocki(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint8_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 453,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(syncId), 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_intra_block(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint64_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 454,
        static_cast<uint64_t>(pipe), syncId, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_intra_blocki(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint8_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 455,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(syncId), 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_intra_block_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint64_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 464,
        static_cast<uint64_t>(pipe), syncId, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_intra_blocki_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint8_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 465,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(syncId), 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_intra_block_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint64_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 466,
        static_cast<uint64_t>(pipe), syncId, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_wait_intra_blocki_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, pipe_t pipe, uint8_t syncId)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 467,
        static_cast<uint64_t>(pipe), static_cast<uint64_t>(syncId), 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_ffts_cross_core_sync_v(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::Synchronization, static_cast<uint16_t>(PIPE_S), 468,
        static_cast<uint64_t>(PIPE_V), config, 0UL, 0UL, 0UL);
}
