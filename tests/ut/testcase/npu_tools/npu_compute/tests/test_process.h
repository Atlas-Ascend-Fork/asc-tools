/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software; you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_TEST_PROCESS_H
#define NPU_COMPUTE_TEST_PROCESS_H

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace npucompute::test {

struct ProcessOptions {
    // argv[0] must be an absolute executable path. Arguments are never passed to a shell.
    std::vector<std::string> arguments;
    std::map<std::string, std::optional<std::string>> environment;
    std::string workingDirectory;
    std::chrono::milliseconds timeout{120000};
};

struct ProcessResult {
    pid_t pid = -1;
    int exitCode = -1;
    int signal = 0;
    int launchError = 0;
    bool timedOut = false;
    std::string standardOutput;
    std::string standardError;
};

ProcessResult RunProcess(const ProcessOptions& options);

class TemporaryDirectory {
public:
    TemporaryDirectory();
    ~TemporaryDirectory();
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::string& Path() const { return path_; }

private:
    std::string path_;
};

} // namespace npucompute::test
#endif
