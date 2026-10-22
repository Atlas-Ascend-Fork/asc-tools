/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_DMA_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_DMA_H

#include "device_instr/common/device_instr_types.h"
#include "dbi/trace_buffer_abi.h"

#include <array>
#include <cstdint>

namespace aclsan {

struct MmadParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t src0Addr = 0;
    uint64_t src1Addr = 0;
    uint16_t m = 0;              // config [0:11]
    uint16_t k = 0;              // config [12:23]
    uint16_t n = 0;              // config [24:35]
    uint8_t unitFlag = 0;        // config [55:56], mode 2/3 enables unit-flag ordering
    bool disableGemv = false;    // config [61]
    bool cmatrixSource = false;  // config [62]
    bool cmatrixInitVal = false; // config [63]
};

struct LoadL1MxParamField {
    uint32_t instrId = 0;
    uint64_t srcAddr = 0;
    uint16_t xStart = 0;    // config0 [0:15]
    uint16_t yStart = 0;    // config0 [16:31]
    uint8_t xStep = 0;      // config0 [32:39]
    uint8_t yStep = 0;      // config0 [40:47]
    uint16_t srcStride = 0; // config1 [0:15], unit: 32B
};

// 位域注释规则：
// 1. bit index 从 0 开始。
// 2. [begin:end] 是闭区间，读取 begin 到 end；例如 [0:3] 读取 bit 0~3，取值范围为 0~15。
// 3. [bit] 只读取指定的一个 bit；例如 [58] 只读取 bit 58。
// 4. 未被任何区间覆盖的 bit 是空隙位，转换时忽略，不自动并入相邻字段。
// 5. 新增字段时，区间必须满足 0 <= begin <= end < 64，并明确写出来源 config0 或 config1。

// - MOV_OUT_TO_L1_ALIGN_V2.<b8/b16/b32>
// - MOV_OUT_TO_UB_ALIGN_V2.<b8/b16/b32>
template <AclsanDeviceMemorySpace SrcSpace, AclsanDeviceMemorySpace DstSpace>
struct MovAlignV2ParamField {
    static constexpr AclsanDeviceMemorySpace SRC_POS = SrcSpace;
    static constexpr AclsanDeviceMemorySpace DST_POS = DstSpace;

    uint32_t instrId = 0;
    uint32_t dataBits = 0; // InstructionId 中的 dtype 位宽，单位为 bit；0 表示未知
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint8_t sid = 0;               // config0 [0:3]
    uint32_t burstNum = 0;         // config0 [4:24]
    uint32_t burstLen = 0;         // config0 [25:45]   单位为Byte
    uint8_t leftPaddingCount = 0;  // config0 [46:51]
    uint8_t rightPaddingCount = 0; // config0 [52:57]
    bool dataSelectBit = false;    // config0 [58]
    uint8_t l2CacheControl = 0;    // config0 [60:63]
    uint64_t burstSrcStride = 0;   // config1 [0:39]    相邻burst源首地址步长，单位为Byte
    uint32_t burstDstStride = 0;   // config1 [40:60]   单位为Byte
};

using CopyGmToUbufAlignV2ParamField =
    MovAlignV2ParamField<ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_SPACE_UB>;
using CopyGmToCbufAlignV2ParamField =
    MovAlignV2ParamField<ACLSAN_DEVICE_MEMORY_SPACE_GM, ACLSAN_DEVICE_MEMORY_SPACE_L1>;

// MOV_UB_TO_UB. Lengths and gaps use 32-byte units.
struct CopyUbufToUbufParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint16_t burstNum = 0; // config [0:15]
    uint16_t burstLen = 0; // config [16:31]
    uint16_t srcGap = 0;   // config [32:47]
    uint16_t dstGap = 0;   // config [48:63]
};

// - MOV_OUT_TO_L1_MULTI_DN2NZ.<b8/b16/b32>
// - MOV_OUT_TO_L1_MULTI_ND2NZ.<b8/b16/b32>
template <NdNzConversionMode ConversionMode>
struct CopyGmToCbufMultiParamField {
    static constexpr NdNzConversionMode CONVERSION_MODE = ConversionMode;

    uint32_t instrId = 0;
    uint32_t dataBits = 0; // InstructionId 中的 dtype 位宽，单位为 bit；0 表示未知
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint8_t sid = 0;             // config0 [0:3]
    uint64_t loop1SrcStride = 0; // config0 [4:43]
    uint8_t l2CacheControl = 0;  // config0 [44:47]
    uint16_t nValue = 0;         // config0 [48:63]
    uint32_t dValue = 0;         // config1 [0:20]
    uint64_t loop4SrcStride = 0; // config1 [21:60]
    bool smallC0Enable = false;  // config1 [61]
};

using CopyGmToCbufMultiDn2NzParamField = CopyGmToCbufMultiParamField<NdNzConversionMode::DN2NZ>;
using CopyGmToCbufMultiNd2NzParamField = CopyGmToCbufMultiParamField<NdNzConversionMode::ND2NZ>;

// MOV_OUT_TO_L1_V2
struct CopyGmToCbufV2ParamField {
    static constexpr AclsanDeviceMemorySpace SRC_POS = ACLSAN_DEVICE_MEMORY_SPACE_GM;
    static constexpr AclsanDeviceMemorySpace DST_POS = ACLSAN_DEVICE_MEMORY_SPACE_L1;

    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint8_t sid = 0;             // config0 [0:3]
    uint32_t burstNum = 0;       // config0 [4:20]
    uint32_t burstLen = 0;       // config0 [25:41]    单位为C0_SIZE(32B)
    uint8_t padFunctionMode = 0; // config0 [56:59]
    uint8_t l2CacheControl = 0;  // config0 [60:63]
    uint64_t srcStride = 0;      // config1 [0:35]     相邻burst源首地址步长，单位为C0_SIZE(32B)
    uint32_t dstStride = 0;      // config1 [40:56]    单位为C0_SIZE(32B)
};

// LOAD_OUT_TO_L1_2DV2
struct LoadGmToCbuf2DV2ParamField {
    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint32_t mStartPosition = 0; // config0 [0:31]
    uint32_t kStartPosition = 0; // config0 [32:63]
    uint16_t dstStride = 0;      // config1 [0:11]
    uint16_t mStep = 0;          // config1 [12:23]
    uint16_t kStep = 0;          // config1 [24:35]
    uint8_t sid = 0;             // config1 [36:39]
    uint8_t decompMode = 0;      // config1 [40:42]
    uint8_t l2CacheControl = 0;  // config1 [60:63]
};

// MOV_UB_TO_OUT_ALIGN_V2
struct CopyUbufToGmAlignV2ParamField {
    static constexpr AclsanDeviceMemorySpace SRC_POS = ACLSAN_DEVICE_MEMORY_SPACE_UB;
    static constexpr AclsanDeviceMemorySpace DST_POS = ACLSAN_DEVICE_MEMORY_SPACE_GM;

    uint32_t instrId = 0;
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint8_t sid = 0;            // config0 [0:3]
    uint32_t burstNum = 0;      // config0 [4:24]
    uint32_t burstLen = 0;      // config0 [25:45]   单位为Byte
    uint8_t l2CacheControl = 0; // config0 [60:63]
    uint64_t dstStride = 0;     // config1 [0:39]    相邻burst目的首地址步长，单位为Byte
    uint32_t srcStride = 0;     // config1 [40:60]   单位为Byte
};

// - ND_DMA_OUT_TO_UB.<b8/b16/b32>
struct NdDmaParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0; // InstructionId 中的 dtype 位宽，单位为 bit；0 表示未知
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint8_t sid = 0;                    // config0 [0:3]
    uint32_t loop0Size = 0;             // config0 [4:23]
    uint32_t loop1Size = 0;             // config0 [24:43]
    uint32_t loop2Size = 0;             // config0 [44:63]
    uint32_t loop3Size = 0;             // config1 [0:19]
    uint32_t loop4Size = 0;             // config1 [20:39]
    uint8_t loop0LeftPaddingCount = 0;  // config1 [40:47]
    uint8_t loop0RightPaddingCount = 0; // config1 [48:55]
    bool paddingMode = false;           // config1 [56]
    uint8_t l2CacheControl = 0;         // config1 [60:63]
};

using NdDmaOutToUbufParamField = NdDmaParamField;

// SET_L1_2D.<b16/b32>
// 1个dst + 1个64B config
struct SetL12DParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0; // InstructionId 中的 dtype 位宽，单位为 bit；0 表示未知
    uint64_t dstAddr = 0;
    uint16_t repeatTimes = 0; // config0 [0:14]
    uint16_t blockNum = 0;    // config0 [16:30]
    uint16_t repeatGap = 0;   // config0 [32:46]
};

// FIX_L0C_TO_OUT.<f32/s32>
struct FixL0cToOutParamField {
    uint32_t instrId = 0;
    uint32_t dataBits = 0; // InstructionId 中的 dtype 位宽，单位为 bit；0 表示未知
    uint64_t dstAddr = 0;
    uint64_t srcAddr = 0;
    uint8_t sid = 0;                     // config0 [0:3]
    uint16_t nSize = 0;                  // config0 [4:15]
    uint16_t mSize = 0;                  // config0 [16:31]
    uint32_t loopDstStride = 0;          // config0 [32:63]
    uint16_t loopSrtStride = 0;          // config1 [0:15]
    uint8_t l2CacheControl = 0;          // config1 [16:19]
    uint8_t clipReluPre = 0;             // config1 [30:31]
    uint8_t unitFlag = 0;                // config1 [32:33]
    uint8_t quantPre = 0;                // config1 [29] is bit 5; config1 [34:38] contains bits [4:0]
    uint8_t reluPre = 0;                 // config1 [39:41]
    bool splitEnable = false;            // config1 [42]
    bool nz2ndEnable = false;            // config1 [43]
    uint8_t quantPost = 0;               // config1 [44:48]
    uint8_t reluPost = 0;                // config1 [49:51]
    bool clipReluPost = false;           // config1 [52]
    bool loopEnhanceEnable = false;      // config1 [53]
    uint8_t eltwiseOp = 0;               // config1 [54:56]
    bool eltwiseAntqEnable = false;      // config1 [57]
    bool loopEnhanceMergeEnable = false; // config1 [58]
    bool c0PadEnable = false;            // config1 [59]
    bool winoPostEnable = false;         // config1 [60]
    bool brcbEnable = false;             // config1 [61]
    bool nz2dnEnable = false;            // config1 [62]
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_DEVICE_INSTR_STRUCT_DMA_H
