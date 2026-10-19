/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_active_probe_plan.h"

#include <atomic>
#include <shared_mutex>

namespace aclsan {
namespace {

std::atomic<uint32_t> g_activeProbeGroupMask{0};
std::shared_mutex g_activeProbePlanMutex;

} // namespace

uint32_t ProbeGroupMaskForCallback(AclsanCallbackDomain domain, AclsanCallbackId id) noexcept
{
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_BARRIER && id == ACLSAN_CBID_DEVICE_PIPE_BARRIER) {
        return PROBE_GROUP_SYNC;
    }
    if (domain != ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION) {
        return 0;
    }
    if (id == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        return PROBE_GROUP_MTE1 | PROBE_GROUP_MTE2 | PROBE_GROUP_MTE3 | PROBE_GROUP_FIXPIPE | PROBE_GROUP_SCALAR |
               PROBE_GROUP_MATRIX;
    }
    if (id == ACLSAN_CBID_DEVICE_STATE) {
        return PROBE_GROUP_MTE1 | PROBE_GROUP_MTE2 | PROBE_GROUP_MTE3 | PROBE_GROUP_FIXPIPE | PROBE_GROUP_SCALAR;
    }
    if (id == ACLSAN_CBID_DEVICE_SYNC) {
        return PROBE_GROUP_SYNC;
    }
    return 0;
}

std::shared_mutex& ActiveProbePlanMutex() noexcept { return g_activeProbePlanMutex; }

void CommitActiveProbePlan(uint32_t probeGroupMask) noexcept
{
    g_activeProbeGroupMask.store(probeGroupMask, std::memory_order_release);
}

uint32_t SnapshotActiveProbePlan() noexcept { return g_activeProbeGroupMask.load(std::memory_order_acquire); }

} // namespace aclsan
