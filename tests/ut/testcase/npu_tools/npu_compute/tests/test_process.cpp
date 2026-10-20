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
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace npucompute::test {
namespace {
struct CloseFile {
    void operator()(FILE* file) const { std::fclose(file); }
};
using File = std::unique_ptr<FILE, CloseFile>;

void ThrowSystemError(const char* operation) { throw std::system_error(errno, std::generic_category(), operation); }

File CaptureFile()
{
    File file(std::tmpfile());
    if (!file || ::fcntl(::fileno(file.get()), F_SETFD, FD_CLOEXEC) < 0) {
        ThrowSystemError("create process capture file");
    }
    return file;
}

std::string ReadOutput(FILE* file)
{
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        ThrowSystemError("rewind process output");
    }
    std::string output;
    char buffer[8192];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        output.append(buffer, count);
    }
    if (std::ferror(file)) {
        ThrowSystemError("read process output");
    }
    return output;
}

std::vector<char*> Pointers(std::vector<std::string>& values)
{
    std::vector<char*> pointers;
    for (auto& value : values) {
        pointers.push_back(value.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

bool HasNull(const std::string& value) { return value.find('\0') != std::string::npos; }

void Validate(const ProcessOptions& options)
{
    if (options.arguments.empty() || options.arguments.front().empty() || options.arguments.front()[0] != '/' ||
        options.timeout.count() <= 0 || HasNull(options.workingDirectory)) {
        throw std::invalid_argument("process requires an absolute executable path and a positive timeout");
    }
    for (const auto& argument : options.arguments) {
        if (HasNull(argument)) {
            throw std::invalid_argument("process argument contains a null byte");
        }
    }
    for (const auto& entry : options.environment) {
        if (entry.first.empty() || entry.first.find('=') != std::string::npos || HasNull(entry.first) ||
            (entry.second && HasNull(*entry.second))) {
            throw std::invalid_argument("invalid process environment entry");
        }
    }
}

std::vector<std::string> Environment(const ProcessOptions& options)
{
    std::map<std::string, std::string> entries;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string value(*entry);
        const auto separator = value.find('=');
        if (separator != std::string::npos) {
            entries[value.substr(0, separator)] = value.substr(separator + 1);
        }
    }
    for (const auto& entry : options.environment) {
        if (entry.second) {
            entries[entry.first] = *entry.second;
        } else {
            entries.erase(entry.first);
        }
    }
    std::vector<std::string> values;
    for (const auto& entry : entries) {
        values.push_back(entry.first + "=" + entry.second);
    }
    return values;
}

[[noreturn]] void ChildError(int descriptor)
{
    const int error = errno;
    ssize_t written;
    do {
        written = ::write(descriptor, &error, sizeof(error));
    } while (written < 0 && errno == EINTR);
    ::_exit(127);
}
} // namespace

ProcessResult RunProcess(const ProcessOptions& options)
{
    Validate(options);
    auto arguments = options.arguments;
    auto environment = Environment(options);
    auto argv = Pointers(arguments);
    auto envp = Pointers(environment);
    auto output = CaptureFile();
    auto error = CaptureFile();
    File input(std::fopen("/dev/null", "re"));
    if (!input) {
        ThrowSystemError("open process input");
    }
    int errors[2];
    if (::pipe2(errors, O_CLOEXEC) != 0) {
        ThrowSystemError("create process launch pipe");
    }
    // Prepare all strings and descriptors before fork. The child only uses async-signal-safe operations.
    const int outputDescriptor = ::fileno(output.get());
    const int errorDescriptor = ::fileno(error.get());
    const int inputDescriptor = ::fileno(input.get());
    const char* workingDirectory = options.workingDirectory.c_str();
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    const pid_t child = ::fork();
    if (child < 0) {
        const int saved = errno;
        ::close(errors[0]);
        ::close(errors[1]);
        errno = saved;
        ThrowSystemError("fork test process");
    }
    if (child == 0) {
        ::close(errors[0]);
        if (::setpgid(0, 0) != 0 || ::dup2(inputDescriptor, STDIN_FILENO) < 0 ||
            ::dup2(outputDescriptor, STDOUT_FILENO) < 0 || ::dup2(errorDescriptor, STDERR_FILENO) < 0 ||
            (workingDirectory[0] != '\0' && ::chdir(workingDirectory) != 0)) {
            ChildError(errors[1]);
        }
        ::execve(argv[0], argv.data(), envp.data());
        ChildError(errors[1]);
    }
    ::close(errors[1]);
    // Also establish the group in the parent, closing the timeout race before the child calls setpgid.
    ::setpgid(child, child);
    ProcessResult result;
    result.pid = child;
    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            break;
        }
        if (waited < 0 && errno != EINTR) {
            const int saved = errno;
            ::kill(-child, SIGKILL);
            ::close(errors[0]);
            throw std::system_error(saved, std::generic_category(), "wait for test process");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            ::kill(-child, SIGKILL);
            ::kill(child, SIGKILL);
            pid_t reaped;
            do {
                reaped = ::waitpid(child, &status, 0);
            } while (reaped < 0 && errno == EINTR);
            if (reaped < 0) {
                const int saved = errno;
                ::close(errors[0]);
                throw std::system_error(saved, std::generic_category(), "reap timed out test process");
            }
            break;
        }
        const timespec interval{0, 10000000};
        ::nanosleep(&interval, nullptr);
    }
    // Do not leave descendants running after the scenario's main process exits.
    ::kill(-child, SIGKILL);
    ssize_t count;
    do {
        count = ::read(errors[0], &result.launchError, sizeof(result.launchError));
    } while (count < 0 && errno == EINTR);
    const int saved = errno;
    ::close(errors[0]);
    if (count < 0) {
        throw std::system_error(saved, std::generic_category(), "read process launch status");
    }
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }
    result.standardOutput = ReadOutput(output.get());
    result.standardError = ReadOutput(error.get());
    return result;
}

TemporaryDirectory::TemporaryDirectory()
{
    auto pattern = (boost::filesystem::temp_directory_path() / "npu-compute-ut-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) {
        ThrowSystemError("create test directory");
    }
    path_ = std::move(pattern);
}

TemporaryDirectory::~TemporaryDirectory()
{
    boost::system::error_code error;
    boost::filesystem::remove_all(path_, error);
}
} // namespace npucompute::test
