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
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include "ctrlbin_bindings.h"
#include "dbi/ctrlbin_generator.h"

extern "C" void CtrlbinWriterStart(const char* output, uint16_t length);

namespace {

struct BindingSpec {
    InstrType instrType;
    uint16_t apiId;
    const char* stubName;
    std::vector<uint16_t> paraMask;
};

thread_local const std::vector<aclsan::ProbeGroup>* g_selectedGroups = nullptr;

std::vector<aclsan::ProbeGroup> NormalizeBindingGroups(const std::vector<aclsan::ProbeGroup>& groups)
{
    return aclsan::NormalizeProbeGroups(groups);
}

aclsan::ProbeGroup BindingGroup(const BindingSpec& binding)
{
    using aclsan::ProbeGroup;
    const uint16_t id = binding.apiId;
    if (id >= 400 && id <= 415) {
        return ProbeGroup::Matrix;
    }
    if ((id >= 174 && id <= 178) || (id >= 417 && id <= 421)) {
        return ProbeGroup::Vector;
    }
    if ((id >= 24 && id <= 71) || id == 90 || id == 123 || (id >= 124 && id <= 136) || (id >= 160 && id <= 164) ||
        (id >= 386 && id <= 387) || (id >= 390 && id <= 399)) {
        return ProbeGroup::Scalar;
    }
    if ((id >= 72 && id <= 82) || (id >= 84 && id <= 89) || id == 149 || id == 150) {
        return ProbeGroup::Mte2;
    }
    if (id == 83 || id == 173) {
        return ProbeGroup::Mte3;
    }
    if ((id >= 137 && id <= 158) || (id >= 422 && id <= 426)) {
        return ProbeGroup::Mte1;
    }
    if (id == 91 || id == 92 || (id >= 167 && id <= 171)) {
        return ProbeGroup::Fixpipe;
    }
    return ProbeGroup::Sync;
}

bool IsSelected(const BindingSpec& binding)
{
    return g_selectedGroups == nullptr ||
           std::find(g_selectedGroups->begin(), g_selectedGroups->end(), BindingGroup(binding)) !=
               g_selectedGroups->end();
}

const std::vector<BindingSpec>& AllBindings()
{
    static const std::vector<BindingSpec> bindings = {
        // MTE2. Binding order is stable for ctrl.bin compatibility; probes follow instr.md order.
        // LOAD_OUT_TO_L1_2DV2.
        {InstrType::LOAD_GM_TO_CBUF_2DV2, 72, "__sanitizer_report_load_gm_to_cbuf_2dv2", {0, 1, 2, 3}},
        // MOV_OUT_TO_L1_V2.
        {InstrType::COPY_GM_TO_CBUF_V2, 73, "__sanitizer_report_copy_gm_to_cbuf_v2", {0, 1, 2, 3}},
        // MOV_OUT_TO_L1_ALIGN_V2.b8/b16/b32.
        {InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B8, 74, "__sanitizer_report_copy_gm_to_cbuf_align_v2_b8", {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B16, 75, "__sanitizer_report_copy_gm_to_cbuf_align_v2_b16", {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_CBUF_ALIGN_V2_B32, 76, "__sanitizer_report_copy_gm_to_cbuf_align_v2_b32", {0, 1, 2, 3}},
        // MOV_OUT_TO_L1_MULTI_ND2NZ.b8/b16/b32.
        {InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B8,
         77,
         "__sanitizer_report_copy_gm_to_cbuf_multi_nd2nz_d_b8",
         {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B16,
         78,
         "__sanitizer_report_copy_gm_to_cbuf_multi_nd2nz_d_b16",
         {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_CBUF_MULTI_ND2NZ_D_B32,
         79,
         "__sanitizer_report_copy_gm_to_cbuf_multi_nd2nz_d_b32",
         {0, 1, 2, 3}},
        // MOV_OUT_TO_L1_MULTI_DN2NZ.b8/b16/b32.
        {InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B8,
         80,
         "__sanitizer_report_copy_gm_to_cbuf_multi_dn2nz_d_b8",
         {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B16,
         81,
         "__sanitizer_report_copy_gm_to_cbuf_multi_dn2nz_d_b16",
         {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_CBUF_MULTI_DN2NZ_D_B32,
         82,
         "__sanitizer_report_copy_gm_to_cbuf_multi_dn2nz_d_b32",
         {0, 1, 2, 3}},
        // MOV_OUT_TO_UB_ALIGN_V2.b8/b16/b32.
        {InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B8, 84, "__sanitizer_report_copy_gm_to_ubuf_align_v2_b8", {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B16, 85, "__sanitizer_report_copy_gm_to_ubuf_align_v2_b16", {0, 1, 2, 3}},
        {InstrType::COPY_GM_TO_UBUF_ALIGN_V2_B32, 86, "__sanitizer_report_copy_gm_to_ubuf_align_v2_b32", {0, 1, 2, 3}},
        // ND_DMA_OUT_TO_UB.b8/b16/b32.
        {InstrType::NDDMA_OUT_TO_UB_B8, 87, "__sanitizer_report_nd_copy_gm_to_ubuf_b8", {0, 1, 2, 3}},
        {InstrType::NDDMA_OUT_TO_UB_B16, 88, "__sanitizer_report_nd_copy_gm_to_ubuf_b16", {0, 1, 2, 3}},
        {InstrType::NDDMA_OUT_TO_UB_B32, 89, "__sanitizer_report_nd_copy_gm_to_ubuf_b32", {0, 1, 2, 3}},
        // SET_L1_2D.b16/b32.
        {InstrType::SET_L1_2D_B16, 149, "__sanitizer_report_set_l1_2d_b16", {0, 1}},
        {InstrType::SET_L1_2D_B32, 150, "__sanitizer_report_set_l1_2d_b32", {0, 1}},
        // SET_MTE2_NZ_PARA. The low 16 bits provide matrixNum for subsequent MULTI copies in the same block.
        {InstrType::SET_MTE2_NZ_PARA, 399, "__sanitizer_report_set_mte2_nz_para", {0}},
        // Scalar SET_* configuration required to reconstruct subsequent MTE2 GM layouts.
        {InstrType::MTE2_SRC_PARA, 124, "__sanitizer_report_set_mte2_src_para", {0}},
        {InstrType::LOOP_SIZE_UBTOOUT, 125, "__sanitizer_report_set_loop_size_ubtoout", {0}},
        {InstrType::LOOP1_STRIDE_UBTOOUT, 126, "__sanitizer_report_set_loop1_stride_ubtoout", {0}},
        {InstrType::LOOP2_STRIDE_UBTOOUT, 127, "__sanitizer_report_set_loop2_stride_ubtoout", {0}},
        {InstrType::LOOP_SIZE_OUTTOUB, 128, "__sanitizer_report_set_loop_size_outtoub", {0}},
        {InstrType::LOOP1_STRIDE_OUTTOUB, 129, "__sanitizer_report_set_loop1_stride_outtoub", {0}},
        {InstrType::LOOP2_STRIDE_OUTTOUB, 130, "__sanitizer_report_set_loop2_stride_outtoub", {0}},
        {InstrType::PAD_CNT_NDDMA, 131, "__sanitizer_report_set_pad_cnt_nddma", {0}},
        {InstrType::LOOP0_STRIDE_NDDMA, 132, "__sanitizer_report_set_loop0_stride_nddma", {0}},
        {InstrType::LOOP1_STRIDE_NDDMA, 133, "__sanitizer_report_set_loop1_stride_nddma", {0}},
        {InstrType::LOOP2_STRIDE_NDDMA, 134, "__sanitizer_report_set_loop2_stride_nddma", {0}},
        {InstrType::LOOP3_STRIDE_NDDMA, 135, "__sanitizer_report_set_loop3_stride_nddma", {0}},
        {InstrType::LOOP4_STRIDE_NDDMA, 136, "__sanitizer_report_set_loop4_stride_nddma", {0}},
        {InstrType::LOOP3_PARA, 90, "__sanitizer_report_set_loop3_para", {0}},
        {InstrType::SET_LOOP_SIZE_OUTTOL1, 394, "__sanitizer_report_set_loop_size_outtol1", {0}},
        {InstrType::SET_LOOP1_STRIDE_OUTTOL1, 395, "__sanitizer_report_set_loop1_stride_outtol1", {0}},
        {InstrType::SET_LOOP2_STRIDE_OUTTOL1, 396, "__sanitizer_report_set_loop2_stride_outtol1", {0}},

        // MTE3: MOV_UB_TO_OUT_ALIGN_V2.
        {InstrType::COPY_UBUF_TO_GM_ALIGN_V2, 83, "__sanitizer_report_copy_ubuf_to_gm_align_v2", {0, 1, 2, 3}},
        // MOV_UB_TO_L1.
        {InstrType::COPY_UBUF_TO_CBUF, 173, "__sanitizer_report_copy_ubuf_to_cbuf", {0, 1, 2}},

        // Scalar: SET_PADDING.
        {InstrType::SET_PADDING, 392, "__sanitizer_report_set_padding", {0}},
        // MTE1: LOAD_L1_TO_L0B_2D_TRANSPOSE.b8/b16/b32/b4.
        {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B8,
         137,
         "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b8",
         {0, 1, 2, 3}},
        {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B16,
         138,
         "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b16",
         {0, 1, 2, 3}},
        {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B32,
         139,
         "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b32",
         {0, 1, 2, 3}},
        {InstrType::LOAD_CBUF_TO_CB_TRANSPOSE_B4,
         140,
         "__sanitizer_report_load_cbuf_to_cb_2d_transpose_b4",
         {0, 1, 2, 3}},
        // LOAD_L1_TO_L0A_2DV2.b4/b16/b8/b32.
        {InstrType::LOAD_CBUF_TO_CA_B4, 141, "__sanitizer_report_load_cbuf_to_ca_2dv2_b4", {0, 1, 2, 3, 4}},
        {InstrType::LOAD_CBUF_TO_CA_B16, 142, "__sanitizer_report_load_cbuf_to_ca_2dv2_b16", {0, 1, 2, 3, 4}},
        {InstrType::LOAD_CBUF_TO_CA_B8, 143, "__sanitizer_report_load_cbuf_to_ca_2dv2_b8", {0, 1, 2, 3, 4}},
        {InstrType::LOAD_CBUF_TO_CA_B32, 144, "__sanitizer_report_load_cbuf_to_ca_2dv2_b32", {0, 1, 2, 3, 4}},
        // LOAD_L1_TO_L0B_2DV2.b16/b4/b8/b32.
        {InstrType::LOAD_CBUF_TO_CB_B16, 145, "__sanitizer_report_load_cbuf_to_cb_2dv2_b16", {0, 1, 2, 3, 4}},
        {InstrType::LOAD_CBUF_TO_CB_B4, 146, "__sanitizer_report_load_cbuf_to_cb_2dv2_b4", {0, 1, 2, 3, 4}},
        {InstrType::LOAD_CBUF_TO_CB_B8, 147, "__sanitizer_report_load_cbuf_to_cb_2dv2_b8", {0, 1, 2, 3, 4}},
        {InstrType::LOAD_CBUF_TO_CB_B32, 148, "__sanitizer_report_load_cbuf_to_cb_2dv2_b32", {0, 1, 2, 3, 4}},
        // LOAD_L1_TO_L0A_3DV2.b16/b8 and LOAD_L1_TO_L0B_3DV2.b16/b8/b32.
        {InstrType::IMG2COLV2_CBUF_TO_CA_B16, 153, "__sanitizer_report_img2colv2_cbuf_to_ca_b16", {0, 1, 2, 3}},
        {InstrType::IMG2COLV2_CBUF_TO_CA_B8, 154, "__sanitizer_report_img2colv2_cbuf_to_ca_b8", {0, 1, 2, 3}},
        {InstrType::IMG2COLV2_CBUF_TO_CB_B16, 155, "__sanitizer_report_img2colv2_cbuf_to_cb_b16", {0, 1, 2, 3}},
        {InstrType::IMG2COLV2_CBUF_TO_CB_B8, 156, "__sanitizer_report_img2colv2_cbuf_to_cb_b8", {0, 1, 2, 3}},
        {InstrType::IMG2COLV2_CBUF_TO_CB_B32, 157, "__sanitizer_report_img2colv2_cbuf_to_cb_b32", {0, 1, 2, 3}},
        // MOV_L1_TO_UB.
        {InstrType::COPY_CBUF_TO_UBUF, 158, "__sanitizer_report_copy_cbuf_to_ubuf", {0, 1, 2}},
        // LOAD_L1_TO_L0A_3DV2.b32.
        {InstrType::IMG2COLV2_CBUF_TO_CA_B32, 422, "__sanitizer_report_img2colv2_cbuf_to_ca_b32", {0, 1, 2, 3}},
        // MOV_L1_TO_BT.f32/s32/f16/bf16.
        {InstrType::COPY_CBUF_TO_BT_F32, 423, "__sanitizer_report_copy_cbuf_to_bt_f32", {0, 1, 2}},
        {InstrType::COPY_CBUF_TO_BT_S32, 424, "__sanitizer_report_copy_cbuf_to_bt_s32", {0, 1, 2}},
        {InstrType::COPY_CBUF_TO_BT_F16, 425, "__sanitizer_report_copy_cbuf_to_bt_f16", {0, 1, 2}},
        {InstrType::COPY_CBUF_TO_BT_B16, 426, "__sanitizer_report_copy_cbuf_to_bt_b16", {0, 1, 2}},

        // FIX: FIX_L0C_TO_OUT.f32/s32. S4 output modes use the same DBI instruction classes
        // and are distinguished by quant_pre in the raw instruction configuration.
        {InstrType::COPY_MATRIX_CC_TO_GM_F32_A5, 91, "__sanitizer_report_copy_matrix_cc_to_gm_f32_a5", {0, 1, 2, 3}},
        {InstrType::COPY_MATRIX_CC_TO_GM_S32_A5, 92, "__sanitizer_report_copy_matrix_cc_to_gm_s32_a5", {0, 1, 2, 3}},
        // Local-only FIX transfers remain independently instrumented at CCE intrinsic granularity.
        {InstrType::COPY_CBUF_TO_FBUF, 167, "__sanitizer_report_copy_cbuf_to_fbuf", {0, 1, 2}},
        {InstrType::COPY_MATRIX_CC_TO_CBUF_F32, 168, "__sanitizer_report_copy_matrix_cc_to_cbuf_f32", {0, 1, 2, 3}},
        {InstrType::COPY_MATRIX_CC_TO_CBUF_S32, 169, "__sanitizer_report_copy_matrix_cc_to_cbuf_s32", {0, 1, 2, 3}},
        {InstrType::COPY_MATRIX_CC_TO_UB_F32, 170, "__sanitizer_report_copy_matrix_cc_to_ubuf_f32", {0, 1, 2, 3}},
        {InstrType::COPY_MATRIX_CC_TO_UB_S32, 171, "__sanitizer_report_copy_matrix_cc_to_ubuf_s32", {0, 1, 2, 3}},
        // SYNCCHECK: SET/WAIT FLAG, API IDs 440-443, 445-446, 456-459, and 469-474.
        {InstrType::SET_FLAG, 440, "__sanitizer_report_set_flag", {0, 1, 2}},
        {InstrType::SET_FLAGI, 441, "__sanitizer_report_set_flagi", {0, 1, 2}},
        {InstrType::WAIT_FLAG, 442, "__sanitizer_report_wait_flag", {0, 1, 2}},
        {InstrType::WAIT_FLAGI, 443, "__sanitizer_report_wait_flagi", {0, 1, 2}},
        {InstrType::SET_CROSS_CORE, 447, "__sanitizer_report_set_cross_core", {0, 1}},
        {InstrType::WAIT_FLAG_DEV, 445, "__sanitizer_report_wait_flag_dev_pipe", {0, 1}},
        {InstrType::WAIT_FLAG_DEVI, 446, "__sanitizer_report_wait_flag_devi_pipe", {0, 1}},
        {InstrType::SET_FLAG_V, 456, "__sanitizer_report_set_flag_v", {0, 1}},
        {InstrType::SET_FLAGI_V, 457, "__sanitizer_report_set_flagi_v", {0, 1}},
        {InstrType::WAIT_FLAG_V, 458, "__sanitizer_report_wait_flag_v", {0, 1}},
        {InstrType::WAIT_FLAGI_V, 459, "__sanitizer_report_wait_flagi_v", {0, 1}},
        {InstrType::WAIT_FLAG_DEV_V, 469, "__sanitizer_report_wait_flag_dev_pipe_v", {0}},
        {InstrType::WAIT_FLAG_DEVI_V, 470, "__sanitizer_report_wait_flag_devi_pipe_v", {0}},
        {InstrType::SET_CROSS_CORE_V, 468, "__sanitizer_report_set_cross_core_v", {0}},
        {InstrType::SET_INTRA_BLOCK_V, 464, "__sanitizer_report_set_intra_block_v", {0}},
        {InstrType::SET_INTRA_BLOCKI_V, 465, "__sanitizer_report_set_intra_blocki_v", {0}},
        {InstrType::WAIT_INTRA_BLOCK_V, 466, "__sanitizer_report_wait_intra_block_v", {0}},
        {InstrType::WAIT_INTRA_BLOCKI_V, 467, "__sanitizer_report_wait_intra_blocki_v", {0}},
        {InstrType::HSET_FLAG, 471, "__sanitizer_report_hset_flag", {0, 1, 2, 3, 4}},
        {InstrType::HSET_FLAGI, 472, "__sanitizer_report_hset_flagi", {0, 1, 2, 3, 4}},
        {InstrType::HWAIT_FLAG, 473, "__sanitizer_report_hwait_flag", {0, 1, 2, 3, 4}},
        {InstrType::HWAIT_FLAGI, 474, "__sanitizer_report_hwait_flagi", {0, 1, 2, 3, 4}},

        // SYNCCHECK: GET/RLS BUF, API IDs 448-451 and 460-463.
        {InstrType::GET_BUF, 448, "__sanitizer_report_get_buf", {0, 1, 2}},
        {InstrType::GET_BUFI, 449, "__sanitizer_report_get_bufi", {0, 1, 2}},
        {InstrType::RLS_BUF, 450, "__sanitizer_report_rls_buf", {0, 1, 2}},
        {InstrType::RLS_BUFI, 451, "__sanitizer_report_rls_bufi", {0, 1, 2}},
        {InstrType::GET_BUF_V, 460, "__sanitizer_report_get_buf_v", {0, 1}},
        {InstrType::GET_BUFI_V, 461, "__sanitizer_report_get_bufi_v", {0, 1}},
        {InstrType::RLS_BUF_V, 462, "__sanitizer_report_rls_buf_v", {0, 1}},
        {InstrType::RLS_BUFI_V, 463, "__sanitizer_report_rls_bufi_v", {0, 1}},

        // Reference-backed dav-3510 coverage.
        {InstrType::LOAD_CBUF_TO_CA_MX, 151, "__sanitizer_report_load_cbuf_to_ca_mx_2dv2", {0, 1, 2, 3}},
        {InstrType::LOAD_CBUF_TO_CB_MX, 152, "__sanitizer_report_load_cbuf_to_cb_mx_2dv2", {0, 1, 2, 3}},
        {InstrType::MAD_S8, 400, "__sanitizer_report_mad_s8", {0, 1, 2, 3}},
        {InstrType::MAD_F16_F32, 401, "__sanitizer_report_mad_f16_f32", {0, 1, 2, 3}},
        {InstrType::MAD_BF16_F32, 402, "__sanitizer_report_mad_bf16_f32", {0, 1, 2, 3}},
        {InstrType::MAD_F32_F32, 403, "__sanitizer_report_mad_f32_f32", {0, 1, 2, 3}},
        {InstrType::MAD_E4M3_E4M3, 404, "__sanitizer_report_mad_e4m3_e4m3", {0, 1, 2, 3}},
        {InstrType::MAD_E4M3_E5M2, 405, "__sanitizer_report_mad_e4m3_e5m2", {0, 1, 2, 3}},
        {InstrType::MAD_E5M2_E4M3, 406, "__sanitizer_report_mad_e5m2_e4m3", {0, 1, 2, 3}},
        {InstrType::MAD_E5M2_E5M2, 407, "__sanitizer_report_mad_e5m2_e5m2", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E1M2X2_E1M2X2, 408, "__sanitizer_report_mad_mx_e1m2_e1m2", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E1M2X2_E2M1X2, 409, "__sanitizer_report_mad_mx_e1m2_e2m1", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E2M1X2_E1M2X2, 410, "__sanitizer_report_mad_mx_e2m1_e1m2", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E2M1X2_E2M1X2, 411, "__sanitizer_report_mad_mx_e2m1_e2m1", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E4M3_E4M3, 412, "__sanitizer_report_mad_mx_e4m3_e4m3", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E4M3_E5M2, 413, "__sanitizer_report_mad_mx_e4m3_e5m2", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E5M2_E4M3, 414, "__sanitizer_report_mad_mx_e5m2_e4m3", {0, 1, 2, 3}},
        {InstrType::MAD_MX_E5M2_E5M2, 415, "__sanitizer_report_mad_mx_e5m2_e5m2", {0, 1, 2, 3}},
        {InstrType::COPY_UBUF_TO_UBUF, 174, "__sanitizer_report_copy_ubuf_to_ubuf", {0, 1, 2}},
        {InstrType::SCATTER_VNCHWCONV_B16, 175, "__sanitizer_report_scatter_vnchwconv_b16", {0, 1, 2}},
        {InstrType::SCATTER_VNCHWCONV_B32, 176, "__sanitizer_report_scatter_vnchwconv_b32", {0, 1, 2}},
        {InstrType::SCATTER_VNCHWCONV_B8, 177, "__sanitizer_report_scatter_vnchwconv_b8", {0, 1, 2, 3, 4}},
        {InstrType::VTRANSPOSE_B16, 178, "__sanitizer_report_vtranspose", {0, 1}},
        {InstrType::LDVA, 417, "__sanitizer_report_ldva", {0, 1, 2}},
        {InstrType::VBS32_F16, 418, "__sanitizer_report_vbs32_f16", {0, 1, 2, 3}},
        {InstrType::VBS32_F32, 419, "__sanitizer_report_vbs32_f32", {0, 1, 2, 3}},
        {InstrType::VMRGSORT_F16, 420, "__sanitizer_report_vmrgsort4_f16", {0, 1, 2, 3}},
        {InstrType::VMRGSORT_F32, 421, "__sanitizer_report_vmrgsort4_f32", {0, 1, 2, 3}},
        // A5 scalar LD/ST instructions share the scalar GM address ABI.
        {InstrType::ST_B64_IMM, 24, "__sanitizer_report_st_b64_imm", {1, 2, 3}},
        {InstrType::ST_B32_IMM, 25, "__sanitizer_report_st_b32_imm", {1, 2, 3}},
        {InstrType::ST_B16_IMM, 26, "__sanitizer_report_st_b16_imm", {1, 2, 3}},
        {InstrType::ST_B8_IMM, 27, "__sanitizer_report_st_b8_imm", {1, 2, 3}},
        {InstrType::ST_B64_REG, 28, "__sanitizer_report_st_b64_reg", {1, 2, 3}},
        {InstrType::ST_B32_REG, 29, "__sanitizer_report_st_b32_reg", {1, 2, 3}},
        {InstrType::ST_B16_REG, 30, "__sanitizer_report_st_b16_reg", {1, 2, 3}},
        {InstrType::ST_B8_REG, 31, "__sanitizer_report_st_b8_reg", {1, 2, 3}},
        {InstrType::STP_B64, 32, "__sanitizer_report_stp_b64", {2, 3}},
        {InstrType::STP_B32, 33, "__sanitizer_report_stp_b32", {2, 3}},
        {InstrType::STP_B16, 34, "__sanitizer_report_stp_b16", {2, 3}},
        {InstrType::STP_B8, 35, "__sanitizer_report_stp_b8", {2, 3}},
        {InstrType::STI_B64_IMM, 36, "__sanitizer_report_sti_b64_imm", {1, 2, 3}},
        {InstrType::STI_B32_IMM, 37, "__sanitizer_report_sti_b32_imm", {1, 2, 3}},
        {InstrType::STI_B16_IMM, 38, "__sanitizer_report_sti_b16_imm", {1, 2, 3}},
        {InstrType::STI_B8_IMM, 39, "__sanitizer_report_sti_b8_imm", {1, 2, 3}},
        {InstrType::STI_B64_REG, 40, "__sanitizer_report_sti_b64_reg", {1, 2, 3}},
        {InstrType::STI_B32_REG, 41, "__sanitizer_report_sti_b32_reg", {1, 2, 3}},
        {InstrType::STI_B16_REG, 42, "__sanitizer_report_sti_b16_reg", {1, 2, 3}},
        {InstrType::STI_B8_REG, 43, "__sanitizer_report_sti_b8_reg", {1, 2, 3}},
        {InstrType::LD_B64_IMM, 44, "__sanitizer_report_ld_b64_imm", {1, 2, 3}},
        {InstrType::LD_B32_IMM, 45, "__sanitizer_report_ld_b32_imm", {1, 2, 3}},
        {InstrType::LD_B16_IMM, 46, "__sanitizer_report_ld_b16_imm", {1, 2, 3}},
        {InstrType::LD_B8_IMM, 47, "__sanitizer_report_ld_b8_imm", {1, 2, 3}},
        {InstrType::LD_B64_REG, 48, "__sanitizer_report_ld_b64_reg", {1, 2, 3}},
        {InstrType::LD_B32_REG, 49, "__sanitizer_report_ld_b32_reg", {1, 2, 3}},
        {InstrType::LD_B16_REG, 50, "__sanitizer_report_ld_b16_reg", {1, 2, 3}},
        {InstrType::LD_B8_REG, 51, "__sanitizer_report_ld_b8_reg", {1, 2, 3}},
        {InstrType::LDP_B64, 52, "__sanitizer_report_ldp_b64", {2, 3}},
        {InstrType::LDP_B32, 53, "__sanitizer_report_ldp_b32", {2, 3}},
        {InstrType::LDP_B16, 54, "__sanitizer_report_ldp_b16", {2, 3}},
        {InstrType::LDP_B8, 55, "__sanitizer_report_ldp_b8", {2, 3}},
        {InstrType::ST_ATOMIC_B8, 58, "__sanitizer_report_st_atomic_b8", {1, 2, 3}},
        {InstrType::ST_ATOMIC_B32, 56, "__sanitizer_report_st_atomic_b32", {1, 2, 3}},
        {InstrType::ST_ATOMIC_B16, 57, "__sanitizer_report_st_atomic_b16", {1, 2, 3}},
        {InstrType::STI_ATOMIC_B32, 59, "__sanitizer_report_sti_atomic_b32", {1, 2, 3}},
        {InstrType::STI_ATOMIC_B16, 60, "__sanitizer_report_sti_atomic_b16", {1, 2, 3}},
        {InstrType::STI_ATOMIC_B8, 61, "__sanitizer_report_sti_atomic_b8", {1, 2, 3}},
        {InstrType::ST_DEV_B64, 64, "__sanitizer_report_st_dev_b64", {1, 2}},
        {InstrType::ST_DEV_B32, 65, "__sanitizer_report_st_dev_b32", {1, 2}},
        {InstrType::ST_DEV_B16, 66, "__sanitizer_report_st_dev_b16", {1, 2}},
        {InstrType::ST_DEV_B8, 67, "__sanitizer_report_st_dev_b8", {1, 2}},
        {InstrType::LD_DEV_B64, 68, "__sanitizer_report_ld_dev_b64", {1, 2}},
        {InstrType::LD_DEV_B32, 69, "__sanitizer_report_ld_dev_b32", {1, 2}},
        {InstrType::LD_DEV_B16, 70, "__sanitizer_report_ld_dev_b16", {1, 2}},
        {InstrType::LD_DEV_B8, 71, "__sanitizer_report_ld_dev_b8", {1, 2}},
        {InstrType::DC_PRELOAD, 62, "__sanitizer_report_dc_preload", {0, 1}},
        {InstrType::DC_PRELOADI, 63, "__sanitizer_report_dc_preloadi", {0, 1}},
        {InstrType::CHANNEL_PARA, 123, "__sanitizer_report_set_channel_para", {0}},
        {InstrType::SET_FFTS_BASE_ADDR, 160, "__sanitizer_report_set_ffts_base_addr", {0}},
        {InstrType::SET_FPC, 161, "__sanitizer_report_set_fpc", {0}},
        {InstrType::SET_QUANT_PRE, 162, "__sanitizer_report_set_quant_pre", {0}},
        {InstrType::SET_QUANT_POST, 163, "__sanitizer_report_set_quant_post", {0}},
        {InstrType::SET_LRELU_ALPHA, 164, "__sanitizer_report_set_lrelu_alpha", {0}},
        {InstrType::SET_FMATRIX, 386, "__sanitizer_report_set_fmatrix", {0}},
        {InstrType::SET_FMATRIX_B, 387, "__sanitizer_report_set_fmatrix_b", {0}},
        {InstrType::SET_L3D_RPT, 390, "__sanitizer_report_set_l3d_rpt", {0}},
        {InstrType::SET_L3D_RPT_B, 391, "__sanitizer_report_set_l3d_rpt_b", {0}},
        {InstrType::SET_PADDING_B, 393, "__sanitizer_report_set_padding_b", {0}},
        {InstrType::PIPE_BARRIER, 439, "__sanitizer_report_pipe_barrier", {0}},
        {InstrType::SET_INTRA_BLOCK, 452, "__sanitizer_report_set_intra_block", {0, 1}},
        {InstrType::SET_INTRA_BLOCKI, 453, "__sanitizer_report_set_intra_blocki", {0, 1}},
        {InstrType::WAIT_INTRA_BLOCK, 454, "__sanitizer_report_wait_intra_block", {0, 1}},
        {InstrType::WAIT_INTRA_BLOCKI, 455, "__sanitizer_report_wait_intra_blocki", {0, 1}},
    };
    return bindings;
}

bool ValidateBindings(const std::vector<aclsan::ProbeGroup>& groups, std::string& diagnostic)
{
    std::set<uint16_t> apiIds;
    std::set<std::string> symbols;
    for (const auto& binding : AllBindings()) {
        if (std::find(groups.begin(), groups.end(), BindingGroup(binding)) == groups.end()) {
            continue;
        }
        const uint16_t enumId = static_cast<uint16_t>(binding.instrType);
        if (enumId != binding.apiId) {
            diagnostic = "binding ID mismatch for " + std::string(binding.stubName);
            return false;
        }
        if (!apiIds.insert(binding.apiId).second) {
            diagnostic = "duplicate instruction ID " + std::to_string(binding.apiId);
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
    for (const auto& binding : AllBindings()) {
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
    for (const auto& binding : AllBindings()) {
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
    for (const auto& binding : AllBindings()) {
        append(std::to_string(static_cast<uint16_t>(binding.instrType)));
        append(std::to_string(binding.apiId));
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
