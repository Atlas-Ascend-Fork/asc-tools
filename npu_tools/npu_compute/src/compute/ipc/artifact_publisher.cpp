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
#include <stdexcept>
#include <poll.h>
#include <unistd.h>
namespace npucompute {
namespace {
ipc::Deadline IoDeadline() { return ipc::After(ipc::kIoTimeout); }
} // namespace
ArtifactPublisher::ArtifactPublisher(ipc::Channel channel) : channel_(std::move(channel))
{
    worker_ = std::thread([this] { Worker(); });
}
ArtifactPublisher::ArtifactPublisher(ipc::Channel channel, ipc::Deadline readyDeadline)
    : channel_(std::move(channel)), ready_deadline_(readyDeadline), send_ready_(true)
{
    worker_ = std::thread([this] { Worker(); });
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (!available_.wait_until(lock, readyDeadline, [this] { return started_ || failed_; }) || failed_) {
        lock.unlock();
        Stop();
        throw std::runtime_error("publisher READY failed");
    }
}
ArtifactPublisher::~ArtifactPublisher() { Stop(); }
void ArtifactPublisher::Enqueue(ipc::Frame frame)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (!available_.wait_until(
            lock, IoDeadline(), [this] { return queue_.size() < 16 || failed_ || stopping_ || interrupted_; }) ||
        failed_ || stopping_ || interrupted_) {
        throw std::runtime_error("ARTIFACT: publisher unavailable or backpressure timeout");
    }
    queue_.push_back(std::move(frame));
    available_.notify_all();
}
void ArtifactPublisher::Begin(const std::string& name)
{
    std::unique_lock<std::mutex> lock(artifact_mutex_);
    if (finishing_ || interrupted_) {
        throw std::runtime_error("ARTIFACT: collection is draining");
    }
    for (const auto& entry : manifest_) {
        if (entry.name == name) {
            throw std::runtime_error("ARTIFACT: duplicate name");
        }
    }
    const auto kind = ipc::KindForName(name);
    current_ = {manifest_.size() + 1, kind, name, ipc::ArtifactTypeForName(name)};
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
void ArtifactPublisher::Abort() noexcept
{
    if (!interrupted_) {
        failed_ = true;
    }
    available_.notify_all();
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
        interrupted_ = true;
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
void ArtifactPublisher::Worker() noexcept
{
    try {
        if (send_ready_) {
            channel_.Send(
                ipc::Type::ConnectAck,
                ipc::EncodeReady({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), 0}),
                ready_deadline_);
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            started_ = true;
        }
        available_.notify_all();
        for (;;) {
            if (failed_ || stopping_) {
                throw std::runtime_error("publisher stopped");
            }
            pollfd input{channel_.Fd(), POLLIN, 0};
            const int ready = poll(&input, 1, 0);
            if (ready < 0 && errno != EINTR) {
                throw std::runtime_error("publisher poll failed");
            }
            if (ready > 0) {
                HandleControl(channel_.Receive(IoDeadline()));
            }
            ipc::Frame frame;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                available_.wait_for(
                    lock, std::chrono::milliseconds(20), [this] { return !queue_.empty() || failed_ || stopping_; });
                if (queue_.empty()) {
                    if (interrupted_) {
                        throw ipc::Exception({3, "collection interrupted", "PROFILING", "collect"});
                    }
                    continue;
                }
                frame = std::move(queue_.front());
                queue_.pop_front();
                available_.notify_all();
            }
            if (interrupted_ && frame.type == ipc::Type::Result) {
                throw ipc::Exception({3, "collection interrupted", "PROFILING", "collect"});
            }
            channel_.Send(frame.type, frame.payload, SendDeadline(), [this] { return SendCancelled(); });
            if (frame.type == ipc::Type::Result) {
                if (interrupted_) {
                    throw std::runtime_error("interrupted result");
                }
                drain_deadline_ms_ = 0;
                const auto deadline = IoDeadline();
                for (;;) {
                    auto reply = channel_.Receive(deadline, [this] { return stopping_.load(); });
                    if (reply.type == ipc::Type::ResultAck && reply.payload.empty()) {
                        acknowledged_ = true;
                        channel_.Close();
                        return;
                    }
                    HandleControl(reply);
                }
            }
        }
    } catch (const std::exception& error) {
        failed_ = true;
        try {
            const auto* failure = dynamic_cast<const ipc::Exception*>(&error);
            const ipc::Error detail = failure ? failure->Detail() : ipc::Error{1, error.what(), "ARTIFACT", "publish"};
            ASCTOOL_ERROR(
                "[UDS] %s/%s code=%u: %s", detail.domain.c_str(), detail.phase.c_str(), detail.code,
                detail.message.c_str());
            channel_.Send(ipc::Type::Error, ipc::EncodeError(detail), ipc::After(std::chrono::milliseconds(100)));
        } catch (...) {
        }
    } catch (...) {
        failed_ = true;
    }
    channel_.Close();
    available_.notify_all();
}
bool ArtifactPublisher::Finish(ipc::Result result)
{
    try {
        std::lock_guard<std::mutex> lock(artifact_mutex_);
        if (finishing_) {
            throw std::runtime_error("duplicate publisher finish");
        }
        finishing_ = true;
        const auto deadline = ipc::After(ipc::kDrainTimeout);
        drain_deadline_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
        result.manifest = manifest_;
        Enqueue({ipc::Type::Result, ipc::EncodeResult(result)});
    } catch (...) {
        if (!interrupted_) {
            failed_ = true;
        }
        available_.notify_all();
    }
    std::lock_guard<std::mutex> joinLock(join_mutex_);
    if (worker_.joinable()) {
        worker_.join();
    }
    return acknowledged_ && !failed_;
}
void ArtifactPublisher::Stop() noexcept
{
    stopping_ = true;
    available_.notify_all();
    // The publisher owns the fd; its bounded IO deadline also bounds teardown.
    std::lock_guard<std::mutex> joinLock(join_mutex_);
    if (worker_.joinable()) {
        worker_.join();
    }
}
} // namespace npucompute
