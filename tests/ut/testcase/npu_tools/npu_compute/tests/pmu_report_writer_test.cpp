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
#include "report/collection_file_validator.h"
#include "report/rep_directory_packer.h"
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
    result.taskPmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, task);
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
    CHECK(records.size() == 2);
    CHECK(std::abs(records[0].get<double>("aiv_main_mem_read_bw(GB/s)") - 0.476837) < 1e-6);
    CHECK(records[1].get<std::string>("Op Name") == "NA");
    CHECK(records[1].get<std::string>("aicore_parallel_utilization") == "NA");
    CHECK(records[1].size() == 20);

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
    result.taskPmuLogs.begin()->second.values[0x501] = 1000;
    CHECK(npucompute::WritePmuReport(result, sections, config, metadata) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records.size() == 6);
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

    config.pmuDataLevel = npucompute::PmuDataLevel::Task;
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
    CHECK(records.size() == 2);
    CHECK(records[0].get<std::string>("aiv_total_cycles") == "NA");
    CHECK(!boost::filesystem::exists(boost::filesystem::path(config.outputDirectory) / "Memory.csv"));

    config.outputDirectory = (boost::filesystem::path(directory) / "average").string();
    result.taskPmuLogs.begin()->second.values[0x501] = 1000;
    auto secondTask = result.taskPmuLogs.begin()->second;
    secondTask.blockId = 1;
    secondTask.totalCycles = 4000;
    secondTask.values[0x501] = 3200;
    result.taskPmuLogs.emplace(aclptiBlockKey{1, 0, ACLPTI_CORE_TYPE_AIV, 0}, secondTask);
    result.pmuLogs = result.taskPmuLogs;
    npucompute::KernelMetadata averageMetadata;
    averageMetadata.kind = npucompute::KernelKind::Vector;
    averageMetadata.blockDim = 2;
    result.taskLogs[1] = {
        {0, 0, 1, 0, 100, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}, {0, 1, 1, 0, 10100, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}};
    CHECK(npucompute::WritePmuReport(result, {"PipeUtilization"}, config, averageMetadata) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records[0].get<double>("aiv_total_cycles") == 3000);
    CHECK(std::abs(records[0].get<double>("aiv_vec_ratio") - 0.65) < 1e-6);
    CHECK(std::abs(records[1].get<double>("aicore_parallel_utilization") - 30) < 1e-6);
    CHECK(std::abs(records[1].get<double>("aicore_parallel_balance") - 75) < 1e-6);

    config.outputDirectory = (boost::filesystem::path(directory) / "mixed").string();
    aclptiPmuDataRow cube{};
    cube.coreType = ACLPTI_CORE_TYPE_AIC;
    cube.totalCycles = 1000;
    cube.values = {{0x422, 16}, {0x423, 8}};
    result.taskPmuLogs.emplace(aclptiBlockKey{}, cube);
    result.taskLogs[1] = {
        {0, 0, 1, 0, 100, 0, 0, ACLPTI_CORE_TYPE_AIC, 0}, {0, 1, 1, 0, 10100, 0, 0, ACLPTI_CORE_TYPE_AIC, 0}};
    config.aicFrequencyMhz = 1200;
    config.aivFrequencyMhz = 1000;
    CHECK(npucompute::WritePmuReport(result, {"Memory"}, config) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records[1].get<std::string>("Op Type") == "mix");
    CHECK(records[1].get<double>("Task Duration(us)") == 10);
    CHECK(records[1].get<std::string>("Current Freq") == "NA");
    CHECK(std::abs(records[1].get<double>("aicore_gm_read_bw(GB/s)") - 0.286102) < 1e-6);
    CHECK(std::abs(records[1].get<double>("aicore_gm_bw_usage_rate(%)") - 0.026822) < 1e-6);

    config.outputDirectory = (boost::filesystem::path(directory) / "zero").string();
    aclptiProfilingDataResult zero;
    cube.totalCycles = 0;
    cube.values = {{810, 0}};
    zero.taskPmuLogs.emplace(aclptiBlockKey{}, cube);
    zero.pmuLogs = zero.taskPmuLogs;
    CHECK(npucompute::WritePmuReport(zero, {"PipeUtilization"}, config) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records[0].get<std::string>("aic_cube_ratio") == "NA");
    CHECK(records[0].get<double>("aic_total_cycles") == 0);
    CHECK(records[1].get<std::string>("aicore_parallel_balance") == "NA");

    // Summary metrics must use task PMU rows even when CSV selects block rows.
    config.outputDirectory = (boost::filesystem::path(directory) / "arithmetic-conflict").string();
    config.pmuDataLevel = npucompute::PmuDataLevel::Block;
    aclptiProfilingDataResult arithmeticConflict;
    aclptiPmuDataRow arithmeticCube{};
    arithmeticCube.coreType = ACLPTI_CORE_TYPE_AIC;
    arithmeticCube.totalCycles = 1000;
    arithmeticCube.values = {{768, 99},  {789, 21}, {790, 32}, {808, 100}, {809, 200},
                             {810, 300}, {11, 10},  {13, 20},  {14, 30},   {15, 40}};
    arithmeticConflict.taskPmuLogs.emplace(aclptiBlockKey{}, arithmeticCube);
    aclptiPmuDataRow arithmeticVector{};
    arithmeticVector.coreType = ACLPTI_CORE_TYPE_AIV;
    arithmeticVector.totalCycles = 2000;
    arithmeticVector.values = {{1281, 1000}, {1282, 200}, {1283, 300}, {1284, 500}, {12, 100},  {14, 200}, {15, 300},
                               {1344, 400},  {1366, 500}, {1376, 10},  {1377, 20},  {1378, 30}, {1379, 40}};
    arithmeticConflict.taskPmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, arithmeticVector);
    arithmeticConflict.pmuLogs = arithmeticConflict.taskPmuLogs;
    for (auto& entry : arithmeticConflict.pmuLogs) {
        entry.second.totalCycles = 9999;
        entry.second.values.clear();
    }
    const std::vector<std::string> arithmeticSections = {"ArithmeticUtilization", "ResourceConflictRatio"};
    CHECK(npucompute::WritePmuReport(arithmeticConflict, arithmeticSections, config) == ACLPTI_SUCCESS);
    records = ReadSummary(config.outputDirectory);
    CHECK(records.size() == 3);
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
    CHECK(records[2].get<std::string>("category") == "OpInfoSummary");
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

    // VF summary metrics average valid AIV task rows and represent unavailable values as null.
    {
        const auto ipcDirectory =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("vf-ipc-summary-%%%%-%%%%");
        boost::filesystem::create_directories(ipcDirectory);
        npucompute::ReportConfig ipcConfig;
        ipcConfig.outputDirectory = ipcDirectory.string();
        ipcConfig.frequencyMhz = 800;
        ipcConfig.aivFrequencyMhz = 800;
        ipcConfig.pmuDataLevel = npucompute::PmuDataLevel::Block;

        // AIV task rows have SIMD IPCs 1, 3 and NA; SIMT IPCs are 0.6, 0.6 and NA.
        aclptiProfilingDataResult ipcResult;
        for (const auto type : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
            for (uint16_t block = 0; block < 3; ++block) {
                aclptiPmuDataRow row{};
                row.coreType = type;
                row.blockId = block;
                row.totalCycles = 1000;
                row.values = {{1281, 800}, {1282, block == 0 ? 100.0 : 300.0},
                              {1284, 100}, {1320, block == 0 ? 100.0 : 900.0},
                              {1466, 40},  {1467, 80},
                              {1468, 120}};
                if (block == 2) {
                    row.values.erase(1320);
                    row.values.erase(1467);
                }
                if (type == ACLPTI_CORE_TYPE_AIC) {
                    // Large AIC counts must not contribute to AIV summary metrics.
                    row.values = {{1281, 100},  {1282, 100},  {1284, 100}, {1320, 9000},
                                  {1466, 9000}, {1467, 9000}, {1468, 9000}};
                }
                ipcResult.taskPmuLogs.emplace(aclptiBlockKey{block, 0, type, 0}, row);

                // Large block counts distinguish task-based summaries from the configured CSV level.
                row.values = {{1281, 100},  {1282, 100},  {1284, 100}, {1320, 9000},
                              {1466, 9000}, {1467, 9000}, {1468, 9000}};
                ipcResult.pmuLogs.emplace(aclptiBlockKey{block, 0, type, 0}, row);
            }
        }

        // Average valid per-row IPCs: (1+3)/2 = 2, rather than dividing summed counts by summed cycles.
        CHECK(npucompute::WritePmuReport(ipcResult, {"ArithmeticUtilization"}, ipcConfig) == ACLPTI_SUCCESS);
        auto ipcRecords = ReadSummary(ipcDirectory);
        CHECK(!ipcRecords.empty());
        CHECK(!ipcRecords[0].get_child_optional("aiv_simd_ipc"));
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_ipc") - 2.0) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_ipc") - 0.6) < 1e-12);

        // Missing instruction counts do not invalidate ratios or times; all three AIV task rows contribute.
        CHECK(!ipcRecords[0].get_child_optional("aiv_vec_vf_ratio"));
        CHECK(!ipcRecords[0].get_child_optional("aiv_vec_simt_vf_ratio"));
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_ratio") - 0.233333) < 1e-6);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_ratio") - 0.1) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_time") - 0.291667) < 1e-6);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_time") - 0.125) < 1e-12);

        // A zero time is valid and participates in the mean; a missing cycle event does not.
        ipcResult.taskPmuLogs.at({0, 0, ACLPTI_CORE_TYPE_AIV, 0}).values[1282] = 0;
        ipcResult.taskPmuLogs.at({0, 0, ACLPTI_CORE_TYPE_AIV, 0}).values[1284] = 0;
        ipcResult.taskPmuLogs.at({2, 0, ACLPTI_CORE_TYPE_AIV, 0}).values.erase(1282);
        ipcResult.taskPmuLogs.at({2, 0, ACLPTI_CORE_TYPE_AIV, 0}).values.erase(1284);
        CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
        ipcRecords = ReadSummary(ipcDirectory);
        CHECK(!ipcRecords.empty());
        // SIMD: (0 + 300/800)/2; SIMT: (0 + 100/800)/2.
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simd_vf_time") - 0.1875) < 1e-12);
        CHECK(std::abs(ipcRecords[0].get<double>("aiv_simt_vf_time") - 0.0625) < 1e-12);

        // Zero VF cycles make all IPC samples unavailable, while ratios and times remain valid zeros.
        for (auto& [key, row] : ipcResult.taskPmuLogs) {
            row.values[1282] = 0;
            row.values[1284] = 0;
        }
        CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
        ipcRecords = ReadSummary(ipcDirectory);
        CHECK(!ipcRecords.empty());
        CHECK(ipcRecords[0].get<std::string>("aiv_simd_vf_ipc") == "NA");
        CHECK(ipcRecords[0].get<std::string>("aiv_simt_vf_ipc") == "NA");
        CHECK(ipcRecords[0].get<double>("aiv_simd_vf_ratio") == 0);
        CHECK(ipcRecords[0].get<double>("aiv_simt_vf_ratio") == 0);

        CHECK(ipcRecords[0].get<double>("aiv_simd_vf_time") == 0);
        CHECK(ipcRecords[0].get<double>("aiv_simt_vf_time") == 0);

        // An AIC-only task collection retains the AIV VF time and IPC fields as NA.
        ipcResult.taskPmuLogs.erase({0, 0, ACLPTI_CORE_TYPE_AIV, 0});
        ipcResult.taskPmuLogs.erase({1, 0, ACLPTI_CORE_TYPE_AIV, 0});
        ipcResult.taskPmuLogs.erase({2, 0, ACLPTI_CORE_TYPE_AIV, 0});
        CHECK(npucompute::WriteSummaryJsonl(ipcResult, {"ArithmeticUtilization"}, ipcConfig, {}) == ACLPTI_SUCCESS);
        ipcRecords = ReadSummary(ipcDirectory);
        CHECK(ipcRecords[0].get<std::string>("aiv_simd_vf_time") == "NA");
        CHECK(ipcRecords[0].get<std::string>("aiv_simd_vf_ipc") == "NA");
        CHECK(ipcRecords[0].get<std::string>("aiv_simt_vf_time") == "NA");
        CHECK(ipcRecords[0].get<std::string>("aiv_simt_vf_ipc") == "NA");

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
