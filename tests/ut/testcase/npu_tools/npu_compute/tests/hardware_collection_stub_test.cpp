/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "test_process.h"
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::size_t CountOccurrences(const std::string& text, const std::string& marker)
{
    std::size_t count = 0;
    for (std::size_t position = 0; (position = text.find(marker, position)) != std::string::npos;
         position += marker.size()) {
        ++count;
    }
    return count;
}

void CheckCallbackFlow(const std::string& log, const std::string& callback)
{
    EXPECT_EQ(CountOccurrences(log, "[acl_pti_callback_stub] subscribe"), 1U) << log;
    const std::array<int, 9> callbacks = {13, 0, 16, 17, 18, 3, 12, 15, 14};
    std::vector<std::string> enabled;
    std::vector<std::string> disabled;
    std::istringstream input(log);
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("[acl_pti_callback_stub] enable=1") != std::string::npos) {
            enabled.push_back(line);
        }
        if (line.find("[acl_pti_callback_stub] enable=0") != std::string::npos) {
            disabled.push_back(line);
        }
    }
    ASSERT_EQ(enabled.size(), callbacks.size()) << log;
    ASSERT_EQ(disabled.size(), callbacks.size()) << log;
    for (std::size_t index = 0; index < callbacks.size(); ++index) {
        EXPECT_NE(enabled[index].find("cbid=" + std::to_string(callbacks[index]) + " result=0"), std::string::npos)
            << enabled[index];
        EXPECT_NE(
            disabled[index].find("cbid=" + std::to_string(callbacks[callbacks.size() - index - 1]) + " result=0"),
            std::string::npos)
            << disabled[index];
    }
    const std::string event =
        "[acl_pti_callback_stub] event domain=1 cbid=" + callback + " site=1 retval=0 dispatched=1";
    EXPECT_NE(log.find(event), std::string::npos) << log;
    EXPECT_EQ(CountOccurrences(log, "[acl_pti_callback_stub] event domain=1"), 1U) << log;
    EXPECT_NE(log.find("runtime callback domain=1 cbid=" + callback + " site=1 retval=0 accepted=1"), std::string::npos)
        << log;
    const std::string published =
        "[hardware_collection_stub_app] HardwareInfo.jsonl available after callback cbid=" + callback;
    const std::string cleaned = "[hardware_collection_stub_app] Device cleanup completed";
    ASSERT_EQ(CountOccurrences(log, published), 1U) << log;
    ASSERT_EQ(CountOccurrences(log, cleaned), 1U) << log;
    EXPECT_LT(log.find(published), log.find(cleaned));
    EXPECT_EQ(log.find("[libnpu-compute] HardwareInfo:"), std::string::npos) << log;
}

void CheckStubLibrarySources(const boost::filesystem::path& output)
{
    std::vector<boost::filesystem::path> loaded;
    unsigned loaderLogs = 0;
    for (const auto& entry : boost::filesystem::directory_iterator(output)) {
        if (entry.path().filename().string().find("loader.") != 0) {
            continue;
        }
        ++loaderLogs;
        std::ifstream input(entry.path().string());
        ASSERT_TRUE(input.is_open());
        std::string line;
        while (std::getline(input, line)) {
            const std::string marker = "calling init: ";
            const auto position = line.find(marker);
            if (position == std::string::npos) {
                continue;
            }
            const auto begin = line.find_first_not_of(" \t", position + marker.size());
            if (begin == std::string::npos || line[begin] != '/') {
                continue;
            }
            const auto end = line.find_last_not_of(" \t\r");
            loaded.push_back(boost::filesystem::canonical(line.substr(begin, end - begin + 1)));
        }
    }
    ASSERT_GT(loaderLogs, 0U);
    ASSERT_FALSE(loaded.empty());
    const auto buildRoot =
        boost::filesystem::canonical(boost::filesystem::path(NPU_COMPUTE_HW_APP_PATH).parent_path().parent_path())
            .string() +
        "/";
    for (const std::string name :
         {"libruntime.so", "libprofapi.so", "libnpu_compute_hardware_api_stub.so", "libacl_pti_callback_stub.so"}) {
        bool found = false;
        for (const auto& path : loaded) {
            const std::string filename = path.filename().string();
            if (filename == name || filename.find(name + ".") == 0) {
                found = true;
                EXPECT_EQ(path.string().find(buildRoot), 0U) << name << " loaded from " << path;
            }
        }
        EXPECT_TRUE(found) << "Test stub was not loaded: " << name;
    }
    for (const auto& path : loaded) {
        for (const std::string name : {"libascendcl.so", "libplatform.so", "libascend_hal.so", "libdrvdsmi_host.so"}) {
            const std::string filename = path.filename().string();
            EXPECT_FALSE(filename == name || filename.find(name + ".") == 0)
                << "Real device library loaded in stub test: " << path;
        }
    }
}

// property_tree erases JSON scalar types, so check the token as well as the parsed value.
char ScalarToken(const std::string& line, const std::string& key)
{
    const auto keyPosition = line.find("\"" + key + "\"");
    if (keyPosition == std::string::npos) {
        return '\0';
    }
    const auto colon = line.find(':', keyPosition + key.size() + 2);
    if (colon == std::string::npos) {
        return '\0';
    }
    const auto value = line.find_first_not_of(" \t\r\n", colon + 1);
    return value == std::string::npos ? '\0' : line[value];
}

void CheckNumber(
    const boost::property_tree::ptree& row, const std::string& line, const std::string& key, double expected)
{
    SCOPED_TRACE(key);
    EXPECT_NE(ScalarToken(line, key), '\0');
    EXPECT_NE(ScalarToken(line, key), '"');
    EXPECT_DOUBLE_EQ(row.get<double>(key), expected);
}

void CheckString(
    const boost::property_tree::ptree& row, const std::string& line, const std::string& key,
    const std::string& expected)
{
    SCOPED_TRACE(key);
    EXPECT_EQ(ScalarToken(line, key), '"');
    EXPECT_EQ(row.get<std::string>(key), expected);
}

void CheckHardwareInfo(const boost::filesystem::path& output)
{
    std::vector<boost::filesystem::path> files;
    for (const auto& entry : boost::filesystem::directory_iterator(output)) {
        const auto name = entry.path().filename().string();
        if (name.find("HardwareInfo") == 0 && entry.path().extension() == ".jsonl") {
            files.push_back(entry.path());
        }
    }
    ASSERT_EQ(files.size(), 1U);
    ASSERT_TRUE(boost::filesystem::is_regular_file(files.front()));
    ASSERT_FALSE(boost::filesystem::is_symlink(files.front()));
    std::ifstream input(files.front().string());
    ASSERT_TRUE(input.is_open());
    std::vector<boost::property_tree::ptree> records;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        boost::property_tree::ptree row;
        std::istringstream json(line);
        ASSERT_NO_THROW(boost::property_tree::read_json(json, row));
        records.push_back(row);
        lines.push_back(line);
    }
    ASSERT_FALSE(input.bad());
    ASSERT_EQ(records.size(), 5U);
    const std::array<std::string, 5> categories = {
        "Host Info", "Device Info", "CPU Information", "AI Core Information", "Memory Information"};
    const std::array<std::vector<std::string>, 5> keys = {
        {{"category", "cpu physical count", "cpu logical count", "memory total size(MB)", "disk total size(GB)"},
         {"category", "npu count", "chip info", "arch info"},
         {"category", "control cpu count", "ai cpu count", "ai cpu frequency(MHZ)"},
         {"category", "ai core count", "ai cube count", "ai vector count", "ai cube frequency(MHZ)",
          "ai vector frequency(MHZ)"},
         {"category", "hbm total(MB)", "hbm used(MB)", "hbm frequency(MHZ)"}}};
    for (std::size_t index = 0; index < records.size(); ++index) {
        SCOPED_TRACE(index);
        std::vector<std::string> actualKeys;
        for (const auto& field : records[index]) {
            actualKeys.push_back(field.first);
        }
        ASSERT_EQ(actualKeys, keys[index]);
        CheckString(records[index], lines[index], "category", categories[index]);
    }
    for (const auto& key : keys[0]) {
        if (key == "category") {
            continue;
        }
        SCOPED_TRACE(key);
        EXPECT_NE(ScalarToken(lines[0], key), '"');
        const auto value = records[0].get<double>(key);
        EXPECT_TRUE(std::isfinite(value));
        EXPECT_GE(value, 0);
    }
    CheckNumber(records[1], lines[1], "npu count", 1);
    CheckString(records[1], lines[1], "chip info", "Ascend950PR_9599 V100");
    CheckString(records[1], lines[1], "arch info", "3510");
    CheckNumber(records[2], lines[2], "control cpu count", 4);
    CheckNumber(records[2], lines[2], "ai cpu count", 8);
    CheckNumber(records[2], lines[2], "ai cpu frequency(MHZ)", 1500);
    CheckNumber(records[3], lines[3], "ai core count", 32);
    CheckNumber(records[3], lines[3], "ai cube count", 16);
    CheckNumber(records[3], lines[3], "ai vector count", 16);
    CheckNumber(records[3], lines[3], "ai cube frequency(MHZ)", 1650);
    CheckNumber(records[3], lines[3], "ai vector frequency(MHZ)", 1650);
    CheckNumber(records[4], lines[4], "hbm total(MB)", 65536);
    CheckNumber(records[4], lines[4], "hbm used(MB)", 16384);
    CheckNumber(records[4], lines[4], "hbm frequency(MHZ)", 3200);
}

} // namespace

class NpuComputeHardwareCollection : public testing::TestWithParam<std::string> {};

TEST_P(NpuComputeHardwareCollection, CollectsOrReportsSetupFailureAndCleansUp)
{
    npucompute::test::TemporaryDirectory temporary;
    const boost::filesystem::path output(temporary.Path());
    const std::string scenario = GetParam();
    const bool missingStub = scenario == "missing-stub";
    const bool missingLibrary = scenario == "missing-library";
    const std::string library = missingLibrary ? (output / "missing.so").string() : NPU_COMPUTE_HW_LIBRARY_PATH;
    const std::string callback = missingStub || missingLibrary ? "13" : scenario;
    npucompute::test::ProcessOptions options;
    options.arguments = {NPU_COMPUTE_HW_APP_PATH, library, output.string(), callback};
    options.environment["ACL_API_INJECTION"] = std::nullopt;
    options.environment["INJECTION_TEST_CALLBACK_EVENTS"] = std::nullopt;
    options.environment["NPU_COMPUTE_HARDWARE_API_STUB"] =
        missingStub ? std::nullopt : std::optional<std::string>(NPU_COMPUTE_HW_API_STUB_PATH);
    options.environment["LD_PRELOAD"] = options.environment["NPU_COMPUTE_HARDWARE_API_STUB"];
    options.environment["NPU_COMPUTE_DEBUG"] = "1";
    options.environment["LD_DEBUG"] = "libs";
    options.environment["LD_DEBUG_OUTPUT"] = (output / "loader").string();
    const auto result = npucompute::test::RunProcess(options);
    ASSERT_EQ(result.launchError, 0);
    ASSERT_FALSE(result.timedOut);
    ASSERT_EQ(result.signal, 0);
    ASSERT_EQ(
        result.exitCode, missingStub    ? 13 :
                         missingLibrary ? 6 :
                                          0)
        << result.standardOutput << result.standardError;
    const std::string& log = result.standardError;
    if (missingStub || missingLibrary) {
        EXPECT_NE(log.find(missingStub ? "hardware stub is required" : "dlopen failed"), std::string::npos) << log;
        if (missingLibrary) {
            EXPECT_NE(log.find("ACL cleanup attempted result=0"), std::string::npos) << log;
        }
        for (const auto& entry : boost::filesystem::directory_iterator(output)) {
            const auto name = entry.path().filename().string();
            EXPECT_FALSE(name.find("HardwareInfo") == 0 && entry.path().extension() == ".jsonl") << entry.path();
        }
        return;
    }
    ASSERT_NO_FATAL_FAILURE(CheckCallbackFlow(log, callback));
    ASSERT_NO_FATAL_FAILURE(CheckHardwareInfo(output));
    ASSERT_NO_FATAL_FAILURE(CheckStubLibrarySources(output));
}

INSTANTIATE_TEST_SUITE_P(
    Scenarios, NpuComputeHardwareCollection, testing::Values("13", "0", "16", "17", "missing-stub", "missing-library"),
    [](const testing::TestParamInfo<std::string>& info) {
        std::string name = info.param;
        for (char& character : name) {
            if (character == '-')
                character = '_';
        }
        return name;
    });
