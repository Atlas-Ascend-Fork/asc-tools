/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/profiling/application_profiler.h"
#include "acl_pti/data/data_types.h"
#include "profiling/prof_api.h"
#include <acl/acl_rt.h>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace aclpti::profiling;
// Inject newer wire IDs without requiring their names in the installed Runtime headers.
constexpr RawDataType kAdditionalRawDataType = static_cast<RawDataType>(17);
constexpr RawDataType kAicpuRawDataType = static_cast<RawDataType>(12);
constexpr RawDataType kCcuInstrRawDataType = static_cast<RawDataType>(13);
constexpr RawDataType kCcuStatRawDataType = static_cast<RawDataType>(14);
constexpr RawDataType kUnknownRawDataType = static_cast<RawDataType>(18);
MsprofRawDataCallback callback = nullptr;
std::mutex callsMutex;
std::vector<MsprofConfig> starts;
std::vector<MsprofConfig> stops;
int32_t startStatus = 0;
int32_t stopStatus = 0;
bool callbackDuringStart = false;
int32_t uploaderStatus = 0;

aclError DeviceCall(int32_t) { return ACL_SUCCESS; }

aclptiResult ConfigureCommunication(ApplicationProfiler& profiler)
{
    const char* sections[] = {"Communication"};
    aclptiRangeProfilerSetConfigParams params{sections, 1, ACLPTI_BLOCK_RESULT_ALL};
    return profiler.SetConfig(&params);
}

class ApplicationProfilerTest : public testing::Test {
protected:
    void SetUp() override
    {
        callback = nullptr;
        starts.clear();
        stops.clear();
        startStatus = 0;
        stopStatus = 0;
        callbackDuringStart = false;
        uploaderStatus = 0;
    }
};

TEST_F(ApplicationProfilerTest, CommunicationUsesOneSummaryPmuRoundOnEachDevice)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    ASSERT_EQ(
        profiler.Initialize([](const auto&) {}, [&summary](const auto& result) { summary = result; }), ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) {
        workers.emplace_back([&profiler] { EXPECT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS); });
    }
    for (auto& thread : workers) {
        thread.join();
    }
    ASSERT_EQ(starts.size(), 1);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 1), ACL_SUCCESS);
    ASSERT_EQ(starts.size(), 2U);
    for (const auto& config : starts) {
        EXPECT_EQ(
            config.profSwitch, static_cast<uint64_t>(
                                   PROF_OP_TIMESTAMP_MASK | PROF_TASK_TIME_MASK | PROF_TASK_TIME_L1_MASK |
                                   PROF_AICPU_TRACE_MASK | PROF_AICORE_METRICS_MASK));
        ASSERT_EQ(config.configInfo.numAttrs, 2U);
        ASSERT_NE(config.configInfo.attrs, nullptr);
        const auto& metrics = config.configInfo.attrs[0];
        EXPECT_EQ(metrics.id, PROF_CONFIG_ATTR_AICORE_METRICS);
        const uint32_t expected[] = {808, 1281, 0x422, 0x423};
        for (size_t i = 0; i < COMPUTE_AICORE_METRICS_NUM; ++i) {
            EXPECT_EQ(metrics.value.aicoreMetrics[i], i < 4 ? expected[i] : MSPROF_INVALID_AICORE_METRIC);
        }
        EXPECT_EQ(config.configInfo.attrs[1].id, PROF_CONFIG_ATTR_TASK_BLOCK);
        EXPECT_EQ(config.configInfo.attrs[1].value.taskBlockMode, PROF_COMPUTE_ALL_BLOCK);
        EXPECT_EQ(config.devNums, 1U);
    }
    EXPECT_EQ(starts[0].devIdList[0], 0U);
    EXPECT_EQ(starts[1].devIdList[0], 1U);
    for (unsigned i = 0; i < 7; ++i) {
        ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
        EXPECT_TRUE(stops.empty());
    }
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    EXPECT_EQ(stops.size(), 1);
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 1, false), ACL_SUCCESS);
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    ASSERT_EQ(stops.size(), 2);
    for (const auto& config : stops) {
        EXPECT_EQ(config.profSwitch, 0x10000080eULL);
    }
    EXPECT_EQ(stops[1].devIdList[0], 1U);
    EXPECT_EQ(summary.devices.at(0).state, ApplicationDeviceState::Inactive);
    EXPECT_FALSE(summary.tailConfirmed);
    EXPECT_TRUE(summary.complete);
}

TEST_F(ApplicationProfilerTest, VisibleDeviceMappingMatchesStartStopAndRawRecords)
{
    struct ScopedVisibility {
        ~ScopedVisibility() { unsetenv("ASCEND_RT_VISIBLE_DEVICES"); }
    } visibility;
    ASSERT_EQ(setenv("ASCEND_RT_VISIBLE_DEVICES", "2,3", 1), 0);
    ApplicationProfiler profiler;
    std::vector<ApplicationRawRecord> records;
    ASSERT_EQ(
        profiler.Initialize([&records](const auto& record) { records.push_back(record); }, [](const auto&) {}),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    const auto deviceCall = [](int32_t id) {
        EXPECT_EQ(id, 1);
        return ACL_SUCCESS;
    };
    ASSERT_EQ(profiler.SetDevice(deviceCall, 1), ACL_SUCCESS);
    ASSERT_EQ(starts.size(), 1U);
    EXPECT_EQ(starts[0].devIdList[0], 3U);

    MsprofRawData raw{};
    raw.type = kAdditionalRawDataType;
    raw.deviceId = 3;
    raw.chunkSize = 1;
    ASSERT_EQ(callback(&raw), 0);
    EXPECT_EQ(profiler.DeviceEpoch(3), 1U);
    ASSERT_EQ(profiler.ResetDevice(deviceCall, 1, false), ACL_SUCCESS);
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    ASSERT_EQ(stops.size(), 1U);
    EXPECT_EQ(stops[0].devIdList[0], 3U);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].deviceId, 3);
    EXPECT_EQ(records[0].deviceEpoch, 1U);
}

TEST_F(ApplicationProfilerTest, ConfigurationIsRequiredAndFrozenByFirstStartAttempt)
{
    ApplicationProfiler profiler;
    EXPECT_EQ(ConfigureCommunication(profiler), ACLPTI_ERROR_INVALID_STATE);
    ASSERT_EQ(profiler.Initialize([](const auto&) {}, [](const auto&) {}), ACLPTI_SUCCESS);
    EXPECT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    EXPECT_TRUE(starts.empty());
    EXPECT_EQ(profiler.DeviceEpoch(0), 0U);
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    EXPECT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    EXPECT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    EXPECT_TRUE(starts.empty());
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    EXPECT_EQ(ConfigureCommunication(profiler), ACLPTI_ERROR_INVALID_STATE);
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    EXPECT_EQ(ConfigureCommunication(profiler), ACLPTI_ERROR_INVALID_STATE);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 1), ACL_SUCCESS);
    EXPECT_EQ(starts.size(), 3U);
    profiler.NoteProfilingFinalized();
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    EXPECT_EQ(ConfigureCommunication(profiler), ACLPTI_ERROR_INVALID_STATE);
}

TEST_F(ApplicationProfilerTest, RejectsMalformedAndUnsupportedConfiguration)
{
    ApplicationProfiler profiler;
    ASSERT_EQ(profiler.Initialize([](const auto&) {}, [](const auto&) {}), ACLPTI_SUCCESS);
    const char* sections[] = {"Communication", "Memory"};
    const aclptiRangeProfilerSetConfigParams valid{sections, 1, ACLPTI_BLOCK_RESULT_ALL};
    auto params = valid;
    EXPECT_EQ(profiler.SetConfig(nullptr), ACLPTI_ERROR_INVALID_PARAMETER);
    params.numSections = 0;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    params.numSections = ACLPTI_MAX_NUM_SECTIONS + 1;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    params = valid;
    params.sections = nullptr;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    for (const char* name : {static_cast<const char*>(nullptr), ""}) {
        params = valid;
        params.sections = &name;
        EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    }
    const std::string longName(ACLPTI_MAX_SECTION_NAME_LENGTH + 1, 'x');
    const char* name = longName.c_str();
    params.sections = &name;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    params = valid;
    params.blockResult = static_cast<aclptiBlockResultMode>(99);
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    params = valid;
    params.cacheControl = static_cast<aclptiCacheControl>(99);
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    for (const char* unsupported : {"Memory", "Unknown"}) {
        params = valid;
        params.sections = &unsupported;
        EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    }
    params = valid;
    params.numSections = 2;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    for (const auto mode : {ACLPTI_BLOCK_RESULT_DISABLED, ACLPTI_BLOCK_RESULT_SHRINK}) {
        params = valid;
        params.blockResult = mode;
        EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    }
    params = valid;
    params.cacheControl = ACLPTI_CACHE_CONTROL_ALL;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    params = valid;
    params.collectPipeline = true;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    params = valid;
    params.collectPcSampling = true;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    params = {};
    params.blockResult = ACLPTI_BLOCK_RESULT_ALL;
    params.collectPipeline = true;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    params.collectPipeline = false;
    params.collectPcSampling = true;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    EXPECT_TRUE(starts.empty());
    EXPECT_EQ(profiler.SetConfig(&valid), ACLPTI_SUCCESS);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
}

TEST_F(ApplicationProfilerTest, ConfigurationValidationPrecedesCapabilityAndSessionChecks)
{
    ApplicationProfiler profiler;
    const char* sections[] = {"Memory", nullptr};
    aclptiRangeProfilerSetConfigParams params{sections, 2, ACLPTI_BLOCK_RESULT_ALL};
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    const std::string tooLong(ACLPTI_MAX_SECTION_NAME_LENGTH + 1, 'x');
    sections[1] = tooLong.c_str();
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
    sections[1] = "Communication";
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_NOT_SUPPORTED);
    params.sections = sections + 1;
    params.numSections = 1;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_STATE);
    ASSERT_EQ(profiler.Initialize([](const auto&) {}, [](const auto&) {}), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetConfig(&params), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    params.sections = sections;
    params.numSections = 2;
    sections[1] = nullptr;
    EXPECT_EQ(profiler.SetConfig(&params), ACLPTI_ERROR_INVALID_PARAMETER);
}

TEST_F(ApplicationProfilerTest, DecodesFixedRecordsOnWorkerAndRetainsUnknownRawData)
{
    ApplicationProfiler profiler;
    std::vector<ApplicationRawRecord> records;
    ApplicationSummary summary;
    std::thread::id consumerThread;
    std::thread::id drainedThread;
    ASSERT_EQ(
        profiler.Initialize(
            [&records, &consumerThread](const auto& record) {
                consumerThread = std::this_thread::get_id();
                records.push_back(record);
            },
            [&summary, &drainedThread](const auto& value) {
                summary = value;
                drainedThread = std::this_thread::get_id();
            }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.type = LOG_DATA_TYPE;
    raw.deviceId = 0;
    raw.chunkSize = 32;
    // Little-endian task start, task=17, stream=3, syscnt=100.
    const uint8_t bytes[] = {0, 0, 0xd3, 0x6b, 3, 0, 17, 0, 100};
    std::copy(std::begin(bytes), std::end(bytes), raw.chunk);
    ASSERT_EQ(callback(&raw), 0);
    raw.chunkSize = 1;
    const RawDataType opaqueTypes[] = {
        kAdditionalRawDataType, kAicpuRawDataType, kCcuInstrRawDataType, kCcuStatRawDataType, kUnknownRawDataType};
    for (const auto type : opaqueTypes) {
        SCOPED_TRACE(static_cast<int32_t>(type));
        raw.type = type;
        ASSERT_EQ(callback(&raw), 0);
    }
    profiler.NoteProfilingFinalized();
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    ASSERT_EQ(records.size(), 6U);
    ASSERT_EQ(records[0].decoded.size(), 1U);
    const auto& log = std::get<aclpti::data::TaskLog32>(records[0].decoded[0].payload);
    EXPECT_EQ(log.taskId, 17);
    EXPECT_EQ(log.rtStreamId, 3);
    EXPECT_EQ(log.systemCounter, 100U);
    EXPECT_EQ(records[0].bytes.size(), 32U);
    for (size_t i = 1; i < records.size(); ++i) {
        SCOPED_TRACE(static_cast<int32_t>(opaqueTypes[i - 1]));
        EXPECT_EQ(records[i].type, static_cast<int32_t>(opaqueTypes[i - 1]));
        EXPECT_TRUE(records[i].decoded.empty());
        EXPECT_EQ(records[i].decodeErrors, 0U);
        EXPECT_EQ(records[i].bytes, std::vector<uint8_t>{0});
    }
    EXPECT_TRUE(summary.complete);
    EXPECT_EQ(consumerThread, drainedThread);
    EXPECT_NE(consumerThread, std::this_thread::get_id());
}

TEST_F(ApplicationProfilerTest, RetainsUnsupportedSchedulerLogsWithoutFailingKnownTaskDecoding)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    std::vector<ApplicationRawRecord> records;
    ASSERT_EQ(
        profiler.Initialize(
            [&](const auto& record) { records.push_back(record); }, [&](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.type = LOG_DATA_TYPE;
    constexpr uint8_t functions[] = {2, 3, 0x3f, 0};
    raw.chunkSize = sizeof(functions) * 32;
    // Preserve opaque records and continue decoding known records that follow them.
    for (size_t index = 0; index < sizeof(functions); ++index) {
        raw.chunk[index * 32] = functions[index];
        raw.chunk[index * 32 + 2] = 0xd3;
        raw.chunk[index * 32 + 3] = 0x6b;
    }
    const std::vector<uint8_t> expected(raw.chunk, raw.chunk + raw.chunkSize);
    ASSERT_EQ(callback(&raw), 0);
    profiler.NoteProfilingFinalized();
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].bytes, expected);
    EXPECT_EQ(records[0].decoded.size(), 1U);
    EXPECT_EQ(records[0].decodeErrors, 0U);
    EXPECT_EQ(summary.processingErrors, 0U);
    EXPECT_TRUE(summary.complete);
}

TEST_F(ApplicationProfilerTest, MalformedSchedulerMagicStillMarksCollectionIncomplete)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    ASSERT_EQ(profiler.Initialize([](const auto&) {}, [&](const auto& value) { summary = value; }), ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.type = LOG_DATA_TYPE;
    raw.chunkSize = 64;
    raw.chunk[0] = 2;
    raw.chunk[2] = 0xd3;
    raw.chunk[3] = 0x6b;
    // The second opaque record has invalid magic and must not be silently ignored.
    raw.chunk[32] = 2;
    ASSERT_EQ(callback(&raw), 0);
    profiler.NoteProfilingFinalized();
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    EXPECT_EQ(summary.processingErrors, 1U);
    EXPECT_FALSE(summary.complete);
}

TEST_F(ApplicationProfilerTest, MalformedKnownDataIsRetainedAndMarksCollectionIncomplete)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    std::vector<ApplicationRawRecord> records;
    ASSERT_EQ(
        profiler.Initialize(
            [&](const auto& record) { records.push_back(record); }, [&](const auto& value) { summary = value; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.type = PMU_DATA_TYPE;
    raw.chunkSize = 1;
    raw.chunk[0] = 0xff;
    ASSERT_EQ(callback(&raw), 0);
    profiler.NoteProfilingFinalized();
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].bytes, std::vector<uint8_t>{0xff});
    EXPECT_EQ(records[0].decodeErrors, 1U);
    EXPECT_EQ(summary.processingErrors, 1U);
    EXPECT_FALSE(summary.complete);
    EXPECT_TRUE(summary.tailConfirmed);
}

TEST_F(ApplicationProfilerTest, StartAndStopFailureAreNotBlindlyRetried)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    std::vector<ApplicationRawRecord> records;
    ASSERT_EQ(
        profiler.Initialize(
            [&records](const auto& record) { records.push_back(record); },
            [&summary](const auto& result) { summary = result; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    callbackDuringStart = true;
    startStatus = -17;
    EXPECT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    EXPECT_EQ(ConfigureCommunication(profiler), ACLPTI_ERROR_INVALID_STATE);
    EXPECT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    EXPECT_EQ(starts.size(), 1);
    EXPECT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    EXPECT_TRUE(stops.empty());
    EXPECT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    EXPECT_TRUE(stops.empty());
    callbackDuringStart = false;
    startStatus = 0;
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 1), ACL_SUCCESS);
    stopStatus = -18;
    EXPECT_EQ(profiler.ResetDevice(&DeviceCall, 1, false), ACL_SUCCESS);
    EXPECT_EQ(profiler.ResetDevice(&DeviceCall, 1, false), ACL_SUCCESS);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_ERROR_PROFILING_FAILED);
    EXPECT_EQ(stops.size(), 1);
    EXPECT_EQ(summary.devices.at(0).state, ApplicationDeviceState::Unknown);
    EXPECT_EQ(summary.devices.at(0).startStatus, -17);
    EXPECT_EQ(summary.devices.at(1).stopStatus, -18);
    EXPECT_EQ(summary.devices.at(1).state, ApplicationDeviceState::Unknown);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].deviceEpoch, 0U);
}

TEST_F(ApplicationProfilerTest, ShutdownWithoutResetPreservesDataAndDoesNotStopDevices)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    ASSERT_EQ(
        profiler.Initialize([](const auto&) {}, [&summary](const auto& result) { summary = result; }), ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.deviceId = 0;
    raw.type = kAdditionalRawDataType;
    raw.chunkSize = 1;
    ASSERT_EQ(callback(&raw), 0);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_ERROR_PROFILING_FAILED);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_ERROR_PROFILING_FAILED);
    profiler.NoteProfilingFinalized();
    EXPECT_EQ(profiler.Shutdown(ApplicationEndReason::ProcessExit), ACLPTI_ERROR_PROFILING_FAILED);
    EXPECT_TRUE(stops.empty());
    EXPECT_EQ(summary.processedRecords, 1U);
    EXPECT_EQ(summary.devices.at(0).state, ApplicationDeviceState::Active);
    EXPECT_FALSE(summary.complete);
    EXPECT_FALSE(summary.tailConfirmed);
    EXPECT_EQ(summary.endReason, ApplicationEndReason::LocalShutdown);
}

TEST_F(ApplicationProfilerTest, OwnsCallbackBytesAndDoesNotAssignLateDataToRestartedEpoch)
{
    ApplicationProfiler profiler;
    std::vector<ApplicationRawRecord> records;
    ApplicationSummary summary;
    ASSERT_EQ(
        profiler.Initialize(
            [&records](const auto& raw) { records.push_back(raw); },
            [&summary](const auto& result) { summary = result; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_NE(callback, nullptr);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.deviceId = 0;
    raw.type = kAdditionalRawDataType;
    raw.chunkModule = 5;
    raw.chunkSize = 2;
    raw.chunk[0] = 'a';
    raw.chunk[1] = 'b';
    raw.isLastChunk = true;
    ASSERT_EQ(callback(&raw), 0);
    raw.chunk[0] = 'z';
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    ASSERT_EQ(callback(&raw), 0);
    raw.deviceId = -1;
    ASSERT_EQ(callback(&raw), 0);
    EXPECT_NE(callback(nullptr), 0);
    raw.chunkSize = RAW_DATA_MAXSIZE + 1;
    EXPECT_NE(callback(&raw), 0);
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    ASSERT_EQ(records.size(), 3);
    EXPECT_EQ(records[0].bytes[0], 'a');
    EXPECT_EQ(records[0].deviceEpoch, 1U);
    EXPECT_EQ(records[1].deviceEpoch, 0U);
    EXPECT_EQ(records[2].deviceId, -1);
    EXPECT_EQ(records[2].deviceEpoch, 0U);
    EXPECT_EQ(summary.processedRecords, 3U);
    EXPECT_EQ(summary.invalidRecords, 2U);
}

TEST_F(ApplicationProfilerTest, DrainsOnceAcrossConcurrentShutdownAndKeepsOtherDevicesReceiving)
{
    ApplicationProfiler profiler;
    std::atomic<unsigned> consumed{0};
    std::atomic<unsigned> drained{0};
    ASSERT_EQ(
        profiler.Initialize([&consumed](const auto&) { ++consumed; }, [&drained](const auto&) { ++drained; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 1), ACL_SUCCESS);
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 0, false), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.deviceId = 1;
    raw.type = kAdditionalRawDataType;
    raw.chunkSize = 1;
    ASSERT_EQ(callback(&raw), 0);
    ASSERT_EQ(profiler.ResetDevice(&DeviceCall, 1, false), ACL_SUCCESS);
    std::thread first([&profiler] { EXPECT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS); });
    std::thread second([&profiler] { EXPECT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS); });
    first.join();
    second.join();
    EXPECT_EQ(consumed, 1U);
    EXPECT_EQ(drained, 1U);
    EXPECT_EQ(profiler.SetDevice(&DeviceCall, 2), ACL_SUCCESS);
    EXPECT_EQ(starts.size(), 2U);
    EXPECT_EQ(profiler.DeviceEpoch(2), 0U);
}

TEST_F(ApplicationProfilerTest, QueueIsBoundedAndReportsLossWithoutBlockingCallback)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    ASSERT_EQ(
        profiler.Initialize(
            [&mutex, &changed, &entered, &release](const auto&) {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&release] { return release; });
            },
            [&summary](const auto& result) { summary = result; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    MsprofRawData raw{};
    raw.type = kAdditionalRawDataType;
    raw.chunkSize = RAW_DATA_MAXSIZE;
    ASSERT_EQ(callback(&raw), 0);
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&entered] { return entered; });
    }
    unsigned dropped = 0;
    for (unsigned i = 0; i < 20000; ++i) {
        const auto status = callback(&raw);
        if (status == ACLPTI_ERROR_QUEUE_FULL) {
            ++dropped;
        } else {
            EXPECT_EQ(status, 0);
        }
    }
    EXPECT_GT(dropped, 0U);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    profiler.NoteProfilingFinalized();
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    EXPECT_EQ(summary.droppedRecords, dropped);
    EXPECT_TRUE(summary.tailConfirmed);
    EXPECT_FALSE(summary.complete);
    EXPECT_EQ(summary.receivedRecords, summary.processedRecords + summary.droppedRecords);
}

TEST_F(ApplicationProfilerTest, ConcurrentCallbacksCopyAllRecordsAndShutdownRejectsConsumerReentry)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    std::atomic<unsigned> rejected{0};
    ASSERT_EQ(
        profiler.Initialize(
            [&profiler, &rejected](const auto&) {
                if (profiler.Shutdown() == ACLPTI_ERROR_INVALID_STATE) {
                    ++rejected;
                }
            },
            [&summary](const auto& result) { summary = result; }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 1), ACL_SUCCESS);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i) {
        workers.emplace_back([] {
            MsprofRawData raw{};
            raw.type = kAdditionalRawDataType;
            raw.chunkSize = RAW_DATA_MAXSIZE;
            for (unsigned j = 0; j < 500; ++j) {
                EXPECT_EQ(callback(&raw), 0);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    profiler.NoteProfilingFinalized();
    ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
    EXPECT_EQ(summary.receivedRecords, 2000U);
    EXPECT_EQ(summary.processedRecords, 2000U);
    EXPECT_EQ(rejected, 2000U);
    EXPECT_TRUE(summary.tailConfirmed);
    EXPECT_TRUE(summary.complete);
    EXPECT_TRUE(stops.empty());
}

TEST_F(ApplicationProfilerTest, DrainCallbackExceptionReturnsCollectionError)
{
    ApplicationProfiler profiler;
    ASSERT_EQ(
        profiler.Initialize([](const auto&) {}, [](const auto&) { throw std::runtime_error("manifest failed"); }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_ERROR_CALLBACK);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_ERROR_CALLBACK);
}

TEST_F(ApplicationProfilerTest, CallbackRegistrationFailureStopsWorkerWithoutPublishing)
{
    ApplicationProfiler profiler;
    unsigned drained = 0;
    uploaderStatus = -1;
    EXPECT_EQ(
        profiler.Initialize([](const auto&) {}, [&drained](const auto&) { ++drained; }),
        ACLPTI_ERROR_INITIALIZATION_FAILED);
    EXPECT_EQ(profiler.Shutdown(), ACLPTI_ERROR_INITIALIZATION_FAILED);
    EXPECT_EQ(drained, 0U);
    EXPECT_TRUE(starts.empty());
}

TEST_F(ApplicationProfilerTest, FinalizeNotificationDoesNotWaitForConsumerAndCloseRunsOnce)
{
    ApplicationProfiler profiler;
    ApplicationSummary summary;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    unsigned drained = 0;
    ASSERT_EQ(
        profiler.Initialize(
            [&](const auto&) {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
            },
            [&](const auto& value) {
                summary = value;
                ++drained;
            }),
        ACLPTI_SUCCESS);
    ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
    ASSERT_EQ(profiler.SetDevice(&DeviceCall, 1), ACL_SUCCESS);
    MsprofRawData raw{};
    raw.chunkSize = 1;
    ASSERT_EQ(callback(&raw), 0);
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    auto notify = std::async(std::launch::async, [&] { profiler.NoteProfilingFinalized(); });
    const auto notified = notify.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(notified, std::future_status::ready);
    // Release even on failure so a regression fails rather than leaving a blocked consumer.
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_NO_THROW(notify.get());
    profiler.NoteProfilingFinalized();
    EXPECT_EQ(callback(&raw), ACLPTI_ERROR_INVALID_STATE);
    EXPECT_EQ(profiler.SetDevice(&DeviceCall, 2), ACL_SUCCESS);
    EXPECT_EQ(starts.size(), 2U);
    EXPECT_EQ(profiler.DeviceEpoch(2), 0U);
    EXPECT_EQ(profiler.Shutdown(ApplicationEndReason::ProcessExit), ACLPTI_SUCCESS);
    EXPECT_EQ(drained, 1U);
    EXPECT_TRUE(summary.complete);
    EXPECT_TRUE(summary.tailConfirmed);
    EXPECT_EQ(summary.endReason, ApplicationEndReason::AclFinalize);
    EXPECT_EQ(summary.devices.at(0).endReason, ApplicationDeviceEndReason::RuntimeFinalize);
    EXPECT_FALSE(summary.devices.at(0).stopAttempted);
    EXPECT_TRUE(stops.empty());
}

TEST_F(ApplicationProfilerTest, ConsumerFailureIsRecordedWithoutMaskingControlErrors)
{
    for (const bool mixedErrors : {false, true}) {
        SCOPED_TRACE(mixedErrors ? "mixed_errors" : "consumer_only");
        ApplicationProfiler profiler;
        ApplicationSummary summary;
        ASSERT_EQ(
            profiler.Initialize(
                [](const auto&) { throw std::runtime_error("output failed"); },
                [&summary](const auto& value) { summary = value; }),
            ACLPTI_SUCCESS);
        ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
        MsprofRawData raw{};
        raw.type = kAdditionalRawDataType;
        if (mixedErrors) {
            startStatus = -10;
            EXPECT_EQ(profiler.SetDevice(&DeviceCall, 0), ACL_SUCCESS);
            startStatus = 0;
            ASSERT_EQ(profiler.SetDevice(&DeviceCall, 1), ACL_SUCCESS);
            stopStatus = -11;
            EXPECT_EQ(profiler.ResetDevice(&DeviceCall, 1, false), ACL_SUCCESS);
            ASSERT_EQ(profiler.SetDevice(&DeviceCall, 2), ACL_SUCCESS);
            EXPECT_EQ(callback(&raw), ACLPTI_ERROR_INVALID_RAW_DATA);
        }
        raw.chunkSize = 1;
        ASSERT_EQ(callback(&raw), 0);
        if (mixedErrors) {
            profiler.NoteProfilingFinalized();
        }
        EXPECT_EQ(profiler.Shutdown(), mixedErrors ? ACLPTI_ERROR_PROFILING_FAILED : ACLPTI_SUCCESS);
        EXPECT_EQ(summary.tailConfirmed, mixedErrors);
        EXPECT_FALSE(summary.complete);
        EXPECT_EQ(summary.invalidRecords, mixedErrors ? 1U : 0U);
        EXPECT_EQ(summary.processingErrors, 1U);
        if (mixedErrors) {
            EXPECT_EQ(summary.devices.at(0).startStatus, -10);
            EXPECT_EQ(summary.devices.at(1).stopStatus, -11);
            EXPECT_EQ(summary.devices.at(1).state, ApplicationDeviceState::Unknown);
            EXPECT_EQ(summary.devices.at(2).state, ApplicationDeviceState::Inactive);
            EXPECT_EQ(stops.size(), 1U);
        }
    }
}

TEST_F(ApplicationProfilerTest, EmptySessionCompletesOnConsumerThreadWithAndWithoutFinalize)
{
    for (const bool finalized : {false, true}) {
        SCOPED_TRACE(finalized ? "with finalize" : "without finalize");
        ApplicationProfiler profiler;
        ApplicationSummary summary;
        std::thread::id drainedThread;
        ASSERT_EQ(
            profiler.Initialize(
                [](const auto&) {},
                [&](const auto& value) {
                    summary = value;
                    drainedThread = std::this_thread::get_id();
                }),
            ACLPTI_SUCCESS);
        ASSERT_EQ(ConfigureCommunication(profiler), ACLPTI_SUCCESS);
        if (finalized) {
            profiler.NoteProfilingFinalized();
        }
        EXPECT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
        EXPECT_TRUE(summary.complete);
        EXPECT_EQ(summary.tailConfirmed, finalized);
        EXPECT_TRUE(summary.devices.empty());
        EXPECT_EQ(summary.receivedRecords, 0U);
        EXPECT_NE(drainedThread, std::thread::id{});
        EXPECT_NE(drainedThread, std::this_thread::get_id());
    }
}

} // namespace

extern "C" aclError aclrtApiInjectionGetFunc(const char* name, aclrtApiFunc* original, aclrtApiFunc*)
{
    if (std::strcmp(name, "aclrtGetLogicDevIdByUserDevId") != 0 || original == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    const auto convert = +[](int32_t userDeviceId, int32_t* logicDeviceId) -> aclError {
        EXPECT_EQ(userDeviceId, 1);
        *logicDeviceId = 3;
        return ACL_SUCCESS;
    };
    *original = reinterpret_cast<aclrtApiFunc>(reinterpret_cast<void*>(convert));
    return ACL_SUCCESS;
}

extern "C" int32_t acltoolUploaderInit(MsprofRawDataCallback uploader)
{
    callback = uploader;
    return uploaderStatus;
}
extern "C" int32_t MsprofStart(uint32_t type, const void* data, uint32_t length)
{
    EXPECT_EQ(type, 8U);
    EXPECT_EQ(length, sizeof(MsprofConfig));
    std::lock_guard<std::mutex> lock(callsMutex);
    starts.push_back(*static_cast<const MsprofConfig*>(data));
    if (callbackDuringStart) {
        MsprofRawData raw{};
        raw.deviceId = static_cast<int32_t>(starts.back().devIdList[0]);
        raw.type = kAdditionalRawDataType;
        raw.chunkSize = 1;
        EXPECT_EQ(callback(&raw), 0);
    }
    return startStatus;
}
extern "C" int32_t MsprofStop(uint32_t type, const void* data, uint32_t length)
{
    EXPECT_EQ(type, 8U);
    EXPECT_EQ(length, sizeof(MsprofConfig));
    std::lock_guard<std::mutex> lock(callsMutex);
    stops.push_back(*static_cast<const MsprofConfig*>(data));
    return stopStatus;
}
