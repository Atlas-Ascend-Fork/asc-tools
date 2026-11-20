/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime/npu_compute_runtime.h"
#include "artifact_sink.h"
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
namespace {
aclptiProfilingDataCallback callback;
bool invalidFrequency = false;
void Check(bool value, const char* reason)
{
    if (!value) {
        throw std::runtime_error(reason);
    }
}
class Sink final : public npucompute::ArtifactSink {
public:
    std::map<std::string, std::string> files;
    std::string current;
    std::string failure;
    std::set<std::string> committed;
    std::map<std::string, uint64_t> records;
    unsigned injectedFailures = 0;
    unsigned aborts = 0;
    void Begin(const std::string& name) override
    {
        current = name;
        files[name] = {};
    }
    void Write(std::string_view bytes) override
    {
        const std::string csvPrefix = "csv-failure-";
        if (failure.rfind(csvPrefix, 0) == 0 && current == failure.substr(csvPrefix.size()) + ".csv") {
            ++injectedFailures;
            throw std::runtime_error("section CSV failure");
        }
        if (failure == "sink-write" && current.find("Occupancy.json") != std::string::npos) {
            throw std::runtime_error("write failure");
        }
        files[current] += bytes;
    }
    void Commit(uint64_t count) override
    {
        if (failure == "sink-commit" && current.find("Occupancy.json") != std::string::npos) {
            throw std::runtime_error("commit failure");
        }
        committed.insert(current);
        records[current] = count;
        current.clear();
    }
    void Abort() noexcept override
    {
        ++aborts;
        files.erase(current);
        current.clear();
    }
};
std::shared_ptr<aclptiProfilingDataResult> Capture(const std::string& mode)
{
    auto result = std::make_shared<aclptiProfilingDataResult>();
    auto& replay = result->replayData[7];
    replay.deviceId = 0;
    replay.taskLogs = {
        {7, 0, 11, 13, 1000, 0, 0, ACLPTI_CORE_TYPE_AIC, 0}, {7, 1, 11, 13, 2000, 0, 0, ACLPTI_CORE_TYPE_AIC, 0}};
    replay.blockLogs = {
        {7, 0x24, 11, 13, 1100, 2, 0, ACLPTI_CORE_TYPE_AIC, 0}, {7, 0x25, 11, 13, 1700, 2, 0, ACLPTI_CORE_TYPE_AIC, 0}};
    replay.pmuRecords.push_back({0x29, 11, 13, ACLPTI_CORE_TYPE_AIC, 5, 2, 0, 1000, 2000, 600, false});
    if (mode.rfind("csv-failure-", 0) == 0) {
        aclptiPmuDataRow row{};
        row.coreType = ACLPTI_CORE_TYPE_AIC;
        row.totalCycles = 600;
        result->pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIC, 0}, row);
    }
    if (mode == "failed-replay") {
        replay.status = ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (mode == "malformed") {
        replay.blockLogs.pop_back();
    }
    if (mode.rfind("section-failure-", 0) == 0) {
        result->status = ACLPTI_ERROR_PROFILING_FAILED;
        const auto section = mode.substr(std::string("section-failure-").size());
        if (section == "Pipeline") {
            auto& pipeline = result->pipelineData[8];
            pipeline.deviceId = 0;
            pipeline.format = aclptiBiuFormat::Chip6;
            pipeline.status = ACLPTI_ERROR_PROFILING_FAILED;
        } else if (section == "PmSampling") {
            result->pcSamplingData.push_back({8, 0, 0, 0, true, {1}});
        } else {
            auto& failed = result->replayData[8];
            failed.deviceId = 0;
            failed.status = ACLPTI_ERROR_PROFILING_FAILED;
            failed.stats.failedRecordCount = 1;
            result->errorStats.failedRecordCount = 1;
            result->errorStats.failedRecordCountByReplay[8] = 1;
        }
    }
    if (mode == "invalid-merged-pmu") {
        aclptiPmuDataRow row{};
        row.blockId = 99;
        result->pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIC, 0}, row);
    }
    return result;
}

std::vector<std::string> SectionsFor(const std::string& mode)
{
    if (mode == "pipeline-only") {
        return {"Pipeline"};
    }
    if (mode == "pm-sampling-only") {
        return {"PmSampling"};
    }
    if (mode == "non-pmu-combo") {
        return {"Pipeline", "PmSampling"};
    }
    if (mode.rfind("csv-failure-", 0) == 0) {
        return {mode.substr(std::string("csv-failure-").size())};
    }
    if (mode.rfind("section-failure-", 0) == 0) {
        return {"PipeUtilization",
                "Memory",
                "MemoryL0",
                "MemoryUB",
                "L2Cache",
                "Pipeline",
                "ArithmeticUtilization",
                "ResourceConflictRatio",
                "PmSampling"};
    }
    return {"PipeUtilization"};
}

bool OccupancyMustSurvive(const std::string& mode)
{
    return mode == "success" || mode == "pipeline-only" || mode == "pm-sampling-only" || mode == "non-pmu-combo" ||
           mode == "invalid-merged-pmu" || mode.rfind("section-failure-", 0) == 0 || mode.rfind("csv-failure-", 0) == 0;
}

void CheckOccupancy(const Sink& sink, const std::string& name, uint64_t launch)
{
    Check(sink.committed.count(name) == 1, "occupancy committed");
    Check(sink.records.at(name) == 2, "task and block record count");
    boost::property_tree::ptree root;
    std::istringstream input(sink.files.at(name));
    boost::property_tree::read_json(input, root);
    Check(root.get<std::string>("status") == "complete", "occupancy JSON complete");
    unsigned tasks = 0;
    unsigned blocks = 0;
    for (const auto& entry : root.get_child("traceEvents")) {
        const auto& event = entry.second;
        if (event.get<std::string>("ph") != "X") {
            continue;
        }
        Check(event.get<uint64_t>("args.replayId") == 7, "first replay only");
        Check(event.get<uint64_t>("args.launchSequence") == launch, "launch identity");
        if (event.get<std::string>("cat") == "block") {
            ++blocks;
            Check(event.get<std::string>("name") == "AIC BLOCK2", "original block ID label");
            Check(event.get<uint64_t>("args.blockStartCounter") == 1000, "PMU block start");
            Check(event.get<uint64_t>("args.blockEndCounter") == 2000, "PMU block end");
            Check(event.get<uint64_t>("args.pmuTotalCycles") == 600, "PMU total cycles");
        } else {
            ++tasks;
        }
    }
    Check(tasks == 1 && blocks == 1, "exact intervals without later replay duplicates");
}
} // namespace
extern "C" aclError aclrtGetDeviceCount(std::uint32_t* count)
{
    *count = 0;
    return ACL_SUCCESS;
}
extern "C" const char* aclrtGetSocName() { return "Ascend950PR_9599"; }
extern "C" int halGetDeviceInfo(std::uint32_t, std::int32_t, std::int32_t, std::int64_t* value)
{
    *value = invalidFrequency ? 0 : 1000000;
    return 0;
}
aclptiResult aclptiRegisterProfilingDataCallback(aclptiProfilingDataCallback value)
{
    callback = std::move(value);
    return ACLPTI_SUCCESS;
}
int main(int argc, char** argv)
{
    try {
        Check(argc == 2, "missing scenario");
        const std::string mode = argv[1];
        invalidFrequency = mode == "invalid-frequency";
        Sink sink;
        sink.failure = mode;
        npucompute::RuntimeConfig config;
        config.sections = SectionsFor(mode);
        auto& runtime = npucompute::NpuComputeRuntime::Instance();
        int finalStatus = 99;
        Check(
            runtime.Initialize(
                config, sink,
                [&finalStatus](int status) {
                    finalStatus = status;
                    return status;
                }) == 0,
            "initialize");
        if (mode != "no-callback") {
            Check(callback(Capture(mode)) == ACLPTI_SUCCESS, "submit result");
            if (mode == "success") {
                Check(callback(Capture(mode)) == ACLPTI_SUCCESS, "submit second result");
            }
        }
        runtime.Stop();
        const int status = runtime.ShutdownAfterPtiDrain();
        const bool occupancyOnlyFailure = mode == "invalid-frequency" || mode == "malformed" ||
                                          mode == "failed-replay" || mode == "sink-write" || mode == "sink-commit";
        const bool expectedComplete =
            mode == "success" || mode == "pm-sampling-only" || mode == "no-callback" || occupancyOnlyFailure;
        Check((status == 0) == expectedComplete, "completion status");
        Check(finalStatus == status, "final callback status");
        const bool noDataError = mode == "success" || mode == "pipeline-only" || mode == "pm-sampling-only" ||
                                 mode == "non-pmu-combo" || mode == "invalid-merged-pmu" || mode == "no-callback" ||
                                 occupancyOnlyFailure || mode.rfind("csv-failure-", 0) == 0;
        Check((runtime.ErrorCount() == 0) == noDataError, "error count");
        if (occupancyOnlyFailure) {
            Check(sink.committed.count("summary.jsonl") == 1, "summary is delivered despite occupancy failure");
        }
        if (mode == "success") {
            Check(sink.files.count("Occupancy.json") == 1, "first Occupancy artifact");
            const auto prefix = "collection-p" + std::to_string(getpid()) + "-0001/";
            Check(sink.files.count(prefix + "Occupancy.json") == 1, "additional launch artifact prefix");
            Check(
                sink.files.at("Occupancy.json").find("physical core unverified") != std::string::npos,
                "logical labels");
            Check(
                sink.files.at(prefix + "Occupancy.json").find("\"launchSequence\":\"1\"") != std::string::npos,
                "launch identity");
            Check(runtime.ReplayCount() == 2, "snapshot replay count");
            CheckOccupancy(sink, "Occupancy.json", 0);
            CheckOccupancy(sink, prefix + "Occupancy.json", 1);
        } else if (OccupancyMustSurvive(mode)) {
            CheckOccupancy(sink, "Occupancy.json", 0);
        } else {
            Check(sink.files.count("Occupancy.json") == 0, "failed occupancy must not publish success artifact");
        }
        if (mode == "sink-write" || mode == "sink-commit") {
            Check(sink.aborts == 1, "owned artifact cleanup");
        }
        if (mode.rfind("csv-failure-", 0) == 0) {
            Check(sink.injectedFailures == 1, "CSV failure injection reached");
        }
        std::cout << "PASS: " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
