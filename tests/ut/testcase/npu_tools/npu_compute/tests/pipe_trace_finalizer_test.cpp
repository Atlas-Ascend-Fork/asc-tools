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

#include "report/pipe_trace_finalizer.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#define CHECK(condition)                                      \
    do {                                                      \
        if (!(condition)) {                                   \
            ADD_FAILURE_AT(__FILE__, __LINE__) << #condition; \
            return 1;                                         \
        }                                                     \
    } while (false)

namespace {

class TestDirectory {
public:
    TestDirectory()
        : path_(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("pipe-final-%%%%-%%%%"))
    {
        boost::filesystem::create_directories(path_);
    }

    ~TestDirectory()
    {
        boost::system::error_code ignored;
        boost::filesystem::remove_all(path_, ignored);
    }

    const boost::filesystem::path& Path() const { return path_; }

private:
    boost::filesystem::path path_;
};

bool Write(const boost::filesystem::path& path, const std::string& content)
{
    std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
    output << content;
    return output.good();
}

std::string Fragment(const std::string& pid, const std::string& name, double timestamp)
{
    return "{\"displayTimeUnit\":\"ns\",\"profilingType\":\"op\",\"schemaVersion\":1,\"traceEvents\":["
           "{\"cname\":\"startup\",\"dur\":0.5,\"name\":\"" +
           name + "\",\"ph\":\"X\",\"pid\":\"" + pid + "\",\"tid\":\"" + name +
           "\",\"ts\":" + std::to_string(timestamp) + "}]}\n";
}

int TestMultipleFragments()
{
    TestDirectory directory;
    const auto staging = directory.Path() / ".biu-staging";
    const auto process0 = staging / "process-aaaa";
    const auto process1 = staging / "process-bbbb";
    CHECK(boost::filesystem::create_directories(process0));
    CHECK(boost::filesystem::create_directories(process1));
    CHECK(Write(process0 / "a.json", Fragment("group0.cubecore", "SCALAR", 1.0)));
    CHECK(Write(process1 / "b.json", Fragment("group2.veccore1", "VECTOR", 2.0)));
    CHECK(Write(
        process0 / "manifest.json",
        "{\"version\":1,\"state\":\"complete\",\"status\":0,\"fragments\":[{\"resultSequence\":\"0\","
        "\"replayId\":\"7\",\"deviceId\":0,\"file\":\"a.json\",\"eventCount\":\"1\"}]}\n"));
    CHECK(Write(
        process1 / "manifest.json",
        "{\"version\":1,\"state\":\"complete\",\"status\":0,\"fragments\":[{\"resultSequence\":\"3\","
        "\"replayId\":\"9\",\"deviceId\":1,\"file\":\"b.json\",\"eventCount\":\"1\"}]}\n"));

    std::string error;
    const bool finalized = npucompute::cli::FinalizePipeTrace(directory.Path(), true, &error);
    if (!finalized) {
        std::fprintf(stderr, "FinalizePipeTrace failed: %s\n", error.c_str());
    }
    CHECK(finalized);
    CHECK(error.empty());
    CHECK(!boost::filesystem::exists(staging));
    std::ifstream input((directory.Path() / "PipeTrace.json").string());
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(json.find("process0.result0.device0.replay7.group0.cubecore") != std::string::npos);
    CHECK(json.find("process1.result3.device1.replay9.group2.veccore1") != std::string::npos);
    return 0;
}

int TestRejectsUnexpectedStaging()
{
    TestDirectory directory;
    CHECK(boost::filesystem::create_directory(directory.Path() / ".biu-staging"));
    std::string error;
    CHECK(!npucompute::cli::FinalizePipeTrace(directory.Path(), false, &error));
    CHECK(error.find("disabled") != std::string::npos);
    return 0;
}

} // namespace

static int RunSuiteMain()
{
    if (const int result = TestMultipleFragments(); result != 0) {
        return result;
    }
    return TestRejectsUnexpectedStaging();
    return 0;
}

TEST(NpuComputePipeTraceFinalizer, Main)
{
    ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputePipeTraceFinalizer reported failure";
}

TEST(NpuComputePipeTraceFinalizer, KeepsCompletedFragmentFromFailedCollection)
{
    TestDirectory directory;
    const auto process = directory.Path() / ".biu-staging/process-uds";
    ASSERT_TRUE(boost::filesystem::create_directories(process));
    ASSERT_TRUE(Write(process / "good.json", Fragment("cube", "SCALAR", 1.0)));
    ASSERT_TRUE(Write(process / "bad.json", "{broken"));
    ASSERT_TRUE(Write(
        process / "manifest.json",
        R"({"version":1,"state":"failed","status":1,"fragments":[{"resultSequence":"0","replayId":"0","deviceId":0,"file":"good.json","eventCount":"1"},{"resultSequence":"1","replayId":"0","deviceId":0,"file":"bad.json","eventCount":"1"}]})"));
    std::string error;
    ASSERT_TRUE(npucompute::cli::FinalizePipeTrace(directory.Path(), true, &error)) << error;
    std::ifstream input((directory.Path() / "PipeTrace.json").string());
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_NE(json.find("SCALAR"), std::string::npos);
    EXPECT_EQ(json.find("broken"), std::string::npos);
}
