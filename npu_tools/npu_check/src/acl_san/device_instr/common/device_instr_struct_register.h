/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_REGISTER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_REGISTER_H

#include <array>
#include <cstdint>

namespace aclsan {

// SET_FMATRIX / SET_FMATRIX_B. Bit ranges refer to the raw config operand.
struct FmatrixParamField {
    uint32_t instrId = 0;
    uint16_t width = 0;        // config [15:0], pixels
    uint16_t height = 0;       // config [31:16], pixels
    uint8_t paddingLeft = 0;   // config [39:32], pixels
    uint8_t paddingRight = 0;  // config [47:40], pixels
    uint8_t paddingTop = 0;    // config [55:48], pixels
    uint8_t paddingBottom = 0; // config [63:56], pixels
};

// SET_L3D_RPT / SET_L3D_RPT_B.
struct L3dRptParamField {
    uint32_t instrId = 0;
    uint16_t repeatStride = 0; // config [15:0], encoded iteration stride
    uint8_t repeatTimes = 0;   // config [23:16]
    bool repeatAlongK = false; // config [24]: false = M, true = K
    uint8_t dstStride = 0;     // config [39:32], 512-byte fractals
    uint8_t dstOffset = 0;     // config [55:48], 512-byte fractals
};

enum class DmaLoopDirection : uint8_t {
    UBUF_TO_GM = 0,
    GM_TO_UBUF = 1,
    GM_TO_CBUF = 2,
};

struct VectorMaskParamField {
    uint64_t vectorMask0 = 0;
    uint64_t vectorMask1 = 0;
};

struct SetPaddingParamField {
    uint64_t value = 0;
};

// SET_CTRL 写入完整的 CTRL 寄存器，包括原子类型 [8:6] 和操作 [10:9]。
struct CtrlRegisterParamField {
    uint64_t value = 0;
};

struct Mte2SourceParamField {
    int64_t srcStride = 0;
};

struct NdDmaPadCountParamField {
    // PAD_CNT_NDDMA carries loop 1 through loop 4. Loop 0 padding belongs to the NDDMA instruction itself.
    std::array<uint8_t, 4> leftPaddingCounts{};
    std::array<uint8_t, 4> rightPaddingCounts{};
};

struct NdDmaLoopStrideParamField {
    uint32_t loopIndex = 0;
    uint64_t srcStride = 0;
    uint32_t dstStride = 0; // config [19:0], in elements; srcStride is config [59:20].
};

struct Mte2NzParamField {
    uint16_t matrixNum = 0;
    uint16_t loop2DstStride = 0;
    uint16_t loop3DstStride = 0;
    uint16_t loop4DstStride = 0;
};

struct Loop3ParamField {
    uint16_t loopCount = 0;
    uint16_t srcStride = 0;
    uint32_t dstStride = 0;
};

struct DmaLoopSizeParamField {
    DmaLoopDirection direction = DmaLoopDirection::GM_TO_UBUF;
    uint32_t loop1Size = 1;
    uint64_t loop2Size = 1;
};

struct DmaLoopStrideParamField {
    DmaLoopDirection direction = DmaLoopDirection::GM_TO_UBUF;
    uint32_t loopIndex = 0;
    uint64_t srcStride = 0;
    uint64_t dstStride = 0;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_REGISTER_H
