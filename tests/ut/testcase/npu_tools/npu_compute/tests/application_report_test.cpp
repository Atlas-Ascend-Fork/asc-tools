/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/application_report.h"
#include <boost/property_tree/json_parser.hpp>
#include <gtest/gtest.h>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
class MemorySink final : public npucompute::ArtifactSink {
public:
    void Begin(const std::string& name) override
    {
        current = name;
        events.push_back("begin:" + name);
    }
    void Write(std::string_view bytes) override
    {
        if (current == failWrite) {
            throw std::runtime_error("write failed");
        }
        files[current].append(bytes);
    }
    void Commit(uint64_t records) override
    {
        counts[current] = records;
        events.push_back("commit:" + current);
        current.clear();
    }
    void Abort() noexcept override
    {
        ++aborts;
        current.clear();
    }
    std::string current;
    std::string failWrite;
    unsigned aborts = 0;
    std::vector<std::string> events;
    std::map<std::string, std::string> files;
    std::map<std::string, uint64_t> counts;
};

TEST(ApplicationReport, PreservesRawBytesDeviceIdentityAndEpochWithoutDecoding)
{
    MemorySink sink;
    npucompute::ApplicationReport report(sink);
    aclpti::profiling::ApplicationRawRecord record;
    record.deviceId = 1;
    record.deviceEpoch = 2;
    record.type = 8;
    record.chunkModule = 4;
    record.offset = 256;
    record.isLastChunk = true;
    record.bytes = {0, 0x80, 0xff};
    report.Consume(record);
    aclpti::profiling::ApplicationSummary summary;
    summary.receivedRecords = summary.processedRecords = 1;
    report.Finish(summary);
    const auto& raw = sink.files.at("application_raw.jsonl");
    std::istringstream input(raw.substr(raw.find('\n') + 1));
    boost::property_tree::ptree row;
    boost::property_tree::read_json(input, row);
    EXPECT_EQ(row.get<uint64_t>("passId"), 0U);
    EXPECT_EQ(row.get<int>("deviceId"), 1);
    EXPECT_EQ(row.get<uint64_t>("deviceEpoch"), 2U);
    EXPECT_EQ(row.get<uint64_t>("offset"), 256U);
    EXPECT_EQ(row.get<std::string>("payloadHex"), "0080ff");
    EXPECT_EQ(sink.counts.at("application_raw.jsonl"), 2U);
}

npucompute::ApplicationOpInfo Op(int device, uint64_t instance)
{
    npucompute::ApplicationOpInfo op;
    op.metadata.name = "comm";
    op.metadata.deviceId = device;
    op.metadata.blockDim = 2;
    op.metadata.kind = npucompute::KernelKind::Cube;
    op.metadata.hardware.collected = true;
    op.metadata.hardware.socName = "Ascend950PR_9599";
    op.metadata.hardware.cubeCoreCount = 32;
    op.metadata.hardware.currentAicFrequencyMhz = 1000;
    op.metadata.hardware.ratedAicFrequencyMhz = 1000;
    op.deviceEpoch = 1;
    op.launchIndex = instance;
    op.instanceIndex = instance;
    op.taskId = 0x12345678;
    op.streamId = 3;
    return op;
}

aclpti::profiling::ApplicationRawRecord TaskLogRecord(int device, uint64_t epoch)
{
    using namespace aclpti::data;
    aclpti::profiling::ApplicationRawRecord record;
    record.deviceId = device;
    record.deviceEpoch = epoch;
    TaskLog32 log{};
    log.taskId = 0x1234;
    log.rtStreamId = 0x5678;
    log.funcType = kTaskStartFunctionType;
    log.systemCounter = 1000;
    record.decoded.push_back({0, log});
    log.funcType = kTaskEndFunctionType;
    log.systemCounter = 11000;
    record.decoded.push_back({1, log});
    return record;
}

void ConsumeTask(npucompute::ApplicationReport& report, int device, uint64_t epoch, double reads)
{
    using namespace aclpti::data;
    auto record = TaskLogRecord(device, epoch);
    PmuRecord128 pmu{};
    pmu.taskId = 0x1234;
    pmu.rtStreamId = 0x5678;
    pmu.funcType = kTaskPmuFunctionType;
    pmu.totalCycles = 10000;
    pmu.pmuValues = {{808, 5000}, {0x422, reads}, {0x423, reads / 2}};
    record.decoded.push_back({2, pmu});
    for (uint16_t block : {0, 1}) {
        pmu.funcType = kBlockPmuFunctionType;
        pmu.blockId = block;
        pmu.totalCycles = 5000;
        record.decoded.push_back({static_cast<uint64_t>(3 + block), pmu});
    }
    report.Consume(record);
}

TEST(ApplicationReport, FrequencyRequiresTaskPmuForEachKernelSide)
{
    MemorySink sink;
    npucompute::ApplicationReport report(sink);
    report.Consume(TaskLogRecord(0, 1));
    ConsumeTask(report, 1, 1, 100);
    npucompute::ApplicationOpSnapshot snapshot;
    snapshot.ops = {Op(0, 0), Op(1, 1)};
    for (auto& op : snapshot.ops) {
        op.launchIndex = 0;
        op.metadata.kind = npucompute::KernelKind::Mix;
        op.metadata.mixBlockDim = 4;
        op.metadata.hardware.currentAivFrequencyMhz = 1000;
        op.metadata.hardware.ratedAivFrequencyMhz = 1000;
    }
    aclpti::profiling::ApplicationSummary summary;
    summary.complete = summary.tailConfirmed = true;
    report.Finish(summary, snapshot);
    for (int device : {0, 1}) {
        SCOPED_TRACE(device);
        std::istringstream input(sink.files.at("device" + std::to_string(device) + "/op-comm/0/summary.jsonl"));
        boost::property_tree::ptree row;
        boost::property_tree::read_json(input, row);
        EXPECT_EQ(row.get<std::string>("Op Type"), "mix");
        EXPECT_EQ(row.get<unsigned>("Mix Block Dim"), 4U);
        EXPECT_DOUBLE_EQ(row.get<double>("Task Duration(us)"), 10);
        EXPECT_EQ(row.get<std::string>("Current Freq"), "NA");
        EXPECT_EQ(row.get<std::string>("Rated Freq"), "NA");
    }
}

TEST(ApplicationReport, FullTaskSnOnDifferentDevicesKeepsIndependentPmuValues)
{
    MemorySink sink;
    npucompute::ApplicationReport report(sink);
    ConsumeTask(report, 0, 1, 100);
    ConsumeTask(report, 1, 1, 200);
    npucompute::ApplicationOpSnapshot snapshot;
    snapshot.ops = {Op(0, 0), Op(1, 1)};
    aclpti::profiling::ApplicationSummary summary;
    summary.complete = summary.tailConfirmed = true;
    report.Finish(summary, snapshot);
    std::vector<double> bandwidths;
    for (const auto& [name, contents] : sink.files) {
        if (name.find("/summary.jsonl") == std::string::npos) {
            continue;
        }
        std::istringstream input(contents);
        boost::property_tree::ptree row;
        boost::property_tree::read_json(input, row);
        EXPECT_DOUBLE_EQ(row.get<double>("Task Duration(us)"), 10);
        EXPECT_DOUBLE_EQ(row.get<double>("Current Freq"), 1000);
        EXPECT_DOUBLE_EQ(row.get<double>("Rated Freq"), 1000);
        EXPECT_GT(row.get<double>("aic_flops"), 0);
        EXPECT_GT(row.get<double>("aicore_parallel_utilization"), 0);
        bandwidths.push_back(row.get<double>("aicore_gm_read_bw(GB/s)"));
    }
    ASSERT_EQ(bandwidths.size(), 2U);
    EXPECT_GT(bandwidths[0], 0);
    EXPECT_DOUBLE_EQ(bandwidths[1], bandwidths[0] * 2);
}

TEST(ApplicationReport, FinalizedDeviceDoesNotInventSuccessfulStop)
{
    using namespace aclpti::profiling;
    MemorySink sink;
    npucompute::ApplicationReport report(sink);
    ApplicationSummary summary;
    summary.complete = true;
    summary.tailConfirmed = true;
    summary.endReason = ApplicationEndReason::AclFinalize;
    auto& device = summary.devices[0];
    device.startAttempted = true;
    device.deviceEpoch = 1;
    device.endReason = ApplicationDeviceEndReason::RuntimeFinalize;
    report.Finish(summary);
    std::istringstream input(sink.files.at("application_manifest.json"));
    boost::property_tree::ptree manifest;
    boost::property_tree::read_json(input, manifest);
    EXPECT_TRUE(manifest.get<bool>("complete"));
    EXPECT_EQ(manifest.get<std::string>("endReason"), "acl_finalize");
    EXPECT_EQ(manifest.get<std::string>("failureReason"), "null");
    const auto& row = manifest.get_child("devices").front().second;
    EXPECT_FALSE(row.get<bool>("stopAttempted"));
    EXPECT_EQ(row.get<std::string>("stopStatus"), "null");
    EXPECT_EQ(row.get<std::string>("endReason"), "runtime_finalize");
}

TEST(ApplicationReport, AssociationReasonsKeepTheirPriority)
{
    MemorySink sink;
    npucompute::ApplicationReport report(sink);
    ConsumeTask(report, 0, 1, 100);
    ConsumeTask(report, 1, 0, 200);
    npucompute::ApplicationOpSnapshot snapshot;
    snapshot.ops = {Op(0, 0), Op(0, 1), Op(0, 2), Op(0, 3), Op(0, 4), Op(0, 5), Op(1, 6)};
    snapshot.ops[0].deviceEpoch = 0;
    snapshot.ops[0].taskId.reset();
    snapshot.ops[1].taskId.reset();
    snapshot.ops[2].streamId.reset();
    snapshot.ops[4].streamId = 70000;
    snapshot.ops[5].taskId = 0x87654321;
    report.Finish({}, snapshot);
    const std::vector<std::string> reasons = {
        "device_collection_epoch_unavailable",
        "runtime_task_identity_unavailable",
        "runtime_task_identity_unavailable",
        "task_identity_reused",
        "task_identity_reused",
        "task_data_not_received_for_device_epoch",
        "task_data_not_received_for_device_epoch"};
    for (size_t index = 0; index < reasons.size(); ++index) {
        SCOPED_TRACE(index);
        const auto& op = snapshot.ops[index];
        std::istringstream input(sink.files.at(
            "device" + std::to_string(*op.metadata.deviceId) + "/op-comm/" + std::to_string(op.launchIndex) +
            "/summary.jsonl"));
        boost::property_tree::ptree row;
        boost::property_tree::read_json(input, row);
        EXPECT_EQ(row.get<std::string>("Task Duration(us)"), "NA");
        EXPECT_EQ(row.get<std::string>("aic_flops"), "NA");
        EXPECT_EQ(row.get<std::string>("collection_status"), "partial");
        const auto& unavailable = row.get_child("unavailable_reasons");
        EXPECT_EQ(unavailable.get<std::string>("Current Freq"), reasons[index]);
        EXPECT_EQ(unavailable.get<std::string>("aic_flops"), reasons[index]);
    }
}

TEST(ApplicationReport, DuplicateTaskOrBlockPmuDiscardsPmuButKeepsTaskLogs)
{
    using namespace aclpti::data;
    MemorySink sink;
    npucompute::ApplicationReport report(sink);
    for (int device : {0, 1}) {
        ConsumeTask(report, device, 1, 100);
        aclpti::profiling::ApplicationRawRecord duplicate;
        duplicate.deviceId = device;
        duplicate.deviceEpoch = 1;
        PmuRecord128 pmu{};
        pmu.taskId = 0x1234;
        pmu.rtStreamId = 0x5678;
        pmu.funcType = device == 0 ? kTaskPmuFunctionType : kBlockPmuFunctionType;
        duplicate.decoded.push_back({0, pmu});
        report.Consume(duplicate);
    }
    npucompute::ApplicationOpSnapshot snapshot;
    snapshot.ops = {Op(0, 0), Op(1, 1)};
    report.Finish({}, snapshot);
    for (int device : {0, 1}) {
        SCOPED_TRACE(device);
        std::istringstream input(
            sink.files.at("device" + std::to_string(device) + "/op-comm/" + std::to_string(device) + "/summary.jsonl"));
        boost::property_tree::ptree row;
        boost::property_tree::read_json(input, row);
        EXPECT_DOUBLE_EQ(row.get<double>("Task Duration(us)"), 10);
        EXPECT_EQ(row.get<std::string>("aic_flops"), "NA");
        EXPECT_EQ(row.get<std::string>("Current Freq"), "NA");
        EXPECT_EQ(row.get_child("unavailable_reasons").get<std::string>("aic_flops"), "duplicate_pmu_records");
    }
}

TEST(ApplicationReport, RequiredArtifactsCommitInOrderAndFailedSummaryAborts)
{
    npucompute::ApplicationOpSnapshot snapshot;
    snapshot.hardwareJsonl = "{\"category\":\"Host Info\"}\n";
    snapshot.ops = {Op(0, 0)};
    for (bool fail : {false, true}) {
        SCOPED_TRACE(fail);
        MemorySink sink;
        npucompute::ApplicationReport report(sink);
        if (fail) {
            sink.failWrite = "device0/op-comm/0/summary.jsonl";
            EXPECT_THROW(report.Finish({}, snapshot), std::runtime_error);
            EXPECT_EQ(sink.aborts, 2U);
        } else {
            report.Finish({}, snapshot);
            EXPECT_EQ(sink.aborts, 0U);
        }
        std::vector<std::string> expected = {
            "begin:application_raw.jsonl", "commit:application_raw.jsonl", "begin:HardwareInfo.jsonl",
            "commit:HardwareInfo.jsonl", "begin:device0/op-comm/0/summary.jsonl"};
        if (!fail) {
            expected.insert(
                expected.end(), {"commit:device0/op-comm/0/summary.jsonl", "begin:application_manifest.json",
                                 "commit:application_manifest.json"});
        }
        EXPECT_EQ(sink.events, expected);
    }
}

TEST(ApplicationReport, KeepsFieldOrderEscapesAndQuotingLocales)
{
    class GroupedNumbers final : public std::numpunct<char> {
        char do_thousands_sep() const override { return '_'; }
        std::string do_grouping() const override { return "\1"; }
    };
    struct LocaleGuard {
        std::locale previous = std::locale();
        ~LocaleGuard() { std::locale::global(previous); }
    } guard;
    std::locale::global(std::locale(guard.previous, new GroupedNumbers));
    MemorySink sink;
    npucompute::ApplicationReport report(sink);
    npucompute::ApplicationOpSnapshot snapshot;
    auto op = Op(0, 0);
    op.metadata.name = "comm\"\\\n\x01\x11";
    op.metadata.blockDim = 12345;
    snapshot.ops.push_back(op);
    report.Finish({}, snapshot);
    const auto& row = sink.files.at("device0/op-comm_22_5c_0a_01_11/0/summary.jsonl");
    EXPECT_EQ(
        row.find("{\"category\":\"OpInfoSummary\",\"Op Name\":\"comm\\\"\\\\\\u000a\\u0001\\u0011\","
                 "\"Op Type\":\"cube\""),
        0U);
    EXPECT_LT(row.find("\"aicore_gm_bw_usage_rate(%)\""), row.find("\"result_unit_id\""));
    EXPECT_LT(row.find("\"Stream Id\""), row.find("\"communication_time\""));
    EXPECT_LT(row.find("\"collection_status\""), row.find("\"unavailable_reasons\""));
    EXPECT_EQ(row.find('\n'), row.size() - 1);
    EXPECT_NE(row.find("\"Block Dim\":12345"), std::string::npos);
    EXPECT_NE(
        sink.files.at("application_manifest.json").find("\"op_name\":\"comm\\\"\\\\\\u000a\\u0001\\u01_1\""),
        std::string::npos);
}
} // namespace
