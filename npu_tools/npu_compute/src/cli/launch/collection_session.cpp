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
            [&prefix](const auto& entry) { return entry.compare(0, prefix.size(), prefix) == 0; }),
        environment.end());
    environment.push_back(prefix + value);
}

struct ReceiverState {
    std::atomic<bool> childExited{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    bool success = false;
    std::string ipcError;
    std::string failureDomain;
    UdsClient client;
};

struct ChildResult {
    int appResult = 0;
    std::string appError;
    int signal = 0;
    bool terminated = false;
};

bool StartChild(
    ProcessHandle& child, ProcessLaunchRequest& request, const ipc::Bootstrap& bootstrap, CollectionOutcome& outcome,
    std::string* error, int* earlyResult)
{
    if (child.Start(request, error, bootstrap.handshakeDeadline)) {
        return true;
    }
    const int signal = child.PendingSignal();
    if (child.Pid() > 0) {
        child.Signal(SIGKILL);
        std::string reapError;
        child.Wait(&reapError);
        outcome.childExit = child.ExitDescription();
    }
    if (signal != 0) {
        outcome.outcome = "app_failed";
        *error = "collection interrupted during startup by signal " + std::to_string(signal);
        *earlyResult = SIGNAL_EXIT_CODE_BASE + signal;
    } else {
        *earlyResult = kInternalErrorExitCode;
    }
    return false;
}

void ReceiveCollection(
    const ipc::Bootstrap& bootstrap, ProcessHandle& child, const ipc::Config& config, const std::string& directory,
    ReceiverState& state)
{
    try {
        state.client.Run(bootstrap, child.Pid(), config, directory, state.childExited, state.cancelled);
        state.success = true;
    } catch (const ipc::Exception& exception) {
        state.failureDomain = exception.Detail().domain;
        state.ipcError = state.failureDomain + "/" + exception.Detail().phase +
                         " code=" + std::to_string(exception.Detail().code) + ": " + exception.what();
    } catch (const std::exception& exception) {
        state.ipcError = exception.what();
    } catch (...) {
        state.ipcError = "unexpected UDS failure";
    }
    state.done = true;
}

void WaitForInterruptAttempt(ReceiverState& state)
{
    const auto deadline = ipc::After(std::chrono::milliseconds(INTERRUPT_GRACE_PERIOD_MS));
    while (!state.done && !state.client.InterruptAttempted() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(RECEIVER_DRAIN_INTERVAL_MS));
    }
}

void MonitorChild(ProcessHandle& child, ReceiverState& state, ChildResult* result)
{
    auto killDeadline = ipc::Deadline::max();
    while (!child.Poll(&result->appResult, &result->appError)) {
        if (child.PendingSignal() != 0 && !result->signal) {
            result->signal = child.PendingSignal();
            state.cancelled = true;
            // Let the communication owner attempt INTERRUPT before forwarding the signal.
            WaitForInterruptAttempt(state);
            child.Signal(result->signal);
            result->terminated = true;
            killDeadline = ipc::After(std::chrono::seconds(1));
        }
        if (state.done && !state.success && !result->terminated) {
            child.Signal(SIGTERM);
            result->terminated = true;
            killDeadline = ipc::After(std::chrono::seconds(1));
        }
        if (result->terminated && std::chrono::steady_clock::now() >= killDeadline) {
            child.Signal(SIGKILL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(CHILD_POLL_INTERVAL_MS));
    }
    state.childExited = true;
    while (!state.done) {
        if (child.PendingSignal() != 0 && !result->signal) {
            result->signal = child.PendingSignal();
            state.cancelled = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECEIVER_DRAIN_INTERVAL_MS));
    }
}

int FinishSession(
    ProcessHandle& child, ReceiverState& state, ChildResult& result, CollectionOutcome& outcome, std::string* error)
{
    if (!result.signal) {
        result.signal = child.PendingSignal();
    }
    outcome.artifacts = state.client.ArtifactCount();
    outcome.complete = state.client.Complete() ? "1" : "0";
    outcome.childExit = child.ExitDescription();
    outcome.outcome = (state.failureDomain == "BOOTSTRAP" || state.failureDomain == "INTERNAL") ? "infra_failed" :
                                                                                                  "collection_failed";
    if (result.signal) {
        outcome.outcome = "app_failed";
        outcome.childExit = "signal:" + std::to_string(result.signal);
        *error = "collection interrupted by signal " + std::to_string(result.signal);
        return SIGNAL_EXIT_CODE_BASE + result.signal;
    }
    if (result.appResult != 0 && !result.terminated) {
        outcome.outcome = "app_failed";
        *error = result.appError;
        if (!state.ipcError.empty()) {
            *error += "; UDS collection failed: " + state.ipcError;
        }
        return result.appResult;
    }
    if (!state.success) {
        *error = "UDS collection failed: " + state.ipcError;
        return kCollectionErrorExitCode;
    }
    if (result.appResult != 0) {
        *error = result.appError;
        return result.appResult;
    }
    outcome.issues = state.client.Issues();
    outcome.outcome = state.client.Complete() ? "success" : "collection_failed";
    return 0;
}
} // namespace
ipc::Config LoadCollectionConfig(const CliConfig& config)
{
    ipc::Config result;
    result.sections = config.sections;
    result.replayMode = ReplayModeName(config.replay_mode);
    result.cacheControlMode =
        config.cache_control == CacheControl::All ? ipc::kCacheControlModeAll : ipc::kCacheControlModeNone;
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
    int earlyResult = 0;
    if (!StartChild(child, request, bootstrap, outcome_, error, &earlyResult)) {
        return earlyResult;
    }
    ReceiverState state;
    std::thread receiver([&bootstrap, &child, &config, &directory, &state] {
        ReceiveCollection(bootstrap, child, config, directory, state);
    });
    ChildResult result;
    MonitorChild(child, state, &result);
    receiver.join();
    return FinishSession(child, state, result, outcome_, error);
}
} // namespace npucompute::cli
