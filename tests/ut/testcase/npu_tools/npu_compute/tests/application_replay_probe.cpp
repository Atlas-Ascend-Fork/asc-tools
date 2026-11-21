/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "driver/ascend_hal_base.h"
#include "injection/prof_api_stub.h"
#include "injection/runtime_stub_api.h"
#include "profiling/prof_api.h"
#include "securec.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
MsprofRawDataCallback callback = nullptr;
std::map<uint32_t, int> starts;
std::map<uint32_t, int> stops;
std::map<uint32_t, int> expectedStops;
std::map<int32_t, int> deviceReferences;
int expectedStarts = 0;
bool resourcesAlive = true;
bool pauseReport = false;
bool communicationPayload = false;
bool silentFinalizeError = false;
bool replaceLastTaskOnDeviceQuery = false;
thread_local int32_t currentDevice = -1;
thread_local uint32_t lastTask = 0;
uint32_t observedLaunches = 0;
void Require(bool condition)
{
    if (!condition) {
        std::_Exit(42);
    }
}
aclError RecordLaunch()
{
    Require(resourcesAlive);
    ++lastTask;
    ++observedLaunches;
    if (replaceLastTaskOnDeviceQuery) {
        lastTask = observedLaunches;
    }
    return ACL_SUCCESS;
}
aclError SetDevice(int32_t deviceId)
{
    if (deviceId == 2) {
        return 37;
    }
    currentDevice = deviceId;
    ++deviceReferences[deviceId];
    return ACL_SUCCESS;
}
aclError ResetDevice(int32_t deviceId)
{
    Require(deviceReferences[deviceId] > 0);
    if (--deviceReferences[deviceId] != 0) {
        Require(stops[deviceId] < starts[deviceId]);
        return ACL_SUCCESS;
    }
    RuntimeStubEmitDeviceState(deviceId, ACL_RT_DEVICE_STATE_RESET_PRE);
    Require(starts[deviceId] == stops[deviceId]);
    RuntimeStubEmitDeviceState(deviceId, ACL_RT_DEVICE_STATE_RESET_POST);
    return ACL_SUCCESS;
}
void ReleaseRuntimeResources()
{
    int total = 0;
    for (const auto& [id, count] : starts) {
        Require(stops[id] == expectedStops[id]);
        total += count;
    }
    Require(total == expectedStarts);
    Require(observedLaunches == 0 || observedLaunches == 140 || observedLaunches == 1000);
    resourcesAlive = false;
    std::puts("application-probe:ok");
}
const MsprofConfig& DeviceConfig(uint32_t type, const void* data, uint32_t bytes)
{
    Require(type == 8 && data != nullptr && bytes == sizeof(MsprofConfig));
    const auto& config = *static_cast<const MsprofConfig*>(data);
    Require(config.devNums == 1 && config.profSwitch == 0x10000080eULL);
    return config;
}

void EmitRaw(int32_t deviceId)
{
    MsprofRawData raw{};
    raw.deviceId = deviceId;
    raw.type = ADDITIONAL_DATA_TYPE;
    raw.isLastChunk = true;
    raw.chunkSize = 3;
    std::memcpy(raw.chunk, "raw", raw.chunkSize);
    if (!communicationPayload) {
        Require(callback(&raw) == 0);
        return;
    }

    const auto put = [&](size_t at, uint64_t value, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            raw.chunk[at + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    constexpr uint32_t taskId = (6U << 16) | 4U;
    constexpr uint64_t start = 1000000;
    constexpr uint64_t end = start + 1000;
    const int32_t taskLogType = 7;
    Require(memcpy_s(&raw.type, sizeof(raw.type), &taskLogType, sizeof(taskLogType)) == EOK);
    std::memset(raw.chunk, 0, sizeof(raw.chunk));
    raw.chunkSize = 64;
    // An AICPU task has paired 32-byte Stars TaskLog records.
    constexpr uint32_t aicpuHeader = (0x6bd3U << 16) | (4U << 10);
    put(0, aicpuHeader, 4);
    put(4, taskId, 4);
    put(8, start, 8);
    put(32, aicpuHeader | 1U, 4);
    put(36, taskId, 4);
    put(40, end, 8);
    Require(callback(&raw) == 0);

    const int32_t additionalType = 12;
    Require(memcpy_s(&raw.type, sizeof(raw.type), &additionalType, sizeof(additionalType)) == EOK);
    std::memset(raw.chunk, 0, sizeof(raw.chunk));
    raw.chunkSize = 256;
    // HCCL metadata carries the task identity and the start timestamp.
    put(0, 0x5a5a, 2);
    put(2, 6000, 2);
    put(4, 13, 4);
    put(12, 116, 4);
    put(24 + 48, start, 8);
    put(24 + 88, taskId, 4);
    put(24 + 96, 4, 2);
    Require(callback(&raw) == 0);
    std::memset(raw.chunk, 0, raw.chunkSize);
}

int RunFinalizeWithActiveHook()
{
    using namespace std::chrono_literals;
    static std::atomic<bool> entered{false};
    static std::atomic<bool> released{false};
    static std::atomic<bool> timedOut{false};
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtSetDevice", +[](int32_t) -> aclError {
                entered = true;
                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (!released && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(1ms);
                }
                timedOut = !released;
                return 37;
            }) == ACL_SUCCESS);
    Require(aclInit(nullptr) == ACL_SUCCESS);
    std::thread runtime([] { Require(aclrtSetDevice(2) == 37); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!entered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    Require(entered);
    Require(aclFinalize() == ACL_SUCCESS);
    released = true;
    runtime.join();
    Require(!timedOut);
    MsprofRawData late{};
    late.chunkSize = 1;
    Require(callback != nullptr && callback(&late) != 0);
    return 0;
}

void NotifyGate(const char* fdName)
{
    if (const char* descriptor = std::getenv(fdName)) {
        Require(write(std::atoi(descriptor), "x", 1) == 1);
    }
}

int RunForked(const std::string& scenario)
{
    Require(RuntimeStubSetOriginFunction("aclrtSetDevice", &SetDevice) == ACL_SUCCESS);
    // Simulate the Runtime constructor loading the injection library in the parent.
    Require(ProfApiLoadApiInjectionFromEnv() == 0);
    Require(callback == nullptr);
    pid_t children[2];
    for (int32_t rank = 0; rank < 2; ++rank) {
        const pid_t child = fork();
        Require(child >= 0);
        if (child == 0) {
            pauseReport = rank == 0 && scenario == "fork-gate";
            communicationPayload = scenario == "fork-communication";
            Require(aclInit(nullptr) == ACL_SUCCESS);
            Require(aclrtSetDevice(rank) == ACL_SUCCESS);
            if (rank == 1 && scenario == "fork-disconnect") {
                EmitRaw(rank);
                _exit(0);
            }
            Require(aclFinalize() == ACL_SUCCESS);
            if (rank == 1) {
                NotifyGate("NPU_COMPUTE_TEST_SIBLING_DONE_FD");
            }
            // Runtime may still retain the stable uploader after session shutdown.
            MsprofRawData late{};
            late.chunkSize = 1;
            Require(callback != nullptr && callback(&late) != 0);
            _exit(rank == 1 && scenario == "fork-app-failure" ? 17 : 0);
        }
        children[rank] = child;
    }
    int appStatus = 0;
    for (pid_t child : children) {
        int status = 0;
        Require(waitpid(child, &status, 0) == child && WIFEXITED(status));
        if (WEXITSTATUS(status) != 0) {
            appStatus = WEXITSTATUS(status);
        }
    }
    return appStatus;
}
} // namespace

extern "C" void DlogRecord(int32_t, int32_t, const char* format, ...)
{
    if (pauseReport && std::strstr(format, "application_report finish begin") != nullptr) {
        NotifyGate("NPU_COMPUTE_TEST_REPORT_PAUSED_FD");
        const char* descriptor = std::getenv("NPU_COMPUTE_TEST_REPORT_RELEASE_FD");
        Require(descriptor != nullptr);
        char signal;
        ssize_t count;
        do {
            count = read(std::atoi(descriptor), &signal, 1);
        } while (count < 0 && errno == EINTR);
        Require(count == 1 && signal == 'x');
    }
}

extern "C" aclError aclrtGetDeviceCount(uint32_t* count)
{
    Require(resourcesAlive && count != nullptr);
    *count = 8;
    return ACL_SUCCESS;
}

extern "C" aclError aclrtGetThreadLastTaskIdAndStreamId(uint32_t* task, uint32_t* stream)
{
    *task = lastTask;
    *stream = 3;
    return ACL_SUCCESS;
}

extern "C" drvError_t halGetDeviceInfo(uint32_t, int32_t module, int32_t type, int64_t* value)
{
    if (module != MODULE_TYPE_SYSTEM || value == nullptr) {
        return static_cast<drvError_t>(1);
    }
    if (type == INFO_TYPE_DEV_OSC_FREQUE) {
        *value = 1000000;
        return DRV_ERROR_NONE;
    }
    if (type == INFO_TYPE_SYS_COUNT) {
        *value = 0;
        return DRV_ERROR_NONE;
    }
    return static_cast<drvError_t>(1);
}

extern "C" int32_t MsprofRegisterDataCallback(uint32_t type, void* function)
{
    Require(type == 0 && function != nullptr);
    callback = reinterpret_cast<MsprofRawDataCallback>(function);
    return 0;
}
extern "C" int32_t MsprofStart(uint32_t type, const void* data, uint32_t bytes)
{
    const auto& config = DeviceConfig(type, data, bytes);
    Require(resourcesAlive);
    ++starts[config.devIdList[0]];
    return 0;
}
extern "C" int32_t MsprofStop(uint32_t type, const void* data, uint32_t bytes)
{
    const auto& config = DeviceConfig(type, data, bytes);
    Require(resourcesAlive && callback != nullptr);
    EmitRaw(static_cast<int32_t>(config.devIdList[0]));
    ++stops[config.devIdList[0]];
    return 0;
}

extern "C" int32_t MsprofFinalize()
{
    std::vector<std::thread> workers;
    for (const auto& [id, count] : starts) {
        if (count > stops[id]) {
            workers.emplace_back([id] { EmitRaw(static_cast<int32_t>(id)); });
        }
    }
    for (auto& worker : workers) {
        worker.join();
    }
    // The subsequent ACL notification must neither stop profiling nor access device resources.
    resourcesAlive = false;
    return silentFinalizeError ? -77 : 0;
}

int main(int argc, char** argv)
{
    Require(argc == 2);
    const std::string scenario(argv[1]);
    if (scenario == "finalize-busy") {
        return RunFinalizeWithActiveHook();
    }
    if (scenario.compare(0, 5, "fork-") == 0) {
        return RunForked(scenario);
    }
    communicationPayload = scenario == "communication-finalize";
    const bool cleanupFailure = scenario == "cleanup-failure";
    const bool finalizes = scenario.find("finalize") != std::string::npos || cleanupFailure;
    const bool partialReset = scenario == "partial-reset-finalize";
    const bool restarts = scenario == "restart" || scenario == "restart-finalize";
    const bool resetsDevices = scenario == "reset" || scenario == "reset-finalize" || restarts;
    silentFinalizeError = scenario == "silent-finalize-error";
    replaceLastTaskOnDeviceQuery = scenario == "launch-variants-finalize";
    Require(RuntimeStubSetOriginFunction("aclrtSetDevice", &SetDevice) == ACL_SUCCESS);
    Require(RuntimeStubSetOriginFunction("aclrtResetDevice", &ResetDevice) == ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtGetDevice", +[](int32_t* device) -> aclError {
                *device = currentDevice;
                if (replaceLastTaskOnDeviceQuery) {
                    // A metadata query must not overwrite the launch identity before it is captured.
                    lastTask = 0;
                }
                return currentDevice >= 0 ? ACL_SUCCESS : ACL_ERROR_INVALID_PARAM;
            }) == ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtBinaryGetFunction", +[](aclrtBinHandle, const char*, aclrtFuncHandle* handle) -> aclError {
                *handle = reinterpret_cast<aclrtFuncHandle>(0x1000);
                return ACL_SUCCESS;
            }) == ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchKernel", +[](aclrtFuncHandle, uint32_t, const void*, size_t, aclrtStream) -> aclError {
                return RecordLaunch();
            }) == ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchKernelWithConfig",
            +[](aclrtFuncHandle, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, aclrtArgsHandle, void*) -> aclError {
                return RecordLaunch();
            }) == ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchKernelWithHostArgs",
            +[](aclrtFuncHandle, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, void*, size_t, aclrtPlaceHolderInfo*,
                size_t) -> aclError { return RecordLaunch(); }) == ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchKernelWithArgsArray",
            +[](void*, uint32_t, aclrtStream, aclrtLaunchKernelCfg*, void**) -> aclError { return RecordLaunch(); }) ==
        ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchSIMTKernelWithHostArgs",
            +[](void*, dim3, dim3, size_t, aclrtStream, aclrtLaunchKernelCfg*, void*, size_t, aclrtPlaceHolderInfo*,
                size_t) -> aclError { return RecordLaunch(); }) == ACL_SUCCESS);
    Require(
        RuntimeStubSetOriginFunction(
            "aclrtLaunchSIMTKernelWithArgsArray",
            +[](void*, dim3, dim3, size_t, aclrtStream, aclrtLaunchKernelCfg*, void**) -> aclError {
                return RecordLaunch();
            }) == ACL_SUCCESS);
    if (cleanupFailure) {
        Require(
            aclFinalizeCallbackRegister(
                ACL_REG_TYPE_OTHER, +[](void*) -> aclError { return ACL_ERROR_FAILURE; }, nullptr) == ACL_SUCCESS);
    }
    Require(aclInit(nullptr) == ACL_SUCCESS);
    // Registered before the first SetDevice/MsprofStart, like a lazily initialized Runtime keeper.
    Require(std::atexit(&ReleaseRuntimeResources) == 0);
    if (scenario == "no-device" || scenario == "finalize-no-device") {
        if (finalizes) {
            Require(aclFinalize() == ACL_SUCCESS);
        }
        return 0;
    }
    expectedStarts = restarts ? 3 : 2;
    Require(aclrtSetDevice(2) == 37);
    Require(aclrtSetDevice(0) == ACL_SUCCESS);
    Require(aclrtSetDevice(0) == ACL_SUCCESS);
    auto launchSummaries = [&] {
        const bool variants = scenario == "launch-variants-finalize";
        if (scenario != "summary-finalize" && scenario != "large-summary-finalize" && !variants) {
            return;
        }
        aclrtFuncHandle handle = reinterpret_cast<aclrtFuncHandle>(0x1000);
        Require(aclrtBinaryGetFunction(nullptr, "comm/\"kernel", &handle) == ACL_SUCCESS);
        for (uint32_t i = 0; i < (scenario == "large-summary-finalize" ? 500U : 70U); ++i) {
            switch (variants ? i % 6 : 0) {
                case 0:
                    Require(aclrtLaunchKernel(handle, 2, nullptr, 0, nullptr) == ACL_SUCCESS);
                    break;
                case 1:
                    Require(aclrtLaunchKernelWithConfig(handle, 2, nullptr, nullptr, nullptr, nullptr) == ACL_SUCCESS);
                    break;
                case 2:
                    Require(
                        aclrtLaunchKernelWithHostArgs(handle, 2, nullptr, nullptr, nullptr, 0, nullptr, 0) ==
                        ACL_SUCCESS);
                    break;
                case 3:
                    Require(aclrtLaunchKernelWithArgsArray(handle, 2, nullptr, nullptr, nullptr) == ACL_SUCCESS);
                    break;
                case 4:
                    Require(
                        aclrtLaunchSIMTKernelWithHostArgs(
                            handle, dim3(2, 1, 1), dim3(32, 1, 1), 0, nullptr, nullptr, nullptr, 0, nullptr, 0) ==
                        ACL_SUCCESS);
                    break;
                default:
                    Require(
                        aclrtLaunchSIMTKernelWithArgsArray(
                            handle, dim3(2, 1, 1), dim3(32, 1, 1), 0, nullptr, nullptr, nullptr) == ACL_SUCCESS);
                    break;
            }
        }
    };
    launchSummaries();
    if (scenario == "reference-only" || scenario == "reference-finalize") {
        Require(aclInit(nullptr) == ACL_ERROR_REPEAT_INITIALIZE);
        uint64_t references = 0;
        Require(aclFinalizeReference(&references) == ACL_SUCCESS && references == 1);
        Require(resourcesAlive && stops[0] == 0);
    }
    Require(aclrtSetDevice(1) == ACL_SUCCESS);
    launchSummaries();
    if (resetsDevices || partialReset) {
        Require(aclrtResetDevice(0) == ACL_SUCCESS);
        Require(stops[0] == 0);
        Require(aclrtResetDevice(0) == ACL_SUCCESS);
        if (!partialReset) {
            Require(aclrtResetDevice(1) == ACL_SUCCESS);
        }
    }
    if (restarts) {
        Require(aclrtSetDevice(0) == ACL_SUCCESS);
        Require(aclrtResetDevice(0) == ACL_SUCCESS);
    }
    for (const auto& [id, count] : starts) {
        expectedStops[id] = resetsDevices ? count : (partialReset && id == 0 ? 1 : 0);
    }
    if (scenario == "return-with-raw" || scenario == "exit-with-raw") {
        EmitRaw(0);
        EmitRaw(1);
        if (scenario == "exit-with-raw") {
            std::exit(0);
        }
    }
    if (finalizes) {
        if (scenario == "reference-finalize") {
            uint64_t references = 1;
            Require(aclFinalizeReference(&references) == ACL_SUCCESS && references == 0);
        } else {
            Require(aclFinalize() == (cleanupFailure ? ACL_ERROR_FAILURE : ACL_SUCCESS));
        }
    }
    return 0;
}
