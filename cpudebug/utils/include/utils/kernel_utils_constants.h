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
 * \file kernel_utils_constants.h
 * \brief
 */
#ifndef ASCENDC_MODULE_UTILS_CONSTANTS_H
#define ASCENDC_MODULE_UTILS_CONSTANTS_H
#include "utils/kernel_utils_ceil_oom_que.h"

namespace AscendC {
const int32_t DEFAULT_BLK_NUM = 8;
const int32_t HALF_FACTOR = 2;
const int32_t DEFAULT_BLK_STRIDE = 1;
const uint8_t DEFAULT_REPEAT_STRIDE = 8;
const uint64_t FULL_MASK = 0xffffffffffffffff;
const uint64_t CONST_MASK_VALUE = 0x8000000000000000;
const uint16_t MAX_HALF_MASK_LEN = 64;
const int32_t DEFAULT_C0_SIZE = 32;
const int32_t DEFAULT_BLOCK_SIZE = 256;
const int32_t MAX_REPEAT_TIMES = 255;
const int32_t MIN_REPEAT_TIMES = 0;
const int32_t ONE_BYTE_BIT_SIZE = 8;
const uint32_t MAX_SLICE_SIZE = 6 * 256;
const uint16_t VALUE_512 = 512; // align with 512B / value range [0, 512]

// int4b_t param
constexpr uint32_t INT4_TWO = 2;
constexpr uint32_t INT4_BIT_NUM = 4;

#if (__NPU_ARCH__ == 5102)
// int2b_t param
constexpr uint32_t INT2_FOUR = 4;
constexpr uint32_t INT2_BIT_NUM = 2;
constexpr uint32_t INT1_BIT_NUM = 1;
#endif

#if defined(__NPU_ARCH__) && \
    ((__NPU_ARCH__ == 2103) || (__NPU_ARCH__ == 3003) || (__NPU_ARCH__ == 3103) || (__NPU_ARCH__ == 3113))
// int3b_t param
constexpr uint32_t INT3_BIT_NUM = 3;
// int2b_t param
constexpr uint32_t INT2_FOUR = 4;
constexpr uint32_t INT2_BIT_NUM = 2;
#endif

#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
namespace ConstantsInternal {
constexpr uint32_t ASCENDC_B4_TWO = 2;
constexpr uint32_t ASCENDC_B4_BIT_NUM = 4;
} // namespace ConstantsInternal
#endif

// AddDeqRelu param
constexpr int32_t DEQ_SHIFT_LEFT_17_BIT = 131072;
constexpr float DEQ_SHIFT_RIGHT_17_BIT = 1.0 / DEQ_SHIFT_LEFT_17_BIT;
constexpr int8_t ADDDEQRELU_MASK_MODE_ONE = 1;
constexpr int8_t ADDDEQRELU_MASK_MODE_TWO = 2;

#if (__NPU_ARCH__ == 5102)
const int32_t TOTAL_VEC_LOCAL_SIZE = 248 * 1024;
const uint32_t TOTAL_UB_SIZE = 248 * 1024;
const uint32_t TMP_UB_OFFSET = 248 * 1024;
const uint32_t TOTAL_L1_SIZE = 1024 * 1024;
const uint32_t SINGLE_MSG_SIZE = 64;
const uint32_t CACHE_LINE_SIZE = 64;
const uint32_t TOTAL_L0C_SIZE = 256 * 1024;
const uint32_t VECTOR_REG_WIDTH = 256;
const uint32_t VECTOR_REG_WIDTH_2XVL = 512;
const uint32_t ONE_BLOCK_SIZE = 32;
#elif defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 1001) || (__NPU_ARCH__ == 2002))
const int32_t TOTAL_VEC_LOCAL_SIZE = 248 * 1024;
const uint32_t TOTAL_UB_SIZE = 256 * 1024;

const uint32_t TMP_UB_OFFSET = 248 * 1024;
const uint32_t TOTAL_L1_SIZE = 1024 * 1024;
const uint32_t TOTAL_L0C_SIZE = 256 * 1024;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 2103)
const int32_t TOTAL_VEC_LOCAL_SIZE = 112 * 1024;
const uint32_t TOTAL_UB_SIZE = 120 * 1024;
const uint32_t TMP_UB_OFFSET = 112 * 1024;
const uint32_t TOTAL_L1_SIZE = 1024 * 1024;
const uint32_t VECTOR_REG_WIDTH = 128;
const uint32_t ONE_BLOCK_SIZE = 32;
const uint32_t TOTAL_L0C_SIZE = 64 * 1024;
#elif (__NPU_ARCH__ == 2201)
const int32_t TOTAL_VEC_LOCAL_SIZE = 184 * 1024;
const uint32_t TOTAL_UB_SIZE = 192 * 1024;
const uint32_t TMP_UB_OFFSET = 184 * 1024;
#ifndef KFC_L1_RESERVER_SIZE
#define KFC_L1_RESERVER_SIZE 128
#endif
const uint32_t TOTAL_L1_SIZE = 512 * 1024 - KFC_L1_RESERVER_SIZE;
const uint32_t SINGLE_MSG_SIZE = 64;
const uint32_t CACHE_LINE_SIZE = 64;
const uint32_t TOTAL_L0C_SIZE = 128 * 1024;
#elif (__NPU_ARCH__ == 3002)
const int32_t TOTAL_VEC_LOCAL_SIZE = 184 * 1024;
const uint32_t TOTAL_UB_SIZE = 248 * 1024;
const uint32_t TMP_UB_OFFSET = 248 * 1024;
const uint32_t TOTAL_L1_SIZE = 1024 * 1024;
const uint32_t SINGLE_MSG_SIZE = 64;
const uint32_t CACHE_LINE_SIZE = 64;
const uint32_t TOTAL_L0C_SIZE = 128 * 1024;
const uint32_t VECTOR_REG_WIDTH = 256;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3003)
const int32_t TOTAL_VEC_LOCAL_SIZE = 118 * 1024;
const uint32_t TOTAL_UB_SIZE = 118 * 1024;
const uint32_t TMP_UB_OFFSET = 118 * 1024;
const uint32_t TOTAL_L1_SIZE = 1024 * 1024;
const uint32_t VECTOR_REG_WIDTH = 256;
const uint32_t ONE_BLOCK_SIZE = 32;
const uint32_t TOTAL_L0C_SIZE = 128 * 1024;
#elif (__NPU_ARCH__ == 3102)
const int32_t TOTAL_VEC_LOCAL_SIZE = 184 * 1024;
const uint32_t TOTAL_UB_SIZE = 256 * 1024;
const uint32_t TMP_UB_OFFSET = 248 * 1024;
const uint32_t TOTAL_L1_SIZE = 1024 * 1024;
const uint32_t SINGLE_MSG_SIZE = 64;
const uint32_t CACHE_LINE_SIZE = 64;
const uint32_t TOTAL_L0C_SIZE = 128 * 1024;
const uint32_t VECTOR_REG_WIDTH = 256;
#elif (__NPU_ARCH__ == 3510)
const int32_t TOTAL_VEC_LOCAL_SIZE = 248 * 1024;
const uint32_t TOTAL_UB_SIZE = 248 * 1024;
const uint32_t TMP_UB_OFFSET = 248 * 1024;
const uint32_t TOTAL_L1_SIZE = 512 * 1024;
const uint32_t SINGLE_MSG_SIZE = 64;
const uint32_t CACHE_LINE_SIZE = 64;
const uint32_t TOTAL_L0C_SIZE = 256 * 1024;
const uint32_t VECTOR_REG_WIDTH = 256;
const uint32_t VECTOR_REG_WIDTH_2XVL = 512;
const uint32_t ONE_BLOCK_SIZE = 32;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3113)
const int32_t TOTAL_VEC_LOCAL_SIZE = 118 * 1024;
const uint32_t TOTAL_UB_SIZE = 118 * 1024;
const uint32_t TMP_UB_OFFSET = 118 * 1024;
const uint32_t TOTAL_L1_SIZE = 512 * 1024;
const uint32_t TOTAL_L0C_SIZE = 64 * 1024;
const uint32_t VECTOR_REG_WIDTH = 256;
const uint32_t ONE_BLOCK_SIZE = 32;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 5161)
const uint32_t TOTAL_UB_SIZE = 128 * 1024;
const uint32_t TOTAL_L1_SIZE = 1280 * 1024;
const uint32_t VECTOR_REG_WIDTH = 64;
const uint32_t ONE_BLOCK_SIZE = 32;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 5163)
const uint32_t TOTAL_UB_SIZE = 128 * 1024;
const uint32_t TOTAL_L1_SIZE = 1024 * 1024;
const uint32_t VECTOR_REG_WIDTH = 64;
const uint32_t ONE_BLOCK_SIZE = 32;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 5165)
const uint32_t TOTAL_UB_SIZE = 128 * 1024;
const uint32_t TOTAL_L1_SIZE = 384 * 1024;
const uint32_t VECTOR_REG_WIDTH = 32;
const uint32_t ONE_BLOCK_SIZE = 32;
#endif

#ifndef __NPU_HOST__

#ifndef ASCC_PARAM_BLOCK_CUBE
#define ASCC_PARAM_BLOCK_CUBE
const int32_t BLOCK_CUBE = 16;
#endif

#ifndef ASCC_PARAM_ONE_BLK_SIZE
#define ASCC_PARAM_ONE_BLK_SIZE
const uint16_t ONE_BLK_SIZE = 32;
#endif

#ifndef ASCC_PARAM_CUBE_MAX_SIZE
#define ASCC_PARAM_CUBE_MAX_SIZE
const int32_t CUBE_MAX_SIZE = 256;
#endif

#else // #ifdef __NPU_HOST__

#ifndef ASCC_PARAM_BLOCK_CUBE
#define ASCC_PARAM_BLOCK_CUBE
constexpr uint32_t BLOCK_CUBE = 16;
#endif

#ifndef ASCC_PARAM_ONE_BLK_SIZE
#define ASCC_PARAM_ONE_BLK_SIZE
constexpr uint32_t ONE_BLK_SIZE = 32;
#endif

#ifndef ASCC_PARAM_CUBE_MAX_SIZE
#define ASCC_PARAM_CUBE_MAX_SIZE
constexpr int32_t CUBE_MAX_SIZE = 256;
#endif

#endif // __NPU_HOST__
const uint8_t PAD_SIZE = 4;
const uint8_t MRG_SORT_ELEMENT_LEN = 4;
const uint8_t DEFAULT_DATA_COPY_NBURST = 1;
const uint8_t DEFAULT_DATA_COPY_STRIDE = 0;
const int32_t BYTE_PER_FRACTAL = 512;
const int32_t SRC_BURST_LEN_SIZE_ELE = 16;
const int32_t SRC_GAP_SIZE_BYTE = 32;
const int32_t DST_BURST_LEN_SIZE_ELE = 256;
const int32_t VREDUCE_PER_REP_OUTPUT = 2;
const uint16_t ONE_PARAM_SIZE = 8;
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
const uint16_t AIV_CORE_NUM = 72;
#else
const uint16_t AIV_CORE_NUM = 50;
#endif
const uint16_t DUMP_MSG_HEAD_SIZE = 24;
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3103)
const int32_t ONE_REPEAT_BYTE_SIZE = 128;
#else
const int32_t ONE_REPEAT_BYTE_SIZE = 256;
#endif
const int32_t FULL_MASK_LEN = 128;
const int32_t HLAF_MASK_LEN = 64;
const int32_t DEFAULT_REDUCE_DST_REP_SRIDE = 1;
const uint8_t B64_BYTE_SIZE = 8;
const uint8_t B32_BYTE_SIZE = 4;
const uint8_t B16_BYTE_SIZE = 2;
const uint8_t B8_BYTE_SIZE = 1;
const uint8_t B32_DATA_NUM_PER_BLOCK = 8;
const uint8_t B16_DATA_NUM_PER_BLOCK = 16;
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3103)
const int32_t B16_DATA_NUM_PER_REPEAT = 64;
const int32_t B32_DATA_NUM_PER_REPEAT = 32;
#else
const int32_t B16_DATA_NUM_PER_REPEAT = 128;
const int32_t B32_DATA_NUM_PER_REPEAT = 64;
#endif

#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
const uint32_t B64_DATA_NUM_PER_REPEAT = 32;
const uint32_t B4_BYTE_SIZE_PER_REPEAT = 64;
const uint32_t L1_DUMP_UB_SIZE = TOTAL_UB_SIZE - 32 * 1024;
#endif

const int32_t BLOCK_STRIDE_POS_IN_SM = 16;
const int32_t PLD_BUFFER_SIZE = 2;
const uint8_t FIXPIPE_DEQ_TENSOR_SIZE = 16;
const uint8_t SET_DATA_EXP_ZERO = 0;
const uint8_t SET_DATA_EXP_ONE = 1;
const uint8_t SET_DATA_EXP_TWO = 2;
const uint8_t SET_DATA_EXP_THREE = 3;
const uint8_t VDEQ_TENSOR_SIZE = 16;
// workspace system reserve 16MB
#if (__NPU_ARCH__ == 1001)
constexpr size_t RESERVED_WORKSPACE = 2 * 1024 * 1024;
#elif (__NPU_ARCH__ == 2002)
constexpr size_t RESERVED_WORKSPACE = 2 * 1024 * 1024;
#elif (__NPU_ARCH__ == 2201)
constexpr size_t RESERVED_WORKSPACE = 16 * 1024 * 1024;
#elif (__NPU_ARCH__ == 3002)
constexpr size_t RESERVED_WORKSPACE = 16 * 1024 * 1024;
#elif (__NPU_ARCH__ == 3102)
constexpr size_t RESERVED_WORKSPACE = 16 * 1024 * 1024;
#elif defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
constexpr size_t RESERVED_WORKSPACE = 16 * 1024 * 1024;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 2103)
constexpr size_t RESERVED_WORKSPACE = 16 * 1024 * 1024;
#elif defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3003)
constexpr size_t RESERVED_WORKSPACE = 0;
#elif defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3103) || (__NPU_ARCH__ == 3113))
constexpr size_t RESERVED_WORKSPACE = 0;
#endif

// nchwconv address list size
const int32_t NCHW_CONV_ADDR_LIST_SIZE = 16;
const int32_t VA_REG_ARRAY_LEN = 8;
const uint8_t CONV2D_IMG_SIZE = 2;
const uint8_t CONV2D_KERNEL_SIZE = 2;
const uint8_t CONV2D_STRIDE = 2;
const uint8_t CONV2D_PAD = 4;
const uint8_t CONV2D_DILATION = 2;
const int32_t K_MAX_DIM = 8;

const uint32_t TWO_OF_STACK_BUFFER = 2;
const uint32_t THREE_OF_STACK_BUFFER = 3;
const uint32_t HALF_REPEAT_SIZE = ONE_REPEAT_BYTE_SIZE / B16_BYTE_SIZE;
const uint32_t FLOAT_REPEAT_SIZE = ONE_REPEAT_BYTE_SIZE / B32_BYTE_SIZE;
const uint32_t ONE_REPEAT_FLOAT_SIZE = ONE_REPEAT_BYTE_SIZE / B32_BYTE_SIZE;
const uint32_t ONE_REPEAT_HALF_SIZE = ONE_REPEAT_BYTE_SIZE / B16_BYTE_SIZE;
const uint32_t MAX_REPEAT_FLOAT_SIZE = ONE_REPEAT_FLOAT_SIZE * MAX_REPEAT_TIMES;
const uint32_t MAX_REPEAT_HALF_SIZE = ONE_REPEAT_HALF_SIZE * MAX_REPEAT_TIMES;
const uint32_t ONE_BLK_HALF_NUM = ONE_BLK_SIZE / B16_BYTE_SIZE;
const uint32_t ONE_BLK_FLOAT_NUM = ONE_BLK_SIZE / B32_BYTE_SIZE;
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
namespace ConstantsInternal {
const uint32_t ONE_BLK_FP4_NUM = 64;
const uint32_t ONE_BLK_B2_NUM = 128;
} // namespace ConstantsInternal
#endif
const uint32_t BRCB_BROADCAST_NUMBER = 8;
const uint32_t BRCB_MAX_REPEAT_SIZE = BRCB_BROADCAST_NUMBER * MAX_REPEAT_TIMES;
const int32_t MIN_BLOCK_LEN = 1;
const uint32_t PAIR_REDUCE_REPEAT_STRIDE_LEN = 128;
const uint32_t PAIR_REDUCE_SUM_MERGES = 2;
const uint32_t TWO_HUNDRED_FIFTY_TWO_REPEAT = 252;
const uint32_t TWO_HUNDRED_FIFTY_TWO_REPEAT_BYTE_SIZE = TWO_HUNDRED_FIFTY_TWO_REPEAT * ONE_REPEAT_BYTE_SIZE;
const uint32_t REDUCEV2_MODE_SEVEN = 7;
const uint32_t DROPOUT_MODE_BYTE_MISALIGN = 1;
const uint32_t DROPOUT_MODE_BYTE_ALIGN = 2;
const uint32_t DROPOUT_MODE_BIT_ALIGN = 3;
const uint32_t DROPOUT_MODE_BIT_MISALIGN = 4;
const uint32_t REDUCEV2_MODE_ONE = 1;
const uint32_t REDUCEV2_MODE_TWO = 2;
const uint32_t REDUCEV2_MODE_THREE = 3;

// 4dTrans param size
const int32_t B8_TMP_ELE_LEN = 1024;
const int32_t B16_TMP_ELE_LEN = 256;
const int32_t B32_TMP_ELE_LEN = 128;
const int32_t B8_TRANS_LEN = 1024;
const int32_t B8_TRANS_FRACTAL = 512;
const int32_t B8_TRANS_ROW = 32;
const int32_t B8_COPY_COL = 32;

// load3dPro config
const uint64_t LOAD_M_START_POSITION = 48;
const uint64_t LOAD_K_START_POSITION = 32;
const uint64_t LOAD_M_EXTENSION = 16;
const uint64_t LOAD_DILATION_FILTER_H = 40;
const uint64_t LOAD_DILATION_FILTER_W = 32;
const uint64_t LOAD_FILTER_H = 24;
const uint64_t LOAD_FILTER_W = 16;
const uint64_t LOAD_STRIDE_H = 8;

#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
namespace Internal {
constexpr int32_t TSCM_CROSS_SYNC_ID_MAX = 11;
}
#endif

#if defined(ASCENDC_CPU_DEBUG) && ASCENDC_CPU_DEBUG == 1
// param check size
const int32_t MAX_BLOCK_COUNT = 4095;
const int32_t MIN_BLOCK_COUNT = 1;
const int32_t MAX_BLOCK_LEN = 65535;

const int32_t MAX_16BITS_STRIDE = 65535;
const int32_t MAX_8BITS_STRIDE = 255;
const int32_t MIN_BLOCK_NUM = 1;
const int32_t MAX_PROPOSAL_MODE_NUM = 5;
const int32_t MIN_PROPOSAL_MODE_NUM = 0;

// load2d param size
const int32_t MAX_LOAD2D_START_INDEX = 65535;
const int32_t MIN_LOAD2D_START_INDEX = 0;
const int32_t MAX_LOAD2D_SID = 15;
const int32_t MIN_LOAD2D_SID = 0;

// load3dv1 param size
const int32_t MAX_LOAD3D_PAD = 255;
const int32_t MIN_LOAD3D_PAD = 0;
const int32_t MAX_LOAD3D_L1 = 32767;
const int32_t MIN_LOAD3D_L1 = 1;
const int32_t MAX_LOAD3D_C1_IDX = 4095;
const int32_t MIN_LOAD3D_C1_IDX = 0;
const int32_t MAX_LOAD3D_LEFT_TOP = 32767;
const int32_t MIN_LOAD3D_LEFT_TOP = -255;
const int32_t MAX_LOAD3D_STRIDE = 63;
const int32_t MIN_LOAD3D_STRIDE = 1;
const int32_t MAX_LOAD3D_FILTER = 255;
const int32_t MIN_LOAD3D_FILTER = 1;
const int32_t MIN_LOAD3D_FETCH_FILTER = 0;
const int32_t MAX_LOAD3D_FETCH_FILTER = 254;
const int32_t MIN_LOAD3D_DILATION_FILTER = 1;
const int32_t MAX_LOAD3D_JUMP_STRIDE = 127;
const int32_t MIN_LOAD3D_JUMP_STRIDE = 1;
const int32_t MAX_LOAD3D_REPEAT_MODE = 1;
const int32_t MIN_LOAD3D_REPEAT_MODE = 0;
const int32_t MIN_LOAD3D_REPEAT_TIMES = 1;
const int32_t MAX_LOAD3D_CSIZE = 1;
const int32_t MIN_LOAD3D_CSIZE = 0;

// load3dv2 param size
const int32_t MAX_LOAD3D_CHANNEL_SIZE = 65535;
const int32_t MIN_LOAD3D_CHANNEL_SIZE = 1;
const int32_t MAX_LOAD3D_EXTENSION = 65535;
const int32_t MIN_LOAD3D_EXTENSION = 1;
const int32_t MAX_LOAD3D_START_PT = 65535;
const int32_t MIN_LOAD3D_START_PT = 0;
const int32_t KEXTENSION_HALF = 16;
const int32_t MEXTENSION_HALF = 16;
const int32_t KSTARTPT_HALF = 16;
const int32_t MSTARTPT_HALF = 16;
const int32_t KEXTENSION_B8 = 32;
const int32_t MEXTENSION_B8 = 16;
const int32_t KSTARTPT_B8 = 32;
const int32_t MSTARTPT_B8 = 16;

// loadImageToLocal param size
constexpr int32_t MAX_LOADIMANG_L1_HORSIZE = 4095;
constexpr int32_t MIN_LOADIMANG_L1_HORSIZE = 1;
constexpr int32_t MAX_LOADIMANG_L1_VERSIZE = 4095;
constexpr int32_t MIN_LOADIMANG_L1_VERSIZE = 0;
constexpr int32_t MAX_LOADIMANG_L1_HWSTART = 4095;
constexpr int32_t MIN_LOADIMANG_L1_HWSTART = 0;
constexpr int32_t MAX_LOADIMANG_L1_SHORRES = 65535;
constexpr int32_t MIN_LOADIMANG_L1_SHORRES = 1;
constexpr int32_t MIN_LOADIMANG_L1_PADSIZE = 0;

// mmad param size
const int32_t MAX_M_K_N_SIZE = 4095;
const int32_t MIN_M_K_N_SIZE = 0;

// mrgsort4 param size
const int32_t MAX_SORT_ELE_LEN = 4095;
const int32_t MIN_SORT_ELE_LEN = 0;
const int32_t MIN_SORT_REPEAT_TIMES = 1;
namespace Internal {
constexpr uint8_t REGION_PROPOSAL_ELEMENT_NUM = 8;
}

template <typename T>
std::string ScalarToString(T scalarValue);
template <>
inline std::string ScalarToString(half scalarValue)
{
    return std::to_string(scalarValue.ToFloat());
}
#if defined(__NPU_ARCH__) && \
    ((__NPU_ARCH__ == 2201) || (__NPU_ARCH__ == 3002) || (__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
template <>
inline std::string ScalarToString(bfloat16_t scalarValue)
{
    return std::to_string(scalarValue.ToFloat());
}
#endif
template <typename T>
uint64_t GetScalarBitcode(T scalarValue);
// deq tensor ptr could not be passed by cce instructions, so pass ptr to model by this function
void SetModelDeqTensor(void* deqTensor);
#if __NPU_ARCH__ == 3002
void SetEleSrcPara(uint64_t baseAddr);
#endif
#if __NPU_ARCH__ == 2002
void SetVbiSrc0Param(half* vbiSrc0Ptr, int32_t vbiSrc0Size);
void SetUnzipCompressedLen(uint32_t compressedLength);
#endif
void SetModelBiasTensor(void* biasTensor);
void SetIndexMatrix(void* indexMatrix);

// src0 of gatherb instr could not be accessed by cce instructions, so pass ptr to model by this function
void SetModelGatherbSrc0Tensor(uint64_t src0, const uint32_t length);

// dst0 of scatter instr could not be accessed by cce instructions, so pass ptr to model by this function
void SetModelScatterDst0Tensor(uint64_t dst0, const uint32_t length);

#endif // ASCENDC_CPU_DEBUG

#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
template <typename T>
struct GetPadValueType {
    using Type = T;
};

// To support FP8 datacopypad, pad type needs transfer to b8
template <>
struct GetPadValueType<fp8_e5m2_t> {
    using Type = uint8_t;
};

template <>
struct GetPadValueType<fp8_e4m3fn_t> {
    using Type = uint8_t;
};

template <>
struct GetPadValueType<fp8_e8m0_t> {
    using Type = uint8_t;
};

template <>
struct GetPadValueType<hifloat8_t> {
    using Type = uint8_t;
};

// To support FP4 datacopypad, pad type needs transfer to b8
template <>
struct GetPadValueType<fp4x2_e1m2_t> {
    using Type = uint8_t;
};

template <>
struct GetPadValueType<fp4x2_e2m1_t> {
    using Type = uint8_t;
};
#endif

template <bool condition, class T1, class T2>
struct Conditional {
    using type = T1;
};

template <class T1, class T2>
struct Conditional<false, T1, T2> {
    using type = T2;
};

template <int bitNum, bool sign = true>
struct IntegerSubType {
    static int const kBits = bitNum;
    static bool const kSigned = sign;

    using T = typename Conditional<kSigned, int8_t, uint8_t>::type;
    using Storage = uint8_t;

    static Storage const mask = Storage(((static_cast<uint64_t>(1)) << static_cast<uint32_t>(kBits)) - 1);
    Storage storage;
    __aicore__ inline IntegerSubType() = default;

    __aicore__ inline IntegerSubType(uint32_t value) : storage(reinterpret_cast<Storage const&>(value) & mask) {}

    __aicore__ inline IntegerSubType(int32_t value) : storage(reinterpret_cast<Storage const&>(value) & mask) {}

    __aicore__ inline operator T() const
    {
        if (kSigned && ((storage & Storage(static_cast<uint64_t>(1) << static_cast<uint32_t>(kBits - 1))) != 0)) {
            // Sign extend
            return T(storage) | ~T(mask);
        }
        return T(storage);
    }

    __aicore__ inline bool operator==(IntegerSubType const& rhs) const { return storage == rhs.storage; }

    __aicore__ inline bool operator!=(IntegerSubType const& rhs) const { return storage != rhs.storage; }

    __aicore__ inline bool operator>(IntegerSubType const& rhs) const
    {
        bool lhsIsNeg = (this->storage & (static_cast<uint64_t>(1) << static_cast<uint32_t>(this->kBits - 1)));
        bool rhsIsNeg = (rhs.storage & (static_cast<uint64_t>(1) << static_cast<uint32_t>(rhs.kBits - 1)));
        if (kSigned && (lhsIsNeg != rhsIsNeg)) {
            return (!lhsIsNeg) && rhsIsNeg;
        }
        return this->storage > rhs.storage;
    }

    __aicore__ inline bool operator>=(IntegerSubType const& rhs) const
    {
        bool lhsIsNeg = (this->storage & (static_cast<uint64_t>(1) << static_cast<uint32_t>(this->kBits - 1)));
        bool rhsIsNeg = (rhs.storage & (static_cast<uint64_t>(1) << static_cast<uint32_t>(rhs.kBits - 1)));
        if (kSigned && (lhsIsNeg != rhsIsNeg)) {
            return (!lhsIsNeg) && rhsIsNeg;
        }
        return storage >= rhs.storage;
    }

    __aicore__ inline bool operator<(IntegerSubType const& rhs) const { return !(*this >= rhs); }

    __aicore__ inline bool operator<=(IntegerSubType const& rhs) const { return !(*this > rhs); }
};

using int4b_t = IntegerSubType<INT4_BIT_NUM, true>;

#if (__NPU_ARCH__ == 5102)
using int2b_t = IntegerSubType<INT2_BIT_NUM, true>;
#endif

#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
using mx_fp8_e5m2_t = struct {};
using mx_fp8_e4m3_t = struct {};
using mx_fp8_e8m0_t = struct {};

template <typename T>
struct GetDstType {
    using Type = T;
};

template <>
struct GetDstType<mx_fp8_e5m2_t> {
    using Type = fp8_e5m2_t;
};

template <>
struct GetDstType<mx_fp8_e4m3_t> {
    using Type = fp8_e4m3fn_t;
};

template <>
struct GetDstType<mx_fp8_e8m0_t> {
    using Type = fp8_e8m0_t;
};
#endif

} // namespace AscendC
#endif // ASCENDC_MODULE_UTILS_CONSTANTS_H
