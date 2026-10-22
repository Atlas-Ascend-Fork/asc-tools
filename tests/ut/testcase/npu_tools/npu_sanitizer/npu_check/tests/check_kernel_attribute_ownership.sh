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

repo_root=${1:?"repository root is required"}
launch_header="${repo_root}/npu_tools/npu_check/include/acl_san/aclsan_cbdata_launch.h"
sanitizer_hook="${repo_root}/npu_tools/npu_check/src/acl_san/aclsan_hook_aclrt.cpp"
attribute_source="${repo_root}/npu_tools/npu_check/src/processor/tool_manager/kernel_attributes.cpp"
attribute_header="${repo_root}/npu_tools/npu_check/src/processor/tool_manager/kernel_attributes.h"
tool_manager="${repo_root}/npu_tools/npu_check/src/processor/tool_manager/tool_manager.cpp"
sync_checker="${repo_root}/npu_tools/npu_check/src/processor/checker/synccheck_callbacks.cpp"
mem_checker="${repo_root}/npu_tools/npu_check/src/processor/checker/memcheck_callbacks.cpp"

Fail()
{
    printf 'kernel attribute ownership contract failed: %s\n' "$1" >&2
    exit 1
}

if rg -n 'kernelType|aicRatio|aivRatio|kernelSchedMode' "${launch_header}"; then
    Fail "AclsanLaunchData must not expose kernel attributes"
fi

if rg -n 'ACL_FUNC_ATTR_KERNEL_(TYPE|RATIO|SCHED_MODE)' "${sanitizer_hook}"; then
    Fail "libacl_san.so must not query kernel attributes"
fi

for attribute in ACL_FUNC_ATTR_KERNEL_TYPE ACL_FUNC_ATTR_KERNEL_RATIO ACL_FUNC_ATTR_KERNEL_SCHED_MODE; do
    rg -q "${attribute}" "${attribute_source}" || Fail "libnpu_check_processor.so does not query ${attribute}"
done

if rg -q 'FunctionAttributeGetter' "${attribute_header}"; then
    Fail "kernel attribute query must not expose an injectable getter overload"
fi

query_definition_count=$(rg -c '^KernelAttributes QueryKernelAttributes' "${attribute_source}")
if [[ "${query_definition_count}" -ne 1 ]]; then
    Fail "kernel attribute query must have exactly one implementation"
fi

if rg -q 'kCommonCallbacks' "${tool_manager}"; then
    Fail "launch callback must not be common to all tools"
fi

synccheck_callbacks=$(sed -n '/Synccheck::GetSubscribedID/,/return callbacks/p' "${sync_checker}")
if ! rg -q 'ACLSAN_CB_DOMAIN_LAUNCH.*ACLSAN_CBID_LAUNCH_KERNEL' <<< "${synccheck_callbacks}"; then
    Fail "Synccheck callbacks do not subscribe to the launch callback"
fi

memcheck_callbacks=$(sed -n '/Memcheck::GetSubscribedID/,/return callbacks/p' "${mem_checker}")
# Memcheck observes launch failures to avoid reporting an incomplete run as clean.
if ! rg -q 'ACLSAN_CB_DOMAIN_LAUNCH.*ACLSAN_CBID_LAUNCH_KERNEL' <<< "${memcheck_callbacks}"; then
    Fail "Memcheck callbacks must subscribe to launch results"
fi
if rg -q 'QueryKernelAttributes' "${mem_checker}"; then
    Fail "Memcheck must not query kernel attributes"
fi

rg -q 'QueryKernelAttributes' "${sync_checker}" || Fail "Synccheck does not query kernel attributes"

callback_body=$(sed -n '/^void ToolManager::OnCallback(/,/^void ToolManager::OnCallbackException/p' "${tool_manager}")
if rg -q 'std::unique_lock<std::mutex> stateLock\(stateMutex_\)' <<< "${callback_body}"; then
    Fail "ToolManager callbacks must not use an explicitly unlockable state lock"
fi

if ! rg -q 'std::lock_guard<std::mutex> stateLock\(stateMutex_\)' <<< "${callback_body}"; then
    Fail "ToolManager callbacks must retain the state lock for the complete callback"
fi

if rg -q 'stateLock\.unlock\(\)' <<< "${callback_body}"; then
    Fail "ToolManager callbacks must not release the state lock early"
fi
