/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "ipc.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace npucompute::ipc {
namespace {
void Check(bool condition, const char* message)
{
    if (!condition) {
        throw Exception({1, message, "protocol", "frame"});
    }
}
[[noreturn]] void SystemError(const char* operation)
{
    const int error = errno;
    throw Exception(
        {static_cast<uint32_t>(error), std::string(operation) + ": " + std::strerror(error), "transport", operation});
}
int Remaining(Deadline deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        throw Exception({2, "UDS deadline exceeded", "transport", "deadline"});
    }
    const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<int64_t>(INT_MAX, count + 1));
}
void CheckCancellation(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) {
        throw Exception({3, "UDS operation cancelled", "transport", "cancel"});
    }
}
void Wait(int fd, short events, Deadline deadline, const std::function<bool()>& cancelled = {})
{
    for (;;) {
        CheckCancellation(cancelled);
        pollfd descriptor{fd, events, 0};
        const int timeout = cancelled ? std::min(20, Remaining(deadline)) : Remaining(deadline);
        const int result = poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            SystemError("UDS poll");
        }
        if (result == 0) {
            Remaining(deadline);
            continue;
        }
        CheckCancellation(cancelled);
        if (descriptor.revents & POLLNVAL) {
            throw Exception({4, "UDS poll error", "transport", "poll"});
        }
        if (descriptor.revents & POLLERR) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                SystemError("UDS poll status");
            }
            errno = error ? error : EIO;
            SystemError("UDS poll");
        }
        // Read buffered messages even when peer has closed the socket.
        if (descriptor.revents & events) {
            return;
        }
        Check(!(descriptor.revents & POLLHUP), "UDS peer disconnected");
    }
}
sockaddr_un Address(const std::string& name)
{
    sockaddr_un result{};
    Check(
        name.size() > 1 && name.front() == '@' && name.size() <= sizeof(result.sun_path) &&
            name.find('\0') == std::string::npos,
        "invalid abstract UDS name");
    result.sun_family = AF_UNIX;
    std::copy(name.begin() + 1, name.end(), std::begin(result.sun_path) + 1);
    return result;
}
socklen_t AddressLength(const std::string& name)
{
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size());
}
void ConfigureBuffers(int fd)
{
    constexpr int requested = 128 * 1024;
    for (const int option : {SO_SNDBUF, SO_RCVBUF}) {
        if (setsockopt(fd, SOL_SOCKET, option, &requested, sizeof(requested)) < 0) {
            SystemError("UDS socket buffer setup");
        }
        int actual = 0;
        socklen_t size = sizeof(actual);
        if (getsockopt(fd, SOL_SOCKET, option, &actual, &size) < 0) {
            SystemError("UDS socket buffer readback");
        }
        // Linux reports twice the requested size; reserve that half for bookkeeping.
        if (size != sizeof(actual) || actual / 2 < static_cast<int>(kMaxFrameBytes)) {
            throw Exception({5, "UDS socket buffer too small", "transport", "socket-buffer"});
        }
    }
}
int Socket()
{
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        SystemError("UDS socket");
    }
    try {
        ConfigureBuffers(fd);
    } catch (...) {
        close(fd);
        throw;
    }
    return fd;
}
ucred PeerCredentials(int fd)
{
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0) {
        SystemError("UDS SO_PEERCRED");
    }
    Check(size == sizeof(credentials), "UDS peer credentials mismatch");
    return credentials;
}
bool IsDescendant(pid_t pid, pid_t ancestor)
{
    while (pid > 0) {
        if (pid == ancestor) {
            return true;
        }
        std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
        std::string stat;
        if (!std::getline(input, stat)) {
            return false;
        }
        // comm may contain spaces and parentheses; ppid follows the final ')'.
        const auto end = stat.rfind(')');
        if (end == std::string::npos) {
            return false;
        }
        std::istringstream fields(stat.substr(end + 1));
        char state = 0;
        pid_t parent = 0;
        if (!(fields >> state >> parent) || parent == pid) {
            return false;
        }
        pid = parent;
    }
    return false;
}
void VerifyPeer(int fd, pid_t expectedPid, uid_t expectedUid, bool allowDescendants = false)
{
    const auto credentials = PeerCredentials(fd);
    Check(
        credentials.uid == expectedUid &&
            (credentials.pid == expectedPid || (allowDescendants && IsDescendant(credentials.pid, expectedPid))),
        "UDS peer credentials mismatch");
}
void Append(Bytes& bytes, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}
uint64_t Read(const Bytes& bytes, size_t offset, size_t size)
{
    uint64_t result = 0;
    for (size_t i = 0; i < size; ++i) {
        result |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return result;
}
void ValidateType(Type type)
{
    bool valid = false;
    switch (type) {
        case Type::Connect:
        case Type::ConnectAck:
        case Type::Interrupt:
        case Type::Flush:
        case Type::Result:
        case Type::ResultAck:
        case Type::Error:
        case Type::DataBegin:
        case Type::DataEnd:
        case Type::Pmu:
        case Type::Pipeline:
        case Type::Hardware:
        case Type::PmSampling:
            valid = true;
            break;
        default:
            break;
    }
    Check(valid, "unknown or unnegotiated UDS message type");
}
void ReadRandomDevice(uint8_t* bytes, size_t size)
{
    int fd = -1;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        SystemError("UDS open /dev/urandom");
    }
    size_t offset = 0;
    while (offset < size) {
        const auto count = read(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            const int error = errno;
            close(fd);
            errno = error;
            SystemError("UDS read /dev/urandom");
        }
        if (count == 0) {
            close(fd);
            Check(false, "UDS /dev/urandom returned no data");
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
}
void Random(void* data, size_t size)
{
    auto* bytes = static_cast<uint8_t*>(data);
    size_t offset = 0;
#ifdef SYS_getrandom
    while (offset < size) {
        const auto count = syscall(SYS_getrandom, bytes + offset, size - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && errno == ENOSYS) {
            break;
        }
        if (count < 0) {
            SystemError("UDS getrandom");
        }
        Check(count != 0, "UDS getrandom returned no data");
        offset += static_cast<size_t>(count);
    }
#endif
    if (offset < size) {
        ReadRandomDevice(bytes + offset, size - offset);
    }
}
} // namespace

Deadline After(std::chrono::milliseconds timeout) { return std::chrono::steady_clock::now() + timeout; }
void DebugLog(const char* phase, uint64_t) noexcept
{
    const char* enabled = std::getenv("NPU_COMPUTE_DEBUG");
    if (enabled && *enabled && std::strcmp(enabled, "0") != 0) {
        std::fprintf(stderr, "[npu-compute ipc pid=%ld] phase=%s\n", static_cast<long>(getpid()), phase);
    }
}
Bootstrap MakeBootstrap()
{
    Bootstrap result;
    std::array<uint8_t, 16> random{};
    Random(random.data(), random.size());
    constexpr char hex[] = "0123456789abcdef";
    result.name = "@npu-compute-";
    for (const auto byte : random) {
        result.name += hex[byte >> 4];
        result.name += hex[byte & 15];
    }
    do {
        Random(&result.session, sizeof(result.session));
    } while (result.session == 0);
    result.cliPid = getpid();
    result.handshakeDeadline = After(std::chrono::milliseconds(result.timeoutMs));
    return result;
}
Channel::~Channel() { Close(); }
Channel::Channel(Channel&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      session_(other.session_),
      sendSequence_(other.sendSequence_),
      receiveSequence_(other.receiveSequence_)
{}
Channel& Channel::operator=(Channel&& other) noexcept
{
    if (this != &other) {
        Close();
        fd_ = std::exchange(other.fd_, -1);
        session_ = other.session_;
        sendSequence_ = other.sendSequence_;
        receiveSequence_ = other.receiveSequence_;
    }
    return *this;
}
void Channel::Close() noexcept
{
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}
void Channel::Shutdown() noexcept
{
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_RDWR);
    }
}
pid_t Channel::PeerPid() const { return PeerCredentials(fd_).pid; }
Channel Channel::Connect(
    const std::string& name, uint64_t session, pid_t peerPid, uid_t peerUid, Deadline deadline,
    const std::function<bool()>& cancelled, bool allowDescendants)
{
    Check(session != 0, "zero UDS session");
    const auto address = Address(name);
    int retryDelay = 20;
    DebugLog("connect");
    for (;;) {
        CheckCancellation(cancelled);
        Remaining(deadline);
        Channel result(Socket(), session);
        if (connect(result.fd_, reinterpret_cast<const sockaddr*>(&address), AddressLength(name)) == 0) {
            VerifyPeer(result.fd_, peerPid, peerUid, allowDescendants);
            return result;
        }
        int connectError = errno;
        if (connectError == EINPROGRESS || connectError == EALREADY) {
            Wait(result.fd_, POLLOUT, deadline, cancelled);
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(result.fd_, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                SystemError("UDS connect status");
            }
            if (error == 0) {
                VerifyPeer(result.fd_, peerPid, peerUid, allowDescendants);
                return result;
            }
            connectError = error;
        }
        if (connectError != ECONNREFUSED && connectError != EAGAIN && connectError != EINTR) {
            errno = connectError;
            SystemError("UDS connect");
        }
        // The SO may not yet have been loaded. Retry without resetting the shared deadline.
        const auto retryAt = std::min(deadline, After(std::chrono::milliseconds(retryDelay)));
        while (std::chrono::steady_clock::now() < retryAt) {
            CheckCancellation(cancelled);
            const int delay = std::min(20, Remaining(deadline));
            if (poll(nullptr, 0, delay) < 0 && errno != EINTR) {
                SystemError("UDS connect retry");
            }
        }
        retryDelay = std::min(100, retryDelay * 2);
    }
}
void Channel::Send(Type type, const Bytes& payload, Deadline deadline, const std::function<bool()>& cancelled)
{
    ValidateType(type);
    Check(payload.size() <= kMaxValueBytes - kPrefixBytes, "UDS payload too large");
    Check(sendSequence_ != std::numeric_limits<uint64_t>::max(), "UDS send sequence exhausted");
    Bytes packet;
    packet.reserve(kHeaderBytes + kPrefixBytes + payload.size());
    Append(packet, 0x5A5B, 4);
    Append(packet, static_cast<uint16_t>(type), 2);
    Append(packet, 0, 2);
    Append(packet, kPrefixBytes + payload.size(), 4);
    Append(packet, 3, 2);
    Append(packet, 0, 2);
    Append(packet, 0, 2);
    Append(packet, session_, 8);
    Append(packet, sendSequence_, 8);
    packet.insert(packet.end(), payload.begin(), payload.end());
    for (;;) {
        CheckCancellation(cancelled);
        Remaining(deadline);
        iovec vector{packet.data(), packet.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const auto sent = sendmsg(fd_, &message, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            Wait(fd_, POLLOUT, deadline, cancelled);
            continue;
        }
        if (sent < 0) {
            SystemError("UDS send");
        }
        Check(static_cast<size_t>(sent) == packet.size(), "short UDS packet send");
        ++sendSequence_;
        return;
    }
}
Frame Channel::Receive(Deadline deadline, const std::function<bool()>& cancelled)
{
    Bytes packet(kHeaderBytes + kMaxValueBytes);
    for (;;) {
        CheckCancellation(cancelled);
        Remaining(deadline);
        iovec vector{packet.data(), packet.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const auto received = recvmsg(fd_, &message, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            Wait(fd_, POLLIN, deadline, cancelled);
            continue;
        }
        if (received < 0) {
            SystemError("UDS receive");
        }
        Check(received > 0, "UDS peer disconnected");
        Check(!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)), "truncated UDS packet");
        packet.resize(static_cast<size_t>(received));
        break;
    }
    Check(packet.size() >= kHeaderBytes + kPrefixBytes, "short UDS packet");
    Check(Read(packet, 0, 4) == 0x5A5B && Read(packet, 6, 2) == 0, "invalid UDS header");
    Check(Read(packet, 8, 4) == packet.size() - kHeaderBytes, "UDS length mismatch");
    Check(
        Read(packet, 12, 2) == 3 && Read(packet, 14, 2) == 0 && Read(packet, 16, 2) == 0,
        "unsupported UDS version or flags");
    Check(Read(packet, 18, 8) == session_, "UDS session mismatch");
    Check(
        Read(packet, 26, 8) == receiveSequence_ && receiveSequence_ != std::numeric_limits<uint64_t>::max(),
        "UDS sequence mismatch");
    Frame result{static_cast<Type>(Read(packet, 4, 2)), {}};
    ValidateType(result.type);
    result.payload.assign(packet.begin() + kHeaderBytes + kPrefixBytes, packet.end());
    ++receiveSequence_;
    return result;
}
Listener::Listener(const std::string& name)
{
    const auto address = Address(name);
    fd_ = Socket();
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), AddressLength(name)) < 0 || listen(fd_, 8) < 0) {
        const int error = errno;
        close(fd_);
        fd_ = -1;
        errno = error;
        SystemError("UDS bind/listen");
    }
}
Listener::~Listener()
{
    if (fd_ >= 0) {
        close(fd_);
    }
}
Channel Listener::Accept(uint64_t session, pid_t peerPid, uid_t peerUid, Deadline deadline)
{
    Check(session != 0, "zero UDS session");
    for (;;) {
        Remaining(deadline);
        const int fd = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0 && errno == EINTR) {
            continue;
        }
        if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            Wait(fd_, POLLIN, deadline);
            continue;
        }
        if (fd < 0) {
            SystemError("UDS accept");
        }
        Channel result(fd, session);
        ConfigureBuffers(fd);
        try {
            VerifyPeer(fd, peerPid, peerUid);
        } catch (const std::runtime_error&) {
            // An unrelated process must not consume the target's only accept opportunity.
            continue;
        }
        close(fd_);
        fd_ = -1;
        DebugLog("accepted");
        return result;
    }
}
} // namespace npucompute::ipc
