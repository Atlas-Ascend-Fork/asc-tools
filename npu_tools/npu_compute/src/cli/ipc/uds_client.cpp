/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_uds_client.h"
#include "artifact_receiver.h"
#include <cerrno>
#include <stdexcept>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
namespace npucompute::cli {
void UdsClient::Run(
    const ipc::Bootstrap& bootstrap, pid_t childPid, const ipc::Config& config, const std::string& directory,
    const std::atomic<bool>& childExited, const std::atomic<bool>& cancelled)
{
    const auto stopHandshake = [&] { return childExited.load() || cancelled.load(); };
    auto channel = ipc::Channel::Connect(
        bootstrap.name, bootstrap.session, childPid, getuid(), bootstrap.handshakeDeadline, stopHandshake);
    try {
        channel.Send(
            ipc::Type::Connect,
            ipc::EncodeHello({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), config}),
            bootstrap.handshakeDeadline);
        auto frame = channel.Receive(bootstrap.handshakeDeadline, stopHandshake);
        if (frame.type == ipc::Type::Error) {
            throw ipc::Exception(ipc::DecodeError(frame.payload));
        }
        if (frame.type != ipc::Type::ConnectAck) {
            throw std::runtime_error("PROTOCOL: READY required");
        }
        auto ready = ipc::DecodeReady(frame.payload);
        if (ready.pid != static_cast<uint32_t>(childPid) || ready.uid != getuid() || ready.status != 0) {
            throw std::runtime_error("AUTHENTICATION: READY identity or status mismatch");
        }
        ArtifactReceiver receiver(directory);
        for (;;) {
            if (cancelled) {
                try {
                    channel.Send(ipc::Type::Interrupt, {}, ipc::After(std::chrono::milliseconds(100)));
                } catch (...) {
                    interrupt_attempted_ = true;
                    throw;
                }
                interrupt_attempted_ = true;
                throw std::runtime_error("collection cancelled");
            }
            pollfd input{channel.Fd(), POLLIN, 0};
            const int count = poll(&input, 1, 20);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                throw std::runtime_error("UDS: poll failed");
            }
            if (count == 0) {
                if (childExited) {
                    throw std::runtime_error("UDS: target exited before RESULT");
                }
                continue;
            }
            frame = channel.Receive(ipc::After(ipc::kIoTimeout));
            if (frame.type == ipc::Type::DataBegin || frame.type == ipc::Type::DataEnd ||
                frame.type == ipc::Type::Pmu || frame.type == ipc::Type::Pipeline ||
                frame.type == ipc::Type::Hardware) {
                receiver.Receive(frame);
                artifact_count_ = receiver.Count();
            } else if (frame.type == ipc::Type::Result) {
                const bool complete = receiver.Validate(ipc::DecodeResult(frame.payload), config, &issues_);
                channel.Send(ipc::Type::ResultAck, {}, ipc::After(ipc::kIoTimeout));
                const auto closeDeadline = ipc::After(ipc::kIoTimeout);
                for (;;) {
                    pollfd closed{channel.Fd(), POLLIN, 0};
                    const int events = poll(&closed, 1, 20);
                    if (events < 0 && errno == EINTR) {
                        continue;
                    }
                    if (events < 0 || cancelled || std::chrono::steady_clock::now() >= closeDeadline) {
                        throw std::runtime_error("UDS: result close not confirmed");
                    }
                    if (events > 0) {
                        char byte;
                        const ssize_t count = recv(channel.Fd(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                        if (count == 0) {
                            break;
                        }
                        if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
                            continue;
                        }
                        throw std::runtime_error("PROTOCOL: unexpected data after RESULT_ACK");
                    }
                }
                complete_ = complete;
                return;
            } else if (frame.type == ipc::Type::Error) {
                const auto error = ipc::DecodeError(frame.payload);
                throw ipc::Exception(error);
            } else {
                throw std::runtime_error("PROTOCOL: unexpected message after READY");
            }
        }
    } catch (const std::exception& error) {
        try {
            channel.Send(
                ipc::Type::Error,
                ipc::EncodeError(
                    dynamic_cast<const ipc::Exception*>(&error) ?
                        dynamic_cast<const ipc::Exception*>(&error)->Detail() :
                        ipc::Error{1, error.what(), "ARTIFACT", "receive"}),
                ipc::After(std::chrono::milliseconds(100)));
        } catch (...) {
        }
        throw;
    }
}
} // namespace npucompute::cli
