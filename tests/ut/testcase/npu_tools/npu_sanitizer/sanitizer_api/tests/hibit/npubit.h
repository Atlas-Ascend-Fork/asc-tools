/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

// TEST FIXTURE ONLY. Cleanup/status declarations are assumed, not the official Npubit ABI.
#ifndef NPUBIT_TEST_FIXTURE_H
#define NPUBIT_TEST_FIXTURE_H
#include <cstddef>
#include <cstdint>
using NpubitHandle = void*;
using NpubitStatus = int;
enum NpubitPlatformType { UNDEFINED, V220, V100, V121 };
enum NpubitSubcoreType { NPUBIT_SUBCORE_TYPE_VEC0, NPUBIT_SUBCORE_TYPE_VEC1, NPUBIT_SUBCORE_TYPE_CUBE };
enum NpubitUbAccessType {
    NPUBIT_UB_ACCESS_VECTOR_READ,
    NPUBIT_UB_ACCESS_VECTOR_WRITE,
    NPUBIT_UB_ACCESS_SCALAR_READ,
    NPUBIT_UB_ACCESS_SCALAR_WRITE,
    NPUBIT_SMEM_BAR_VV_ALL,
    NPUBIT_SMEM_BAR_VST_VLD,
    NPUBIT_SMEM_BAR_VLD_VST,
    NPUBIT_SMEM_BAR_VST_VST,
    NPUBIT_SMEM_BAR_VS_ALL,
    NPUBIT_SMEM_BAR_VST_LD,
    NPUBIT_SMEM_BAR_VLD_ST,
    NPUBIT_SMEM_BAR_VST_ST,
    NPUBIT_SMEM_BAR_SV_ALL,
    NPUBIT_SMEM_BAR_ST_VLD,
    NPUBIT_SMEM_BAR_LD_VST,
    NPUBIT_SMEM_BAR_ST_VST
};
struct NpubitUbAddressRange {
    uint64_t pc;
    uint64_t target_address;
    uint64_t processed_bytes;
    NpubitUbAccessType access_type;
#ifndef NPUBIT_LEGACY_NO_INDEX
    uint64_t instruction_execution_number = 0; // Confirmed field name; not an official ABI layout.
#endif
};
struct NpubitPushPBStateRecord {
    uint8_t aicore_id;
    NpubitSubcoreType subcore_type;
    uint64_t vf_instr_address;
    uint64_t states[16];
    uint64_t mask[2];
};
extern "C" {
NpubitStatus NpubitCreateHandle(const void*, size_t, NpubitHandle*, NpubitPlatformType, bool);
NpubitStatus NpubitGetUbAddressRanges(NpubitHandle, const NpubitPushPBStateRecord*, NpubitUbAddressRange**, size_t*);
void NpubitReleaseHandle(NpubitHandle);
void NpubitFree(void**);
}
#endif
