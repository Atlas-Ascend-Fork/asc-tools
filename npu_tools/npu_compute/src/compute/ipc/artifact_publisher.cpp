/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "artifact_publisher.h"
#include "npu_compute_log.h"
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <poll.h>
#include <unistd.h>
namespace npucompute {
namespace {
ipc::Deadline IoDeadline() { return ipc::After(ipc::kIoTimeout); }
} // namespace
ArtifactPublisher::ArtifactPublisher()
{
    worker_ = std::thread([this] { Worker(); });
}

ArtifactPublisher::ArtifactPublisher(ipc::Channel channel) : ArtifactPublisher()
{
    Attach(std::move(channel));
    std::lock_guard<std::mutex> lock(queue_mutex_);
    startup_action_ = StartupAction::Publish;
    available_.notify_all();
}

ArtifactPublisher::ArtifactPublisher(ipc::Channel channel, ipc::Deadline readyDeadline) : ArtifactPublisher()
{
    Attach(std::move(channel));
    SendReady(readyDeadline);
}

ArtifactPublisher::~ArtifactPublisher() { Stop(); }

void ArtifactPublisher::Attach(ipc::Channel channel)
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (channel_attached_ || channel.Fd() < 0 || stopping_) {
        throw std::runtime_error("publisher cannot attach channel");
    }
    channel_ = std::move(channel);
    channel_attached_ = true;
}

void ArtifactPublisher::SendReady(ipc::Deadline deadline)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (!channel_attached_ || startup_action_ != StartupAction::Wait || stopping_) {
        throw std::runtime_error("publisher cannot send READY in current state");
    }
    startup_action_ = StartupAction::Ready;
    ready_deadline_ = deadline;
    available_.notify_all();
    // Channel::Send enforces the deadline. Wait for its result so a successful
    // READY cannot race a second deadline on this thread.
    available_.wait(lock, [this] { return started_ || failed_ || stopping_; });
    if (!started_) {
        throw std::runtime_error("publisher READY failed");
    }
}

void ArtifactPublisher::FailStartup(ipc::Error error) noexcept
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (channel_attached_ && startup_action_ == StartupAction::Wait && !stopping_) {
            startup_error_.emplace(std::move(error));
        }
        stopping_ = true;
    }
    available_.notify_all();
    JoinWorker();
}

void ArtifactPublisher::Enqueue(ipc::Frame frame)
{
    constexpr std::size_t kMaxQueuedFrames = 16;
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (!available_.wait_until(
            lock, IoDeadline(),
            [this] { return queue_.size() < kMaxQueuedFrames || failed_ || stopping_ || interrupted_; }) ||
        failed_ || stopping_ || interrupted_) {
        throw std::runtime_error("ARTIFACT: publisher unavailable or backpressure timeout");
    }
    queue_.push_back(std::move(frame));
    available_.notify_all();
}
ipc::ArtifactBegin ArtifactPublisher::PrepareArtifact(const std::string& name) const
{
    if (finishing_ || interrupted_) {
        throw std::runtime_error("ARTIFACT: collection is draining");
    }
    for (const auto& entry : manifest_) {
        if (entry.name == name) {
            throw std::runtime_error("ARTIFACT: duplicate name");
        }
    }
    return {manifest_.size() + 1, ipc::KindForName(name), name, ipc::ArtifactTypeForName(name)};
}
void ArtifactPublisher::Begin(const std::string& name)
{
    std::unique_lock<std::mutex> lock(artifact_mutex_);
    current_ = PrepareArtifact(name);
    bytes_ = 0;
    Enqueue({ipc::Type::DataBegin, ipc::EncodeArtifactBegin(current_)});
    artifact_lock_ = std::move(lock);
}
void ArtifactPublisher::Write(std::string_view bytes)
{
    if (!artifact_lock_.owns_lock()) {
        throw std::runtime_error("ARTIFACT: write without begin");
    }
    while (!bytes.empty()) {
        const size_t length = std::min(bytes.size(), ipc::kChunkBytes);
        ipc::ArtifactChunk chunk{current_.id, bytes_, ipc::Bytes(bytes.begin(), bytes.begin() + length)};
        Enqueue({ipc::TypeForKind(current_.kind), ipc::EncodeArtifactChunk(chunk)});
        bytes_ += length;
        bytes.remove_prefix(length);
    }
}
void ArtifactPublisher::Commit(uint64_t records)
{
    if (!artifact_lock_.owns_lock()) {
        throw std::runtime_error("ARTIFACT: commit without begin");
    }
    Enqueue({ipc::Type::DataEnd, ipc::EncodeArtifactEnd({current_.id, bytes_, records})});
    manifest_.push_back({current_.id, current_.name, bytes_});
    artifact_lock_.unlock();
}
void ArtifactPublisher::PublishOptional(const std::string& name, std::string_view contents, uint64_t records)
{
    std::lock_guard<std::mutex> artifactLock(artifact_mutex_);
    const auto artifact = PrepareArtifact(name);
    const auto type = ipc::TypeForKind(artifact.kind);
    std::deque<ipc::Frame> frames;
    frames.push_back({ipc::Type::DataBegin, ipc::EncodeArtifactBegin(artifact)});
    uint64_t offset = 0;
    while (!contents.empty()) {
        const size_t length = std::min(contents.size(), ipc::kChunkBytes);
        const ipc::ArtifactChunk chunk{artifact.id, offset, ipc::Bytes(contents.begin(), contents.begin() + length)};
        frames.push_back({type, ipc::EncodeArtifactChunk(chunk)});
        offset += length;
        contents.remove_prefix(length);
    }
    frames.push_back({ipc::Type::DataEnd, ipc::EncodeArtifactEnd({artifact.id, offset, records})});

    // Keep at most one buffered optional artifact in the shared queue.
    // Make all allocations before publishing any of its frames to the worker.
    std::unique_lock<std::mutex> queueLock(queue_mutex_);
    if (!available_.wait_until(
            queueLock, IoDeadline(), [this] { return queue_.empty() || failed_ || stopping_ || interrupted_; }) ||
        failed_ || stopping_ || interrupted_) {
        throw std::runtime_error("ARTIFACT: publisher unavailable or backpressure timeout");
    }
    manifest_.push_back({artifact.id, name, offset});
    queue_.swap(frames);
    available_.notify_all();
}
void ArtifactPublisher::Abort() noexcept
{
    if (!interrupted_) {
        MarkFailed();
    }
    if (artifact_lock_.owns_lock()) {
        artifact_lock_.unlock();
    }
}
bool ArtifactPublisher::SendCancelled() const
{
    if (failed_ || stopping_) {
        return true;
    }
    const auto value = drain_deadline_ms_.load();
    return value != 0 && std::chrono::steady_clock::now() >= ipc::Deadline(std::chrono::milliseconds(value));
}
ipc::Deadline ArtifactPublisher::SendDeadline()
{
    const auto value = drain_deadline_ms_.load();
    return value == 0 ? IoDeadline() : std::min(IoDeadline(), ipc::Deadline(std::chrono::milliseconds(value)));
}
void ArtifactPublisher::HandleControl(const ipc::Frame& frame)
{
    if (frame.type == ipc::Type::Error) {
        throw ipc::Exception(ipc::DecodeError(frame.payload));
    }
    if (!frame.payload.empty()) {
        throw ipc::Exception({1, "control payload must be empty", "PROTOCOL", "collect"});
    }
    if (frame.type == ipc::Type::Interrupt) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            interrupted_ = true;
        }
        available_.notify_all();
        ASCTOOL_DEBUG("[npu-compute ipc] phase=interrupt");
        return;
    }
    if (frame.type != ipc::Type::Flush || flush_requested_) {
        throw ipc::Exception({2, "unexpected or repeated control", "PROTOCOL", "collect"});
    }
    flush_requested_ = true;
    // Publication drains continuously. Only ACLPTI's shutdown callback may end
    // the consumer; a flush request must never race an active kernel.
    available_.notify_all();
    ASCTOOL_DEBUG("[npu-compute ipc] phase=flush");
}
void ArtifactPublisher::WaitForResultAck()
{
    drain_deadline_ms_ = 0;
    const auto deadline = ipc::After(ipc::kResultAckTimeout);
    while (!interrupted_) {
        auto reply = channel_.Receive(deadline, [this] { return stopping_.load(); });
        if (reply.type == ipc::Type::ResultAck && reply.payload.empty()) {
            acknowledged_ = true;
            channel_.Close();
            return;
        }
        HandleControl(reply);
    }
    throw ipc::Exception({3, "collection interrupted", "PROFILING", "collect"});
}

bool ArtifactPublisher::TakeNextFrame(ipc::Frame& frame)
{
    constexpr std::chrono::milliseconds kControlPollInterval{20};
    std::unique_lock<std::mutex> lock(queue_mutex_);
    available_.wait_for(lock, kControlPollInterval, [this] { return !queue_.empty() || failed_ || stopping_; });
    if (queue_.empty()) {
        if (interrupted_) {
            throw ipc::Exception({3, "collection interrupted", "PROFILING", "collect"});
        }
        return false;
    }
    frame = std::move(queue_.front());
    queue_.pop_front();
    available_.notify_all();
    return true;
}

void ArtifactPublisher::PollControl()
{
    if (failed_ || stopping_) {
        throw std::runtime_error("publisher stopped");
    }
    pollfd input{channel_.Fd(), POLLIN, 0};
    const int ready = poll(&input, 1, 0);
    if (ready < 0 && errno != EINTR) {
        throw std::runtime_error("publisher poll failed");
    }
    if (ready > 0) {
        HandleControl(channel_.Receive(IoDeadline(), [this] { return SendCancelled(); }));
    }
}

bool ArtifactPublisher::StartPublishing()
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    available_.wait(lock, [this] { return startup_action_ != StartupAction::Wait || stopping_; });
    if (stopping_) {
        auto error = std::move(startup_error_);
        lock.unlock();
        if (error) {
            SendFailure(*error);
        }
        return false;
    }
    const auto action = startup_action_;
    const auto deadline = ready_deadline_;
    lock.unlock();

    if (action == StartupAction::Ready) {
        channel_.Send(
            ipc::Type::ConnectAck,
            ipc::EncodeReady({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), 0}), deadline,
            [this] { return stopping_.load(); });
    }
    lock.lock();
    started_ = true;
    available_.notify_all();
    return true;
}

void ArtifactPublisher::SendFailure(const ipc::Error& error) noexcept
{
    try {
        ASCTOOL_ERROR(
            "[UDS] %s/%s code=%u: %s", error.domain.c_str(), error.phase.c_str(), error.code, error.message.c_str());
        constexpr std::chrono::milliseconds kErrorNotificationTimeout{100};
        channel_.Send(ipc::Type::Error, ipc::EncodeError(error), ipc::After(kErrorNotificationTimeout));
    } catch (...) {
    }
}

void ArtifactPublisher::MarkFailed()
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        failed_ = true;
    }
    available_.notify_all();
}

void ArtifactPublisher::Worker() noexcept
{
    try {
        if (!StartPublishing()) {
            channel_.Close();
            return;
        }
        for (;;) {
            PollControl();
            ipc::Frame frame;
            if (!TakeNextFrame(frame)) {
                continue;
            }
            if (interrupted_ && frame.type == ipc::Type::Result) {
                throw ipc::Exception({3, "collection interrupted", "PROFILING", "collect"});
            }
            channel_.Send(frame.type, frame.payload, SendDeadline(), [this] { return SendCancelled(); });
            if (frame.type == ipc::Type::Result) {
                WaitForResultAck();
                return;
            }
        }
    } catch (const std::exception& error) {
        MarkFailed();
        try {
            const auto* failure = dynamic_cast<const ipc::Exception*>(&error);
            if (failure != nullptr) {
                SendFailure(failure->Detail());
            } else {
                SendFailure({1, error.what(), "ARTIFACT", "publish"});
            }
        } catch (...) {
        }
    } catch (...) {
        MarkFailed();
    }
    channel_.Close();
}

void ArtifactPublisher::RequestFinish(ipc::Result result)
{
    try {
        std::lock_guard<std::mutex> lock(artifact_mutex_);
        if (finishing_) {
            return;
        }
        finishing_ = true;
        const auto deadline = ipc::After(ipc::kDrainTimeout);
        drain_deadline_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
        auto next = manifest_.begin();
        while (static_cast<size_t>(manifest_.end() - next) > ipc::kMaxManifestEntriesPerFrame) {
            const auto end = next + ipc::kMaxManifestEntriesPerFrame;
            result.manifest.assign(next, end);
            Enqueue({ipc::Type::ResultManifest, ipc::EncodeResult(result)});
            next = end;
        }
        result.manifest.assign(next, manifest_.end());
        Enqueue({ipc::Type::Result, ipc::EncodeResult(result)});
    } catch (...) {
        if (!interrupted_) {
            MarkFailed();
        }
        throw;
    }
}

bool ArtifactPublisher::WaitForFinish() noexcept
{
    JoinWorker();
    return acknowledged_ && !failed_;
}

bool ArtifactPublisher::Finish(ipc::Result result)
{
    try {
        RequestFinish(std::move(result));
    } catch (...) {
    }
    return WaitForFinish();
}

void ArtifactPublisher::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
    }
    available_.notify_all();
    JoinWorker();
}

void ArtifactPublisher::JoinWorker() noexcept
{
    std::lock_guard<std::mutex> joinLock(join_mutex_);
    if (worker_.joinable()) {
        worker_.join();
        ASCTOOL_DEBUG("[npu-compute ipc] phase=publisher-joined");
    }
}
} // namespace npucompute
