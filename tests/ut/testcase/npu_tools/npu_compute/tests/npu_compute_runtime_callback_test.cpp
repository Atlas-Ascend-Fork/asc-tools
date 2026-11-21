/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "test_process.h"
#include "occupancy_test_data.h"
#include <gtest/gtest.h>

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
#include <boost/property_tree/json_parser.hpp>
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
extern "C" void NpuComputeTestMetadata(
    void*, aclptiCallbackDomain, aclptiCallbackId, const aclptiCallbackData*) noexcept;
namespace npucompute::test {
NpuComputeRuntime& Runtime();
}

namespace {

constexpr char SECTIONS[] = "PipeUtilization,Memory";
constexpr char HARDWARE_INFO_FILE[] = "HardwareInfo.jsonl";
constexpr char DEVICE_COUNT_FILE[] = "device_count.calls";
constexpr std::array<aclptiCallbackId, 9> HARDWARE_INFO_TRIGGER_CBIDS = {
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
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;

std::mutex g_deviceCountMutex;
std::condition_variable g_deviceCountCondition;
bool g_blockDeviceCount = false;
bool g_deviceCountStarted = false;
bool g_releaseDeviceCount = false;
bool g_summaryHardwareAvailable = false;
int32_t g_currentDevice = 0;

#define CHECK(expression)                 \
    do {                                  \
        if (!(expression)) {              \
            ADD_FAILURE() << #expression; \
            return false;                 \
        }                                 \
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
    NpuComputeTestConfigure(output.c_str(), SECTIONS);
    return true;
}

TEST(NpuComputeRuntimeCallback, PipelineSection)
{
    npucompute::SectionConfig config;
    std::string error;
    ASSERT_TRUE(config.Load({"Pipeline"}, &error));
    ASSERT_TRUE(config.PipelineEnabled());
    ASSERT_TRUE(config.Params()->numSections == 0);
    ASSERT_TRUE(config.Load({"Memory", "Pipeline"}, &error));
    ASSERT_TRUE(config.PipelineEnabled());
    ASSERT_TRUE(config.Sections() == std::vector<std::string>{"Memory"});
    ASSERT_TRUE(config.JoinedSections() == "Memory,Pipeline");
    ASSERT_TRUE(config.Load({"Memory"}, &error));
    ASSERT_TRUE(!config.PipelineEnabled());
    ASSERT_TRUE(!config.Load({"pipeline"}, &error));
}

bool CheckSubscribeAndEnableContract()
{
    using npucompute::test::AclPtiEnableCall;

    CHECK(npucompute::test::AclPtiSubscribeCount() == 1);
    CHECK(npucompute::test::CapturedAclPtiCallback() == &NpuComputeTestMetadata);
    CHECK(npucompute::test::CapturedAclPtiUserData() == nullptr);
    const aclptiSubscribeHandle subscriber = npucompute::test::CapturedAclPtiSubscriber();
    CHECK(subscriber != nullptr);

    const std::vector<AclPtiEnableCall> enableCalls = npucompute::test::CapturedAclPtiEnableCalls();
    CHECK(enableCalls.size() == HARDWARE_INFO_TRIGGER_CBIDS.size());
    std::size_t previousSequence = npucompute::test::AclPtiSubscribeSequence();
    CHECK(previousSequence > 0);
    for (std::size_t index = 0; index < enableCalls.size(); ++index) {
        const AclPtiEnableCall& call = enableCalls[index];
        CHECK(call.sequence > previousSequence);
        CHECK(call.enable);
        CHECK(call.subscriber == subscriber);
        CHECK(call.domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
        CHECK(call.cbid == HARDWARE_INFO_TRIGGER_CBIDS[index]);
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
    CHECK(calls.size() == HARDWARE_INFO_TRIGGER_CBIDS.size() * 2);
    for (std::size_t index = 0; index < HARDWARE_INFO_TRIGGER_CBIDS.size(); ++index) {
        const AclPtiEnableCall& call = calls[HARDWARE_INFO_TRIGGER_CBIDS.size() + index];
        CHECK(!call.enable);
        CHECK(call.domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
        CHECK(call.cbid == HARDWARE_INFO_TRIGGER_CBIDS[HARDWARE_INFO_TRIGGER_CBIDS.size() - index - 1]);
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

bool RunWarpCsvChild(const boost::filesystem::path& output, const std::string& scenario)
{
    CHECK(SetScenarioEnvironment(output));
    NpuComputeTestConfigure(output.c_str(), "occupancy");
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(npucompute::test::InvokeAclPtiRuntimeReady());
    auto result = std::make_shared<aclptiProfilingDataResult>();
    auto warp = std::make_shared<aclptiSimtWarpData>();
    aclptiSimtWarpRecord record;
    record.functionName = "kernel,\"test\"";
    record.aicoreId = 2;
    record.subcoreId = 1;
    record.warpId = 7;
    record.startTime = 10;
    record.endTime = 14;
    record.duration = 4;
    warp->records.push_back(record);
    record.aicoreId = 3;
    record.subcoreId = 0;
    record.warpId = 1;
    record.startTime = 100;
    record.endTime = 105;
    record.duration = 5;
    warp->records.push_back(record);
    record.subcoreId = 2;
    warp->records.push_back(record);
    if (scenario != "warp-csv") {
        constexpr uint64_t kFirstAnchor = UINT64_C(9007199254740993);
        const bool noFrequency = scenario == "warp-csv-no-frequency" || scenario == "warp-csv-activity-no-frequency";
        warp->deviceId = noFrequency ? -1 : 3;
        if (scenario == "warp-csv-rated-frequency") {
            warp->deviceId = 0;
            g_summaryHardwareAvailable = true;
        }
        warp->vfAnchors = {
            {2, 1, 0, kFirstAnchor, kFirstAnchor + 1500},
            {3, 0, 0, kFirstAnchor + 3000, kFirstAnchor + 4800},
            {3, 2, 0, kFirstAnchor + 5000, kFirstAnchor + 6000}};
        record = warp->records.front();
        if (scenario != "warp-csv-repeated-warp" && scenario != "warp-csv-multiple-vfs" &&
            scenario != "warp-csv-activity-local") {
            record.warpId = 8;
        }
        record.vfInstrAddress = scenario == "warp-csv-multiple-vfs" ? 42 : 0;
        record.startTime = 14;
        record.endTime = 22;
        record.duration = 8;
        warp->records.push_back(record);
        if (scenario == "warp-csv-hybrid-vf" || scenario == "warp-csv-hybrid-unknown-vf") {
            warp->vfAnchors.front().vfInstrAddress = 3172;
            warp->vfAnchors.front().endSyscnt = kFirstAnchor + 1;
            warp->vfAnchors.front().kind = aclptiSimtVfKind::Simd;
            const auto kind = scenario == "warp-csv-hybrid-vf" ? aclptiSimtVfKind::Simt : aclptiSimtVfKind::Unknown;
            warp->vfAnchors.push_back({2, 1, 3392, kFirstAnchor + 6000, kFirstAnchor + 8300, kind});
            warp->records.front().vfInstrAddress = 4280;
            warp->records.back().vfInstrAddress = 4280;
        } else if (scenario == "warp-csv-activity-local") {
            warp->vfAnchors.push_back({2, 1, 0, kFirstAnchor + 6000, kFirstAnchor + 8300});
        } else if (scenario == "warp-csv-extra-anchor") {
            warp->vfAnchors.push_back({2, 1, 0, kFirstAnchor + 7000});
        } else if (scenario == "warp-csv-multiple-vfs") {
            warp->vfAnchors.push_back({2, 1, record.vfInstrAddress, kFirstAnchor + 7000});
        } else if (scenario == "warp-csv-address-mismatch") {
            warp->vfAnchors.back().vfInstrAddress = UINT64_C(1) << 32;
        } else if (scenario == "warp-csv-missing-anchor") {
            warp->vfAnchors.pop_back();
        } else if (scenario == "warp-csv-wrap") {
            warp->records.front().startTime = UINT32_MAX - 1;
            warp->records.front().endTime = 2;
            warp->records.back().startTime = 2;
            warp->records.back().endTime = 10;
        }
    }
    result->simtWarpData = warp;
    CHECK(npucompute::test::Runtime().Submit(result) == ACLPTI_SUCCESS);
    npucompute::test::Runtime().Stop();
    CHECK(npucompute::test::Runtime().ShutdownAfterPtiDrain() == 0);
    CHECK(boost::filesystem::is_regular_file(output / "SimtWarpOccupancy.json"));
    CHECK(boost::filesystem::is_regular_file(output / "summary.jsonl"));
    return true;
}

bool RunCsvSocNameChild(const boost::filesystem::path& output, bool missingWarp)
{
    CHECK(SetScenarioEnvironment(output));
    if (missingWarp) {
        NpuComputeTestConfigure(output.c_str(), "Memory,occupancy");
    }
    CHECK(::setenv("NPU_COMPUTE_FREQUENCY_MHZ", "1000", 1) == 0);
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(npucompute::test::InvokeAclPtiRuntimeReady());

    auto result = std::make_shared<aclptiProfilingDataResult>();
    npucompute::test::AddOccupancyReplay(*result, 3);
    aclptiPmuDataRow row{};
    row.blockId = 0;
    row.subBlockId = 0;
    row.coreType = ACLPTI_CORE_TYPE_AIV;
    row.coreId = 0;
    row.totalCycles = 1000.0;
    row.values = {{0x422U, 100.0}, {0x57fU, 10.0}, {0x580U, 10.0}};
    result->pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, std::move(row));

    CHECK(npucompute::test::Runtime().Submit(result) == ACLPTI_SUCCESS);
    npucompute::test::Runtime().Stop();
    CHECK(npucompute::test::Runtime().ShutdownAfterPtiDrain() == 0);
    CHECK(npucompute::test::Runtime().RowCount() == result->pmuLogs.size());
    CHECK(CsvValue(output / "Memory.csv", "vector0", "GM_to_UB_bw_usage_rate(%)") == "5.358925");
    CHECK(boost::filesystem::is_regular_file(output / "summary.jsonl"));
    if (missingWarp) {
        CHECK(!boost::filesystem::exists(output / "SimtWarpOccupancy.json"));
    }
    return true;
}

bool RunSummaryHardwareChild(const std::string& scenario, const boost::filesystem::path& output)
{
    CHECK(SetScenarioEnvironment(output));
    g_summaryHardwareAvailable = scenario != "summary-hardware-missing";
    if (scenario == "summary-hardware-override") {
        CHECK(::setenv("NPU_COMPUTE_FREQUENCY_MHZ", "1200", 1) == 0);
    } else {
        CHECK(::unsetenv("NPU_COMPUTE_FREQUENCY_MHZ") == 0);
    }
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(npucompute::test::InvokeAclPtiRuntimeReady());
    auto result = std::make_shared<aclptiProfilingDataResult>();
    npucompute::test::AddOccupancyReplay(*result, 3);
    result->taskPmuData.aiv = aclptiTaskPmuCoreData{1000, false, {}};
    CHECK(npucompute::test::Runtime().Submit(result) == ACLPTI_SUCCESS);
    npucompute::test::Runtime().Stop();
    CHECK(npucompute::test::Runtime().ShutdownAfterPtiDrain() == 0);
    CHECK(npucompute::test::Runtime().RowCount() == 0);
    CHECK(!boost::filesystem::exists(output / "Memory.csv"));
    std::ifstream input((output / "summary.jsonl").string());
    std::string line;
    boost::property_tree::ptree summary;
    while (std::getline(input, line)) {
        std::istringstream record(line);
        boost::property_tree::read_json(record, summary);
    }
    CHECK(summary.get<std::string>("category") == "OpInfoSummary");
    CHECK(summary.get<std::string>("Op Type") == "vector");
    if (!g_summaryHardwareAvailable) {
        CHECK(summary.get<std::string>("Current Freq") == "NA");
        CHECK(summary.get<std::string>("Rated Freq") == "NA");
    } else {
        CHECK(summary.get<double>("Current Freq") == (scenario == "summary-hardware-override" ? 1200 : 1500));
        CHECK(summary.get<double>("Rated Freq") == 1500);
    }
    return true;
}

bool RunPipelineClockChild(const boost::filesystem::path& output, bool failQuery)
{
    CHECK(SetScenarioEnvironment(output));
    NpuComputeTestConfigure(output.c_str(), "Pipeline");
    CHECK(::unsetenv("NPU_COMPUTE_SYSCNT_FREQUENCY_MHZ") == 0);
    CHECK(::setenv("NPU_COMPUTE_FREQUENCY_MHZ", "1000", 1) == 0);
    g_currentDevice = failQuery ? 4 : 3;
    npucompute::test::ResetAclPtiCallbackStub();
    CHECK(acltoolInitialize() == ACLPTI_SUCCESS);
    CHECK(npucompute::test::InvokeAclPtiRuntimeReady());
    CHECK(npucompute::test::CapturedAclPtiCollectPipeline());
    auto result = std::make_shared<aclptiProfilingDataResult>();
    npucompute::test::AddOccupancyReplay(*result, 3);
    auto& replay = result->pipelineData[7];
    replay.deviceId = failQuery ? 4 : 3;
    replay.format = aclptiBiuFormat::Chip6;
    replay.channels[{0, aclptiBiuCoreKind::Aic}] = {0xe00003e8U, 0xe0000000U, 0xe0000000U, 0xe0000000U,
                                                    0xf0000000U, 0xf001000aU, 0xf0000014U, 0xf0010004U};
    CHECK(npucompute::test::Runtime().Submit(result) == ACLPTI_SUCCESS);
    npucompute::test::Runtime().Stop();
    CHECK(npucompute::test::Runtime().ShutdownAfterPtiDrain() == (failQuery ? npucompute::kInitializeFailed : 0));
    CHECK(boost::filesystem::is_regular_file(output / "summary.jsonl"));
    const auto published = output / ".biu-staging/process-uds";
    CHECK(boost::filesystem::is_regular_file(published / "manifest.json"));
    CHECK(boost::filesystem::is_regular_file(published / "result-0-device-3-replay-7.json") == !failQuery);
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
        CHECK(boost::filesystem::is_regular_file(output / HARDWARE_INFO_FILE));
        CHECK(CountLines(output / HARDWARE_INFO_FILE) == 5);
        return true;
    }
    if (scenario == "success-host-args") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(boost::filesystem::is_regular_file(output / HARDWARE_INFO_FILE));
        CHECK(CountLines(output / HARDWARE_INFO_FILE) == 5);
        return true;
    }
    if (scenario == "success-simt-host-args") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(boost::filesystem::is_regular_file(output / HARDWARE_INFO_FILE));
        CHECK(CountLines(output / HARDWARE_INFO_FILE) == 5);
        return true;
    }
    if (scenario == "success-args-array") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray, ACLPTI_API_EXIT,
            ACL_SUCCESS, nullptr));
        CHECK(boost::filesystem::is_regular_file(output / HARDWARE_INFO_FILE));
        CHECK(CountLines(output / HARDWARE_INFO_FILE) == 5);
        return true;
    }
    if (scenario == "success-repeated") {
        CHECK(npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, ACLPTI_RUNTIME_CBID_aclrtLaunchKernel, ACLPTI_API_EXIT, ACL_SUCCESS,
            nullptr));
        CHECK(boost::filesystem::is_regular_file(output / HARDWARE_INFO_FILE));
        CHECK(CountLines(output / HARDWARE_INFO_FILE) == 5);
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
    CHECK(acltoolShutdown() == 0);
    CHECK(CheckDisableContract());
    for (aclptiCallbackId cbid : HARDWARE_INFO_TRIGGER_CBIDS) {
        CHECK(!npucompute::test::InvokeAclPtiCallback(
            ACLPTI_CB_DOMAIN_RUNTIME_API, cbid, ACLPTI_API_EXIT, ACL_SUCCESS, nullptr));
    }
    CHECK(!boost::filesystem::exists(output / HARDWARE_INFO_FILE));
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
        npucompute::test::Runtime().Stop();
        stopReturned = true;
    });

    std::atomic<bool> drainReturned{false};
    std::atomic<int> drainStatus{npucompute::kInitializeFailed};
    std::thread drainThread([&drainReturned, &drainStatus] {
        drainStatus = npucompute::test::Runtime().ShutdownAfterPtiDrain();
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
    CHECK(npucompute::test::CapturedAclPtiEnableCalls().size() == HARDWARE_INFO_TRIGGER_CBIDS.size());
    CHECK(callbackDispatched.load());
    CHECK(!callbackReturnedBeforeRelease);
    CHECK(!stopReturnedBeforeRelease);
    CHECK(drainReturnedBeforeRelease);
    CHECK(drainStatus.load() == 0);
    CHECK(boost::filesystem::is_regular_file(output / HARDWARE_INFO_FILE));
    CHECK(CountLines(output / HARDWARE_INFO_FILE) == 5);
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
        std::find(HARDWARE_INFO_TRIGGER_CBIDS.begin(), HARDWARE_INFO_TRIGGER_CBIDS.end(), failedCbid);
    CHECK(failurePosition != HARDWARE_INFO_TRIGGER_CBIDS.end());
    const std::size_t successfulEnableCount =
        static_cast<std::size_t>(std::distance(HARDWARE_INFO_TRIGGER_CBIDS.begin(), failurePosition));
    const std::vector<npucompute::test::AclPtiEnableCall> calls = npucompute::test::CapturedAclPtiEnableCalls();
    CHECK(calls.size() == successfulEnableCount * 2 + 1);

    for (std::size_t index = 0; index <= successfulEnableCount; ++index) {
        CHECK(calls[index].enable);
        CHECK(calls[index].domain == ACLPTI_CB_DOMAIN_RUNTIME_API);
        CHECK(calls[index].cbid == HARDWARE_INFO_TRIGGER_CBIDS[index]);
        CHECK(calls[index].result == (index == successfulEnableCount ? ACLPTI_ERROR_NOT_SUPPORTED : ACLPTI_SUCCESS));
    }
    for (std::size_t index = 0; index < successfulEnableCount; ++index) {
        const aclptiCallbackId enabledCbid = HARDWARE_INFO_TRIGGER_CBIDS[index];
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
    for (aclptiCallbackId cbid : HARDWARE_INFO_TRIGGER_CBIDS) {
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
    if (scenario.rfind("warp-csv", 0) == 0) {
        return RunWarpCsvChild(output, scenario);
    }
    if (scenario == "csv-soc-device-api" || scenario == "csv-missing-warp") {
        return RunCsvSocNameChild(output, scenario == "csv-missing-warp");
    }
    if (scenario.compare(0, 17, "summary-hardware-") == 0) {
        return RunSummaryHardwareChild(scenario, output);
    }
    std::fprintf(stderr, "unknown child scenario: %s\n", scenario.c_str());
    return false;
}

bool LaunchChild(const char* executable, const char* scenario, const boost::filesystem::path& output)
{
    npucompute::test::ProcessOptions options;
    options.arguments = {executable, "--child", scenario, output.string()};
    const auto result = npucompute::test::RunProcess(options);
    if (result.launchError != 0 || result.timedOut || result.signal != 0 || result.exitCode != 0) {
        ADD_FAILURE() << "runtime callback child " << scenario << ": launchError=" << result.launchError
                      << ", timeout=" << result.timedOut << ", signal=" << result.signal
                      << ", exitCode=" << result.exitCode << "\n"
                      << result.standardOutput << result.standardError;
        return false;
    }
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

class NpuComputeRuntimeSuccessAndNormalExit : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeRuntimeSuccessAndNormalExit, VerifiesCallbackAndArtifacts)
{
    const char* executable = "/proc/self/exe";
    const auto& scenario = GetParam();

    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild(executable, scenario.c_str(), temporary.Path()));
    ASSERT_TRUE(boost::filesystem::is_regular_file(temporary.Path() / HARDWARE_INFO_FILE));
    ASSERT_TRUE(CountLines(temporary.Path() / HARDWARE_INFO_FILE) == 5);
    ASSERT_TRUE(boost::filesystem::file_size(temporary.Path() / DEVICE_COUNT_FILE) == 1);
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios, NpuComputeRuntimeSuccessAndNormalExit,
    testing::Values(
        "success-launch", "success-host-args", "success-simt-host-args", "success-args-array", "success-repeated"),
    [](const testing::TestParamInfo<std::string>& info) {
        std::string name = info.param;
        for (char& character : name) {
            if (character == '-')
                character = '_';
        }
        return name;
    });

class NpuComputeRuntimeIgnoredEvents : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeRuntimeIgnoredEvents, VerifiesCallbackAndArtifacts)
{
    const char* executable = "/proc/self/exe";
    const auto& scenario = GetParam();

    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild(executable, scenario.c_str(), temporary.Path()));
    ASSERT_TRUE(!boost::filesystem::exists(temporary.Path() / HARDWARE_INFO_FILE));
    ASSERT_TRUE(!boost::filesystem::exists(temporary.Path() / DEVICE_COUNT_FILE));
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios, NpuComputeRuntimeIgnoredEvents,
    testing::Values(
        "ignore-enter", "ignore-failed-exit", "ignore-simt-failed-exit", "ignore-set-device", "ignore-malloc",
        "ignore-nontarget", "ignore-domain", "ignore-mismatched-metadata"),
    [](const testing::TestParamInfo<std::string>& info) {
        std::string name = info.param;
        for (char& character : name) {
            if (character == '-')
                character = '_';
        }
        return name;
    });

class NpuComputeRuntimeInitializationFailures : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeRuntimeInitializationFailures, VerifiesCallbackAndArtifacts)
{
    const char* executable = "/proc/self/exe";
    const auto& scenario = GetParam();

    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild(executable, scenario.c_str(), temporary.Path()));
    ASSERT_TRUE(!boost::filesystem::exists(temporary.Path() / HARDWARE_INFO_FILE));
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios, NpuComputeRuntimeInitializationFailures,
    testing::Values(
        "subscribe-failure", "enable-failure-launch", "enable-failure-host-args", "enable-failure-simt-host-args",
        "enable-failure-args-array", "config-failure", "section-failure"),
    [](const testing::TestParamInfo<std::string>& info) {
        std::string name = info.param;
        for (char& character : name) {
            if (character == '-')
                character = '_';
        }
        return name;
    });

TEST(NpuComputeRuntimeCallback, NormalStop)
{
    const char* executable = "/proc/self/exe";
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild(executable, "normal-stop", temporary.Path()));
    TempDirectory pipeline;
    ASSERT_TRUE(LaunchChild(executable, "pipeline-clock", pipeline.Path()));
    TempDirectory failure;
    ASSERT_TRUE(LaunchChild(executable, "pipeline-clock-failure", failure.Path()));
    ASSERT_TRUE(!boost::filesystem::exists(temporary.Path() / HARDWARE_INFO_FILE));
    ASSERT_TRUE(!boost::filesystem::exists(temporary.Path() / DEVICE_COUNT_FILE));
}

TEST(NpuComputeRuntimeCallback, StopDuringCollectionDoesNotHoldRuntimeMutex)
{
    const char* executable = "/proc/self/exe";
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild(executable, "stop-during-collection", temporary.Path()));
}

TEST(NpuComputeRuntimeCallback, MissingWarpDoesNotFailPmuReport)
{
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild("/proc/self/exe", "csv-missing-warp", temporary.Path()));
}

TEST(NpuComputeRuntimeCallback, CsvSocNameSelection)
{
    const char* executable = "/proc/self/exe";
    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild(executable, "csv-soc-device-api", temporary.Path()));
}

class NpuComputeRuntimeSummaryHardwareInputs : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeRuntimeSummaryHardwareInputs, VerifiesCallbackAndArtifacts)
{
    const char* executable = "/proc/self/exe";
    const auto& scenario = GetParam();

    TempDirectory temporary;
    ASSERT_TRUE(!temporary.Path().empty());
    ASSERT_TRUE(LaunchChild(executable, scenario.c_str(), temporary.Path()));
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios, NpuComputeRuntimeSummaryHardwareInputs,
    testing::Values("summary-hardware-current", "summary-hardware-missing", "summary-hardware-override"),
    [](const testing::TestParamInfo<std::string>& info) {
        std::string name = info.param;
        for (char& character : name) {
            if (character == '-')
                character = '_';
        }
        return name;
    });

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
    const boost::filesystem::path countPath = boost::filesystem::path(output) / DEVICE_COUNT_FILE;
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
    if (g_summaryHardwareAvailable && deviceId == 0 && value != nullptr && (moduleType == 4 || moduleType == 7)) {
        if (infoType == 32) {
            *value = moduleType == 4 ? 1500 : 0; // AIV must fall back to its own rated value.
            return 0;
        }
        if (infoType == 4) {
            *value = moduleType == 4 ? 1700 : 1500;
            return 0;
        }
    }
    if (deviceId == 3 && infoType == 32 && value != nullptr && (moduleType == 4 || moduleType == 7)) {
        *value = moduleType == 4 ? 1000 : 2000;
        return 0;
    }
    const bool knownDevice = deviceId == 3 || (deviceId == 0 && g_summaryHardwareAvailable);
    if (!knownDevice || moduleType != 0 || infoType != 25 || value == nullptr) {
        return 3;
    }
    *value = 1000000; // kHz: 1 GHz system counter
    return 0;
}

extern "C" aclError aclrtGetDevice(int32_t* device)
{
    *device = g_currentDevice;
    return ACL_SUCCESS;
}

extern "C" const char* aclrtGetSocName() { return "Ascend950PR_9599"; }

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--child") {
        return RunChildScenario(argv[2], argv[3]) ? 0 : 1;
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
