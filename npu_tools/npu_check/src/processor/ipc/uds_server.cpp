/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "ipc/uds_server.h"

#include "uds_transport.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace npucheck::ipc {
namespace {

std::optional<uint64_t> ParseUnsigned(const char* text, uint64_t maximum)
{
    if (text == nullptr || text[0] == '\0') {
        return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(parsed);
}

std::string ErrnoMessage(const char* operation) { return std::string(operation) + ": " + std::strerror(errno); }

} // namespace

UdsServer::~UdsServer() { Shutdown(); }

bool UdsServer::LoadEnvironment(std::string& error)
{
    const char* udsName = std::getenv(npucheck::ipc::kUdsNameEnv);
    const char* session = std::getenv(npucheck::ipc::kSessionIdEnv);
    const char* cliPid = std::getenv(npucheck::ipc::kCliPidEnv);
    if (udsName == nullptr || udsName[0] == '\0') {
        error = "missing UDS address environment";
        return false;
    }
    const auto parsedSession = ParseUnsigned(session, std::numeric_limits<uint64_t>::max());
    const auto parsedPid = ParseUnsigned(cliPid, std::numeric_limits<uint32_t>::max());
    if (!parsedSession || *parsedSession == 0 || !parsedPid || *parsedPid == 0) {
        error = "invalid UDS session or CLI PID environment";
        return false;
    }
    udsName_ = udsName;
    sessionId_ = *parsedSession;
    expectedCliPid_ = static_cast<uint32_t>(*parsedPid);

    const char* timeout = std::getenv(npucheck::ipc::kHandshakeTimeoutEnv);
    if (timeout != nullptr) {
        const auto parsedTimeout = ParseUnsigned(timeout, MAX_HANDSHAKE_TIMEOUT_MS);
        if (!parsedTimeout || *parsedTimeout < MIN_HANDSHAKE_TIMEOUT_MS) {
            error = "invalid handshake timeout environment";
            return false;
        }
        handshakeTimeoutMs_ = static_cast<int>(*parsedTimeout);
    }
    return true;
}

bool UdsServer::CreateListener(std::string& error)
{
    sockaddr_un address{};
    socklen_t addrLen = 0;
    if (!npucheck::ipc::BuildAbstractAddress(udsName_, address, addrLen, error)) {
        return false;
    }
    listenFd_ = npucheck::ipc::CreateSeqpacketSocket(error);
    if (listenFd_ < 0) {
        return false;
    }
    // 抽象命名空间下没有 socket 文件，因此既不需要事先 unlink，也不需要收尾时清理：
    // 地址在最后一个引用它的 fd 关闭时由内核自动回收，进程被 SIGKILL 也不会残留。
    if (bind(listenFd_, reinterpret_cast<const sockaddr*>(&address), addrLen) != 0) {
        // EADDRINUSE 说明同一会话中已有另一个进程绑定了这个地址（例如多进程作业里
        // 的第二个进程也继承了环境变量并加载了注入库）。调用方据此打印诊断后放行，
        // 让应用不做检查地正常运行。这条互斥是"地址唯一"免费提供的。
        error = ErrnoMessage("bind");
        return false;
    }
    // backlog 不能是 1。抽象地址没有文件系统权限位，同一 network namespace 内任何进程
    // 都能发起 connect；backlog 为 1 时，一个抢先建立的连接就会让真正的 CLI 一直拿到
    // EAGAIN 直到超时。取 8 留出余量，配合下面的循环 accept 一起用。
    if (listen(listenFd_, kListenBacklog) != 0) {
        error = ErrnoMessage("listen");
        return false;
    }
    return true;
}

bool UdsServer::AcceptClient(std::string& error)
{
    // 握手阶段的绝对截止时刻。注意这个 deadline 从这里一直用到 Ready 发出为止，
    // 而不是每一步各给一份完整超时。
    const npucheck::ipc::DeadlineMs deadline = npucheck::ipc::DeadlineAfterMs(handshakeTimeoutMs_);

    // 去掉文件系统这一层之后，SO_PEERCRED 是唯一的身份闸门，因此必须能容忍并丢弃
    // 无关连接：任何 UID 都可以连上来，但只有 pid/uid 都对得上的那个才是本次会话的
    // CLI。取到不合格的连接就关掉继续 accept，不能就此判定会话失败 —— 否则外部进程
    // 随便连一下就能让检查失败。
    while (true) {
        const npucheck::ipc::IoStatus waited = npucheck::ipc::WaitFor(listenFd_, POLLIN, deadline, error);
        if (waited != npucheck::ipc::IoStatus::OK) {
            if (waited == npucheck::ipc::IoStatus::TIMEOUT) {
                error = "timed out waiting for the npu_check client";
            }
            return false;
        }

        int candidate = -1;
        do {
            candidate = accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        } while (candidate < 0 && errno == EINTR);
        if (candidate < 0) {
            // poll 报告可读后 accept4 仍可能返回 EAGAIN（连接被对端撤回，或被别的线程
            // 抢走），回等待循环即可。
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            error = ErrnoMessage("accept4");
            return false;
        }

        // SO_PEERCRED 必须在 accept4 返回的连接 fd 上取。在监听 fd 上取到的是本进程
        // 自己的凭据，校验会永远通过，等于没有这道闸门。
        ucred peer{};
        socklen_t peerSize = sizeof(peer);
        if (getsockopt(candidate, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0) {
            (void)close(candidate);
            continue;
        }
        if (peer.uid != getuid() || static_cast<uint32_t>(peer.pid) != expectedCliPid_) {
            (void)close(candidate);
            continue;
        }

        clientFd_ = candidate;
        // 取得合法连接后立刻关闭监听 fd，杜绝第二个连接。
        if (listenFd_ >= 0) {
            (void)close(listenFd_);
            listenFd_ = -1;
        }
        handshakeDeadline_ = deadline;
        return true;
    }
}

bool UdsServer::ReceiveChecked(
    npucheck::ipc::Frame& frame, npucheck::ipc::MessageType expected, npucheck::ipc::DeadlineMs deadline,
    std::string& error)
{
    while (true) {
        if (npucheck::ipc::ReceiveFrame(clientFd_, frame, deadline, error) != npucheck::ipc::IoStatus::OK) {
            return false;
        }
        if (frame.sessionId != sessionId_) {
            error = "received frame for another session";
            return false;
        }
        // 来自 CLI 方向的第一帧必须为 1，其后严格 +1。原实现在各调用点硬编码判断
        // sequence == 1 / == 2，新增消息就会漏校验，这里统一到一处。
        //
        // 顺序很重要：sequence 计数的是"线路上流过的帧"，发送侧对每一帧都会自增，
        // 因此被跳过的帧同样要消耗一个序号。若把 must-ignore 的跳过放在这段校验之前，
        // 跳过一帧就会让本侧计数比对端少 1，紧随其后的下一帧必然被误判为不连续。
        if (frame.sequence != receiveSequence_) {
            error = "non-contiguous client sequence";
            return false;
        }
        ++receiveSequence_;
        // must-ignore 类型（最高位为 1）必须跳过并继续等，不能当成协议错误。握手阶段
        // 本不该收到这类帧，但规则要一致，否则将来 CLI 侧新增可选消息就会打挂握手。
        if (npucheck::ipc::IsMustIgnoreType(static_cast<uint16_t>(frame.type))) {
            continue;
        }
        if (frame.type != expected) {
            error = "unexpected message type during handshake";
            return false;
        }
        return true;
    }
}

bool UdsServer::ExchangeHandshake(npucheck::ipc::ConfigureRequest& configure, std::string& error)
{
    // 握手的每一步都复用 AcceptClient 算出的同一个绝对 deadline。
    npucheck::ipc::Frame helloFrame{};
    if (!ReceiveChecked(helloFrame, npucheck::ipc::MessageType::CLIENT_HELLO, handshakeDeadline_, error)) {
        return false;
    }
    npucheck::ipc::HelloPayload hello{};
    if (!npucheck::ipc::DecodeHello(helloFrame.payload, hello, error)) {
        return false;
    }
    // 对端 minor 取自 Hello 的帧头，与本实现取较小值。收到更高的 minor 不是错误，
    // 详见 npucheck::ipc::NegotiateMinor 的说明。
    negotiatedMinor_ = npucheck::ipc::NegotiateMinor(helloFrame.minor);
    // Hello 携带的 pid/uid 与 accept 时读到的 SO_PEERCRED 交叉比对。后者来自内核、
    // 无法伪造，是权威来源；前者只是对端的自我声明，两者必须一致。
    if (hello.pid != expectedCliPid_ || hello.uid != getuid()) {
        error = "client hello identity mismatch";
        return false;
    }

    npucheck::ipc::HelloPayload serverHello{};
    serverHello.pid = static_cast<uint32_t>(getpid());
    serverHello.uid = static_cast<uint32_t>(getuid());
    if (!SendSynchronous(
            npucheck::ipc::MessageType::SERVER_HELLO, npucheck::ipc::EncodeHello(serverHello), 0, handshakeDeadline_,
            error)) {
        return false;
    }

    npucheck::ipc::Frame configFrame{};
    if (!ReceiveChecked(configFrame, npucheck::ipc::MessageType::CONFIGURE, handshakeDeadline_, error)) {
        return false;
    }
    if (!npucheck::ipc::DecodeConfigure(configFrame.payload, configure, error)) {
        return false;
    }
    // 解码只保证结构合法；能不能理解这些选项还取决于协商出的 minor。不能假设对端
    // 守规矩地只发它该发的东西。
    return npucheck::ipc::ValidateConfigureMinor(configure, negotiatedMinor_, error);
}

bool UdsServer::StartAndHandshake(npucheck::ipc::ConfigureRequest& configure, std::string& error)
{
    if (!LoadEnvironment(error) || !CreateListener(error) || !AcceptClient(error) ||
        !ExchangeHandshake(configure, error)) {
        return false;
    }
    return true;
}

bool UdsServer::SendSynchronous(
    npucheck::ipc::MessageType type, const std::vector<uint8_t>& payload, uint16_t flags,
    npucheck::ipc::DeadlineMs deadline, std::string& error)
{
    {
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        npucheck::ipc::Frame frame{};
        frame.type = type;
        frame.flags = flags;
        frame.sessionId = sessionId_;
        frame.sequence = sendSequence_++;
        frame.payload = payload;
        if (npucheck::ipc::SendFrame(clientFd_, frame, deadline, error) == npucheck::ipc::IoStatus::OK) {
            return true;
        }
    }
    std::lock_guard<std::mutex> lock(queueMutex_);
    transportComplete_ = false;
    return false;
}

bool UdsServer::SendReady(std::string& error)
{
    // payload 恒为空：Ready 只表达"会话就绪"。
    if (!SendSynchronous(npucheck::ipc::MessageType::READY, {}, 0, handshakeDeadline_, error)) {
        return false;
    }
    // Ready 已发出，握手 deadline 到此结束；后续结果发送改用独立的上限。
    StartPublisher();
    return true;
}

void UdsServer::StartPublisher()
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (publisherStarted_) {
        return;
    }
    publisherStarted_ = true;
    publisher_ = std::thread(&UdsServer::PublisherLoop, this);
}

bool UdsServer::Publish(npucheck::ipc::MessageType type, const std::string& message)
{
    auto payload = npucheck::ipc::EncodeText(message);
    if (payload.empty() && !message.empty()) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        ++droppedMessages_;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (clientFd_ < 0 || closing_ || !transportComplete_ || queue_.size() >= kMaxQueuedMessages) {
            ++droppedMessages_;
            return false;
        }
        queue_.push_back({type, std::move(payload)});
    }
    queueReady_.notify_one();
    return true;
}

void UdsServer::PublisherLoop()
{
    while (true) {
        QueuedMessage message{};
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueReady_.wait(lock, [this] { return closing_ || !queue_.empty(); });
            if (queue_.empty() && closing_) {
                return;
            }
            message = std::move(queue_.front());
            queue_.pop_front();
        }
        std::string error;
        // 结果发送用独立上限，不复用握手 deadline，也不能不设上限：CLI 已死或长时间不读时
        // sendmsg 会一直 EAGAIN，无限等待会把目标应用卡在退出路径上。
        if (!SendSynchronous(
                message.type, message.payload, 0, npucheck::ipc::DeadlineAfterMs(kResultSendTimeoutMs), error)) {
            std::lock_guard<std::mutex> lock(queueMutex_);
            droppedMessages_ += queue_.size();
            queue_.clear();
            closing_ = true;
            return;
        }
    }
}

void UdsServer::SendInitializationError(npucheck::ipc::ErrorDomain domain, uint16_t code, const std::string& message)
{
    if (clientFd_ < 0) {
        return;
    }
    std::string ignored;
    (void)SendSynchronous(
        npucheck::ipc::MessageType::ERROR, npucheck::ipc::EncodeError({domain, code, message}), 0, handshakeDeadline_,
        ignored);
}

void UdsServer::SendError(npucheck::ipc::ErrorDomain domain, uint16_t code, const std::string& message) noexcept
{
    try {
        if (clientFd_ < 0) {
            return;
        }
        // Error 与 Result 互斥：走到这里说明报告已经不可能完整交付，所以同样要先把实时
        // 诊断队列排空，避免 Error 之后还有孤零零的诊断帧继续流出去。
        StopPublisher();
        std::string ignored;
        (void)SendSynchronous(
            npucheck::ipc::MessageType::ERROR, npucheck::ipc::EncodeError({domain, code, message}), 0,
            npucheck::ipc::DeadlineAfterMs(kResultSendTimeoutMs), ignored);
    } catch (...) {
        return;
    }
}

bool UdsServer::SendResult(const std::string& report, bool hasErrors, bool truncated, std::string& error)
{
    // 实时诊断与 Result 共用同一条连接和同一个发送序号。publisher 线程还活着时，两个
    // 分片之间随时可能被插入一帧 DIAGNOSTIC_STREAM —— 接收端按 MORE 位拼接，插进来的
    // 帧本身会被跳过，但这会让"报告"和"实时流"在时间上交错，语义上不再是一个整体。
    // 因此先把 publisher 停干净，Result 序列独占后续线路。
    StopPublisher();
    if (clientFd_ < 0) {
        error = "no client connection for the result";
        return false;
    }
    if (report.size() > npucheck::ipc::kMaxResultBytes) {
        error = "result exceeds the maximum report size";
        return false;
    }
    // 整个分片序列共用一个绝对 deadline。分片数取决于报告长度，若每片各给一份完整超时，
    // 最坏会把上限放大到分片数倍，把目标应用长时间钉在退出路径上、NPU 资源不释放。
    const npucheck::ipc::DeadlineMs deadline = npucheck::ipc::DeadlineAfterMs(kResultSendTimeoutMs);
    size_t offset = 0;
    while (true) {
        const size_t chunk = std::min(npucheck::ipc::kMaxPayloadSize, report.size() - offset);
        const bool isLast = offset + chunk >= report.size();
        uint16_t flags = 0;
        if (!isLast) {
            flags |= npucheck::ipc::kFlagMore;
        } else {
            // 结论位只在末帧上有效：接收端在收到 MORE=0 之前不该对本次检查下任何结论。
            if (hasErrors) {
                flags |= npucheck::ipc::kFlagHasErrors;
            }
            if (truncated) {
                flags |= npucheck::ipc::kFlagTruncated;
            }
        }
        const auto begin = report.begin() + static_cast<std::ptrdiff_t>(offset);
        const std::vector<uint8_t> payload(begin, begin + static_cast<std::ptrdiff_t>(chunk));
        if (!SendSynchronous(npucheck::ipc::MessageType::RESULT, payload, flags, deadline, error)) {
            return false;
        }
        if (isLast) {
            // 报告为空时循环也恰好走一趟：chunk 为 0、isLast 为真，发出一个空的末帧。
            // 接收端据此知道"检查完成且没有任何诊断"，而不是把连接关闭当成报告丢失。
            return true;
        }
        offset += chunk;
    }
}

void UdsServer::StopPublisher()
{
    bool shouldJoin = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        closing_ = true;
        // publisherStopped_ 在锁内翻转，保证线程句柄只被处置一次：本函数在退出路径上
        // 会被 SendResult / SendError / Shutdown 依次调用，重复 join 同一个 thread 是 UB。
        shouldJoin = !publisherStopped_ && publisher_.joinable();
        publisherStopped_ = true;
    }
    queueReady_.notify_all();
    if (shouldJoin) {
        publisher_.join();
    }
}

void UdsServer::Shutdown()
{
    StopPublisher();
    CloseDescriptors();
}

void UdsServer::CloseDescriptors()
{
    if (clientFd_ >= 0) {
        (void)close(clientFd_);
        clientFd_ = -1;
    }
    if (listenFd_ >= 0) {
        (void)close(listenFd_);
        listenFd_ = -1;
    }
    // 抽象命名空间没有 socket 文件，关闭 fd 即由内核回收地址，无需 unlink。
}

uint64_t UdsServer::SessionId() const { return sessionId_; }

uint16_t UdsServer::NegotiatedMinor() const { return negotiatedMinor_; }

uint64_t UdsServer::DroppedMessages() const
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    return droppedMessages_;
}

bool UdsServer::TransportComplete() const
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    return transportComplete_;
}

} // namespace npucheck::ipc
