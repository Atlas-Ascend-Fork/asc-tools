/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "ipc/ipc.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace npucompute::ipc;
void Check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition) {
        ADD_FAILURE_AT(file, line) << expression;
        throw std::runtime_error("IPC test check failed");
    }
}
#define Check(expression) Check((expression), #expression, __FILE__, __LINE__)

void Reject(const std::function<void()>& operation)
{
    bool failed = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    Check(failed);
}
TEST(NpuComputeIpcProtocol, Codecs)
{
    Config config;
    config.sections = {
        "Pipeline",
        "PipeUtilization",
        "Memory",
        "MemoryL0",
        "MemoryUB",
        "L2Cache",
        "ArithmeticUtilization",
        "ResourceConflictRatio",
    };
    auto bytes = EncodeConfig(config);
    const auto decoded = DecodeConfig(bytes);
    Check(decoded.sections == config.sections);
    Check(decoded.cacheControlMode == npucompute::ipc::kCacheControlModeNone);
    Check(static_cast<uint16_t>(Type::Hardware) == 0x0103);
    Check(static_cast<uint16_t>(Type::DataEnd) == 0x0104);
    Check(TypeForKind(KindForName("PipeUtilization.csv")) == Type::Pmu);
    Check(TypeForKind(KindForName("ArithmeticUtilization.csv")) == Type::Pmu);
    Check(TypeForKind(KindForName("ResourceConflictRatio.csv")) == Type::Pmu);
    Check(TypeForKind(KindForName("summary.jsonl")) == Type::Pmu);
    Check(TypeForKind(KindForName("PipeTrace.json")) == Type::Pipeline);
    Check(TypeForKind(KindForName("HardwareInfo.jsonl")) == Type::Hardware);
    for (size_t i = 0; i < bytes.size(); ++i) {
        Reject([&] { DecodeConfig(Bytes(bytes.begin(), bytes.begin() + i)); });
    }
    bytes.push_back(0);
    Reject([&] { DecodeConfig(bytes); });
    config.sections.push_back("MemoryL0");
    Reject([&] { EncodeConfig(config); });
    config.sections.pop_back();
    for (uint32_t mode : {npucompute::ipc::kCacheControlModeAll, npucompute::ipc::kCacheControlModeNone}) {
        config.cacheControlMode = mode;
        Check(DecodeConfig(EncodeConfig(config)).cacheControlMode == mode);
        const auto hello = DecodeHello(EncodeHello({1, 2, config}));
        Check(hello.config.cacheControlMode == mode);
    }
    for (uint32_t mode : {uint32_t{2}, uint32_t{3}}) {
        config.cacheControlMode = mode;
        Reject([&] { EncodeConfig(config); });
        auto invalid = EncodeConfig(Config{config.sections, "kernel", 0, npucompute::ipc::kCacheControlModeAll});
        invalid[invalid.size() - 4] = static_cast<uint8_t>(mode);
        Reject([&] { DecodeConfig(invalid); });
    }
    Reject([&] { EncodeArtifactBegin({1, DataKind::PmuCsv, "../Memory.csv"}); });
    Check(
        DecodeArtifactBegin(EncodeArtifactBegin({1, DataKind::PmuCsv, "PipeUtilization.csv"})).kind ==
        DataKind::PmuCsv);
    Reject([&] { EncodeArtifactBegin({1, DataKind::HardwareInfo, "HardwareInfo.jsonl"}); });
    const auto hardware = DecodeArtifactBegin(
        EncodeArtifactBegin({1, DataKind::HardwareInfo, "HardwareInfo.jsonl", ArtifactType::Jsonl}));
    Check(hardware.artifactType == ArtifactType::Jsonl);
    const auto summary =
        DecodeArtifactBegin(EncodeArtifactBegin({2, DataKind::PmuCsv, "summary.jsonl", ArtifactType::Jsonl}));
    Check(summary.artifactType == ArtifactType::Jsonl);
    const auto pipeline =
        DecodeArtifactBegin(EncodeArtifactBegin({3, DataKind::PipelineTrace, "PipeTrace.json", ArtifactType::Json}));
    Check(pipeline.artifactType == ArtifactType::Json);
    constexpr uint64_t formerArtifactLimit = 256ULL * 1024 * 1024;
    const auto largeChunk = DecodeArtifactChunk(EncodeArtifactChunk({1, formerArtifactLimit, {1}}));
    Check(largeChunk.offset == formerArtifactLimit && largeChunk.data == Bytes{1});
    const auto largeEnd = DecodeArtifactEnd(EncodeArtifactEnd({1, formerArtifactLimit + 1, 1}));
    Check(largeEnd.bytes == formerArtifactLimit + 1);
    Reject([&] { EncodeArtifactChunk({1, std::numeric_limits<uint64_t>::max(), {1}}); });
    Result largeResult;
    largeResult.sections = {"MemoryL0", "MemoryUB"};
    largeResult.manifest = {
        {1, "MemoryL0.csv", formerArtifactLimit + 1},
        {2, "MemoryUB.csv", formerArtifactLimit + 1},
    };
    const auto decodedLargeResult = DecodeResult(EncodeResult(largeResult));
    Check(
        decodedLargeResult.manifest.size() == 2 && decodedLargeResult.manifest[0].bytes == formerArtifactLimit + 1 &&
        decodedLargeResult.manifest[1].bytes == formerArtifactLimit + 1);
    Result result;
    result.sections = config.sections;
    result.manifest.push_back({1, "MemoryL0.csv", 123});
    Check(DecodeResult(EncodeResult(result)).manifest[0].bytes == 123);
    result.manifest.push_back({1, "MemoryL0.csv", 123});
    Reject([&] { EncodeResult(result); });
    const Error error{3, "failed", "transport", "handshake"};
    Check(DecodeError(EncodeError(error)).phase == error.phase);
}
TEST(NpuComputeIpcProtocol, ProcessHandshake)
{
    const auto bootstrap = MakeBootstrap();
    const auto child = fork();
    Check(child >= 0);
    if (child == 0) {
        try {
            Listener listener(bootstrap.name);
            auto server = listener.Accept(bootstrap.session, getppid(), getuid(), bootstrap.handshakeDeadline);
            const auto frame = server.Receive(bootstrap.handshakeDeadline);
            const auto hello = DecodeHello(frame.payload);
            Check(hello.pid == static_cast<uint32_t>(getppid()));
            server.Send(
                Type::ConnectAck, EncodeReady({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), 0}),
                bootstrap.handshakeDeadline);
            Check(server.Receive(bootstrap.handshakeDeadline).type == Type::ResultAck);
            _exit(0);
        } catch (...) {
            _exit(10);
        }
    }
    auto client = Channel::Connect(bootstrap.name, bootstrap.session, child, getuid(), bootstrap.handshakeDeadline);
    Config config;
    config.sections = {"MemoryL0"};
    client.Send(
        Type::Connect, EncodeHello({static_cast<uint32_t>(getpid()), static_cast<uint32_t>(getuid()), config}),
        bootstrap.handshakeDeadline);
    Check(DecodeReady(client.Receive(bootstrap.handshakeDeadline).payload).pid == static_cast<uint32_t>(child));
    client.Send(Type::ResultAck, {}, bootstrap.handshakeDeadline);
    int status = 0;
    Check(waitpid(child, &status, 0) == child);
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
TEST(NpuComputeIpcProtocol, MalformedPackets)
{
    const auto bootstrap = MakeBootstrap();
    Listener listener(bootstrap.name);
    auto sender = Channel::Connect(bootstrap.name, bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    auto receiver = listener.Accept(bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    Reject([&] { receiver.Receive(After(std::chrono::milliseconds(5))); });
    Check(listener.Fd() == -1);
    Reject([&] { sender.Send(Type::Status, {}, bootstrap.handshakeDeadline); });
    sender.Send(Type::Flush, {}, bootstrap.handshakeDeadline);
    Bytes original(34);
    Check(recv(receiver.Fd(), original.data(), original.size(), 0) == 34);
    Check(original[0] == 0x5b && original[1] == 0x5a && original[2] == 0 && original[3] == 0);
    Check(original[4] == 4 && original[5] == 0 && original[8] == 22 && original[12] == 3);
    for (size_t i = 0; i < 8; ++i) {
        Check(original[18 + i] == static_cast<uint8_t>(bootstrap.session >> (8 * i)));
        Check(original[26 + i] == 0);
    }
    // Magic, type, reserved bits, length, major, minor, flags, session, sequence.
    for (const size_t offset : {0, 4, 6, 8, 12, 14, 16, 18, 26}) {
        auto malformed = original;
        malformed[offset] ^= 0x80;
        Check(send(sender.Fd(), malformed.data(), malformed.size(), MSG_NOSIGNAL) == 34);
        Reject([&] { receiver.Receive(bootstrap.handshakeDeadline); });
    }
    for (uint8_t reservedType : {8, 9}) {
        auto malformed = original;
        malformed[4] = reservedType;
        Check(send(sender.Fd(), malformed.data(), malformed.size(), MSG_NOSIGNAL) == 34);
        Reject([&] { receiver.Receive(bootstrap.handshakeDeadline); });
    }
    auto oldMajor = original;
    oldMajor[12] = 2;
    Check(send(sender.Fd(), oldMajor.data(), oldMajor.size(), MSG_NOSIGNAL) == 34);
    Reject([&] { receiver.Receive(bootstrap.handshakeDeadline); });
    // Unexpected ancillary descriptors must be rejected via MSG_CTRUNC.
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    iovec vector{original.data(), original.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* ancillary = CMSG_FIRSTHDR(&message);
    ancillary->cmsg_level = SOL_SOCKET;
    ancillary->cmsg_type = SCM_RIGHTS;
    ancillary->cmsg_len = CMSG_LEN(sizeof(int));
    const int sentFd = sender.Fd();
    std::memcpy(CMSG_DATA(ancillary), &sentFd, sizeof(sentFd));
    Check(sendmsg(sender.Fd(), &message, MSG_NOSIGNAL) == 34);
    Reject([&] { receiver.Receive(bootstrap.handshakeDeadline); });
    Check(send(sender.Fd(), original.data(), original.size(), MSG_NOSIGNAL) == 34);
    Check(receiver.Receive(bootstrap.handshakeDeadline).type == Type::Flush);
    Check(send(sender.Fd(), original.data(), original.size(), MSG_NOSIGNAL) == 34);
    Reject([&] { receiver.Receive(bootstrap.handshakeDeadline); });
    original.resize(kMaxValueBytes + 13);
    Check(send(sender.Fd(), original.data(), original.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(original.size()));
    Reject([&] { receiver.Receive(bootstrap.handshakeDeadline); });
    sender.Shutdown();
    Reject([&] { receiver.Receive(bootstrap.handshakeDeadline); });
}
TEST(NpuComputeIpcProtocol, Cancellation)
{
    const auto bootstrap = MakeBootstrap();
    auto cancelAt = After(std::chrono::milliseconds(30));
    const auto cancelled = [&] { return std::chrono::steady_clock::now() >= cancelAt; };
    Reject([&] {
        Channel::Connect(bootstrap.name, bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline, cancelled);
    });
    Check(std::chrono::steady_clock::now() < cancelAt + std::chrono::seconds(1));
    Listener listener(bootstrap.name);
    auto sender = Channel::Connect(bootstrap.name, bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    auto receiver = listener.Accept(bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    cancelAt = After(std::chrono::milliseconds(30));
    Reject([&] { receiver.Receive(bootstrap.handshakeDeadline, cancelled); });
    Check(std::chrono::steady_clock::now() < cancelAt + std::chrono::seconds(1));
    // Cancellation consumes neither a packet nor its sequence number.
    sender.Send(Type::Flush, {}, bootstrap.handshakeDeadline);
    Check(receiver.Receive(bootstrap.handshakeDeadline).type == Type::Flush);
    cancelAt = After(std::chrono::milliseconds(30));
    Reject([&] {
        for (;;) {
            sender.Send(Type::Pmu, Bytes(kChunkBytes), bootstrap.handshakeDeadline, cancelled);
        }
    });
    Check(std::chrono::steady_clock::now() < cancelAt + std::chrono::seconds(1));
}
TEST(NpuComputeIpcProtocol, CredentialsAndTimeouts)
{
    auto bootstrap = MakeBootstrap();
    Reject([&] {
        Channel::Connect(bootstrap.name, bootstrap.session, getpid(), getuid(), After(std::chrono::milliseconds(10)));
    });
    Listener listener(bootstrap.name);
    Reject([&] {
        Channel::Connect(bootstrap.name, bootstrap.session, getpid() + 1, getuid(), bootstrap.handshakeDeadline);
    });
    auto closed = listener.Accept(bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    Reject([&] { closed.Receive(bootstrap.handshakeDeadline); });
    // Allowing descendants must still reject a peer outside the target process tree.
    bootstrap = MakeBootstrap();
    Listener descendantListener(bootstrap.name);
    Reject([&] {
        Channel::Connect(
            bootstrap.name, bootstrap.session, getpid() + 1, getuid(), bootstrap.handshakeDeadline, {}, true);
    });
    closed = descendantListener.Accept(bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    Reject([&] { closed.Receive(bootstrap.handshakeDeadline); });
    bootstrap = MakeBootstrap();
    Listener secondListener(bootstrap.name);
    auto sender = Channel::Connect(bootstrap.name, bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    Reject([&] {
        secondListener.Accept(bootstrap.session, getpid() + 1, getuid(), After(std::chrono::milliseconds(10)));
    });
    sender = Channel::Connect(bootstrap.name, bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    auto receiver = secondListener.Accept(bootstrap.session, getpid(), getuid(), bootstrap.handshakeDeadline);
    Check(sender.PeerPid() == getpid());
    for (int fd : {sender.Fd(), receiver.Fd()}) {
        Check(fcntl(fd, F_GETFL) & O_NONBLOCK);
        Check(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        for (int option : {SO_SNDBUF, SO_RCVBUF}) {
            int capacity = 0;
            socklen_t size = sizeof(capacity);
            Check(getsockopt(fd, SOL_SOCKET, option, &capacity, &size) == 0);
            Check(capacity / 2 >= static_cast<int>(kMaxFrameBytes));
        }
    }
    const Bytes maximum(kMaxValueBytes - kPrefixBytes, 0xa5);
    sender.Send(Type::Pmu, maximum, bootstrap.handshakeDeadline);
    Check(receiver.Receive(bootstrap.handshakeDeadline).payload == maximum);
    Reject([&] { sender.Send(Type::Pmu, Bytes(maximum.size() + 1), bootstrap.handshakeDeadline); });
    const Bytes payload(kChunkBytes, 0);
    const auto deadline = After(std::chrono::milliseconds(10));
    // A peer that stops consuming cannot block a publisher indefinitely.
    Reject([&] {
        for (size_t i = 0; i < 10000; ++i) {
            sender.Send(Type::Pmu, payload, deadline);
        }
    });
}
TEST(NpuComputeIpcProtocol, InvalidPeerDoesNotConsumeListener)
{
    const auto bootstrap = MakeBootstrap();
    Listener listener(bootstrap.name);
    const pid_t parent = getpid();
    const auto child = fork();
    Check(child >= 0);
    if (child == 0) {
        try {
            auto intruder =
                Channel::Connect(bootstrap.name, bootstrap.session, parent, getuid(), bootstrap.handshakeDeadline);
            _exit(0);
        } catch (...) {
            _exit(20);
        }
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    auto sender = Channel::Connect(bootstrap.name, bootstrap.session, parent, getuid(), bootstrap.handshakeDeadline);
    auto receiver = listener.Accept(bootstrap.session, parent, getuid(), bootstrap.handshakeDeadline);
    sender.Send(Type::Flush, {}, bootstrap.handshakeDeadline);
    Check(receiver.Receive(bootstrap.handshakeDeadline).type == Type::Flush);
    Check(listener.Fd() == -1);
}
} // namespace
