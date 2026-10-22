/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <gtest/gtest.h>

#include "aclsan_boundary_stub.h"

#include "aclsan_active_probe_plan.h"
#include "aclsan_dispatch.h"
#include "device_runtime/device_symbolizer.h"
#include "aclsan_device_call_stack.h"
#include "aclsan_runtime_hook.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#define CHECK(expression)                                      \
    do {                                                       \
        if (!(expression)) {                                   \
            ADD_FAILURE_AT(__FILE__, __LINE__) << #expression; \
            return;                                            \
        }                                                      \
    } while (false)

namespace {

std::atomic<bool> g_delayApply{false};
std::atomic<int> g_activeApplyCalls{0};
std::atomic<int> g_maxActiveApplyCalls{0};

void RecordMaximum(int value)
{
    int previous = g_maxActiveApplyCalls.load(std::memory_order_relaxed);
    while (previous < value &&
           !g_maxActiveApplyCalls.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

} // namespace

void ApplyRuntimeHooksTracking(const std::set<aclrtApiId>&) noexcept
{
    const int activeCalls = g_activeApplyCalls.fetch_add(1, std::memory_order_acq_rel) + 1;
    RecordMaximum(activeCalls);
    if (g_delayApply.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    g_activeApplyCalls.fetch_sub(1, std::memory_order_acq_rel);
}

AclsanStatus ResolveInvalidState(uint64_t, aclsan::device_runtime::CallStackResult*) noexcept
{
    return ACLSAN_STATUS_ERROR_INVALID_STATE;
}

void Callback(void*, AclsanCallbackDomain, AclsanCallbackId, const void*) {}

TEST(AclsanProbePlanCallback, Main)
{
    aclsan_test::Boundary boundary;
    boundary.applyRuntimeHooks = &ApplyRuntimeHooksTracking;
    boundary.resolveActiveDeviceCallStack = &ResolveInvalidState;
    const aclsan_test::BoundaryGuard boundaryGuard{boundary};

    using aclsan::PROBE_GROUP_FIXPIPE;
    using aclsan::PROBE_GROUP_MATRIX;
    using aclsan::PROBE_GROUP_MTE1;
    using aclsan::PROBE_GROUP_MTE2;
    using aclsan::PROBE_GROUP_MTE3;
    using aclsan::PROBE_GROUP_SCALAR;
    using aclsan::PROBE_GROUP_SYNC;
    using aclsan::PROBE_GROUP_VECTOR;

    constexpr uint32_t memoryProbeMask = PROBE_GROUP_MTE1 | PROBE_GROUP_MTE2 | PROBE_GROUP_MTE3 | PROBE_GROUP_FIXPIPE |
                                         PROBE_GROUP_SCALAR | PROBE_GROUP_MATRIX | PROBE_GROUP_VECTOR;

    CHECK((memoryProbeMask & PROBE_GROUP_MATRIX) != 0);
    CHECK((memoryProbeMask & PROBE_GROUP_VECTOR) != 0);
    CHECK(
        (aclsan::PROBE_GROUP_ALL & (PROBE_GROUP_MATRIX | PROBE_GROUP_VECTOR)) ==
        (PROBE_GROUP_MATRIX | PROBE_GROUP_VECTOR));

    CHECK(
        aclsan::ProbeGroupMaskForCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        memoryProbeMask);
    CHECK(
        aclsan::ProbeGroupMaskForCallback(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        PROBE_GROUP_SYNC);
    CHECK(aclsan::ProbeGroupMaskForCallback(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) == 0);
    CHECK(
        aclsan::ProbeGroupsFromMask(PROBE_GROUP_SYNC | PROBE_GROUP_MTE3 | PROBE_GROUP_MTE1 | PROBE_GROUP_SCALAR) ==
        (std::vector<aclsan::ProbeGroup>{
            aclsan::ProbeGroup::Mte1, aclsan::ProbeGroup::Mte3, aclsan::ProbeGroup::Scalar, aclsan::ProbeGroup::Sync}));

    aclsan::CommitActiveProbePlan(memoryProbeMask);
    CHECK(aclsan::SnapshotActiveProbePlan() == memoryProbeMask);
    aclsan::CommitActiveProbePlan(memoryProbeMask | PROBE_GROUP_SYNC);
    CHECK(aclsan::SnapshotActiveProbePlan() == (memoryProbeMask | PROBE_GROUP_SYNC));
    bool readerAcquiredLock = true;
    {
        std::unique_lock<std::shared_mutex> writer(aclsan::ActiveProbePlanMutex());
        std::thread reader([&readerAcquiredLock] {
            readerAcquiredLock = aclsan::ActiveProbePlanMutex().try_lock_shared();
            if (readerAcquiredLock) {
                aclsan::ActiveProbePlanMutex().unlock_shared();
            }
        });
        reader.join();
    }
    CHECK(!readerAcquiredLock);
    aclsan::CommitActiveProbePlan(0);

    AclsanSubscriberHandle subscriber = nullptr;
    CHECK(aclsan::SnapshotActiveProbePlan() == 0);
    CHECK(aclsanSubscribe(&subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);

    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(aclsan::SnapshotActiveProbePlan() == 0);

    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(aclsan::SnapshotActiveProbePlan() == memoryProbeMask);

    CHECK(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(aclsan::SnapshotActiveProbePlan() == (memoryProbeMask | PROBE_GROUP_SYNC));

    CHECK(
        aclsanEnableCallback(0, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS) ==
        ACLSAN_STATUS_SUCCESS);
    CHECK(aclsan::SnapshotActiveProbePlan() == PROBE_GROUP_SYNC);

    CHECK(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
    CHECK(aclsan::SnapshotActiveProbePlan() == 0);

    subscriber = nullptr;
    CHECK(aclsanSubscribe(&subscriber, &Callback, nullptr) == ACLSAN_STATUS_SUCCESS);
    g_maxActiveApplyCalls.store(0, std::memory_order_release);
    g_delayApply.store(true, std::memory_order_release);
    AclsanStatus memoryStatus = ACLSAN_STATUS_ERROR_INTERNAL;
    AclsanStatus syncStatus = ACLSAN_STATUS_ERROR_INTERNAL;
    std::mutex startMutex;
    std::condition_variable startCondition;
    int readyThreads = 0;
    bool startThreads = false;
    auto awaitStart = [&] {
        std::unique_lock<std::mutex> lock(startMutex);
        ++readyThreads;
        startCondition.notify_all();
        startCondition.wait(lock, [&startThreads] { return startThreads; });
    };
    std::thread memoryThread([&] {
        awaitStart();
        memoryStatus =
            aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS);
    });
    std::thread syncThread([&] {
        awaitStart();
        syncStatus = aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC);
    });
    {
        std::unique_lock<std::mutex> lock(startMutex);
        startCondition.wait(lock, [&readyThreads] { return readyThreads == 2; });
        startThreads = true;
    }
    startCondition.notify_all();
    memoryThread.join();
    syncThread.join();
    g_delayApply.store(false, std::memory_order_release);
    CHECK(memoryStatus == ACLSAN_STATUS_SUCCESS);
    CHECK(syncStatus == ACLSAN_STATUS_SUCCESS);
    CHECK(g_maxActiveApplyCalls.load(std::memory_order_acquire) == 1);
    CHECK(aclsan::SnapshotActiveProbePlan() == (memoryProbeMask | PROBE_GROUP_SYNC));
    CHECK(aclsanUnsubscribe(subscriber) == ACLSAN_STATUS_SUCCESS);
}

TEST(AclsanProbePlanCallback, BarrierUsesInstructionDomainAndIndependentId)
{
    aclsan_test::Boundary boundary;
    boundary.applyRuntimeHooks = &ApplyRuntimeHooksTracking;
    const aclsan_test::BoundaryGuard boundaryGuard{boundary};
    int deliveries = 0;
    const auto callback = [](void* userdata, AclsanCallbackDomain domain, AclsanCallbackId id, const void*) {
        EXPECT_EQ(domain, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION);
        EXPECT_EQ(id, ACLSAN_CBID_DEVICE_PIPE_BARRIER);
        ++*static_cast<int*>(userdata);
    };
    AclsanSubscriberHandle subscriber = nullptr;
    ASSERT_EQ(aclsanSubscribe(&subscriber, callback, &deliveries), ACLSAN_STATUS_SUCCESS);
    EXPECT_EQ(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER),
        ACLSAN_STATUS_SUCCESS);
    EXPECT_EQ(aclsan::SnapshotActiveProbePlan(), aclsan::PROBE_GROUP_SYNC);
    EXPECT_EQ(
        aclsanEnableCallback(1, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_CONTROL),
        ACLSAN_STATUS_ERROR_INVALID_PARAMETER);
    AclsanDeviceBarrierData barrier{};
    aclsan::AclsanCallbackDispatcher::DispatchDeviceBarrier(barrier);
    EXPECT_EQ(deliveries, 1);
    EXPECT_EQ(
        aclsanEnableCallback(0, subscriber, ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER),
        ACLSAN_STATUS_SUCCESS);
    EXPECT_EQ(aclsan::SnapshotActiveProbePlan(), 0U);
    aclsan::AclsanCallbackDispatcher::DispatchDeviceBarrier(barrier);
    EXPECT_EQ(deliveries, 1);
    EXPECT_EQ(aclsanUnsubscribe(subscriber), ACLSAN_STATUS_SUCCESS);
}
