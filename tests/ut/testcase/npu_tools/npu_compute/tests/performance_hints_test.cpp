/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/collection_file_validator.h"
#include "report/collection_processor.h"

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <vector>

namespace {
using namespace npucompute::cli;

class SummaryJsonlValidation : public ::testing::Test {
protected:
    boost::filesystem::path directory;
    std::optional<std::string> debugEnvironment;

    void SetUp() override
    {
        const char* debug = std::getenv("NPU_COMPUTE_DEBUG");
        if (debug != nullptr) {
            debugEnvironment = debug;
        }
        directory = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("hints-%%%%-%%%%");
        boost::filesystem::create_directories(directory);
    }

    void TearDown() override
    {
        if (debugEnvironment) {
            setenv("NPU_COMPUTE_DEBUG", debugEnvironment->c_str(), 1);
        } else {
            unsetenv("NPU_COMPUTE_DEBUG");
        }
        boost::filesystem::remove_all(directory);
    }

    void Write(const std::string& content) { std::ofstream(directory / "summary.jsonl") << content; }

    const std::string info = "{\"category\":\"OpInfoSummary\",\"Op Name\":\"test_kernel\",\"Device Id\":1}\n";
};

TEST_F(SummaryJsonlValidation, ParsesContextAndOnlyStringsInHints)
{
    Write("{\"category\":\"PerformanceHints\",\"hints\":[\"[PH01][AIV] 检查 Vector\"]}\n" + info);
    SummaryJsonlInfo display;
    std::string error;
    ASSERT_TRUE(ValidateCollectionFile(directory / "summary.jsonl", NpuRepFileType::Jsonl, &error, &display)) << error;
    EXPECT_TRUE(display.hasHintsRecord);
    EXPECT_EQ(display.opName, "test_kernel");
    EXPECT_EQ(display.device, "1");
    ASSERT_EQ(display.hints.size(), 1U);
    EXPECT_EQ(display.hints[0], "[PH01][AIV] 检查 Vector");
}

TEST_F(SummaryJsonlValidation, HistoricalNullContextRemainsReadable)
{
    Write("{\"category\":\"OpInfoSummary\",\"Op Name\":null,\"Device Id\":null}\n");
    SummaryJsonlInfo display;
    std::string error;
    ASSERT_TRUE(ValidateCollectionFile(directory / "summary.jsonl", NpuRepFileType::Jsonl, &error, &display)) << error;
    EXPECT_EQ(display.opName, "NA");
    EXPECT_EQ(display.device, "NA");
    EXPECT_FALSE(display.hasHintsRecord);
}

TEST_F(SummaryJsonlValidation, RejectsWrongTypesDuplicateRecordsAndInvalidOrdering)
{
    std::string error;
    for (const std::string hints : {"{}", "null", "\"text\"", "[1]", "[null]", "[{}]", "[[]]"}) {
        Write("{\"category\":\"PerformanceHints\",\"hints\":" + hints + "}\n" + info);
        EXPECT_FALSE(ValidateCollectionFile(directory / "summary.jsonl", NpuRepFileType::Jsonl, &error)) << hints;
        EXPECT_NE(error.find("PerformanceHints.hints"), std::string::npos) << error;
    }
    const std::string hint = "{\"category\":\"PerformanceHints\",\"hints\":[]}\n";
    for (const auto& invalid :
         {hint + hint + info, info + hint, hint, hint + "{\"category\":\"Memory\"}\n" + info,
          hint + info.substr(0, info.size() - 1), std::string("[]\n") + info}) {
        Write(invalid);
        EXPECT_FALSE(ValidateCollectionFile(directory / "summary.jsonl", NpuRepFileType::Jsonl, &error)) << invalid;
        EXPECT_NE(error.find("invalid summary JSONL"), std::string::npos) << error;
    }
}

TEST_F(SummaryJsonlValidation, HistoricalAndEmptySummariesProduceNoOutput)
{
    std::string error;
    Write(info);
    std::vector<uint8_t> encoded;
    testing::internal::CaptureStdout();
    ASSERT_TRUE(PackDirectoryToRep(directory, &encoded, &error)) << error;
    EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());

    Write("{\"category\":\"PerformanceHints\",\"hints\":[]}\n" + info);
    testing::internal::CaptureStdout();
    ASSERT_TRUE(PackDirectoryToRep(directory, &encoded, &error)) << error;
    EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
}

TEST_F(SummaryJsonlValidation, EscapesTerminalControls)
{
    Write("{\"category\":\"PerformanceHints\",\"hints\":[\"[PH01][AIV] 检查\\n\\u001b[31m\"]}\n" + info);
    std::string error;
    std::vector<uint8_t> encoded;
    testing::internal::CaptureStdout();
    ASSERT_TRUE(PackDirectoryToRep(directory, &encoded, &error)) << error;
    const auto text = testing::internal::GetCapturedStdout();
    EXPECT_NE(text.find("test_kernel"), std::string::npos);
    EXPECT_NE(text.find("device=1"), std::string::npos);
    EXPECT_NE(text.find("检查\\u000a\\u001b[31m"), std::string::npos);
    EXPECT_EQ(text.find('\x1b'), std::string::npos);
}

TEST_F(SummaryJsonlValidation, SkipDiagnosticsUseStderrOnlyWhenCliDebugIsEnabled)
{
    std::string error;
    std::vector<uint8_t> encoded;
    for (const char* value : {static_cast<const char*>(nullptr), "", "0", "true", "1"}) {
        if (value != nullptr) {
            ASSERT_EQ(setenv("NPU_COMPUTE_DEBUG", value, 1), 0);
        } else {
            ASSERT_EQ(unsetenv("NPU_COMPUTE_DEBUG"), 0);
        }
        testing::internal::CaptureStdout();
        testing::internal::CaptureStderr();
        Write(info);
        const bool historicalValid = PackDirectoryToRep(directory, &encoded, &error);
        Write("{\"category\":\"PerformanceHints\",\"hints\":[]}\n" + info);
        const bool emptyValid = PackDirectoryToRep(directory, &encoded, &error);
        const auto diagnostics = testing::internal::GetCapturedStderr();
        const auto output = testing::internal::GetCapturedStdout();
        ASSERT_TRUE(historicalValid) << error;
        ASSERT_TRUE(emptyValid) << error;
        if (value != nullptr && std::string(value) == "1") {
            EXPECT_NE(diagnostics.find("historical_summary_without_hints"), std::string::npos);
            EXPECT_NE(diagnostics.find("empty_hints"), std::string::npos);
        } else {
            EXPECT_TRUE(diagnostics.empty());
        }
        EXPECT_TRUE(output.empty());
    }
}

} // namespace
