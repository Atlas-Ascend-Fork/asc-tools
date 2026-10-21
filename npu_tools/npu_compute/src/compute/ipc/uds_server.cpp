/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_uds_server.h"
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unistd.h>
namespace npucompute {
namespace {
constexpr uint64_t kMaxHandshakeTimeoutMs = 120000;

std::string Environment(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw ipc::Exception({1, std::string("missing ") + name, "BOOTSTRAP", "initialize"});
    }
    return value;
}
uint64_t Number(const char* name)
{
    const std::string value = Environment(name);
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || value.find_first_not_of("0123456789") != std::string::npos ||
        end != value.c_str() + value.size() || parsed == 0 || parsed > std::numeric_limits<uint64_t>::max()) {
        throw ipc::Exception({1, std::string("invalid ") + name, "BOOTSTRAP", "initialize"});
    }
    return static_cast<uint64_t>(parsed);
}
} // namespace
ipc::Hello UdsServer::Accept()
{
    const auto name = Environment("NPU_COMPUTE_UDS_NAME");
    const auto session = Number("NPU_COMPUTE_SESSION_ID");
    const auto pid = Number("NPU_COMPUTE_CLI_PID");
    const auto timeout = Number("NPU_COMPUTE_HANDSHAKE_TIMEOUT_MS");
    if (pid > static_cast<uint64_t>(std::numeric_limits<pid_t>::max()) || timeout > kMaxHandshakeTimeoutMs) {
        throw ipc::Exception({1, "invalid PID or handshake timeout", "BOOTSTRAP", "initialize"});
    }
    deadline_ = ipc::After(std::chrono::milliseconds(timeout));
    ipc::Listener listener(name);
    channel_ = listener.Accept(session, static_cast<pid_t>(pid), getuid(), deadline_);
    const auto frame = channel_.Receive(deadline_);
    if (frame.type != ipc::Type::Connect) {
        throw ipc::Exception({1, "CONNECT required", "PROTOCOL", "handshake"});
    }
    auto hello = ipc::DecodeHello(frame.payload);
    if (hello.pid != pid || hello.uid != getuid()) {
        throw ipc::Exception({1, "CONNECT identity mismatch", "AUTHENTICATION", "handshake"});
    }
    return hello;
}
void UdsServer::Ready()
{
    channel_.Send(
        ipc::Type::ConnectAck, ipc::EncodeReady({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), 0}),
        deadline_);
}
void UdsServer::Fail(const std::string& message) noexcept { Fail(ipc::Error{1, message, "INJECTION", "initialize"}); }
void UdsServer::Fail(const ipc::Error& error) noexcept
{
    try {
        channel_.Send(ipc::Type::Error, ipc::EncodeError(error), ipc::After(std::chrono::milliseconds(100)));
    } catch (...) {
    }
    channel_.Close();
}
} // namespace npucompute
