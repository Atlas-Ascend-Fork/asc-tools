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

#include <boost/filesystem.hpp>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace npucompute::test {
namespace {
ProcessOptions Child(std::initializer_list<std::string> arguments)
{
    ProcessOptions options;
    options.arguments.push_back(NPU_COMPUTE_PROCESS_CHILD_PATH);
    options.arguments.insert(options.arguments.end(), arguments);
    return options;
}

TEST(NpuComputeTestProcess, CapturesOutputAndPassesArgumentsLiterally)
{
    auto options = Child({"arguments", "space separated", "$(echo expanded);*", ""});
    const auto result = RunProcess(options);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.launchError, 0);
    EXPECT_EQ(result.signal, 0);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.standardOutput, "space separated\n$(echo expanded);*\n\n");
    EXPECT_EQ(result.standardError, "child stderr\n");
}

TEST(NpuComputeTestProcess, PreservesNonzeroExitCode)
{
    const auto result = RunProcess(Child({"exit", "37"}));
    EXPECT_EQ(result.exitCode, 37);
    EXPECT_EQ(result.launchError, 0);
    EXPECT_EQ(result.signal, 0);
}

TEST(NpuComputeTestProcess, DistinguishesSignalFromExitCode)
{
    const auto result = RunProcess(Child({"signal"}));
    EXPECT_EQ(result.exitCode, -1);
    EXPECT_EQ(result.signal, SIGTERM);
    EXPECT_FALSE(result.timedOut);
}

TEST(NpuComputeTestProcess, DistinguishesMissingExecutableFromExit127)
{
    auto options = Child({});
    options.arguments[0] = "/npu-compute-missing-test-executable";
    const auto missing = RunProcess(options);
    EXPECT_EQ(missing.launchError, ENOENT);
    const auto exited = RunProcess(Child({"exit", "127"}));
    EXPECT_EQ(exited.exitCode, 127);
    EXPECT_EQ(exited.launchError, 0);
}

TEST(NpuComputeTestProcess, OverridesChildEnvironmentWithoutChangingParent)
{
    const char* inherited = std::getenv("PATH");
    const std::string original = inherited == nullptr ? "<unset>" : inherited;
    auto options = Child({"environment", "PATH"});
    options.environment["PATH"] = "child value";
    EXPECT_EQ(RunProcess(options).standardOutput, "child value");
    EXPECT_EQ(RunProcess(Child({"environment", "PATH"})).standardOutput, original);
}

TEST(NpuComputeTestProcess, CanUnsetChildEnvironment)
{
    auto options = Child({"environment", "PATH"});
    options.environment["PATH"] = std::nullopt;
    EXPECT_EQ(RunProcess(options).standardOutput, "<unset>");
}

TEST(NpuComputeTestProcess, ChangesOnlyChildWorkingDirectory)
{
    const auto original = boost::filesystem::current_path();
    TemporaryDirectory directory;
    auto options = Child({"directory"});
    options.workingDirectory = directory.Path();
    const auto result = RunProcess(options);
    ASSERT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.standardOutput, directory.Path());
    EXPECT_EQ(boost::filesystem::current_path(), original);
}

TEST(NpuComputeTestProcess, ReportsWorkingDirectorySetupFailure)
{
    auto options = Child({"exit", "0"});
    options.workingDirectory = "/npu-compute-missing-test-directory";
    EXPECT_EQ(RunProcess(options).launchError, ENOENT);
}

TEST(NpuComputeTestProcess, CapturesOutputLargerThanPipeBuffer)
{
    const auto result = RunProcess(Child({"large-output"}));
    ASSERT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.standardOutput, std::string(262144, 'x'));
    EXPECT_EQ(result.standardError, std::string(262144, 'x'));
}

TEST(NpuComputeTestProcess, TimesOutAndReapsChild)
{
    auto options = Child({"sleep"});
    options.timeout = std::chrono::milliseconds(100);
    const auto start = std::chrono::steady_clock::now();
    const auto result = RunProcess(options);
    EXPECT_TRUE(result.timedOut);
    EXPECT_EQ(result.signal, SIGKILL);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    int status = 0;
    errno = 0;
    EXPECT_EQ(::waitpid(result.pid, &status, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST(NpuComputeTestProcess, TerminatesDescendantsOnTimeout)
{
    auto options = Child({"descendant"});
    options.timeout = std::chrono::milliseconds(500);
    const auto result = RunProcess(options);
    ASSERT_TRUE(result.timedOut);
    ASSERT_FALSE(result.standardOutput.empty());
    const auto pid = std::stol(result.standardOutput);
    // An orphan may remain a zombie until init reaps it; it must not keep running.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream status("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (!std::getline(status, line)) {
            return;
        }
        const auto state = line.find_last_of(')');
        ASSERT_NE(state, std::string::npos);
        if (line.substr(state + 2, 1) == "Z") {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL() << "descendant still running after timeout: " << pid;
}

TEST(NpuComputeTestProcess, RejectsInvalidOptionsBeforeStartingChild)
{
    EXPECT_THROW(RunProcess(ProcessOptions{}), std::invalid_argument);
    auto options = Child({"exit", "0"});
    options.timeout = std::chrono::milliseconds(0);
    EXPECT_THROW(RunProcess(options), std::invalid_argument);
    options = Child({"exit", "0"});
    options.environment["INVALID=NAME"] = "value";
    EXPECT_THROW(RunProcess(options), std::invalid_argument);
}

TEST(NpuComputeTestProcess, RemovesTemporaryDirectoryAndContents)
{
    std::string path;
    {
        TemporaryDirectory directory;
        path = directory.Path();
        ASSERT_TRUE(boost::filesystem::is_directory(path));
        std::ofstream(path + "/output") << "test output";
    }
    EXPECT_FALSE(boost::filesystem::exists(path));
}
} // namespace
} // namespace npucompute::test
