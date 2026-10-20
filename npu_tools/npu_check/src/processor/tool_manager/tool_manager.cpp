/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "tool_manager/tool_manager.h"

#include "diagnostic/report/report_assembler.h"

#include <algorithm>
#include <exception>
#include <cstdlib>
#include "npu_tool_log.h"
#include <memory>
#include <sstream>

namespace npucheck {
namespace {

std::string StatusMessage(const char* operation, AclsanStatus status)
{
    std::ostringstream output;
    output << operation << " failed with AclsanStatus=" << static_cast<int>(status);
    return output.str();
}

} // namespace

ToolManager::~ToolManager() noexcept
{
    try {
        Finalize();
    } catch (...) {
        // Destruction runs at process exit and must not cross the C injection boundary.
        return;
    }
}

ToolManager::ActiveCallbackGuard::~ActiveCallbackGuard() noexcept { service_.LeaveCallback(); }

int ToolManager::Initialize()
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (initialized_) {
        return 0;
    }

    std::string error;
    if (!server_.StartAndHandshake(configure_, error)) {
        LogHandshakeFailure(error);
        server_.Shutdown();
        return 1;
    }
    const char* workDir = std::getenv(npucheck::ipc::kWorkDirEnv);
    workDir_ = workDir != nullptr ? workDir : "";
    ASCTOOL_INFO(
        "UDS handshake completed session=%llu negotiated_minor=%u",
        static_cast<unsigned long long>(server_.SessionId()), static_cast<unsigned>(server_.NegotiatedMinor()));
    std::ostringstream configMessage;
    for (const auto& tool : configure_.tools) {
        configMessage << " tool=" << npucheck::ipc::ToolName(tool.toolId) << " option_count=" << tool.options.size();
        for (const auto& option : tool.options) {
            configMessage << " option_id=0x" << std::hex << static_cast<unsigned>(option.optionId) << std::dec;
        }
    }
    ASCTOOL_INFO(
        "tool configuration work_dir=%s tool_count=%zu%s", workDir_.c_str(), configure_.tools.size(),
        configMessage.str().c_str());
    if (!ConfigureSanitizer(error)) {
        ASCTOOL_ERROR("sanitizer initialization failed: %s", error.c_str());
        server_.SendInitializationError(
            npucheck::ipc::ErrorDomain::CONFIGURATION, npucheck::ipc::error_code::kToolInitializationFailed, error);
        RollbackSanitizer();
        server_.Shutdown();
        return 1;
    }
    // Ready 不带 payload，会话细节只写 plog。
    ASCTOOL_INFO(
        "session=%llu api_version=%u tools=%s work_dir=%s", static_cast<unsigned long long>(server_.SessionId()),
        static_cast<unsigned>(ACLSAN_API_VERSION), BuildToolNames().c_str(), workDir_.c_str());
    if (!server_.SendReady(error)) {
        ASCTOOL_ERROR("failed to send UDS ready notification: %s", error.c_str());
        RollbackSanitizer();
        server_.Shutdown();
        return 1;
    }
    initialized_ = true;
    ASCTOOL_INFO("npu_check initialization completed");
    return 0;
}

void ToolManager::LogHandshakeFailure(const std::string& reason) noexcept
{
    try {
        ASCTOOL_ERROR("UDS handshake failed: %s", reason.c_str());
    } catch (...) {
        return;
    }
}

bool ToolManager::ConfigureSanitizer(std::string& error)
{
    if (configure_.tools.empty()) {
        error = "configure enabled no tool";
        return false;
    }
    for (const auto& tool : configure_.tools) {
        auto checker = CreateChecker(tool.toolId);
        if (!checker) {
            error = std::string("unsupported tool '") + npucheck::ipc::ToolName(tool.toolId) + "'";
            return false;
        }
        checkers_.push_back(std::move(checker));
    }
    AclsanStatus status = aclsanSubscribe(&subscriber_, &ToolManager::Callback, this);
    if (status != ACLSAN_STATUS_SUCCESS) {
        error = StatusMessage("aclsanSubscribe", status);
        return false;
    }
    subscribed_ = true;
    ASCTOOL_INFO("sanitizer callback subscriber registered");
    return EnableCallbacks(error);
}

bool ToolManager::EnableCallbacks(std::string& error)
{
    const auto required = RequiredCallbacks(checkers_);

    for (const auto& [domain, cbid] : required) {
        const AclsanStatus status = aclsanEnableCallback(1, subscriber_, domain, cbid);
        if (status != ACLSAN_STATUS_SUCCESS) {
            error = StatusMessage("aclsanEnableCallback", status);
            return false;
        }
        ASCTOOL_DEBUG("callback enabled domain=%u cbid=%u", static_cast<unsigned>(domain), static_cast<unsigned>(cbid));
    }
    return true;
}

void ToolManager::RollbackSanitizer()
{
    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        stopping_ = true;
    }
    if (subscribed_) {
        (void)aclsanUnsubscribe(subscriber_);
        subscribed_ = false;
        subscriber_ = nullptr;
    }
    {
        std::unique_lock<std::mutex> callbackLock(callbackMutex_);
        callbacksDrained_.wait(callbackLock, [this] { return activeCallbacks_ == 0; });
    }
    checkers_.clear();
}

void ToolManager::Finalize()
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (!initialized_ && !subscribed_) {
        return;
    }
    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        stopping_ = true;
    }
    AclsanStatus unsubscribeStatus = ACLSAN_STATUS_SUCCESS;
    if (subscribed_) {
        unsubscribeStatus = aclsanUnsubscribe(subscriber_);
        subscribed_ = false;
        subscriber_ = nullptr;
    }
    {
        std::unique_lock<std::mutex> callbackLock(callbackMutex_);
        callbacksDrained_.wait(callbackLock, [this] { return activeCallbacks_ == 0; });
    }

    CheckerReportList finalReports;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        for (auto& checker : checkers_) {
            try {
                checker->Finalize(finalReports);
            } catch (...) {
                ++frameworkErrors_;
            }
        }
    }
    StoreReports(std::move(finalReports));

    // 当前不再发布实时诊断；停掉 publisher 线程后，Result 独占后续线路，且
    // dropped_messages 才是最终值。
    server_.StopPublisher();

    std::vector<npucheck::ReportRecord> reportRecords;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        reportRecords.swap(reportRecords_);
    }
    std::string renderedReport;
    const auto renderStatus = npucheck::RenderReportBundle(reportRecords, {}, &renderedReport);
    const bool reportBundleAvailable = renderStatus == npucheck::ReportRenderStatus::SUCCESS;
    if (!reportBundleAvailable) {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            ++frameworkErrors_;
        }
        ASCTOOL_ERROR("failed to render the session report bundle status=%d", static_cast<int>(renderStatus));
    } else if (!report_.Append(renderedReport) && !report_.Truncated()) {
        ASCTOOL_ERROR("failed to record the rendered session report bundle");
    }

    const std::string summary = BuildAndLogSummaryMessage();
    // 多工具时取"全部工具都分析完整"，任一工具留有在途或被丢弃的事件，整份报告就
    // 不能声称完整 —— 这里必须是与，不是二选一。
    bool analysisComplete = true;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        analysisComplete = std::all_of(
            checkers_.begin(), checkers_.end(), [](const auto& checker) { return checker->AnalysisComplete(); });
        analysisComplete = analysisComplete && malformedCallbacks_ == 0 && frameworkErrors_ == 0;
    }
    const bool truncated = report_.Truncated();
    std::ostringstream sessionEnd;
    const bool transportComplete = server_.TransportComplete() && server_.DroppedMessages() == 0;
    const char* completionStatus =
        unsubscribeStatus == ACLSAN_STATUS_SUCCESS && transportComplete && analysisComplete ? "complete" : "incomplete";
    const auto droppedMessages = server_.DroppedMessages();
    sessionEnd << "status=" << completionStatus << " aclsan_unsubscribe=" << static_cast<int>(unsubscribeStatus)
               << " dropped_messages=" << droppedMessages
               << " analysis_complete=" << (analysisComplete ? "true" : "false")
               << " report_truncated=" << (truncated ? "true" : "false");

    // 汇总与完整性信息进报告正文，而不是单独的消息类型：CLI 不解析报告内容，能不能
    // 信任这份结论由 Result 末帧的标志位表达，正文只负责让人读懂发生了什么。
    std::ostringstream trailer;
    trailer << "===== npu_check summary =====\n" << summary << '\n' << sessionEnd.str() << '\n';
    (void)report_.Append(trailer.str());

    const bool hasErrors = HasDetectedErrors();
    if (!reportBundleAvailable || report_.Failed()) {
        // 报告本身没能拼出来，此时宁可什么都不给，也不能把残缺的正文当成结论发出去。
        server_.SendError(
            npucheck::ipc::ErrorDomain::INTERNAL, npucheck::ipc::error_code::kReportUnavailable,
            "npu_check cannot produce the session report");
    } else {
        const std::string reportText = report_.Take();
        std::string sendError;
        if (!server_.SendResult(reportText, hasErrors, truncated, sendError)) {
            ASCTOOL_ERROR("failed to deliver the session report: %s", sendError.c_str());
        }
    }
    server_.Shutdown();
    ASCTOOL_INFO(
        "status=%s aclsan_unsubscribe=%d dropped_messages=%llu analysis_complete=%s report_truncated=%s",
        completionStatus, static_cast<int>(unsubscribeStatus), static_cast<unsigned long long>(droppedMessages),
        analysisComplete ? "true" : "false", truncated ? "true" : "false");
    checkers_.clear();
    initialized_ = false;
}

bool ToolManager::HasDetectedErrors() const
{
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    return std::any_of(checkers_.begin(), checkers_.end(), [](const auto& checker) { return checker->HasErrors(); });
}

bool ToolManager::IsInitialized() const
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return initialized_;
}

void ToolManager::Callback(
    void* userdata, AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata) noexcept
{
    auto* service = static_cast<ToolManager*>(userdata);
    if (service == nullptr) {
        return;
    }
    if (!service->EnterCallback()) {
        return;
    }
    ActiveCallbackGuard callbackGuard(*service);
    try {
        service->OnCallback(domain, cbid, cbdata);
    } catch (const std::exception& exception) {
        service->OnCallbackException(exception.what());
    } catch (...) {
        service->OnCallbackException("unknown exception");
    }
}

bool ToolManager::EnterCallback()
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (stopping_) {
        return false;
    }
    ++activeCallbacks_;
    return true;
}

void ToolManager::LeaveCallback()
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (activeCallbacks_ != 0) {
        --activeCallbacks_;
    }
    if (activeCallbacks_ == 0) {
        callbacksDrained_.notify_all();
    }
}

void ToolManager::OnCallback(AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata)
{
    LogCallback(domain, cbid, cbdata);
    if (!cbdata) {
        LogMalformed(domain, cbid, "null callback data");
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            ++malformedCallbacks_;
        }
        return;
    }
    CheckerReportList reports;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        for (const auto& checker : checkers_) {
            if (!checker->IsSubscribed(domain, cbid)) {
                continue;
            }
            try {
                if (!checker->OnCallback(domain, cbid, cbdata, reports))
                    ++malformedCallbacks_;
            } catch (const std::exception& error) {
                ++frameworkErrors_;
                ASCTOOL_ERROR("checker callback failed: %s", error.what());
            } catch (...) {
                ++frameworkErrors_;
                ASCTOOL_ERROR("checker callback failed");
            }
        }
    }
    StoreReports(std::move(reports));
}

void ToolManager::OnCallbackException(const char* reason) noexcept
{
    try {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            ++frameworkErrors_;
        }
        ASCTOOL_ERROR("npu_check callback failed: %s", reason != nullptr ? reason : "unspecified exception");
    } catch (...) {
        // Error reporting is best effort inside a noexcept runtime callback.
        return;
    }
}

void ToolManager::StoreReports(CheckerReportList reports)
{
    for (auto& report : reports) {
        // 与回调串行组装并保存最终报告。
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        auto result = AssembleReport(report);
        if (result.status != ReportRenderStatus::SUCCESS) {
            ++frameworkErrors_;
            ASCTOOL_ERROR(
                "checker report normalization failed report_id=%llu status=%d",
                static_cast<unsigned long long>(result.reportId), static_cast<int>(result.status));
            continue;
        }
        reportRecords_.push_back(std::move(result.record));
    }
}

void ToolManager::LogMalformed(AclsanCallbackDomain domain, AclsanCallbackId cbid, const char* reason)
{
    ASCTOOL_ERROR(
        "[NPU-CHECK-MALFORMED-CALLBACK]count=%llu domain=%u cbid=%u reason=%s",
        static_cast<unsigned long long>(callbackCount_.load()), static_cast<unsigned>(domain),
        static_cast<unsigned>(cbid), reason);
}

void ToolManager::LogCallback(AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata)
{
    const uint64_t count = ++callbackCount_;
    ASCTOOL_DEBUG(
        "cbdata received count=%llu domain=%u cbid=%u address=%p", static_cast<unsigned long long>(count),
        static_cast<unsigned>(domain), static_cast<unsigned>(cbid), cbdata);
}

std::string ToolManager::BuildToolNames() const
{
    std::ostringstream output;
    for (size_t index = 0; index < configure_.tools.size(); ++index) {
        output << (index == 0 ? "" : ",") << npucheck::ipc::ToolName(configure_.tools[index].toolId);
    }
    return output.str();
}

std::string ToolManager::BuildAndLogSummaryMessage() const
{
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    std::ostringstream output;
    for (const auto& checker : checkers_) {
        output << checker->Summary() << '\n';
    }
    const auto callbackCount = callbackCount_.load();
    const auto droppedMessages = server_.DroppedMessages();
    ASCTOOL_INFO(
        "session summary: %scallbacks=%llu malformed_callbacks=%llu framework_errors=%llu dropped_messages=%llu",
        output.str().c_str(), static_cast<unsigned long long>(callbackCount),
        static_cast<unsigned long long>(malformedCallbacks_), static_cast<unsigned long long>(frameworkErrors_),
        static_cast<unsigned long long>(droppedMessages));
    output << "callbacks=" << callbackCount << " malformed_callbacks=" << malformedCallbacks_
           << " framework_errors=" << frameworkErrors_ << " dropped_messages=" << droppedMessages;
    return output.str();
}

} // namespace npucheck
