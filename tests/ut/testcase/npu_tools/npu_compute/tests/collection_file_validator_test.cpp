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

#include <gtest/gtest.h>

#include <cstdio>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

using npucompute::cli::NpuRepFileType;
using npucompute::cli::ResolveCollectionFileType;
using npucompute::cli::ValidateCollectionFile;

class TempDirectory {
public:
    TempDirectory()
    {
        std::string path_template =
            (boost::filesystem::temp_directory_path() / "npu-compute-validator-test-XXXXXX").string();
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

bool WriteFile(const boost::filesystem::path& path, std::string_view content)
{
    std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

bool ReadFile(const boost::filesystem::path& path, std::string* content)
{
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    content->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

int TestFileTypeResolution(const boost::filesystem::path& directory)
{
    struct Case {
        const char* name;
        NpuRepFileType expected;
    };
    const Case cases[] = {
        {"metadata.json", NpuRepFileType::Json},      {"HardwareInfo.jsonl", NpuRepFileType::Jsonl},
        {"PipeUtilization.csv", NpuRepFileType::Csv}, {"records.db", NpuRepFileType::Sqlite},
        {"records.txt", NpuRepFileType::Txt},
    };

    for (const Case& test_case : cases) {
        NpuRepFileType actual = NpuRepFileType::NpuRep;
        std::string error = "old error";
        CHECK(ResolveCollectionFileType(directory / test_case.name, &actual, &error));
        CHECK(actual == test_case.expected);
        CHECK(error.empty());
    }

    NpuRepFileType actual = NpuRepFileType::NpuRep;
    std::string error;
    const boost::filesystem::path unknown = directory / "unknown.bin";
    CHECK(!ResolveCollectionFileType(unknown, &actual, &error));
    CHECK(error.find(unknown.string()) != std::string::npos);
    CHECK(!ResolveCollectionFileType(directory / "legacy.sqlite3", &actual, &error));
    return 0;
}

int TestHardwareInfoJsonl(const boost::filesystem::path& directory)
{
    const std::string complete = "{\"category\":\"Host Info\",\"cpu physical count\":1,"
                                 "\"cpu logical count\":2,\"memory total size(MB)\":3,"
                                 "\"disk total size(GB)\":4}\n"
                                 "{\"category\":\"Device Info\",\"npu count\":1,"
                                 "\"chip info\":\"chip\",\"arch info\":\"arch\"}\n"
                                 "{\"category\":\"CPU Information\",\"control cpu count\":1,"
                                 "\"ai cpu count\":2,\"ai cpu frequency(MHZ)\":3}\n"
                                 "{\"category\":\"AI Core Information\",\"ai core count\":1,"
                                 "\"ai cube count\":2,\"ai vector count\":3,"
                                 "\"ai cube frequency(MHZ)\":4,\"ai vector frequency(MHZ)\":5}\n"
                                 "{\"category\":\"Memory Information\",\"hbm total(MB)\":1,"
                                 "\"hbm used(MB)\":2,\"hbm frequency(MHZ)\":3}\n";
    const boost::filesystem::path path = directory / "HardwareInfo.jsonl";
    std::string error;

    CHECK(WriteFile(path, complete));
    CHECK(ValidateCollectionFile(path, NpuRepFileType::Jsonl, &error));
    CHECK(error.empty());
    std::string actual;
    CHECK(ReadFile(path, &actual));
    CHECK(actual == complete);

    const std::size_t fifth_line = complete.rfind("{\"category\":\"Memory Information\"");
    CHECK(fifth_line != std::string::npos);
    CHECK(WriteFile(path, complete.substr(0, fifth_line)));
    CHECK(!ValidateCollectionFile(path, NpuRepFileType::Jsonl, &error));
    CHECK(!error.empty());

    CHECK(WriteFile(path, complete.substr(0, complete.size() - 1U)));
    CHECK(!ValidateCollectionFile(path, NpuRepFileType::Jsonl, &error));
    CHECK(!error.empty());
    return 0;
}

int TestCsv(const boost::filesystem::path& directory)
{
    const boost::filesystem::path path = directory / "PipeUtilization.csv";
    std::string error;

    const std::string complete = "block_id,sub_block_id\n0,0\n";
    CHECK(WriteFile(path, complete));
    CHECK(ValidateCollectionFile(path, NpuRepFileType::Csv, &error));
    CHECK(error.empty());
    std::string actual;
    CHECK(ReadFile(path, &actual));
    CHECK(actual == complete);

    CHECK(WriteFile(path, "block_id,sub_block_id\n"));
    CHECK(!ValidateCollectionFile(path, NpuRepFileType::Csv, &error));
    CHECK(!error.empty());

    CHECK(WriteFile(path, ""));
    CHECK(!ValidateCollectionFile(path, NpuRepFileType::Csv, &error));
    CHECK(!error.empty());
    return 0;
}

int TestOtherSupportedFiles(const boost::filesystem::path& directory)
{
    const NpuRepFileType types[] = {
        NpuRepFileType::Json,
        NpuRepFileType::Sqlite,
        NpuRepFileType::Txt,
    };
    const char* names[] = {"metadata.json", "records.db", "records.txt"};
    std::string error;

    for (std::size_t index = 0; index < 3U; ++index) {
        const boost::filesystem::path path = directory / names[index];
        CHECK(WriteFile(path, "payload"));
        CHECK(ValidateCollectionFile(path, types[index], &error));
        CHECK(error.empty());
        CHECK(WriteFile(path, ""));
        CHECK(!ValidateCollectionFile(path, types[index], &error));
        CHECK(!error.empty());
    }

    const boost::filesystem::path missing = directory / "missing.json";
    CHECK(!ValidateCollectionFile(missing, NpuRepFileType::Json, &error));
    CHECK(error.find(missing.string()) != std::string::npos);

    const boost::filesystem::path invalid_type = directory / "invalid.csv";
    CHECK(WriteFile(invalid_type, "header\ndata\n"));
    CHECK(!ValidateCollectionFile(invalid_type, static_cast<NpuRepFileType>(999U), &error));
    CHECK(!error.empty());
    return 0;
}

} // namespace

static int RunSuiteMain()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    if (TestFileTypeResolution(temporary.Path()) != 0 || TestHardwareInfoJsonl(temporary.Path()) != 0 ||
        TestCsv(temporary.Path()) != 0 || TestOtherSupportedFiles(temporary.Path()) != 0) {
        return 1;
    }
    return 0;
}

TEST(NpuComputeCollectionFileValidator, Main)
{
    ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeCollectionFileValidator reported failure";
}
