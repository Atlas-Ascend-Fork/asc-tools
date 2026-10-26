/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "plog_capture.h"
#include <gtest/gtest.h>

#include "report/report_writer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#define CHECK(condition)                                      \
    do {                                                      \
        if (!(condition)) {                                   \
            ADD_FAILURE_AT(__FILE__, __LINE__) << #condition; \
            return 1;                                         \
        }                                                     \
    } while (false)

namespace {

aclptiPmuDataRow::CoreData Core(
    aclptiCoreType type, uint8_t id, double cycles, std::initializer_list<std::pair<uint32_t, double>> values)
{
    aclptiPmuDataRow::CoreData core{};
    core.coreType = type;
    core.coreId = id;
    core.sampleCount = 1;
    core.totalCycles = cycles;
    for (const auto& [eventId, value] : values) {
        core.values.emplace(eventId, value);
        core.valueCounts.emplace(eventId, 1);
    }
    return core;
}

std::string ReadFile(const boost::filesystem::path& path)
{
    std::ifstream input(path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<std::string> SplitCsvLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::size_t first = 0;
    while (first <= line.size()) {
        const std::size_t comma = line.find(',', first);
        if (comma == std::string::npos) {
            fields.push_back(line.substr(first));
            break;
        }
        fields.push_back(line.substr(first, comma - first));
        first = comma + 1;
    }
    return fields;
}

std::vector<std::string> CsvHeader(const boost::filesystem::path& path)
{
    std::ifstream input(path.string());
    std::string line;
    return std::getline(input, line) ? SplitCsvLine(line) : std::vector<std::string>{};
}

std::string CsvValue(const boost::filesystem::path& path, const std::string& subBlockId, const std::string& column)
{
    std::ifstream input(path.string());
    std::string line;
    if (!std::getline(input, line)) {
        return {};
    }
    const std::vector<std::string> header = SplitCsvLine(line);
    const auto columnIt = std::find(header.begin(), header.end(), column);
    if (columnIt == header.end()) {
        return {};
    }
    const std::size_t columnIndex = static_cast<std::size_t>(std::distance(header.begin(), columnIt));
    while (std::getline(input, line)) {
        const std::vector<std::string> fields = SplitCsvLine(line);
        if (fields.size() == header.size() && (subBlockId.empty() || fields[1] == subBlockId)) {
            return fields[columnIndex];
        }
    }
    return {};
}

const std::vector<std::string>& ExpectedA5Header(const std::string& section)
{
    static const std::vector<std::string> l2 = {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_read_close_hit",
        "aic_read_close_miss",
        "aic_read_close_victim",
        "aic_read_far_hit",
        "aic_read_far_miss",
        "aic_read_far_victim",
        "aic_read_hit_rate(%)",
        "aic_write_close_hit",
        "aic_write_close_miss",
        "aic_write_close_victim",
        "aic_write_far_hit",
        "aic_write_far_miss",
        "aic_write_far_victim",
        "aic_write_hit_rate(%)",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_read_close_hit",
        "aiv_read_close_miss",
        "aiv_read_close_victim",
        "aiv_read_far_hit",
        "aiv_read_far_miss",
        "aiv_read_far_victim",
        "aiv_read_hit_rate(%)",
        "aiv_write_close_hit",
        "aiv_write_close_miss",
        "aiv_write_close_victim",
        "aiv_write_far_hit",
        "aiv_write_far_miss",
        "aiv_write_far_victim",
        "aiv_write_hit_rate(%)"};
    static const std::vector<std::string> memory = {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_l1_read_bw(GB/s)",
        "aic_l1_write_bw(GB/s)",
        "aic_main_mem_read_bw(GB/s)",
        "aic_main_mem_write_bw(GB/s)",
        "aic_mte1_instructions",
        "aic_mte1_ratio",
        "aic_mte2_instructions",
        "aic_mte2_ratio",
        "aic_mte3_instructions",
        "aic_mte3_ratio",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_ub_to_gm_bw(GB/s)",
        "aiv_gm_to_ub_bw(GB/s)",
        "aiv_main_mem_read_bw(GB/s)",
        "aiv_main_mem_write_bw(GB/s)",
        "aiv_mte2_instructions",
        "aiv_mte2_ratio",
        "aiv_mte3_instructions",
        "aiv_mte3_ratio",
        "read_main_memory_datas(KB)",
        "write_main_memory_datas(KB)",
        "GM_to_L1_datas(KB)",
        "GM_to_L1_bw_usage_rate(%)",
        "L0C_to_L1_datas(KB)",
        "L0C_to_L1_bw_usage_rate(%)",
        "L0C_to_GM_datas(KB)",
        "L0C_to_GM_bw_usage_rate(%)",
        "GM_to_UB_datas(KB)",
        "GM_to_UB_bw_usage_rate(%)",
        "UB_to_GM_datas(KB)",
        "UB_to_GM_bw_usage_rate(%)"};
    static const std::vector<std::string> memoryL0 = {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_l0a_read_bw(GB/s)",
        "aic_l0a_write_bw(GB/s)",
        "aic_l0b_read_bw(GB/s)",
        "aic_l0b_write_bw(GB/s)",
        "aic_l0c_read_bw_cube(GB/s)",
        "aic_l0c_write_bw_cube(GB/s)",
        "aiv_time(us)",
        "aiv_total_cycles"};
    static const std::vector<std::string> memoryUb = {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_ub_read_bw_vector(GB/s)",
        "aiv_ub_write_bw_vector(GB/s)",
        "aiv_ub_read_bw_gm(GB/s)",
        "aiv_ub_write_bw_gm(GB/s)"};
    static const std::vector<std::string> pipe = {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_cube_time(us)",
        "aic_cube_ratio",
        "aic_scalar_time(us)",
        "aic_scalar_ratio",
        "aic_mte1_time(us)",
        "aic_mte1_ratio",
        "aic_mte1_active_bw(GB/s)",
        "aic_mte2_time(us)",
        "aic_mte2_ratio",
        "aic_mte2_active_bw(GB/s)",
        "aic_mte3_time(us)",
        "aic_mte3_ratio",
        "aic_mte3_active_bw(GB/s)",
        "aic_fixpipe_time(us)",
        "aic_fixpipe_ratio",
        "aic_fixpipe_active_bw(GB/s)",
        "aic_icache_miss_rate",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_vec_time(us)",
        "aiv_vec_ratio",
        "aiv_scalar_time(us)",
        "aiv_scalar_ratio",
        "aiv_mte2_time(us)",
        "aiv_mte2_ratio",
        "aiv_mte2_active_bw(GB/s)",
        "aiv_mte3_time(us)",
        "aiv_mte3_ratio",
        "aiv_mte3_active_bw(GB/s)",
        "aiv_icache_miss_rate"};
    if (section == "L2Cache") {
        return l2;
    }
    if (section == "Memory") {
        return memory;
    }
    if (section == "MemoryL0") {
        return memoryL0;
    }
    if (section == "MemoryUB") {
        return memoryUb;
    }
    return pipe;
}

std::size_t FieldCount(const std::string& line)
{
    return line.empty() ? 0 : 1 + static_cast<std::size_t>(std::count(line.begin(), line.end(), ','));
}

bool HasMatchingColumnCounts(const boost::filesystem::path& path)
{
    std::ifstream input(path.string());
    std::string header;
    std::string row;
    return std::getline(input, header) && std::getline(input, row) && FieldCount(header) == FieldCount(row);
}

std::vector<boost::filesystem::path> NestedCsvFiles(const boost::filesystem::path& root, const std::string& filename)
{
    std::vector<boost::filesystem::path> paths;
    for (const auto& entry : boost::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().filename() == filename && entry.path().parent_path() != root) {
            paths.push_back(entry.path());
        }
    }
    return paths;
}

using npu_compute_test::CapturePlog;

} // namespace

static int RunSuiteMain()
{
    {
        const auto path = boost::filesystem::temp_directory_path() /
                          boost::filesystem::unique_path("npu_compute_arithmetic_%%%%-%%%%");
        npucompute::ReportConfig config;
        config.outputDirectory = path.string();
        config.frequencyMhz = 1000.0;
        aclptiProfilingDataResult result;
        aclptiPmuDataRow cube{};
        cube.coreType = ACLPTI_CORE_TYPE_AIC;
        cube.coreData.push_back(Core(
            ACLPTI_CORE_TYPE_AIC, 0, 1000,
            {{768, 99},
             {789, 21},
             {790, 32},
             {808, 100},
             {809, 200},
             {810, 300},
             {11, 10},
             {13, 20},
             {14, 30},
             {15, 40}}));
        aclptiPmuDataRow vector{};
        vector.coreType = ACLPTI_CORE_TYPE_AIV;
        vector.coreData.push_back(Core(
            ACLPTI_CORE_TYPE_AIV, 0, 2000,
            {{1281, 1000},
             {1282, 200},
             {1283, 300},
             {1284, 500},
             {12, 100},
             {14, 200},
             {15, 300},
             {1344, 400},
             {1366, 500},
             {1376, 10},
             {1377, 20},
             {1378, 30},
             {1379, 40}}));
        result.pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIC, 0}, cube);
        result.pmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, vector);
        const std::vector<std::string> sections = {"ArithmeticUtilization", "ResourceConflictRatio", "PipeUtilization"};
        CHECK(npucompute::WritePmuCsv(result, sections, config) == ACLPTI_SUCCESS);
        const auto arithmetic = path / "ArithmeticUtilization.csv";
        const auto conflict = path / "ResourceConflictRatio.csv";
        CHECK(CsvHeader(arithmetic).size() == 20);
        CHECK(CsvHeader(conflict).size() == 16);
        for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
                 {"aic_cube_ratio", "0.300000"},
                 {"aic_cube_fp_ratio", "0.100000"},
                 {"aic_cube_int_ratio", "0.200000"},
                 {"aic_cube_total_instr_number", "99"},
                 {"aic_cube_fp_instr_number", "21"},
                 {"aic_cube_int_instr_number", "32"},
                 {"aic_time(us)", "1.000000"}}) {
            CHECK(CsvValue(arithmetic, "cube0", field) == value);
            CHECK(CsvValue(arithmetic, "vector0", field) == "NA");
        }
        for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
                 {"aiv_vec_ratio", "0.500000"},
                 {"aiv_simd_vf_ratio", "0.100000"},
                 {"aiv_vec_sfu_ratio", "0.150000"},
                 {"aiv_simt_vf_ratio", "0.250000"}}) {
            CHECK(CsvValue(arithmetic, "vector0", field) == value);
            CHECK(CsvValue(arithmetic, "cube0", field) == "NA");
        }
        for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
                 {"aic_cube_wait_ratio", "0.010000"},
                 {"aic_mte1_wait_ratio", "0.020000"},
                 {"aic_mte2_wait_ratio", "0.030000"},
                 {"aic_mte3_wait_ratio", "0.040000"}}) {
            CHECK(CsvValue(conflict, "cube0", field) == value);
            CHECK(CsvValue(conflict, "vector0", field) == "NA");
        }
        for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
                 {"aiv_vec_wait_ratio", "0.050000"},
                 {"aiv_mte2_wait_ratio", "0.100000"},
                 {"aiv_mte3_wait_ratio", "0.150000"},
                 {"aiv_vec_ldu_cflt_ratio", "0.200000"},
                 {"aiv_vec_stu_cflt_ratio", "0.250000"},
                 {"aiv_vec_sfu_cflt_ratio", "0.050000"}}) {
            CHECK(CsvValue(conflict, "vector0", field) == value);
            CHECK(CsvValue(conflict, "cube0", field) == "NA");
        }
        CHECK(
            CsvValue(arithmetic, "cube0", "aic_cube_ratio") ==
            CsvValue(path / "PipeUtilization.csv", "cube0", "aic_cube_ratio"));
        // Exercise missing events and zero cycles using block input without changing the valid fixture.
        auto invalidMetricsResult = result;
        auto& invalidMetricsAivRow = invalidMetricsResult.pmuLogs.at({0, 0, ACLPTI_CORE_TYPE_AIV, 0});
        invalidMetricsAivRow.coreData.front().values.erase(1377);
        invalidMetricsAivRow.coreData.front().values[1282] = 0;
        config.outputDirectory = (path / "missing-event").string();
        config.frequencyMhz = 2000;
        CHECK(npucompute::WritePmuCsv(invalidMetricsResult, sections, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(path / "missing-event/ResourceConflictRatio.csv", "vector0", "aiv_vec_sfu_cflt_ratio") == "NA");
        CHECK(CsvValue(path / "missing-event/ArithmeticUtilization.csv", "vector0", "aiv_simd_vf_ratio") == "0.000000");
        CHECK(CsvValue(path / "missing-event/ArithmeticUtilization.csv", "vector0", "aiv_time(us)") == "1.000000");
        CHECK(CsvValue(path / "missing-event/ArithmeticUtilization.csv", "vector0", "aiv_vec_ratio") == "0.500000");
        invalidMetricsAivRow.coreData.front().values[1377] = 20;
        invalidMetricsAivRow.coreData.front().totalCycles = 0;
        config.outputDirectory = (path / "zero-cycles").string();
        CHECK(npucompute::WritePmuCsv(invalidMetricsResult, sections, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(path / "zero-cycles/ResourceConflictRatio.csv", "vector0", "aiv_vec_sfu_cflt_ratio") == "NA");
        CHECK(CsvValue(path / "zero-cycles/ArithmeticUtilization.csv", "vector0", "aiv_vec_ratio") == "NA");
        // A collection containing only a new section must also prevent overwrites.
        for (const std::string section : {"ArithmeticUtilization", "ResourceConflictRatio"}) {
            const auto repeated = path / section;
            config.outputDirectory = repeated.string();
            CHECK(npucompute::WritePmuCsv(result, {section}, config) == ACLPTI_SUCCESS);
            const auto original = ReadFile(repeated / (section + ".csv"));
            config.frequencyMhz = 4000;
            CHECK(npucompute::WritePmuCsv(result, {section}, config) == ACLPTI_SUCCESS);
            CHECK(ReadFile(repeated / (section + ".csv")) == original);
            std::size_t children = 0;
            for (const auto& entry : boost::filesystem::directory_iterator(repeated)) {
                if (boost::filesystem::is_directory(entry.path())) {
                    ++children;
                    CHECK(boost::filesystem::exists(entry.path() / (section + ".csv")));
                }
            }
            CHECK(children == 1);
        }
        boost::filesystem::remove_all(path);
    }
    const auto directory =
        boost::filesystem::temp_directory_path() /
        ("npu_compute_csv_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    aclptiProfilingDataResult result;
    result.status = ACLPTI_SUCCESS;
    aclptiPmuDataRow aicRow{};
    aicRow.blockId = 2;
    aicRow.subBlockId = 3;
    aicRow.coreType = ACLPTI_CORE_TYPE_AIC;
    aicRow.coreId = 3;
    aicRow.coreData.push_back(Core(
        ACLPTI_CORE_TYPE_AIC, 3, 1000.0,
        {{0x424, 10.0}, {0x425, 2.0}, {0x426, 1.0}, {0x427, 5.0}, {0x428, 1.0}, {0x429, 1.0}}));
    aclptiPmuDataRow aivRow{};
    aivRow.blockId = 2;
    aivRow.subBlockId = 3;
    aivRow.coreType = ACLPTI_CORE_TYPE_AIV;
    aivRow.coreId = 3;
    aivRow.coreData.push_back(Core(
        ACLPTI_CORE_TYPE_AIV, 3, 2000.0,
        {{0x424, 20.0},
         {0x425, 4.0},
         {0x426, 2.0},
         {0x427, 10.0},
         {0x428, 2.0},
         {0x429, 2.0},
         {0x34, 40.0},
         {0x35, 10.0},
         {0x422, 100.0},
         {0x57f, 10.0},
         {0x580, 10.0}}));
    result.pmuLogs.emplace(aclptiBlockKey{2, 3, ACLPTI_CORE_TYPE_AIC, 3}, aicRow);
    result.pmuLogs.emplace(aclptiBlockKey{2, 3, ACLPTI_CORE_TYPE_AIV, 3}, aivRow);

    npucompute::ReportConfig config;
    config.outputDirectory = directory.string();
    config.frequencyMhz = 1000.0;
    config.socName = "950X";
    CHECK(
        npucompute::WritePmuCsv(result, {"L2Cache", "Memory", "MemoryL0", "MemoryUB", "PipeUtilization"}, config) ==
        ACLPTI_SUCCESS);

    const std::string l2 = ReadFile(directory / "L2Cache.csv");
    CHECK(l2.find("block_id,sub_block_id") == 0);
    CHECK(l2.find("aic_read_close_hit") != std::string::npos);
    CHECK(l2.find("aic_read_hit_rate(%)") != std::string::npos);
    CHECK(l2.find("2,cube3,") != std::string::npos);
    CHECK(l2.find("2,vector3,") != std::string::npos);
    CHECK(boost::filesystem::exists(directory / "Memory.csv"));
    CHECK(boost::filesystem::exists(directory / "MemoryL0.csv"));
    CHECK(boost::filesystem::exists(directory / "MemoryUB.csv"));
    CHECK(boost::filesystem::exists(directory / "PipeUtilization.csv"));
    for (const std::string& section : {"L2Cache", "Memory", "MemoryL0", "MemoryUB", "PipeUtilization"}) {
        CHECK(CsvHeader(directory / (section + ".csv")) == ExpectedA5Header(section));
    }
    CHECK(CsvValue(directory / "PipeUtilization.csv", "vector3", "aiv_icache_miss_rate") == "0.250000");
    CHECK(CsvValue(directory / "MemoryUB.csv", "vector3", "aiv_ub_read_bw_gm(GB/s)") == "NA");
    CHECK(CsvValue(directory / "MemoryUB.csv", "vector3", "aiv_ub_write_bw_gm(GB/s)") == "4.768372");
    CHECK(HasMatchingColumnCounts(directory / "L2Cache.csv"));
    CHECK(HasMatchingColumnCounts(directory / "Memory.csv"));
    CHECK(HasMatchingColumnCounts(directory / "MemoryL0.csv"));
    CHECK(HasMatchingColumnCounts(directory / "MemoryUB.csv"));
    CHECK(HasMatchingColumnCounts(directory / "PipeUtilization.csv"));

    const auto splitFrequencyDirectory = boost::filesystem::temp_directory_path() /
                                         ("npu_compute_csv_split_frequency_test_" +
                                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    config.outputDirectory = splitFrequencyDirectory.string();
    config.aicFrequencyMhz = 500.0;
    config.aivFrequencyMhz = 2000.0;
    CHECK(npucompute::WritePmuCsv(result, {"L2Cache"}, config) == ACLPTI_SUCCESS);
    const std::string splitFrequencyL2 = ReadFile(splitFrequencyDirectory / "L2Cache.csv");
    CHECK(CsvValue(splitFrequencyDirectory / "L2Cache.csv", "cube3", "aic_time(us)") == "2.000000");
    CHECK(CsvValue(splitFrequencyDirectory / "L2Cache.csv", "vector3", "aiv_time(us)") == "1.000000");
    boost::filesystem::remove_all(splitFrequencyDirectory);
    config.outputDirectory = directory.string();
    config.aicFrequencyMhz = 0.0;
    config.aivFrequencyMhz = 0.0;

    aclptiProfilingDataResult secondResult = result;
    secondResult.pmuLogs.clear();
    aclptiPmuDataRow secondRow = aicRow;
    secondRow.blockId = 8;
    secondRow.subBlockId = 9;
    secondRow.coreId = 9;
    secondRow.coreData.clear();
    secondRow.coreData.push_back(Core(
        ACLPTI_CORE_TYPE_AIC, 9, 1000.0,
        {{0x424, 10.0}, {0x425, 2.0}, {0x426, 1.0}, {0x427, 5.0}, {0x428, 1.0}, {0x429, 1.0}}));
    secondResult.pmuLogs.emplace(aclptiBlockKey{8, 9, ACLPTI_CORE_TYPE_AIC, 9}, secondRow);
    CHECK(npucompute::WritePmuCsv(secondResult, {"L2Cache"}, config) == ACLPTI_SUCCESS);
    CHECK(ReadFile(directory / "L2Cache.csv") == l2);
    const std::vector<boost::filesystem::path> nestedL2Files = NestedCsvFiles(directory, "L2Cache.csv");
    CHECK(nestedL2Files.size() == 1);
    CHECK(ReadFile(nestedL2Files[0]).find("8,cube9,") != std::string::npos);

    boost::filesystem::remove_all(directory);

    const auto sparseDirectory =
        boost::filesystem::temp_directory_path() /
        ("npu_compute_csv_sparse_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    aclptiProfilingDataResult sparseResult;
    aclptiPmuDataRow sparseRow{};
    sparseRow.blockId = 4;
    sparseRow.subBlockId = 5;
    sparseRow.coreType = ACLPTI_CORE_TYPE_AIC;
    sparseRow.coreId = 5;
    sparseRow.coreData.push_back(Core(ACLPTI_CORE_TYPE_AIC, 5, 1000.0, {{0x424, 10.0}, {0x425, 0.0}}));
    sparseResult.pmuLogs.emplace(aclptiBlockKey{4, 5, ACLPTI_CORE_TYPE_AIC, 5}, sparseRow);
    config.outputDirectory = sparseDirectory.string();
    const char* previousDebug = std::getenv("NPU_COMPUTE_DEBUG");
    const std::string previousDebugValue = previousDebug == nullptr ? "" : previousDebug;
    const bool hadPreviousDebug = previousDebug != nullptr;
    CHECK(setenv("NPU_COMPUTE_DEBUG", "1", 1) == 0);
    std::string sparseLog;
    aclptiResult sparseWriteStatus = ACLPTI_ERROR_INTERNAL;
    CHECK(CapturePlog(
        [&] { sparseWriteStatus = npucompute::WritePmuCsv(sparseResult, {"L2Cache"}, config); }, &sparseLog));
    CHECK(sparseWriteStatus == ACLPTI_SUCCESS);
    CHECK(sparseLog.find("CSV section data availability: section=L2Cache rows=1") != std::string::npos);
    CHECK(sparseLog.find("missingFields=") != std::string::npos);
    CHECK(sparseLog.find("missingReasons=") != std::string::npos);
    CHECK(sparseLog.find("missingColumnReasons=") != std::string::npos);
    CHECK(sparseLog.find("CoreTypeNotApplicable") != std::string::npos);
    CHECK(sparseLog.find("EventMissing") != std::string::npos);
    CHECK(sparseLog.find("aiv_time(us)@CoreTypeNotApplicable:1") != std::string::npos);
    CHECK(sparseLog.find("aic_read_close_victim@EventMissing:1") != std::string::npos);
    CHECK(sparseLog.find("aic_read_close_victim:1") != std::string::npos);
    CHECK(sparseLog.find("aiv_time(us):1") != std::string::npos);
    const std::string sparseL2 = ReadFile(sparseDirectory / "L2Cache.csv");
    CHECK(CsvValue(sparseDirectory / "L2Cache.csv", "cube5", "aic_time(us)") == "1.000000");
    CHECK(CsvValue(sparseDirectory / "L2Cache.csv", "cube5", "aic_read_close_hit") == "10");
    CHECK(CsvValue(sparseDirectory / "L2Cache.csv", "cube5", "aic_read_close_miss") == "0");
    boost::filesystem::remove_all(sparseDirectory);

    const auto blockedDirectory =
        boost::filesystem::temp_directory_path() /
        ("npu_compute_csv_blocked_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream blockedOutput(blockedDirectory.string());
        blockedOutput << "not a directory\n";
    }
    config.outputDirectory = blockedDirectory.string();
    std::string filesystemLog;
    aclptiResult filesystemStatus = ACLPTI_SUCCESS;
    CHECK(CapturePlog(
        [&] { filesystemStatus = npucompute::WritePmuCsv(sparseResult, {"L2Cache"}, config); }, &filesystemLog));
    CHECK(filesystemStatus == ACLPTI_ERROR_CSV_WRITE);
    CHECK(filesystemLog.find("CSV write rejected: output path is not a directory") != std::string::npos);
    CHECK(filesystemLog.find("path=" + blockedDirectory.string()) != std::string::npos);
    CHECK(filesystemLog.find("reason=") != std::string::npos);
    boost::filesystem::remove(blockedDirectory);

    config.outputDirectory = "relative_csv_output";
    std::string relativeLog;
    aclptiResult relativeStatus = ACLPTI_SUCCESS;
    CHECK(CapturePlog(
        [&] { relativeStatus = npucompute::WritePmuCsv(sparseResult, {"L2Cache"}, config); }, &relativeLog));
    CHECK(relativeStatus == ACLPTI_ERROR_INVALID_PARAMETER);
    CHECK(relativeLog.find("CSV write rejected: output path must be absolute") != std::string::npos);
    CHECK(relativeLog.find("path=relative_csv_output") != std::string::npos);
    if (hadPreviousDebug) {
        CHECK(setenv("NPU_COMPUTE_DEBUG", previousDebugValue.c_str(), 1) == 0);
    } else {
        CHECK(unsetenv("NPU_COMPUTE_DEBUG") == 0);
    }

    const auto emptyDirectory =
        boost::filesystem::temp_directory_path() /
        ("npu_compute_csv_empty_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(boost::filesystem::create_directories(emptyDirectory));
    const boost::filesystem::path existingCsv = emptyDirectory / "L2Cache.csv";
    {
        std::ofstream existingOutput(existingCsv.string());
        existingOutput << "sentinel\n";
    }
    aclptiProfilingDataResult emptyResult;
    config.outputDirectory = emptyDirectory.string();
    CHECK(npucompute::WritePmuCsv(emptyResult, {"L2Cache"}, config) == ACLPTI_SUCCESS);
    CHECK(ReadFile(existingCsv) == "sentinel\n");

    aclptiProfilingDataResult failedEmptyResult;
    failedEmptyResult.status = ACLPTI_ERROR_DECODE;
    failedEmptyResult.errorStats.failedRecordCount = 1;
    CHECK(npucompute::WritePmuCsv(failedEmptyResult, {"L2Cache"}, config) == ACLPTI_SUCCESS);
    CHECK(ReadFile(existingCsv) == "sentinel\n");

    boost::filesystem::remove_all(emptyDirectory);

    CHECK(npucompute::ReportConfig{}.outputDirectory.empty());
    config.outputDirectory.clear();
    CHECK(setenv("NPU_COMPUTE_DEBUG", "1", 1) == 0);
    std::string emptyOutputLog;
    aclptiResult emptyOutputStatus = ACLPTI_SUCCESS;
    CHECK(CapturePlog(
        [&] { emptyOutputStatus = npucompute::WritePmuCsv(sparseResult, {"L2Cache"}, config); }, &emptyOutputLog));
    CHECK(emptyOutputStatus == ACLPTI_ERROR_INVALID_PARAMETER);
    CHECK(emptyOutputLog.find("CSV write rejected: output path is empty") != std::string::npos);
    if (hadPreviousDebug) {
        CHECK(setenv("NPU_COMPUTE_DEBUG", previousDebugValue.c_str(), 1) == 0);
    } else {
        CHECK(unsetenv("NPU_COMPUTE_DEBUG") == 0);
    }

    // Atlas 350 的时间始终为 cycles / frequency，与 block 数和硬件核数无关。
    {
        const auto s1Dir = boost::filesystem::temp_directory_path() /
                           ("npu_compute_csv_block_scale1_" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        aclptiProfilingDataResult s1Result;
        for (uint16_t i = 0; i < 2; ++i) {
            aclptiPmuDataRow row{};
            row.blockId = i;
            row.subBlockId = 0;
            row.coreType = ACLPTI_CORE_TYPE_AIC;
            row.coreId = 0;
            row.coreData.push_back(Core(ACLPTI_CORE_TYPE_AIC, 0, 4000.0, {}));
            s1Result.pmuLogs.emplace(aclptiBlockKey{i, 0, ACLPTI_CORE_TYPE_AIC, 0}, row);
        }
        npucompute::ReportConfig s1Config;
        s1Config.outputDirectory = s1Dir.string();
        s1Config.frequencyMhz = 1000.0;
        CHECK(npucompute::WritePmuCsv(s1Result, {"L2Cache"}, s1Config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(s1Dir / "L2Cache.csv", "cube0", "aic_time(us)") == "4.000000");
        boost::filesystem::remove_all(s1Dir);
    }

    // 单 block 同样不引入时间缩放。
    {
        const auto s2Dir = boost::filesystem::temp_directory_path() /
                           ("npu_compute_csv_block_scale2_" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        aclptiProfilingDataResult s2Result;
        aclptiPmuDataRow s2Row{};
        s2Row.blockId = 5;
        s2Row.subBlockId = 0;
        s2Row.coreType = ACLPTI_CORE_TYPE_AIC;
        s2Row.coreId = 0;
        s2Row.coreData.push_back(Core(ACLPTI_CORE_TYPE_AIC, 0, 4000.0, {}));
        s2Result.pmuLogs.emplace(aclptiBlockKey{5, 0, ACLPTI_CORE_TYPE_AIC, 0}, s2Row);
        npucompute::ReportConfig s2Config;
        s2Config.outputDirectory = s2Dir.string();
        s2Config.frequencyMhz = 1000.0;
        CHECK(npucompute::WritePmuCsv(s2Result, {"L2Cache"}, s2Config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(s2Dir / "L2Cache.csv", "cube0", "aic_time(us)") == "4.000000");
        boost::filesystem::remove_all(s2Dir);
    }

    // 多 block 逐行保留各自的 cycles / frequency。
    {
        const auto s3Dir = boost::filesystem::temp_directory_path() /
                           ("npu_compute_csv_block_scale3_" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        aclptiProfilingDataResult s3Result;
        for (uint16_t i = 0; i < 3; ++i) {
            aclptiPmuDataRow row{};
            row.blockId = i;
            row.subBlockId = 0;
            row.coreType = ACLPTI_CORE_TYPE_AIC;
            row.coreId = 0;
            row.coreData.push_back(Core(ACLPTI_CORE_TYPE_AIC, 0, 3000.0, {}));
            s3Result.pmuLogs.emplace(aclptiBlockKey{i, 0, ACLPTI_CORE_TYPE_AIC, 0}, row);
        }
        npucompute::ReportConfig s3Config;
        s3Config.outputDirectory = s3Dir.string();
        s3Config.frequencyMhz = 1000.0;
        CHECK(npucompute::WritePmuCsv(s3Result, {"L2Cache"}, s3Config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(s3Dir / "L2Cache.csv", "cube0", "aic_time(us)") == "3.000000");
        boost::filesystem::remove_all(s3Dir);
    }

    // AIV 使用同一 Atlas 350 时间公式。
    {
        const auto s4Dir = boost::filesystem::temp_directory_path() /
                           ("npu_compute_csv_block_scale4_" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        aclptiProfilingDataResult s4Result;
        aclptiPmuDataRow s4Row{};
        s4Row.blockId = 10;
        s4Row.subBlockId = 0;
        s4Row.coreType = ACLPTI_CORE_TYPE_AIV;
        s4Row.coreId = 0;
        s4Row.coreData.push_back(Core(ACLPTI_CORE_TYPE_AIV, 0, 4000.0, {}));
        s4Result.pmuLogs.emplace(aclptiBlockKey{10, 0, ACLPTI_CORE_TYPE_AIV, 0}, s4Row);
        npucompute::ReportConfig s4Config;
        s4Config.outputDirectory = s4Dir.string();
        s4Config.frequencyMhz = 1000.0;
        CHECK(npucompute::WritePmuCsv(s4Result, {"L2Cache"}, s4Config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(s4Dir / "L2Cache.csv", "vector0", "aiv_time(us)") == "4.000000");
        boost::filesystem::remove_all(s4Dir);
    }

    // Pure AIV kernels use each block's PMU cycles even when task logs are available.
    {
        const auto pureAivDir = boost::filesystem::temp_directory_path() /
                                ("npu_compute_csv_pure_aiv_duration_" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        aclptiProfilingDataResult pureAivResult;
        for (uint16_t blockId = 0; blockId < 2; ++blockId) {
            aclptiPmuDataRow row{};
            row.blockId = blockId;
            row.subBlockId = blockId;
            row.coreType = ACLPTI_CORE_TYPE_AIV;
            row.coreId = static_cast<uint8_t>(blockId);
            row.coreData.push_back(Core(
                ACLPTI_CORE_TYPE_AIV, static_cast<uint8_t>(blockId), 1000.0 * static_cast<double>(blockId + 1),
                {{0x422U, 100.0}, {0x571U, 4.0}, {0x57fU, 0.0}, {0x580U, 0.0}}));
            pureAivResult.pmuLogs.emplace(
                aclptiBlockKey{blockId, blockId, ACLPTI_CORE_TYPE_AIV, static_cast<uint8_t>(blockId)}, row);
        }
        pureAivResult.taskLogs.emplace(
            1, std::vector<aclptiTaskLogRow>{{1, 0x00U, 1, 2, 1000}, {1, 0x01U, 1, 2, 5000}});

        npucompute::ReportConfig config;
        config.outputDirectory = pureAivDir.string();
        config.frequencyMhz = 1000.0;
        CHECK(npucompute::WritePmuCsv(pureAivResult, {"Memory", "MemoryUB"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(pureAivDir / "Memory.csv", "vector0", "aiv_gm_to_ub_bw(GB/s)") == "11.920929");
        CHECK(CsvValue(pureAivDir / "Memory.csv", "vector1", "aiv_gm_to_ub_bw(GB/s)") == "5.960464");
        CHECK(CsvValue(pureAivDir / "MemoryUB.csv", "vector0", "aiv_ub_read_bw_vector(GB/s)") == "0.953674");
        CHECK(CsvValue(pureAivDir / "MemoryUB.csv", "vector1", "aiv_ub_read_bw_vector(GB/s)") == "0.476837");
        boost::filesystem::remove_all(pureAivDir);
    }

    // Pure AIC kernels use each block's PMU cycles even when task logs are available.
    {
        const auto pureAicDir = boost::filesystem::temp_directory_path() /
                                ("npu_compute_csv_pure_aic_duration_" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        aclptiProfilingDataResult pureAicResult;
        for (uint16_t blockId = 0; blockId < 2; ++blockId) {
            aclptiPmuDataRow row{};
            row.blockId = blockId;
            row.subBlockId = blockId;
            row.coreType = ACLPTI_CORE_TYPE_AIC;
            row.coreId = static_cast<uint8_t>(blockId);
            row.coreData.push_back(Core(
                ACLPTI_CORE_TYPE_AIC, static_cast<uint8_t>(blockId), 1000.0 * static_cast<double>(blockId + 1),
                {{0x304U, 16.0}, {0x707U, 4.0}}));
            pureAicResult.pmuLogs.emplace(
                aclptiBlockKey{blockId, blockId, ACLPTI_CORE_TYPE_AIC, static_cast<uint8_t>(blockId)}, row);
        }
        pureAicResult.taskLogs.emplace(
            1, std::vector<aclptiTaskLogRow>{{1, 0x00U, 1, 2, 1000}, {1, 0x01U, 1, 2, 5000}});

        npucompute::ReportConfig config;
        config.outputDirectory = pureAicDir.string();
        config.frequencyMhz = 1000.0;
        CHECK(npucompute::WritePmuCsv(pureAicResult, {"Memory", "MemoryL0"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(pureAicDir / "Memory.csv", "cube0", "aic_l1_read_bw(GB/s)") == "0.953674");
        CHECK(CsvValue(pureAicDir / "Memory.csv", "cube1", "aic_l1_read_bw(GB/s)") == "0.476837");
        CHECK(CsvValue(pureAicDir / "MemoryL0.csv", "cube0", "aic_l0a_read_bw(GB/s)") == "0.953674");
        CHECK(CsvValue(pureAicDir / "MemoryL0.csv", "cube1", "aic_l0a_read_bw(GB/s)") == "0.476837");
        boost::filesystem::remove_all(pureAicDir);
    }

    // Atlas 350 deterministic formulas and formatting across all five supported sections.
    {
        const auto formulaDir = boost::filesystem::temp_directory_path() /
                                ("npu_compute_csv_a5_formula_" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        aclptiProfilingDataResult formulaResult;
        aclptiPmuDataRow formulaAic{};
        formulaAic.blockId = 20;
        formulaAic.subBlockId = 7;
        formulaAic.coreType = ACLPTI_CORE_TYPE_AIC;
        formulaAic.coreId = 9;
        formulaAic.coreData.push_back(Core(
            ACLPTI_CORE_TYPE_AIC, 9, 1000.0,
            {{1, 100},   {52, 20},  {53, 5},   {512, 7},   {513, 8},    {514, 100}, {515, 50}, {769, 999},
             {772, 1},   {774, 1},  {776, 1},  {778, 1},   {810, 250},  {1058, 4},  {1059, 3}, {1060, 10},
             {1061, 2},  {1062, 3}, {1063, 5}, {1064, 4},  {1065, 1},   {1066, 0},  {1067, 0}, {1068, 0},
             {1069, 0},  {1070, 0}, {1071, 0}, {1792, 7},  {1794, 200}, {1795, 1},  {1797, 1}, {1799, 4},
             {1801, 10}, {1804, 1}, {1806, 2}, {1812, 25}, {1815, 1}}));
        aclptiPmuDataRow formulaAiv{};
        formulaAiv.blockId = 20;
        formulaAiv.subBlockId = 7;
        formulaAiv.coreType = ACLPTI_CORE_TYPE_AIV;
        formulaAiv.coreId = 11;
        formulaAiv.coreData.push_back(Core(
            ACLPTI_CORE_TYPE_AIV, 11, 2000.0,
            {{1, 100},
             {52, 40},
             {53, 10},
             {512, 11},
             {513, 12},
             {514, 200},
             {515, 100},
             {1058, 100},
             {1059, 20},
             {1281, 500},
             {1393, 2},
             {1394, 4},
             {1407, 10},
             {1408, 20}}));
        formulaResult.pmuLogs.emplace(aclptiBlockKey{20, 7, ACLPTI_CORE_TYPE_AIC, 9}, formulaAic);
        formulaResult.pmuLogs.emplace(aclptiBlockKey{20, 7, ACLPTI_CORE_TYPE_AIV, 11}, formulaAiv);

        npucompute::ReportConfig formulaConfig;
        formulaConfig.outputDirectory = formulaDir.string();
        formulaConfig.frequencyMhz = 1000.0;
        formulaConfig.socName = "950X";
        CHECK(
            npucompute::WritePmuCsv(
                formulaResult, {"L2Cache", "Memory", "MemoryL0", "MemoryUB", "PipeUtilization"}, formulaConfig) ==
            ACLPTI_SUCCESS);

        CHECK(CsvValue(formulaDir / "L2Cache.csv", "cube7", "aic_time(us)") == "1.000000");
        CHECK(CsvValue(formulaDir / "L2Cache.csv", "cube7", "aic_total_cycles") == "1000");
        CHECK(CsvValue(formulaDir / "L2Cache.csv", "cube7", "aic_read_close_hit") == "10");
        CHECK(CsvValue(formulaDir / "L2Cache.csv", "cube7", "aic_read_hit_rate(%)") == "60.000000");
        CHECK(CsvValue(formulaDir / "L2Cache.csv", "cube7", "aic_write_hit_rate(%)") == "0.000000");
        CHECK(CsvValue(formulaDir / "L2Cache.csv", "cube7", "aiv_time(us)") == "NA");

        CHECK(CsvValue(formulaDir / "Memory.csv", "cube7", "aic_l1_read_bw(GB/s)") == "0.953674");
        CHECK(CsvValue(formulaDir / "Memory.csv", "cube7", "aic_mte1_instructions") == "7");
        CHECK(CsvValue(formulaDir / "Memory.csv", "cube7", "aic_mte1_ratio") == "0.200000");
        CHECK(CsvValue(formulaDir / "Memory.csv", "cube7", "read_main_memory_datas(KB)") == "0.500000");
        CHECK(CsvValue(formulaDir / "Memory.csv", "vector7", "read_main_memory_datas(KB)") == "12.500000");
        CHECK(CsvValue(formulaDir / "Memory.csv", "vector7", "aiv_gm_to_ub_bw(GB/s)") == "4.172325");
        CHECK(CsvValue(formulaDir / "Memory.csv", "vector7", "GM_to_UB_datas(KB)") == "8.750000");
        CHECK(CsvValue(formulaDir / "Memory.csv", "vector7", "UB_to_GM_datas(KB)") == "NA");

        const boost::filesystem::path formula959Dir = formulaDir.string() + "_9599";
        formulaConfig.outputDirectory = formula959Dir.string();
        formulaConfig.socName = "Ascend950PR_9599";
        CHECK(npucompute::WritePmuCsv(formulaResult, {"Memory"}, formulaConfig) == ACLPTI_SUCCESS);
        for (const auto& cell : std::array<std::pair<const char*, const char*>, 8>{
                 std::make_pair("cube7", "read_main_memory_datas(KB)"),
                 std::make_pair("cube7", "aic_main_mem_read_bw(GB/s)"),
                 std::make_pair("cube7", "GM_to_L1_datas(KB)"),
                 std::make_pair("cube7", "L0C_to_L1_datas(KB)"),
                 std::make_pair("cube7", "L0C_to_GM_datas(KB)"),
                 std::make_pair("vector7", "read_main_memory_datas(KB)"),
                 std::make_pair("vector7", "aiv_main_mem_read_bw(GB/s)"),
                 std::make_pair("vector7", "GM_to_UB_datas(KB)"),
             }) {
            const auto& row = cell.first;
            const auto& column = cell.second;
            CHECK(
                CsvValue(formula959Dir / "Memory.csv", row, column) ==
                CsvValue(formulaDir / "Memory.csv", row, column));
        }
        CHECK(CsvValue(formulaDir / "Memory.csv", "cube7", "GM_to_L1_bw_usage_rate(%)") == "0.292952");
        CHECK(CsvValue(formula959Dir / "Memory.csv", "cube7", "GM_to_L1_bw_usage_rate(%)") == "0.268535");
        CHECK(CsvValue(formulaDir / "Memory.csv", "cube7", "L0C_to_L1_bw_usage_rate(%)") == "0.063107");
        CHECK(CsvValue(formula959Dir / "Memory.csv", "cube7", "L0C_to_L1_bw_usage_rate(%)") == "0.057848");
        CHECK(CsvValue(formulaDir / "Memory.csv", "cube7", "L0C_to_GM_bw_usage_rate(%)") == "0.274655");
        CHECK(CsvValue(formula959Dir / "Memory.csv", "cube7", "L0C_to_GM_bw_usage_rate(%)") == "0.251762");
        CHECK(CsvValue(formulaDir / "Memory.csv", "vector7", "GM_to_UB_bw_usage_rate(%)") == "2.557669");
        CHECK(CsvValue(formula959Dir / "Memory.csv", "vector7", "GM_to_UB_bw_usage_rate(%)") == "2.344530");
        boost::filesystem::remove_all(formula959Dir);
        formulaConfig.outputDirectory = formulaDir.string();
        formulaConfig.socName = "950X";

        CHECK(CsvValue(formulaDir / "MemoryL0.csv", "cube7", "aic_l0a_read_bw(GB/s)") == "0.059605");
        CHECK(CsvValue(formulaDir / "MemoryL0.csv", "cube7", "aic_l0c_write_bw_cube(GB/s)") == "0.953674");
        CHECK(CsvValue(formulaDir / "MemoryUB.csv", "vector7", "aiv_ub_read_bw_vector(GB/s)") == "0.238419");
        CHECK(CsvValue(formulaDir / "MemoryUB.csv", "vector7", "aiv_ub_read_bw_gm(GB/s)") == "NA");
        CHECK(CsvValue(formulaDir / "MemoryUB.csv", "vector7", "aiv_ub_write_bw_gm(GB/s)") == "4.172325");

        CHECK(CsvValue(formulaDir / "PipeUtilization.csv", "cube7", "aic_cube_time(us)") == "0.250000");
        CHECK(CsvValue(formulaDir / "PipeUtilization.csv", "cube7", "aic_cube_ratio") == "0.250000");
        CHECK(CsvValue(formulaDir / "PipeUtilization.csv", "cube7", "aic_mte1_active_bw(GB/s)") == "4.768372");
        CHECK(CsvValue(formulaDir / "PipeUtilization.csv", "cube7", "aic_mte3_active_bw(GB/s)") == "28.610229");
        CHECK(CsvValue(formulaDir / "PipeUtilization.csv", "vector7", "aiv_vec_time(us)") == "0.500000");
        CHECK(CsvValue(formulaDir / "PipeUtilization.csv", "vector7", "aiv_mte2_active_bw(GB/s)") == "41.723251");
        CHECK(CsvValue(formulaDir / "PipeUtilization.csv", "vector7", "aiv_mte3_active_bw(GB/s)") == "NA");

        formulaResult.taskLogs.emplace(
            1, std::vector<aclptiTaskLogRow>{{1, 0x00U, 1, 2, 1000}, {1, 0x01U, 1, 2, 3000}});
        formulaResult.taskLogs.emplace(
            2, std::vector<aclptiTaskLogRow>{{2, 0x00U, 1, 2, 1000}, {2, 0x01U, 1, 2, 5000}});
        formulaResult.taskLogs.emplace(
            3, std::vector<aclptiTaskLogRow>{{3, 0x00U, 1, 2, 1000}, {3, 0x01U, 1, 2, 7000}});
        const boost::filesystem::path taskDurationDir = formulaDir.string() + "_task_duration";
        formulaConfig.outputDirectory = taskDurationDir.string();
        CHECK(
            npucompute::WritePmuCsv(
                formulaResult, {"Memory", "MemoryL0", "MemoryUB", "PipeUtilization"}, formulaConfig) == ACLPTI_SUCCESS);
        CHECK(CsvValue(taskDurationDir / "Memory.csv", "cube7", "aic_l1_read_bw(GB/s)") == "0.238419");
        CHECK(CsvValue(taskDurationDir / "Memory.csv", "vector7", "aiv_gm_to_ub_bw(GB/s)") == "2.086163");
        CHECK(CsvValue(taskDurationDir / "MemoryL0.csv", "cube7", "aic_l0a_read_bw(GB/s)") == "0.014901");
        CHECK(CsvValue(taskDurationDir / "MemoryUB.csv", "vector7", "aiv_ub_read_bw_vector(GB/s)") == "0.119209");
        CHECK(CsvValue(taskDurationDir / "MemoryUB.csv", "vector7", "aiv_ub_write_bw_gm(GB/s)") == "2.086163");
        CHECK(CsvValue(taskDurationDir / "PipeUtilization.csv", "cube7", "aic_mte1_active_bw(GB/s)") == "4.768372");
        boost::filesystem::remove_all(taskDurationDir);
        boost::filesystem::remove_all(formulaDir);
    }

    // CSV always uses block counters, regardless of the presence or values of task counters.
    {
        const auto directory = boost::filesystem::temp_directory_path() /
                               boost::filesystem::unique_path("npu_compute_csv_block_source_%%%%-%%%%");
        const std::vector<std::string> sections = {"L2Cache", "Memory", "MemoryL0", "MemoryUB", "PipeUtilization"};
        aclptiProfilingDataResult mixedSourceResult;
        aclptiPmuDataRow blockRow{};
        blockRow.blockId = 7;
        blockRow.subBlockId = 3;
        blockRow.coreType = ACLPTI_CORE_TYPE_AIC;
        blockRow.coreId = 1;
        blockRow.coreData.push_back(Core(ACLPTI_CORE_TYPE_AIC, 1, 1000.0, {}));
        mixedSourceResult.pmuLogs.emplace(aclptiBlockKey{7, 3, ACLPTI_CORE_TYPE_AIC, 1}, blockRow);
        mixedSourceResult.taskPmuData.aic = aclptiTaskPmuCoreData{2000, false, {}};

        npucompute::ReportConfig config;
        config.outputDirectory = (directory / "mixed").string();
        CHECK(npucompute::WritePmuCsv(mixedSourceResult, sections, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(directory / "mixed/L2Cache.csv", "cube3", "block_id") == "7");
        CHECK(CsvValue(directory / "mixed/L2Cache.csv", "cube3", "aic_total_cycles") == "1000");

        auto changedTaskResult = mixedSourceResult;
        changedTaskResult.taskPmuData.aic->totalCycles = 9000;
        config.outputDirectory = (directory / "changed-task").string();
        CHECK(npucompute::WritePmuCsv(changedTaskResult, sections, config) == ACLPTI_SUCCESS);
        auto blockOnlyResult = mixedSourceResult;
        blockOnlyResult.taskPmuData = {};
        config.outputDirectory = (directory / "block-only").string();
        CHECK(npucompute::WritePmuCsv(blockOnlyResult, sections, config) == ACLPTI_SUCCESS);
        for (const auto& section : sections) {
            const auto filename = section + ".csv";
            CHECK(CsvHeader(directory / "mixed" / filename) == ExpectedA5Header(section));
            CHECK(ReadFile(directory / "mixed" / filename) == ReadFile(directory / "changed-task" / filename));
            CHECK(ReadFile(directory / "mixed" / filename) == ReadFile(directory / "block-only" / filename));
        }

        auto taskOnlyResult = mixedSourceResult;
        taskOnlyResult.pmuLogs.clear();
        config.outputDirectory = (directory / "task-only").string();
        CHECK(npucompute::WritePmuCsv(taskOnlyResult, sections, config) == ACLPTI_SUCCESS);
        CHECK(!boost::filesystem::exists(config.outputDirectory));
        boost::filesystem::remove_all(directory);
    }

    // SIMD/SIMT VF metrics retain fixed CSV columns and always use block PMU data.
    {
        const auto directory =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("vf-ipc-%%%%-%%%%");
        npucompute::ReportConfig config;
        config.outputDirectory = directory.string();
        config.frequencyMhz = 800;
        config.aivFrequencyMhz = 1000;
        config.fixedOutputDirectory = true;

        // Give block and task rows different instruction counts to detect source selection errors.
        aclptiProfilingDataResult result;
        for (const auto type : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
            aclptiPmuDataRow row{};
            row.coreType = type;
            row.coreData.push_back(Core(
                type, 0, 1000,
                {{1281, 800}, {1282, 200}, {1284, 100}, {1320, 500}, {1466, 40}, {1467, 80}, {1468, 120}}));
            result.pmuLogs.emplace(aclptiBlockKey{0, 0, type, 0}, row);
            row.coreData.front().values[1320] = 100;
            row.coreData.front().values[1466] = 80;
            const auto& core = row.coreData.front();
            auto& taskCore = type == ACLPTI_CORE_TYPE_AIC ? result.taskPmuData.aic : result.taskPmuData.aiv;
            taskCore = aclptiTaskPmuCoreData{core.totalCycles, core.overflow, core.values};
        }

        // Renamed ratio columns replace the old names; the four new fields are appended in order.
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        const auto csv = directory / "ArithmeticUtilization.csv";
        const auto header = CsvHeader(csv);
        CHECK(header.size() == 20U);
        const std::vector<std::string> vfFields = {
            "aiv_simd_vf_time", "aiv_simd_vf_ipc", "aiv_simt_vf_time", "aiv_simt_vf_ipc"};
        CHECK(std::equal(vfFields.begin(), vfFields.end(), header.end() - vfFields.size()));
        CHECK(std::count(header.begin(), header.end(), "aiv_simd_ipc") == 0);
        CHECK(std::count(header.begin(), header.end(), "aiv_vec_vf_ratio") == 0);
        CHECK(std::count(header.begin(), header.end(), "aiv_vec_simt_vf_ratio") == 0);

        // Ratios use total cycles; times use the AIV frequency; IPCs are 500/200 and (40+80+120)/100/4.
        for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
                 {"aiv_simd_vf_ratio", "0.200000"},
                 {"aiv_simt_vf_ratio", "0.100000"},
                 {"aiv_simd_vf_time", "0.200000"},
                 {"aiv_simd_vf_ipc", "2.500000"},
                 {"aiv_simt_vf_time", "0.100000"},
                 {"aiv_simt_vf_ipc", "0.600000"}}) {
            CHECK(CsvValue(csv, "vector0", field) == value);
        }

        // AIC rows retain the VF columns with NA even when their input contains the same event IDs.
        for (const auto& field : vfFields) {
            CHECK(CsvValue(csv, "cube0", field) == "NA");
        }

        // Removing task counters must not change CSV values or its block schema.
        result.taskPmuData = {};
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvHeader(csv) == header);
        const auto blockCsv = ReadFile(csv);
        CHECK(std::count(blockCsv.begin(), blockCsv.end(), '\n') == 3);
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ipc") == "2.500000");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ipc") == "0.600000");

        // Missing instruction counts yield NA; SIMT must not use a partial sum of its three events.
        auto& core = result.pmuLogs.at({0, 0, ACLPTI_CORE_TYPE_AIV, 0}).coreData.front();
        const auto complete = core.values;
        for (const auto missing : {1320U, 1466U, 1467U, 1468U}) {
            core.values = complete;
            core.values.erase(missing);
            CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
            CHECK(CsvValue(csv, "vector0", missing == 1320U ? "aiv_simd_vf_ipc" : "aiv_simt_vf_ipc") == "NA");
        }

        // Missing event 1281 does not affect VF ratios.
        core.values = complete;
        core.values.erase(1281);
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ratio") == "0.200000");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ratio") == "0.100000");

        // Missing SIMD cycle event 1282 makes SIMD ratio, time and IPC unavailable.
        core.values = complete;
        core.values.erase(1282);
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ratio") == "NA");
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_time") == "NA");
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ipc") == "NA");

        // Missing SIMT cycle event 1284 makes SIMT ratio, time and IPC unavailable.
        core.values = complete;
        core.values.erase(1284);
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ratio") == "NA");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_time") == "NA");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ipc") == "NA");

        // Zero SIMD cycles produce zero ratio and time; IPC remains unavailable.
        core.values = complete;
        core.values[1282] = 0;
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ratio") == "0.000000");
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_time") == "0.000000");
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ipc") == "NA");

        // Zero SIMT cycles produce zero ratio and time; IPC remains unavailable.
        core.values = complete;
        core.values[1284] = 0;
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ratio") == "0.000000");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_time") == "0.000000");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ipc") == "NA");

        // Zero active cycles (1281) do not change ratios whose denominator is total cycles.
        core.values = complete;
        core.values[1281] = 0;
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ratio") == "0.200000");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ratio") == "0.100000");

        // Collected instruction counts of zero are valid and produce zero IPCs.
        core.values = complete;
        core.values[1320] = 0;
        core.values[1466] = 0;
        core.values[1467] = 0;
        core.values[1468] = 0;
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvValue(csv, "vector0", "aiv_simd_vf_ipc") == "0.000000");
        CHECK(CsvValue(csv, "vector0", "aiv_simt_vf_ipc") == "0.000000");

        // An AIC-only collection keeps the same header and renders all AIV VF fields as NA.
        result.pmuLogs.erase({0, 0, ACLPTI_CORE_TYPE_AIV, 0});
        CHECK(npucompute::WritePmuCsv(result, {"ArithmeticUtilization"}, config) == ACLPTI_SUCCESS);
        const auto aicHeader = CsvHeader(csv);
        CHECK(aicHeader == header);
        CHECK(CsvValue(csv, "cube0", "aiv_simd_vf_ratio") == "NA");
        CHECK(CsvValue(csv, "cube0", "aiv_simt_vf_ratio") == "NA");
        for (const auto& field : vfFields) {
            CHECK(CsvValue(csv, "cube0", field) == "NA");
        }

        // ArithmeticUtilization additions do not alter the PipeUtilization CSV schema.
        CHECK(npucompute::WritePmuCsv(result, {"PipeUtilization"}, config) == ACLPTI_SUCCESS);
        CHECK(CsvHeader(directory / "PipeUtilization.csv") == ExpectedA5Header("PipeUtilization"));

        boost::filesystem::remove_all(directory);
    }

    return 0;
}

TEST(NpuComputePmuCsvWriter, Main) { ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputePmuCsvWriter reported failure"; }
