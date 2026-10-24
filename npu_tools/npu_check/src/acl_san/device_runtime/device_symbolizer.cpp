/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "device_symbolizer.h"

#include <sys/stat.h>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <spawn.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace aclsan::device_runtime {
namespace {

std::string HexPc(uint64_t pc)
{
    std::ostringstream output;
    output << "0x" << std::hex << pc;
    return output.str();
}

std::string CommandText(const std::vector<std::string>& command)
{
    std::ostringstream output;
    for (size_t index = 0; index < command.size(); ++index) {
        output << (index == 0 ? "" : " ") << command[index];
    }
    return output.str();
}

bool RunCommand(const std::vector<std::string>& command, const std::string& logPath, std::string& error)
{
    if (command.empty()) {
        error = "empty command";
        return false;
    }
    std::vector<char*> arguments;
    arguments.reserve(command.size() + 1);
    for (const std::string& argument : command) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    const char* failureContext = "posix_spawn_file_actions_init";
    int spawnStatus = posix_spawn_file_actions_init(&actions);
    const bool actionsInitialized = spawnStatus == 0;
    if (spawnStatus == 0) {
        failureContext = "posix_spawn_file_actions_addopen";
        spawnStatus = posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, (S_IRUSR | S_IWUSR));
    }
    if (spawnStatus == 0) {
        failureContext = "posix_spawn_file_actions_adddup2";
        spawnStatus = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    }

    pid_t child = -1;
    if (spawnStatus == 0) {
        failureContext = "posix_spawnp";
        spawnStatus = posix_spawnp(&child, arguments.front(), &actions, nullptr, arguments.data(), environ);
    }
    if (actionsInitialized) {
        (void)posix_spawn_file_actions_destroy(&actions);
    }
    if (spawnStatus != 0) {
        error = std::string(failureContext) + " failed: " + std::strerror(spawnStatus);
        return false;
    }

    int processStatus = 0;
    while (waitpid(child, &processStatus, 0) < 0) {
        if (errno != EINTR) {
            error = "waitpid failed: " + std::string(std::strerror(errno));
            return false;
        }
    }
    if (!WIFEXITED(processStatus) || WEXITSTATUS(processStatus) != 0) {
        error = "command failed: " + CommandText(command) + ", log=" + logPath;
        return false;
    }
    return true;
}

std::string NormalizeError(const std::string& error)
{
    std::string normalized = error.empty() ? "command_failed" : error;
    for (char& character : normalized) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            character = ' ';
        }
    }
    return normalized;
}

bool ParseUint32(const std::string& text, uint32_t& value)
{
    if (text.empty()) {
        return false;
    }
    uint64_t parsed = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        parsed = parsed * 10 + static_cast<uint32_t>(character - '0');
        if (parsed > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseLocation(const std::string& source, std::string& fileName, uint32_t& line, uint32_t& column)
{
    const size_t columnSeparator = source.rfind(':');
    if (columnSeparator == std::string::npos || columnSeparator == 0) {
        return false;
    }
    const size_t lineSeparator = source.rfind(':', columnSeparator - 1);
    if (lineSeparator == std::string::npos || lineSeparator == 0) {
        return false;
    }
    if (!ParseUint32(source.substr(lineSeparator + 1, columnSeparator - lineSeparator - 1), line) ||
        !ParseUint32(source.substr(columnSeparator + 1), column)) {
        return false;
    }
    fileName = source.substr(0, lineSeparator);
    return !fileName.empty();
}

bool ReadFrames(const std::string& logPath, std::vector<CallStackFrame>& frames)
{
    std::ifstream input(logPath);
    if (!input) {
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.empty() || lines.size() % 2 != 0) {
        return false;
    }

    frames.clear();
    frames.reserve(lines.size() / 2);
    for (size_t index = 0; index < lines.size(); index += 2) {
        if (lines[index] == "??" || lines[index + 1].find("??") == 0) {
            frames.clear();
            return false;
        }
        CallStackFrame frame;
        frame.functionName = lines[index];
        if (!ParseLocation(lines[index + 1], frame.fileName, frame.line, frame.column)) {
            frames.clear();
            return false;
        }
        frame.inlineDepth = static_cast<uint32_t>(frames.size());
        frames.push_back(std::move(frame));
    }
    return !frames.empty();
}

CallStackResult Unavailable(uint64_t pc, std::string error)
{
    CallStackResult result;
    result.pc = pc;
    result.error = std::move(error);
    return result;
}

} // namespace

DeviceSymbolizer::DeviceSymbolizer(DeviceSymbolizerConfig config) : config_(std::move(config)) {}

CallStackResult DeviceSymbolizer::ResolveCallStack(uint64_t pc) const
{
    return ResolveCallStackWithRunner(pc, RunCommand);
}

CallStackResult DeviceSymbolizer::ResolveCallStackWithRunner(uint64_t pc, const CommandRunner& runner) const
{
    if (config_.symbolizer.empty() || config_.image.empty() || config_.workDirectory.empty() || !runner) {
        return Unavailable(pc, "invalid_configuration");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const FrameCacheKey key{config_.image, pc};
    const auto cached = cache_.find(key);
    if (cached != cache_.end()) {
        return cached->second;
    }

    const std::string pcText = HexPc(pc);
    const std::string logPath = config_.workDirectory + "/symbolizer_" + pcText + ".log";
    const std::vector<std::string> command{config_.symbolizer, "--obj=" + config_.image, "--inlines",
                                           "--demangle",       "--functions=short",      pcText};

    std::string error;
    CallStackResult result;
    result.pc = pc;
    if (!runner(command, logPath, error)) {
        result.error = NormalizeError(error);
    } else if (!ReadFrames(logPath, result.frames)) {
        result.error = "invalid_symbolizer_output";
    } else {
        result.available = true;
    }
    cache_.emplace(key, result);
    return result;
}

void DeviceSymbolizer::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

} // namespace aclsan::device_runtime
