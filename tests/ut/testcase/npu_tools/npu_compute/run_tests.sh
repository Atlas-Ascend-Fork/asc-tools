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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../../.." && pwd -P)"
BUILD_DIR="${NPU_COMPUTE_BUILD_DIR:-/tmp/asc_tools_npu_compute_integration}"
CONFIG=Release
JOBS=8
CANN_PATH="${ASCEND_HOME_PATH:-}"
while (($#)); do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --config) CONFIG="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --cann-path) CANN_PATH="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: bash $0 [--build-dir DIR] [--config CONFIG] [--jobs N] [--cann-path DIR]"
            echo "Build and run native C++ tests."
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
RUN_SCRIPT="$BUILD_DIR/tests/ut/testcase/npu_tools/run_npu_compute_native_tests-$CONFIG.cmake"
RUN_UNIFIED=ON
OPTIONS=(-DBUILD_OPEN_PROJECT=ON -DENABLE_TEST=ON -DTEST_MOD=cpp
    -DENABLE_BUILD_DEVICE=ON "-DCMAKE_BUILD_TYPE=$CONFIG")
if [[ -n "$CANN_PATH" ]]; then
    OPTIONS+=("-DASCEND_CANN_PACKAGE_PATH=$CANN_PATH")
fi
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" "${OPTIONS[@]}"
UNIFIED_TEST="$(cmake -DNPU_COMPUTE_PRINT_UNIFIED=ON -P "$RUN_SCRIPT")"
UNIFIED_BEFORE="$(stat -Lc '%i:%y:%s' "$UNIFIED_TEST" 2>/dev/null || true)"
cmake --build "$BUILD_DIR" --config "$CONFIG" --target npu_compute_test_artifacts -j "$JOBS"

# A newly linked unified LLT already ran through its original POST_BUILD.
# Otherwise run it explicitly so this invocation still tests it once.
UNIFIED_AFTER="$(stat -Lc '%i:%y:%s' "$UNIFIED_TEST")"
POST_BUILD="$(sed -n 's/^NPU_TOOLS_LLT_RUN_POST_BUILD:BOOL=//p' "$BUILD_DIR/CMakeCache.txt")"
case "$POST_BUILD" in
    ON|TRUE|YES|Y|1)
        if [[ "$UNIFIED_AFTER" != "$UNIFIED_BEFORE" ]]; then RUN_UNIFIED=OFF; fi ;;
esac
cmake "-DNPU_COMPUTE_RUN_UNIFIED=$RUN_UNIFIED" -P "$RUN_SCRIPT"
