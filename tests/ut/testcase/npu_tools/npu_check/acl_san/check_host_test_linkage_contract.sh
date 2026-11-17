#!/usr/bin/env bash
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.

set -euo pipefail

readelf_command=${1:?readelf executable is required}
sanitizer_library=${2:?test sanitizer library is required}
shift 2
[[ $# -gt 0 ]] || { printf 'host test executables are required\n' >&2; exit 1; }

CheckDependencies()
{
    local artifact=$1
    local required=$2
    local dynamic
    dynamic=$("${readelf_command}" -d "${artifact}")
    if grep -Eq '\(NEEDED\).*\[(libacl_san\.so|libacl_tool_injection\.so|libacl_rt\.so)\]' <<< "${dynamic}"; then
        printf 'host test linkage contract failed: %s links a production sanitizer or Runtime entry library\n' \
            "${artifact}" >&2
        return 1
    fi
    if ! grep -Fq "[${required}]" <<< "${dynamic}"; then
        printf 'host test linkage contract failed: %s does not depend on %s\n' "${artifact}" "${required}" >&2
        return 1
    fi
}

CheckDependencies "${sanitizer_library}" libacl_tool_injection_test.so
for executable in "$@"; do
    CheckDependencies "${executable}" libacl_san_test.so
done
printf 'host test linkage contract passed\n'
