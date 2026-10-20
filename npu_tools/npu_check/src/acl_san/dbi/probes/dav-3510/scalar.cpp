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

// SET_PADDING, API ID 392.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_padding(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 392, config,
        0UL, 0UL, 0UL, 0UL);
}

// SET_MTE2_NZ_PARA, API ID 399.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_mte2_nz_para(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 399, config,
        0UL, 0UL, 0UL, 0UL);
}

// MTE2_SRC_PARA, API ID 124.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_mte2_src_para(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 124, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP0_STRIDE_NDDMA, API ID 132.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop0_stride_nddma(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 132, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP1_STRIDE_NDDMA, API ID 133.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop1_stride_nddma(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 133, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP2_STRIDE_NDDMA, API ID 134.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop2_stride_nddma(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 134, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP3_STRIDE_NDDMA, API ID 135.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop3_stride_nddma(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 135, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP4_STRIDE_NDDMA, API ID 136.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop4_stride_nddma(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 136, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_st_atomic_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset, uint64_t post)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 58, addr,
        static_cast<uint64_t>(offset), post, __cce_scalar::get_sys_va_base(), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
}

// ST_ATOMIC.b32, API ID 56. Offset is an element count; post=1 uses the captured base without adding offset.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_st_atomic_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset, uint64_t post)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 56, addr,
        static_cast<uint64_t>(offset), post, __cce_scalar::get_sys_va_base(), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
}

// ST_ATOMIC.b16, API ID 57. Offset is an element count.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_st_atomic_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset, uint64_t post)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 57, addr,
        static_cast<uint64_t>(offset), post, __cce_scalar::get_sys_va_base(), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
}

// STI_ATOMIC.b32, API ID 59. Offset is a byte count.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_sti_atomic_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset, uint64_t post)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 59, addr,
        static_cast<uint64_t>(offset), post, __cce_scalar::get_sys_va_base(), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
}

// STI_ATOMIC.b16, API ID 60. Offset is a byte count.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_sti_atomic_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset, uint64_t post)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 60, addr,
        static_cast<uint64_t>(offset), post, __cce_scalar::get_sys_va_base(), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
}

// STI_ATOMIC.b8, API ID 61. Offset is a byte count.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_sti_atomic_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset, uint64_t post)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 61, addr,
        static_cast<uint64_t>(offset), post, __cce_scalar::get_sys_va_base(), aclsan::ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
}

// ST_DEV.b64, API ID 64. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_st_dev_b64(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 64, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

// ST_DEV.b32, API ID 65. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_st_dev_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 65, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

// ST_DEV.b16, API ID 66. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_st_dev_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 66, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

// ST_DEV.b8, API ID 67. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_st_dev_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 67, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

// LD_DEV.b64, API ID 68. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_ld_dev_b64(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 68, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

// LD_DEV.b32, API ID 69. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_ld_dev_b32(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 69, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

// LD_DEV.b16, API ID 70. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_ld_dev_b16(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 70, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

// LD_DEV.b8, API ID 71. Offset is in bytes.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_ld_dev_b8(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t addr, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 71, addr,
        static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_dc_preload(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __gm__ uint64_t* address, int64_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 62,
        reinterpret_cast<uint64_t>(address), static_cast<uint64_t>(offset), 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_dc_preloadi(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, __gm__ uint64_t* address, int16_t offset)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::MemoryAccess, static_cast<uint16_t>(PIPE_S), 63,
        reinterpret_cast<uint64_t>(address), static_cast<uint64_t>(static_cast<int64_t>(offset)), 0UL, 0UL, 0UL);
}

// LOOP3_PARA, API ID 90.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop3_para(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 90, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_channel_para(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 123, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP_SIZE_UBTOOUT, API ID 125.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop_size_ubtoout(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 125, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP1_STRIDE_UBTOOUT, API ID 126.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop1_stride_ubtoout(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 126, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP2_STRIDE_UBTOOUT, API ID 127.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop2_stride_ubtoout(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 127, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP_SIZE_OUTTOUB, API ID 128.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop_size_outtoub(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 128, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP1_STRIDE_OUTTOUB, API ID 129.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop1_stride_outtoub(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 129, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP2_STRIDE_OUTTOUB, API ID 130.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop2_stride_outtoub(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 130, config,
        0UL, 0UL, 0UL, 0UL);
}

// PAD_CNT_NDDMA, API ID 131.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_pad_cnt_nddma(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 131, config,
        0UL, 0UL, 0UL, 0UL);
}

// SET_CTRL，API ID 159。捕获源操作数，用于原子配置变更和完整 CTRL 恢复。
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_ctrl(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 159, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_ffts_base_addr(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 160, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_fpc(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 161, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_quant_pre(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 162, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_quant_post(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 163, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_lrelu_alpha(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 164, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_fmatrix(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 386, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_fmatrix_b(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 387, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_l3d_rpt(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 390, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_l3d_rpt_b(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 391, config,
        0UL, 0UL, 0UL, 0UL);
}

extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_padding_b(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 393, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP_SIZE_OUTTOL1, API ID 394.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop_size_outtol1(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 394, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP1_STRIDE_OUTTOL1, API ID 395.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop1_stride_outtol1(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 395, config,
        0UL, 0UL, 0UL, 0UL);
}

// LOOP2_STRIDE_OUTTOL1, API ID 396.
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __sanitizer_report_set_loop2_stride_outtol1(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint64_t config)
{
    aclsan::WriteTraceRecord(
        memInfo, pc, bid, aclsan::DeviceInstructionCategory::RegisterState, static_cast<uint16_t>(PIPE_S), 396, config,
        0UL, 0UL, 0UL, 0UL);
}
