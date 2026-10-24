/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_TOOL_MANAGER_TOOL_MANAGER_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_TOOL_MANAGER_TOOL_MANAGER_H

#include "acl_san/aclsan_api.h"
#include "checker/checker.h"
#include "diagnostic/report_renderer.h"
#include "diagnostic/report_buffer.h"
#include "ipc/uds_server.h"
#include "wire_protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace npucheck {

class ToolManager {
public:
    ToolManager() = default;
    ~ToolManager() noexcept;
    ToolManager(const ToolManager&) = delete;
    ToolManager& operator=(const ToolManager&) = delete;

    int Initialize();
    void Finalize();
    bool IsInitialized() const;

private:
    class ActiveCallbackGuard {
    public:
        explicit ActiveCallbackGuard(ToolManager& service) noexcept : service_(service) {}
        ~ActiveCallbackGuard() noexcept;
        ActiveCallbackGuard(const ActiveCallbackGuard&) = delete;
        ActiveCallbackGuard& operator=(const ActiveCallbackGuard&) = delete;

    private:
        ToolManager& service_;
    };

    static void Callback(
        void* userdata, AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata) noexcept;
    void OnCallback(AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata);
    void OnCallbackException(const char* reason) noexcept;
    bool EnterCallback();
    void LeaveCallback();
    bool ConfigureSanitizer(std::string& error);
    bool EnableCallbacks(std::string& error);
    void RollbackSanitizer();
    static void LogHandshakeFailure(const std::string& reason) noexcept;
    void StoreReports(CheckerReportList reports);
    void LogMalformed(AclsanCallbackDomain domain, AclsanCallbackId cbid, const char* reason);
    void LogCallback(AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata);
    std::string BuildToolNames() const;
    std::string BuildAndLogSummaryMessage() const;
    // 本次检查是否检出问题，对应 Result 末帧的 FLAG_HAS_ERRORS。
    bool HasDetectedErrors() const;

    mutable std::mutex lifecycleMutex_;
    std::mutex callbackMutex_;
    std::condition_variable callbacksDrained_;
    mutable std::mutex stateMutex_;
    uint64_t activeCallbacks_ = 0;
    bool stopping_ = false;
    bool initialized_ = false;
    bool subscribed_ = false;

    // 本次会话启用的工具及其子选项，按 toolId 升序。多个工具可以同时启用。
    npucheck::ipc::ConfigureRequest configure_{};
    // 工作目录：probe 缓存的落点，由环境变量传入（Configure 只承载
    // 工具与子选项，不承载路径）。
    std::string workDir_;
    ipc::UdsServer server_{};
    // 权威报告的聚合缓冲。Finalize 一次性写入已渲染的完整 bundle 后取走发出。
    diagnostic::ReportBuffer report_{};
    // callback 线程即时标准化的拥有型记录；Finalize 渲染它们以及工具级汇总。
    std::vector<npucheck::ReportRecord> reportRecords_;
    AclsanSubscriberHandle subscriber_ = nullptr;
    std::vector<std::unique_ptr<Checker>> checkers_;
    std::atomic<uint64_t> callbackCount_{0};
    uint64_t malformedCallbacks_ = 0;
    uint64_t frameworkErrors_ = 0;
};

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_TOOL_MANAGER_TOOL_MANAGER_H
