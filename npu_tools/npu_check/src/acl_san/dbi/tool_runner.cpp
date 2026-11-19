/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "dbi/tool_runner.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace aclsan {
namespace {
constexpr int SIGNAL_EXIT_CODE_BASE = 128;

std::string ReadPipe(int fd)
{
    std::string output;
    char buffer[4096];
    while (true) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return output;
}

bool IsInjectionEnvironment(const char* value)
{
    return std::strncmp(value, "LD_PRELOAD=", sizeof("LD_PRELOAD=") - 1) == 0 ||
           std::strncmp(value, "ACL_API_INJECTION=", sizeof("ACL_API_INJECTION=") - 1) == 0;
}

int SpawnWithCapturedOutput(
    pid_t& child, const std::vector<char*>& argv, const std::vector<char*>& environment, const int (&outputPipe)[2],
    const int (&errorPipe)[2])
{
    posix_spawn_file_actions_t actions;
    int status = posix_spawn_file_actions_init(&actions);
    if (status != 0) {
        return status;
    }
    status = posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
    if (status == 0)
        status = posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    for (const int fd : {outputPipe[0], errorPipe[0], outputPipe[1], errorPipe[1]}) {
        if (status != 0) {
            break;
        }
        status = posix_spawn_file_actions_addclose(&actions, fd);
    }
    if (status == 0) {
        status = posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environment.data());
    }
    (void)posix_spawn_file_actions_destroy(&actions);
    return status;
}

} // namespace

ToolResult RunTool(const std::vector<std::string>& commandArguments)
{
    ToolResult result{};
    if (commandArguments.empty() || commandArguments.front().empty()) {
        result.standardError = "tool command is empty";
        return result;
    }

    int outputPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if (pipe2(outputPipe, O_CLOEXEC) != 0 || pipe2(errorPipe, O_CLOEXEC) != 0) {
        result.standardError = std::string("pipe failed: ") + std::strerror(errno);
        if (outputPipe[0] >= 0) {
            close(outputPipe[0]);
            close(outputPipe[1]);
        }
        if (errorPipe[0] >= 0) {
            close(errorPipe[0]);
            close(errorPipe[1]);
        }
        return result;
    }

    auto argumentStorage = commandArguments;
    const auto argv = MakeSpawnArguments(argumentStorage);
    std::vector<std::string> environmentStorage;
    for (char** item = environ; item != nullptr && *item != nullptr; ++item) {
        if (!IsInjectionEnvironment(*item)) {
            environmentStorage.emplace_back(*item);
        }
    }
    const auto environment = MakeSpawnArguments(environmentStorage);

    pid_t child = -1;
    const int spawnStatus = SpawnWithCapturedOutput(child, argv, environment, outputPipe, errorPipe);
    if (spawnStatus != 0) {
        result.standardError = std::string("posix_spawnp failed: ") + std::strerror(spawnStatus);
        close(outputPipe[0]);
        close(outputPipe[1]);
        close(errorPipe[0]);
        close(errorPipe[1]);
        return result;
    }

    close(outputPipe[1]);
    close(errorPipe[1]);
    std::string standardOutput;
    std::string standardError;
    std::thread outputReader([&standardOutput, &outputPipe] { standardOutput = ReadPipe(outputPipe[0]); });
    std::thread errorReader([&standardError, &errorPipe] { standardError = ReadPipe(errorPipe[0]); });
    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    outputReader.join();
    errorReader.join();
    result.standardOutput = std::move(standardOutput);
    result.standardError = std::move(standardError);
    if (waited < 0) {
        if (!result.standardError.empty()) {
            result.standardError.push_back('\n');
        }
        result.standardError += std::string("waitpid failed: ") + std::strerror(errno);
    } else {
        const auto childStatus = DecodeChildExitStatus(status);
        if (childStatus.exited) {
            result.exitCode = childStatus.exitCode;
        } else if (childStatus.signaled) {
            result.exitCode = SIGNAL_EXIT_CODE_BASE + childStatus.signalNumber;
        }
    }
    return result;
}

} // namespace aclsan
