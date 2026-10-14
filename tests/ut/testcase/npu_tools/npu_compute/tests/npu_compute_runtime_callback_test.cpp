/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute/acl_pti_callback_stub.h"
#include "npu_compute/npu_compute.h"
#include "runtime/npu_compute_runtime.h"

#include <acl/acl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void NpuComputeTestConfigure(const char*, const char*);

namespace {

constexpr char kSections[] = "PipeUtilization,Memory";
constexpr char kHardwareInfoFile[] = "HardwareInfo.jsonl";
constexpr char kDeviceCountFile[] = "device_count.calls";
constexpr std::array<aclptiCallbackId, 9> kHardwareInfoTriggerCbids = {
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernel,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry,
    ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad,
    ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol,
};
using namespace std::chrono_literals;

std::mutex g_deviceCountMutex;
std::condition_variable g_deviceCountCondition;
bool g_blockDeviceCount = false;
bool g_deviceCountStarted = false;
bool g_releaseDeviceCount = false;
aclptiProfilingDataCallback g_profilingDataCallback;

#define CHECK(expression)                                                                 \
    do {                                                                                  \
        if (!(expression)) {                                                              \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #expression); \
            return false;                                                                 \
        }                                                                                 \
    } while (false)

class TempDirectory {
public:
    TempDirectory()
    {
        std::string pathTemplate =
            (boost::filesystem::temp_directory_path() / "npu-compute-runtime-callback-test-XXXXXX").string();
        pathTemplate.push_back('\0');
        char* created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TempDirectory()
    {
        if (!path_.empty()) {
            boost::system::error_code error;
            boost::filesystem::remove_all(path_, error);
        }
    }

    const boost::filesystem::path& Path() const { return path_; }

private:
    boost::filesystem::path path_;
};

boost::filesystem::path g_testOutput;

bool SetScenarioEnvironment(const boost::filesystem::path& output)
{
    g_testOutput = output;
    NpuComputeTestConfigure(output.c_str(), kSections);
    return true;
}

bool TestPipelineSection()
{
    npucompute::SectionConfig config;
    std::string error;
    CHECK(config.Load({"Pipeline"}, &error));
    CHECK(config.PipelineEnabled());
    CHECK(config.Params()->numSections == 0);
    CHECK(config.Load({"Memory", "Pipeline"}, &error));
    CHECK(config.PipelineEnabled());
    CHECK(config.Sections() == std::vector<std::string>{"Memory"});
    CHECK(config.JoinedSections() == "Memory,Pipeline");
    CHECK(config.Load({"Memory"}, &error));
    CHECK(!config.PipelineEnabled());
    CHECK(!config.Load({"pipeline"}, &error));
    return true;
}

bool CheckSubscribeAndEnableContract()
{
    using npucompute::test::AclPtiEnableCall;

    CHECK(npucompute::test::AclPtiSubscribeCount() == 1);
    CHECK(npucompute::test::CapturedAclPtiCallback() != nullptr);
    CHECK(npucompute::test::CapturedAclPtiUserData() != nullptr);
    CHECK(npucompute::test::CapturedAclPtiUserData() == static_cast<void*>(&npucompute::NpuComputeRuntime::Instance()));
    const aclptiSubscribeHandle subscriber = npucompute::test::CapturedAclPtiSubscriber();
    CHECK(subscriber != nullptr);

    const std::vector<AclPtiEnableCall> enableCalls = npucompute::test::CapturedAclPtiEnableCalls();
    CHECK(enableCalls.size() == kHardwareInfoTriggerCbids.size());
    std::size_t previousSequence = npucompute::test::AclPtiSubscribeSequence();
    CHECK(previousSequence > 0);
    for (std::size_t index = 0; index < enableCalls.size(); ++index) {
        const AclPtiEnableCall& call = enableCalls[index];
        CHECK(call.sequence > previousSequence);
        CHECK(call.enable);
        CHECK(call.subscriber == subscriber);
        CHECK(call.domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
        CHECK(call.cbid == kHardwareInfoTriggerCbids[index]);
        CHECK(call.result == ACLPTI_SUCCESS);
        previousSequence = call.sequence;
    }

    CHECK(npucompute::test::AclPtiRangeConfigCount() == 1);
    CHECK(npucompute::test::AclPtiRangeConfigSequence() > previousSequence);
    const std::vector<std::string> sections = npucompute::test::CapturedAclPtiSections();
    CHECK(sections == std::vector<std::string>({"PipeUtilization", "Memory"}));
    CHECK(npucompute::test::CapturedAclPtiBlockResult() == ACLPTI_BLOCK_RESULT_ALL);
    CHECK(!npucompute::test::CapturedAclPtiCollectPipeline());
    CHECK(!npucompute::test::CapturedAclPtiCollectPcSampling());
    return true;
}

bool CheckDisableContract()
{
    using npucompute::test::AclPtiEnableCall;

    const std::vector<AclPtiEnableCall> calls = npucompute::test::CapturedAclPtiEnableCalls();
    CHECK(calls.size() == kHardwareInfoTriggerCbids.size() * 2);
    for (std::size_t index = 0; index < kHardwareInfoTriggerCbids.size(); ++index) {
        const AclPtiEnableCall& call = calls[kHardwareInfoTriggerCbids.size() + index];
        CHECK(!call.enable);
        CHECK(call.domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
        CHECK(call.cbid == kHardwareInfoTriggerCbids[kHardwareInfoTriggerCbids.size() - index - 1]);
        CHECK(call.result == ACLPTI_SUCCESS);
    }
    return true;
}

void InvokeCallbackDirectly(aclptiCallbackDomain domain, aclptiCallbackId cbid, const aclptiCallbackData& callbackData)
{
    const aclptiCallbackFunc callback = npucompute::test::CapturedAclPtiCallback();
    if (callback != nullptr) {
        callback(npucompute::test::CapturedAclPtiUserData(), domain, cbid, &callbackData);
    }
}

std::size_t CountLines(const boost::filesystem::path& path);

std::vector<std::string> SplitCsvLine(const std::string& line)
{
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const std::size_t end = line.find(',', begin);
        values.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return values;
}

std::string CsvValue(const boost::filesystem::path& path, const std::string& rowName, const std::string& columnName)
{
    std::ifstream input(path.string());
    std::string line;
    if (!std::getline(input, line)) {
        return {};
    }
    const std::vector<std::string> header = SplitCsvLine(line);
    const auto column = std::find(header.begin(), header.end(), columnName);
    if (column == header.end()) {
        return {};
    }
    const std::size_t columnIndex = static_cast<std::size_t>(std::distance(header.begin(), column));
    while (std::getline(input, line)) {
        const std::vector<std::string> row = SplitCsvLine(line);
        if (row.size() > columnIndex && row.size() > 1 && row[1] == rowName) {
            return row[columnIndex];
        }
    }
    return {};
}

bool RunCsvSocNameChild(const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    CHECK(::setenv("NPU_COMPUTE_FREQUENCY_MHZ", "1000", 1) == 0);
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(g_profilingDataCallback != nullptr);

    auto result = std::make_shared<aclptiProfilingDataResult>();
    aclptiPmuDataRow row{};
    row.blockId = 0;
    row.subBlockId = 0;
    row.coreType = ACLPTI_CORE_TYPE_AIV;
    row.coreId = 0;
    row.totalCycles = 1000.0;
    row.values = {{0x422U, 100.0}, {0x57fU, 10.0}, {0x580U, 10.0}};
    result->pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, std::move(row));

    CHECK(g_profilingDataCallback(result) == ACLPTI_SUCCESS);
    npucompute::NpuComputeRuntime::Instance().Stop();
    CHECK(npucompute::NpuComputeRuntime::Instance().ShutdownAfterPtiDrain() == 0);
    CHECK(CsvValue(output / "Memory.csv", "vector0", "GM_to_UB_bw_usage_rate(%)") == "5.358925");
    return true;
}

bool RunPipelineClockChild(const boost::filesystem::path& output, bool failQuery)
{
    CHECK(SetScenarioEnvironment(output));
    NpuComputeTestConfigure(output.c_str(), "Pipeline");
    CHECK(::unsetenv("NPU_COMPUTE_SYSCNT_FREQUENCY_MHZ") == 0);
    CHECK(::setenv("NPU_COMPUTE_FREQUENCY_MHZ", "1000", 1) == 0);
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(npucompute::test::CapturedAclPtiCollectPipeline());
    CHECK(g_profilingDataCallback != nullptr);
    auto result = std::make_shared<aclptiProfilingDataResult>();
    auto& replay = result->pipelineData[7];
    replay.deviceId = failQuery ? 4 : 3;
    replay.format = aclptiBiuFormat::Chip6;
    replay.channels[{0, aclptiBiuCoreKind::Aic}] = {0xe00003e8U, 0xe0000000U, 0xe0000000U, 0xe0000000U,
                                                    0xf0000000U, 0xf001000aU, 0xf0000014U, 0xf0010004U};
    if (failQuery) {
        result->pipelineData[6] = replay;
        result->pipelineData[6].deviceId = 3;
    }
    CHECK(g_profilingDataCallback(result) == ACLPTI_SUCCESS);
    npucompute::NpuComputeRuntime::Instance().Stop();
    CHECK(
        npucompute::NpuComputeRuntime::Instance().ShutdownAfterPtiDrain() ==
        (failQuery ? npucompute::kInitializeFailed : 0));
    const auto published = output / ".biu-staging/process-uds";
    CHECK(boost::filesystem::is_regular_file(published / "manifest.json"));
    CHECK(boost::filesystem::is_regular_file(
        published / (failQuery ? "result-0-device-3-replay-6.json" : "result-0-device-3-replay-7.json")));
    CHECK(!boost::filesystem::exists(published / "result-0-device-4-replay-7.json"));
    return true;
}

bool RunSuccessChild(const std::string& scenario, const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(CheckSubscribeAndEnableContract());

    if (scenario == "success-launch") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_EXIT, ACL_SUCCESS,
            nullptr));
        CHECK(boost::filesystem::is_regular_file(output / kHardwareInfoFile));
        CHECK(CountLines(output / kHardwareInfoFile) == 5);
        return true;
    }
    if (scenario == "success-host-args") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(boost::filesystem::is_regular_file(output / kHardwareInfoFile));
        CHECK(CountLines(output / kHardwareInfoFile) == 5);
        return true;
    }
    if (scenario == "success-simt-host-args") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(boost::filesystem::is_regular_file(output / kHardwareInfoFile));
        CHECK(CountLines(output / kHardwareInfoFile) == 5);
        return true;
    }
    if (scenario == "success-args-array") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(boost::filesystem::is_regular_file(output / kHardwareInfoFile));
        CHECK(CountLines(output / kHardwareInfoFile) == 5);
        return true;
    }
    if (scenario == "success-repeated") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_EXIT, ACL_SUCCESS,
            nullptr));
        CHECK(boost::filesystem::is_regular_file(output / kHardwareInfoFile));
        CHECK(CountLines(output / kHardwareInfoFile) == 5);
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_EXIT, ACL_SUCCESS,
            nullptr));
        return true;
    }
    return false;
}

void InvokeSuccessfulExitDirectly(aclptiCallbackId cbid)
{
    const aclptiCallbackData callbackData{ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_EXIT, nullptr, ACL_SUCCESS};
    InvokeCallbackDirectly(ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, callbackData);
}

bool RunNormalStopChild(const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(CheckSubscribeAndEnableContract());
    npucompute::NpuComputeRuntime::Instance().Stop();
    CHECK(CheckDisableContract());
    for (aclptiCallbackId cbid : kHardwareInfoTriggerCbids) {
        CHECK(!npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr));
    }
    return true;
}

bool RunStopDuringCollectionChild(const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    npucompute::test::ResetAclPtiCallbackStub();
    {
        std::lock_guard<std::mutex> lock(g_deviceCountMutex);
        g_blockDeviceCount = true;
        g_deviceCountStarted = false;
        g_releaseDeviceCount = false;
    }
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(CheckSubscribeAndEnableContract());

    std::atomic<bool> callbackReturned{false};
    std::atomic<bool> callbackDispatched{false};
    std::thread callbackThread([&callbackReturned, &callbackDispatched] {
        callbackDispatched = npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr);
        callbackReturned = true;
    });

    bool deviceCountStarted = false;
    {
        std::unique_lock<std::mutex> lock(g_deviceCountMutex);
        deviceCountStarted = g_deviceCountCondition.wait_for(lock, 10s, [] { return g_deviceCountStarted; });
    }

    std::atomic<bool> stopReturned{false};
    std::thread stopThread([&stopReturned] {
        npucompute::NpuComputeRuntime::Instance().Stop();
        stopReturned = true;
    });

    bool callbacksDisabled = false;
    if (deviceCountStarted) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (npucompute::test::CapturedAclPtiEnableCalls().size() == kHardwareInfoTriggerCbids.size() * 2) {
                callbacksDisabled = true;
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    std::atomic<bool> drainReturned{false};
    std::atomic<int> drainStatus{npucompute::kInitializeFailed};
    std::thread drainThread([&drainReturned, &drainStatus] {
        drainStatus = npucompute::NpuComputeRuntime::Instance().ShutdownAfterPtiDrain();
        drainReturned = true;
    });
    const auto drainDeadline = std::chrono::steady_clock::now() + 500ms;
    while (!drainReturned.load() && std::chrono::steady_clock::now() < drainDeadline) {
        std::this_thread::sleep_for(1ms);
    }

    const bool callbackReturnedBeforeRelease = callbackReturned.load();
    const bool stopReturnedBeforeRelease = stopReturned.load();
    const bool drainReturnedBeforeRelease = drainReturned.load();
    {
        std::lock_guard<std::mutex> lock(g_deviceCountMutex);
        g_releaseDeviceCount = true;
    }
    g_deviceCountCondition.notify_all();
    callbackThread.join();
    stopThread.join();
    drainThread.join();

    CHECK(deviceCountStarted);
    CHECK(callbacksDisabled);
    CHECK(callbackDispatched.load());
    CHECK(!callbackReturnedBeforeRelease);
    CHECK(!stopReturnedBeforeRelease);
    CHECK(drainReturnedBeforeRelease);
    CHECK(drainStatus.load() == 0);
    CHECK(CheckDisableContract());
    CHECK(boost::filesystem::is_regular_file(output / kHardwareInfoFile));
    CHECK(CountLines(output / kHardwareInfoFile) == 5);
    return true;
}

bool RunIgnoredEventChild(const std::string& scenario, const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(CheckSubscribeAndEnableContract());

    if (scenario == "ignore-enter") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_ENTER, ACL_SUCCESS,
            nullptr));
        return true;
    }
    if (scenario == "ignore-failed-exit") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_ERROR_INVALID_PARAM, nullptr));
        return true;
    }
    if (scenario == "ignore-simt-failed-exit") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_ERROR_INVALID_PARAM, nullptr));
        return true;
    }
    if (scenario == "ignore-set-device") {
        InvokeSuccessfulExitDirectly(ACLPTI_RUNTIME_CBID_aclrtSetDevice);
        return true;
    }
    if (scenario == "ignore-malloc") {
        InvokeSuccessfulExitDirectly(ACLPTI_RUNTIME_CBID_aclrtMalloc);
        return true;
    }
    if (scenario == "ignore-nontarget") {
        const aclptiCallbackData callbackData{
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMemcpy, ACLPTI_API_EXIT, nullptr, ACL_SUCCESS};
        InvokeCallbackDirectly(ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtMemcpy, callbackData);
        return true;
    }
    if (scenario == "ignore-domain") {
        const aclptiCallbackData callbackData{
            ACLPTI_CB_DOMAIN_INVALID, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_EXIT, nullptr, ACL_SUCCESS};
        InvokeCallbackDirectly(ACLPTI_CB_DOMAIN_INVALID, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, callbackData);
        return true;
    }
    if (scenario == "ignore-mismatched-metadata") {
        const aclptiCallbackData callbackData{
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, ACLPTI_API_EXIT, nullptr,
            ACL_SUCCESS};
        InvokeCallbackDirectly(ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, callbackData);
        return true;
    }
    return false;
}

bool RunSubscribeFailureChild(const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    npucompute::test::ResetAclPtiCallbackStub();
    npucompute::test::SetAclPtiSubscribeResult(ACLPTI_ERROR_INITIALIZATION_FAILED);
    CHECK(acltoolInitialize() == ACLPTI_ERROR_INITIALIZATION_FAILED);
    CHECK(npucompute::test::AclPtiSubscribeCount() == 1);
    CHECK(npucompute::test::AclPtiEnableCount() == 0);
    CHECK(npucompute::test::AclPtiRangeConfigCount() == 0);
    CHECK(npucompute::test::CapturedAclPtiCallback() == nullptr);
    CHECK(npucompute::test::CapturedAclPtiUserData() == nullptr);
    CHECK(npucompute::test::CapturedAclPtiSubscriber() == nullptr);
    return true;
}

bool RunEnableFailureChild(const boost::filesystem::path& output, aclptiCallbackId failedCbid)
{
    CHECK(SetScenarioEnvironment(output));
    npucompute::test::ResetAclPtiCallbackStub();
    npucompute::test::SetAclPtiEnableResult(failedCbid, ACLPTI_ERROR_NOT_SUPPORTED);
    CHECK(acltoolInitialize() == ACLPTI_ERROR_NOT_SUPPORTED);
    CHECK(npucompute::test::AclPtiSubscribeCount() == 1);
    CHECK(npucompute::test::AclPtiRangeConfigCount() == 0);

    const auto failurePosition =
        std::find(kHardwareInfoTriggerCbids.begin(), kHardwareInfoTriggerCbids.end(), failedCbid);
    CHECK(failurePosition != kHardwareInfoTriggerCbids.end());
    const std::size_t successfulEnableCount =
        static_cast<std::size_t>(std::distance(kHardwareInfoTriggerCbids.begin(), failurePosition));
    const std::vector<npucompute::test::AclPtiEnableCall> calls = npucompute::test::CapturedAclPtiEnableCalls();
    CHECK(calls.size() == successfulEnableCount * 2 + 1);

    for (std::size_t index = 0; index <= successfulEnableCount; ++index) {
        CHECK(calls[index].enable);
        CHECK(calls[index].domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
        CHECK(calls[index].cbid == kHardwareInfoTriggerCbids[index]);
        CHECK(calls[index].result == (index == successfulEnableCount ? ACLPTI_ERROR_NOT_SUPPORTED : ACLPTI_SUCCESS));
    }
    for (std::size_t index = 0; index < successfulEnableCount; ++index) {
        const aclptiCallbackId enabledCbid = kHardwareInfoTriggerCbids[index];
        std::size_t disableCount = 0;
        for (std::size_t callIndex = successfulEnableCount + 1; callIndex < calls.size(); ++callIndex) {
            if (!calls[callIndex].enable && calls[callIndex].cbid == enabledCbid &&
                calls[callIndex].result == ACLPTI_SUCCESS) {
                ++disableCount;
            }
        }
        CHECK(disableCount == 1);
        CHECK(!npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, enabledCbid, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr));
    }
    return true;
}

bool RunConfigFailureChild(const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    npucompute::test::ResetAclPtiCallbackStub();
    npucompute::test::SetAclPtiRangeConfigResult(ACLPTI_ERROR_PROFILING_FAILED);
    CHECK(acltoolInitialize() == ACLPTI_ERROR_PROFILING_FAILED);
    CHECK(npucompute::test::AclPtiSubscribeCount() == 1);
    CHECK(npucompute::test::AclPtiRangeConfigCount() == 1);
    CHECK(CheckDisableContract());
    for (aclptiCallbackId cbid : kHardwareInfoTriggerCbids) {
        CHECK(!npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr));
    }
    return true;
}

bool RunSectionFailureChild(const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    NpuComputeTestConfigure(output.c_str(), "invalid-section");
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == npucompute::kInitializeFailed);
    CHECK(npucompute::test::AclPtiSubscribeCount() == 0);
    CHECK(npucompute::test::AclPtiEnableCount() == 0);
    CHECK(npucompute::test::AclPtiRangeConfigCount() == 0);
    return true;
}

bool RunChildScenario(const std::string& scenario, const boost::filesystem::path& output)
{
    if (scenario == "pipeline-clock" || scenario == "pipeline-clock-failure") {
        return RunPipelineClockChild(output, scenario == "pipeline-clock-failure");
    }
    if (scenario.rfind("success-", 0) == 0) {
        return RunSuccessChild(scenario, output);
    }
    if (scenario.rfind("ignore-", 0) == 0) {
        return RunIgnoredEventChild(scenario, output);
    }
    if (scenario == "subscribe-failure") {
        return RunSubscribeFailureChild(output);
    }
    if (scenario == "enable-failure-launch") {
        return RunEnableFailureChild(output, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel);
    }
    if (scenario == "enable-failure-host-args") {
        return RunEnableFailureChild(output, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs);
    }
    if (scenario == "enable-failure-simt-host-args") {
        return RunEnableFailureChild(output, ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs);
    }
    if (scenario == "enable-failure-args-array") {
        return RunEnableFailureChild(output, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray);
    }
    if (scenario == "config-failure") {
        return RunConfigFailureChild(output);
    }
    if (scenario == "section-failure") {
        return RunSectionFailureChild(output);
    }
    if (scenario == "normal-stop") {
        return RunNormalStopChild(output);
    }
    if (scenario == "stop-during-collection") {
        return RunStopDuringCollectionChild(output);
    }
    if (scenario == "csv-soc-device-api") {
        return RunCsvSocNameChild(output);
    }
    std::fprintf(stderr, "unknown child scenario: %s\n", scenario.c_str());
    return false;
}

bool LaunchChild(const char* executable, const char* scenario, const boost::filesystem::path& output)
{
    const pid_t process = ::fork();
    CHECK(process >= 0);
    if (process == 0) {
        ::execl(executable, executable, "--child", scenario, output.c_str(), static_cast<char*>(nullptr));
        std::perror("execl runtime callback child failed");
        ::_exit(127);
    }

    int status = 0;
    CHECK(::waitpid(process, &status, 0) == process);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    return true;
}

std::size_t CountLines(const boost::filesystem::path& path)
{
    std::ifstream input(path.string());
    std::size_t lines = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++lines;
    }
    return lines;
}

bool TestSuccessAndNormalExit(const char* executable)
{
    constexpr std::array<const char*, 5> scenarios = {
        "success-launch", "success-host-args", "success-simt-host-args", "success-args-array", "success-repeated",
    };
    for (const char* scenario : scenarios) {
        TempDirectory temporary;
        CHECK(!temporary.Path().empty());
        CHECK(LaunchChild(executable, scenario, temporary.Path()));
        CHECK(boost::filesystem::is_regular_file(temporary.Path() / kHardwareInfoFile));
        CHECK(CountLines(temporary.Path() / kHardwareInfoFile) == 5);
        CHECK(boost::filesystem::file_size(temporary.Path() / kDeviceCountFile) == 1);
    }
    return true;
}

bool TestIgnoredEvents(const char* executable)
{
    constexpr std::array<const char*, 8> scenarios = {
        "ignore-enter",  "ignore-failed-exit", "ignore-simt-failed-exit", "ignore-set-device",
        "ignore-malloc", "ignore-nontarget",   "ignore-domain",           "ignore-mismatched-metadata",
    };
    for (const char* scenario : scenarios) {
        TempDirectory temporary;
        CHECK(!temporary.Path().empty());
        CHECK(LaunchChild(executable, scenario, temporary.Path()));
        CHECK(!boost::filesystem::exists(temporary.Path() / kHardwareInfoFile));
        CHECK(!boost::filesystem::exists(temporary.Path() / kDeviceCountFile));
    }
    return true;
}

bool TestInitializationFailures(const char* executable)
{
    constexpr std::array<const char*, 7> scenarios = {
        "subscribe-failure",         "enable-failure-launch",
        "enable-failure-host-args",  "enable-failure-simt-host-args",
        "enable-failure-args-array", "config-failure",
        "section-failure",
    };
    for (const char* scenario : scenarios) {
        TempDirectory temporary;
        CHECK(!temporary.Path().empty());
        CHECK(LaunchChild(executable, scenario, temporary.Path()));
        CHECK(!boost::filesystem::exists(temporary.Path() / kHardwareInfoFile));
    }
    return true;
}

bool TestNormalStop(const char* executable)
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    CHECK(LaunchChild(executable, "normal-stop", temporary.Path()));
    TempDirectory pipeline;
    CHECK(LaunchChild(executable, "pipeline-clock", pipeline.Path()));
    TempDirectory failure;
    CHECK(LaunchChild(executable, "pipeline-clock-failure", failure.Path()));
    CHECK(!boost::filesystem::exists(temporary.Path() / kHardwareInfoFile));
    CHECK(!boost::filesystem::exists(temporary.Path() / kDeviceCountFile));
    return true;
}

bool TestStopDuringCollectionDoesNotHoldRuntimeMutex(const char* executable)
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    CHECK(LaunchChild(executable, "stop-during-collection", temporary.Path()));
    return true;
}

bool TestCsvSocNameSelection(const char* executable)
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    CHECK(LaunchChild(executable, "csv-soc-device-api", temporary.Path()));
    return true;
}

} // namespace

extern "C" aclError aclrtGetDeviceCount(uint32_t* count)
{
    if (count == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    *count = 0;
    const auto& output = g_testOutput;
    if (output.empty()) {
        return ACL_ERROR_FAILURE;
    }
    {
        std::unique_lock<std::mutex> lock(g_deviceCountMutex);
        if (g_blockDeviceCount) {
            g_deviceCountStarted = true;
            g_deviceCountCondition.notify_all();
            g_deviceCountCondition.wait(lock, [] { return g_releaseDeviceCount; });
        }
    }
    const boost::filesystem::path countPath = boost::filesystem::path(output) / kDeviceCountFile;
    const int descriptor = ::open(countPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return ACL_ERROR_FAILURE;
    }
    const char marker = '1';
    const bool written = ::write(descriptor, &marker, sizeof(marker)) == static_cast<ssize_t>(sizeof(marker));
    ::close(descriptor);
    return written ? ACL_SUCCESS : ACL_ERROR_FAILURE;
}

extern "C" int halGetDeviceInfo(uint32_t deviceId, int32_t moduleType, int32_t infoType, int64_t* value)
{
    if (deviceId != 3 || moduleType != 0 || infoType != 25 || value == nullptr) {
        return 3;
    }
    *value = 1000000; // kHz: 1 GHz system counter
    return 0;
}

extern "C" const char* aclrtGetSocName() { return "Ascend950PR_9599"; }

aclptiResult aclptiRegisterProfilingDataCallback(aclptiProfilingDataCallback callback)
{
    if (!callback || g_profilingDataCallback) {
        return callback ? ACLPTI_ERROR_INVALID_STATE : ACLPTI_ERROR_INVALID_PARAMETER;
    }
    g_profilingDataCallback = std::move(callback);
    return ACLPTI_SUCCESS;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--child") {
        return RunChildScenario(argv[2], argv[3]) ? 0 : 1;
    }
    if (argc != 1) {
        std::fprintf(stderr, "unexpected runtime callback test arguments\n");
        return 2;
    }
    return TestPipelineSection() && TestSuccessAndNormalExit(argv[0]) && TestIgnoredEvents(argv[0]) &&
                   TestInitializationFailures(argv[0]) && TestNormalStop(argv[0]) &&
                   TestStopDuringCollectionDoesNotHoldRuntimeMutex(argv[0]) && TestCsvSocNameSelection(argv[0]) ?
               0 :
               1;
}
