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

#include "device_symbolizer.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

namespace {

using aclsan::device_runtime::CallStackResult;
using aclsan::device_runtime::DeviceSymbolizer;
using aclsan::device_runtime::DeviceSymbolizerConfig;

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(const char* name) : name_(name)
    {
        const char* value = std::getenv(name_);
        if (value != nullptr) {
            wasSet_ = true;
            value_ = value;
        }
    }

    ~ScopedEnvironmentVariable()
    {
        if (wasSet_) {
            setenv(name_, value_.c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    const char* name_;
    bool wasSet_ = false;
    std::string value_;
};

fs::path TestDirectory(const char* name)
{
    const fs::path directory = fs::temp_directory_path() / name;
    fs::remove_all(directory);
    fs::create_directories(directory);
    return directory;
}

void TestParsesFramesAndCachesByImageAndPc()
{
    const fs::path directory = TestDirectory("aclsan-device-symbolizer-test");
    const DeviceSymbolizerConfig config{"/tools/llvm-symbolizer", "/work/kernel.elf", directory.string()};
    DeviceSymbolizer symbolizer(config);
    size_t runnerCalls = 0;
    std::vector<std::string> command;
    const auto runner = [&runnerCalls, &command](
                            const std::vector<std::string>& actualCommand, const std::string& logPath, std::string&) {
        ++runnerCalls;
        command = actualCommand;
        std::ofstream output(logPath);
        output << "CopyIn\n/src/kernel.asc:46:5\n"
               << "AddKernel\n/src/kernel.asc:58:1\n";
        return true;
    };

    const CallStackResult first = symbolizer.ResolveCallStackWithRunner(0x170, runner);
    const CallStackResult second = symbolizer.ResolveCallStackWithRunner(0x170, runner);

    assert(first.available);
    assert(first.error.empty());
    assert(first.frames.size() == 2);
    assert(first.frames[0].functionName == "CopyIn");
    assert(first.frames[0].fileName == "/src/kernel.asc");
    assert(first.frames[0].line == 46);
    assert(first.frames[0].column == 5);
    assert(first.frames[0].inlineDepth == 0);
    assert(first.frames[1].functionName == "AddKernel");
    assert(first.frames[1].fileName == "/src/kernel.asc");
    assert(first.frames[1].line == 58);
    assert(first.frames[1].column == 1);
    assert(first.frames[1].inlineDepth == 1);
    assert(second.frames.size() == 2);
    assert(runnerCalls == 1);
    assert(
        command == std::vector<std::string>(
                       {"/tools/llvm-symbolizer", "--obj=/work/kernel.elf", "--inlines", "--demangle",
                        "--functions=short", "0x170"}));
    fs::remove_all(directory);
}

void TestRejectsInvalidSymbolizerOutput()
{
    const fs::path directory = TestDirectory("aclsan-device-symbolizer-invalid-test");
    const DeviceSymbolizerConfig config{"/tools/llvm-symbolizer", "/work/kernel.elf", directory.string()};
    const std::vector<std::string> outputs{
        "", "CopyIn\n", "??\n??\n", "CopyIn\n/src/kernel.asc:not-a-line:5\n", "CopyIn\n/src/kernel.asc:42\n"};
    for (const std::string& text : outputs) {
        DeviceSymbolizer symbolizer(config);
        const auto runner = [&text](const std::vector<std::string>&, const std::string& logPath, std::string&) {
            std::ofstream output(logPath);
            output << text;
            return true;
        };
        const CallStackResult result = symbolizer.ResolveCallStackWithRunner(0x180, runner);
        assert(!result.available);
        assert(result.frames.empty());
        assert(result.error == "invalid_symbolizer_output");
    }
    fs::remove_all(directory);
}

void TestParsesLocationFromTheRight()
{
    const fs::path directory = TestDirectory("aclsan-device-symbolizer-location-test");
    const DeviceSymbolizerConfig config{"/tools/llvm-symbolizer", "/work/kernel.elf", directory.string()};
    DeviceSymbolizer symbolizer(config);
    const auto runner = [](const std::vector<std::string>&, const std::string& logPath, std::string&) {
        std::ofstream output(logPath);
        output << "CopyIn\n/src/generated:kernel.asc:46:5\n";
        return true;
    };

    const CallStackResult result = symbolizer.ResolveCallStackWithRunner(0x1a0, runner);

    assert(result.available);
    assert(result.frames.size() == 1);
    assert(result.frames[0].fileName == "/src/generated:kernel.asc");
    assert(result.frames[0].line == 46);
    assert(result.frames[0].column == 5);
    fs::remove_all(directory);
}

void TestReportsRunnerFailure()
{
    const fs::path directory = TestDirectory("aclsan-device-symbolizer-runner-test");
    const DeviceSymbolizerConfig config{"/tools/llvm-symbolizer", "/work/kernel.elf", directory.string()};
    DeviceSymbolizer symbolizer(config);
    const auto runner = [](const std::vector<std::string>&, const std::string&, std::string& error) {
        error = "simulated failure";
        return false;
    };

    const CallStackResult result = symbolizer.ResolveCallStackWithRunner(0x190, runner);

    assert(!result.available);
    assert(result.error == "simulated failure");
    fs::remove_all(directory);
}

void TestRealRunnerFindsBareToolOnPath()
{
    const fs::path directory = TestDirectory("aclsan-device-symbolizer-real-runner-test");
    const fs::path tool = directory / "fake-symbolizer";
    {
        std::ofstream script(tool.string());
        script << "#!/bin/sh\n"
               << "printf 'RealFunction\\n/src/kernel.asc:21:4\\n'\n";
    }
    fs::permissions(tool, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exe);

    ScopedEnvironmentVariable pathGuard("PATH");
    std::string path = directory.string();
    if (const char* currentPath = std::getenv("PATH"); currentPath != nullptr && currentPath[0] != '\0') {
        path += ":" + std::string(currentPath);
    }
    assert(setenv("PATH", path.c_str(), 1) == 0);

    const DeviceSymbolizerConfig config{"fake-symbolizer", (directory / "kernel.elf").string(), directory.string()};
    DeviceSymbolizer symbolizer(config);
    const CallStackResult result = symbolizer.ResolveCallStack(0x1b0);

    assert(result.available);
    assert(result.error.empty());
    assert(result.frames.size() == 1);
    assert(result.frames[0].functionName == "RealFunction");
    assert(result.frames[0].fileName == "/src/kernel.asc");
    assert(result.frames[0].line == 21);
    assert(result.frames[0].column == 4);
    fs::remove_all(directory);
}

void TestRealRunnerReportsSpawnFailure()
{
    const fs::path directory = TestDirectory("aclsan-device-symbolizer-spawn-failure-test");
    const DeviceSymbolizerConfig config{
        "aclsan-symbolizer-that-does-not-exist", (directory / "kernel.elf").string(), directory.string()};
    DeviceSymbolizer symbolizer(config);

    const CallStackResult result = symbolizer.ResolveCallStack(0x1c0);

    assert(!result.available);
    assert(result.error.find("posix_spawnp") != std::string::npos);
    assert(result.error.find(std::strerror(ENOENT)) != std::string::npos);
    fs::remove_all(directory);
}

} // namespace

TEST(DeviceSymbolizer, Main)
{
    TestParsesFramesAndCachesByImageAndPc();
    TestRejectsInvalidSymbolizerOutput();
    TestParsesLocationFromTheRight();
    TestReportsRunnerFailure();
    TestRealRunnerFindsBareToolOnPath();
    TestRealRunnerReportsSpawnFailure();
}

TEST(DeviceSymbolizer, RejectsSymbolicLinkReturnedAsLog)
{
    const fs::path directory = TestDirectory("aclsan-symbolizer-log-link-test");
    const auto target = directory / "target.log";
    {
        std::ofstream output(target.string());
        output << "Function\n/src/kernel.asc:1:2\n";
    }
    DeviceSymbolizer symbolizer({"fake-symbolizer", "/work/kernel.elf", directory.string()});
    const auto runner = [&target](const std::vector<std::string>&, const std::string& logPath, std::string&) {
        fs::create_symlink(target, logPath);
        return true;
    };
    const auto result = symbolizer.ResolveCallStackWithRunner(0x123, runner);
    EXPECT_FALSE(result.available);
    EXPECT_EQ(result.error, "invalid_symbolizer_output");
    fs::remove_all(directory);
}
