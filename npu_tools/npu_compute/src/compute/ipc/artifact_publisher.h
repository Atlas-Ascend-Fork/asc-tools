/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_ARTIFACT_PUBLISHER_H
#define NPU_COMPUTE_ARTIFACT_PUBLISHER_H
#include "artifact_sink.h"
#include "ipc/ipc.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
namespace npucompute {
class ArtifactPublisher final : public ArtifactSink {
public:
    ArtifactPublisher();
    explicit ArtifactPublisher(ipc::Channel channel);
    ArtifactPublisher(ipc::Channel channel, ipc::Deadline readyDeadline);
    ~ArtifactPublisher() override;
    void Attach(ipc::Channel channel);
    void SendReady(ipc::Deadline deadline);
    void FailStartup(ipc::Error error) noexcept;
    void Begin(const std::string& name) override;
    void Write(std::string_view bytes) override;
    void Commit(uint64_t records) override;
    void Abort() noexcept override;
    void PublishOptional(const std::string& name, std::string_view contents, uint64_t records) override;
    void RequestFinish(ipc::Result result);
    bool WaitForFinish() noexcept;
    bool Finish(ipc::Result result);
    void Stop() noexcept;
    bool Cancelled() const { return interrupted_ || failed_ || stopping_; }

private:
    ipc::ArtifactBegin PrepareArtifact(const std::string& name) const;
    enum class StartupAction { Wait, Publish, Ready };
    bool StartPublishing();
    void SendFailure(const ipc::Error& error) noexcept;
    void MarkFailed();
    void JoinWorker() noexcept;
    void Enqueue(ipc::Frame frame);
    void Worker() noexcept;
    void PollControl();
    bool TakeNextFrame(ipc::Frame& frame);
    void WaitForResultAck();
    void HandleControl(const ipc::Frame& frame);
    bool SendCancelled() const;
    ipc::Deadline SendDeadline();
    ipc::Channel channel_;
    std::thread worker_;
    std::mutex queue_mutex_;
    std::condition_variable available_;
    std::deque<ipc::Frame> queue_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> interrupted_{false};
    std::atomic<int64_t> drain_deadline_ms_{0};
    bool finishing_ = false;
    bool flush_requested_ = false;
    bool channel_attached_ = false;
    bool started_ = false;
    StartupAction startup_action_ = StartupAction::Wait;
    ipc::Deadline ready_deadline_{};
    std::optional<ipc::Error> startup_error_;
    std::mutex join_mutex_;
    bool acknowledged_ = false;
    std::mutex artifact_mutex_;
    std::unique_lock<std::mutex> artifact_lock_;
    ipc::ArtifactBegin current_;
    uint64_t bytes_ = 0;
    std::vector<ipc::ManifestEntry> manifest_;
};
} // namespace npucompute
#endif
