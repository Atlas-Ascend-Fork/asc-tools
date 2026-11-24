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
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/json.hpp>
#include <sys/stat.h>
#include "common/trace_writer.h"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <set>
#include <map>
#include <unistd.h>
#include "cli/report/communication_finalizer.h"
#include "communication/record_json.h"
#include "aclpti/aclpti_data.h"
namespace {
namespace fs = boost::filesystem;
namespace comm = npucompute::communication;
using aclpti::communication::Record;
using aclpti::communication::RecordKind;
class CommunicationReportTest : public testing::Test {
protected:
    fs::path directory;
    void SetUp() override
    {
        const char* temporary = std::getenv("TMPDIR");
        const auto base = temporary ? fs::path(temporary) : fs::temp_directory_path();
        directory = base / "communication-profiling" / fs::unique_path("report-ut-%%%%-%%%%");
        fs::create_directories(directory / ".communication-staging/source");
    }
    // Fixtures remain available for inspection with the other task evidence.
    void Stage(
        const std::vector<Record>& records, bool calibrated = true, bool complete = true,
        const std::string& source = "source")
    {
        const auto path = directory / ".communication-staging" / source;
        fs::create_directories(path);
        std::ofstream rows((path / "records-0.jsonl").string());
        std::set<int32_t> devices;
        for (const auto& record : records) {
            rows << comm::Json(comm::RecordTree(record));
            if (record.deviceId != 64)
                devices.insert(record.deviceId);
        }
        rows.close();
        comm::Tree manifest, files, file, clocks;
        manifest.put("schema", 1);
        manifest.put("complete", complete);
        manifest.put("dropped", 0);
        manifest.put("errors", 0);
        manifest.put("records", records.size());
        file.put("file", "records-0.jsonl");
        file.put("records", records.size());
        files.push_back({"", file});
        for (int32_t device : devices) {
            comm::Tree clock;
            clock.put("deviceId", device);
            const auto mapping = comm::ClockTree({0, 0, 1000000000, calibrated});
            clock.add_child("aicpu", mapping);
            clock.add_child("task", mapping);
            clock.add_child("ccu", mapping);
            clocks.push_back({"", clock});
        }
        manifest.add_child("files", files);
        manifest.add_child("clocks", clocks);
        std::ofstream((path / "manifest.json").string()) << comm::Json(manifest);
    }
    void WriteHostTrack(uint16_t device, uint16_t stream, uint32_t task, uint64_t nameId)
    {
        std::array<uint8_t, 64> record{};
        const auto put = [&](size_t offset, uint64_t value, size_t size) {
            for (size_t i = 0; i < size; ++i)
                record[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        };
        put(0, 0x5a5a, 2);
        put(2, 5000, 2);
        put(4, 802, 4);
        put(12, 40, 4);
        put(24, device, 2);
        put(26, stream, 2);
        put(28, task, 4);
        put(40, nameId, 8);
        WriteHostRaw(16, {reinterpret_cast<const char*>(record.data()), record.size()});
    }
    void WriteHostRaw(int type, std::string_view bytes)
    {
        static constexpr char hex[] = "0123456789abcdef";
        std::string payload;
        for (unsigned char b : bytes) {
            payload.push_back(hex[b >> 4]);
            payload.push_back(hex[b & 15]);
        }
        comm::Tree row;
        row.put("rawType", type);
        row.put("deviceId", 64);
        row.put("deviceEpoch", 0);
        row.put("chunkModule", 5);
        row.put("offset", 0);
        row.put("isLastChunk", true);
        row.put("payloadHex", payload);
        std::ofstream output(
            (directory / ".communication-staging/process-uds/task-tracks.jsonl").string(), std::ios::app);
        output << comm::Json(row);
    }
    std::vector<comm::Tree> Summary()
    {
        std::ifstream input((directory / "app_communication_summary.jsonl").string());
        std::vector<comm::Tree> result;
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream stream(line);
            comm::Tree row;
            boost::property_tree::read_json(stream, row);
            result.push_back(row);
        }
        return result;
    }
    comm::Tree Trace()
    {
        comm::Tree trace;
        boost::property_tree::read_json((directory / "CommunicationTrace.json").string(), trace);
        return trace;
    }
    Record Make(RecordKind kind, int device, uint64_t start, uint64_t end = 0)
    {
        Record record;
        record.kind = kind;
        record.deviceId = device;
        record.taskId = 17;
        record.streamId = 4;
        record.times[0] = start;
        record.times[1] = end;
        return record;
    }
    void AddTask(
        std::vector<Record>& rows, int device, uint32_t taskId, uint32_t streamId, RecordKind metadataKind,
        uint64_t startCounter, uint64_t endCounter)
    {
        auto start = Make(RecordKind::TaskStart, device, startCounter);
        start.taskId = taskId;
        start.streamId = UINT32_MAX;
        start.taskType = metadataKind == RecordKind::LegacyTaskInfo ? 0 : 11;
        auto end = start;
        end.kind = RecordKind::TaskEnd;
        end.times[0] = endCounter;
        auto metadata = start;
        metadata.kind = metadataKind;
        metadata.streamId = streamId;
        metadata.kernelType = metadataKind == RecordKind::LegacyTaskInfo ? 0 : UINT32_MAX;
        rows.insert(rows.end(), {start, end, metadata});
    }
    void AddDevice(std::vector<Record>& rows, int device, uint64_t compute, uint64_t communication)
    {
        AddTask(rows, device, 17, 4, RecordKind::LegacyTaskInfo, compute, compute + 10000);
        AddTask(rows, device, 19, 8, RecordKind::HcclTask, communication, communication + 10000);
    }
    std::vector<comm::Tree> Events(const std::string& name, uint32_t task = UINT32_MAX)
    {
        std::vector<comm::Tree> result;
        for (const auto& entry : Trace()) {
            if (entry.second.get<std::string>("name", "") == name &&
                (task == UINT32_MAX || entry.second.get<uint32_t>("args.TaskId", UINT32_MAX) == task))
                result.push_back(entry.second);
        }
        return result;
    }
    std::map<int64_t, std::string> Processes()
    {
        std::map<int64_t, std::string> result;
        for (const auto& entry : Events("process_name"))
            result[entry.get<int64_t>("pid")] = entry.get<std::string>("args.name");
        return result;
    }
    std::string TrackFor(const comm::Tree& event)
    {
        for (const auto& thread : Events("thread_name")) {
            if (thread.get<int64_t>("pid") == event.get<int64_t>("pid") &&
                thread.get<int64_t>("tid") == event.get<int64_t>("tid"))
                return thread.get<std::string>("args.name");
        }
        return "";
    }
};
TEST_F(CommunicationReportTest, PerDeviceEnvelopesHaveNoGlobalOverlapOrGlobalProcess)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 0, 20000);
    AddDevice(rows, 1, 20000, 0);
    Stage(rows);
    std::vector<std::string> issues;
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, &issues));
    EXPECT_TRUE(issues.empty());
    const auto summary = Summary();
    ASSERT_EQ(summary.size(), 2);
    for (size_t i = 0; i < summary.size(); ++i) {
        EXPECT_EQ(summary[i].get<int>("Device Id"), i);
        EXPECT_EQ(summary[i].get<std::string>("category"), "DeviceCommunicationSummary");
        EXPECT_EQ(summary[i].get<std::string>("Scope"), "Device");
        EXPECT_DOUBLE_EQ(summary[i].get<double>("Overlap Duration(us)"), 0);
        EXPECT_DOUBLE_EQ(summary[i].get<double>("Compute Duration(us)"), 10);
    }
    for (const auto& entry : Trace()) {
        EXPECT_GE(entry.second.get<int64_t>("pid"), 0);
        EXPECT_GE(entry.second.get<int64_t>("tid"), 0);
    }
    const auto processes = Processes();
    ASSERT_EQ(processes.size(), 6);
    EXPECT_EQ(processes.at(1), "AiCpu-Compute-Npu0");
    EXPECT_EQ(processes.at(2), "AiCpu-Communication-Npu0");
    EXPECT_EQ(processes.at(5), "Computation-Communication-Fusion-Npu0");
    EXPECT_EQ(Events("process_sort_index").size(), processes.size());
    EXPECT_EQ(Events("thread_sort_index").size(), Events("thread_name").size());
}
TEST_F(CommunicationReportTest, DeviceEnvelopesIncludeGapsAndTheirIntersection)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 1000, 6000);
    AddTask(rows, 0, 18, 4, RecordKind::LegacyTaskInfo, 21000, 31000);
    Stage(rows);
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto summary = Summary();
    ASSERT_EQ(summary.size(), 1);
    EXPECT_DOUBLE_EQ(summary[0].get<double>("Compute Duration(us)"), 30);
    EXPECT_DOUBLE_EQ(summary[0].get<double>("Communication Duration(us)"), 10);
    EXPECT_DOUBLE_EQ(summary[0].get<double>("Overlap Duration(us)"), 10);
    ASSERT_EQ(Events("Compute").size(), 1);
    EXPECT_DOUBLE_EQ(Events("Compute")[0].get<double>("dur"), 30);
    const auto tracks = Events("thread_sort_index");
    std::vector<int> fusionOrder;
    for (const auto& event : tracks)
        if (event.get<int>("pid") == 5)
            fusionOrder.push_back(event.get<int>("args.sort_index"));
    EXPECT_EQ(fusionOrder, (std::vector<int>{0, 1, 2}));
}
TEST_F(CommunicationReportTest, LateMetadataEnablesItsDeviceAndMode)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 1000, 6000);
    auto mission = Make(RecordKind::CcuMission, 1, 4000, 5000);
    auto orphan = Make(RecordKind::ComputeTurn, 2, 1000, 2000);
    orphan.times[2] = 3000;
    rows.insert(rows.end(), {mission, orphan});
    Stage(rows);
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto processes = Processes();
    ASSERT_EQ(processes.size(), 6);
    EXPECT_EQ(processes.at(1), "AiCpu-Compute-Npu0");
    EXPECT_EQ(processes.at(8), "Ccu-Compute-Npu1");
    EXPECT_EQ(processes.at(9), "Ccu-Communication-Npu1");
    ASSERT_EQ(Summary().size(), 3);
    EXPECT_EQ(Summary()[2].get<std::string>("Compute Duration(us)"), "NA");
}

TEST_F(CommunicationReportTest, LegacyMetadataIsOptionalForComputeTaskLogs)
{
    std::vector<Record> rows;
    AddTask(rows, 0, 17, 4, RecordKind::LegacyTaskInfo, 1000, 3000);
    fs::remove(directory / ".communication-staging/source");
    Stage({rows[0], rows[1]}, true, true, "a-data");
    Stage({rows[2]}, true, true, "z-metadata");
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto compute = Events("AiCpu-Comp-Launch", 17);
    ASSERT_EQ(compute.size(), 1);
    EXPECT_EQ(TrackFor(compute.front()), "Stream unresolved");
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Compute Duration(us)"), 2);
}

TEST_F(CommunicationReportTest, AicpuUsesTaskLogAndMetadataForPlaneTracks)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 1000, 7000);
    AddTask(rows, 0, 20, 8, RecordKind::HcclTask, 19000, 20000);
    rows[2].kernelType = 2;
    rows[5].rankId = 1;
    rows[5].remoteRank = 0;
    rows[5].dataLen = 4096;
    rows[5].notifyId = 55;
    rows[5].planeId = 3;
    rows[5].times[0] = 99999; // The metadata timestamp differs from the TaskLog.
    rows[8].remoteRank = UINT32_MAX;
    rows[8].notifyId = UINT64_MAX;
    auto turn = Make(RecordKind::ComputeTurn, 0, 1000, 100000);
    turn.times[2] = 200000;
    auto commTurn = turn;
    commTurn.kind = RecordKind::CommTurn;
    rows.insert(rows.begin(), {turn, commTurn});
    Stage(rows);
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto compute = Events("AiCpu-Comp-Launch", 17);
    const auto firstComm = Events("AiCpu-Comm-Task", 19);
    const auto secondComm = Events("AiCpu-Comm-Task", 20);
    ASSERT_EQ(compute.size(), 1);
    ASSERT_EQ(firstComm.size(), 1);
    ASSERT_EQ(secondComm.size(), 1);
    EXPECT_EQ(TrackFor(compute[0]), "Stream unresolved");
    EXPECT_EQ(TrackFor(firstComm[0]), "Plane 3");
    EXPECT_EQ(TrackFor(secondComm[0]), "Plane 0");
    EXPECT_EQ(firstComm[0].get<uint32_t>("args.planeID"), 3);
    EXPECT_EQ(firstComm[0].get<uint32_t>("args.SqId"), 8);
    EXPECT_EQ(firstComm[0].get<std::string>("args.StreamId"), "N/A");
    EXPECT_DOUBLE_EQ(compute[0].get<double>("ts"), 1);
    EXPECT_DOUBLE_EQ(compute[0].get<double>("dur"), 10);
    EXPECT_DOUBLE_EQ(firstComm[0].get<double>("dur"), 10);
    EXPECT_DOUBLE_EQ(secondComm[0].get<double>("dur"), 1);
    EXPECT_DOUBLE_EQ(compute[0].get<double>("args.StartTime"), 1);
    EXPECT_DOUBLE_EQ(compute[0].get<double>("args.EndTime"), 11);
    EXPECT_EQ(compute[0].get<std::string>("args.SqeType"), "AIC");
    EXPECT_DOUBLE_EQ(firstComm[0].get<double>("args.StartTime"), 7);
    EXPECT_DOUBLE_EQ(firstComm[0].get<double>("args.EndTime"), 17);
    EXPECT_EQ(firstComm[0].get<uint32_t>("args.localRank"), 1);
    EXPECT_EQ(firstComm[0].get<uint32_t>("args.remoteRank"), 0);
    EXPECT_EQ(firstComm[0].get<uint64_t>("args.dataSize"), 4096);
    EXPECT_EQ(firstComm[0].get<uint64_t>("args.notifyID"), 55);
    EXPECT_EQ(secondComm[0].get<std::string>("args.remoteRank"), "N/A");
    EXPECT_EQ(secondComm[0].get<std::string>("args.notifyID"), "N/A");

    const auto fusionCompute = Events("Compute");
    const auto fusionCommunication = Events("Communication");
    const auto fusionOverlap = Events("Overlap");
    ASSERT_EQ(fusionCompute.size(), 1);
    ASSERT_EQ(fusionCommunication.size(), 1);
    ASSERT_EQ(fusionOverlap.size(), 1);
    EXPECT_EQ(fusionCompute[0].get<int>("args.deviceId"), 0);
    EXPECT_EQ(fusionCompute[0].get<std::string>("args.communicationMode"), "AICPU");
    EXPECT_EQ(fusionCompute[0].get<std::string>("args.aggregation"), "envelope");
    EXPECT_EQ(fusionCompute[0].get<size_t>("args.intervalCount"), 1);
    EXPECT_EQ(fusionCommunication[0].get<size_t>("args.intervalCount"), 2);
    EXPECT_EQ(fusionOverlap[0].get<std::string>("args.aggregation"), "envelopeIntersection");
    EXPECT_DOUBLE_EQ(fusionOverlap[0].get<double>("args.computeEnvelopeUs"), 10);
    EXPECT_DOUBLE_EQ(fusionOverlap[0].get<double>("args.communicationEnvelopeUs"), 13);
    EXPECT_TRUE(Events("Task17-Turn0-Duration").empty());
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Overlap Duration(us)"), 4);
}

TEST_F(CommunicationReportTest, AicpuPlaneGroupsQueuesAndPreservesTasksWithInvalidPlane)
{
    std::vector<Record> rows;
    AddTask(rows, 0, 19, 8, RecordKind::HcclTask, 1000, 3000);
    rows.back().planeId = 2;
    AddTask(rows, 0, 20, 9, RecordKind::HcclTask, 2000, 4000);
    rows.back().planeId = 2;
    AddTask(rows, 0, 21, 9, RecordKind::HcclTask, 4000, 5000);
    rows.back().planeId = UINT16_MAX;
    AddTask(rows, 0, 22, 9, RecordKind::HcclTask, 5000, 6000);
    rows.back().planeId = UINT32_MAX;
    Stage(rows);
    ASSERT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto first = Events("AiCpu-Comm-Task", 19);
    const auto second = Events("AiCpu-Comm-Task", 20);
    const auto missing = Events("AiCpu-Comm-Task", 21);
    const auto invalid = Events("AiCpu-Comm-Task", 22);
    ASSERT_EQ(first.size(), 1);
    ASSERT_EQ(second.size(), 1);
    ASSERT_EQ(missing.size(), 1);
    ASSERT_EQ(invalid.size(), 1);
    EXPECT_EQ(TrackFor(first[0]), "Plane 2");
    EXPECT_EQ(first[0].get<int64_t>("tid"), second[0].get<int64_t>("tid"));
    EXPECT_EQ(first[0].get<uint32_t>("args.SqId"), 8);
    EXPECT_EQ(second[0].get<uint32_t>("args.SqId"), 9);
    EXPECT_EQ(TrackFor(missing.front()), "Communication");
    EXPECT_EQ(TrackFor(invalid.front()), "Communication");
    EXPECT_EQ(missing.front().get<std::string>("args.planeID"), "N/A");
    EXPECT_EQ(invalid.front().get<std::string>("args.planeID"), "N/A");
}

TEST_F(CommunicationReportTest, ReusedTaskIdentityKeepsSeparateTaskLogPairs)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 0, 20000);
    AddTask(rows, 0, 19, 8, RecordKind::HcclTask, 50000, 60000);
    rows.back().planeId = 5;
    std::reverse(rows.begin(), rows.end());
    Stage(rows);
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto tasks = Events("AiCpu-Comm-Task", 19);
    ASSERT_EQ(tasks.size(), 2);
    for (const auto& task : tasks) {
        EXPECT_EQ(task.get<std::string>("args.localRank"), "N/A");
        EXPECT_EQ(TrackFor(task), "Communication");
    }
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Communication Duration(us)"), 40);
}

TEST_F(CommunicationReportTest, MissingTaskLogPairPreservesOtherDomain)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 1000, 20000);
    rows.erase(rows.begin() + 4); // Missing communication end.
    Stage(rows);
    testing::internal::CaptureStderr();
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("no usable communication TaskLog pair for device 0 Task19"), std::string::npos);
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Compute Duration(us)"), 10);
    EXPECT_EQ(Summary()[0].get<std::string>("Communication Duration(us)"), "NA");
    EXPECT_EQ(Summary()[0].get<std::string>("Overlap Duration(us)"), "NA");
    EXPECT_TRUE(Events("AiCpu-Comm-Task", 19).empty());
}

TEST_F(CommunicationReportTest, AmbiguousStreamDoesNotHideValidTasks)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 1000, 20000);
    auto conflicting = rows.back();
    conflicting.streamId = 9;
    rows.push_back(conflicting);
    AddTask(rows, 0, 21, 8, RecordKind::HcclTask, 40000, 42000);
    Stage(rows);
    testing::internal::CaptureStderr();
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("ambiguous TaskLog Stream for device 0 Task19"), std::string::npos);
    EXPECT_TRUE(Events("AiCpu-Comm-Task", 19).empty());
    EXPECT_EQ(Events("AiCpu-Comm-Task", 21).size(), 1);
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Communication Duration(us)"), 2);
}

TEST_F(CommunicationReportTest, CcuSelectsOneIntervalPerInstructionAndUsesAllIntervalsForSummary)
{
    auto mission = Make(RecordKind::CcuMission, 0, 10000, 12000);
    mission.streamId = UINT32_MAX;
    mission.instrId = 271;
    mission.setckeInstrId = 51;
    mission.times[2] = 11000;
    mission.relative[3] = 2000;
    auto second = mission;
    second.times[0] = 0;
    second.times[1] = 0;
    second.times[2] = 20000;
    second.relative[3] = 1000;
    auto loop = Make(RecordKind::CcuMission, 0, 16000, 17000);
    loop.streamId = UINT32_MAX;
    loop.instrId = 271;
    auto lastLoop = loop;
    lastLoop.times[0] = 15000;
    lastLoop.times[1] = 15500;
    auto metadata = Make(RecordKind::CcuTask, 0, 0);
    metadata.rawType = 17;
    metadata.level = 5500;
    metadata.type = 14;
    metadata.streamId = 8;
    auto hostWait = metadata;
    hostWait.kind = RecordKind::CcuWait;
    hostWait.type = 15;
    hostWait.instrId = 51;
    hostWait.mask = 8;
    hostWait.channelIds.fill(UINT16_MAX);
    hostWait.channelIds[0] = 3;
    auto hostGroup = metadata;
    hostGroup.kind = RecordKind::CcuGroup;
    hostGroup.type = 16;
    hostGroup.instrId = 271;
    hostGroup.dataLen = 2048;
    hostGroup.opType = 2;
    hostGroup.dataType = 4;
    hostGroup.outputDataType = 3;
    auto sample = Make(RecordKind::CcuChannel, 0, 0);
    sample.channelId = 3;
    sample.timestamp = 23000;
    sample.channelAverage = 71;
    auto late = sample;
    late.timestamp = 24000;
    late.channelAverage = 90;
    Stage({mission, second, loop, lastLoop, metadata, hostWait, hostGroup, late, sample});
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto waits = Events("Wait");
    ASSERT_EQ(waits.size(), 1);
    const auto& wait = waits.front();
    EXPECT_EQ(TrackFor(wait), "Communication");
    EXPECT_EQ(wait.get<uint32_t>("args.setckeBitInstrId"), 51);
    EXPECT_EQ(wait.get<uint32_t>("args.StreamId"), 8);
    EXPECT_EQ(wait.get<uint32_t>("args.Notify Rank Id"), 12);
    EXPECT_DOUBLE_EQ(wait.get<double>("args.StartTime"), 20);
    EXPECT_DOUBLE_EQ(wait.get<double>("args.EndTime"), 24);
    EXPECT_EQ(wait.get<std::string>("args.LpInstrId"), "N/A");
    EXPECT_EQ(wait.get<std::string>("args.Mask"), "0x8");
    EXPECT_EQ(wait.get<uint32_t>("args.Maximum Delay Channel"), 3);
    EXPECT_EQ(wait.get<uint32_t>("args.Maximum Channel Delay"), 71);
    EXPECT_DOUBLE_EQ(wait.get<double>("ts"), 20);
    EXPECT_DOUBLE_EQ(wait.get<double>("dur"), 4);
    const auto loops = Events("LoopGroup");
    ASSERT_EQ(loops.size(), 1);
    const auto& loopEvent = loops.front();
    EXPECT_EQ(TrackFor(loopEvent), "Communication");
    EXPECT_EQ(wait.get<uint32_t>("tid"), loopEvent.get<uint32_t>("tid"));
    EXPECT_DOUBLE_EQ(loopEvent.get<double>("ts"), 15);
    EXPECT_DOUBLE_EQ(loopEvent.get<double>("dur"), 0.5);
    EXPECT_EQ(loopEvent.get<uint32_t>("args.LpInstrId"), 271);
    EXPECT_DOUBLE_EQ(loopEvent.get<double>("args.StartTime"), 15);
    EXPECT_DOUBLE_EQ(loopEvent.get<double>("args.EndTime"), 15.5);
    EXPECT_EQ(loopEvent.get<std::string>("args.Notify Rank Id"), "N/A");
    EXPECT_EQ(loopEvent.get<uint64_t>("args.Data Size"), 2048);
    EXPECT_DOUBLE_EQ(loopEvent.get<double>("args.Bandwidth (MB/s)"), 4096);
    EXPECT_EQ(loopEvent.get<std::string>("args.Reduce Op Type"), "MAX");
    EXPECT_EQ(loopEvent.get<std::string>("args.Input Data Type"), "FP32");
    EXPECT_EQ(loopEvent.get<std::string>("args.Output Data Type"), "FP16");
    EXPECT_TRUE(Events("Ccu-Comp-Launch", 17).empty());
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Communication Duration(us)"), 14);
    EXPECT_DOUBLE_EQ(Events("Communication")[0].get<double>("dur"), 14);
}
TEST_F(CommunicationReportTest, CcuStreamLookupUsesFullTaskIdAndDevice)
{
    auto mission = Make(RecordKind::CcuMission, 0, 10000, 12000);
    mission.taskId = 0x10011;
    mission.streamId = UINT32_MAX;
    mission.instrId = 271;
    auto otherCard = mission;
    otherCard.deviceId = 1;
    otherCard.taskId = 0x20011;
    auto metadata = Make(RecordKind::CcuTask, 64, 0);
    metadata.rawType = 17;
    metadata.level = 5500;
    metadata.type = 14;
    metadata.taskId = mission.taskId;
    metadata.streamId = 60;
    auto lowHalf = metadata;
    lowHalf.deviceId = 1;
    lowHalf.taskId = otherCard.taskId & UINT16_MAX;
    auto taskTrack = Make(RecordKind::RuntimeTaskTrack, 1, 0);
    taskTrack.taskId = otherCard.taskId;
    taskTrack.streamId = 61;
    Stage({mission, otherCard, metadata, lowHalf, taskTrack});
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto loops = Events("LoopGroup");
    ASSERT_EQ(loops.size(), 2);
    EXPECT_EQ(loops[0].get<uint32_t>("args.TaskId"), mission.taskId);
    EXPECT_EQ(loops[0].get<uint32_t>("args.StreamId"), 60);
    EXPECT_EQ(loops[1].get<std::string>("args.StreamId"), "N/A");
    ASSERT_EQ(Summary().size(), 2);
}
TEST_F(CommunicationReportTest, HostCcuTaskInfoWithMultipleCandidateDevicesIsUnresolved)
{
    auto mission = Make(RecordKind::CcuMission, 0, 10000, 12000);
    auto otherCard = mission;
    otherCard.deviceId = 1;
    auto metadata = Make(RecordKind::CcuTask, 64, 0);
    metadata.rawType = 17;
    metadata.level = 5500;
    metadata.type = 14;
    metadata.streamId = 60;
    Stage({mission, otherCard, metadata});
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto loops = Events("LoopGroup");
    ASSERT_EQ(loops.size(), 2);
    for (const auto& event : loops)
        EXPECT_EQ(event.get<std::string>("args.StreamId"), "N/A");
    ASSERT_EQ(Summary().size(), 2);
}
TEST_F(CommunicationReportTest, CcuComputationUsesTaskLogTypesAndExplicitComputeType)
{
    std::vector<Record> rows;
    auto ccu = Make(RecordKind::CcuTask, 0, 0);
    rows.push_back(ccu);
    auto mission = Make(RecordKind::CcuMission, 0, 30000, 40000);
    rows.push_back(mission);
    for (uint32_t taskId = 1; taskId <= 8; ++taskId) {
        auto start = Make(RecordKind::TaskStart, 0, taskId * 1000);
        start.taskId = taskId;
        start.taskType = taskId == 3 ? 16 : (taskId == 4 ? UINT32_MAX : (taskId == 2 || taskId == 8 ? 2 : 0));
        start.fusionType = taskId == 2 ? 4 : (taskId == 8 ? 2 : 0);
        auto end = start;
        end.kind = RecordKind::TaskEnd;
        end.times[0] += 500;
        auto info = start;
        info.kind = RecordKind::LegacyTaskInfo;
        info.kernelType = taskId == 5 ? 100 : (taskId == 7 ? UINT32_MAX : 0);
        rows.insert(rows.end(), {start, end});
        if (taskId != 6)
            rows.push_back(info);
    }
    Stage(rows);
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto processes = Processes();
    std::set<uint32_t> computeTasks;
    for (const auto& entry : Trace()) {
        if (entry.second.get<std::string>("ph", "") == "X" && entry.second.get<int>("pid") == 3) {
            computeTasks.insert(entry.second.get<uint32_t>("args.TaskId"));
            EXPECT_EQ(entry.second.get<std::string>("name"), "Ccu-Comp-Launch");
            EXPECT_NE(entry.second.get<std::string>("args.SqeType"), "N/A");
            EXPECT_EQ(TrackFor(entry.second), "Stream unresolved");
        }
    }
    EXPECT_EQ(computeTasks, (std::set<uint32_t>{1, 2, 5, 6, 7}));
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Compute Duration(us)"), 6.5);
}
TEST_F(CommunicationReportTest, MissingCcuComputeTaskLogPairRetainsKnownComputeIntervals)
{
    auto start = Make(RecordKind::TaskStart, 0, 10000);
    start.taskType = 0;
    auto end = start;
    end.kind = RecordKind::TaskEnd;
    end.times[0] = 20000;
    auto info = start;
    info.kind = RecordKind::LegacyTaskInfo;
    info.kernelType = 0;
    auto missing = info;
    missing.taskId = 18;
    auto ccu = Make(RecordKind::CcuTask, 0, 0);
    auto mission = Make(RecordKind::CcuMission, 0, 30000, 40000);
    Stage({start, end, info, missing, ccu, mission});
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Compute Duration(us)"), 10);
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Overlap Duration(us)"), 0);
    EXPECT_FALSE(Summary()[0].get_optional<std::string>("Status"));
    EXPECT_FALSE(Events("Ccu-Comp-Launch", 17).empty());
    EXPECT_FALSE(Events("Compute").empty());
}
TEST_F(CommunicationReportTest, PairingKeepsFusionAndMissionFamiliesSeparate)
{
    auto start = Make(RecordKind::TaskStart, 0, 10000);
    start.taskType = 2;
    start.fusionType = 4;
    start.functionType = 0x16;
    start.missionId = 1;
    auto end = start;
    end.kind = RecordKind::TaskEnd;
    end.functionType = 0x17;
    end.times[0] = 20000;
    auto other = start;
    other.missionId = 2;
    other.times[0] = 12000;
    auto otherEnd = end;
    otherEnd.missionId = 2;
    otherEnd.times[0] = 18000;
    auto info = start;
    info.kind = RecordKind::LegacyTaskInfo;
    info.kernelType = 0;
    auto ccu = Make(RecordKind::CcuTask, 0, 0);
    auto mission = Make(RecordKind::CcuMission, 0, 30000, 40000);
    Stage({start, other, end, otherEnd, info, ccu, mission});
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Compute Duration(us)"), 10);
}
TEST_F(CommunicationReportTest, InvalidClocksProduceNaAndNoDurationEvents)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 0, 20000);
    Stage(rows, false);
    std::vector<std::string> issues;
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, &issues));
    EXPECT_TRUE(issues.empty());
    EXPECT_EQ(Summary()[0].get<std::string>("Compute Duration(us)"), "NA");
    EXPECT_EQ(Summary()[0].get<std::string>("Communication Duration(us)"), "NA");
    for (const auto& entry : Trace())
        EXPECT_NE(entry.second.get<std::string>("ph", ""), "X");
    EXPECT_EQ(Events("thread_name").size(), 3); // Fusion track metadata.
}
TEST_F(CommunicationReportTest, MissingManifestFailsWithoutInventingDevice)
{
    EXPECT_FALSE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    EXPECT_TRUE(Summary().empty());
    EXPECT_TRUE(Trace().empty());
}
TEST_F(CommunicationReportTest, FailedCollectionRetainsKnownDurationsAndNeverOverwrites)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 0, 20000);
    Stage(rows, true, false);
    EXPECT_FALSE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Compute Duration(us)"), 10);
    EXPECT_FALSE(Summary()[0].get_optional<std::string>("Status"));
    EXPECT_FALSE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Compute Duration(us)"), 10);
}
TEST_F(CommunicationReportTest, UnsupportedManifestIsRejected)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 0, 20000);
    Stage(rows);
    const auto manifestPath = directory / ".communication-staging/source/manifest.json";
    comm::Tree manifest;
    boost::property_tree::read_json(manifestPath.string(), manifest);
    manifest.put("schema", 99);
    std::ofstream(manifestPath.string()) << comm::Json(manifest);
    EXPECT_FALSE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    EXPECT_TRUE(Summary().empty());
    EXPECT_TRUE(Trace().empty());
}
TEST_F(CommunicationReportTest, SymlinkFragmentIsRejectedWithoutReadingTarget)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 0, 20000);
    Stage(rows);
    const auto fragment = directory / ".communication-staging/source/records-0.jsonl";
    const auto outside = directory / "outside.jsonl";
    fs::rename(fragment, outside);
    fs::create_symlink(outside, fragment);
    EXPECT_FALSE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    ASSERT_EQ(Summary().size(), 1);
    EXPECT_EQ(Summary()[0].get<std::string>("Compute Duration(us)"), "NA");
    EXPECT_TRUE(Trace().empty());
}
TEST_F(CommunicationReportTest, UpstreamFailureRetainsReportsWithoutDuplicatingIssue)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 0, 20000);
    Stage(rows);
    std::vector<std::string> issues{"application failed: \"example\"\nline"};
    EXPECT_FALSE(npucompute::cli::FinalizeCommunication(directory, &issues));
    ASSERT_EQ(issues.size(), 1);
    EXPECT_FALSE(Summary()[0].get_optional<std::string>("Status"));
    EXPECT_TRUE(Events("communication_diagnostic").empty());
}
TEST_F(CommunicationReportTest, AicpuTaskLogJoinsFullTaskNumber)
{
    std::vector<Record> rows;
    constexpr uint32_t fullTask = 0x80000011U;
    AddTask(rows, 0, fullTask, 53, RecordKind::LegacyTaskInfo, 1000, 2000);
    AddTask(rows, 0, 0x90000011U, 64, RecordKind::HcclTask, 1500, 3000);
    AddTask(rows, 0, 0x80000012U, 53, RecordKind::LegacyTaskInfo, 1200, 1800);
    fs::remove(directory / ".communication-staging/source");
    Stage(rows, true, true, "process-uds");
    WriteHostTrack(0, 53, fullTask, 1);
    WriteHostTrack(0, 64, 0x90000011U, 2);
    WriteHostTrack(0, 53, 0x80000012U, 1);
    WriteHostTrack(0, 54, 0x80000012U, 1);
    WriteHostRaw(17, "1:ExampleComputeKernel\n2:SameLowBitsButDifferentTask\n");
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto tasks = Events("ExampleComputeKernel", fullTask);
    ASSERT_EQ(tasks.size(), 1);
    EXPECT_EQ(tasks[0].get<uint32_t>("args.TaskId"), fullTask);
    EXPECT_EQ(TrackFor(tasks[0]), "Stream53");
    const auto ambiguous = Events("AiCpu-Comp-Launch", 0x80000012U);
    ASSERT_EQ(ambiguous.size(), 1);
    EXPECT_EQ(TrackFor(ambiguous.front()), "Stream unresolved");
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Overlap Duration(us)"), 0.5);
}

TEST_F(CommunicationReportTest, AivBlockMarksShareLogicalTracksAndOnlyPredefinedMarksExpandCommunication)
{
    constexpr uint32_t task = 0x80000011U;
    std::vector<Record> rows;
    AddTask(rows, 0, task, 53, RecordKind::LegacyTaskInfo, 1000, 11000);
    auto block = Make(RecordKind::BlockStart, 0, 1200);
    block.taskId = task;
    block.blockId = 3;
    const auto addBlock = [&](uint32_t coreType, uint32_t subBlockId, uint32_t coreId) {
        block.kind = RecordKind::BlockStart;
        block.times[Record::kTaskSystemCounter] = 1200;
        block.coreType = coreType;
        block.subBlockId = subBlockId;
        block.coreId = coreId;
        rows.push_back(block);
        block.kind = RecordKind::BlockEnd;
        block.times[Record::kTaskSystemCounter] = 10000;
        rows.push_back(block);
    };
    addBlock(ACLPTI_CORE_TYPE_AIC, 0, 3);
    addBlock(ACLPTI_CORE_TYPE_AIV, 0, 36);
    addBlock(ACLPTI_CORE_TYPE_AIV, 1, 37);
    auto mark = Make(RecordKind::AicoreTimestamp, 64, 1500);
    mark.blockId = 7;
    mark.coreId = 37;
    const auto addMark = [&](uint32_t descId, uint64_t counter) {
        mark.descId = descId;
        mark.times[Record::kTaskSystemCounter] = counter;
        rows.push_back(mark);
    };
    addMark(0x1011, 6000);
    addMark(0x1010, 3000);
    addMark(65577, 2000);
    addMark(65577, 9000);
    fs::remove(directory / ".communication-staging/source");
    Stage(rows, true, true, "process-uds");
    WriteHostTrack(0, 53, task, 1);
    WriteHostRaw(17, "1:ExampleAivKernel\n");

    ASSERT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    const auto compute = Events("ExampleAivKernel", task);
    ASSERT_EQ(compute.size(), 1);
    EXPECT_EQ(TrackFor(compute.front()), "Compute");
    EXPECT_EQ(compute.front().get<uint32_t>("args.StreamId"), 53);
    const auto cube = Events("cube time", task);
    ASSERT_EQ(cube.size(), 1);
    EXPECT_EQ(TrackFor(cube.front()), "block3-cube0");
    for (const auto* name : {"COMMIT", "65577"}) {
        const auto events = Events(name, task);
        ASSERT_EQ(events.size(), 1);
        EXPECT_EQ(TrackFor(events.front()), "block3-vector1");
        EXPECT_EQ(events.front().get<unsigned>("args.Physical Core Id"), 37);
    }
    const auto summary = Summary();
    ASSERT_EQ(summary.size(), 1);
    EXPECT_DOUBLE_EQ(summary.front().get<double>("Compute Duration(us)"), 10);
    EXPECT_DOUBLE_EQ(summary.front().get<double>("Communication Duration(us)"), 3);
    EXPECT_DOUBLE_EQ(summary.front().get<double>("Overlap Duration(us)"), 3);
}

TEST_F(CommunicationReportTest, AivAndFusionProcessesSortTogetherByDevice)
{
    std::vector<Record> rows;
    for (const auto device : {5, 4}) {
        AddTask(rows, device, 17, 4, RecordKind::LegacyTaskInfo, 1000, 6000);
        auto start = Make(RecordKind::BlockStart, device, 1200);
        start.coreType = ACLPTI_CORE_TYPE_AIV;
        auto end = start;
        end.kind = RecordKind::BlockEnd;
        end.times[Record::kTaskSystemCounter] = 5800;
        auto commitStart = Make(RecordKind::AicoreTimestamp, device, 2000);
        commitStart.descId = 0x1010;
        auto commitEnd = commitStart;
        commitEnd.descId = 0x1011;
        commitEnd.times[Record::kTaskSystemCounter] = 4000;
        rows.insert(rows.end(), {start, end, commitStart, commitEnd});
    }
    Stage(rows);
    ASSERT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));

    const auto processes = Processes();
    std::map<int64_t, std::string> sorted;
    for (const auto& event : Events("process_sort_index")) {
        const auto index = event.get<int64_t>("args.sort_index");
        ASSERT_TRUE(sorted.emplace(index, processes.at(event.get<int64_t>("pid"))).second);
    }
    std::vector<std::string> names;
    for (const auto& entry : sorted) {
        names.push_back(entry.second);
    }
    const std::vector<std::string> expected = {
        "AIV-Npu4", "Computation-Communication-Fusion-Npu4", "AIV-Npu5", "Computation-Communication-Fusion-Npu5"};
    EXPECT_EQ(names, expected);
}

TEST_F(CommunicationReportTest, TraceKeepsNumericIdentitiesRawArrayMetadataAndNanosecondFractions)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 123, 20123);
    Stage(rows);
    ASSERT_TRUE(npucompute::cli::FinalizeCommunication(directory, nullptr));
    std::ifstream input((directory / "CommunicationTrace.json").string());
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ASSERT_GE(json.size(), 3U);
    EXPECT_EQ(json.front(), '[');
    EXPECT_EQ(json.substr(json.size() - 3), "\n]\n");
    const auto events = boost::json::parse(json).as_array();
    for (const auto& event : events) {
        EXPECT_TRUE(event.as_object().at("pid").is_int64() || event.as_object().at("pid").is_uint64());
        EXPECT_TRUE(event.as_object().at("tid").is_int64() || event.as_object().at("tid").is_uint64());
    }
    EXPECT_NE(
        json.find(R"({"ph":"M","name":"process_name","pid":1,"tid":0,"args":{"name":"AiCpu-Compute-Npu0"}})"),
        std::string::npos);
    EXPECT_NE(json.find(R"("ts":0.123,"dur":10)"), std::string::npos);
    struct stat status {};
    ASSERT_EQ(stat((directory / "CommunicationTrace.json").c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
}
TEST_F(CommunicationReportTest, SharedOutputNeverReplacesATargetPublishedAfterCreation)
{
    const auto target = directory / "late-target.json";
    {
        npucompute::TraceFileOutput output(target);
        output.Write("new contents");
        output.Close();
        std::ofstream(target.string()) << "existing contents";
        EXPECT_THROW(output.Publish(), std::runtime_error);
    }
    std::ifstream input(target.string());
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, "existing contents");
    for (const auto& entry : fs::directory_iterator(directory)) {
        EXPECT_NE(entry.path().filename().string().find(".communication-output-"), 0);
    }
}

TEST_F(CommunicationReportTest, InvalidRecordDoesNotHideValidIntervalsOrOtherDevice)
{
    std::vector<Record> rows;
    AddDevice(rows, 0, 1000, 20000);
    AddDevice(rows, 1, 1000, 20000);
    AddTask(rows, 0, 18, 4, RecordKind::LegacyTaskInfo, 5000, 4000);
    Stage(rows);
    std::vector<std::string> issues;
    testing::internal::CaptureStderr();
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, &issues));
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("unmatched or duplicate TaskLog for Task18"), std::string::npos);
    EXPECT_TRUE(issues.empty());
    const auto summary = Summary();
    ASSERT_EQ(summary.size(), 2);
    for (const auto& row : summary) {
        EXPECT_FALSE(row.get_optional<std::string>("Status"));
        EXPECT_DOUBLE_EQ(row.get<double>("Compute Duration(us)"), 10);
        EXPECT_DOUBLE_EQ(row.get<double>("Communication Duration(us)"), 10);
    }
}

TEST_F(CommunicationReportTest, CcuMissionWithoutStreamMetadataRetainsKnownDuration)
{
    auto mission = Make(RecordKind::CcuMission, 0, 10000, 20000);
    mission.streamId = UINT32_MAX;
    Stage({mission});
    std::vector<std::string> issues;
    EXPECT_TRUE(npucompute::cli::FinalizeCommunication(directory, &issues));
    EXPECT_TRUE(issues.empty());
    EXPECT_FALSE(Events("LoopGroup").empty());
    ASSERT_EQ(Summary().size(), 1);
    EXPECT_DOUBLE_EQ(Summary()[0].get<double>("Communication Duration(us)"), 10);
    EXPECT_EQ(Summary()[0].get<std::string>("Compute Duration(us)"), "NA");
}

} // namespace
