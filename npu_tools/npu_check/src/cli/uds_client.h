/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_CLI_UDS_CLIENT_H
#define NPU_TOOLS_NPU_CHECK_SRC_CLI_UDS_CLIENT_H

#include "uds_transport.h"

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace npucheck::ipc {

class UdsClient {
public:
    UdsClient() = default;
    ~UdsClient();
    UdsClient(const UdsClient&) = delete;
    UdsClient& operator=(const UdsClient&) = delete;

    // deadline 由调用方在 fork 之前算出并传入，覆盖 connect 重试、Hello 往返、
    // Configure 发送、Ready 接收的全过程，而不是每步各给一份完整超时。
    //
    // 起点必须在 fork 之前：exec 与 CANN 动态库加载是整段里最不可控的部分，把它排除
    // 在预算之外，用户设定的超时就形同虚设。因此这里收的是绝对时刻，不是时长。
    bool ConnectAndConfigure(
        const std::string& udsName, uint64_t sessionId, uint32_t childPid, npucheck::ipc::DeadlineMs deadline,
        const npucheck::ipc::ConfigureRequest& configure, std::string& error);
    // 采集阶段读取后续帧。deadline 传 npucheck::ipc::NO_DEADLINE 表示不设超时 —— 应用可能跑数小时。
    npucheck::ipc::IoStatus Receive(
        npucheck::ipc::Frame& frame, npucheck::ipc::DeadlineMs deadline, std::string& error);
    void Close();

    // 与对端协商出的 minor，取双方较小值。V1 恒为 0，供维测日志记录。
    uint16_t NegotiatedMinor() const { return negotiatedMinor_; }

private:
    bool ConnectWithRetry(
        const std::string& udsName, npucheck::ipc::DeadlineMs deadline, pid_t childPid, std::string& error);
    bool CheckServerIdentity(uint32_t childPid, npucheck::ipc::DeadlineMs deadline, std::string& error);
    bool Send(
        npucheck::ipc::MessageType type, const std::vector<uint8_t>& payload, npucheck::ipc::DeadlineMs deadline,
        std::string& error);

    int fd_ = -1;
    uint16_t negotiatedMinor_ = npucheck::ipc::PROTOCOL_MINOR;
    uint64_t sessionId_ = 0;
    uint64_t sendSequence_ = 1;
    uint64_t receiveSequence_ = 1;
};

} // namespace npucheck::ipc

#endif // NPU_TOOLS_NPU_CHECK_SRC_CLI_UDS_CLIENT_H
