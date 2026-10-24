/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_IPC_UDS_SERVER_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_IPC_UDS_SERVER_H

#include "uds_transport.h"
#include "wire_protocol.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace npucheck::ipc {

class UdsServer {
public:
    UdsServer() = default;
    ~UdsServer();
    UdsServer(const UdsServer&) = delete;
    UdsServer& operator=(const UdsServer&) = delete;

    bool StartAndHandshake(npucheck::ipc::ConfigureRequest& configure, std::string& error);
    // Ready 的 payload 必须为空：它只表达"会话就绪"这一个事实。会话细节（工具、回调数
    // 等）属于 Server 自己的维测信息，写本地日志，不占线路。
    bool SendReady(std::string& error);
    bool Publish(npucheck::ipc::MessageType type, const std::string& message);
    // 握手阶段的失败。domain/code 见 wire_protocol.h，供 CLI 记结构化日志；message 只供人读。
    void SendInitializationError(npucheck::ipc::ErrorDomain domain, uint16_t code, const std::string& message);
    // Ready 之后的失败。与 Result 互斥，发出后不再发任何 Result 分片。
    void SendError(npucheck::ipc::ErrorDomain domain, uint16_t code, const std::string& message) noexcept;
    // 把整份报告作为一个 Result 分片序列发出。除末帧外都置 npucheck::ipc::FLAG_MORE，hasErrors 与
    // truncated 只体现在末帧上。内部会先停掉 publisher 线程，保证分片之间不被插入
    // 实时诊断帧。
    bool SendResult(const std::string& report, bool hasErrors, bool truncated, std::string& error);
    // 排空实时诊断队列并结束 publisher 线程。调用方需要在统计"丢弃了多少消息"之前
    // 先调它，否则拿到的是尚未落定的中间值。可重复调用。
    void StopPublisher();
    void Shutdown();

    uint64_t SessionId() const;
    uint64_t DroppedMessages() const;
    bool TransportComplete() const;
    // 与对端协商出的 minor，取双方较小值。V1 恒为 0，记入日志供跨版本定位。
    uint16_t NegotiatedMinor() const;

private:
    struct QueuedMessage {
        npucheck::ipc::MessageType type = npucheck::ipc::MessageType::DIAGNOSTIC_STREAM;
        std::vector<uint8_t> payload;
    };

    bool LoadEnvironment(std::string& error);
    bool CreateListener(std::string& error);
    bool AcceptClient(std::string& error);
    // 收一帧并做统一校验：跳过 must-ignore 类型、校验 session_id、校验 sequence 严格
    // 递增、再确认类型符合预期。
    bool ReceiveChecked(
        npucheck::ipc::Frame& frame, npucheck::ipc::MessageType expected, npucheck::ipc::DeadlineMs deadline,
        std::string& error);
    bool ExchangeHandshake(npucheck::ipc::ConfigureRequest& configure, std::string& error);
    bool SendSynchronous(
        npucheck::ipc::MessageType type, const std::vector<uint8_t>& payload, uint16_t flags,
        npucheck::ipc::DeadlineMs deadline, std::string& error);
    void StartPublisher();
    void PublisherLoop();
    void CloseDescriptors();

    int listenFd_ = -1;
    int clientFd_ = -1;
    // 抽象命名空间地址名（含前导 '@'），不是文件路径。
    std::string udsName_;
    uint64_t sessionId_ = 0;
    uint16_t negotiatedMinor_ = npucheck::ipc::PROTOCOL_MINOR;
    uint32_t expectedCliPid_ = 0;
    int handshakeTimeoutMs_ = 10000;
    // 握手阶段的绝对截止时刻，在 AcceptClient 中一次算出，覆盖 accept、Hello 往返、
    // Configure 接收到 Ready 发出的全过程。Ready 之后的发送不再受它约束。
    npucheck::ipc::DeadlineMs handshakeDeadline_ = npucheck::ipc::NO_DEADLINE;
    uint64_t sendSequence_ = 1;
    // 期望从 CLI 方向收到的下一个 sequence。两个方向各自维护计数器，互不影响。
    uint64_t receiveSequence_ = 1;
    std::mutex sendMutex_;

    mutable std::mutex queueMutex_;
    std::condition_variable queueReady_;
    std::deque<QueuedMessage> queue_;
    std::thread publisher_;
    bool publisherStarted_ = false;
    bool publisherStopped_ = false;
    bool closing_ = false;
    bool transportComplete_ = true;
    uint64_t droppedMessages_ = 0;
    static constexpr size_t MAX_QUEUED_MESSAGES = 1024;
    // listen 的 backlog。抽象地址没有权限位，任何进程都能发起 connect，取 1 会让一个
    // 抢先连接就堵死真正的 CLI，因此留出余量并配合循环 accept 丢弃不合格连接。
    static constexpr int LISTEN_BACKLOG = 8;
    // Ready 之后发送结果的超时上限。CLI 侧的等待不设超时，但发送侧不能照搬：若 CLI 已死
    // 或长时间不读，sendmsg 会在发送缓冲填满后持续 EAGAIN，无限等待 POLLOUT 会把目标
    // 应用卡在退出路径上、NPU 资源不释放。超时即放弃发送，让应用正常退出。
    static constexpr int RESULT_SEND_TIMEOUT_MS = 30000;
};

} // namespace npucheck::ipc

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_IPC_UDS_SERVER_H
