#!/bin/bash
# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------

set -euo pipefail

WORK_DIR="./third_party"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --3rd_party|--dest_dir|-d)
            if [[ $# -lt 2 || -z "$2" || "$2" == --* ]]; then
                echo "Error: --3rd_party requires a directory." >&2
                exit 1
            fi
            WORK_DIR="$2"
            shift 2
            ;;
        --3rd_party=*|--dest_dir=*)
            WORK_DIR="${1#*=}"
            if [[ -z "${WORK_DIR}" ]]; then
                echo "Error: --3rd_party requires a directory." >&2
                exit 1
            fi
            shift
            ;;
        -h|--help)
            echo "Usage: bash install_dep_tar.sh [--dest_dir <directory>] (alias: --3rd_party, -d)"
            echo "Default: ./third_party; all patches are saved in <directory>/patch."
            exit 0
            ;;
        *)
            echo "Error: unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

OBS_URL="https://cann-3rd.obs.cn-north-4.myhuaweicloud.com"
# Source: https://gitcode.com/cann/cmake/tree/master-059/third_party
# Tag commit: 3c4a9da4a0e81faa47dcc1c8e11db63ec7ff6231
# Format: "URL SHA256 Path relative to third_party"
DOWNLOAD_LIST=(
    "${OBS_URL}/boost/boost_1_87_0.tar.gz f55c340aa49763b1925ccf02b2e83f35fdcf634c9d5164a2acb87540173c741d boost_1_87_0.tar.gz"
    "${OBS_URL}/googletest/googletest-1.14.0.tar.gz 8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7 googletest-1.14.0.tar.gz"
    "${OBS_URL}/mockcpp/mockcpp-2.7.tar.gz 73ab0a8b6d1052361c2cebd85e022c0396f928d2e077bf132790ae3be766f603 mockcpp-2.7.tar.gz"
    "https://gitcode.com/cann-src-third-party/mockcpp/releases/download/v2.7-h3/mockcpp-2.7_py3-h3.patch 30f78d8173d50fa9af36efbc683aee82bcd5afc7acdc4dbef7381b92a1b4c800 patch/mockcpp-2.7_py3-h3.patch"
    "${OBS_URL}/makeself/makeself-release-2.5.0.tar.gz 705d0376db9109a8ef1d4f3876c9997ee6bed454a23619e1dbc03d25033e46ea makeself-release-2.5.0.tar.gz"
    "${OBS_URL}/makeself/fix/makeself-2.5.0.patch 0ecac3fe200221353322626900d5257a59d98347f83653b98456e08475994486 patch/makeself-2.5.0.patch"
    "https://raw.gitcode.com/cann/cmake/archive/refs/heads/master-059.tar.gz 9e59939b5c97c9498293969df22ab2dd40e450700c51f84f117d065880f40d25 cmake-master-059.tar.gz"
)

TEMP_FILE=""
trap 'if [[ -n "${TEMP_FILE}" ]]; then rm -f -- "${TEMP_FILE}"; fi' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for TOOL in wget sha256sum; do
    if ! command -v "${TOOL}" >/dev/null 2>&1; then
        echo "Error: ${TOOL} is required." >&2
        exit 1
    fi
done

check_sha256() {
    printf '%s  %s\n' "$1" "$2" | sha256sum --check --status
}

for ITEM in "${DOWNLOAD_LIST[@]}"; do
    read -r URL SHA256 FILE_NAME <<< "${ITEM}"
    FILE_PATH="${WORK_DIR}/${FILE_NAME}"

    if [[ -f "${FILE_PATH}" ]]; then
        if check_sha256 "${SHA256}" "${FILE_PATH}"; then
            echo "Verified cache, skipping: ${FILE_NAME}"
            continue
        fi
        echo "Cached SHA256 mismatch, downloading again: ${FILE_NAME}"
        rm -f -- "${FILE_PATH}"
    fi

    mkdir -p -- "$(dirname "${FILE_PATH}")"
    TEMP_FILE=$(mktemp -- "${FILE_PATH}.part.XXXXXX")
    echo "Download from ${URL} to ${FILE_PATH}"
    if ! wget -q --show-progress --timeout=60 --tries=3 -O "${TEMP_FILE}" "${URL}"; then
        echo "Error: download failed: ${FILE_NAME}" >&2
        exit 1
    fi
    if ! check_sha256 "${SHA256}" "${TEMP_FILE}"; then
        echo "Error: SHA256 mismatch: ${FILE_NAME}" >&2
        exit 1
    fi
    mv -f -- "${TEMP_FILE}" "${FILE_PATH}"
    TEMP_FILE=""
    echo "Downloaded and verified: ${FILE_NAME}"
done

echo "All downloads verified. Files are saved in '${WORK_DIR}'."
