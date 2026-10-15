/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file stub_def.h
 * \brief
 */
#ifndef ASCENDC_STUB_DEF_H
#define ASCENDC_STUB_DEF_H
#include <iostream>
#include <array>
#include <map>
#include <vector>
#include <string>
#include <thread>
#include <cstdio>
#include <cassert>
#include "kernel_fp16.h"
#include "kernel_bf16.h"
#include "kernel_vectorized.h"
#include "kernel_fp8_e4m3.h"
#include "kernel_fp8_e5m2.h"
#include "kernel_fp8_e8m0.h"
#include "kernel_fp8_e6m2.h"
#include "kernel_fp4_e2m1.h"
#include "kernel_fp4_e1m2.h"
#include "kernel_hif8.h"
#include "kernel_hif4.h"
#include "kernel_print_lock.h"
#include "kernel_raise_signal.h"

#ifndef ASCENDC_FLOAT4_X2_TYPEDEF
#define ASCENDC_FLOAT4_X2_TYPEDEF
using float4_e1m2x2_t = fp4x2_e1m2_t;
using float4_e1m1x2_t = fp4x2_e2m1_t;
#endif

#define __host__
#define __global__
#define __WORKGROUP_LOCAL__
#define __BLOCK_LOCAL__
#define __VEC_SCOPE__
#define __cube__
#define __vector__
#define __mix__(...)
#define __aicore__
#define __host_aicore__
#define __gm__
#define __cbuf__
#define __ubuf__
#define __cc__
#define __ca__
#define __cb__
#define __biasbuf__
#define __fbuf__
#define __sync_noalias__
#define __sync_alias__
#define __check_sync_alias__
#define __sync_out__
#define __sync_in__
#define __inout_pipe__(...)
#define __in_pipe__(...)
#define __out_pipe__(...)
#define __ssbuf__
#define __simt_callee__
#define __simt_vf__
#define __simd_callee__
#define __simd_vf__
#define __no_simd_vf_fusion__
#define __disable_kernel_type_autoinfer__
#define __kfc_workspace__
#define LAUNCH_BOUND(x)
#define ASCENDC_HOST_AICORE
#define __vector__
#define __callee__

namespace ConstantsInternal {
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3103)
constexpr uint16_t b4EleSize = 256;
constexpr uint16_t b8EleSize = 128;
constexpr uint16_t b16EleSize = 64;
constexpr uint16_t b32EleSize = 32;
constexpr uint16_t b64EleSize = 32;
#else
constexpr uint16_t b4EleSize = 512;
constexpr uint16_t b8EleSize = 256;
constexpr uint16_t b16EleSize = 128;
constexpr uint16_t b32EleSize = 64;
constexpr uint16_t b64EleSize = 32;
#endif
} // namespace ConstantsInternal

// kirin symbol dependency
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3003) || (__NPU_ARCH__ == 3113))
typedef float float32_t;
typedef half float16_t;
#endif

using vector_u8 = std::array<uint8_t, ConstantsInternal::b8EleSize>;
using vector_u16 = std::array<uint16_t, ConstantsInternal::b16EleSize>;
using vector_u32 = std::array<uint32_t, ConstantsInternal::b32EleSize>;
using vector_s8 = std::array<int8_t, ConstantsInternal::b8EleSize>;
using vector_s16 = std::array<int16_t, ConstantsInternal::b16EleSize>;
using vector_s32 = std::array<int32_t, ConstantsInternal::b32EleSize>;
using vector_s64 = std::array<int64_t, ConstantsInternal::b64EleSize>;
using vector_u64 = std::array<uint64_t, ConstantsInternal::b64EleSize>;
using vector_2xvl_s64 = std::array<int64_t, ConstantsInternal::b64EleSize>;
using vector_2xvl_u64 = std::array<uint64_t, ConstantsInternal::b64EleSize>;
using vector_bf16 = std::array<bfloat16_t, ConstantsInternal::b16EleSize>;
using vector_f16 = std::array<half, ConstantsInternal::b16EleSize>;
using vector_f32 = std::array<float, ConstantsInternal::b32EleSize>;
using vector_f8e5m2 = std::array<fp8_e5m2_t, ConstantsInternal::b8EleSize>;
using vector_f8e4m3 = std::array<fp8_e4m3fn_t, ConstantsInternal::b8EleSize>;
using vector_f8e8m0 = std::array<fp8_e8m0_t, ConstantsInternal::b8EleSize>;
using vector_f8e6m2 = std::array<fp8_e6m2_t, ConstantsInternal::b8EleSize>;
using vector_hif8 = std::array<hifloat8_t, ConstantsInternal::b8EleSize>;
using vector_hif4x2 = std::array<hifloat4x2_t, ConstantsInternal::b8EleSize>;
using vector_f4e2m1x2 = std::array<fp4x2_e2m1_t, ConstantsInternal::b8EleSize>;
using vector_f4e1m2x2 = std::array<fp4x2_e1m2_t, ConstantsInternal::b8EleSize>;

#ifndef INT4X2_T_STRUCT
#define INT4X2_T_STRUCT
struct int4x2_t {
    uint8_t data;

    const static uint16_t BIT_NUM = 4u;

    // fix atomic compile problem
    int4x2_t operator+(const int4x2_t& other) const
    {
        int4x2_t tmp;
        tmp.data = ((((data >> BIT_NUM) + (other.data >> BIT_NUM)) & 0xfu) << BIT_NUM) + ((data + other.data) & 0xfu);
        return tmp;
    }
};

#endif

using vector_s4x2 = std::array<int4x2_t, ConstantsInternal::b8EleSize>;

using vector_bool = uint16_t; // preg index.
struct vector_address {
    __aicore__ inline vector_address() {}
    explicit __aicore__ inline vector_address(int64_t) {}
    int64_t value = 0;
    __aicore__ inline operator int64_t&() { return value; }
};

using vector_align = std::array<int8_t, 32>;

#define Mode_Zeroing_Type Literal

#define ARG_STEP 0x1000000000000000

extern int64_t block_idx;
extern int64_t block_num;
extern int64_t g_ubBase;
extern uint64_t g_tilingKey;
extern int32_t g_matmulCount;
extern int32_t g_coreType;
extern int32_t g_taskRation;
extern uint32_t g_threadDimX;
extern uint32_t g_threadDimY;
extern uint32_t g_threadDimZ;
extern thread_local uint32_t g_threadIdxX;
extern thread_local uint32_t g_threadIdxY;
extern thread_local uint32_t g_threadIdxZ;
std::string& GetStrCoreType();
extern int32_t sub_block_idx;
extern uint64_t* g_workspaceSharedPtr;
extern uint64_t g_fullSizeOfWorkspace;
extern uint64_t g_fixpipeNdNzParam;

enum class KernelMode {
    MIX_MODE = 0,
    AIC_MODE,
    AIV_MODE,
    MIX_AIC_1_1,
};
enum class SocVersion { VER_100 = 100, VER_200 = 200, VER_220 = 220, VER_310 = 310, VER_510 = 510, VER_MAX = 0xFFFFFF };

using ArgInfoT = struct ArgInfoT {  // parameter info
    std::string argType;            // tensor, tensorlist, workspace, tiling
    std::vector<uint8_t*> addrList; // addr list
};

extern KernelMode g_kernelMode;
extern SocVersion g_socVersion;
std::vector<ArgInfoT>& GetArgInfoList();
std::vector<std::string>& GetValidArgTypeList();
std::vector<std::string>& GetTmpFileName();
std::vector<int32_t>& GetProcessId();
extern int32_t g_mainPid;
extern int32_t g_processNum;

namespace AscendC {
extern const int MIX_TYPE;
extern const int AIC_TYPE;
extern const int AIV_TYPE;
extern const int PAGE_SIZE;
extern const uint64_t ONE_GIGABYTE;
extern bool g_isVdeq;
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
constexpr int32_t FLAG_NUM = 32;
#else
constexpr int32_t FLAG_NUM = 16;
#endif
constexpr int32_t MAX_CORE_NUM_V220 = 25;
constexpr int32_t MAX_CORE_NUM_V310 = 36;
constexpr int32_t MIX_IN_GROUP_CORE_NUM = 3;
constexpr int32_t AIV_IN_GROUP_CORE_NUM = 2;
constexpr uint64_t FFTS_MODE_BITS = 3ull;
constexpr uint64_t FFTS_FLAG_BITS = 15ull;
constexpr uint64_t FFTS_FLAG_CONFIG_BIT_POSITION = 8ull;
constexpr uint64_t FFTS_MODE_CONFIG_BIT_POSITION = 4ull;
constexpr int32_t INTER_BLOCK_MODE = 0;
constexpr int32_t INTER_SUBBLOCK_MODE = 1;
constexpr int32_t INTRA_GROUP_MODE = 2;
constexpr int32_t FFTS_THRESHOLD = 16;
constexpr int32_t FFTS_COUNTER_NUM = 2;

extern uint8_t g_fftsGlobalLock;
extern uint8_t (*g_syncCounterEachcore)[FLAG_NUM];
extern uint8_t (*g_syncCounterFfts)[FLAG_NUM];

void AddNameArg(const char* name, unsigned long val);
unsigned long GetNameArg(const char* name);
std::string BuildExp(uint64_t val);

inline void InitSocVersion()
{
#if (defined(__NPU_ARCH__) && (__NPU_ARCH__ == 1001))
    g_socVersion = SocVersion::VER_100;
#elif (defined(__NPU_ARCH__) && (__NPU_ARCH__ == 2002))
    g_socVersion = SocVersion::VER_200;
#elif (defined(__NPU_ARCH__) && (__NPU_ARCH__ == 2201))
    g_socVersion = SocVersion::VER_220;
#elif (defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3102))
    g_socVersion = SocVersion::VER_310;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 5102)
    g_socVersion = SocVersion::VER_510;
#else
    g_socVersion = SocVersion::VER_MAX;
#endif
}

inline constexpr int32_t GetMaxCoreNum()
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510)
    return MAX_CORE_NUM_V310;
#else
    return MAX_CORE_NUM_V220;
#endif
}

bool FileExit(std::string fileName);
std::string GetFileName();
std::string GetCoreName(int idx);
void* GmAlloc(size_t size);
void GmFree(void* ptr);
void CheckGmValied(int argn, uint64_t* argv);
uint64_t GmGetUserSize(uint64_t addr);
void SetGCoreType(int type);
void SetArgInfoList(const std::vector<ArgInfoT>& argInfoList);
void SetKernelMode(KernelMode mode);
void CheckBlockdimForFfts(uint64_t numBlocks);
void CheckNumBlocksForFfts(uint64_t numBlocks);
void BacktracePrint(int sig);
void CheckSyncState();
typedef struct {
    uint64_t magicCode;
    int fd;
    size_t size;
    char fileName[256];
} ShmMemT;

} // namespace AscendC
#endif // ASCENDC_STUB_DEF_H
