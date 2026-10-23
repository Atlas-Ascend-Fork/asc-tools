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

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../../../.." && pwd)
api_dir="${repo_root}/npu_tools/npu_check/src/acl_san"
translator="${api_dir}/aclsan_translate_device_data.cpp"

Fail()
{
    printf 'device memory access producer contract failed: %s\n' "$1" >&2
    exit 1
}

if rg -n '\bMemoryAccessEndpoint\b|\bDATA_COPY_ACCESS_COUNT\b' "${translator}"; then
    Fail 'producer must build and size the complete access list'
fi

if rg -n 'MakeDeviceMemoryAccessData|ConfigureMemoryAccessLayout|useExactGmConverter' "${translator}"; then
    Fail 'legacy device-memory producer paths must not remain beside MemoryFieldToCbdataConverter'
fi
grep -Fq 'MemoryFieldToCbdataConverter{context, registerState}.Convert(MemoryInstructionField{params})' "${translator}" || \
    Fail 'memory instructions do not use the unified converter and independent register state'
if grep -Fq 'MakeMovAlignV2MemoryAccessData' "${translator}"; then
    Fail 'converter-covered MOV_ALIGN_V2 helper must be removed'
fi
converter="${api_dir}/aclsan_memory_cbdata.cpp"
grep -Fq 'data.accessIndex = sequence.nextIndex++;' "${converter}" || Fail 'accessIndex is not derived from the owner sequence'
grep -Fq 'data.accessCount = sequence.count;' "${converter}" || Fail 'accessCount is not derived from the owner access count'

printf 'device memory access producer contract passed\n'
