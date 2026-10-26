/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <boost/filesystem.hpp>
#include <gtest/gtest.h>
#include "launch/disk_output_directory.h"
#include "report/collection_processor.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace {
using namespace npucompute::cli;

class DiskResultsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::string value = "/tmp/npu-compute-disk-test-XXXXXX";
        char* directory = ::mkdtemp(value.data());
        ASSERT_NE(directory, nullptr);
        parent = directory;
    }
    void TearDown() override
    {
        boost::system::error_code error;
        boost::filesystem::remove_all(parent, error);
    }
    void Write(const boost::filesystem::path& path, const std::string& text = "existing")
    {
        std::ofstream stream(path.string(), std::ios::binary);
        stream << text;
        ASSERT_TRUE(stream.good());
    }
    boost::filesystem::path parent;
};

class StdoutCapture {
public:
    StdoutCapture() : previous_(std::cout.rdbuf(output_.rdbuf())) {}

    ~StdoutCapture() { std::cout.rdbuf(previous_); }

    std::string Text() const { return output_.str(); }

private:
    std::ostringstream output_;
    std::streambuf* previous_;
};

struct FixedNames {
    std::size_t calls = 0;
    bool vary = false;
    bool fail = false;
};

bool FixedEpoch(uint64_t* value, void*, std::string*)
{
    *value = 1700000000123ULL;
    return true;
}

bool FixedRandom(std::array<uint8_t, 4>* value, void* context, std::string* error)
{
    auto& fixed = *static_cast<FixedNames*>(context);
    if (fixed.fail) {
        *error = "random source failed";
        return false;
    }
    *value = {0x01, 0x23, 0xab, static_cast<uint8_t>(fixed.vary && fixed.calls != 0 ? 0xfe : 0xff)};
    ++fixed.calls;
    return true;
}

ReportNameSources Names(FixedNames* fixed)
{
    ReportNameSources sources;
    sources.epoch_milliseconds = FixedEpoch;
    sources.random_bytes = FixedRandom;
    sources.context = fixed;
    return sources;
}

TEST_F(DiskResultsTest, RetainedDirectorySurvivesDestructionWithPrivatePermissions)
{
    FixedNames fixed;
    std::string error;
    const auto expected = parent / "report_1700000000123_0123abff";
    {
        DiskOutputDirectory output;
        ASSERT_TRUE(DiskOutputDirectory::CreateWithSources(parent, Names(&fixed), &output, &error)) << error;
        EXPECT_EQ(output.Path(), expected.string());
        struct stat status {};
        ASSERT_EQ(::stat(output.Path().c_str(), &status), 0);
        EXPECT_EQ(status.st_mode & 0777, 0700);
        Write(expected / "result.csv", "a\n1\n");
        output.Retain();
    }
    EXPECT_TRUE(boost::filesystem::exists(expected / "result.csv"));
}

TEST_F(DiskResultsTest, UnretainedDirectoryIsCleanedWithoutTouchingExistingData)
{
    Write(parent / "keep");
    std::string path;
    std::string error;
    {
        DiskOutputDirectory output;
        ASSERT_TRUE(DiskOutputDirectory::Create(parent, &output, &error));
        path = output.Path();
        Write(boost::filesystem::path(path) / "partial.csv");
    }
    EXPECT_FALSE(boost::filesystem::exists(path));
    EXPECT_TRUE(boost::filesystem::exists(parent / "keep"));
}

TEST_F(DiskResultsTest, CollisionRetriesAndNeverAdoptsExistingDirectory)
{
    const auto occupied = parent / "report_1700000000123_0123abff";
    ASSERT_TRUE(boost::filesystem::create_directory(occupied));
    Write(occupied / "keep");
    FixedNames fixed;
    fixed.vary = true;
    std::string error;
    {
        DiskOutputDirectory output;
        ASSERT_TRUE(DiskOutputDirectory::CreateWithSources(parent, Names(&fixed), &output, &error));
        EXPECT_EQ(output.Path(), (parent / "report_1700000000123_0123abfe").string());
        EXPECT_EQ(fixed.calls, 2U);
    }
    EXPECT_TRUE(boost::filesystem::exists(occupied / "keep"));
    fixed = {};
    {
        DiskOutputDirectory output;
        EXPECT_FALSE(DiskOutputDirectory::CreateWithSources(parent, Names(&fixed), &output, &error));
        EXPECT_EQ(fixed.calls, 128U);
        EXPECT_TRUE(output.Path().empty());
    }
    EXPECT_TRUE(boost::filesystem::exists(occupied / "keep"));
}

TEST_F(DiskResultsTest, FailedNameGenerationOwnsNothingAndRemoveIsIdempotent)
{
    FixedNames fixed;
    fixed.fail = true;
    DiskOutputDirectory output;
    std::string error;
    EXPECT_FALSE(DiskOutputDirectory::CreateWithSources(parent, Names(&fixed), &output, &error));
    EXPECT_EQ(error, "random source failed");
    EXPECT_TRUE(output.Path().empty());
    EXPECT_TRUE(boost::filesystem::is_empty(parent));
    EXPECT_TRUE(output.Remove(&error));
    EXPECT_TRUE(output.Remove(&error));
}

TEST_F(DiskResultsTest, ValidationPreservesNestedFilesAndRemovesOnlyInternalLocks)
{
    ASSERT_TRUE(boost::filesystem::create_directory(parent / "child"));
    Write(parent / "Memory.csv", "block,bytes\n0,128\n");
    Write(parent / "child/result.json", "{}");
    Write(parent / ".hardware_info.lock", "");
    Write(parent / "child/.hardware_info.lock", "");
    std::string error;
    ASSERT_TRUE(ValidateAndCleanCollectionDirectory(parent, &error)) << error;
    EXPECT_TRUE(boost::filesystem::exists(parent / "child/result.json"));
    EXPECT_FALSE(boost::filesystem::exists(parent / ".hardware_info.lock"));
    EXPECT_FALSE(boost::filesystem::exists(parent / "child/.hardware_info.lock"));
}

TEST_F(DiskResultsTest, NoValidFilesPreventDirectoryDelivery)
{
    std::string error;
    for (const auto& item : std::vector<std::pair<std::string, std::string>>{
             {"bad.csv", "header\n"},
             {"bad.jsonl", "{}"},
             {"bad.json", ""},
             {"left.csv.tmp", "x"},
             {"left.tmp.123", "x"},
             {"unknown.bin", "x"}}) {
        Write(parent / item.first, item.second);
        EXPECT_FALSE(ValidateAndCleanCollectionDirectory(parent, &error)) << item.first;
        boost::filesystem::remove(parent / item.first);
    }
    Write(parent / "good.json", "{}");
    boost::filesystem::create_symlink(parent / "good.json", parent / "linked.json");
    EXPECT_FALSE(ValidateAndCleanCollectionDirectory(parent, &error));
    boost::filesystem::remove(parent / "linked.json");
    ASSERT_EQ(::mkfifo((parent / "fifo.csv").c_str(), 0600), 0);
    EXPECT_FALSE(ValidateAndCleanCollectionDirectory(parent, &error));
    boost::filesystem::remove(parent / "fifo.csv");
    boost::filesystem::create_directory(parent / ".hardware_info.lock");
    EXPECT_FALSE(ValidateAndCleanCollectionDirectory(parent, &error));
}
} // namespace

namespace {
TEST_F(DiskResultsTest, InvalidSamplingFilesAreRemovedWithoutDeletingValidSiblings)
{
    ASSERT_TRUE(boost::filesystem::create_directory(parent / "child"));
    Write(parent / "child/Memory.csv", "block,bytes\n0,128\n");
    Write(parent / "summary.db", "broken database");
    Write(parent / "objdump.txt", "no instructions");
    Write(parent / "symbolizer.json", "broken json");
    Write(parent / "bad.jsonl", "{}");
    Write(parent / ".hardware_info.lock", "");
    boost::filesystem::create_directory(parent / ".biu-staging");
    Write(parent / ".biu-staging/fragment.json", "{}");
    std::vector<std::string> issues;
    std::string error;
    EXPECT_TRUE(ValidateAndCleanCollectionDirectory(parent, &error, &issues)) << error;
    EXPECT_EQ(issues.size(), 5U);
    EXPECT_TRUE(boost::filesystem::exists(parent / "child/Memory.csv"));
    for (const auto* name :
         {"summary.db", "objdump.txt", "symbolizer.json", "bad.jsonl", ".hardware_info.lock", ".biu-staging"}) {
        EXPECT_FALSE(boost::filesystem::exists(parent / name)) << name;
    }
}

TEST_F(DiskResultsTest, NoPackValidatesAndPrintsRootAndNestedSummaries)
{
    ASSERT_TRUE(boost::filesystem::create_directory(parent / "child"));
    const std::string root_summary = "{\"category\":\"PerformanceHints\",\"hints\":[\"root hint\"]}\n"
                                     "{\"category\":\"OpInfoSummary\",\"Op Name\":\"root kernel\",\"Device Id\":0}\n";
    const std::string nested_summary =
        "{\"category\":\"PerformanceHints\",\"hints\":[\"nested hint\"]}\n"
        "{\"category\":\"OpInfoSummary\",\"Op Name\":\"nested kernel\",\"Device Id\":1}\n";
    ASSERT_NO_FATAL_FAILURE(Write(parent / "summary.jsonl", root_summary));
    ASSERT_NO_FATAL_FAILURE(Write(parent / "child/summary.jsonl", nested_summary));
    // Generic JSONL results must still end with a newline; this sibling is
    // intentionally malformed so retain mode removes it after the scan.
    ASSERT_NO_FATAL_FAILURE(Write(parent / "invalid.jsonl", "{}"));

    std::vector<std::string> issues;
    std::string error;
    StdoutCapture output;
    ASSERT_TRUE(ValidateAndCleanCollectionDirectory(parent, &error, &issues)) << error;

    EXPECT_TRUE(boost::filesystem::exists(parent / "summary.jsonl"));
    EXPECT_TRUE(boost::filesystem::exists(parent / "child/summary.jsonl"));
    EXPECT_FALSE(boost::filesystem::exists(parent / "invalid.jsonl"));
    ASSERT_EQ(issues.size(), 1U);
    EXPECT_NE(issues.front().find("invalid.jsonl"), std::string::npos);
    const std::string text = output.Text();
    for (const std::string& expected :
         {"op=root kernel  device=0", "op=nested kernel  device=1", "root hint", "nested hint"}) {
        const auto position = text.find(expected);
        ASSERT_NE(position, std::string::npos) << expected;
        EXPECT_EQ(text.find(expected, position + expected.size()), std::string::npos) << expected;
    }
}
} // namespace
