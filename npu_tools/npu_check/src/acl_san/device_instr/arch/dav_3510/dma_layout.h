/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_ARCH_DAV_3510_DMA_LAYOUT_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_ARCH_DAV_3510_DMA_LAYOUT_H

#include "device_instr/common/bit_range.h"

namespace aclsan::dav3510 {

// TODO: 解码部分可能得放私仓
// TODO: 看A5和A6什么区别
// CopyGmToCbufAlignV2 + CopyGmToUbufAlignV2
struct MovAlignV2Layout {
    static constexpr BitRange SID{0, 3};
    static constexpr BitRange BURST_NUM{4, 24};
    static constexpr BitRange BURST_LEN{25, 45};
    static constexpr BitRange LEFT_PADDING_COUNT{46, 51};
    static constexpr BitRange RIGHT_PADDING_COUNT{52, 57};
    static constexpr BitRange DATA_SELECT_BIT{58, 58};
    static constexpr BitRange L2_CACHE_CONTROL{60, 63};
    static constexpr BitRange BURST_SRC_STRIDE{0, 39};
    static constexpr BitRange BURST_DST_STRIDE{40, 60};
};

struct CopyUbufToUbufLayout {
    static constexpr BitRange BURST_NUM{0, 15};
    static constexpr BitRange BURST_LEN{16, 31};
    static constexpr BitRange SRC_GAP{32, 47};
    static constexpr BitRange DST_GAP{48, 63};
};

struct CopyGmToCbufMultiLayout {
    static constexpr BitRange SID{0, 3};
    static constexpr BitRange LOOP1_SRC_STRIDE{4, 43};
    static constexpr BitRange L2_CACHE_CONTROL{44, 47};
    static constexpr BitRange N_VALUE{48, 63};
    static constexpr BitRange D_VALUE{0, 20};
    static constexpr BitRange LOOP4_SRC_STRIDE{21, 60};
    static constexpr BitRange SMALL_C0_ENABLE{61, 61};
};

struct CopyGmToCbufV2Layout {
    static constexpr BitRange SID{0, 3};
    static constexpr BitRange BURST_NUM{4, 20};
    static constexpr BitRange BURST_LEN{25, 41};
    static constexpr BitRange PAD_FUNCTION_MODE{56, 59};
    static constexpr BitRange L2_CACHE_CONTROL{60, 63};
    static constexpr BitRange SRC_STRIDE{0, 35};
    static constexpr BitRange DST_STRIDE{40, 56};
};

struct LoadGmToCbuf2DV2Layout {
    static constexpr BitRange M_START_POSITION{0, 31};
    static constexpr BitRange K_START_POSITION{32, 63};
    static constexpr BitRange DST_STRIDE{0, 11};
    static constexpr BitRange M_STEP{12, 23};
    static constexpr BitRange K_STEP{24, 35};
    static constexpr BitRange SID{36, 39};
    static constexpr BitRange DECOMP_MODE{40, 42};
    static constexpr BitRange L2_CACHE_CONTROL{60, 63};
};

struct NdDmaLayout {
    static constexpr BitRange SID{0, 3};
    static constexpr BitRange LOOP0_SIZE{4, 23};
    static constexpr BitRange LOOP1_SIZE{24, 43};
    static constexpr BitRange LOOP2_SIZE{44, 63};
    static constexpr BitRange LOOP3_SIZE{0, 19};
    static constexpr BitRange LOOP4_SIZE{20, 39};
    static constexpr BitRange LOOP0_LEFT_PADDING_COUNT{40, 47};
    static constexpr BitRange LOOP0_RIGHT_PADDING_COUNT{48, 55};
    static constexpr BitRange PADDING_MODE{56, 56};
    static constexpr BitRange L2_CACHE_CONTROL{60, 63};
};

struct SetL12DLayout {
    static constexpr BitRange REPEAT_TIMES{0, 14};
    static constexpr BitRange BLOCK_NUM{16, 30};
    static constexpr BitRange REPEAT_GAP{32, 46};
};

struct FixL0cToOutLayout {
    static constexpr BitRange SID{0, 3};
    static constexpr BitRange N_SIZE{4, 15};
    static constexpr BitRange M_SIZE{16, 31};
    static constexpr BitRange LOOP_DST_STRIDE{32, 63};
    static constexpr BitRange LOOP_SRT_STRIDE{0, 15};
    static constexpr BitRange L2_CACHE_CONTROL{16, 19};
    static constexpr BitRange QUANT_PRE_HIGH{29, 29};
    static constexpr BitRange CLIP_RELU_PRE{30, 31};
    static constexpr BitRange UNIT_FLAG{32, 33};
    static constexpr BitRange QUANT_PRE_LOW{34, 38};
    static constexpr BitRange RELU_PRE{39, 41};
    static constexpr BitRange SPLIT_ENABLE{42, 42};
    static constexpr BitRange NZ2ND_ENABLE{43, 43};
    static constexpr BitRange QUANT_POST{44, 48};
    static constexpr BitRange RELU_POST{49, 51};
    static constexpr BitRange CLIP_RELU_POST{52, 52};
    static constexpr BitRange LOOP_ENHANCE_ENABLE{53, 53};
    static constexpr BitRange ELTWISE_OP{54, 56};
    static constexpr BitRange ELTWISE_ANTQ_ENABLE{57, 57};
    static constexpr BitRange LOOP_ENHANCE_MERGE_ENABLE{58, 58};
    static constexpr BitRange C0_PAD_ENABLE{59, 59};
    static constexpr BitRange WINO_POST_ENABLE{60, 60};
    static constexpr BitRange BRCB_ENABLE{61, 61};
    static constexpr BitRange NZ2DN_ENABLE{62, 62};
};

// Packed register and cube-operation fields consumed by the dav-3510 decoder.
struct FmatrixLayout {
    static constexpr BitRange HEIGHT{16, 31};
    static constexpr BitRange PADDING_LEFT{32, 39};
    static constexpr BitRange PADDING_RIGHT{40, 47};
    static constexpr BitRange PADDING_TOP{48, 55};
    static constexpr BitRange PADDING_BOTTOM{56, 63};
};

struct L3dRptLayout {
    static constexpr BitRange REPEAT_TIMES{16, 23};
    static constexpr BitRange REPEAT_ALONG_K{24, 24};
    static constexpr BitRange DST_STRIDE{32, 39};
    static constexpr BitRange DST_OFFSET{48, 55};
};

struct LoadL1ToL0Layout {
    static constexpr BitRange K_START_POSITION{16, 31};
    static constexpr BitRange M_STEP{32, 39};
    static constexpr BitRange K_STEP{40, 47};
    static constexpr BitRange DST_STRIDE{16, 31};
};

struct LoadL1TransposeLayout {
    static constexpr BitRange REPEAT{16, 23};
    static constexpr BitRange SRC_STRIDE{24, 39};
    static constexpr BitRange DST_GAP{44, 59};
    static constexpr BitRange SRC_FRAC_GAP{16, 31};
    static constexpr BitRange DECREMENT{63, 63};
};

struct Img2ColLayout {
    static constexpr BitRange M_EXTENSION{16, 31};
    static constexpr BitRange K_START_POINT{32, 47};
    static constexpr BitRange M_START_POINT{48, 63};
    static constexpr BitRange STRIDE_WIDTH{0, 5};
    static constexpr BitRange STRIDE_HEIGHT{6, 11};
    static constexpr BitRange FILTER_WIDTH{12, 19};
    static constexpr BitRange FILTER_HEIGHT{20, 27};
    static constexpr BitRange DILATION_FILTER_WIDTH{28, 35};
    static constexpr BitRange DILATION_FILTER_HEIGHT{36, 43};
    static constexpr BitRange FILTER_SIZE_WIDTH{44, 44};
    static constexpr BitRange FILTER_SIZE_HEIGHT{45, 45};
    static constexpr BitRange TRANSPOSE{46, 46};
    static constexpr BitRange FMATRIX_CONTROL{47, 47};
    static constexpr BitRange CHANNEL_SIZE{48, 63};
};

struct MmadLayout {
    static constexpr BitRange M_SIZE{0, 11};
    static constexpr BitRange K_SIZE{12, 23};
    static constexpr BitRange N_SIZE{24, 35};
    static constexpr BitRange DISABLE_GEMV{61, 61};
    static constexpr BitRange CMATRIX_SOURCE{62, 62};
    static constexpr BitRange CMATRIX_INIT_VAL{63, 63};
};

struct LoadL1MxLayout {
    static constexpr BitRange Y_START{16, 31};
    static constexpr BitRange X_STEP{32, 39};
    static constexpr BitRange Y_STEP{40, 47};
};

} // namespace aclsan::dav3510

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_ARCH_DAV_3510_DMA_LAYOUT_H
