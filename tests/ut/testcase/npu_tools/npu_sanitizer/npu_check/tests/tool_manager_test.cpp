// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "acl_san/aclsan_api.h"
#include "tool_manager/tool_manager.h"
#include "options.h"
#include "uds_client.h"
#include "acl/acl_rt.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <functional>
#include <set>
#include <thread>
#include <unistd.h>

namespace {
AclsanCallbackFunc callback = nullptr;
void* userdata = nullptr;
std::set<std::pair<AclsanCallbackDomain, AclsanCallbackId>> enabled;
size_t enableCalls = 0;
std::vector<uint32_t> timeoutValues;
aclError timeoutLookupStatus = ACL_SUCCESS;
aclError timeoutSetStatus = ACL_SUCCESS;
bool timeoutApiAvailable = true;

aclError SetOpExecuteTimeout(uint32_t seconds)
{
    timeoutValues.push_back(seconds);
    return timeoutSetStatus;
}

void Emit(AclsanCallbackDomain domain, AclsanCallbackId id, const void* data)
{
    if (enabled.count({domain, id}) != 0)
        callback(userdata, domain, id, data);
}

TEST(NpuCheckOptionsTest, ParsesOperatorExecutionTimeoutInSeconds)
{
    std::vector<std::string> args = {"npu-check", "--deadlock-timeout", "5", "/bin/true"};
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    npucheck::Options options;
    std::string error;
    ASSERT_TRUE(npucheck::ParseOptions(static_cast<int>(argv.size()), argv.data(), options, error)) << error;
    ASSERT_TRUE(options.opExecuteTimeoutSec.has_value());
    EXPECT_EQ(*options.opExecuteTimeoutSec, 5U);
}

TEST(NpuCheckOptionsTest, RejectsZeroOperatorExecutionTimeout)
{
    std::vector<std::string> args = {"npu-check", "--deadlock-timeout", "0", "/bin/true"};
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    npucheck::Options options;
    std::string error;
    EXPECT_FALSE(npucheck::ParseOptions(static_cast<int>(argv.size()), argv.data(), options, error));
    EXPECT_NE(error.find("positive"), std::string::npos);
}

struct Session {
    std::string text;
    std::string error;
    uint16_t flags = 0;
    int initialized = -1;
};

Session RunSession(
    std::vector<std::string> args, bool memoryError, bool syncError, bool failSync = false, bool nullCallback = false,
    bool initError = false, bool race = false, const std::function<void()>& onReady = {})
{
    enabled.clear();
    enableCalls = 0;
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    npucheck::Options options;
    Session result;
    if (!npucheck::ParseOptions(argv.size(), argv.data(), options, result.error))
        return result;
    const auto name = "@npu-check-manager-" + std::to_string(getpid());
    constexpr uint64_t sessionId = 93091;
    setenv(npucheck::ipc::UDS_NAME_ENV, name.c_str(), 1);
    setenv(npucheck::ipc::SESSION_ID_ENV, "93091", 1);
    setenv(npucheck::ipc::CLI_PID_ENV, std::to_string(getpid()).c_str(), 1);
    setenv(npucheck::ipc::HANDSHAKE_TIMEOUT_ENV, "5000", 1);
    npucheck::ipc::ConfigureRequest config;
    config.tools = options.tools;
    config.opExecuteTimeoutSec = options.opExecuteTimeoutSec;
    std::thread client([&] {
        npucheck::ipc::UdsClient transport;
        if (!transport.ConnectAndConfigure(
                name, sessionId, getpid(), npucheck::ipc::DeadlineAfterMs(5000), config, result.error))
            return;
        while (true) {
            npucheck::ipc::Frame frame;
            if (transport.Receive(frame, npucheck::ipc::DeadlineAfterMs(5000), result.error) !=
                npucheck::ipc::IoStatus::OK)
                return;
            if (frame.type != npucheck::ipc::MessageType::RESULT) {
                result.error = "expected Result";
                return;
            }
            result.text.append(frame.payload.begin(), frame.payload.end());
            result.flags = frame.flags;
            if ((frame.flags & npucheck::ipc::FLAG_MORE) == 0)
                return;
        }
    });
    npucheck::ToolManager manager;
    result.initialized = manager.Initialize();
    if (result.initialized == 0) {
        if (onReady) {
            onReady();
        }
        if (nullCallback) {
            Emit(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, nullptr);
        }
        AclsanResourceData allocation{};
        allocation.common.version = ACLSAN_API_VERSION;
        allocation.common.size = sizeof(allocation);
        allocation.ptr = reinterpret_cast<void*>(0x1000);
        allocation.bytes = 32;
        allocation.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
        allocation.resourceId = 1;
        Emit(ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, &allocation);
        if (race) {
            AclsanLaunchData launch{};
            launch.common.version = ACLSAN_API_VERSION;
            launch.common.size = sizeof(launch);
            launch.launchId = 1;
            Emit(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL, &launch);
        }
        AclsanDeviceMemoryAccessData memory{};
        memory.header.version = ACLSAN_API_VERSION;
        memory.header.size = sizeof(memory);
        memory.header.pc = 0x170;
        memory.address = memoryError ? 0x1018 : 0x1000;
        memory.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
        memory.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
        memory.accessCount = 1;
        memory.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
        memory.layout.range.bytes = 16;
        if (initError) {
            memory.header.instrExecId = 1;
            memory.instructionId = 72;
            memory.regDependencyMask0 = UINT64_C(1) << ACLSAN_DEVICE_REGISTER_MTE2_SOURCE;
        }
        if (race) {
            memory.header.launchId = 1;
            memory.header.instrExecId = 1;
            memory.header.flags = ACLSAN_DEVICE_EVENT_FLAG_EXACT;
            memory.header.pipeline = ACLSAN_DEVICE_PIPE_MTE2;
            memory.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        }
        Emit(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &memory);
        if (race) {
            memory.header.instrExecId = 2;
            memory.header.pc = 0x180;
            memory.header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
            memory.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_READ;
            Emit(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS, &memory);
        }
        if (syncError) {
            AclsanDeviceSyncData sync{};
            sync.header.version = ACLSAN_API_VERSION;
            sync.header.size = sizeof(sync);
            sync.header.pc = 0x180;
            sync.syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
            sync.action = ACLSAN_DEVICE_SYNC_ACTION_WAIT;
            sync.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
            sync.srcPipe = ACLSAN_DEVICE_PIPE_VECTOR;
            sync.dstPipe = ACLSAN_DEVICE_PIPE_MTE2;
            sync.objectId = 7;
            Emit(ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC, &sync);
        }
        AclsanSynchronizeData sync{};
        sync.common.version = ACLSAN_API_VERSION;
        sync.common.size = sizeof(sync);
        sync.common.result = failSync ? 1 : 0;
        Emit(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync);
        manager.Finalize();
    }
    client.join();
    unsetenv(npucheck::ipc::UDS_NAME_ENV);
    unsetenv(npucheck::ipc::SESSION_ID_ENV);
    unsetenv(npucheck::ipc::CLI_PID_ENV);
    unsetenv(npucheck::ipc::HANDSHAKE_TIMEOUT_ENV);
    return result;
}
} // namespace

extern "C" aclError aclrtApiInjectionGetFunc(const char* name, aclrtApiFunc* origin, aclrtApiFunc*)
{
    if (std::string(name) != "aclrtSetOpExecuteTimeOut") {
        return ACL_ERROR_INVALID_PARAM;
    }
    *origin = timeoutApiAvailable ? reinterpret_cast<aclrtApiFunc>(&SetOpExecuteTimeout) : nullptr;
    return timeoutLookupStatus;
}

// Mock only the public sanitizer boundary; the CLI parser, UDS exchange,
// ToolManager, checkers and reporter all run their production implementations.
extern "C" AclsanStatus aclsanSubscribe(AclsanSubscriberHandle* handle, AclsanCallbackFunc cb, void* data)
{
    callback = cb;
    userdata = data;
    *handle = reinterpret_cast<AclsanSubscriberHandle>(0x1);
    return ACLSAN_STATUS_SUCCESS;
}
extern "C" AclsanStatus aclsanUnsubscribe(AclsanSubscriberHandle)
{
    callback = nullptr;
    return ACLSAN_STATUS_SUCCESS;
}
extern "C" AclsanStatus aclsanEnableCallback(
    uint32_t, AclsanSubscriberHandle, AclsanCallbackDomain domain, AclsanCallbackId id)
{
    ++enableCalls;
    enabled.insert({domain, id});
    return ACLSAN_STATUS_SUCCESS;
}
extern "C" AclsanStatus aclsanGetDeviceCallStack(uint64_t, AclsanDeviceCallStack*)
{
    return ACLSAN_STATUS_ERROR_INVALID_STATE;
}

TEST(ToolManagerTest, CombinedToolsReportBothAndSubscribeSharedCallbackOnce)
{
    const auto result = RunSession(
        {"npu-check", "--tool", "synccheck", "--tool", "memcheck", "--tool", "memcheck", "/bin/true"}, true, true);
    ASSERT_EQ(result.initialized, 0);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(enableCalls, 6U);
    EXPECT_EQ(enabled.size(), 6U);
    EXPECT_NE(result.text.find("tool=memcheck"), std::string::npos);
    EXPECT_NE(result.text.find("tool=synccheck"), std::string::npos);
    EXPECT_NE(result.text.find("status=complete"), std::string::npos);
    EXPECT_NE(result.text.find("Line information unavailable."), std::string::npos);
    EXPECT_EQ(result.text.find("api_status_"), std::string::npos);
    EXPECT_EQ(result.text.find("[CALL-STACK] pc="), std::string::npos);
    EXPECT_NE(result.flags & npucheck::ipc::FLAG_HAS_ERRORS, 0U);
}

TEST(ToolManagerTest, RacecheckConfigurationReachesGraphAndRejectsMalformedObservation)
{
    const auto session = RunSession({"npu-check", "--tool", "racecheck", "/bin/true"}, false, false);
    ASSERT_EQ(session.initialized, 0);
    ASSERT_TRUE(session.error.empty()) << session.error;
    EXPECT_EQ(enabled.size(), 5U);
    EXPECT_NE(session.text.find("tool=racecheck"), std::string::npos);
    EXPECT_NE(session.text.find("malformed_callback=1"), std::string::npos);
    EXPECT_NE(session.text.find("status=incomplete"), std::string::npos);
    EXPECT_NE(session.text.find("analysis_complete=false"), std::string::npos);
    EXPECT_EQ(session.text.find("WARNING:[RACECHECK]"), std::string::npos);
}

TEST(ToolManagerTest, RacecheckGraphPublishesMemstateEvidenceThroughCliSession)
{
    const auto session =
        RunSession({"npu-check", "--tool", "racecheck", "/bin/true"}, false, false, false, false, false, true);
    ASSERT_EQ(session.initialized, 0);
    ASSERT_TRUE(session.error.empty()) << session.error;
    EXPECT_NE(session.text.find("WARNING:[RACECHECK]"), std::string::npos);
    EXPECT_NE(session.text.find("RAW"), std::string::npos);
    EXPECT_NE(session.text.find("analysis_complete=true"), std::string::npos);
    EXPECT_NE(session.text.find("raw=1"), std::string::npos);
    EXPECT_NE(session.flags & npucheck::ipc::FLAG_HAS_ERRORS, 0U);
}

TEST(ToolManagerTest, RacecheckDoesNotSuppressOtherToolsReports)
{
    const auto session =
        RunSession({"npu-check", "--tool", "racecheck", "--tool", "memcheck", "/bin/true"}, true, false);
    ASSERT_EQ(session.initialized, 0);
    ASSERT_TRUE(session.error.empty()) << session.error;
    EXPECT_EQ(enabled.size(), 7U);
    EXPECT_NE(session.text.find("MEMCHECK SUMMARY: 1 errors"), std::string::npos);
    EXPECT_NE(session.text.find("tool=racecheck"), std::string::npos);
    EXPECT_NE(session.text.find("analysis_complete=false"), std::string::npos);
    EXPECT_NE(session.flags & npucheck::ipc::FLAG_HAS_ERRORS, 0U);
}

TEST(ToolManagerTest, ThreeToolsReportIndependentlyAndShareCallbacks)
{
    const auto result = RunSession(
        {"npu-check", "--tool", "memcheck", "--tool", "synccheck", "--tool", "initcheck", "/bin/true"}, false, false,
        false, false, true);
    ASSERT_EQ(result.initialized, 0);
    ASSERT_TRUE(result.error.empty()) << result.error;
    const decltype(enabled) expected{
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC},
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END}};
    EXPECT_EQ(enabled, expected);
    EXPECT_EQ(enableCalls, expected.size());
    EXPECT_NE(result.text.find("tool=memcheck"), std::string::npos);
    EXPECT_NE(result.text.find("tool=synccheck"), std::string::npos);
    EXPECT_NE(result.text.find("tool=initcheck"), std::string::npos);
    EXPECT_NE(result.text.find("Register mte2_source"), std::string::npos);
    EXPECT_NE(result.flags & npucheck::ipc::FLAG_HAS_ERRORS, 0U);
}

TEST(ToolManagerTest, ErrorFlagAggregatesEitherTool)
{
    for (auto errors : {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}}) {
        const auto result = RunSession(
            {"npu-check", "--tool", "memcheck", "--tool", "synccheck", "/bin/true"}, errors.first, errors.second);
        ASSERT_EQ(result.initialized, 0);
        ASSERT_TRUE(result.error.empty()) << result.error;
        EXPECT_EQ((result.flags & npucheck::ipc::FLAG_HAS_ERRORS) != 0, errors.first || errors.second);
        EXPECT_NE(result.text.find("status=complete"), std::string::npos);
    }
}

TEST(ToolManagerTest, SingleCheckerAndFailedSyncRetainIndependentState)
{
    const auto single = RunSession({"npu-check", "--tool", "synccheck", "/bin/true"}, true, true);
    ASSERT_TRUE(single.error.empty()) << single.error;
    EXPECT_EQ(enableCalls, 3U);
    EXPECT_EQ(single.text.find("tool=memcheck"), std::string::npos);
    EXPECT_NE(single.text.find("tool=synccheck"), std::string::npos);
    const auto incomplete =
        RunSession({"npu-check", "--tool", "memcheck", "--tool", "synccheck", "/bin/true"}, true, true, true);
    ASSERT_TRUE(incomplete.error.empty()) << incomplete.error;
    EXPECT_NE(incomplete.text.find("status=incomplete"), std::string::npos);
    EXPECT_NE(incomplete.text.find("pending_device_operations=0"), std::string::npos);
    EXPECT_NE(incomplete.text.find("MEMCHECK SUMMARY: 1 errors"), std::string::npos);
    EXPECT_NE(incomplete.flags & npucheck::ipc::FLAG_HAS_ERRORS, 0U);
}

TEST(ToolManagerTest, NullCallbackIsCountedOnceAndDoesNotReachCheckers)
{
    const auto session =
        RunSession({"npu-check", "--tool", "memcheck", "--tool", "synccheck", "/bin/true"}, false, false, false, true);
    ASSERT_TRUE(session.error.empty()) << session.error;
    EXPECT_NE(session.text.find("malformed_callbacks=1"), std::string::npos);
    EXPECT_NE(session.text.find("status=incomplete"), std::string::npos);
    EXPECT_NE(session.text.find("pending_device_operations=0"), std::string::npos);
    EXPECT_EQ(session.flags & npucheck::ipc::FLAG_HAS_ERRORS, 0U);
}

TEST(ToolManagerTest, AppliesConfiguredTimeoutBeforeEveryLaunch)
{
    timeoutValues.clear();
    timeoutLookupStatus = ACL_SUCCESS;
    timeoutSetStatus = ACL_SUCCESS;
    timeoutApiAvailable = true;
    const auto session =
        RunSession({"npu-check", "--deadlock-timeout", "5", "/bin/true"}, false, false, false, false, false, false, [] {
            for (uint64_t id : {1U, 2U}) {
                AclsanLaunchData launch{};
                launch.common.version = ACLSAN_API_VERSION;
                launch.common.size = sizeof(launch);
                launch.launchId = id;
                Emit(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL_BEFORE, &launch);
                EXPECT_EQ(launch.common.result, ACL_SUCCESS);
            }
        });
    ASSERT_EQ(session.initialized, 0);
    ASSERT_TRUE(session.error.empty()) << session.error;
    EXPECT_EQ(timeoutValues, (std::vector<uint32_t>{5, 5}));
}

TEST(ToolManagerTest, TimeoutSetupAndLookupFailuresLeaveLaunchResultUnchanged)
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        timeoutValues.clear();
        timeoutLookupStatus = scenario == 0 ? ACL_ERROR_INVALID_PARAM : ACL_SUCCESS;
        timeoutApiAvailable = scenario != 1;
        timeoutSetStatus = ACL_ERROR_RT_FEATURE_NOT_SUPPORT;
        const auto session = RunSession(
            {"npu-check", "--deadlock-timeout", "5", "/bin/true"}, false, false, false, false, false, false, [&] {
                AclsanLaunchData launch{};
                launch.common.version = ACLSAN_API_VERSION;
                launch.common.size = sizeof(launch);
                Emit(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL_BEFORE, &launch);
                EXPECT_EQ(launch.common.result, ACL_SUCCESS);
            });
        ASSERT_EQ(session.initialized, 0);
        ASSERT_TRUE(session.error.empty()) << session.error;
        EXPECT_EQ(timeoutValues.size(), scenario == 2 ? 1U : 0U);
    }
}

TEST(ToolManagerTest, StreamSyncTimeoutStillAllowsFinalResult)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            const auto session = RunSession(
                {"npu-check", "--tool", "synccheck", "--deadlock-timeout", "5", "/bin/true"}, false, false, false,
                false, false, false, [] {
                    AclsanSynchronizeData sync{};
                    sync.common.version = ACLSAN_API_VERSION;
                    sync.common.size = sizeof(sync);
                    sync.common.result = ACL_ERROR_RT_STREAM_SYNC_TIMEOUT;
                    Emit(ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync);
                });
            _exit(session.initialized == 0 && session.error.empty() && !session.text.empty() ? 0 : 1);
        },
        testing::ExitedWithCode(0), "");
}
