// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "ipc/uds_server.h"

#include "uds_transport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace npucheck::ipc {
namespace {

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value) : name_(name)
    {
        if (const char* current = std::getenv(name); current != nullptr) {
            original_ = current;
        }
        (void)setenv(name, value.c_str(), 1);
    }

    ~ScopedEnvironmentVariable()
    {
        if (original_.has_value()) {
            (void)setenv(name_.c_str(), original_->c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> original_;
};

struct ClientResult {
    Frame ready;
    Frame flowError;
    std::string error;
};

// Result 是分片序列，逐帧留下来才能检查 MORE / HAS_ERRORS / TRUNCATED 的落位。
struct ResultClientResult {
    std::vector<Frame> frames;
    std::string error;
};

TEST(UdsTransportTest, ValidatesAbstractAddressLengthWithoutAddingTrailingBytes)
{
    sockaddr_un address{};
    socklen_t length = 0;
    std::string error;
    for (const std::string name : {"", "@", "plain-name"}) {
        EXPECT_FALSE(BuildAbstractAddress(name, address, length, error));
    }
    const std::string body(sizeof(address.sun_path) - 1, 'x');
    ASSERT_TRUE(BuildAbstractAddress("@" + body, address, length, error)) << error;
    EXPECT_EQ(address.sun_family, AF_UNIX);
    EXPECT_EQ(address.sun_path[0], '\0');
    EXPECT_EQ(std::string(address.sun_path + 1, body.size()), body);
    EXPECT_EQ(length, offsetof(sockaddr_un, sun_path) + sizeof(address.sun_path));
    EXPECT_FALSE(BuildAbstractAddress("@" + body + "x", address, length, error));

    ASSERT_TRUE(BuildAbstractAddress("@a", address, length, error)) << error;
    EXPECT_EQ(length, offsetof(sockaddr_un, sun_path) + 2);
    EXPECT_EQ(address.sun_path[1], 'a');
}

TEST(UdsServerTest, SendFailureMarksTransportIncomplete)
{
    UdsServer server;
    EXPECT_TRUE(server.TransportComplete());
    std::string error;
    EXPECT_FALSE(server.SendReady(error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(server.TransportComplete());
    EXPECT_FALSE(server.Publish(MessageType::DIAGNOSTIC_STREAM, "after failure"));
    EXPECT_EQ(server.DroppedMessages(), 1U);
    server.Shutdown();
}

bool SendClientFrame(
    int fd, MessageType type, uint64_t sessionId, uint64_t sequence, std::vector<uint8_t> payload, std::string& error)
{
    Frame frame{};
    frame.type = type;
    frame.sessionId = sessionId;
    frame.sequence = sequence;
    frame.payload = std::move(payload);
    return SendFrame(fd, frame, DeadlineAfterMs(2000), error) == IoStatus::OK;
}

// sendMustIgnoreFrame 为真时，在 CLIENT_HELLO 与 CONFIGURE 之间插入一帧 must-ignore
// 类型的消息，用来验证 Server 跳过它之后仍能正确接上后续帧。
//
// 这里刻意不给默认实参：std::thread 会把函数退化成函数指针，而默认实参属于声明、不属于
// 函数类型，因此通过指针调用时不会被填充，少传一个参数是编译错误而不是"取默认值"。
void RunClient(const std::string& udsName, uint64_t sessionId, ClientResult& result, bool sendMustIgnoreFrame)
{
    sockaddr_un address{};
    socklen_t addrLen = 0;
    if (!BuildAbstractAddress(udsName, address, addrLen, result.error)) {
        return;
    }

    // 抽象地址不落文件系统，没法用"文件是否存在"来等 Server 就绪，只能重试 connect。
    // Server 还没 bind 或还没 listen 时都表现为 ECONNREFUSED。
    const DeadlineMs deadline = DeadlineAfterMs(2000);
    int fd = -1;
    while (true) {
        fd = CreateSeqpacketSocket(result.error);
        if (fd < 0) {
            return;
        }
        if (connect(fd, reinterpret_cast<const sockaddr*>(&address), addrLen) == 0) {
            break;
        }
        (void)close(fd);
        fd = -1;
        if (MonotonicNowMs() >= deadline) {
            result.error = "client connect timed out";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // 发送方向的序号严格自增，被跳过的帧同样占一个序号。
    uint64_t sequence = 1;

    HelloPayload hello{};
    hello.pid = static_cast<uint32_t>(getpid());
    hello.uid = static_cast<uint32_t>(getuid());
    if (!SendClientFrame(fd, MessageType::CLIENT_HELLO, sessionId, sequence++, EncodeHello(hello), result.error)) {
        (void)close(fd);
        return;
    }

    Frame serverHello{};
    if (ReceiveFrame(fd, serverHello, deadline, result.error) != IoStatus::OK ||
        serverHello.type != MessageType::SERVER_HELLO || serverHello.sequence != 1) {
        if (result.error.empty()) {
            result.error = "invalid server hello";
        }
        (void)close(fd);
        return;
    }

    if (sendMustIgnoreFrame &&
        !SendClientFrame(
            fd, MessageType::DIAGNOSTIC_STREAM, sessionId, sequence++, EncodeText("ignore me"), result.error)) {
        (void)close(fd);
        return;
    }

    ConfigureRequest configure;
    ToolRequest tool;
    tool.toolId = ToolId::MEMCHECK;
    configure.tools.push_back(std::move(tool));
    if (!SendClientFrame(fd, MessageType::CONFIGURE, sessionId, sequence++, EncodeConfigure(configure), result.error) ||
        ReceiveFrame(fd, result.ready, deadline, result.error) != IoStatus::OK ||
        ReceiveFrame(fd, result.flowError, deadline, result.error) != IoStatus::OK) {
        (void)close(fd);
        return;
    }
    (void)close(fd);
}

// 走完握手后持续收帧，直到拿到 MORE=0 的 Result 末帧为止，把整个分片序列原样留下。
void RunResultClient(const std::string& udsName, uint64_t sessionId, ResultClientResult& result)
{
    sockaddr_un address{};
    socklen_t addrLen = 0;
    if (!BuildAbstractAddress(udsName, address, addrLen, result.error)) {
        return;
    }

    const DeadlineMs deadline = DeadlineAfterMs(5000);
    int fd = -1;
    while (true) {
        fd = CreateSeqpacketSocket(result.error);
        if (fd < 0) {
            return;
        }
        if (connect(fd, reinterpret_cast<const sockaddr*>(&address), addrLen) == 0) {
            break;
        }
        (void)close(fd);
        fd = -1;
        if (MonotonicNowMs() >= deadline) {
            result.error = "client connect timed out";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    uint64_t sequence = 1;
    HelloPayload hello{};
    hello.pid = static_cast<uint32_t>(getpid());
    hello.uid = static_cast<uint32_t>(getuid());
    Frame serverHello{};
    ConfigureRequest configure;
    ToolRequest tool;
    tool.toolId = ToolId::MEMCHECK;
    configure.tools.push_back(std::move(tool));
    Frame ready{};
    if (!SendClientFrame(fd, MessageType::CLIENT_HELLO, sessionId, sequence++, EncodeHello(hello), result.error) ||
        ReceiveFrame(fd, serverHello, deadline, result.error) != IoStatus::OK ||
        !SendClientFrame(fd, MessageType::CONFIGURE, sessionId, sequence++, EncodeConfigure(configure), result.error) ||
        ReceiveFrame(fd, ready, deadline, result.error) != IoStatus::OK) {
        (void)close(fd);
        return;
    }

    while (true) {
        Frame frame{};
        if (ReceiveFrame(fd, frame, deadline, result.error) != IoStatus::OK) {
            break;
        }
        result.frames.push_back(frame);
        if (frame.type == MessageType::RESULT && (frame.flags & FLAG_MORE) == 0) {
            break;
        }
    }
    (void)close(fd);
}

// Result 必须按 MAX_PAYLOAD_SIZE 切片，除末帧外都置 MORE，结论位只出现在末帧上。
TEST(UdsServerTest, FragmentsResultAcrossFramesAndFlagsOnlyTheLastOne)
{
    constexpr uint64_t SESSION_ID = 90212;
    const std::string udsName = "@npu_check_uds_result_test_" + std::to_string(static_cast<uint64_t>(getpid()));
    ScopedEnvironmentVariable udsNameEnvironment(UDS_NAME_ENV, udsName);
    ScopedEnvironmentVariable sessionEnvironment(SESSION_ID_ENV, std::to_string(SESSION_ID));
    ScopedEnvironmentVariable pidEnvironment(CLI_PID_ENV, std::to_string(static_cast<uint64_t>(getpid())));
    ScopedEnvironmentVariable timeoutEnvironment(HANDSHAKE_TIMEOUT_ENV, "5000");

    // 刻意跨过两个整分片，验证边界处既不丢字节也不多发空帧。
    const std::string report(MAX_PAYLOAD_SIZE * 2 + 17, 'r');

    ResultClientResult clientResult{};
    std::thread client(RunResultClient, udsName, SESSION_ID, std::ref(clientResult));

    npucheck::ipc::UdsServer server;
    ConfigureRequest config{};
    std::string serverError;
    const bool handshakeSucceeded = server.StartAndHandshake(config, serverError);
    const bool readySucceeded = handshakeSucceeded && server.SendReady(serverError);
    bool resultSucceeded = false;
    if (readySucceeded) {
        resultSucceeded = server.SendResult(report, true, false, serverError);
    }
    client.join();
    server.Shutdown();

    ASSERT_TRUE(handshakeSucceeded) << serverError;
    ASSERT_TRUE(readySucceeded) << serverError;
    ASSERT_TRUE(resultSucceeded) << serverError;
    ASSERT_TRUE(clientResult.error.empty()) << clientResult.error;
    ASSERT_EQ(clientResult.frames.size(), 3U);

    std::string received;
    for (size_t index = 0; index < clientResult.frames.size(); ++index) {
        const Frame& frame = clientResult.frames[index];
        EXPECT_EQ(frame.type, MessageType::RESULT);
        // Ready 占用了序号 2，Result 从 3 开始严格递增。
        EXPECT_EQ(frame.sequence, index + 3);
        const bool isLast = index + 1 == clientResult.frames.size();
        EXPECT_EQ((frame.flags & FLAG_MORE) != 0, !isLast);
        EXPECT_EQ((frame.flags & FLAG_HAS_ERRORS) != 0, isLast);
        EXPECT_FALSE((frame.flags & FLAG_TRUNCATED) != 0);
        received.append(frame.payload.begin(), frame.payload.end());
    }
    EXPECT_EQ(received, report);
}

// 没有任何诊断时也必须发出一个空的末帧：接收端据此区分"检查完成、无问题"与"报告丢失"。
TEST(UdsServerTest, SendsOneEmptyFrameForAnEmptyResult)
{
    constexpr uint64_t SESSION_ID = 90213;
    const std::string udsName = "@npu_check_uds_empty_result_test_" + std::to_string(static_cast<uint64_t>(getpid()));
    ScopedEnvironmentVariable udsNameEnvironment(UDS_NAME_ENV, udsName);
    ScopedEnvironmentVariable sessionEnvironment(SESSION_ID_ENV, std::to_string(SESSION_ID));
    ScopedEnvironmentVariable pidEnvironment(CLI_PID_ENV, std::to_string(static_cast<uint64_t>(getpid())));
    ScopedEnvironmentVariable timeoutEnvironment(HANDSHAKE_TIMEOUT_ENV, "5000");

    ResultClientResult clientResult{};
    std::thread client(RunResultClient, udsName, SESSION_ID, std::ref(clientResult));

    npucheck::ipc::UdsServer server;
    ConfigureRequest config{};
    std::string serverError;
    const bool handshakeSucceeded = server.StartAndHandshake(config, serverError);
    const bool readySucceeded = handshakeSucceeded && server.SendReady(serverError);
    bool resultSucceeded = false;
    if (readySucceeded) {
        resultSucceeded = server.SendResult({}, false, false, serverError);
    }
    client.join();
    server.Shutdown();

    ASSERT_TRUE(handshakeSucceeded) << serverError;
    ASSERT_TRUE(resultSucceeded) << serverError;
    ASSERT_TRUE(clientResult.error.empty()) << clientResult.error;
    ASSERT_EQ(clientResult.frames.size(), 1U);
    EXPECT_EQ(clientResult.frames[0].type, MessageType::RESULT);
    EXPECT_EQ(clientResult.frames[0].flags, 0U);
    EXPECT_TRUE(clientResult.frames[0].payload.empty());
}

TEST(UdsServerTest, SendsFlowErrorSynchronouslyAfterReady)
{
    constexpr uint64_t SESSION_ID = 90210;
    // '@' 前缀表示抽象命名空间；带上 pid 避免并发跑用例时撞车。
    const std::string udsName = "@npu_check_uds_test_" + std::to_string(static_cast<uint64_t>(getpid()));
    ScopedEnvironmentVariable udsNameEnvironment(UDS_NAME_ENV, udsName);
    ScopedEnvironmentVariable sessionEnvironment(SESSION_ID_ENV, std::to_string(SESSION_ID));
    ScopedEnvironmentVariable pidEnvironment(CLI_PID_ENV, std::to_string(static_cast<uint64_t>(getpid())));
    ScopedEnvironmentVariable timeoutEnvironment(HANDSHAKE_TIMEOUT_ENV, "2000");

    ClientResult clientResult{};
    std::thread client(RunClient, udsName, SESSION_ID, std::ref(clientResult), false);

    npucheck::ipc::UdsServer server;
    ConfigureRequest config{};
    std::string serverError;
    const bool handshakeSucceeded = server.StartAndHandshake(config, serverError);
    const bool readySucceeded = handshakeSucceeded && server.SendReady(serverError);
    if (readySucceeded) {
        server.SendError(ErrorDomain::INTERNAL, error_code::REPORT_UNAVAILABLE, "callback processing failed");
    }
    client.join();
    server.Shutdown();

    ASSERT_TRUE(handshakeSucceeded) << serverError;
    ASSERT_TRUE(readySucceeded) << serverError;
    ASSERT_TRUE(clientResult.error.empty()) << clientResult.error;
    ASSERT_EQ(config.tools.size(), 1U);
    EXPECT_EQ(config.tools[0].toolId, ToolId::MEMCHECK);
    EXPECT_EQ(clientResult.ready.type, MessageType::READY);
    EXPECT_EQ(clientResult.ready.sequence, 2U);
    EXPECT_EQ(clientResult.flowError.type, MessageType::ERROR);
    EXPECT_EQ(clientResult.flowError.sequence, 3U);
    // Ready 的 payload 必须为空，会话细节只写 Server 本地日志。
    EXPECT_TRUE(clientResult.ready.payload.empty());
    // Error 走 8 字节结构化头 + 文本，CLI 据 domain/code 记日志，message 只供人读。
    ErrorPayload flowError{};
    ASSERT_TRUE(DecodeError(clientResult.flowError.payload, flowError, clientResult.error));
    EXPECT_EQ(flowError.domain, ErrorDomain::INTERNAL);
    EXPECT_EQ(flowError.code, error_code::REPORT_UNAVAILABLE);
    EXPECT_EQ(flowError.message, "callback processing failed");
}

// 回归测试：must-ignore 帧必须被跳过，且不能打乱后续帧的序号校验。
//
// 曾经的写法把"跳过 must-ignore"放在序号校验之前，跳过一帧就少消耗一个序号，导致紧随
// 其后的 CONFIGURE 被误判为 non-contiguous、握手直接失败。这里在 CLIENT_HELLO 与
// CONFIGURE 之间插一帧 DIAGNOSTIC_STREAM，正是当年会踩中的场景。
TEST(UdsServerTest, SkipsMustIgnoreFramesWithoutBreakingSequence)
{
    constexpr uint64_t SESSION_ID = 90211;
    const std::string udsName = "@npu_check_uds_skip_test_" + std::to_string(static_cast<uint64_t>(getpid()));
    ScopedEnvironmentVariable udsNameEnvironment(UDS_NAME_ENV, udsName);
    ScopedEnvironmentVariable sessionEnvironment(SESSION_ID_ENV, std::to_string(SESSION_ID));
    ScopedEnvironmentVariable pidEnvironment(CLI_PID_ENV, std::to_string(static_cast<uint64_t>(getpid())));
    ScopedEnvironmentVariable timeoutEnvironment(HANDSHAKE_TIMEOUT_ENV, "2000");

    ClientResult clientResult{};
    std::thread client(RunClient, udsName, SESSION_ID, std::ref(clientResult), true);

    npucheck::ipc::UdsServer server;
    ConfigureRequest config{};
    std::string serverError;
    const bool handshakeSucceeded = server.StartAndHandshake(config, serverError);
    const bool readySucceeded = handshakeSucceeded && server.SendReady(serverError);
    if (readySucceeded) {
        server.SendError(ErrorDomain::INTERNAL, error_code::REPORT_UNAVAILABLE, "callback processing failed");
    }
    client.join();
    server.Shutdown();

    // 跳过那一帧之后，CONFIGURE 仍应被正常接收并解出，握手照常完成。
    ASSERT_TRUE(handshakeSucceeded) << serverError;
    ASSERT_TRUE(readySucceeded) << serverError;
    ASSERT_TRUE(clientResult.error.empty()) << clientResult.error;
    ASSERT_EQ(config.tools.size(), 1U);
    EXPECT_EQ(config.tools[0].toolId, ToolId::MEMCHECK);
    EXPECT_EQ(clientResult.ready.type, MessageType::READY);
}

} // namespace
} // namespace npucheck::ipc
