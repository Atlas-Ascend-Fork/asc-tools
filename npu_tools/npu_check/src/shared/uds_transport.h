/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_SHARED_UDS_TRANSPORT_H
#define NPU_TOOLS_NPU_CHECK_SRC_SHARED_UDS_TRANSPORT_H

#include "wire_protocol.h"

#include <cstdint>
#include <limits>
// 调用方需要 POLLIN / POLLOUT 来填 WaitFor 的 events 参数。
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace npucheck::ipc {

enum class IoStatus : uint8_t {
    OK,
    CLOSED,
    TIMEOUT,
    SYSTEM_ERROR,
    PROTOCOL_ERROR,
};

// 绝对截止时刻，单位毫秒，取自 std::chrono::steady_clock。
//
// 这里刻意用"时刻"而不是"时长"：若往下传的是剩余毫秒数，被调函数内部一旦有重试循环
// （EINTR、EAGAIN 都会触发），每一轮都会拿到同一个完整时长，超时上限就随重试次数无限
// 延后。传绝对时刻则每轮都要重算 deadline - now，只减不增。
using DeadlineMs = int64_t;

// "不设超时"用一个值表达，而不是在每个调用点写特殊分支。
// 采集阶段等待结果时使用：应用可能跑数小时，任何固定超时都是错的。
constexpr DeadlineMs NO_DEADLINE = std::numeric_limits<DeadlineMs>::max();

// 单次 poll 的等待上限。切片使剩余时长不会超出 poll 的 int 参数范围，同时让等待循环
// 保持可响应；时间片到期后回到循环顶部，重新按绝对 deadline 判断。
constexpr int POLL_SLICE_MS = 1000;

// 必须是 CLOCK_MONOTONIC。CLOCK_REALTIME 会被 NTP 校时或手工改钟拨动，一次向前跳变
// 就能让 deadline - now 变成负数，把健康的握手判成超时。
int64_t MonotonicNowMs();

// 由"还能等多久"换算成绝对时刻。整个会话只在最外层换算一次，之后一路传绝对值。
inline DeadlineMs DeadlineAfterMs(int timeoutMs) { return MonotonicNowMs() + timeoutMs; }

// 创建 AF_UNIX + SOCK_SEQPACKET 套接字，并校验收发缓冲能容纳一个完整帧。
// SOCK_NONBLOCK / SOCK_CLOEXEC 在创建时一次设定，而不是事后 fcntl —— 后者会留下一段
// 仍是阻塞的窗口，以及一段可被 exec 继承出去的窗口。失败返回 -1。
int CreateSeqpacketSocket(std::string& error);

// 按抽象命名空间约定填充 sockaddr_un，并算出 bind/connect 必须使用的 addrLen。
// name 取自环境变量，以 '@' 开头，见 wire_protocol.h 中 UDS_NAME_ENV 的说明。
bool BuildAbstractAddress(const std::string& name, sockaddr_un& address, socklen_t& addrLen, std::string& error);

// 全项目唯一的等待原语。events 传 POLLIN 或 POLLOUT。
// 返回 CLOSED 表示对端已挂断且当前没有可读数据。
IoStatus WaitFor(int fd, short events, DeadlineMs deadline, std::string& error);

IoStatus SendFrame(int fd, const Frame& frame, DeadlineMs deadline, std::string& error);
IoStatus ReceiveFrame(int fd, Frame& frame, DeadlineMs deadline, std::string& error);

} // namespace npucheck::ipc

#endif // NPU_TOOLS_NPU_CHECK_SRC_SHARED_UDS_TRANSPORT_H
