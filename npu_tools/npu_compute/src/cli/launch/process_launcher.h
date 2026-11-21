/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_LAUNCH_PROCESS_LAUNCHER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_LAUNCH_PROCESS_LAUNCHER_H

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <sys/types.h>

namespace npucompute::cli {

struct ProcessLaunchRequest {
    std::string program;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;

    void SetEnvironment(const std::string& name, const std::string& value);
};

class ProcessHandle {
public:
    ProcessHandle();
    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    bool Start(
        const ProcessLaunchRequest& request, std::string* error,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
    std::string ExitDescription() const;
    bool Poll(int* exitCode, std::string* error);
    int Wait(std::string* error);
    pid_t Pid() const;
    void Signal(int number) const noexcept;
    int PendingSignal() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_LAUNCH_PROCESS_LAUNCHER_H
