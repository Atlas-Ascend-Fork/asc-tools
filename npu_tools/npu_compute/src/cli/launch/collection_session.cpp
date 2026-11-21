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
#include <cmath>
#include <boost/filesystem.hpp>
#include <cerrno>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
namespace npucompute::cli {
namespace {
constexpr int SIGNAL_EXIT_CODE_BASE = 128;
constexpr int INTERRUPT_GRACE_PERIOD_MS = 200;
constexpr int RECEIVER_DRAIN_INTERVAL_MS = 5;
constexpr int CHILD_POLL_INTERVAL_MS = 10;

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

template <typename Receive>
void ReceiveCollection(ReceiverState& state, Receive receive)
{
    try {
        receive();
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
        if (child.PendingSignal() != 0 && result->signal == 0) {
            result->signal = child.PendingSignal();
            state.cancelled = true;
            // Let the communication owner attempt INTERRUPT before forwarding the signal.
            WaitForInterruptAttempt(state);
            child.Signal(result->signal);
            result->terminated = true;
            killDeadline = ipc::After(std::chrono::seconds(1));
        }
        if (state.done && !state.success && !state.client.Ready() && !result->terminated) {
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
        if (child.PendingSignal() != 0 && result->signal == 0) {
            result->signal = child.PendingSignal();
            state.cancelled = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECEIVER_DRAIN_INTERVAL_MS));
    }
}

int FinishSession(
    ProcessHandle& child, ReceiverState& state, ChildResult& result, CollectionOutcome& outcome, std::string* error)
{
    if (result.signal == 0) {
        result.signal = child.PendingSignal();
    }
    outcome.artifacts = state.client.ArtifactCount();
    outcome.complete = state.client.Complete() ? "1" : "0";
    outcome.childExit = child.ExitDescription();
    outcome.outcome = (state.failureDomain == "BOOTSTRAP" || state.failureDomain == "INTERNAL") ? "infra_failed" :
                                                                                                  "collection_failed";
    if (result.signal != 0) {
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
struct RankReceiver {
    ReceiverState state;
    boost::filesystem::path directory;
    std::thread worker;

    ~RankReceiver()
    {
        if (worker.joinable()) {
            state.cancelled = true;
            worker.join();
        }
    }
};

using RankReceivers = std::map<pid_t, RankReceiver>;

void StartRankReceiver(
    ipc::Channel channel, const ipc::Bootstrap& bootstrap, const ipc::Config& config,
    const boost::filesystem::path& directory, RankReceivers& ranks)
{
    const auto pid = channel.PeerPid();
    auto [entry, inserted] = ranks.try_emplace(pid);
    if (!inserted) {
        return;
    }
    auto& rank = entry->second;
    try {
        auto rankDirectory = directory / ("pid-" + std::to_string(pid));
        if (mkdir(rankDirectory.c_str(), S_IRWXU) != 0) {
            throw std::runtime_error("cannot create rank artifact directory");
        }
        rank.directory = std::move(rankDirectory);
        rank.worker = std::thread([&rank, &bootstrap, &config, channel = std::move(channel)]() mutable {
            ReceiveCollection(rank.state, [&] {
                rank.state.client.RunConnected(
                    std::move(channel), bootstrap, config, rank.directory.string(), rank.state.childExited,
                    rank.state.cancelled);
            });
        });
    } catch (const std::exception& failure) {
        rank.state.ipcError = failure.what();
        rank.state.done = true;
    }
}

void InterruptRanks(RankReceivers& ranks)
{
    for (auto& entry : ranks) {
        entry.second.state.cancelled = true;
    }
    const auto deadline = ipc::After(std::chrono::milliseconds(INTERRUPT_GRACE_PERIOD_MS));
    const auto interruptAttempted = [](const auto& entry) {
        const auto& state = entry.second.state;
        return state.done || state.client.InterruptAttempted();
    };
    while (std::chrono::steady_clock::now() < deadline &&
           !std::all_of(ranks.begin(), ranks.end(), interruptAttempted)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(RECEIVER_DRAIN_INTERVAL_MS));
    }
}

void CollectRanks(
    ipc::Listener& listener, const ipc::Bootstrap& bootstrap, const ipc::Config& config,
    const boost::filesystem::path& directory, ProcessHandle& child, RankReceivers& ranks, ChildResult& result,
    CollectionOutcome& outcome)
{
    auto killDeadline = ipc::Deadline::max();
    bool accepting = true;
    while (!child.Poll(&result.appResult, &result.appError)) {
        if (child.PendingSignal() != 0 && result.signal == 0) {
            result.signal = child.PendingSignal();
            InterruptRanks(ranks);
            child.Signal(result.signal);
            killDeadline = ipc::After(std::chrono::seconds(1));
            accepting = false;
        }
        if (std::chrono::steady_clock::now() >= killDeadline) {
            child.Signal(SIGKILL);
        }
        if (accepting) {
            try {
                auto channel = listener.TryAcceptDescendant(bootstrap.session, child.Pid(), getuid());
                if (channel.Fd() >= 0) {
                    StartRankReceiver(std::move(channel), bootstrap, config, directory, ranks);
                    continue;
                }
            } catch (const std::exception& failure) {
                outcome.issues.push_back(std::string("UDS accept failed: ") + failure.what());
                accepting = false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(CHILD_POLL_INTERVAL_MS));
    }
    for (auto& entry : ranks) {
        entry.second.state.childExited = true;
    }
    while (!std::all_of(ranks.begin(), ranks.end(), [](const auto& entry) { return entry.second.state.done.load(); })) {
        if (child.PendingSignal() != 0 && result.signal == 0) {
            result.signal = child.PendingSignal();
            InterruptRanks(ranks);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RECEIVER_DRAIN_INTERVAL_MS));
    }
}

void FlattenSingleRank(const boost::filesystem::path& rankDirectory, const boost::filesystem::path& directory)
{
    namespace fs = boost::filesystem;
    std::vector<fs::path> names;
    for (const auto& file : fs::directory_iterator(rankDirectory)) {
        names.push_back(file.path().filename());
    }
    std::size_t moved = 0;
    try {
        for (; moved < names.size(); ++moved) {
            fs::rename(rankDirectory / names[moved], directory / names[moved]);
        }
        fs::remove(rankDirectory);
    } catch (...) {
        while (moved > 0) {
            const auto& name = names[--moved];
            boost::system::error_code error;
            fs::rename(directory / name, rankDirectory / name, error);
        }
        throw;
    }
}

void FinishRankCollection(
    pid_t pid, RankReceiver& rank, const boost::filesystem::path& directory, bool singleRank,
    CollectionOutcome& outcome)
{
    if (rank.worker.joinable()) {
        rank.worker.join();
    }
    const auto prefix = "pid-" + std::to_string(pid) + ": ";
    const auto& client = rank.state.client;
    const auto artifacts = client.ArtifactCount();
    const auto& issues = client.Issues();
    outcome.artifacts += artifacts;
    for (const auto& issue : issues) {
        outcome.issues.push_back(prefix + issue);
    }
    if ((!rank.state.success || !client.Complete()) && (!rank.state.ipcError.empty() || issues.empty())) {
        outcome.issues.push_back(prefix + (rank.state.ipcError.empty() ? "incomplete result" : rank.state.ipcError));
    }
    if (rank.directory.empty()) {
        return;
    }
    try {
        if (artifacts == 0) {
            boost::filesystem::remove_all(rank.directory);
        } else if (singleRank) {
            FlattenSingleRank(rank.directory, directory);
        }
    } catch (const std::exception& failure) {
        outcome.issues.push_back(prefix + failure.what());
    }
}

int RunApplicationCollection(
    ipc::Listener& listener, const ipc::Bootstrap& bootstrap, const ipc::Config& config,
    const boost::filesystem::path& directory, ProcessHandle& child, CollectionOutcome& outcome, std::string* error)
{
    RankReceivers ranks;
    ChildResult result;
    CollectRanks(listener, bootstrap, config, directory, child, ranks, result, outcome);
    for (auto& [pid, rank] : ranks) {
        FinishRankCollection(pid, rank, directory, ranks.size() == 1, outcome);
    }
    if (ranks.empty()) {
        outcome.issues.push_back("UDS: target command exited before a collection process connected");
    }
    outcome.complete = outcome.issues.empty() ? "1" : "0";
    outcome.childExit = child.ExitDescription();
    if (result.signal == 0) {
        result.signal = child.PendingSignal();
    }
    if (result.signal != 0) {
        outcome.outcome = "app_failed";
        outcome.childExit = "signal:" + std::to_string(result.signal);
        *error = "collection interrupted by signal " + std::to_string(result.signal);
        return SIGNAL_EXIT_CODE_BASE + result.signal;
    }
    if (result.appResult != 0) {
        outcome.outcome = "app_failed";
        *error = result.appError;
        return result.appResult;
    }
    if (outcome.issues.empty()) {
        outcome.outcome = "success";
        return 0;
    }
    outcome.outcome = "collection_failed";
    *error = "UDS collection failed: " + outcome.issues.front();
    return kCollectionErrorExitCode;
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
    request.SetEnvironment("NPU_COMPUTE_UDS_NAME", bootstrap.name);
    request.SetEnvironment("NPU_COMPUTE_SESSION_ID", std::to_string(bootstrap.session));
    request.SetEnvironment("NPU_COMPUTE_CLI_PID", std::to_string(bootstrap.cliPid));
    request.SetEnvironment("NPU_COMPUTE_HANDSHAKE_TIMEOUT_MS", std::to_string(bootstrap.timeoutMs));
    const bool application = config.replayMode == "application";
    std::unique_ptr<ipc::Listener> listener;
    if (application) {
        request.SetEnvironment("NPU_COMPUTE_REPLAY_MODE", config.replayMode);
        try {
            listener = std::make_unique<ipc::Listener>(bootstrap.name);
        } catch (const std::exception& failure) {
            *error = std::string("cannot listen for collection processes: ") + failure.what();
            return kInternalErrorExitCode;
        }
    }
    bootstrap.handshakeDeadline = ipc::After(std::chrono::milliseconds(bootstrap.timeoutMs));
    ProcessHandle child;
    int earlyResult = 0;
    if (!StartChild(child, request, bootstrap, outcome_, error, &earlyResult)) {
        return earlyResult;
    }
    if (application) {
        return RunApplicationCollection(*listener, bootstrap, config, directory, child, outcome_, error);
    }
    ReceiverState state;
    std::thread receiver;
    try {
        receiver = std::thread([&] {
            ReceiveCollection(state, [&] {
                state.client.Run(bootstrap, child.Pid(), config, directory, state.childExited, state.cancelled);
            });
        });
    } catch (const std::exception& failure) {
        child.Signal(SIGKILL);
        std::string reapError;
        child.Wait(&reapError);
        outcome_.outcome = "infra_failed";
        outcome_.childExit = child.ExitDescription();
        *error = "cannot start collection receiver: " + std::string(failure.what());
        return kInternalErrorExitCode;
    }
    ChildResult result;
    MonitorChild(child, state, &result);
    receiver.join();
    return FinishSession(child, state, result, outcome_, error);
}
} // namespace npucompute::cli
