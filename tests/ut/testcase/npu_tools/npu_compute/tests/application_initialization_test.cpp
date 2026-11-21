/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "isolated_gtest.h"
#include "acl_pti/handler/runtime_api_handlers.h"
#include "acl_pti/initialization.h"
#include "aclpti/aclpti.h"
#include "injection/runtime_stub_api.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
aclptiResult ConfigureCommunication()
{
    const char* sections[] = {"Communication"};
    aclptiRangeProfilerSetConfigParams params{sections, 1, ACLPTI_BLOCK_RESULT_ALL};
    return aclptiRangeProfilerSetConfig(&params);
}
} // namespace

TEST(ApplicationInitialization, SubscriptionDoesNotInitializeKernelReplay)
{
    NPU_COMPUTE_ISOLATE_TEST();
    bool drained = false;
    ASSERT_EQ(
        aclpti::initialization::InitializeApplication([](const auto&) {}, [&](const auto&) { drained = true; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    aclptiSubscribeHandle subscriber = nullptr;
    EXPECT_EQ(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr), ACLPTI_SUCCESS);
    EXPECT_NE(subscriber, nullptr);
    EXPECT_EQ(aclptiRangeProfilerSetConfig(nullptr), ACLPTI_ERROR_INVALID_PARAMETER);
    const char* sections[] = {"Communication"};
    aclptiRangeProfilerSetConfigParams params{sections, 1, ACLPTI_BLOCK_RESULT_ALL};
    EXPECT_EQ(aclptiRangeProfilerSetConfig(&params), ACLPTI_SUCCESS);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
    EXPECT_TRUE(drained);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
}

TEST(ApplicationInitialization, MissingRuntimeEntryFailsInitializationAndCanBeRetried)
{
    NPU_COMPUTE_ISOLATE_TEST();
    aclrtApiFunc origin = nullptr;
    ASSERT_EQ(aclrtApiInjectionGetFunc("aclrtLaunchKernelWithConfig", &origin, nullptr), ACL_SUCCESS);
    ASSERT_EQ(RuntimeStubClearOrigin("aclrtLaunchKernelWithConfig"), ACL_SUCCESS);
    EXPECT_EQ(
        aclpti::initialization::InitializeApplication([](const auto&) {}, [](const auto&) {}),
        ACLPTI_ERROR_INITIALIZATION_FAILED);
    ASSERT_EQ(RuntimeStubSetOrigin("aclrtLaunchKernelWithConfig", origin), ACL_SUCCESS);
    ASSERT_EQ(aclpti::initialization::InitializeApplication([](const auto&) {}, [](const auto&) {}), ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
}

TEST(ApplicationInitialization, CannotSwitchAnInitializedKernelSessionToApplication)
{
    NPU_COMPUTE_ISOLATE_TEST();
    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_EQ(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr), ACLPTI_SUCCESS);
    // Kernel initialization must leave the application device-state callback unregistered.
    EXPECT_EQ(
        aclrtRegDeviceStateCallback(
            "npu-compute-application", +[](int32_t, aclrtDeviceState, void*) {}, nullptr),
        ACL_SUCCESS);
    EXPECT_EQ(
        aclpti::initialization::InitializeApplication([](const auto&) {}, [](const auto&) {}),
        ACLPTI_ERROR_INVALID_STATE);
}

TEST(ApplicationInitialization, FailsWhenFinalizeCallbackCannotBeRegistered)
{
    NPU_COMPUTE_ISOLATE_TEST();
    RuntimeStubSetFinalizeRegistrationStatus(ACL_ERROR_FAILURE);
    EXPECT_EQ(
        aclpti::initialization::InitializeApplication([](const auto&) {}, [](const auto&) {}),
        ACLPTI_ERROR_INITIALIZATION_FAILED);
}

TEST(ApplicationInitialization, EarlierAclCleanupFailureDoesNotConfirmTail)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(
        aclFinalizeCallbackRegister(
            ACL_REG_TYPE_OTHER, +[](void*) -> aclError { return ACL_ERROR_FAILURE; }, nullptr),
        ACL_SUCCESS);
    aclpti::profiling::ApplicationSummary summary;
    ASSERT_EQ(
        aclpti::initialization::InitializeApplication([](const auto&) {}, [&](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    EXPECT_EQ(aclFinalize(), ACL_ERROR_FAILURE);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
    EXPECT_FALSE(summary.tailConfirmed);
}

TEST(ApplicationInitialization, PreparedSessionPassesThroughUntilActivationAndAfterShutdown)
{
    NPU_COMPUTE_ISOLATE_TEST();
    static aclpti::profiling::ApplicationSummary summary;
    using namespace aclpti::initialization;
    ASSERT_EQ(
        PrepareApplicationSession(
            +[](const aclpti::profiling::ApplicationRawRecord&) {},
            +[](const aclpti::profiling::ApplicationSummary& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    EXPECT_EQ(aclrtSetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(ApplicationDeviceEpoch(0), 0U);
    ActivateProcessorSession();
    ASSERT_EQ(aclrtSetDevice(1), ACL_SUCCESS);
    EXPECT_EQ(ApplicationDeviceEpoch(1), 1U);
    EXPECT_EQ(
        ShutdownProcessorSession(aclpti::profiling::ApplicationEndReason::ProcessExit), ACLPTI_ERROR_PROFILING_FAILED);
    EXPECT_EQ(ApplicationDeviceEpoch(1), 0U);
    EXPECT_EQ(aclrtSetDevice(2), ACL_SUCCESS);
    EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
    EXPECT_EQ(summary.endReason, aclpti::profiling::ApplicationEndReason::ProcessExit);
    EXPECT_FALSE(summary.tailConfirmed);
    EXPECT_FALSE(summary.complete);
    ASSERT_EQ(summary.devices.size(), 1U);
    EXPECT_FALSE(summary.devices.at(1).stopAttempted);
}

TEST(ApplicationInitialization, ConcurrentCloseWaitsForAnAdmittedHook)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::initialization;
    using namespace aclpti::profiling;
    struct Coordination {
        std::mutex mutex;
        std::condition_variable ready;
        bool inOriginal = false;
        bool release = false;
        std::atomic<unsigned> drainCount{0};
    };
    static Coordination sync;
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtSetDevice",
            +[](int32_t device) -> aclError {
                if (device == 0) {
                    std::unique_lock<std::mutex> lock(sync.mutex);
                    sync.inOriginal = true;
                    sync.ready.notify_all();
                    sync.ready.wait(lock, [] { return sync.release; });
                }
                return ACL_SUCCESS;
            }),
        ACL_SUCCESS);
    ASSERT_EQ(
        PrepareApplicationSession(
            +[](const ApplicationRawRecord&) {}, +[](const ApplicationSummary&) { ++sync.drainCount; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    ActivateProcessorSession();
    aclrtApiFunc lateHook = nullptr;
    ASSERT_EQ(aclrtApiInjectionGetFunc("aclrtSetDevice", nullptr, &lateHook), ACL_SUCCESS);
    ASSERT_NE(lateHook, nullptr);

    std::thread admitted([] { EXPECT_EQ(aclrtSetDevice(0), ACL_SUCCESS); });
    {
        std::unique_lock<std::mutex> lock(sync.mutex);
        sync.ready.wait(lock, [] { return sync.inOriginal; });
    }

    auto close = [] {
        EXPECT_EQ(ShutdownProcessorSession(ApplicationEndReason::ProcessExit), ACLPTI_ERROR_PROFILING_FAILED);
    };
    std::thread first(close);
    std::thread second(close);
    while (CollectionOpen()) {
        std::this_thread::yield();
    }
    EXPECT_EQ(sync.drainCount, 0U);
    {
        std::lock_guard<std::mutex> lock(sync.mutex);
        sync.release = true;
    }
    sync.ready.notify_all();
    admitted.join();
    first.join();
    second.join();
    EXPECT_EQ(sync.drainCount, 1U);
    EXPECT_EQ(reinterpret_cast<aclrtSetDeviceFunc>(lateHook)(1), ACL_SUCCESS);
    EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
    EXPECT_EQ(sync.drainCount, 1U);
}

TEST(ApplicationInitialization, FinalizeRecordsTailBeforeTheMetadataFence)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::initialization;
    using namespace aclpti::profiling;
    struct Coordination {
        std::atomic<unsigned> metadataCount{0};
        std::atomic<unsigned> drainCount{0};
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool release = false;
    };
    static Coordination sync;
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtSetDevice",
            +[](int32_t device) -> aclError {
                if (device != 0) {
                    return ACL_SUCCESS;
                }
                std::unique_lock<std::mutex> lock(sync.mutex);
                sync.entered = true;
                sync.changed.notify_all();
                sync.changed.wait(lock, [] { return sync.release; });
                return ACL_SUCCESS;
            }),
        ACL_SUCCESS);
    ASSERT_EQ(
        PrepareApplicationSession(
            +[](const ApplicationRawRecord&) {},
            +[](const ApplicationSummary& summary) {
                EXPECT_TRUE(summary.tailConfirmed);
                EXPECT_TRUE(summary.complete);
                EXPECT_EQ(summary.endReason, ApplicationEndReason::AclFinalize);
                EXPECT_EQ(summary.devices.at(1).state, ApplicationDeviceState::Inactive);
                EXPECT_FALSE(summary.devices.at(1).stopAttempted);
                EXPECT_EQ(summary.receivedRecords, 0U);
                EXPECT_EQ(sync.metadataCount, 2U);
                ++sync.drainCount;
            }),
        ACLPTI_SUCCESS);
    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_EQ(
        aclptiSubscribe(
            &subscriber,
            +[](void*, aclptiCallbackDomain, aclptiCallbackId, const aclptiCallbackData*) { ++sync.metadataCount; },
            nullptr, nullptr),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    ActivateProcessorSession();
    ASSERT_EQ(aclrtSetDevice(1), ACL_SUCCESS);
    ASSERT_EQ(
        aclptiEnableCallback(true, subscriber, ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtSetDevice),
        ACLPTI_SUCCESS);
    std::thread admitted([] { EXPECT_EQ(aclrtSetDevice(0), ACL_SUCCESS); });
    {
        std::unique_lock<std::mutex> lock(sync.mutex);
        sync.changed.wait(lock, [] { return sync.entered; });
    }
    EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
    std::thread shutdown(
        [] { EXPECT_EQ(ShutdownProcessorSession(ApplicationEndReason::ProcessExit), ACLPTI_SUCCESS); });
    while (CollectionOpen()) {
        std::this_thread::yield();
    }
    EXPECT_EQ(sync.metadataCount, 1U);
    EXPECT_EQ(sync.drainCount, 0U);
    {
        std::lock_guard<std::mutex> lock(sync.mutex);
        sync.release = true;
    }
    sync.changed.notify_all();
    admitted.join();
    shutdown.join();
    EXPECT_EQ(sync.drainCount, 1U);
}

TEST(ApplicationInitialization, StartupRollbackSuppressesSessionHandoff)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::initialization;
    using namespace aclpti::profiling;
    ASSERT_EQ(
        PrepareApplicationSession(
            +[](const ApplicationRawRecord&) { ADD_FAILURE() << "unexpected data"; },
            +[](const ApplicationSummary&) { ADD_FAILURE() << "unexpected summary"; }),
        ACLPTI_SUCCESS);
    EXPECT_EQ(ShutdownProcessorSession(ApplicationEndReason::StartupRollback), ACLPTI_SUCCESS);
    EXPECT_EQ(aclrtSetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
}

TEST(ApplicationInitialization, LifecycleRegistrationDefersInitializationAndRunsFinalize)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::initialization;
    static unsigned starts = 0;
    static unsigned finishes = 0;
    ASSERT_EQ(
        RegisterApplicationLifecycle(+[]() noexcept { ++starts; }, +[]() noexcept { ++finishes; }), ACLPTI_SUCCESS);
    ASSERT_EQ(
        RegisterApplicationLifecycle(+[]() noexcept { ++starts; }, +[]() noexcept { ++finishes; }), ACLPTI_SUCCESS);
    EXPECT_EQ(starts, 0U);
    EXPECT_EQ(finishes, 0U);

    ASSERT_EQ(aclInit(nullptr), ACL_SUCCESS);
    EXPECT_EQ(starts, 1U);
    ASSERT_EQ(aclFinalize(), ACL_SUCCESS);
    EXPECT_EQ(finishes, 1U);
}

TEST(ApplicationInitialization, FinalizeClosesAdmissionAndDistinguishesLocalUploaderFromRuntimeAccess)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::initialization;
    using namespace aclpti::profiling;
    ASSERT_EQ(
        PrepareApplicationSession(
            +[](const ApplicationRawRecord&) {}, +[](const ApplicationSummary&) {}),
        ACLPTI_SUCCESS);
    ActivateProcessorSession();

    {
        const CallbackAccess upload(CallbackAccess::Kind::Raw);
        ASSERT_TRUE(upload);
        {
            const CallbackAccess runtime;
            ASSERT_TRUE(runtime);
            EXPECT_FALSE(CloseApplicationAdmissionForFinalize());
            EXPECT_FALSE(CallbackAccess());
            EXPECT_FALSE(CallbackAccess(CallbackAccess::Kind::Raw));
        }
        EXPECT_TRUE(CloseApplicationAdmissionForFinalize());
        EXPECT_FALSE(CallbackAccess());
        EXPECT_FALSE(CallbackAccess(CallbackAccess::Kind::Raw));
    }
    EXPECT_EQ(ShutdownProcessorSession(ApplicationEndReason::AclFinalize), ACLPTI_SUCCESS);
}
