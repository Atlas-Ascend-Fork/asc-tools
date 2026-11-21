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
#include "checker/memory_analysis.h"
#include "acl_san/aclsan_cbdata_common.h"
#include "aclsan_simt_ub_access.h"

#include "diagnostic/report/report_assembler.h"

#include <algorithm>
#include <exception>
#include <cstdlib>
#include "npu_tool_log.h"
#include <memory>
#include <sstream>
#include "acl/acl_rt.h"

namespace npucheck {
namespace {

constexpr size_t MAX_DEFERRED_SYNCHRONIZATIONS = 65536;
constexpr size_t MAX_DEFERRED_OPERATORS = 65536;

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

    {
        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        stopping_ = false;
        callbacksClosed_ = false;
    }
    std::string error;
    if (!server_.StartAndHandshake(configure_, error)) {
        LogHandshakeFailure(error);
        server_.Shutdown();
        return 1;
    }
    const char* workDir = std::getenv(npucheck::ipc::WORK_DIR_ENV);
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
            npucheck::ipc::ErrorDomain::CONFIGURATION, npucheck::ipc::error_code::TOOL_INITIALIZATION_FAILED, error);
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
    memoryAnalysis_ = std::make_shared<MemoryAnalysis>(true);
    for (const auto& tool : configure_.tools) {
        auto checker = CreateChecker(tool.toolId, memoryAnalysis_);
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
    auto required = RequiredCallbacks(checkers_);
    if (configure_.opExecuteTimeoutSec.has_value()) {
        required.emplace_back(ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL_BEFORE);
    }

    for (const auto& [domain, cbid] : required) {
        const AclsanStatus status = aclsanEnableCallback(1, subscriber_, domain, cbid);
        if (status != ACLSAN_STATUS_SUCCESS) {
            // 旧注入库没有完成事件时保留兼容，但不能将未完成的 launch 标为完整。
            const bool optionalBoundary =
                domain == ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION && cbid == ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE;
            if (optionalBoundary) {
                ASCTOOL_WARNING(
                    "acl_san operator completion callback unavailable; pending launches will remain incomplete: "
                    "domain=%u cbid=%u status=%u",
                    static_cast<unsigned>(domain), static_cast<unsigned>(cbid), static_cast<unsigned>(status));
                continue;
            }
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
        callbacksClosed_ = true;
    }
    checkers_.clear();
    memoryAnalysis_.reset();
}

void ToolManager::FinalizeCheckers()
{
    CheckerReportList finalReports;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        DrainSynchronizations(finalReports);
        DrainOperatorExecutions(finalReports);
        if (!deferredOperators_.empty()) {
            ++frameworkErrors_;
            ASCTOOL_ERROR("operator completion dependencies unresolved: count=%zu", deferredOperators_.size());
            deferredOperators_.clear();
        }
        for (auto& checker : checkers_) {
            try {
                checker->Finalize(finalReports);
            } catch (...) {
                ++frameworkErrors_;
            }
        }
    }
    StoreReports(std::move(finalReports));
}

bool ToolManager::RenderSessionReport()
{
    std::vector<npucheck::ReportRecord> reportRecords;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        reportRecords.swap(reportRecords_);
    }
    std::string renderedReport;
    const auto renderStatus = npucheck::RenderReportBundle(reportRecords, {}, &renderedReport);
    if (renderStatus != npucheck::ReportRenderStatus::SUCCESS) {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            ++frameworkErrors_;
        }
        ASCTOOL_ERROR("failed to render the session report bundle status=%d", static_cast<int>(renderStatus));
        return false;
    }
    if (!report_.Append(renderedReport) && !report_.Truncated()) {
        ASCTOOL_ERROR("failed to record the rendered session report bundle");
    }
    return true;
}

void ToolManager::Finalize()
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (!initialized_ && !subscribed_) {
        return;
    }
    AclsanStatus unsubscribeStatus = ACLSAN_STATUS_SUCCESS;
    if (subscribed_) {
        unsubscribeStatus = aclsanUnsubscribe(subscriber_);
        subscribed_ = false;
        subscriber_ = nullptr;
    }
    {
        std::unique_lock<std::mutex> callbackLock(callbackMutex_);
        stopping_ = true;
        callbacksDrained_.wait(callbackLock, [this] { return activeCallbacks_ == 0; });
        callbacksClosed_ = true;
    }

    FinalizeCheckers();

    // 当前不再发布实时诊断；停掉 publisher 线程后，Result 独占后续线路，且
    // dropped_messages 才是最终值。
    server_.StopPublisher();
    const bool reportBundleAvailable = RenderSessionReport();

    const std::string summary = BuildAndLogSummaryMessage();
    // 多工具时取"全部工具都分析完整"，任一工具留有在途或被丢弃的事件，整份报告就
    // 不能声称完整 —— 这里必须是与，不是二选一。
    bool analysisComplete = true;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        analysisComplete = std::all_of(
            checkers_.begin(), checkers_.end(), [](const auto& checker) { return checker->AnalysisComplete(); });
        analysisComplete =
            analysisComplete && memoryAnalysis_->Complete() && malformedCallbacks_ == 0 && frameworkErrors_ == 0;
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
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        for (const auto& reason : frameworkErrorReasons_) {
            trailer << "framework error: " << reason << '\n';
        }
    }
    (void)report_.Append(trailer.str());

    const bool hasErrors = HasDetectedErrors();
    if (!reportBundleAvailable || report_.Failed()) {
        // 报告本身没能拼出来，此时宁可什么都不给，也不能把残缺的正文当成结论发出去。
        server_.SendError(
            npucheck::ipc::ErrorDomain::INTERNAL, npucheck::ipc::error_code::REPORT_UNAVAILABLE,
            "npu_check cannot produce the session report");
    } else {
        const std::string reportText = report_.Take();
        std::string sendError;
        if (!server_.SendResult(reportText, hasErrors, truncated, sendError, analysisComplete)) {
            ASCTOOL_ERROR("failed to deliver the session report: %s", sendError.c_str());
        }
    }
    server_.Shutdown();
    ASCTOOL_INFO(
        "status=%s aclsan_unsubscribe=%d dropped_messages=%llu analysis_complete=%s report_truncated=%s",
        completionStatus, static_cast<int>(unsubscribeStatus), static_cast<unsigned long long>(droppedMessages),
        analysisComplete ? "true" : "false", truncated ? "true" : "false");
    checkers_.clear();
    memoryAnalysis_.reset();
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
    if (!service->EnterCallback(domain)) {
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

bool ToolManager::EnterCallback(AclsanCallbackDomain domain)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    // Unsubscribe may deliver a final framework ERROR after normal callbacks stop.
    if (callbacksClosed_ || (stopping_ && domain != ACLSAN_CB_DOMAIN_ERROR)) {
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

void ToolManager::QueueSynchronization(const void* cbdata) noexcept
{
    try {
        if (cbdata == nullptr) {
            synchronizationLost_.store(true);
            return;
        }
        const auto& common = *static_cast<const AclsanCallbackCommonData*>(cbdata);
        if (common.version != ACLSAN_API_VERSION || common.size < sizeof(AclsanSynchronizeData)) {
            synchronizationLost_.store(true);
            return;
        }
        auto data = *static_cast<const AclsanSynchronizeData*>(cbdata);
        data.common.apiName = nullptr;
        std::lock_guard<std::mutex> lock(synchronizationMutex_);
        if (synchronizations_.size() >= MAX_DEFERRED_SYNCHRONIZATIONS) {
            synchronizationLost_.store(true);
            return;
        }
        synchronizations_.push_back({data, std::this_thread::get_id()});
    } catch (...) {
        synchronizationLost_.store(true);
    }
}

void ToolManager::DrainSynchronizations(CheckerReportList& reports)
{
    std::vector<DeferredSynchronization> pending;
    {
        std::lock_guard<std::mutex> lock(synchronizationMutex_);
        pending.swap(synchronizations_);
    }
    if (synchronizationLost_.exchange(false)) {
        ++frameworkErrors_;
        ASCTOOL_ERROR("stream synchronization observation was lost");
    }
    for (const auto& sync : pending) {
        if (!memoryAnalysis_->RecordSynchronization(sync.data, sync.thread)) {
            ++malformedCallbacks_;
            continue;
        }
        NotifyCheckersLocked(
            ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END, &sync.data, reports);
    }
}

void ToolManager::DrainOperatorExecutions(CheckerReportList& reports)
{
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (size_t index = 0; index < deferredOperators_.size(); ++index) {
            if (memoryAnalysis_->HasUnfinishedPredecessor(deferredOperators_[index].launchId))
                continue;
            const auto data = deferredOperators_[index];
            deferredOperators_.erase(deferredOperators_.begin() + static_cast<std::ptrdiff_t>(index));
            ProcessCallbackLocked(
                ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE, &data, reports);
            progressed = true;
            break;
        }
    }
}

void ToolManager::ProcessCallbackLocked(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata, CheckerReportList& reports)
{
    if (IsSimtUbMemcheckCallback(domain, cbid, cbdata)) {
        NotifyCheckersLocked(domain, cbid, cbdata, reports);
        return;
    }
    struct EventGuard {
        MemoryAnalysis& analysis;
        ~EventGuard() { analysis.EndCallback(); }
    } eventGuard{*memoryAnalysis_};
    if (!memoryAnalysis_->BeginCallback(domain, cbid, cbdata)) {
        ++malformedCallbacks_;
        return;
    }
    NotifyCheckersLocked(domain, cbid, cbdata, reports);
}

void ToolManager::NotifyCheckersLocked(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata, CheckerReportList& reports)
{
    for (const auto& checker : checkers_) {
        if (!checker->IsSubscribed(domain, cbid))
            continue;
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

bool ToolManager::DeferOperatorCompletionLocked(AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata)
{
    if (domain != ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION || cbid != ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE) {
        return false;
    }
    const auto& common = *static_cast<const AclsanCallbackCommonData*>(cbdata);
    if (common.version != ACLSAN_API_VERSION || common.size < sizeof(AclsanOperatorExecutionData)) {
        return false;
    }
    auto event = *static_cast<const AclsanOperatorExecutionData*>(cbdata);
    if (!memoryAnalysis_->HasUnfinishedPredecessor(event.launchId)) {
        return false;
    }
    const bool duplicate = std::any_of(
        deferredOperators_.begin(), deferredOperators_.end(),
        [launchId = event.launchId](const auto& pending) { return pending.launchId == launchId; });
    if (duplicate) {
        ++malformedCallbacks_;
    } else if (deferredOperators_.size() >= MAX_DEFERRED_OPERATORS) {
        ++frameworkErrors_;
        ASCTOOL_ERROR("operator completion queue capacity exceeded");
    } else {
        event.common.apiName = nullptr;
        deferredOperators_.push_back(event);
    }
    return true;
}

void ToolManager::OnCallback(AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* cbdata)
{
    LogCallback(domain, cbid, cbdata);
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE && cbid == ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END) {
        QueueSynchronization(cbdata);
        return;
    }
    if (cbdata == nullptr) {
        LogMalformed(domain, cbid, "null callback data");
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            ++malformedCallbacks_;
        }
        return;
    }
    if (domain == ACLSAN_CB_DOMAIN_ERROR) {
        const auto& common = *static_cast<const AclsanCallbackCommonData*>(cbdata);
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (common.version != ACLSAN_API_VERSION || common.size < sizeof(common)) {
            ++malformedCallbacks_;
            LogMalformed(domain, cbid, "invalid error callback data");
            return;
        }
        ++frameworkErrors_;
        frameworkErrorReasons_.emplace_back(common.apiName != nullptr ? common.apiName : "unspecified");
        ASCTOOL_ERROR("framework error: %s result=%d", frameworkErrorReasons_.back().c_str(), common.result);
        return;
    }
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH && cbid == ACLSAN_CBID_LAUNCH_KERNEL_BEFORE &&
        configure_.opExecuteTimeoutSec.has_value()) {
        using SetOpExecuteTimeout = aclError (*)(uint32_t);
        aclrtApiFunc original = nullptr;
        aclError status = aclrtApiInjectionGetFunc("aclrtSetOpExecuteTimeOut", &original, nullptr);
        if (status == ACL_SUCCESS) {
            const auto setTimeout = reinterpret_cast<SetOpExecuteTimeout>(reinterpret_cast<void*>(original));
            status = setTimeout != nullptr ? setTimeout(*configure_.opExecuteTimeoutSec) : ACL_ERROR_UNINITIALIZE;
        }
        if (status != ACL_SUCCESS) {
            ASCTOOL_ERROR(
                "operator execution timeout setup failed: timeout_sec=%u result=%d", *configure_.opExecuteTimeoutSec,
                status);
        } else {
            ASCTOOL_DEBUG("operator execution timeout set: timeout_sec=%u", *configure_.opExecuteTimeoutSec);
        }
        return;
    }
    CheckerReportList reports;
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        DrainSynchronizations(reports);
        if (!DeferOperatorCompletionLocked(domain, cbid, cbdata)) {
            ProcessCallbackLocked(domain, cbid, cbdata, reports);
        }
        DrainOperatorExecutions(reports);
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
    if (memoryAnalysis_) {
        output << "memory_resource_scans=" << memoryAnalysis_->ResourceRecordsScanned()
               << " initialization_candidates=" << memoryAnalysis_->InitializationCandidates() << '\n';
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
