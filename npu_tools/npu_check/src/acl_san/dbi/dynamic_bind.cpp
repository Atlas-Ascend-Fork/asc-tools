/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <cstdint>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include "ctrlbin_bindings.h"
#include "dbi/ctrlbin_generator.h"

extern "C" void CtrlbinWriterStart(const char* output, uint16_t length);

namespace {

// Probe parameter masks contain the zero-based ABI argument positions passed to each sanitizer stub.
enum class ProbeArgument : uint16_t {
    FIRST = 0,
    SECOND = 1,
    THIRD = 2,
    FOURTH = 3,
    FIFTH = 4,
};

constexpr uint16_t PROBE_ARGUMENT_0 = static_cast<uint16_t>(ProbeArgument::FIRST);
constexpr uint16_t PROBE_ARGUMENT_1 = static_cast<uint16_t>(ProbeArgument::SECOND);
constexpr uint16_t PROBE_ARGUMENT_2 = static_cast<uint16_t>(ProbeArgument::THIRD);
constexpr uint16_t PROBE_ARGUMENT_3 = static_cast<uint16_t>(ProbeArgument::FOURTH);
constexpr uint16_t PROBE_ARGUMENT_4 = static_cast<uint16_t>(ProbeArgument::FIFTH);

struct BindingSpec {
    InstrType instrType;
    const char* stubName;
    std::vector<uint16_t> paraMask;
};

static const BindingSpec ALL_BINDINGS[] = {
    // MTE2. Binding order is stable for ctrl.bin compatibility; probes follow instr.md order.
    // LOAD_OUT_TO_L1_2DV2.
    {InstrType::LOAD_GM_TO_CBUF_2DV2,
     "__sanitizer_report_load_gm_to_cbuf_2dv2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_OUT_TO_L1_V2.
    {InstrType::COPY_GM_TO_CBUF_V2,
     "__sanitizer_report_copy_gm_to_cbuf_v2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_OUT_TO_L1_ALIGN_V2.b8/b16/b32.
    {InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B8,
     "__sanitizer_report_copy_gm_to_cbuf_align_v2_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B16,
     "__sanitizer_report_copy_gm_to_cbuf_align_v2_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B32,
     "__sanitizer_report_copy_gm_to_cbuf_align_v2_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_OUT_TO_L1_MULTI_ND2NZ.b8/b16/b32.
    {InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B8,
     "__sanitizer_report_copy_gm_to_cbuf_multi_nd2nz_d_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B16,
     "__sanitizer_report_copy_gm_to_cbuf_multi_nd2nz_d_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B32,
     "__sanitizer_report_copy_gm_to_cbuf_multi_nd2nz_d_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_OUT_TO_L1_MULTI_DN2NZ.b8/b16/b32.
    {InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B8,
     "__sanitizer_report_copy_gm_to_cbuf_multi_dn2nz_d_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B16,
     "__sanitizer_report_copy_gm_to_cbuf_multi_dn2nz_d_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B32,
     "__sanitizer_report_copy_gm_to_cbuf_multi_dn2nz_d_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_OUT_TO_UB_ALIGN_V2.b8/b16/b32.
    {InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B8,
     "__sanitizer_report_copy_gm_to_ubuf_align_v2_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B16,
     "__sanitizer_report_copy_gm_to_ubuf_align_v2_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B32,
     "__sanitizer_report_copy_gm_to_ubuf_align_v2_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // ND_DMA_OUT_TO_UB.b8/b16/b32.
    {InstrType::NDDMA_OUT_TO_UB_B8,
     "__sanitizer_report_nd_copy_gm_to_ubuf_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::NDDMA_OUT_TO_UB_B16,
     "__sanitizer_report_nd_copy_gm_to_ubuf_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::NDDMA_OUT_TO_UB_B32,
     "__sanitizer_report_nd_copy_gm_to_ubuf_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // SET_L1_2D.b16/b32.
    {InstrType::SET_L1_2D_B16, "__sanitizer_report_set_l1_2d_b16", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_L1_2D_B32, "__sanitizer_report_set_l1_2d_b32", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    // SET_MTE2_NZ_PARA. The low 16 bits provide matrixNum for subsequent MULTI copies in the same block.
    {InstrType::SET_MTE2_NZ_PARA, "__sanitizer_report_set_mte2_nz_para", {PROBE_ARGUMENT_0}},
    // Scalar SET_* configuration required to reconstruct subsequent MTE2 GM layouts.
    {InstrType::MTE2_SRC_PARA, "__sanitizer_report_set_mte2_src_para", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP_SIZE_UBTOOUT, "__sanitizer_report_set_loop_size_ubtoout", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP1_STRIDE_UBTOOUT, "__sanitizer_report_set_loop1_stride_ubtoout", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP2_STRIDE_UBTOOUT, "__sanitizer_report_set_loop2_stride_ubtoout", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP_SIZE_OUTTOUB, "__sanitizer_report_set_loop_size_outtoub", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP1_STRIDE_OUTTOUB, "__sanitizer_report_set_loop1_stride_outtoub", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP2_STRIDE_OUTTOUB, "__sanitizer_report_set_loop2_stride_outtoub", {PROBE_ARGUMENT_0}},
    {InstrType::PAD_CNT_NDDMA, "__sanitizer_report_set_pad_cnt_nddma", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP0_STRIDE_NDDMA, "__sanitizer_report_set_loop0_stride_nddma", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP1_STRIDE_NDDMA, "__sanitizer_report_set_loop1_stride_nddma", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP2_STRIDE_NDDMA, "__sanitizer_report_set_loop2_stride_nddma", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP3_STRIDE_NDDMA, "__sanitizer_report_set_loop3_stride_nddma", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP4_STRIDE_NDDMA, "__sanitizer_report_set_loop4_stride_nddma", {PROBE_ARGUMENT_0}},
    {InstrType::LOOP3_PARA, "__sanitizer_report_set_loop3_para", {PROBE_ARGUMENT_0}},
    {InstrType::SET_LOOP_SIZE_OUTTOL1, "__sanitizer_report_set_loop_size_outtol1", {PROBE_ARGUMENT_0}},
    {InstrType::SET_LOOP1_STRIDE_OUTTOL1, "__sanitizer_report_set_loop1_stride_outtol1", {PROBE_ARGUMENT_0}},
    {InstrType::SET_LOOP2_STRIDE_OUTTOL1, "__sanitizer_report_set_loop2_stride_outtol1", {PROBE_ARGUMENT_0}},

    // MTE3: MOV_UB_TO_OUT_ALIGN_V2.
    {InstrType::COPY_UBUF_TO_GM_ALIGN_V2,
     "__sanitizer_report_copy_ubuf_to_gm_align_v2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_UB_TO_L1.
    {InstrType::COPY_UBUF_TO_CBUF,
     "__sanitizer_report_copy_ubuf_to_cbuf",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},

    // Scalar: SET_PADDING.
    {InstrType::SET_PADDING, "__sanitizer_report_set_padding", {PROBE_ARGUMENT_0}},
    // MTE1: LOAD_L1_TO_L0B_2D_TRANSPOSE.b8/b16/b32/b4.
    {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B8,
     "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B16,
     "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B32,
     "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B4,
     "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b4",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // LOAD_L1_TO_L0A_2DV2.b4/b16/b8/b32.
    {InstrType::LOAD_CBUF_TO_CA_B4,
     "__sanitizer_report_load_cbuf_to_ca_2dv2_b4",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::LOAD_CBUF_TO_CA_B16,
     "__sanitizer_report_load_cbuf_to_ca_2dv2_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::LOAD_CBUF_TO_CA_B8,
     "__sanitizer_report_load_cbuf_to_ca_2dv2_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::LOAD_CBUF_TO_CA_B32,
     "__sanitizer_report_load_cbuf_to_ca_2dv2_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    // LOAD_L1_TO_L0B_2DV2.b16/b4/b8/b32.
    {InstrType::LOAD_CBUF_TO_CB_B16,
     "__sanitizer_report_load_cbuf_to_cb_2dv2_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::LOAD_CBUF_TO_CB_B4,
     "__sanitizer_report_load_cbuf_to_cb_2dv2_b4",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::LOAD_CBUF_TO_CB_B8,
     "__sanitizer_report_load_cbuf_to_cb_2dv2_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::LOAD_CBUF_TO_CB_B32,
     "__sanitizer_report_load_cbuf_to_cb_2dv2_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    // LOAD_L1_TO_L0A_3DV2.b16/b8 and LOAD_L1_TO_L0B_3DV2.b16/b8/b32.
    {InstrType::IMG2COLV2_CBUF_TO_CA_B16,
     "__sanitizer_report_img2colv2_cbuf_to_ca_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::IMG2COLV2_CBUF_TO_CA_B8,
     "__sanitizer_report_img2colv2_cbuf_to_ca_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::IMG2COLV2_CBUF_TO_CB_B16,
     "__sanitizer_report_img2colv2_cbuf_to_cb_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::IMG2COLV2_CBUF_TO_CB_B8,
     "__sanitizer_report_img2colv2_cbuf_to_cb_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::IMG2COLV2_CBUF_TO_CB_B32,
     "__sanitizer_report_img2colv2_cbuf_to_cb_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_L1_TO_UB.
    {InstrType::COPY_CBUF_TO_UBUF,
     "__sanitizer_report_copy_cbuf_to_ubuf",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    // LOAD_L1_TO_L0A_3DV2.b32.
    {InstrType::IMG2COLV2_CBUF_TO_CA_B32,
     "__sanitizer_report_img2colv2_cbuf_to_ca_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // MOV_L1_TO_BT.f32/s32/f16/bf16.
    {InstrType::COPY_CBUF_TO_BT_F32,
     "__sanitizer_report_copy_cbuf_to_bt_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::COPY_CBUF_TO_BT_S32,
     "__sanitizer_report_copy_cbuf_to_bt_s32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::COPY_CBUF_TO_BT_F16,
     "__sanitizer_report_copy_cbuf_to_bt_f16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::COPY_CBUF_TO_BT_B16,
     "__sanitizer_report_copy_cbuf_to_bt_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},

    // FIX: FIX_L0C_TO_OUT.f32/s32. S4 output modes use the same DBI instruction classes
    // and are distinguished by quant_pre in the raw instruction configuration.
    {InstrType::COPY_MATRIX_CC_TO_GM_F32_A5,
     "__sanitizer_report_copy_matrix_cc_to_gm_f32_a5",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_MATRIX_CC_TO_GM_S32_A5,
     "__sanitizer_report_copy_matrix_cc_to_gm_s32_a5",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // Local-only FIX transfers remain independently instrumented at CCE intrinsic granularity.
    {InstrType::COPY_CBUF_TO_FBUF,
     "__sanitizer_report_copy_cbuf_to_fbuf",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::COPY_MATRIX_CC_TO_CBUF_F32,
     "__sanitizer_report_copy_matrix_cc_to_cbuf_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_MATRIX_CC_TO_CBUF_S32,
     "__sanitizer_report_copy_matrix_cc_to_cbuf_s32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_MATRIX_CC_TO_UB_F32,
     "__sanitizer_report_copy_matrix_cc_to_ubuf_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::COPY_MATRIX_CC_TO_UB_S32,
     "__sanitizer_report_copy_matrix_cc_to_ubuf_s32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // SYNCCHECK: SET/WAIT FLAG, API IDs 440-443, 445-446, 456-459, and 469-474.
    {InstrType::SET_FLAG, "__sanitizer_report_set_flag", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::SET_FLAGI, "__sanitizer_report_set_flagi", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::WAIT_FLAG, "__sanitizer_report_wait_flag", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::WAIT_FLAGI, "__sanitizer_report_wait_flagi", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::WAIT_FLAG_DEV, "__sanitizer_report_wait_flag_dev_pipe", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::WAIT_FLAG_DEVI, "__sanitizer_report_wait_flag_devi_pipe", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_CROSS_CORE, "__sanitizer_report_ffts_cross_core_sync", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_FLAG_V, "__sanitizer_report_set_flag_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_FLAGI_V, "__sanitizer_report_set_flagi_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::WAIT_FLAG_V, "__sanitizer_report_wait_flag_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::WAIT_FLAGI_V, "__sanitizer_report_wait_flagi_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_INTRA_BLOCK_V, "__sanitizer_report_set_intra_block_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_INTRA_BLOCKI_V, "__sanitizer_report_set_intra_blocki_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::WAIT_INTRA_BLOCK_V, "__sanitizer_report_wait_intra_block_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::WAIT_INTRA_BLOCKI_V, "__sanitizer_report_wait_intra_blocki_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_CROSS_CORE_V, "__sanitizer_report_ffts_cross_core_sync_v", {PROBE_ARGUMENT_0}},
    {InstrType::WAIT_FLAG_DEV_V, "__sanitizer_report_wait_flag_dev_pipe_v", {PROBE_ARGUMENT_0}},
    {InstrType::WAIT_FLAG_DEVI_V, "__sanitizer_report_wait_flag_devi_pipe_v", {PROBE_ARGUMENT_0}},
    {InstrType::HSET_FLAG,
     "__sanitizer_report_hset_flag",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::HSET_FLAGI,
     "__sanitizer_report_hset_flagi",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::HWAIT_FLAG,
     "__sanitizer_report_hwait_flag",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::HWAIT_FLAGI,
     "__sanitizer_report_hwait_flagi",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},

    // SYNCCHECK: GET/RLS BUF, API IDs 448-451 and 460-463.
    {InstrType::GET_BUF, "__sanitizer_report_get_buf", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::GET_BUFI, "__sanitizer_report_get_bufi", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::RLS_BUF, "__sanitizer_report_rls_buf", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::RLS_BUFI, "__sanitizer_report_rls_bufi", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::GET_BUF_V, "__sanitizer_report_get_buf_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::GET_BUFI_V, "__sanitizer_report_get_bufi_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::RLS_BUF_V, "__sanitizer_report_rls_buf_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::RLS_BUFI_V, "__sanitizer_report_rls_bufi_v", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},

    // Reference-backed dav-3510 coverage.
    {InstrType::LOAD_CBUF_TO_CA_MX,
     "__sanitizer_report_load_cbuf_to_ca_mx_2dv2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LOAD_CBUF_TO_CB_MX,
     "__sanitizer_report_load_cbuf_to_cb_mx_2dv2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_S8,
     "__sanitizer_report_mad_s8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_F16_F32,
     "__sanitizer_report_mad_f16_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_BF16_F32,
     "__sanitizer_report_mad_bf16_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_F32_F32,
     "__sanitizer_report_mad_f32_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_E4M3_E4M3,
     "__sanitizer_report_mad_e4m3_e4m3",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_E4M3_E5M2,
     "__sanitizer_report_mad_e4m3_e5m2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_E5M2_E4M3,
     "__sanitizer_report_mad_e5m2_e4m3",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_E5M2_E5M2,
     "__sanitizer_report_mad_e5m2_e5m2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E1M2X2_E1M2X2,
     "__sanitizer_report_mad_mx_e1m2_e1m2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E1M2X2_E2M1X2,
     "__sanitizer_report_mad_mx_e1m2_e2m1",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E2M1X2_E1M2X2,
     "__sanitizer_report_mad_mx_e2m1_e1m2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E2M1X2_E2M1X2,
     "__sanitizer_report_mad_mx_e2m1_e2m1",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E4M3_E4M3,
     "__sanitizer_report_mad_mx_e4m3_e4m3",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E4M3_E5M2,
     "__sanitizer_report_mad_mx_e4m3_e5m2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E5M2_E4M3,
     "__sanitizer_report_mad_mx_e5m2_e4m3",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MAD_MX_E5M2_E5M2,
     "__sanitizer_report_mad_mx_e5m2_e5m2",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::MOVEMASK, "__sanitizer_report_movemask", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::MOVE_MASK_0, "__sanitizer_report_move_mask_0", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::MOVE_MASK_1, "__sanitizer_report_move_mask_1", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::COPY_UBUF_TO_UBUF,
     "__sanitizer_report_copy_ubuf_to_ubuf",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::SCATTER_VNCHWCONV_B16,
     "__sanitizer_report_scatter_vnchwconv_b16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::SCATTER_VNCHWCONV_B32,
     "__sanitizer_report_scatter_vnchwconv_b32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::SCATTER_VNCHWCONV_B8,
     "__sanitizer_report_scatter_vnchwconv_b8",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3, PROBE_ARGUMENT_4}},
    {InstrType::VTRANSPOSE_B16, "__sanitizer_report_vtranspose", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::LDVA, "__sanitizer_report_ldva", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::VBS32_F16,
     "__sanitizer_report_vbs32_f16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::VBS32_F32,
     "__sanitizer_report_vbs32_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::VMRGSORT_F16,
     "__sanitizer_report_vmrgsort4_f16",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::VMRGSORT_F32,
     "__sanitizer_report_vmrgsort4_f32",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::VF, "__sanitizer_report_vf", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::PUSH_PB,
     "__sanitizer_report_push_pb",
     {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    // A5 scalar LD/ST instructions share the scalar GM address ABI.
    {InstrType::ST_B64_IMM, "__sanitizer_report_st_b64_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_B32_IMM, "__sanitizer_report_st_b32_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_B16_IMM, "__sanitizer_report_st_b16_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_B8_IMM, "__sanitizer_report_st_b8_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_B64_REG, "__sanitizer_report_st_b64_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_B32_REG, "__sanitizer_report_st_b32_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_B16_REG, "__sanitizer_report_st_b16_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_B8_REG, "__sanitizer_report_st_b8_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STP_B64, "__sanitizer_report_stp_b64", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STP_B32, "__sanitizer_report_stp_b32", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STP_B16, "__sanitizer_report_stp_b16", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STP_B8, "__sanitizer_report_stp_b8", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B64_IMM, "__sanitizer_report_sti_b64_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B32_IMM, "__sanitizer_report_sti_b32_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B16_IMM, "__sanitizer_report_sti_b16_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B8_IMM, "__sanitizer_report_sti_b8_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B64_REG, "__sanitizer_report_sti_b64_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B32_REG, "__sanitizer_report_sti_b32_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B16_REG, "__sanitizer_report_sti_b16_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_B8_REG, "__sanitizer_report_sti_b8_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B64_IMM, "__sanitizer_report_ld_b64_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B32_IMM, "__sanitizer_report_ld_b32_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B16_IMM, "__sanitizer_report_ld_b16_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B8_IMM, "__sanitizer_report_ld_b8_imm", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B64_REG, "__sanitizer_report_ld_b64_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B32_REG, "__sanitizer_report_ld_b32_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B16_REG, "__sanitizer_report_ld_b16_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LD_B8_REG, "__sanitizer_report_ld_b8_reg", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LDP_B64, "__sanitizer_report_ldp_b64", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LDP_B32, "__sanitizer_report_ldp_b32", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LDP_B16, "__sanitizer_report_ldp_b16", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::LDP_B8, "__sanitizer_report_ldp_b8", {PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_ATOMIC_B8,
     "__sanitizer_report_st_atomic_b8",
     {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_ATOMIC_B32,
     "__sanitizer_report_st_atomic_b32",
     {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_ATOMIC_B16,
     "__sanitizer_report_st_atomic_b16",
     {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_ATOMIC_B32,
     "__sanitizer_report_sti_atomic_b32",
     {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_ATOMIC_B16,
     "__sanitizer_report_sti_atomic_b16",
     {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::STI_ATOMIC_B8,
     "__sanitizer_report_sti_atomic_b8",
     {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2, PROBE_ARGUMENT_3}},
    {InstrType::ST_DEV_B64, "__sanitizer_report_st_dev_b64", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::ST_DEV_B32, "__sanitizer_report_st_dev_b32", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::ST_DEV_B16, "__sanitizer_report_st_dev_b16", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::ST_DEV_B8, "__sanitizer_report_st_dev_b8", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::LD_DEV_B64, "__sanitizer_report_ld_dev_b64", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::LD_DEV_B32, "__sanitizer_report_ld_dev_b32", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::LD_DEV_B16, "__sanitizer_report_ld_dev_b16", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::LD_DEV_B8, "__sanitizer_report_ld_dev_b8", {PROBE_ARGUMENT_1, PROBE_ARGUMENT_2}},
    {InstrType::DC_PRELOAD, "__sanitizer_report_dc_preload", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::DC_PRELOADI, "__sanitizer_report_dc_preloadi", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::CHANNEL_PARA, "__sanitizer_report_set_channel_para", {PROBE_ARGUMENT_0}},
    {InstrType::SET_CTRL, "__sanitizer_report_set_ctrl", {PROBE_ARGUMENT_0}},
    {InstrType::SET_FFTS_BASE_ADDR, "__sanitizer_report_set_ffts_base_addr", {PROBE_ARGUMENT_0}},
    {InstrType::SET_FPC, "__sanitizer_report_set_fpc", {PROBE_ARGUMENT_0}},
    {InstrType::SET_QUANT_PRE, "__sanitizer_report_set_quant_pre", {PROBE_ARGUMENT_0}},
    {InstrType::SET_QUANT_POST, "__sanitizer_report_set_quant_post", {PROBE_ARGUMENT_0}},
    {InstrType::SET_LRELU_ALPHA, "__sanitizer_report_set_lrelu_alpha", {PROBE_ARGUMENT_0}},
    {InstrType::SET_FMATRIX, "__sanitizer_report_set_fmatrix", {PROBE_ARGUMENT_0}},
    {InstrType::SET_FMATRIX_B, "__sanitizer_report_set_fmatrix_b", {PROBE_ARGUMENT_0}},
    {InstrType::SET_L3D_RPT, "__sanitizer_report_set_l3d_rpt", {PROBE_ARGUMENT_0}},
    {InstrType::SET_L3D_RPT_B, "__sanitizer_report_set_l3d_rpt_b", {PROBE_ARGUMENT_0}},
    {InstrType::SET_PADDING_B, "__sanitizer_report_set_padding_b", {PROBE_ARGUMENT_0}},
    {InstrType::PIPE_BARRIER, "__sanitizer_report_pipe_barrier", {PROBE_ARGUMENT_0}},
    {InstrType::SET_INTRA_BLOCK, "__sanitizer_report_set_intra_block", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::SET_INTRA_BLOCKI, "__sanitizer_report_set_intra_blocki", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::WAIT_INTRA_BLOCK, "__sanitizer_report_wait_intra_block", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
    {InstrType::WAIT_INTRA_BLOCKI, "__sanitizer_report_wait_intra_blocki", {PROBE_ARGUMENT_0, PROBE_ARGUMENT_1}},
};

using aclsan::ProbeGroup;

struct BindingGroupSpec {
    ProbeGroup group;
    std::initializer_list<InstrType> instructions;
};

static constexpr BindingGroupSpec BINDING_GROUPS[] = {
    {ProbeGroup::Mte1,
     {
         InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B8,
         InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B16,
         InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B32,
         InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B4,
         InstrType::LOAD_CBUF_TO_CA_B4,
         InstrType::LOAD_CBUF_TO_CA_B16,
         InstrType::LOAD_CBUF_TO_CA_B8,
         InstrType::LOAD_CBUF_TO_CA_B32,
         InstrType::LOAD_CBUF_TO_CB_B16,
         InstrType::LOAD_CBUF_TO_CB_B4,
         InstrType::LOAD_CBUF_TO_CB_B8,
         InstrType::LOAD_CBUF_TO_CB_B32,
         InstrType::IMG2COLV2_CBUF_TO_CA_B16,
         InstrType::IMG2COLV2_CBUF_TO_CA_B8,
         InstrType::IMG2COLV2_CBUF_TO_CB_B16,
         InstrType::IMG2COLV2_CBUF_TO_CB_B8,
         InstrType::IMG2COLV2_CBUF_TO_CB_B32,
         InstrType::COPY_CBUF_TO_UBUF,
         InstrType::IMG2COLV2_CBUF_TO_CA_B32,
         InstrType::COPY_CBUF_TO_BT_F32,
         InstrType::COPY_CBUF_TO_BT_S32,
         InstrType::COPY_CBUF_TO_BT_F16,
         InstrType::COPY_CBUF_TO_BT_B16,
         InstrType::LOAD_CBUF_TO_CA_MX,
         InstrType::LOAD_CBUF_TO_CB_MX,
     }},
    {ProbeGroup::Mte2,
     {
         InstrType::LOAD_GM_TO_CBUF_2DV2,
         InstrType::COPY_GM_TO_CBUF_V2,
         InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B8,
         InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B16,
         InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B32,
         InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B8,
         InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B16,
         InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B32,
         InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B8,
         InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B16,
         InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B32,
         InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B8,
         InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B16,
         InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B32,
         InstrType::NDDMA_OUT_TO_UB_B8,
         InstrType::NDDMA_OUT_TO_UB_B16,
         InstrType::NDDMA_OUT_TO_UB_B32,
         InstrType::SET_L1_2D_B16,
         InstrType::SET_L1_2D_B32,
     }},
    {ProbeGroup::Mte3,
     {
         InstrType::COPY_UBUF_TO_GM_ALIGN_V2,
         InstrType::COPY_UBUF_TO_CBUF,
     }},
    {ProbeGroup::Fixpipe,
     {
         InstrType::COPY_MATRIX_CC_TO_GM_F32_A5,
         InstrType::COPY_MATRIX_CC_TO_GM_S32_A5,
         InstrType::COPY_CBUF_TO_FBUF,
         InstrType::COPY_MATRIX_CC_TO_CBUF_F32,
         InstrType::COPY_MATRIX_CC_TO_CBUF_S32,
         InstrType::COPY_MATRIX_CC_TO_UB_F32,
         InstrType::COPY_MATRIX_CC_TO_UB_S32,
     }},
    {ProbeGroup::Scalar,
     {
         InstrType::SET_MTE2_NZ_PARA,
         InstrType::MTE2_SRC_PARA,
         InstrType::LOOP_SIZE_UBTOOUT,
         InstrType::LOOP1_STRIDE_UBTOOUT,
         InstrType::LOOP2_STRIDE_UBTOOUT,
         InstrType::LOOP_SIZE_OUTTOUB,
         InstrType::LOOP1_STRIDE_OUTTOUB,
         InstrType::LOOP2_STRIDE_OUTTOUB,
         InstrType::PAD_CNT_NDDMA,
         InstrType::LOOP0_STRIDE_NDDMA,
         InstrType::LOOP1_STRIDE_NDDMA,
         InstrType::LOOP2_STRIDE_NDDMA,
         InstrType::LOOP3_STRIDE_NDDMA,
         InstrType::LOOP4_STRIDE_NDDMA,
         InstrType::LOOP3_PARA,
         InstrType::SET_LOOP_SIZE_OUTTOL1,
         InstrType::SET_LOOP1_STRIDE_OUTTOL1,
         InstrType::SET_LOOP2_STRIDE_OUTTOL1,
         InstrType::SET_PADDING,
         InstrType::ST_B64_IMM,
         InstrType::ST_B32_IMM,
         InstrType::ST_B16_IMM,
         InstrType::ST_B8_IMM,
         InstrType::ST_B64_REG,
         InstrType::ST_B32_REG,
         InstrType::ST_B16_REG,
         InstrType::ST_B8_REG,
         InstrType::STP_B64,
         InstrType::STP_B32,
         InstrType::STP_B16,
         InstrType::STP_B8,
         InstrType::STI_B64_IMM,
         InstrType::STI_B32_IMM,
         InstrType::STI_B16_IMM,
         InstrType::STI_B8_IMM,
         InstrType::STI_B64_REG,
         InstrType::STI_B32_REG,
         InstrType::STI_B16_REG,
         InstrType::STI_B8_REG,
         InstrType::LD_B64_IMM,
         InstrType::LD_B32_IMM,
         InstrType::LD_B16_IMM,
         InstrType::LD_B8_IMM,
         InstrType::LD_B64_REG,
         InstrType::LD_B32_REG,
         InstrType::LD_B16_REG,
         InstrType::LD_B8_REG,
         InstrType::LDP_B64,
         InstrType::LDP_B32,
         InstrType::LDP_B16,
         InstrType::LDP_B8,
         InstrType::ST_ATOMIC_B8,
         InstrType::ST_ATOMIC_B32,
         InstrType::ST_ATOMIC_B16,
         InstrType::STI_ATOMIC_B32,
         InstrType::STI_ATOMIC_B16,
         InstrType::STI_ATOMIC_B8,
         InstrType::ST_DEV_B64,
         InstrType::ST_DEV_B32,
         InstrType::ST_DEV_B16,
         InstrType::ST_DEV_B8,
         InstrType::LD_DEV_B64,
         InstrType::LD_DEV_B32,
         InstrType::LD_DEV_B16,
         InstrType::LD_DEV_B8,
         InstrType::DC_PRELOAD,
         InstrType::DC_PRELOADI,
         InstrType::CHANNEL_PARA,
         InstrType::SET_CTRL,
         InstrType::SET_FFTS_BASE_ADDR,
         InstrType::SET_FPC,
         InstrType::SET_QUANT_PRE,
         InstrType::SET_QUANT_POST,
         InstrType::SET_LRELU_ALPHA,
         InstrType::SET_FMATRIX,
         InstrType::SET_FMATRIX_B,
         InstrType::SET_L3D_RPT,
         InstrType::SET_L3D_RPT_B,
         InstrType::SET_PADDING_B,
     }},
    {ProbeGroup::Sync,
     {
         InstrType::SET_FLAG,
         InstrType::SET_FLAGI,
         InstrType::WAIT_FLAG,
         InstrType::WAIT_FLAGI,
         InstrType::WAIT_FLAG_DEV,
         InstrType::WAIT_FLAG_DEVI,
         InstrType::SET_CROSS_CORE,
         InstrType::SET_FLAG_V,
         InstrType::SET_FLAGI_V,
         InstrType::WAIT_FLAG_V,
         InstrType::WAIT_FLAGI_V,
         InstrType::SET_INTRA_BLOCK_V,
         InstrType::SET_INTRA_BLOCKI_V,
         InstrType::WAIT_INTRA_BLOCK_V,
         InstrType::WAIT_INTRA_BLOCKI_V,
         InstrType::SET_CROSS_CORE_V,
         InstrType::WAIT_FLAG_DEV_V,
         InstrType::WAIT_FLAG_DEVI_V,
         InstrType::HSET_FLAG,
         InstrType::HSET_FLAGI,
         InstrType::HWAIT_FLAG,
         InstrType::HWAIT_FLAGI,
         InstrType::GET_BUF,
         InstrType::GET_BUFI,
         InstrType::RLS_BUF,
         InstrType::RLS_BUFI,
         InstrType::GET_BUF_V,
         InstrType::GET_BUFI_V,
         InstrType::RLS_BUF_V,
         InstrType::RLS_BUFI_V,
         InstrType::PIPE_BARRIER,
         InstrType::SET_INTRA_BLOCK,
         InstrType::SET_INTRA_BLOCKI,
         InstrType::WAIT_INTRA_BLOCK,
         InstrType::WAIT_INTRA_BLOCKI,
     }},
    {ProbeGroup::Matrix,
     {
         InstrType::MAD_S8,
         InstrType::MAD_F16_F32,
         InstrType::MAD_BF16_F32,
         InstrType::MAD_F32_F32,
         InstrType::MAD_E4M3_E4M3,
         InstrType::MAD_E4M3_E5M2,
         InstrType::MAD_E5M2_E4M3,
         InstrType::MAD_E5M2_E5M2,
         InstrType::MAD_MX_E1M2X2_E1M2X2,
         InstrType::MAD_MX_E1M2X2_E2M1X2,
         InstrType::MAD_MX_E2M1X2_E1M2X2,
         InstrType::MAD_MX_E2M1X2_E2M1X2,
         InstrType::MAD_MX_E4M3_E4M3,
         InstrType::MAD_MX_E4M3_E5M2,
         InstrType::MAD_MX_E5M2_E4M3,
         InstrType::MAD_MX_E5M2_E5M2,
     }},
    {ProbeGroup::Vector,
     {
         InstrType::VF,
         InstrType::PUSH_PB,
         InstrType::MOVEMASK,
         InstrType::MOVE_MASK_0,
         InstrType::MOVE_MASK_1,
         InstrType::COPY_UBUF_TO_UBUF,
         InstrType::SCATTER_VNCHWCONV_B16,
         InstrType::SCATTER_VNCHWCONV_B32,
         InstrType::SCATTER_VNCHWCONV_B8,
         InstrType::VTRANSPOSE_B16,
         InstrType::LDVA,
         InstrType::VBS32_F16,
         InstrType::VBS32_F32,
         InstrType::VMRGSORT_F16,
         InstrType::VMRGSORT_F32,
     }},
};

thread_local const std::vector<aclsan::ProbeGroup>* g_selectedGroups = nullptr;

std::vector<aclsan::ProbeGroup> NormalizeBindingGroups(const std::vector<aclsan::ProbeGroup>& groups)
{
    return aclsan::NormalizeProbeGroups(groups);
}

aclsan::ProbeGroup BindingGroup(const BindingSpec& binding)
{
    for (const auto& group : BINDING_GROUPS) {
        if (std::find(group.instructions.begin(), group.instructions.end(), binding.instrType) !=
            group.instructions.end()) {
            return group.group;
        }
    }
    return ProbeGroup::Sync;
}

bool IsSelected(const BindingSpec& binding)
{
    return g_selectedGroups == nullptr ||
           std::find(g_selectedGroups->begin(), g_selectedGroups->end(), BindingGroup(binding)) !=
               g_selectedGroups->end();
}

bool ValidateBindings(const std::vector<aclsan::ProbeGroup>& groups, std::string& diagnostic)
{
    std::set<uint16_t> instrIds;
    std::set<std::string> symbols;
    for (const auto& binding : ALL_BINDINGS) {
        if (std::find(groups.begin(), groups.end(), BindingGroup(binding)) == groups.end()) {
            continue;
        }
        const uint16_t instrId = static_cast<uint16_t>(binding.instrType);
        if (!instrIds.insert(instrId).second) {
            diagnostic = "duplicate instruction ID " + std::to_string(instrId);
            return false;
        }
        if (!symbols.insert(binding.stubName).second) {
            diagnostic = "duplicate injected symbol " + std::string(binding.stubName);
            return false;
        }
    }
    return true;
}

bool IsNonEmptyFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    return input.good() && input.tellg() > 0;
}

} // namespace

extern "C" void RegisterCtrlbinBindings()
{
    for (const auto& binding : ALL_BINDINGS) {
        if (IsSelected(binding)) {
            RegisterCtrlbinBinding(binding.instrType, binding.stubName, binding.paraMask);
        }
    }
}

namespace aclsan {

std::vector<ProbeGroup> AllProbeGroups()
{
    return {ProbeGroup::Mte1,   ProbeGroup::Mte2, ProbeGroup::Mte3,   ProbeGroup::Fixpipe,
            ProbeGroup::Scalar, ProbeGroup::Sync, ProbeGroup::Matrix, ProbeGroup::Vector};
}

std::vector<std::string> BindingSymbols(const std::vector<ProbeGroup>& groups)
{
    std::vector<std::string> symbols;
    const auto selected = NormalizeBindingGroups(groups);
    for (const auto& binding : ALL_BINDINGS) {
        if (std::find(selected.begin(), selected.end(), BindingGroup(binding)) != selected.end()) {
            symbols.emplace_back(binding.stubName);
        }
    }
    return symbols;
}

std::string CtrlBinGeneratorIdentity()
{
    uint64_t hash = 1469598103934665603ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    const auto append = [&](const std::string& value) {
        for (const unsigned char byte : value) {
            hash ^= byte;
            hash *= prime;
        }
        hash ^= 0xffU;
        hash *= prime;
    };
    for (const auto& binding : ALL_BINDINGS) {
        append(std::to_string(static_cast<uint16_t>(binding.instrType)));
        append(binding.stubName);
        append(std::to_string(static_cast<unsigned int>(BindingGroup(binding))));
        for (const uint16_t mask : binding.paraMask) {
            append(std::to_string(mask));
        }
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

bool GenerateCtrlBin(const std::string& outputPath, const std::vector<ProbeGroup>& groups, std::string& diagnostic)
{
    std::vector<ProbeGroup> selected = NormalizeBindingGroups(groups);
    if (selected.empty()) {
        diagnostic = "probe set is empty";
        return false;
    }
    if (!ValidateBindings(selected, diagnostic)) {
        return false;
    }
    if (outputPath.size() > UINT16_MAX) {
        diagnostic = "output path is too long";
        return false;
    }
    g_selectedGroups = &selected;
    CtrlbinWriterStart(outputPath.c_str(), static_cast<uint16_t>(outputPath.size()));
    g_selectedGroups = nullptr;
    if (!IsNonEmptyFile(outputPath)) {
        diagnostic = "failed to generate a non-empty file: " + outputPath;
        return false;
    }
    diagnostic.clear();
    return true;
}

} // namespace aclsan
