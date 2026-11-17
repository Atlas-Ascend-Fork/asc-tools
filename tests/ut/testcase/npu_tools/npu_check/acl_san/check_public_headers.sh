#!/usr/bin/env bash
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.

set -euo pipefail

if [[ $# -ne 3 ]]; then
    printf 'usage: %s <include-dir> <c-compiler> <cxx-compiler>\n' "${0##*/}" >&2
    exit 2
fi

include_dir=$1
c_compiler=$2
cxx_compiler=$3

for header in "${include_dir}"/acl_san/*.h; do
    for language in c c++; do
        compiler=${c_compiler}
        standard=c11
        if [[ ${language} == c++ ]]; then
            compiler=${cxx_compiler}
            standard=c++17
        fi
        printf 'Checking %s (%s)\n' "${header##*/}" "${standard}"
        printf '#include "acl_san/%s"\n' "${header##*/}" |
            "${compiler}" -x "${language}" -std="${standard}" -Wall -Wextra -Werror \
                -fsyntax-only -I "${include_dir}" -
    done
done
