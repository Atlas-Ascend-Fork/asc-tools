/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_INTERNAL_UDS_CLIENT_H
#define NPU_COMPUTE_INTERNAL_UDS_CLIENT_H
#include "ipc/ipc.h"
#include <atomic>
namespace npucompute::cli {
class ArtifactReceiver;
class UdsClient {
public:
    void Run(
        const ipc::Bootstrap& bootstrap, pid_t childPid, const ipc::Config& config, const std::string& directory,
        const std::atomic<bool>& childExited, const std::atomic<bool>& cancelled);
    size_t ArtifactCount() const { return artifact_count_; }
    bool Complete() const { return complete_; }
    const std::vector<std::string>& Issues() const { return issues_; }
    bool InterruptAttempted() const { return interrupt_attempted_; }

private:
    ipc::Channel Connect(
        const ipc::Bootstrap& bootstrap, pid_t childPid, const std::atomic<bool>& childExited,
        const std::atomic<bool>& cancelled);
    void Handshake(
        ipc::Channel& channel, const ipc::Bootstrap& bootstrap, const ipc::Config& config, pid_t targetPid,
        const std::atomic<bool>& childExited, const std::atomic<bool>& cancelled);
    void ReceiveFrames(
        ipc::Channel& channel, const ipc::Config& config, const std::string& directory,
        const std::atomic<bool>& childExited, const std::atomic<bool>& cancelled);
    bool FinishResult(
        ipc::Channel& channel, ArtifactReceiver& receiver, const ipc::Frame& frame, const ipc::Config& config,
        const std::atomic<bool>& cancelled);
    void WaitForResultClose(ipc::Channel& channel, const std::atomic<bool>& cancelled);

    size_t artifact_count_ = 0;
    bool complete_ = false;
    std::vector<std::string> issues_;
    std::atomic<bool> interrupt_attempted_{false};
};
} // namespace npucompute::cli
#endif // NPU_COMPUTE_INTERNAL_UDS_CLIENT_H
