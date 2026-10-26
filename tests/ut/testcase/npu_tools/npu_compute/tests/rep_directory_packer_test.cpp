/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/collection_processor.h"

#include <gtest/gtest.h>
#include "rep_test_decoder.h"

#include <cstdio>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int Check(bool condition, const char* expression, int line)
{
    if (condition) {
        return 0;
    }
    ADD_FAILURE_AT(__FILE__, line) << expression;
    return 1;
}

#define CHECK(expression)                               \
    do {                                                \
        if (Check((expression), #expression, __LINE__)) \
            return 1;                                   \
    } while (false)

using npucompute::cli::NPU_REP_FILE_INFO_SIZE;
using npucompute::cli::NPU_REP_HEAD_SIZE;
using npucompute::cli::NpuRepFileType;
using npucompute::cli::PackDirectoryToRep;
using npucompute::cli::test::DecodedRep;
using npucompute::cli::test::DecodeRep;

class TempDirectory {
public:
    TempDirectory()
    {
        std::string path_template =
            (boost::filesystem::temp_directory_path() / "npu-compute-packer-test-XXXXXX").string();
        path_template.push_back('\0');
        char* created = ::mkdtemp(path_template.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TempDirectory()
    {
        if (!path_.empty()) {
            boost::system::error_code error;
            boost::filesystem::remove_all(path_, error);
        }
    }

    const boost::filesystem::path& Path() const { return path_; }

private:
    boost::filesystem::path path_;
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

bool WriteFile(const boost::filesystem::path& path, std::string_view content)
{
    std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

std::vector<uint8_t> Bytes(std::string_view content) { return {content.begin(), content.end()}; }

std::string HardwareInfo()
{
    return "{\"category\":\"Host Info\",\"cpu physical count\":1}\n"
           "{\"category\":\"Device Info\",\"npu count\":1}\n"
           "{\"category\":\"CPU Information\",\"control cpu count\":1}\n"
           "{\"category\":\"AI Core Information\",\"ai core count\":1}\n"
           "{\"category\":\"Memory Information\",\"hbm total(MB)\":1}\n";
}

bool ExpectPackFailure(const boost::filesystem::path& directory, std::string_view error_fragment)
{
    std::vector<uint8_t> encoded = {1U, 2U, 3U};
    std::string error;
    PackDirectoryToRep(directory, &encoded, &error);
    if (!encoded.empty() || error.find(error_fragment) == std::string::npos) {
        std::fprintf(stderr, "unexpected pack result: error=%s\n", error.c_str());
        return false;
    }
    return true;
}

int TestRecursivePacking()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    const boost::filesystem::path root = temporary.Path() / "staging";
    const boost::filesystem::path device = root / "device_0";
    const boost::filesystem::path details = device / "details";
    const boost::filesystem::path empty = device / "empty";
    CHECK(boost::filesystem::create_directories(details));
    CHECK(boost::filesystem::create_directory(empty));

    const std::string hardware = HardwareInfo();
    const std::string pipe = "block_id,sub_block_id\n0,0\n";
    const std::string memory = "block_id,read_bytes\n0,128\n";
    const std::string l2 = "block_id,hit_rate\n0,99\n";
    CHECK(WriteFile(root / "HardwareInfo.jsonl", hardware));
    CHECK(WriteFile(root / "PipeUtilization.csv", pipe));
    CHECK(WriteFile(root / ".hardware_info.lock", "lock"));
    CHECK(WriteFile(device / "Memory.csv", memory));
    CHECK(WriteFile(details / "L2Cache.csv", l2));

    std::vector<uint8_t> encoded;
    std::string error;
    CHECK(PackDirectoryToRep(root, &encoded, &error));
    CHECK(error.empty());

    DecodedRep top;
    CHECK(DecodeRep(encoded, &top, &error));
    CHECK(top.entries.size() == 3U);
    CHECK(top.entries[0].file_name == "HardwareInfo.jsonl");
    CHECK(top.entries[0].file_type == NpuRepFileType::Jsonl);
    CHECK(top.entries[0].payload == Bytes(hardware));
    CHECK(top.entries[1].file_name == "PipeUtilization.csv");
    CHECK(top.entries[1].file_type == NpuRepFileType::Csv);
    CHECK(top.entries[1].payload == Bytes(pipe));
    CHECK(top.entries[2].file_name == "device_0.npu.rep");
    CHECK(top.entries[2].file_type == NpuRepFileType::NpuRep);

    DecodedRep device_rep;
    CHECK(DecodeRep(top.entries[2].payload, &device_rep, &error));
    CHECK(device_rep.entries.size() == 3U);
    CHECK(device_rep.entries[0].file_name == "Memory.csv");
    CHECK(device_rep.entries[0].payload == Bytes(memory));
    CHECK(device_rep.entries[1].file_name == "details.npu.rep");
    CHECK(device_rep.entries[1].file_type == NpuRepFileType::NpuRep);
    CHECK(device_rep.entries[2].file_name == "empty.npu.rep");
    CHECK(device_rep.entries[2].file_type == NpuRepFileType::NpuRep);
    CHECK(device_rep.entries[0].file_offset == NPU_REP_HEAD_SIZE + 3U * NPU_REP_FILE_INFO_SIZE);

    DecodedRep details_rep;
    CHECK(DecodeRep(device_rep.entries[1].payload, &details_rep, &error));
    CHECK(details_rep.entries.size() == 1U);
    CHECK(details_rep.entries[0].file_name == "L2Cache.csv");
    CHECK(details_rep.entries[0].payload == Bytes(l2));
    CHECK(details_rep.entries[0].file_offset == NPU_REP_HEAD_SIZE + NPU_REP_FILE_INFO_SIZE);

    DecodedRep empty_rep;
    CHECK(DecodeRep(device_rep.entries[2].payload, &empty_rep, &error));
    CHECK(empty_rep.entries.empty());
    CHECK(device_rep.entries[2].payload.size() == NPU_REP_HEAD_SIZE);
    return 0;
}

int TestRejectsSymlink()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    CHECK(WriteFile(temporary.Path() / "target.csv", "header\ndata\n"));
    boost::system::error_code error;
    boost::filesystem::create_symlink(temporary.Path() / "target.csv", temporary.Path() / "link.csv", error);
    CHECK(!error);
    std::vector<uint8_t> encoded;
    std::vector<std::string> issues;
    std::string detail;
    CHECK(PackDirectoryToRep(temporary.Path(), &encoded, &detail, &issues));
    CHECK(issues.size() == 1 && issues[0].find("link.csv") != std::string::npos);
    DecodedRep decoded;
    CHECK(DecodeRep(encoded, &decoded, &detail));
    CHECK(decoded.entries.size() == 2);
    CHECK(decoded.entries.back().file_name == "target.csv");
    return 0;
}

int TestRejectsTemporaryUnknownAndSpecialFiles()
{
    {
        TempDirectory temporary;
        CHECK(!temporary.Path().empty());
        CHECK(WriteFile(temporary.Path() / "HardwareInfo.jsonl.tmp.42", "partial"));
        CHECK(ExpectPackFailure(temporary.Path(), "temporary"));
    }
    {
        TempDirectory temporary;
        CHECK(!temporary.Path().empty());
        CHECK(WriteFile(temporary.Path() / "notes.bin", "unknown"));
        CHECK(ExpectPackFailure(temporary.Path(), "notes.bin"));
    }
    {
        TempDirectory temporary;
        CHECK(!temporary.Path().empty());
        const boost::filesystem::path fifo = temporary.Path() / "stream.csv";
        CHECK(::mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR) == 0);
        CHECK(ExpectPackFailure(temporary.Path(), "stream.csv"));
    }
    return 0;
}

int TestRejectsConvertedNameConflict()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    CHECK(boost::filesystem::create_directory(temporary.Path() / "device"));
    CHECK(WriteFile(temporary.Path() / "device.npu.rep", "conflict"));
    CHECK(ExpectPackFailure(temporary.Path(), "conflict"));
    return 0;
}

} // namespace

static int RunSuiteMain()
{
    if (TestRecursivePacking() != 0 || TestRejectsSymlink() != 0 || TestRejectsTemporaryUnknownAndSpecialFiles() != 0 ||
        TestRejectsConvertedNameConflict() != 0) {
        return 1;
    }
    return 0;
}

TEST(NpuComputeRepDirectoryPacker, Main)
{
    ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeRepDirectoryPacker reported failure";
}

TEST(NpuComputeRepDirectoryPacker, KeepsValidFilesWhenOneCollectionFileFails)
{
    TempDirectory temporary;
    const auto nested = temporary.Path() / "collection";
    ASSERT_TRUE(boost::filesystem::create_directory(nested));
    const std::string csv = "header\nvalue\n";
    ASSERT_TRUE(WriteFile(temporary.Path() / "Memory.csv", csv));
    ASSERT_TRUE(WriteFile(nested / "L2Cache.csv", csv));
    ASSERT_TRUE(WriteFile(nested / "MemoryUB.csv", "header\n"));
    std::vector<uint8_t> encoded;
    std::string error;
    ASSERT_TRUE(PackDirectoryToRep(temporary.Path(), &encoded, &error)) << error;
    DecodedRep top;
    ASSERT_TRUE(DecodeRep(encoded, &top, &error)) << error;
    ASSERT_EQ(top.entries.size(), 2U);
    EXPECT_EQ(top.entries[0].payload, Bytes(csv));
    DecodedRep child;
    ASSERT_TRUE(DecodeRep(top.entries[1].payload, &child, &error)) << error;
    ASSERT_EQ(child.entries.size(), 1U);
    EXPECT_EQ(child.entries[0].file_name, "L2Cache.csv");
    EXPECT_EQ(child.entries[0].payload, Bytes(csv));
}

TEST(NpuComputeRepDirectoryPacker, PrintsValidatedSummaryHints)
{
    TempDirectory temporary;
    ASSERT_FALSE(temporary.Path().empty());
    const std::string summary = "{\"category\":\"PerformanceHints\",\"hints\":[\"hint\"]}\n"
                                "{\"category\":\"OpInfoSummary\",\"Op Name\":\"kernel\",\"Device Id\":1}\n";
    ASSERT_TRUE(WriteFile(temporary.Path() / "summary.jsonl", summary));

    std::vector<uint8_t> encoded;
    std::string error;
    StdoutCapture output;
    ASSERT_TRUE(PackDirectoryToRep(temporary.Path(), &encoded, &error)) << error;
    EXPECT_EQ(output.Text(), "npu-compute: Performance Hints\n  op=kernel  device=1\n  hint\n");

    DecodedRep decoded;
    ASSERT_TRUE(DecodeRep(encoded, &decoded, &error)) << error;
    ASSERT_EQ(decoded.entries.size(), 1U);
    EXPECT_EQ(decoded.entries[0].file_name, "summary.jsonl");
    EXPECT_EQ(decoded.entries[0].payload, Bytes(summary));
}

TEST(NpuComputeRepDirectoryPacker, SkippedSummaryDoesNotPrintHints)
{
    TempDirectory temporary;
    ASSERT_FALSE(temporary.Path().empty());
    ASSERT_TRUE(WriteFile(temporary.Path() / "Memory.csv", "header\nvalue\n"));
    ASSERT_TRUE(
        WriteFile(temporary.Path() / "summary.jsonl", "{\"category\":\"PerformanceHints\",\"hints\":[\"hint\"]}\n"));

    std::vector<uint8_t> encoded;
    std::vector<std::string> issues;
    std::string error;
    StdoutCapture output;
    ASSERT_TRUE(PackDirectoryToRep(temporary.Path(), &encoded, &error, &issues)) << error;
    ASSERT_EQ(issues.size(), 1U);
    EXPECT_NE(issues[0].find("summary.jsonl"), std::string::npos);
    EXPECT_TRUE(output.Text().empty());

    DecodedRep decoded;
    ASSERT_TRUE(DecodeRep(encoded, &decoded, &error)) << error;
    ASSERT_EQ(decoded.entries.size(), 2U);
    EXPECT_EQ(decoded.entries[0].file_name, "Memory.csv");
    EXPECT_EQ(decoded.entries[1].file_name, "collection-status.json");
}

TEST(NpuComputeRepDirectoryPacker, PrintsRootAndNestedSummaryHintsOnce)
{
    TempDirectory temporary;
    ASSERT_FALSE(temporary.Path().empty());
    const auto nested = temporary.Path() / "collection";
    ASSERT_TRUE(boost::filesystem::create_directory(nested));
    const std::string summary = "{\"category\":\"PerformanceHints\",\"hints\":[\"nested hint\"]}\n"
                                "{\"category\":\"OpInfoSummary\",\"Op Name\":\"nested kernel\",\"Device Id\":1}\n";
    ASSERT_TRUE(WriteFile(nested / "summary.jsonl", summary));
    const std::string rootSummary = "{\"category\":\"PerformanceHints\",\"hints\":[\"root hint\"]}\n"
                                    "{\"category\":\"OpInfoSummary\",\"Op Name\":\"root kernel\",\"Device Id\":0}\n";
    ASSERT_TRUE(WriteFile(temporary.Path() / "summary.jsonl", rootSummary));

    std::vector<uint8_t> encoded;
    std::string error;
    StdoutCapture output;
    ASSERT_TRUE(PackDirectoryToRep(temporary.Path(), &encoded, &error)) << error;
    const auto text = output.Text();
    for (const std::string expected :
         {"op=root kernel  device=0", "op=nested kernel  device=1", "root hint", "nested hint"}) {
        const auto position = text.find(expected);
        ASSERT_NE(position, std::string::npos) << expected;
        EXPECT_EQ(text.find(expected, position + expected.size()), std::string::npos) << expected;
    }

    DecodedRep decoded;
    ASSERT_TRUE(DecodeRep(encoded, &decoded, &error)) << error;
    ASSERT_EQ(decoded.entries.size(), 2U);
    DecodedRep child;
    ASSERT_TRUE(DecodeRep(decoded.entries[0].payload, &child, &error)) << error;
    ASSERT_EQ(child.entries.size(), 1U);
    EXPECT_EQ(child.entries[0].file_name, "summary.jsonl");
    EXPECT_EQ(child.entries[0].payload, Bytes(summary));
    EXPECT_EQ(decoded.entries[1].file_name, "summary.jsonl");
    EXPECT_EQ(decoded.entries[1].payload, Bytes(rootSummary));
}
