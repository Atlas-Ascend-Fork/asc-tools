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

#include "report/report_writer.h"
#include "artifact_sink.h"
#include "report/collection_file_validator.h"
#include "report/collection_processor.h"
#include "report/rep_decoder.h"
#include "import/imported_profile_results.h"

#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <unistd.h>

#define CHECK(condition)                                      \
    do {                                                      \
        if (!(condition)) {                                   \
            ADD_FAILURE_AT(__FILE__, __LINE__) << #condition; \
            return 1;                                         \
        }                                                     \
    } while (false)

std::vector<boost::property_tree::ptree> ReadSummary(const boost::filesystem::path& directory)
{
    std::ifstream input((directory / "summary.jsonl").string());
    std::vector<boost::property_tree::ptree> records;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        boost::property_tree::ptree record;
        boost::property_tree::read_json(stream, record);
        records.push_back(std::move(record));
    }
    return records;
}

static int RunSuiteMain()
{
    char directory[] = "/tmp/npu-summary-test-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    npucompute::ReportConfig config;
    config.outputDirectory = directory;
    config.frequencyMhz = 1000;
    config.aicFrequencyMhz = 1000;
    config.aivFrequencyMhz = 1000;
    aclptiProfilingDataResult result;
    aclptiPmuDataRow task{};
    task.coreType = ACLPTI_CORE_TYPE_AIV;
    task.totalCycles = 2000;
    task.values = {{0x422, 8}, {0x423, 4}};
    result.taskPmuData.aiv = aclptiTaskPmuCoreData{task.totalCycles, false, task.values};
    task.totalCycles = 9999;
    result.pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, task);
    CHECK(npucompute::WritePmuReport(result, {"Memory"}, config) == ACLPTI_SUCCESS);
    std::ifstream input(std::string(directory) + "/summary.jsonl");
    CHECK(input.is_open());
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(text.find("\"category\":\"Memory\"") == 1);
    CHECK(text.find("\"aiv_total_cycles\":2000") != std::string::npos);
    CHECK(text.find("9999") == std::string::npos);
    CHECK(text.find("block_id") == std::string::npos);
    CHECK(text.find("\"aic_time(us)\":\"NA\"") != std::string::npos);
    CHECK(text.find("\"category\":\"OpInfoSummary\"") > text.find("\"category\":\"Memory\""));
    CHECK(text.find("\"aic_flops\":\"NA\"") != std::string::npos);
    CHECK(text.back() == '\n');
    std::string error;
    CHECK(npucompute::cli::ValidateCollectionFile(
        std::string(directory) + "/summary.jsonl", npucompute::cli::NpuRepFileType::Jsonl, &error));
    auto records = ReadSummary(directory);
    CHECK(records.size() == 3);
    CHECK(std::abs(records[0].get<double>("aiv_main_mem_read_bw(GB/s)") - 0.476837) < 1e-6);
    CHECK(records.back().get<std::string>("Op Name") == "NA");
    CHECK(records.back().get<std::string>("aicore_parallel_utilization") == "NA");
    CHECK(records.back().size() == 20);

    std::vector<uint8_t> encoded;
    CHECK(npucompute::cli::PackDirectoryToRep(directory, &encoded, &error));
    npucompute::cli::DecodedRep decoded;
    CHECK(npucompute::cli::DecodeRep(encoded, &decoded, &error));
    std::vector<npucompute::cli::ImportedProfileEntry> imported;
    for (const auto& entry : decoded.entries) {
        imported.push_back({entry.file_name, entry.file_type, entry.payload, {}});
    }
    const auto importDirectory = boost::filesystem::path(directory) / "imported";
    boost::filesystem::create_directory(importDirectory);
    CHECK(npucompute::cli::UnpackImportedProfileResults(imported, importDirectory, &error));
    CHECK(ReadSummary(importDirectory)[0].get<double>("aiv_total_cycles") == 2000);

    config.outputDirectory = (boost::filesystem::path(directory) / "all").string();
    npucompute::KernelMetadata metadata;
    metadata.name = "kernel\"\\\n\t中文";
    metadata.blockDim = 64;
    metadata.deviceId = 0;
    metadata.ratedAivFrequencyMhz = 1800;
    const std::vector<std::string> sections = {"PipeUtilization", "Memory", "L2Cache", "MemoryUB", "MemoryL0"};
    result.taskPmuData.aiv->values[0x501] = 1000;
    CHECK(npucompute::WritePmuReport(result, sections, config, metadata) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records.size() == 7);
    auto sortedSections = sections;
    std::sort(sortedSections.begin(), sortedSections.end());
    for (std::size_t index = 0; index < sortedSections.size(); ++index) {
        CHECK(records[index].get<std::string>("category") == sortedSections[index]);
        CHECK(records[index].get<double>("aiv_total_cycles") == 2000);
    }
    CHECK(records.back().get<std::string>("category") == "OpInfoSummary");
    CHECK(records.back().get<std::string>("Op Name") == metadata.name);
    CHECK(records.back().get<uint64_t>("Block Dim") == 64);
    CHECK(records.back().get<double>("Rated Freq") == 1800);

    for (const auto& names : std::vector<std::pair<std::string, std::string>>{
             {"_Z9vectorAddPf", "vectorAdd(float*)"},
             {"vectorAdd(float*)", "vectorAdd(float*)"},
             {"_Zinvalid", "_Zinvalid"},
             {"", ""}}) {
        metadata.name = names.first;
        CHECK(npucompute::WriteSummaryJsonl(result, sections, config, metadata) == ACLPTI_SUCCESS);
        CHECK(ReadSummary(config.outputDirectory).back().get<std::string>("Op Name") == names.second);
    }

    // Repeated reports keep block CSV and task summary in a separate collection.
    CHECK(npucompute::WritePmuReport(result, sections, config, metadata) == ACLPTI_SUCCESS);
    bool foundCollection = false;
    for (const auto& entry : boost::filesystem::directory_iterator(config.outputDirectory)) {
        if (boost::filesystem::is_directory(entry.path())) {
            CHECK(ReadSummary(entry.path())[0].get<double>("aiv_total_cycles") == 2000);
            CHECK(boost::filesystem::exists(entry.path() / "Memory.csv"));
            foundCollection = true;
        }
    }
    CHECK(foundCollection);

    config.outputDirectory = (boost::filesystem::path(directory) / "missing").string();
    aclptiProfilingDataResult missing;
    missing.pmuLogs = result.pmuLogs;
    CHECK(npucompute::WritePmuReport(missing, {"Memory"}, config) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records.size() == 3);
    CHECK(records[0].get<std::string>("aiv_total_cycles") == "NA");
    CHECK(boost::filesystem::exists(boost::filesystem::path(config.outputDirectory) / "Memory.csv"));

    // Task-only data still produces a summary, but must not produce a CSV.
    config.outputDirectory = (boost::filesystem::path(directory) / "task-only").string();
    auto taskOnlyResult = result;
    taskOnlyResult.pmuLogs.clear();
    CHECK(npucompute::WritePmuReport(taskOnlyResult, {"Memory"}, config) == ACLPTI_SUCCESS);
    CHECK(
        ReadSummary(config.outputDirectory)[0].get<double>("aiv_total_cycles") == result.taskPmuData.aiv->totalCycles);
    CHECK(!boost::filesystem::exists(boost::filesystem::path(config.outputDirectory) / "Memory.csv"));

    config.outputDirectory = (boost::filesystem::path(directory) / "task-counters").string();
    result.taskPmuData.aiv->totalCycles = 3000;
    result.taskPmuData.aiv->values[0x501] = 1950;
    result.pmuLogs.clear();
    for (uint16_t block = 0; block < 2; ++block) {
        auto row = task;
        row.blockId = block;
        row.totalCycles = block == 0 ? 2000 : 4000;
        result.pmuLogs.emplace(aclptiBlockKey{block, 0, ACLPTI_CORE_TYPE_AIV, 0}, row);
    }
    npucompute::KernelMetadata averageMetadata;
    averageMetadata.kind = npucompute::KernelKind::Vector;
    averageMetadata.blockDim = 2;
    result.taskLogs[1] = {
        {0, 0, 1, 0, 100, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}, {0, 1, 1, 0, 10100, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}};
    CHECK(npucompute::WritePmuReport(result, {"PipeUtilization"}, config, averageMetadata) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records[0].get<double>("aiv_total_cycles") == 3000);
    CHECK(std::abs(records[0].get<double>("aiv_vec_ratio") - 0.65) < 1e-6);
    CHECK(std::abs(records.back().get<double>("aicore_parallel_utilization") - 30) < 1e-6);
    CHECK(std::abs(records.back().get<double>("aicore_parallel_balance") - 75) < 1e-6);

    config.outputDirectory = (boost::filesystem::path(directory) / "mixed").string();
    aclptiPmuDataRow cube{};
    cube.coreType = ACLPTI_CORE_TYPE_AIC;
    cube.totalCycles = 1000;
    cube.values = {{0x422, 16}, {0x423, 8}};
    result.taskPmuData.aic = aclptiTaskPmuCoreData{cube.totalCycles, false, cube.values};
    result.taskLogs[1] = {
        {0, 0, 1, 0, 100, 0, 0, ACLPTI_CORE_TYPE_AIC, 0}, {0, 1, 1, 0, 10100, 0, 0, ACLPTI_CORE_TYPE_AIC, 0}};
    config.aicFrequencyMhz = 1200;
    config.aivFrequencyMhz = 1000;
    CHECK(npucompute::WritePmuReport(result, {"Memory"}, config) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records.back().get<std::string>("Op Type") == "mix");
    CHECK(records.back().get<double>("Task Duration(us)") == 10);
    CHECK(records.back().get<std::string>("Current Freq") == "NA");
    CHECK(std::abs(records.back().get<double>("aicore_gm_read_bw(GB/s)") - 0.286102) < 1e-6);
    CHECK(std::abs(records.back().get<double>("aicore_gm_bw_usage_rate(%)") - 0.026822) < 1e-6);

    config.outputDirectory = (boost::filesystem::path(directory) / "zero").string();
    aclptiProfilingDataResult zero;
    cube.totalCycles = 0;
    cube.values = {{810, 0}};
    zero.taskPmuData.aic = aclptiTaskPmuCoreData{cube.totalCycles, false, cube.values};
    zero.pmuLogs.emplace(aclptiBlockKey{}, cube);
    CHECK(npucompute::WritePmuReport(zero, {"PipeUtilization"}, config) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records[0].get<std::string>("aic_cube_ratio") == "NA");
    CHECK(records[0].get<double>("aic_total_cycles") == 0);
    CHECK(records.back().get<std::string>("aicore_parallel_balance") == "NA");

    // Summary metrics use task counters independently of the block CSV.
    config.outputDirectory = (boost::filesystem::path(directory) / "arithmetic-conflict").string();
    aclptiProfilingDataResult arithmeticConflict;
    aclptiPmuDataRow arithmeticCube{};
    arithmeticCube.coreType = ACLPTI_CORE_TYPE_AIC;
    arithmeticCube.totalCycles = 1000;
    arithmeticCube.values = {{768, 99},  {789, 21}, {790, 32}, {808, 100}, {809, 200},
                             {810, 300}, {11, 10},  {13, 20},  {14, 30},   {15, 40}};
    arithmeticConflict.taskPmuData.aic =
        aclptiTaskPmuCoreData{arithmeticCube.totalCycles, false, arithmeticCube.values};
    aclptiPmuDataRow arithmeticVector{};
    arithmeticVector.coreType = ACLPTI_CORE_TYPE_AIV;
    arithmeticVector.totalCycles = 2000;
    arithmeticVector.values = {{1281, 1000}, {1282, 200}, {1283, 300}, {1284, 500}, {12, 100},  {14, 200}, {15, 300},
                               {1344, 400},  {1366, 500}, {1376, 10},  {1377, 20},  {1378, 30}, {1379, 40}};
    arithmeticConflict.taskPmuData.aiv =
        aclptiTaskPmuCoreData{arithmeticVector.totalCycles, false, arithmeticVector.values};
    arithmeticConflict.pmuLogs.emplace(aclptiBlockKey{}, arithmeticCube);
    arithmeticConflict.pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, arithmeticVector);
    for (auto& entry : arithmeticConflict.pmuLogs) {
        entry.second.totalCycles = 9999;
        entry.second.values.clear();
    }
    const std::vector<std::string> arithmeticSections = {"ArithmeticUtilization", "ResourceConflictRatio"};
    CHECK(npucompute::WritePmuReport(arithmeticConflict, arithmeticSections, config) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records.size() == 4);
    for (std::size_t index = 0; index < arithmeticSections.size(); ++index) {
        CHECK(records[index].get<std::string>("category") == arithmeticSections[index]);
        CHECK(records[index].get<double>("aic_total_cycles") == 1000);
        CHECK(records[index].get<double>("aiv_total_cycles") == 2000);
        CHECK(!records[index].get_optional<std::string>("block_id"));
        CHECK(!records[index].get_optional<std::string>("sub_block_id"));
    }
    CHECK(records[0].get<uint64_t>("aic_cube_total_instr_number") == 99);
    CHECK(records[0].get<uint64_t>("aic_cube_fp_instr_number") == 21);
    CHECK(records[0].get<uint64_t>("aic_cube_int_instr_number") == 32);
    CHECK(std::abs(records[0].get<double>("aic_cube_ratio") - 0.3) < 1e-6);
    CHECK(std::abs(records[0].get<double>("aiv_vec_ratio") - 0.5) < 1e-6);
    CHECK(std::abs(records[1].get<double>("aic_cube_wait_ratio") - 0.01) < 1e-6);
    CHECK(std::abs(records[1].get<double>("aiv_vec_sfu_cflt_ratio") - 0.05) < 1e-6);
    CHECK(records.back().get<std::string>("category") == "OpInfoSummary");
    CHECK(npucompute::cli::ValidateCollectionFile(
        boost::filesystem::path(config.outputDirectory) / "summary.jsonl", npucompute::cli::NpuRepFileType::Jsonl,
        &error));

    const auto bad = boost::filesystem::path(directory) / "bad";
    boost::filesystem::create_directories(bad / "summary.jsonl");
    config.outputDirectory = bad.string();
    CHECK(npucompute::WriteSummaryJsonl(result, {"Memory"}, config, {}) != ACLPTI_SUCCESS);
    for (const auto& entry : boost::filesystem::directory_iterator(bad)) {
        CHECK(entry.path().filename() == "summary.jsonl");
    }
    const auto malformed = boost::filesystem::path(directory) / "summary.jsonl";
    for (const auto* invalid :
         {"{\"category\":\"Memory\"}\n", "not json\n",
          "{\"category\":\"OpInfoSummary\"}\n{\"category\":\"Memory\"}\n"}) {
        std::ofstream output(malformed.string());
        output << invalid;
        output.close();
        CHECK(!npucompute::cli::ValidateCollectionFile(malformed, npucompute::cli::NpuRepFileType::Jsonl, &error));
    }
    boost::filesystem::remove_all(directory);

    // VF summary metrics use the single AIV task counters and represent unavailable values as NA.
    {
        const auto ipcDirectory =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("vf-ipc-summary-%%%%-%%%%");
        boost::filesystem::create_directories(ipcDirectory);
        npucompute::ReportConfig ipcConfig;
        ipcConfig.outputDirectory = ipcDirectory.string();
        ipcConfig.frequencyMhz = 800;
        ipcConfig.aivFrequencyMhz = 800;

        aclptiProfilingDataResult ipcResult;
        ipcResult.taskPmuData.aiv = aclptiTaskPmuCoreData{
            1000, false, {{1281, 800}, {1282, 300}, {1284, 100}, {1320, 900}, {1466, 40}, {1467, 80}, {1468, 120}}};
        // Large AIC task counts must not contribute to AIV summary metrics.
        ipcResult.taskPmuData.aic = aclptiTaskPmuCoreData{
            2000,
            false,
            {{1281, 100}, {1282, 100}, {1284, 100}, {1320, 9000}, {1466, 9000}, {1467, 9000}, {1468, 9000}}};
        for (const auto type : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
            // Different block counts distinguish task summaries from the configured CSV data level.
            aclptiPmuDataRow row{};
            row.coreType = type;
            row.totalCycles = 9000;
            row.values = {{1281, 100},  {1282, 100},  {1284, 100}, {1320, 9000},
                          {1466, 9000}, {1467, 9000}, {1468, 9000}};
            ipcResult.pmuLogs.emplace(aclptiBlockKey{0, 0, type, 0}, row);
        }

        // Task SIMD IPC is 900/300; SIMT IPC is (40+80+120)/100/4.
        CHECK(npucompute::WritePmuReport(ipcResult, {"ArithmeticUtilization"}, ipcConfig) == ACLPTI_SUCCESS);
        auto ipcRecords = ReadSummary(ipcDirectory);
        CHECK(!ipcRecords.empty());
        CHECK(ipcRecords[0].get<double>("aic_total_cycles") == 2000);
        CHECK(ipcRecords[0].get<double>("aiv_total_cycles") == 1000);
        CHECK(!ipcRecords[0].get_child_optional("block_id"));
        CHECK(!ipcRecords[0].get_child_optional("sub_block_id"));
        CHECK(!ipcRecords[0].get_child_optional("aiv_simd_ipc"));
        CHECK(!ipcRecords[0].get_child_optional("aiv_vec_vf_ratio"));
        CHECK(!ipcRecords[0].get_child_optional("aiv_vec_simt_vf_ratio"));
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_ipc") - 3.0) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_ipc") - 0.6) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_ratio") - 0.3) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_ratio") - 0.1) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_time") - 0.375) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_time") - 0.125) < 1e-12);

        auto& taskCore = *ipcResult.taskPmuData.aiv;
        const auto complete = taskCore.values;
        // Missing instruction counts leave ratios and times valid; SIMT requires all three counts.
        for (const auto missing : {1320U, 1466U, 1467U, 1468U}) {
            taskCore.values = complete;
            taskCore.values.erase(missing);
            CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
            ipcRecords = ReadSummary(ipcDirectory);
            CHECK(ipcRecords[0].get<std::string>(missing == 1320U ? "aiv_simd_vf_ipc" : "aiv_simt_vf_ipc") == "NA");
            CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_ratio") - 0.3) < 1e-12);
            CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_ratio") - 0.1) < 1e-12);
            CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_time") - 0.375) < 1e-12);
            CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_time") - 0.125) < 1e-12);
        }

        // Missing VF cycle counts make the corresponding ratio, time and IPC unavailable.
        for (const auto missing : {1282U, 1284U}) {
            taskCore.values = complete;
            taskCore.values.erase(missing);
            CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
            ipcRecords = ReadSummary(ipcDirectory);
            const std::string prefix = missing == 1282U ? "aiv_simd_vf_" : "aiv_simt_vf_";
            for (const auto* suffix : {"ratio", "time", "ipc"}) {
                CHECK(ipcRecords[0].get<std::string>(prefix + suffix) == "NA");
            }
        }

        // Collected zero instruction counts are valid and produce zero IPCs.
        taskCore.values = complete;
        for (const auto event : {1320U, 1466U, 1467U, 1468U}) {
            taskCore.values[event] = 0;
        }
        CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
        ipcRecords = ReadSummary(ipcDirectory);
        CHECK(ipcRecords[0].get<double>("aiv_simd_vf_ipc") == 0);
        CHECK(ipcRecords[0].get<double>("aiv_simt_vf_ipc") == 0);

        // Zero VF cycles leave valid zero ratios and times, while IPCs are unavailable.
        taskCore.values = complete;
        taskCore.values[1282] = 0;
        taskCore.values[1284] = 0;
        CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
        ipcRecords = ReadSummary(ipcDirectory);
        CHECK(ipcRecords[0].get<std::string>("aiv_simd_vf_ipc") == "NA");
        CHECK(ipcRecords[0].get<std::string>("aiv_simt_vf_ipc") == "NA");
        CHECK(ipcRecords[0].get<double>("aiv_simd_vf_ratio") == 0);
        CHECK(ipcRecords[0].get<double>("aiv_simt_vf_ratio") == 0);
        CHECK(ipcRecords[0].get<double>("aiv_simd_vf_time") == 0);
        CHECK(ipcRecords[0].get<double>("aiv_simt_vf_time") == 0);

        // An AIC-only task collection retains all AIV VF fields as NA despite AIV block data.
        ipcResult.taskPmuData.aiv.reset();
        CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
        ipcRecords = ReadSummary(ipcDirectory);
        for (const auto* field :
             {"aiv_simd_vf_ratio", "aiv_simt_vf_ratio", "aiv_simd_vf_time", "aiv_simd_vf_ipc", "aiv_simt_vf_time",
              "aiv_simt_vf_ipc"}) {
            CHECK(ipcRecords[0].get<std::string>(field) == "NA");
        }

        // A summary requested only for PipeUtilization does not expose the ArithmeticUtilization IPCs.
        CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"PipeUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
        ipcRecords = ReadSummary(ipcDirectory);
        CHECK(!ipcRecords.empty());
        CHECK(!ipcRecords[0].get_child_optional("aiv_simd_vf_ipc"));
        CHECK(!ipcRecords[0].get_child_optional("aiv_simt_vf_ipc"));

        boost::filesystem::remove_all(ipcDirectory);
    }

    return 0;
}

TEST(NpuComputePmuReportWriter, Main) { ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputePmuReportWriter reported failure"; }

TEST(NpuComputePmuReportWriter, BlockCsvRetainsIdentityAndIgnoresTaskCounters)
{
    class Sink final : public npucompute::ArtifactSink {
    public:
        void Begin(const std::string&) override { text.clear(); }
        void Write(std::string_view bytes) override { text.append(bytes); }
        void Commit(uint64_t count) override { rows = count; }
        void Abort() noexcept override {}
        std::string text;
        uint64_t rows = 0;
    } sink;
    aclptiProfilingDataResult result;
    result.taskPmuData.aic = aclptiTaskPmuCoreData{1000, false, {}};
    result.taskPmuData.aiv = aclptiTaskPmuCoreData{2000, false, {}};
    npucompute::ReportConfig config;
    aclptiPmuDataRow block{};
    block.blockId = 7;
    block.subBlockId = 3;
    block.coreType = ACLPTI_CORE_TYPE_AIV;
    block.totalCycles = 3000;
    result.pmuLogs.emplace(aclptiBlockKey{7, 3, ACLPTI_CORE_TYPE_AIV, 0}, block);
    ASSERT_EQ(npucompute::WritePmuCsv(result, {"PipeUtilization"}, config, &sink), ACLPTI_SUCCESS);
    EXPECT_EQ(sink.rows, 1U);
    EXPECT_EQ(std::count(sink.text.begin(), sink.text.end(), '\n'), 2);
    EXPECT_NE(sink.text.find("block_id"), std::string::npos);
    EXPECT_NE(sink.text.find("sub_block_id"), std::string::npos);
    EXPECT_NE(sink.text.find("aic_total_cycles"), std::string::npos);
    EXPECT_NE(sink.text.find("aiv_total_cycles"), std::string::npos);
    std::istringstream input(sink.text);
    std::string line;
    std::getline(input, line);
    std::vector<std::string> header;
    std::istringstream headerStream(line);
    while (std::getline(headerStream, line, ',')) {
        header.push_back(line);
    }
    std::getline(input, line);
    std::vector<std::string> values;
    std::istringstream valueStream(line);
    while (std::getline(valueStream, line, ',')) {
        values.push_back(line);
    }
    ASSERT_EQ(header.size(), values.size());
    const auto aic = std::find(header.begin(), header.end(), "aic_total_cycles") - header.begin();
    const auto aiv = std::find(header.begin(), header.end(), "aiv_total_cycles") - header.begin();
    EXPECT_EQ(values.at(0), "7");
    EXPECT_EQ(values.at(1), "vector3");
    EXPECT_EQ(values.at(aic), "NA");
    EXPECT_EQ(values.at(aiv), "3000");

    result.pmuLogs.clear();
    sink.text.clear();
    sink.rows = 0;
    ASSERT_EQ(npucompute::WritePmuCsv(result, {"PipeUtilization"}, config, &sink), ACLPTI_SUCCESS);
    EXPECT_EQ(sink.rows, 0U);
    EXPECT_TRUE(sink.text.empty());
}
