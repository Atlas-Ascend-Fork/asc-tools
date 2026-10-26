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
#include "securec.h"

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
constexpr uint32_t UDS_PROTOCOL_ERROR_CODE = 1;
constexpr uint32_t UDS_DEADLINE_ERROR_CODE = 2;
constexpr uint32_t UDS_CANCELLED_ERROR_CODE = 3;
constexpr uint32_t UDS_POLL_ERROR_CODE = 4;
constexpr uint32_t UDS_SOCKET_BUFFER_ERROR_CODE = 5;
constexpr uint32_t UDS_PROTOCOL_MAGIC = 0x5A5B;
constexpr uint16_t UDS_PROTOCOL_VERSION = 3;
constexpr uint16_t UDS_RESERVED_FLAGS = 0;
constexpr size_t UDS_TYPE_OFFSET = 4;
constexpr size_t UDS_TYPE_BYTES = 2;
constexpr size_t UDS_FLAGS_OFFSET = 6;
constexpr size_t UDS_LENGTH_OFFSET = 8;
constexpr size_t UDS_LENGTH_BYTES = 4;
constexpr size_t UDS_VERSION_OFFSET = 12;
constexpr size_t UDS_VERSION_BYTES = 2;
constexpr size_t UDS_SESSION_OFFSET = 18;
constexpr size_t UDS_SESSION_BYTES = 8;
constexpr size_t UDS_SEQUENCE_OFFSET = 26;
constexpr size_t UDS_SEQUENCE_BYTES = 8;
constexpr size_t UDS_BYTE_BITS = 8;
constexpr size_t UDS_IOV_COUNT = 1;
constexpr nfds_t UDS_NO_FILE_DESCRIPTORS = 0;
constexpr size_t UDS_TRAILING_FLAG_FIELDS = 2;
constexpr int UDS_CANCEL_POLL_INTERVAL_MS = 20;
constexpr int UDS_INITIAL_RETRY_DELAY_MS = 20;
constexpr int UDS_MAX_RETRY_DELAY_MS = 100;
constexpr int UDS_RETRY_BACKOFF_FACTOR = 2;
constexpr int UDS_SOCKET_BACKLOG = 8;
constexpr size_t UDS_RANDOM_BYTES = 16;
constexpr uint8_t UDS_HEX_LOW_NIBBLE_MASK = 0x0F;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw Exception({UDS_PROTOCOL_ERROR_CODE, message, "protocol", "frame"});
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
        throw Exception({UDS_DEADLINE_ERROR_CODE, "UDS deadline exceeded", "transport", "deadline"});
    }
    const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<int64_t>(INT_MAX, count + 1));
}
void CheckCancellation(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) {
        throw Exception({UDS_CANCELLED_ERROR_CODE, "UDS operation cancelled", "transport", "cancel"});
    }
}
void Wait(int fd, short events, Deadline deadline, const std::function<bool()>& cancelled = {})
{
    for (;;) {
        CheckCancellation(cancelled);
        pollfd descriptor{fd, events, 0};
        const int timeout =
            cancelled ? std::min(UDS_CANCEL_POLL_INTERVAL_MS, Remaining(deadline)) : Remaining(deadline);
        const int result = poll(&descriptor, UDS_IOV_COUNT, timeout);
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
        if ((static_cast<unsigned int>(descriptor.revents) & POLLNVAL) != 0) {
            throw Exception({UDS_POLL_ERROR_CODE, "UDS poll error", "transport", "poll"});
        }
        if ((static_cast<unsigned int>(descriptor.revents) & POLLERR) != 0) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
                SystemError("UDS poll status");
            }
            errno = error != 0 ? error : EIO;
            SystemError("UDS poll");
        }
        // Read buffered messages even when peer has closed the socket.
        if ((static_cast<unsigned int>(descriptor.revents) & static_cast<unsigned int>(events)) != 0) {
            return;
        }
        Check((static_cast<unsigned int>(descriptor.revents) & POLLHUP) == 0, "UDS peer disconnected");
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
    Check(
        memcpy_s(result.sun_path + 1, sizeof(result.sun_path) - 1, name.data() + 1, name.size() - 1) == EOK,
        "cannot copy abstract UDS name");
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
            throw Exception(
                {UDS_SOCKET_BUFFER_ERROR_CODE, "UDS socket buffer too small", "transport", "socket-buffer"});
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
        bytes.push_back(static_cast<uint8_t>(value >> (UDS_BYTE_BITS * i)));
    }
}
uint64_t Read(const Bytes& bytes, size_t offset, size_t size)
{
    uint64_t result = 0;
    for (size_t i = 0; i < size; ++i) {
        result |= static_cast<uint64_t>(bytes[offset + i]) << (UDS_BYTE_BITS * i);
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
Bootstrap MakeBootstrap()
{
    Bootstrap result;
    std::array<uint8_t, UDS_RANDOM_BYTES> random{};
    Random(random.data(), random.size());
    constexpr char hex[] = "0123456789abcdef";
    result.name = "@npu-compute-";
    for (const auto byte : random) {
        result.name += hex[byte >> 4];
        result.name += hex[byte & UDS_HEX_LOW_NIBBLE_MASK];
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
    int retryDelay = UDS_INITIAL_RETRY_DELAY_MS;
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
            const int delay = std::min(UDS_CANCEL_POLL_INTERVAL_MS, Remaining(deadline));
            if (poll(nullptr, UDS_NO_FILE_DESCRIPTORS, delay) < 0 && errno != EINTR) {
                SystemError("UDS connect retry");
            }
        }
        retryDelay = std::min(UDS_MAX_RETRY_DELAY_MS, retryDelay * UDS_RETRY_BACKOFF_FACTOR);
    }
}
void Channel::Send(Type type, const Bytes& payload, Deadline deadline, const std::function<bool()>& cancelled)
{
    ValidateType(type);
    Check(payload.size() <= kMaxValueBytes - kPrefixBytes, "UDS payload too large");
    Check(sendSequence_ != std::numeric_limits<uint64_t>::max(), "UDS send sequence exhausted");
    Bytes packet;
    packet.reserve(kHeaderBytes + kPrefixBytes + payload.size());
    Append(packet, UDS_PROTOCOL_MAGIC, UDS_LENGTH_BYTES);
    Append(packet, static_cast<uint16_t>(type), UDS_TYPE_BYTES);
    Append(packet, UDS_RESERVED_FLAGS, UDS_VERSION_BYTES);
    Append(packet, kPrefixBytes + payload.size(), UDS_LENGTH_BYTES);
    Append(packet, UDS_PROTOCOL_VERSION, UDS_VERSION_BYTES);
    Append(packet, UDS_RESERVED_FLAGS, UDS_VERSION_BYTES);
    Append(packet, UDS_RESERVED_FLAGS, UDS_VERSION_BYTES);
    Append(packet, session_, UDS_SESSION_BYTES);
    Append(packet, sendSequence_, UDS_SEQUENCE_BYTES);
    packet.insert(packet.end(), payload.begin(), payload.end());
    for (;;) {
        CheckCancellation(cancelled);
        Remaining(deadline);
        iovec vector{packet.data(), packet.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = UDS_IOV_COUNT;
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
        message.msg_iovlen = UDS_IOV_COUNT;
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
    Check(
        Read(packet, 0, UDS_LENGTH_BYTES) == UDS_PROTOCOL_MAGIC &&
            Read(packet, UDS_FLAGS_OFFSET, UDS_VERSION_BYTES) == UDS_RESERVED_FLAGS,
        "invalid UDS header");
    Check(Read(packet, UDS_LENGTH_OFFSET, UDS_LENGTH_BYTES) == packet.size() - kHeaderBytes, "UDS length mismatch");
    Check(
        Read(packet, UDS_VERSION_OFFSET, UDS_VERSION_BYTES) == UDS_PROTOCOL_VERSION &&
            Read(packet, UDS_VERSION_OFFSET + UDS_VERSION_BYTES, UDS_VERSION_BYTES) == UDS_RESERVED_FLAGS &&
            Read(packet, UDS_VERSION_OFFSET + UDS_TRAILING_FLAG_FIELDS * UDS_VERSION_BYTES, UDS_VERSION_BYTES) ==
                UDS_RESERVED_FLAGS,
        "unsupported UDS version or flags");
    Check(Read(packet, UDS_SESSION_OFFSET, UDS_SESSION_BYTES) == session_, "UDS session mismatch");
    Check(
        Read(packet, UDS_SEQUENCE_OFFSET, UDS_SEQUENCE_BYTES) == receiveSequence_ &&
            receiveSequence_ != std::numeric_limits<uint64_t>::max(),
        "UDS sequence mismatch");
    Frame result{static_cast<Type>(Read(packet, UDS_TYPE_OFFSET, UDS_TYPE_BYTES)), {}};
    ValidateType(result.type);
    result.payload.assign(packet.begin() + kHeaderBytes + kPrefixBytes, packet.end());
    ++receiveSequence_;
    return result;
}
Listener::Listener(const std::string& name)
{
    const auto address = Address(name);
    fd_ = Socket();
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), AddressLength(name)) < 0 ||
        listen(fd_, UDS_SOCKET_BACKLOG) < 0) {
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
        return result;
    }
}
} // namespace npucompute::ipc
