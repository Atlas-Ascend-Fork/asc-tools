/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_NEXT_ITER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_NEXT_ITER_H

#include <cstdint>

namespace aclsan {

// asc_copy_ub2l1_impl
// 对应 CCE 指令：MOV_UB_TO_L1
struct CopyUbufToCbufParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint8_t sid = 0;
    uint16_t burstNum = 0;
    uint16_t burstLen = 0;
    uint16_t srcGap = 0;
    uint16_t dstGap = 0;
};

// asc_copy_l12bt_impl
// 对应 CCE 指令：MOV_L1_TO_BT.<bf16/f16/s32/f32>
struct CopyCbufToBtParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint16_t conversionControl = 0;
    uint16_t burstNum = 0;
    uint16_t burstLen = 0;
    uint16_t srcGap = 0;
    uint16_t dstGap = 0;
};

enum class LocalMemoryTransferKind : uint32_t {
    CopyCbufToFbuf,
    FixL0cToCbufF32,
    FixL0cToCbufS32,
    FixL0cToUbufF32,
    FixL0cToUbufS32,
    CopyUbufToCbuf,
    CopyCbufToUbuf,
    CopyCbufToBt,
};

// These CCE intrinsics transfer data only between on-chip memory spaces:
// MOV_L1_TO_FB/BT/UB, FIX_L0C_TO_L1/UB.<f32/s32>, and MOV_UB_TO_L1.
struct LocalMemoryTransferParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint64_t config0 = 0;
    uint64_t config1 = 0;
    LocalMemoryTransferKind kind = LocalMemoryTransferKind::CopyCbufToFbuf;
    // Decoded MOV_L1_TO_FB/BT fields; raw configs remain available for other transfers.
    uint16_t nBurst = 0;
    uint16_t lenBurst = 0;   // FB: 64-byte units; BT: 32-byte units.
    uint16_t srcStride = 0;  // FB: absolute source stride in 32-byte units.
    uint16_t dstStride = 0;  // FB: destination stride in 64-byte units.
    uint8_t dstMemBlock = 0; // FB: destination block selector.
    uint16_t srcGap = 0;     // BT: source gap in 32-byte units.
    uint16_t dstGap = 0;     // BT: destination gap in 32-byte units.
    bool cvtEnable = false;  // BT: conversion control.
    bool subBlockId = false; // L1->UB config0[0].
    uint8_t dualDstCtl = 0;  // L0C->UB config1[16:17].
};

// 对应 CCE 指令：MOV_L1_TO_FB_V2
struct CopyCbufToFbufV2ParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
};

// asc_copy_l12ub_impl
// 对应 CCE 指令：MOV_L1_TO_UB
struct CopyCbufToUbufParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    bool subBlockId = false;
    uint16_t burstNum = 0;
    uint16_t burstLen = 0;
    uint16_t srcGap = 0;
    uint16_t dstGap = 0;
};

// asc_copy_l12l0a_impl / asc_copy_l12l0b_impl
// 对应 CCE 指令：
// - LOAD_L1_TO_L0A_3DV2.<b8/b16/b32>
// - LOAD_L1_TO_L0B_3DV2.<b8/b16/b32>
struct Img2ColParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint16_t kExtension = 0;
    uint16_t mExtension = 0;
    uint16_t kStartPoint = 0;
    uint16_t mStartPoint = 0;
    uint8_t strideWidth = 0;
    uint8_t strideHeight = 0;
    uint8_t filterWidth = 0;
    uint8_t filterHeight = 0;
    uint8_t dilationFilterWidth = 0;
    uint8_t dilationFilterHeight = 0;
    bool filterSizeWidth = false;
    bool filterSizeHeight = false;
    bool transpose = false;
    bool fMatrixControl = false;
    uint16_t channelSize = 0;
};

using Img2ColCbufToCaParamField = Img2ColParamField;
using Img2ColCbufToCbParamField = Img2ColParamField;

// asc_copy_l12l0a_impl / asc_copy_l12l0b_impl 2D overload
// 对应 CCE 指令：
// - LOAD_L1_TO_L0A_2DV2.<b4/b8/b16/b32>
// - LOAD_L1_TO_L0B_2DV2.<b4/b8/b16/b32>
struct LoadCbufToL0ParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint16_t mStartPosition = 0;
    uint16_t kStartPosition = 0;
    uint8_t mStep = 0;
    uint8_t kStep = 0;
    uint16_t srcStride = 0;
    uint16_t dstStride = 0;
    bool transpose = false;
};

using LoadCbufToCa2DV2ParamField = LoadCbufToL0ParamField;
using LoadCbufToCb2DV2ParamField = LoadCbufToL0ParamField;

// asc_copy_l12l0b_sparse_impl
// 对应 CCE 指令：LOAD_L1_TO_L0B_2D_SP.<b8/b16/b32>
struct LoadCbufToCb2DSpParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint16_t startIndex = 0;
    uint8_t repeat = 0;
};

// asc_copy_l12l0b_trans_impl
// 对应 CCE 指令：LOAD_L1_TO_L0B_2D_TRANSPOSE.<b4/b8/b16/b32>
struct LoadCbufToCbTransposeParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint16_t indexId = 0;
    uint8_t repeat = 0;
    uint16_t srcStride = 0;
    uint16_t dstGap = 0;
    uint16_t dstFracGap = 0;
    uint16_t srcFracGap = 0;
    bool decrement = false; // config0 [63]
};

// 对应 CCE 指令：FIX_L0C_TO_L1.<f32/s32>
struct FixL0cToL1ParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
};

// 对应 CCE 指令：FIX_L0C_TO_UB.<f32/s32>
struct FixL0cToUbufParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_NEXT_ITER_H
