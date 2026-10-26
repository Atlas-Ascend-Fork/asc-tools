/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "collection_session.h"
#include "ipc/npu_compute_uds_client.h"
#include "launch/launcher.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <signal.h>
namespace npucompute::cli {
namespace {
constexpr int SIGNAL_EXIT_CODE_BASE = 128;
constexpr int INTERRUPT_GRACE_PERIOD_MS = 200;
constexpr int RECEIVER_DRAIN_INTERVAL_MS = 5;
constexpr int CHILD_POLL_INTERVAL_MS = 10;

void SetEnvironment(const std::string& name, const std::string& value, std::vector<std::string>& environment)
{
    const auto prefix = name + "=";
    environment.erase(
        std::remove_if(
            environment.begin(), environment.end(),
            [&](const auto& entry) { return entry.compare(0, prefix.size(), prefix) == 0; }),
        environment.end());
    environment.push_back(prefix + value);
}
} // namespace
ipc::Config LoadCollectionConfig(const CliConfig& config)
{
    ipc::Config result;
    result.sections = config.sections;
    result.replayMode = ReplayModeName(config.replay_mode);
    result.cacheControlMode =
        config.cache_control == CacheControl::All ? ipc::kCacheControlModeAll : ipc::kCacheControlModeNone;
    if (const char* level = std::getenv("NPU_COMPUTE_PMU_LEVEL"); level != nullptr && *level != '\0') {
        result.pmuLevel = level;
    }
    ipc::ValidateConfig(result);
    return result;
}
int CollectionSession::Run(
    ProcessLaunchRequest request, const ipc::Config& config, const std::string& directory, std::string* error)
{
    auto bootstrap = ipc::MakeBootstrap();
    SetEnvironment("NPU_COMPUTE_UDS_NAME", bootstrap.name, request.environment);
    SetEnvironment("NPU_COMPUTE_SESSION_ID", std::to_string(bootstrap.session), request.environment);
    SetEnvironment("NPU_COMPUTE_CLI_PID", std::to_string(bootstrap.cliPid), request.environment);
    SetEnvironment("NPU_COMPUTE_HANDSHAKE_TIMEOUT_MS", std::to_string(bootstrap.timeoutMs), request.environment);
    bootstrap.handshakeDeadline = ipc::After(std::chrono::milliseconds(bootstrap.timeoutMs));
    ProcessHandle child;
    if (!child.Start(request, error, bootstrap.handshakeDeadline)) {
        const int signal = child.PendingSignal();
        if (child.Pid() > 0) {
            child.Signal(SIGKILL);
            std::string reapError;
            child.Wait(&reapError);
            outcome_.childExit = child.ExitDescription();
        }
        if (signal != 0) {
            outcome_.outcome = "app_failed";
            *error = "collection interrupted during startup by signal " + std::to_string(signal);
            return SIGNAL_EXIT_CODE_BASE + signal;
        }
        return kInternalErrorExitCode;
    }
    std::atomic<bool> childExited{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    bool success = false;
    std::string ipcError;
    UdsClient client;
    std::string failureDomain;
    std::thread receiver([&] {
        try {
            client.Run(bootstrap, child.Pid(), config, directory, childExited, cancelled);
            success = true;
        } catch (const ipc::Exception& exception) {
            failureDomain = exception.Detail().domain;
            ipcError = failureDomain + "/" + exception.Detail().phase +
                       " code=" + std::to_string(exception.Detail().code) + ": " + exception.what();
        } catch (const std::exception& exception) {
            ipcError = exception.what();
        } catch (...) {
            ipcError = "unexpected UDS failure";
        }
        done = true;
    });
    int appResult = 0;
    std::string appError;
    int signal = 0;
    bool terminated = false;
    auto killDeadline = ipc::Deadline::max();
    while (!child.Poll(&appResult, &appError)) {
        if (child.PendingSignal() != 0 && !signal) {
            signal = child.PendingSignal();
            cancelled = true;
            // Wait for the communication owner to attempt INTERRUPT. A stuck
            // filesystem must not indefinitely delay forwarding the signal.
            const auto interruptDeadline = ipc::After(std::chrono::milliseconds(INTERRUPT_GRACE_PERIOD_MS));
            while (!done && !client.InterruptAttempted() && std::chrono::steady_clock::now() < interruptDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(RECEIVER_DRAIN_INTERVAL_MS));
            }
            child.Signal(signal);
            terminated = true;
            killDeadline = ipc::After(std::chrono::seconds(1));
        }
        if (done && !success && !terminated) {
            child.Signal(SIGTERM);
            terminated = true;
            killDeadline = ipc::After(std::chrono::seconds(1));
        }
        if (terminated && std::chrono::steady_clock::now() >= killDeadline) {
            child.Signal(SIGKILL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(CHILD_POLL_INTERVAL_MS));
    }
    childExited = true;
    // Reaping the child does not end the session: retain signal handling while
    // the receiver finishes committing artifacts and checking the final close.
    while (!done) {
        if (child.PendingSignal() != 0 && !signal) {
            signal = child.PendingSignal();
            cancelled = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECEIVER_DRAIN_INTERVAL_MS));
    }
    receiver.join();
    if (!signal) {
        signal = child.PendingSignal();
    }
    outcome_.artifacts = client.ArtifactCount();
    outcome_.complete = client.Complete() ? "1" : "0";
    outcome_.childExit = child.ExitDescription();
    outcome_.outcome =
        (failureDomain == "BOOTSTRAP" || failureDomain == "INTERNAL") ? "infra_failed" : "collection_failed";
    if (signal) {
        outcome_.outcome = "app_failed";
        outcome_.childExit = "signal:" + std::to_string(signal);
        *error = "collection interrupted by signal " + std::to_string(signal);
        return SIGNAL_EXIT_CODE_BASE + signal;
    }
    if (appResult != 0 && !terminated) {
        outcome_.outcome = "app_failed";
        *error = appError;
        if (!ipcError.empty()) {
            *error += "; UDS collection failed: " + ipcError;
        }
        return appResult;
    }
    if (!success) {
        *error = "UDS collection failed: " + ipcError;
        return kCollectionErrorExitCode;
    }
    if (appResult != 0) {
        *error = appError;
        return appResult;
    }
    outcome_.issues = client.Issues();
    outcome_.outcome = client.Complete() ? "success" : "collection_failed";
    return 0;
}
} // namespace npucompute::cli
