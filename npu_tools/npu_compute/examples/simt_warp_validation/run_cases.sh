#!/usr/bin/env bash
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
set -u
set -o pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf 'Usage: bash run_cases.sh /absolute/path/to/npu-compute [01..10|all]\n' >&2
    exit 2
fi
tool=$1
selection=${2:-all}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
case "$tool" in /*) ;; *) printf 'Please provide an absolute npu-compute path.\n' >&2; exit 2;; esac
if [ ! -x "$tool" ]; then printf 'npu-compute is not executable: %s\n' "$tool" >&2; exit 2; fi
case "$selection" in
    all) cases='01 02 03 04 05 06 07 08 09 10' ;;
    01|02|03|04|05|06|07|08|09|10) cases=$selection ;;
    *) printf 'Case must be 01..10 or all.\n' >&2; exit 2 ;;
esac
if [ -z "${NPU_COMPUTE_NPUBIT_PLATFORM:-}" ]; then
    printf 'Missing environment variable: NPU_COMPUTE_NPUBIT_PLATFORM\n' >&2
    exit 2
fi
if [ "$NPU_COMPUTE_NPUBIT_PLATFORM" != V100 ]; then
    if [ -z "${NPU_COMPUTE_NPUBIT_DIE_COUNT:-}" ]; then
        printf 'Missing environment variable: NPU_COMPUTE_NPUBIT_DIE_COUNT\n' >&2
        exit 2
    fi
    if [ -z "${NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE:-}" ] && [ -z "${NPU_COMPUTE_NPUBIT_AICORE_PER_DIE:-}" ]; then
        printf 'Missing NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE (or legacy AICORE_PER_DIE).\n' >&2
        exit 2
    fi
fi
for id in $cases; do
    if [ ! -x "$root/build/simt_case_$id" ]; then
        printf 'Missing executable: %s/build/simt_case_%s\n' "$root" "$id" >&2
        exit 2
    fi
done

run_dir=$(mktemp -d "${TMPDIR:-/tmp}/simtwarp-cases-XXXXXXXX") || exit 2
printf 'Results directory: %s\n' "$run_dir"
for id in $cases; do
    case_dir="$run_dir/case_$id"
    mkdir -p "$case_dir/extracted" || exit 2
    printf '\n[case %s] baseline: no profiling\n' "$id"
    "$root/build/simt_case_$id" >"$case_dir/baseline.log" 2>&1
    baseline_status=$?
    cat "$case_dir/baseline.log"
    if [ "$baseline_status" -ne 0 ] || ! grep -Fq "[case $id] PASS" "$case_dir/baseline.log"; then
        printf '[case %s] FAIL: baseline (status=%s); stopped.\n' "$id" "$baseline_status"
        exit 1
    fi
    printf '[case %s] profiling\n' "$id"
    env -u NPU_COMPUTE_REPLAY_ORIGINAL_TEST "$tool" --section occupancy \
        -o "$case_dir/result.npu-rep" -- "$root/build/simt_case_$id" >"$case_dir/collect.log" 2>&1
    collect_status=$?
    cat "$case_dir/collect.log"
    if [ "$collect_status" -ne 0 ] || ! grep -Fq "[case $id] PASS" "$case_dir/collect.log" || [ ! -f "$case_dir/result.npu-rep" ]; then
        printf '[case %s] FAIL: collection (status=%s); stopped. See %s/collect.log\n' "$id" "$collect_status" "$case_dir"
        exit 1
    fi
    "$tool" --import "$case_dir/result.npu-rep" --export "$case_dir/extracted" >"$case_dir/import.log" 2>&1
    import_status=$?
    cat "$case_dir/import.log"
    if [ "$import_status" -ne 0 ]; then printf '[case %s] FAIL: import; stopped.\n' "$id"; exit 1; fi
    if ! python3 "$root/check_trace.py" "$case_dir/extracted"; then
        printf '[case %s] FAIL: trace validation; stopped.\n' "$id"
        exit 1
    fi
    printf '[case %s] PASS: baseline, collection and trace shape\n' "$id"
done
printf '\nAll selected cases passed basic checks. Results: %s\n' "$run_dir"
