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
#include "config/config.h"
#include "runtime/section_config.h"
#include "ipc/ipc.h"

TEST(NpuComputeOccupancy, AutomaticCollectionNeedsNoSection)
{
    std::vector<std::string> args{"npu-compute", "demo"};
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    npucompute::cli::CliConfig config;
    std::vector<std::string> errors;
    ASSERT_TRUE(npucompute::cli::ParseCli(argv.size(), argv.data(), &config, &errors));
    npucompute::SectionConfig runtime;
    std::string error;
    ASSERT_TRUE(runtime.Load(config.sections, &error)) << error;
    EXPECT_EQ(runtime.Params()->blockResult, ACLPTI_BLOCK_RESULT_ALL);
    args = {"npu-compute", "--set", "full", "demo"};
    argv.clear();
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    ASSERT_TRUE(npucompute::cli::ParseCli(argv.size(), argv.data(), &config, &errors));
    ASSERT_TRUE(runtime.Load(config.sections, &error)) << error;
    EXPECT_EQ(runtime.Params()->blockResult, ACLPTI_BLOCK_RESULT_ALL);
}

TEST(NpuComputeOccupancy, ExistingIpcCarriesJsonAndAllSections)
{
    namespace ipc = npucompute::ipc;
    ipc::Config config{};
    config.sections = {
        "PipeUtilization",       "Memory",     "MemoryL0", "MemoryUB", "L2Cache", "Pipeline", "ArithmeticUtilization",
        "ResourceConflictRatio", "PmSampling", "Roofline"};
    EXPECT_EQ(ipc::DecodeConfig(ipc::EncodeConfig(config)).sections, config.sections);
    EXPECT_EQ(ipc::ArtifactTypeForName("Occupancy.json"), ipc::ArtifactType::Json);
    EXPECT_EQ(ipc::ArtifactTypeForName("collection-p123-0001/Occupancy.json"), ipc::ArtifactType::Json);
}

#include "occupancy/occupancy_trace.h"
#include <boost/json.hpp>
#include <limits>
#include <locale>
#include <set>
#include <stdexcept>
#include <algorithm>

namespace {
aclptiTaskLogRow Log(
    uint8_t kind, uint64_t counter, uint64_t replay = 7, aclptiCoreType core = ACLPTI_CORE_TYPE_AIC, uint8_t coreId = 3)
{
    return {replay, kind, 11, 13, counter, 2, 0, core, coreId};
}
aclptiProfilingDataResult Capture(uint64_t origin = 1000)
{
    aclptiProfilingDataResult result;
    auto& replay = result.replayData[7];
    replay.deviceId = 0;
    replay.taskLogs = {Log(1, origin + 1000), Log(0, origin)};
    replay.blockLogs = {Log(0x25, origin + 700), Log(0x24, origin + 100)};
    replay.pmuRecords.push_back({0x29, 11, 13, ACLPTI_CORE_TYPE_AIC, 3, 2, 0, origin + 20, origin + 900, 600, false});
    return result;
}
constexpr double kTestSystemCounterFrequencyHz = 1.0e9;
const std::map<int32_t, double> kClocks{{0, kTestSystemCounterFrequencyHz}};
boost::json::array Events(const npucompute::OccupancyTrace& trace)
{
    return boost::json::parse(trace.json).as_object().at("traceEvents").as_array();
}
boost::json::object BlockEvent(const npucompute::OccupancyTrace& trace)
{
    for (const auto& event : Events(trace)) {
        const auto& object = event.as_object();
        if (object.at("ph") == "X" && object.at("cat") == "block") {
            return object;
        }
    }
    throw std::runtime_error("no block interval");
}
void Reject(const aclptiProfilingDataResult& capture)
{
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 4);
    EXPECT_NE(trace.status, ACLPTI_SUCCESS);
    EXPECT_FALSE(trace.diagnostic.empty());
    EXPECT_TRUE(trace.json.empty());
}
} // namespace

TEST(NpuComputeOccupancy, UsesTaskRelativeMicrosecondsAndPreservesLargeRawCounters)
{
    for (const auto origin : {uint64_t{0}, uint64_t{1} << 60}) {
        const auto trace = npucompute::BuildOccupancyTrace(Capture(origin), kClocks, 4);
        ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
        EXPECT_EQ(trace.intervalCount, 2);
        const auto event = BlockEvent(trace);
        EXPECT_EQ(event.at("name"), "AIC BLOCK2");
        EXPECT_DOUBLE_EQ(event.at("ts").as_double(), 0.02);
        EXPECT_DOUBLE_EQ(event.at("dur").as_double(), 0.88);
        const auto& args = event.at("args").as_object();
        EXPECT_EQ(args.at("blockStartCounter").as_string(), std::to_string(origin + 20));
        EXPECT_EQ(args.at("blockEndCounter").as_string(), std::to_string(origin + 900));
        EXPECT_EQ(args.at("blockLogStartCounter").as_string(), std::to_string(origin + 100));
        EXPECT_EQ(args.at("blockLogEndCounter").as_string(), std::to_string(origin + 700));
        EXPECT_EQ(args.at("blockCounterSource").as_string(), "PMU 0x29");
        EXPECT_EQ(args.at("pmuTotalCycles").as_string(), "600");
        EXPECT_EQ(args.at("taskStartCounter").as_string(), std::to_string(origin));
        EXPECT_EQ(args.at("launchSequence").as_string(), "4");
        EXPECT_EQ(args.at("replayId").as_string(), "7");
        EXPECT_EQ(args.at("coreId32").as_int64(), 3);
        EXPECT_EQ(args.at("coreId128").as_int64(), 3);
    }
}

TEST(NpuComputeOccupancy, NumbersTracksInNumericBlockOrder)
{
    auto capture = Capture();
    auto& replay = capture.replayData[7];
    auto laterBlock = replay.blockLogs;
    for (auto& row : laterBlock) {
        row.blockId = 10;
        row.coreTypeId = 1;
        row.systemCounter += 100;
    }
    replay.blockLogs.insert(replay.blockLogs.end(), laterBlock.begin(), laterBlock.end());
    auto laterPmu = replay.pmuRecords.front();
    laterPmu.blockId = 10;
    laterPmu.coreId = 1;
    laterPmu.taskStartSystemCounter += 100;
    laterPmu.taskEndSystemCounter += 100;
    replay.pmuRecords.push_back(laterPmu);

    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    std::vector<std::string> blockNames;
    for (const auto& value : Events(trace)) {
        const auto& event = value.as_object();
        if (event.at("ph") == "X" && event.at("cat") == "block") {
            blockNames.push_back(std::string(event.at("name").as_string()));
        }
    }
    ASSERT_EQ(blockNames.size(), 2);
    EXPECT_EQ(blockNames[0], "AIC BLOCK2");
    EXPECT_EQ(blockNames[1], "AIC BLOCK10");
    for (const auto& value : Events(trace)) {
        const auto& event = value.as_object();
        if (event.at("ph") == "M" && event.at("name") == "thread_name") {
            const auto tid = event.at("tid").as_int64();
            const std::vector<std::string> expectedNames = {"Task window", "AIC0", "AIC1"};
            EXPECT_EQ(std::string(event.at("args").as_object().at("name").as_string()), expectedNames.at(tid));
        }
    }
}

TEST(NpuComputeOccupancy, UsesChromeSupportedDisplayTimeUnit)
{
    const auto trace = npucompute::BuildOccupancyTrace(Capture(), kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    const auto root = boost::json::parse(trace.json).as_object();
    const auto* unit = root.if_contains("displayTimeUnit");
    // Chrome tracing accepts ms/ns for display; event timestamps are always microseconds.
    EXPECT_TRUE(unit == nullptr || *unit == "ms" || *unit == "ns");
    const auto event = BlockEvent(trace);
    EXPECT_DOUBLE_EQ(event.at("ts").as_double(), 0.02);
    EXPECT_DOUBLE_EQ(event.at("dur").as_double(), 0.88);
}

TEST(NpuComputeOccupancy, FirstPmuReplayKeepsTaskAndStreamIsolated)
{
    auto capture = Capture();
    capture.replayData[8] = capture.replayData[7];
    for (auto* rows : {&capture.replayData[8].taskLogs, &capture.replayData[8].blockLogs}) {
        for (auto& row : *rows) {
            row.replayId = 8;
        }
    }
    auto& replay = capture.replayData[7];
    auto task = replay.taskLogs;
    auto block = replay.blockLogs;
    for (auto& row : task) {
        row.rtStreamId = 14;
    }
    for (auto& row : block) {
        row.rtStreamId = 14;
    }
    replay.taskLogs.insert(replay.taskLogs.end(), task.begin(), task.end());
    replay.blockLogs.insert(replay.blockLogs.end(), block.begin(), block.end());
    auto pmu = replay.pmuRecords.front();
    pmu.rtStreamId = 14;
    replay.pmuRecords.push_back(pmu);
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    std::set<int64_t> pids;
    for (const auto& event : Events(trace)) {
        if (event.as_object().at("ph") == "X") {
            pids.insert(event.as_object().at("pid").as_int64());
        }
    }
    EXPECT_EQ(pids.size(), 2);
    EXPECT_EQ(trace.intervalCount, 4);
    EXPECT_EQ(trace.json.find("\"replayId\":\"8\""), std::string::npos);
}

TEST(NpuComputeOccupancy, IgnoresEarlierNonPmuAndFailedLaterPmuReplays)
{
    auto capture = Capture();
    capture.replayData[1].kind = aclptiReplayKind::Pipeline;
    capture.replayData[1].status = ACLPTI_ERROR_PROFILING_FAILED;
    capture.replayData[2].kind = aclptiReplayKind::PcSampling;
    auto& later = capture.replayData[8];
    later.deviceId = 0;
    later.status = ACLPTI_ERROR_PROFILING_FAILED;
    later.stats.rejectedChunkCount = 1;
    capture.status = ACLPTI_ERROR_PROFILING_FAILED;
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    EXPECT_EQ(trace.intervalCount, 2);
    EXPECT_EQ(BlockEvent(trace).at("args").as_object().at("replayId"), "7");
}

TEST(NpuComputeOccupancy, FailedFirstPmuDoesNotFallForwardToSuccessfulReplay)
{
    auto capture = Capture();
    capture.replayData[8] = capture.replayData[7];
    for (auto* rows : {&capture.replayData[8].taskLogs, &capture.replayData[8].blockLogs}) {
        for (auto& row : *rows) {
            row.replayId = 8;
        }
    }
    capture.replayData[7].status = ACLPTI_ERROR_PROFILING_FAILED;
    Reject(capture);
}

TEST(NpuComputeOccupancy, SelectsFirstPmuIndependentlyForEachDevice)
{
    auto capture = Capture();
    capture.replayData[9] = capture.replayData[7];
    capture.replayData[9].deviceId = 1;
    for (auto* rows : {&capture.replayData[9].taskLogs, &capture.replayData[9].blockLogs}) {
        for (auto& row : *rows) {
            row.replayId = 9;
        }
    }
    capture.replayData[8].deviceId = 0;
    capture.replayData[8].status = ACLPTI_ERROR_PROFILING_FAILED;
    capture.replayData[10].deviceId = 1;
    capture.replayData[10].status = ACLPTI_ERROR_PROFILING_FAILED;
    const auto trace = npucompute::BuildOccupancyTrace(capture, {{0, 1e9}, {1, 2e9}}, 3);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    EXPECT_EQ(trace.intervalCount, 4);
    std::set<int64_t> devices;
    for (const auto& value : Events(trace)) {
        const auto& event = value.as_object();
        if (event.at("ph") == "X" && event.at("cat") == "block") {
            const auto& args = event.at("args").as_object();
            const auto device = args.at("deviceId").as_int64();
            devices.insert(device);
            EXPECT_EQ(args.at("replayId"), device == 0 ? "7" : "9");
            EXPECT_DOUBLE_EQ(event.at("dur").as_double(), device == 0 ? 0.88 : 0.44);
        }
    }
    EXPECT_EQ(devices, (std::set<int64_t>{0, 1}));
}

TEST(NpuComputeOccupancy, ConflictDowngradesWholeTaskAndKeepsBothObservedIds)
{
    auto capture = Capture();
    auto& replay = capture.replayData[7];
    replay.pmuRecords.front().coreId = 5;
    auto second = replay.blockLogs;
    for (auto& row : second) {
        row.blockId = 4;
        row.coreTypeId = 3;
    }
    replay.blockLogs.insert(replay.blockLogs.end(), second.begin(), second.end());
    auto pmu = replay.pmuRecords.front();
    pmu.blockId = 4;
    pmu.coreId = 3;
    replay.pmuRecords.push_back(pmu);
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    EXPECT_FALSE(trace.diagnostic.empty());
    EXPECT_NE(trace.json.find("AIC0"), std::string::npos);
    EXPECT_NE(trace.json.find("AIC1"), std::string::npos);
    const auto args = BlockEvent(trace).at("args").as_object();
    EXPECT_EQ(args.at("coreId32").as_int64(), 3);
    EXPECT_EQ(args.at("coreId128").as_int64(), 5);
}

TEST(NpuComputeOccupancy, MissingBlockPmuCannotFallBackToBlockLog)
{
    auto capture = Capture();
    capture.replayData[7].pmuRecords.clear();
    Reject(capture);
}

TEST(NpuComputeOccupancy, AivUsesItsOwnBlockPmuCounters)
{
    auto capture = Capture();
    auto& replay = capture.replayData[7];
    auto vectorRows = replay.blockLogs;
    for (auto& row : vectorRows) {
        row.coreType = ACLPTI_CORE_TYPE_AIV;
        row.subBlockId = 1;
    }
    replay.blockLogs.insert(replay.blockLogs.end(), vectorRows.begin(), vectorRows.end());
    auto vectorPmu = replay.pmuRecords.front();
    vectorPmu.coreType = ACLPTI_CORE_TYPE_AIV;
    vectorPmu.subBlockId = 1;
    vectorPmu.taskStartSystemCounter = 1040;
    vectorPmu.taskEndSystemCounter = 1800;
    vectorPmu.totalCycles = 777;
    replay.pmuRecords.push_back(vectorPmu);
    for (auto& row : vectorRows) {
        row.subBlockId = 0;
    }
    replay.blockLogs.insert(replay.blockLogs.end(), vectorRows.begin(), vectorRows.end());
    vectorPmu.subBlockId = 0;
    vectorPmu.coreId = 4;
    for (auto& row : replay.blockLogs) {
        if (row.coreType == ACLPTI_CORE_TYPE_AIV && row.subBlockId == 0) {
            row.coreTypeId = 4;
        }
    }
    replay.pmuRecords.push_back(vectorPmu);
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    EXPECT_EQ(trace.intervalCount, 4);
    EXPECT_NE(trace.json.find("AIC0"), std::string::npos);
    size_t aivCount = 0;
    for (const auto& value : Events(trace)) {
        const auto& event = value.as_object();
        if (event.at("ph") != "X" || event.at("cat") != "block" ||
            event.at("args").as_object().at("coreType") != "AIV") {
            continue;
        }
        const auto& args = event.at("args").as_object();
        EXPECT_EQ(std::string(event.at("name").as_string()), "AIV BLOCK2 SUBBLOCK" + std::to_string(aivCount));
        EXPECT_EQ(args.at("blockId").as_int64(), 2);
        EXPECT_EQ(args.at("subBlockId").as_int64(), static_cast<int64_t>(aivCount));
        ++aivCount;
        EXPECT_DOUBLE_EQ(event.at("ts").as_double(), 0.04);
        EXPECT_DOUBLE_EQ(event.at("dur").as_double(), 0.76);
        EXPECT_EQ(event.at("args").as_object().at("pmuTotalCycles"), "777");
    }
    EXPECT_EQ(aivCount, 2);
    std::set<std::string> trackNames;
    for (const auto& value : Events(trace)) {
        const auto& event = value.as_object();
        if (event.at("ph") == "M" && event.at("name") == "thread_name") {
            trackNames.insert(std::string(event.at("args").as_object().at("name").as_string()));
        }
    }
    EXPECT_EQ(trackNames, (std::set<std::string>{"Task window", "AIC0", "AIV0", "AIV1"}));
}

TEST(NpuComputeOccupancy, RejectsReversedOrOutOfTaskPmuCounters)
{
    auto capture = Capture();
    capture.replayData[7].pmuRecords.front().taskStartSystemCounter = 999;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].pmuRecords.front().taskEndSystemCounter = 2001;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].pmuRecords.front().taskStartSystemCounter = 1901;
    Reject(capture);
}

TEST(NpuComputeOccupancy, TaskPmuCountersDoNotReplaceBlockPmuCounters)
{
    auto capture = Capture();
    capture.replayData[7].pmuRecords.push_back({0x2a, 11, 13, ACLPTI_CORE_TYPE_AIC, 3, 0, 0, 1000, 2000, 900, false});
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    const auto args = BlockEvent(trace).at("args").as_object();
    EXPECT_EQ(args.at("blockStartCounter"), "1020");
    EXPECT_EQ(args.at("blockEndCounter"), "1900");
    EXPECT_EQ(args.at("pmuTotalCycles"), "600");
    size_t startCounterFields = 0;
    size_t position = 0;
    const std::string field = "\"pmuTaskStartCounter\"";
    while ((position = trace.json.find(field, position)) != std::string::npos) {
        ++startCounterFields;
        position += field.size();
    }
    EXPECT_EQ(startCounterFields, 2);
}

TEST(NpuComputeOccupancy, PmuOverlapOnSequentialBlockLogsUsesLogicalTracks)
{
    auto capture = Capture();
    auto& replay = capture.replayData[7];
    replay.blockLogs.front().systemCounter = 1300;
    auto later = replay.blockLogs;
    for (auto& row : later) {
        row.blockId = 4;
        row.systemCounter += 300;
    }
    replay.blockLogs.insert(replay.blockLogs.end(), later.begin(), later.end());
    auto pmu = replay.pmuRecords.front();
    pmu.blockId = 4;
    pmu.taskEndSystemCounter = 1950;
    replay.pmuRecords.push_back(pmu);
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    EXPECT_NE(trace.diagnostic.find("PMU"), std::string::npos);
    EXPECT_NE(trace.json.find("AIC1"), std::string::npos);
    EXPECT_EQ(trace.intervalCount, 3);
}

TEST(NpuComputeOccupancy, RejectsMissingDuplicateReversedAndOutOfTaskBoundaries)
{
    auto capture = Capture();
    capture.replayData[7].taskLogs.pop_back();
    Reject(capture);
    capture = Capture();
    capture.replayData[7].taskLogs.push_back(Log(0, 1000));
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs.pop_back();
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs.push_back(Log(0x24, 1100));
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs[0].systemCounter = 1050;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs[0].systemCounter = 2001;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs[1].systemCounter = 999;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs[1].coreTypeId = 9;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs[0].rtStreamId = 99;
    Reject(capture);
}

TEST(NpuComputeOccupancy, RejectsFailedMalformedOrEmptyPmuReplays)
{
    auto capture = Capture();
    capture.replayData[7].status = ACLPTI_ERROR_PROFILING_FAILED;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].stats.failedRecordCount = 1;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].stats.rejectedChunkCount = 1;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].pmuRecords.push_back(capture.replayData[7].pmuRecords.front());
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs.front().coreType = static_cast<aclptiCoreType>(9);
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs.front().replayId = 8;
    Reject(capture);
    capture = Capture();
    capture.replayData[7].blockLogs.clear();
    Reject(capture);
    capture = Capture();
    capture.replayData[7].kind = aclptiReplayKind::Pipeline;
    Reject(capture);
    Reject({});
}

TEST(NpuComputeOccupancy, RejectsUnknownOrInvalidSystemFrequency)
{
    for (const auto frequency :
         {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        const auto trace = npucompute::BuildOccupancyTrace(Capture(), {{0, frequency}}, 0);
        EXPECT_NE(trace.status, ACLPTI_SUCCESS);
        EXPECT_FALSE(trace.diagnostic.empty());
    }
    EXPECT_NE(npucompute::BuildOccupancyTrace(Capture(), {}, 0).status, ACLPTI_SUCCESS);
}

TEST(NpuComputeOccupancy, RejectsOverlappingIntervalsOnConsistentObservedCore)
{
    auto capture = Capture();
    auto& replay = capture.replayData[7];
    auto rows = replay.blockLogs;
    for (auto& row : rows) {
        row.blockId = 8;
    }
    replay.blockLogs.insert(replay.blockLogs.end(), rows.begin(), rows.end());
    auto pmu = replay.pmuRecords.front();
    pmu.blockId = 8;
    replay.pmuRecords.push_back(pmu);
    Reject(capture);
}

TEST(NpuComputeOccupancy, CrossTaskOverlapFallsBackUsingRawCounters)
{
    auto capture = Capture();
    auto& replay = capture.replayData[7];
    auto tasks = replay.taskLogs;
    auto blocks = replay.blockLogs;
    for (auto& row : tasks) {
        row.rtStreamId = 14;
        row.systemCounter += 100;
    }
    for (auto& row : blocks) {
        row.rtStreamId = 14;
        row.systemCounter += 100;
    }
    replay.taskLogs.insert(replay.taskLogs.end(), tasks.begin(), tasks.end());
    replay.blockLogs.insert(replay.blockLogs.end(), blocks.begin(), blocks.end());
    auto pmu = replay.pmuRecords.front();
    pmu.rtStreamId = 14;
    pmu.taskStartSystemCounter += 100;
    pmu.taskEndSystemCounter += 100;
    replay.pmuRecords.push_back(pmu);
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    EXPECT_NE(trace.diagnostic.find("overlap"), std::string::npos);
    EXPECT_NE(trace.json.find("AIC0"), std::string::npos);
}

namespace {
class GroupedNumbers final : public std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};
} // namespace
TEST(NpuComputeOccupancy, JsonNumbersIgnoreApplicationGlobalLocale)
{
    struct RestoreLocale {
        std::locale saved = std::locale();
        ~RestoreLocale() { std::locale::global(saved); }
    } restore;
    auto capture = Capture();
    auto& replay = capture.replayData[7];
    replay.pmuRecords.front().blockId = 1234;
    replay.pmuRecords.front().subBlockId = 5678;
    for (auto& row : replay.blockLogs) {
        row.blockId = 1234;
        row.subBlockId = 5678;
    }
    std::locale::global(std::locale(std::locale::classic(), new GroupedNumbers));
    const auto trace = npucompute::BuildOccupancyTrace(capture, kClocks, 0);
    ASSERT_EQ(trace.status, ACLPTI_SUCCESS) << trace.diagnostic;
    const auto args = BlockEvent(trace).at("args").as_object();
    EXPECT_EQ(args.at("blockId").as_int64(), 1234);
    EXPECT_EQ(args.at("subBlockId").as_int64(), 5678);
}
