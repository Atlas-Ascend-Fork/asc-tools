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

if [[ $# -ne 1 ]]; then
    printf 'usage: %s <libacl_san.so>\n' "${0##*/}" >&2
    exit 2
fi

library=$1
expected_symbols=(
    ACLSAN_1.0
    aclsanEnableCallback@@ACLSAN_1.0
    aclsanEnableDomain@@ACLSAN_1.0
    aclsanGetCallbackState@@ACLSAN_1.0
    aclsanGetDeviceCallStack@@ACLSAN_1.0
    aclsanSubscribe@@ACLSAN_1.0
    aclsanUnsubscribe@@ACLSAN_1.0
)
mapfile -t actual_symbols < <(
    nm -D --defined-only --format=posix "${library}" |
        awk '{print $1}' |
        LC_ALL=C sort
)

if [[ "${actual_symbols[*]}" != "${expected_symbols[*]}" ]]; then
    printf 'unexpected dynamic exports in %s\n' "${library}" >&2
    printf 'expected:\n' >&2
    printf '  %s\n' "${expected_symbols[@]}" >&2
    printf 'actual:\n' >&2
    printf '  %s\n' "${actual_symbols[@]}" >&2
    exit 1
fi

# Consume readelf output fully to avoid SIGPIPE under pipefail.
readelf --version-info "${library}" | grep -F 'Name: ACLSAN_1.0' > /dev/null
