/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "uds_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace npucheck::ipc {
namespace {

std::string ErrnoMessage(const char* operation) { return std::string(operation) + ": " + std::strerror(errno); }

// 单个数据报必须能整体放进收发缓冲。SOCK_SEQPACKET 不做分片：超出缓冲上限时
// sendmsg 直接返回 EMSGSIZE，重试永远不会成功。
constexpr int REQUIRED_SOCKET_BUFFER = static_cast<int>(MAX_FRAME_SIZE);

// 校验并尽量抬高收发缓冲。
// setsockopt 的返回值在这里不可靠：内核会把请求值静默截断到 net.core.wmem_max /
// rmem_max，同时仍然返回成功。因此真正的判据是随后的回读。另外内核存的是请求值的
// 两倍（含记账开销），所以只校验下界，不做相等比较。
bool EnsureSocketBuffer(int fd, int optionName, const char* optionText, const char* sysctlName, std::string& error)
{
    int requested = REQUIRED_SOCKET_BUFFER;
    (void)setsockopt(fd, SOL_SOCKET, optionName, &requested, sizeof(requested));

    int actual = 0;
    socklen_t length = sizeof(actual);
    if (getsockopt(fd, SOL_SOCKET, optionName, &actual, &length) != 0) {
        error = ErrnoMessage("getsockopt");
        return false;
    }
    if (actual < REQUIRED_SOCKET_BUFFER) {
        error = std::string(optionText) + " is " + std::to_string(actual) + " bytes but a single frame needs " +
                std::to_string(REQUIRED_SOCKET_BUFFER) + " bytes; raise " + sysctlName;
        return false;
    }
    return true;
}

} // namespace

int64_t MonotonicNowMs()
{
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

int CreateSeqpacketSocket(std::string& error)
{
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = ErrnoMessage("socket");
        return -1;
    }
    if (!EnsureSocketBuffer(fd, SO_SNDBUF, "SO_SNDBUF", "net.core.wmem_max", error) ||
        !EnsureSocketBuffer(fd, SO_RCVBUF, "SO_RCVBUF", "net.core.rmem_max", error)) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

bool BuildAbstractAddress(const std::string& name, sockaddr_un& address, socklen_t& addrLen, std::string& error)
{
    // 约定：环境变量取值以 '@' 开头表示抽象命名空间。'@' 只是便于人读，也与 ss -x、
    // /proc/net/unix 的显示保持一致；落到 sun_path 时必须换成 '\0'，不能字面写入。
    if (name.empty() || name.front() != '@') {
        error = "UDS address must start with '@' to denote the abstract namespace";
        return false;
    }
    const std::string_view body(name.data() + 1, name.size() - 1);
    if (body.empty()) {
        error = "UDS abstract address must have a non-empty name after '@'";
        return false;
    }
    // 前导的 '\0' 也占一个字节。
    if (body.size() + 1 > sizeof(address.sun_path)) {
        error = "UDS abstract address exceeds the sockaddr_un limit";
        return false;
    }

    address = {};
    address.sun_family = AF_UNIX;
    address.sun_path[0] = '\0';
    std::copy(body.begin(), body.end(), address.sun_path + 1);

    // 抽象地址不以 NUL 结尾，其长度完全由 addrLen 决定。若图省事传 sizeof(sockaddr_un)，
    // 实际绑定的名字会带上上百个尾随 '\0'；bind 与 connect 两侧算法但凡不一致就永远连
    // 不上，而 errno 只有 ECONNREFUSED，完全无从定位。两侧必须共用这一条公式。
    addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + body.size());
    return true;
}

IoStatus WaitFor(int fd, short events, DeadlineMs deadline, std::string& error)
{
    while (true) {
        int sliceMs = -1; // NO_DEADLINE 时无限等待
        if (deadline != NO_DEADLINE) {
            const int64_t remain = deadline - MonotonicNowMs();
            if (remain <= 0) {
                error = "deadline exceeded";
                return IoStatus::TIMEOUT;
            }
            sliceMs = static_cast<int>(std::min<int64_t>(remain, POLL_SLICE_MS));
        }

        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = events;
        const int rc = poll(&descriptor, 1, sliceMs);
        if (rc < 0) {
            // 被信号打断后回到循环顶部，按绝对 deadline 重算剩余时间。若在这里直接用原来
            // 的相对超时再等一遍，signal 每来一次就把上限往后推一次。CLI 装了信号转发器，
            // 这条路径并不罕见。
            if (errno == EINTR) {
                continue;
            }
            error = ErrnoMessage("poll");
            return IoStatus::SYSTEM_ERROR;
        }
        if (rc == 0) {
            continue; // 时间片到期，回顶部重判绝对 deadline
        }
        if ((static_cast<unsigned int>(descriptor.revents) & (POLLERR | POLLNVAL)) != 0) {
            error = "socket error reported by poll";
            return IoStatus::SYSTEM_ERROR;
        }
        // 就绪位必须先于 POLLHUP 判断：对端"发完最后一帧再 close"会让两个位同时置起，
        // 先判 POLLHUP 就会把那一帧连同它携带的诊断信息一起丢掉。
        if ((static_cast<unsigned int>(descriptor.revents) & static_cast<unsigned int>(events)) != 0) {
            return IoStatus::OK;
        }
        if ((static_cast<unsigned int>(descriptor.revents) & POLLHUP) != 0) {
            return IoStatus::CLOSED;
        }
    }
}

IoStatus SendFrame(int fd, const Frame& frame, DeadlineMs deadline, std::string& error)
{
    auto bytes = EncodeFrame(frame);
    if (bytes.empty()) {
        error = "cannot encode frame";
        return IoStatus::PROTOCOL_ERROR;
    }
    while (true) {
        // MSG_NOSIGNAL 不可省略：向已关闭的对端写入会触发 SIGPIPE，其默认动作是终止进程。
        // 少了这个标志，本进程会在对端崩溃的瞬间被杀掉，来不及打印任何诊断。
        const ssize_t sent = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent >= 0) {
            // SOCK_SEQPACKET 不存在部分发送：要么整帧入队，要么返回错误。
            if (static_cast<size_t>(sent) != bytes.size()) {
                error = "short SOCK_SEQPACKET send";
                return IoStatus::SYSTEM_ERROR;
            }
            return IoStatus::OK;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 发送缓冲暂时满了。等到可写后整帧重发即可，不需要记录偏移。
            const IoStatus status = WaitFor(fd, POLLOUT, deadline, error);
            if (status != IoStatus::OK) {
                return status;
            }
            continue;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            error = ErrnoMessage("send");
            return IoStatus::CLOSED;
        }
        // 其余错误（含 EMSGSIZE：帧超出 socket 缓冲上限）重试都不会成功。
        error = ErrnoMessage("send");
        return IoStatus::SYSTEM_ERROR;
    }
}

IoStatus ReceiveFrame(int fd, Frame& frame, DeadlineMs deadline, std::string& error)
{
    std::array<uint8_t, MAX_FRAME_SIZE> buffer{};
    while (true) {
        const IoStatus waited = WaitFor(fd, POLLIN, deadline, error);
        if (waited != IoStatus::OK) {
            return waited;
        }

        iovec iov{};
        iov.iov_base = buffer.data();
        iov.iov_len = buffer.size();
        msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;

        const ssize_t received = recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            // poll 报告可读之后 recvmsg 仍可能返回 EAGAIN：数据报可能被内核丢弃，也可能被
            // 另一个线程抢先取走。这不是错误，回等待循环按同一个 deadline 继续等。
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            error = ErrnoMessage("recvmsg");
            return IoStatus::SYSTEM_ERROR;
        }
        if (received == 0) {
            // SOCK_SEQPACKET 本身允许零长数据报，但本协议每帧必含固定长度的帧头，
            // 不存在零长帧，因此返回 0 只可能是对端关闭。
            // 将来若新增任何零长消息（例如心跳空包），这条判定立即失效。
            return IoStatus::CLOSED;
        }
        if ((static_cast<unsigned int>(message.msg_flags) & static_cast<unsigned int>(MSG_TRUNC)) != 0U) {
            // SOCK_SEQPACKET 在接收缓冲小于数据报时会静默丢弃超出部分，返回值看起来
            // 就是一个合法的短帧。不检查这个标志，截断的帧会被当成正常帧解码。
            error = "truncated frame";
            return IoStatus::PROTOCOL_ERROR;
        }
        if (!DecodeFrame(buffer.data(), static_cast<size_t>(received), frame, error)) {
            return IoStatus::PROTOCOL_ERROR;
        }
        return IoStatus::OK;
    }
}

} // namespace npucheck::ipc
