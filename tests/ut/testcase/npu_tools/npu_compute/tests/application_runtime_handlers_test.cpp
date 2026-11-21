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
#include "acl_pti/callback/dispatcher.h"
#include "acl_pti/data/profiling_data_manager.h"
#include "acl_pti/profiling/application_profiler.h"
#include "acl_pti/initialization.h"
#include "aclpti/aclpti_data.h"
#include "aclpti/aclpti_runtime_api.h"
#include <vector>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "injection/runtime_stub_api.h"
#include "plog_capture.h"

namespace {
int calls = 0;
aclError OriginalMalloc(void** output, size_t bytes, aclrtMemMallocPolicy policy)
{
    ++calls;
    EXPECT_EQ(bytes, 127U);
    EXPECT_EQ(policy, ACL_MEM_MALLOC_HUGE_FIRST);
    *output = reinterpret_cast<void*>(0x1234);
    return 73;
}

namespace {
aclptiResult ConfigureCommunication()
{
    const char* sections[] = {"Communication"};
    aclptiRangeProfilerSetConfigParams params{sections, 1, ACLPTI_BLOCK_RESULT_ALL};
    return aclptiRangeProfilerSetConfig(&params);
}
} // namespace

TEST(ApplicationRuntimeHandlers, BusinessFailureDoesNotStopKernelReplayDataModule)
{
    NPU_COMPUTE_ISOLATE_TEST();
    calls = 0;
    int shutdowns = 0;
    ASSERT_EQ(
        aclptiRegisterDataModuleShutdownCallback(
            +[](void* value) {
                ++*static_cast<int*>(value);
                return ACLPTI_SUCCESS;
            },
            &shutdowns),
        ACLPTI_SUCCESS);
    ASSERT_EQ(aclpti::data::ProfilingDataManager::Instance().Initialize(), ACLPTI_SUCCESS);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtMalloc", &OriginalMalloc), ACL_SUCCESS);
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    void* pointer = nullptr;
    EXPECT_EQ(handlers.malloc(&pointer, 127, ACL_MEM_MALLOC_HUGE_FIRST), 73);
    EXPECT_EQ(pointer, reinterpret_cast<void*>(0x1234));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(shutdowns, 0);
    EXPECT_EQ(aclpti::data::ProfilingDataManager::Instance().ForceShutdown(), ACLPTI_SUCCESS);
    EXPECT_EQ(shutdowns, 1);
}

TEST(ApplicationRuntimeHandlers, LaunchForwardsOriginalArgumentsOnceAndDeliversEnterExit)
{
    NPU_COMPUTE_ISOLATE_TEST();
    int launches = 0;
    std::vector<aclptiCallbackSite> sites;
    auto& dispatcher = aclpti::callback::GetDispatcher();
    dispatcher.Configure(
        +[](void* value, aclptiCallbackDomain, aclptiCallbackId, const aclptiCallbackData* data) {
            auto& received = *static_cast<std::vector<aclptiCallbackSite>*>(value);
            received.push_back(data->callbackSite);
            if (data->callbackSite == ACLPTI_API_EXIT) {
                EXPECT_EQ(data->retval, 91);
            }
        },
        &sites);
    ASSERT_EQ(
        dispatcher.Enable(
            true, dispatcher.SubscriberHandle(), ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel),
        ACLPTI_SUCCESS);
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchKernel",
            +[](aclrtFuncHandle function, uint32_t blocks, const void* args, size_t bytes,
                aclrtStream stream) -> aclError {
                EXPECT_EQ(function, reinterpret_cast<void*>(0x2222));
                EXPECT_EQ(blocks, 17U);
                EXPECT_EQ(bytes, sizeof(int));
                EXPECT_EQ(stream, reinterpret_cast<void*>(0x3333));
                ++*static_cast<int*>(const_cast<void*>(args));
                return 91;
            }),
        ACL_SUCCESS);
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    EXPECT_EQ(
        handlers.launchKernel(
            reinterpret_cast<void*>(0x2222), 17, &launches, sizeof(launches), reinterpret_cast<void*>(0x3333)),
        91);
    EXPECT_EQ(launches, 1);
    ASSERT_EQ(sites.size(), 2);
    EXPECT_EQ(sites[0], ACLPTI_API_ENTER);
    EXPECT_EQ(sites[1], ACLPTI_API_EXIT);
}

TEST(ApplicationRuntimeHandlers, ConfigLaunchPreservesArgumentsAndResultThroughInjection)
{
    NPU_COMPUTE_ISOLATE_TEST();
    std::vector<aclptiCallbackSite> sites;
    auto& dispatcher = aclpti::callback::GetDispatcher();
    dispatcher.Configure(
        +[](void* value, aclptiCallbackDomain, aclptiCallbackId, const aclptiCallbackData* data) {
            static_cast<std::vector<aclptiCallbackSite>*>(value)->push_back(data->callbackSite);
            auto& params = *static_cast<aclptiAclrtLaunchKernelWithConfigParams*>(data->functionParams);
            EXPECT_EQ(params.numBlocks, 17U);
            if (data->callbackSite == ACLPTI_API_EXIT) {
                EXPECT_EQ(data->retval, *static_cast<aclError*>(params.reserve));
            }
        },
        &sites);
    ASSERT_EQ(
        dispatcher.Enable(
            true, dispatcher.SubscriberHandle(), ACLPTI_CB_DOMAIN_RUNTIME_API,
            ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithConfig),
        ACLPTI_SUCCESS);
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchKernelWithConfig",
            +[](aclrtFuncHandle function, uint32_t blocks, aclrtStream stream, aclrtLaunchKernelCfg* cfg,
                aclrtArgsHandle args, void* reserve) -> aclError {
                EXPECT_EQ(function, reinterpret_cast<void*>(0x2222));
                EXPECT_EQ(blocks, 17U);
                EXPECT_EQ(stream, reinterpret_cast<void*>(0x3333));
                EXPECT_EQ(cfg, reinterpret_cast<aclrtLaunchKernelCfg*>(0x4444));
                ++*static_cast<int*>(args);
                return *static_cast<aclError*>(reserve);
            }),
        ACL_SUCCESS);
    ASSERT_EQ(aclpti::initialization::InitializeApplication([](const auto&) {}, [](const auto&) {}), ACLPTI_SUCCESS);
    int launches = 0;
    for (aclError result : {ACL_SUCCESS, 91}) {
        EXPECT_EQ(
            aclrtLaunchKernelWithConfig(
                reinterpret_cast<void*>(0x2222), 17, reinterpret_cast<void*>(0x3333),
                reinterpret_cast<aclrtLaunchKernelCfg*>(0x4444), &launches, &result),
            result);
    }
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
    EXPECT_EQ(launches, 2);
    EXPECT_EQ(
        sites, (std::vector<aclptiCallbackSite>{ACLPTI_API_ENTER, ACLPTI_API_EXIT, ACLPTI_API_ENTER, ACLPTI_API_EXIT}));
}

TEST(ApplicationRuntimeHandlers, KernelModeLeavesConfigLaunchOnOriginalFallback)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchKernelWithConfig",
            +[](aclrtFuncHandle, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, aclrtArgsHandle,
                void* reserve) -> aclError {
                ++*static_cast<int*>(reserve);
                return 71;
            }),
        ACL_SUCCESS);
    ASSERT_TRUE(aclpti::handler::RegisterRuntimeApiHandlers(aclpti::handler::RuntimeHandlerMode::Kernel));
    ASSERT_EQ(acltoolHookInit(), ACL_SUCCESS);
    int launches = 0;
    EXPECT_EQ(aclrtLaunchKernelWithConfig(nullptr, 2, nullptr, nullptr, nullptr, &launches), 71);
    EXPECT_EQ(launches, 1);
}

TEST(ApplicationRuntimeHandlers, BinaryLookupDoesNotRequireAnInstrumentedBinaryRegistration)
{
    NPU_COMPUTE_ISOLATE_TEST();
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtBinaryGetFunction",
            +[](aclrtBinHandle binary, const char* name, aclrtFuncHandle* output) -> aclError {
                EXPECT_EQ(binary, reinterpret_cast<void*>(0x4444));
                EXPECT_STREQ(name, "communication");
                *output = reinterpret_cast<void*>(0x5555);
                return ACL_SUCCESS;
            }),
        ACL_SUCCESS);
    aclrtFuncHandle function = nullptr;
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    EXPECT_EQ(handlers.binaryGetFunction(reinterpret_cast<void*>(0x4444), "communication", &function), ACL_SUCCESS);
    EXPECT_EQ(function, reinterpret_cast<void*>(0x5555));
}

TEST(ApplicationRuntimeHandlers, FailedSetDoesNotRetainAReferenceOrStartCollection)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::profiling;
    ApplicationSummary summary;
    std::vector<int32_t> receivedTypes;
    ASSERT_EQ(
        aclpti::initialization::InitializeApplication(
            [&receivedTypes](const auto& record) { receivedTypes.push_back(record.type); },
            [&summary](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtSetDevice", +[](int32_t) -> aclError { return 77; }), ACL_SUCCESS);
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    EXPECT_EQ(handlers.setDevice(0), 77);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(0), 0U);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtSetDevice", +[](int32_t) { return ACL_SUCCESS; }), ACL_SUCCESS);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtResetDevice", +[](int32_t) { return ACL_SUCCESS; }), ACL_SUCCESS);
    EXPECT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(0), 1U);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
    ASSERT_EQ(summary.devices.size(), 1U);
    EXPECT_EQ(summary.receivedRecords, 2U);
    EXPECT_EQ(receivedTypes, (std::vector<int32_t>{PMU_DATA_TYPE, LOG_DATA_TYPE}));
    EXPECT_TRUE(summary.complete);
}

TEST(ApplicationRuntimeHandlers, ForceResetStopsBeforeOriginalAndLeavesOtherDevicesActive)
{
    NPU_COMPUTE_ISOLATE_TEST();
    aclpti::profiling::ApplicationSummary summary;
    ASSERT_EQ(
        aclpti::initialization::InitializeApplication(
            [](const auto&) {}, [&summary](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    ASSERT_EQ(handlers.setDevice(1), ACL_SUCCESS);
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtResetDeviceForce",
            +[](int32_t device) -> aclError {
                ASCTOOL_DEBUG("[test] teardown entered");
                EXPECT_EQ(device, 0);
                ASCTOOL_DEBUG("[test] device resources released");
                return ACL_SUCCESS;
            }),
        ACL_SUCCESS);
    std::string output;
    ASSERT_TRUE(npu_compute_test::CapturePlog([&] { EXPECT_EQ(handlers.resetDeviceForce(0), ACL_SUCCESS); }, &output));
    const auto stop = output.find("[prof_api_stub] MsprofStop");
    ASSERT_NE(stop, std::string::npos);
    EXPECT_LT(stop, output.find("[test] teardown entered"));
    EXPECT_LT(stop, output.find("[test] device resources released"));
    EXPECT_EQ(aclpti::profiling::GetApplicationProfiler().DeviceEpoch(0), 0U);
    EXPECT_EQ(aclpti::profiling::GetApplicationProfiler().DeviceEpoch(1), 1U);
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    EXPECT_EQ(aclpti::profiling::GetApplicationProfiler().DeviceEpoch(0), 2U);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtResetDevice", +[](int32_t) { return ACL_SUCCESS; }), ACL_SUCCESS);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(aclpti::profiling::GetApplicationProfiler().DeviceEpoch(0), 0U);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    // Device 1 remains active; reset of device 0 must not stop the whole session.
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_ERROR_PROFILING_FAILED);
    EXPECT_EQ(summary.devices.at(0).state, aclpti::profiling::ApplicationDeviceState::Inactive);
    EXPECT_EQ(summary.devices.at(0).deviceEpoch, 2U);
    EXPECT_EQ(summary.devices.at(1).state, aclpti::profiling::ApplicationDeviceState::Active);
    EXPECT_EQ(summary.receivedRecords, 4U);
}

TEST(ApplicationRuntimeHandlers, FailedNonFinalResetKeepsTheReferenceAndCollection)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::profiling;
    ApplicationSummary summary;
    ASSERT_EQ(
        aclpti::initialization::InitializeApplication(
            [](const auto&) {}, [&summary](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtResetDevice", +[](int32_t) -> aclError { return 81; }), ACL_SUCCESS);
    EXPECT_EQ(handlers.resetDevice(0), 81);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(0), 1U);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtResetDevice", +[](int32_t) { return ACL_SUCCESS; }), ACL_SUCCESS);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(0), 1U);
    EXPECT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(0), 1U);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(0), 0U);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
    ASSERT_EQ(summary.devices.size(), 1U);
    EXPECT_EQ(summary.devices.at(0).deviceEpoch, 1U);
    EXPECT_EQ(summary.devices.at(0).state, ApplicationDeviceState::Inactive);
    EXPECT_TRUE(summary.complete);
    EXPECT_EQ(summary.receivedRecords, 2U);
}

TEST(ApplicationRuntimeHandlers, FailedFinalResetPreservesBusinessErrorAndCoverageGapAfterFinalize)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::profiling;
    ApplicationSummary summary;
    ASSERT_EQ(
        aclpti::initialization::InitializeApplication(
            [](const auto&) {}, [&summary](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtResetDevice", +[](int32_t) -> aclError { return 81; }), ACL_SUCCESS);
    EXPECT_EQ(handlers.resetDevice(0), 81);
    // The device may still be used after Runtime rolls back a failed teardown, but profiling has stopped.
    EXPECT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(0), 0U);
    EXPECT_EQ(aclFinalize(), ACL_SUCCESS);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_ERROR_PROFILING_FAILED);
    EXPECT_TRUE(summary.tailConfirmed);
    EXPECT_FALSE(summary.complete);
    EXPECT_EQ(summary.devices.at(0).state, ApplicationDeviceState::Unknown);
    EXPECT_EQ(summary.devices.at(0).endReason, ApplicationDeviceEndReason::Unconfirmed);
    EXPECT_EQ(summary.devices.at(0).stopStatus, 0);
    EXPECT_EQ(summary.devices.at(0).deviceEpoch, 1U);
    EXPECT_EQ(summary.receivedRecords, 2U);
}

TEST(ApplicationRuntimeHandlers, UserDeviceCallbackCanSetAnotherDeviceOnTheSameThread)
{
    NPU_COMPUTE_ISOLATE_TEST();
    using namespace aclpti::profiling;
    ApplicationSummary summary;
    auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    ASSERT_EQ(
        aclrtRegDeviceStateCallback(
            "npu-compute-application",
            +[](int32_t device, aclrtDeviceState state, void* value) {
                if (device == 0 && state == ACL_RT_DEVICE_STATE_RESET_PRE) {
                    EXPECT_EQ(static_cast<const aclpti::handler::RuntimeHandlerSet*>(value)->setDevice(1), ACL_SUCCESS);
                }
            },
            &handlers),
        ACL_SUCCESS);
    ASSERT_EQ(
        aclpti::initialization::InitializeApplication(
            [](const auto&) {}, [&summary](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(GetApplicationProfiler().DeviceEpoch(1), 1U);
    EXPECT_EQ(handlers.resetDevice(1), ACL_SUCCESS);
    EXPECT_EQ(aclpti::initialization::ShutdownApplication(), ACLPTI_SUCCESS);
    ASSERT_EQ(summary.devices.size(), 2U);
    EXPECT_TRUE(summary.devices.at(0).startAttempted);
    EXPECT_TRUE(summary.devices.at(0).stopAttempted);
    EXPECT_EQ(summary.devices.at(0).deviceEpoch, 1U);
    EXPECT_TRUE(summary.complete);
    EXPECT_EQ(summary.receivedRecords, 4U);
}

TEST(ApplicationRuntimeHandlers, ConcurrentSetDeviceCannotRestartBetweenStopAndOriginalReset)
{
    NPU_COMPUTE_ISOLATE_TEST();
    struct Coordination {
        std::mutex mutex;
        std::condition_variable ready;
        bool resetEntered = false;
        bool releaseReset = false;
        bool setThreadStarted = false;
        bool setOriginalEntered = false;
    };
    static Coordination coordination;
    auto& profiler = aclpti::profiling::GetApplicationProfiler();
    ASSERT_EQ(aclpti::initialization::InitializeApplication([](const auto&) {}, [](const auto&) {}), ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(), ACLPTI_SUCCESS);
    const auto handlers = aclpti::handler::BuildApplicationRuntimeHandlers();
    ASSERT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtResetDevice",
            +[](int32_t device) -> aclError {
                EXPECT_EQ(device, 0);
                EXPECT_EQ(aclpti::profiling::GetApplicationProfiler().DeviceEpoch(device), 0U);
                std::unique_lock<std::mutex> lock(coordination.mutex);
                coordination.resetEntered = true;
                coordination.ready.notify_all();
                coordination.ready.wait(lock, [] { return coordination.releaseReset; });
                return ACL_SUCCESS;
            }),
        ACL_SUCCESS);
    ASSERT_EQ(
        RuntimeStubSetOriginFunction(
            "aclrtSetDevice",
            +[](int32_t) -> aclError {
                std::lock_guard<std::mutex> lock(coordination.mutex);
                coordination.setOriginalEntered = true;
                coordination.ready.notify_all();
                return ACL_SUCCESS;
            }),
        ACL_SUCCESS);
    std::thread reset([&] { EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS); });
    {
        std::unique_lock<std::mutex> lock(coordination.mutex);
        coordination.ready.wait(lock, [] { return coordination.resetEntered; });
    }
    std::thread set([&] {
        {
            std::lock_guard<std::mutex> lock(coordination.mutex);
            coordination.setThreadStarted = true;
            coordination.ready.notify_all();
        }
        EXPECT_EQ(handlers.setDevice(0), ACL_SUCCESS);
    });
    {
        std::unique_lock<std::mutex> lock(coordination.mutex);
        coordination.ready.wait(lock, [] { return coordination.setThreadStarted; });
        EXPECT_FALSE(coordination.ready.wait_for(
            lock, std::chrono::milliseconds(100), [] { return coordination.setOriginalEntered; }));
        coordination.releaseReset = true;
        coordination.ready.notify_all();
    }
    reset.join();
    set.join();
    EXPECT_TRUE(coordination.setOriginalEntered);
    EXPECT_EQ(handlers.resetDevice(0), ACL_SUCCESS);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
}

} // namespace
