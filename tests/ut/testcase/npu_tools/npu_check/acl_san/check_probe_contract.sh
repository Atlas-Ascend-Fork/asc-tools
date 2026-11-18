#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------

set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../.." && pwd)
api_dir="${repo_root}/npu_tools/npu_check/src/acl_san"
dbi_source_dir="${api_dir}/dbi"

Fail()
{
    printf 'probe contract check failed: %s\n' "$1" >&2
    exit 1
}

binding_source="${dbi_source_dir}/dynamic_bind.cpp"
probe_generator="${dbi_source_dir}/probe_source_generator.cpp"
ctrlbin_bindings="${dbi_source_dir}/ctrlbin/ctrlbin_bindings.h"
# Compare complete tuples independently of line wrapping and named argument constants.
binding_catalog=$(tr -d '[:space:]' < "${binding_source}" | sed -E 's/PROBE_ARGUMENT_([0-4])/\1/g')
probe_catalog=$(tr -d '[:space:]' < "${probe_generator}")
# Numeric IDs now live in InstrType rather than being duplicated in BindingSpec.
instruction_ids=$(awk '
    /enum class InstrType/ { in_enum = 1; next }
    in_enum && /};/ { exit }
    in_enum {
        sub(/\/\/.*/, "")
        gsub(/[[:space:],]/, "")
        if ($0 == "") next
        if ($0 !~ /^[A-Z][A-Z0-9_]*(=[0-9]+)?$/) exit 1
        if (index($0, "=")) {
            split($0, fields, "=")
            name = fields[1]
            value = fields[2] + 0
        } else {
            name = $0
        }
        printf "%s=%d\n", name, value++
    }
' "${ctrlbin_bindings}") || Fail 'cannot read instruction IDs from InstrType'

CheckBinding()
{
    local type=$1 id=$2 symbol=$3 mask=$4
    grep -Fxq "${type}=${id}" <<< "${instruction_ids}" || Fail "incorrect instruction ID for ${type}"
    grep -Fq "{InstrType::${type},\"${symbol}\",{${mask}}}" <<< "${binding_catalog}" || \
        Fail "incorrect binding symbol or argument mask for ${type}"
}

CheckProbe()
{
    local type=$1 group=$2 symbol=$3
    grep -Fq "{static_cast<uint16_t>(InstrType::${type}),ProbeGroup::${group},\"${symbol}\"," \
        <<< "${probe_catalog}" || Fail "incorrect probe group or symbol for ${type}"
}

scalar_dev_id=64
for scalar_dev_op in st ld; do
    for scalar_dev_bits in 64 32 16 8; do
        scalar_dev_type="${scalar_dev_op^^}_DEV_B${scalar_dev_bits}"
        scalar_dev_symbol="__npu_check_probe_${scalar_dev_op}_dev_b${scalar_dev_bits}"
        CheckBinding "${scalar_dev_type}" "${scalar_dev_id}" "${scalar_dev_symbol}" '1,2'
        CheckProbe "${scalar_dev_type}" Scalar "${scalar_dev_symbol}"
        scalar_dev_id=$((scalar_dev_id + 1))
    done
done

scalar_atomic_id=56
for scalar_atomic_spec in 'ST_ATOMIC_B32:st_atomic_b32' 'ST_ATOMIC_B16:st_atomic_b16' 'ST_ATOMIC_B8:st_atomic_b8' \
    'STI_ATOMIC_B32:sti_atomic_b32' 'STI_ATOMIC_B16:sti_atomic_b16' 'STI_ATOMIC_B8:sti_atomic_b8'; do
    scalar_atomic_type="${scalar_atomic_spec%%:*}"
    scalar_atomic_symbol="__npu_check_probe_${scalar_atomic_spec##*:}"
    CheckBinding "${scalar_atomic_type}" "${scalar_atomic_id}" "${scalar_atomic_symbol}" '1,2,3'
    CheckProbe "${scalar_atomic_type}" Scalar "${scalar_atomic_symbol}"
    scalar_atomic_id=$((scalar_atomic_id + 1))
done

CheckBinding SET_PADDING 392 __npu_check_probe_set_padding 0
grep -Eq '\{static_cast<uint16_t>\(InstrType::SET_PADDING\),ProbeGroup::Scalar,"__npu_check_probe_set_padding",[^}]*"RegisterState","PIPE_S",R"ARGS\(value,0UL,0UL,0UL,0UL\)ARGS"\}' \
    <<< "${probe_catalog}" || Fail 'SET_PADDING must record PIPE_S and preserve value in raw argument 0'
CheckBinding SET_L1_2D_B16 149 __npu_check_probe_set_l1_2d_b16 '0,1'
CheckProbe SET_L1_2D_B16 Mte2 __npu_check_probe_set_l1_2d_b16
CheckBinding SET_L1_2D_B32 150 __npu_check_probe_set_l1_2d_b32 '0,1'
CheckProbe SET_L1_2D_B32 Mte2 __npu_check_probe_set_l1_2d_b32
required_scalar_probes=(
    'LOOP3_PARA|90|__npu_check_probe_set_loop3_para'
    'LOOP_SIZE_UBTOOUT|125|__npu_check_probe_set_loop_size_ubtoout'
    'LOOP1_STRIDE_UBTOOUT|126|__npu_check_probe_set_loop1_stride_ubtoout'
    'LOOP2_STRIDE_UBTOOUT|127|__npu_check_probe_set_loop2_stride_ubtoout'
    'LOOP_SIZE_OUTTOUB|128|__npu_check_probe_set_loop_size_outtoub'
    'LOOP1_STRIDE_OUTTOUB|129|__npu_check_probe_set_loop1_stride_outtoub'
    'LOOP2_STRIDE_OUTTOUB|130|__npu_check_probe_set_loop2_stride_outtoub'
    'PAD_CNT_NDDMA|131|__npu_check_probe_set_pad_cnt_nddma'
    'SET_LOOP_SIZE_OUTTOL1|394|__npu_check_probe_set_loop_size_outtol1'
    'SET_LOOP1_STRIDE_OUTTOL1|395|__npu_check_probe_set_loop1_stride_outtol1'
    'SET_LOOP2_STRIDE_OUTTOL1|396|__npu_check_probe_set_loop2_stride_outtol1'
)
for specification in "${required_scalar_probes[@]}"; do
    IFS='|' read -r instruction_type instruction_id probe_symbol <<< "${specification}"
    CheckBinding "${instruction_type}" "${instruction_id}" "${probe_symbol}" 0
    CheckProbe "${instruction_type}" Scalar "${probe_symbol}"
done
CheckBinding MAD_S8 400 __npu_check_probe_mad_s8 '0,1,2,3'
CheckProbe MAD_S8 Matrix __npu_check_probe_mad_s8
CheckBinding SCATTER_VNCHWCONV_B8 177 __npu_check_probe_scatter_vnchwconv_b8 '0,1,2,3,4'
CheckProbe SCATTER_VNCHWCONV_B8 Vector __npu_check_probe_scatter_vnchwconv_b8
CheckBinding PIPE_BARRIER 439 __npu_check_probe_pipe_barrier 0
CheckProbe PIPE_BARRIER Sync __npu_check_probe_pipe_barrier

printf 'probe binding contract check passed\n'
