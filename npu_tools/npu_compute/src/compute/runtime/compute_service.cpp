/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime/compute_service.h"

#include "acl_pti/initialization.h"
#include "ipc/artifact_publisher.h"
#include "ipc/npu_compute_uds_server.h"
#include "npu_compute_log.h"
#include "report/application_report.h"
#include "runtime/application_op_collector.h"
#include "runtime/npu_compute_runtime.h"
#include "runtime/profiling_result_snapshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace npucompute {
namespace {
using aclpti::profiling::ApplicationEndReason;
using aclpti::profiling::ApplicationRawRecord;
using aclpti::profiling::ApplicationSummary;

constexpr std::size_t kApplicationReportQueueCapacity = 16384;
constexpr auto kCancelPollInterval = std::chrono::milliseconds(20);
thread_local bool reportWorker = false;
std::mutex applicationCallbackMutex;
ComputeService* applicationCallbackTarget = nullptr;
bool applicationCallbacksRetired = false;

void ReportStartupFailure(UdsServer& server, ArtifactPublisher* publisher, const std::exception* error) noexcept
{
    try {
        const auto* failure = dynamic_cast<const ipc::Exception*>(error);
        auto detail = failure ? failure->Detail() :
                                ipc::Error{
                                    1, error == nullptr ? "unexpected initialization failure" : error->what(),
                                    "INJECTION", "initialize"};
        if (publisher != nullptr) {
            publisher->FailStartup(std::move(detail));
        } else {
            server.Fail(detail);
        }
    } catch (...) {
        // Rollback also runs if reporting an initialization error fails.
    }
}
} // namespace

struct ComputeService::Session {
    explicit Session(ipc::Config config) : config(std::move(config)) {}

    // ACLPTI owns admission and waits for the last callback before returning from
    // ShutdownProcessorSession. Only a callback admitted by ACLPTI uses this route.
    static std::atomic<Session*> target;

    static void Metadata(
        void*, aclptiCallbackDomain domain, aclptiCallbackId id, const aclptiCallbackData* data) noexcept
    {
        auto* session = target.load(std::memory_order_acquire);
        if (session == nullptr || data == nullptr || domain != ACLPTI_CB_DOMAIN_RUNTIME_API) {
            return;
        }
        if (session->kernel) {
            session->kernel->ObserveRuntime(domain, id, data);
        } else {
            try {
                session->ops->OnCallback(id, *data);
            } catch (...) {
                ASCTOOL_ERROR("[npu-compute] application metadata collection failed");
            }
        }
    }

    static void ApplicationRecord(const ApplicationRawRecord& value)
    {
        auto* session = target.load(std::memory_order_acquire);
        if (session == nullptr) {
            throw std::runtime_error("application report session closed");
        }
        std::unique_lock<std::mutex> lock(session->queueMutex);
        while (session->records.size() >= kApplicationReportQueueCapacity && !session->aborted &&
               !session->reportFailed && !session->publisher.Cancelled()) {
            session->queueChanged.wait_for(lock, kCancelPollInterval);
        }
        if (session->aborted || session->summary || session->reportFailed || session->publisher.Cancelled()) {
            throw std::runtime_error("application report queue unavailable");
        }
        session->records.push_back(value);
        lock.unlock();
        session->queueChanged.notify_one();
    }

    static void ApplicationEnded(const ApplicationSummary& value)
    {
        if (auto* session = target.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(session->queueMutex);
                if (!session->aborted && !session->summary) {
                    session->summary = value;
                }
            }
            session->queueChanged.notify_all();
        }
    }

    static void KernelResult(const aclptiProfilingDataResult& value)
    {
        auto* session = target.load(std::memory_order_acquire);
        if (session == nullptr) {
            return;
        }
        try {
            auto snapshot = std::make_shared<const aclptiProfilingDataResult>(SnapshotProfilingResult(value));
            std::lock_guard<std::mutex> lock(session->kernelMutex);
            session->kernelResult = std::move(snapshot);
        } catch (...) {
            std::lock_guard<std::mutex> lock(session->kernelMutex);
            session->kernelResultFailed = true;
        }
    }

    static void KernelEnded(aclptiResult status)
    {
        if (auto* session = target.load(std::memory_order_acquire)) {
            session->FinishKernel(status);
            (void)session->publisher.WaitForFinish();
        }
    }

    void Prepare(bool fromAcl)
    {
        if (config.replayMode == "application") {
            SectionConfig sections;
            std::string error;
            if (!sections.Load(config.sections, &error)) {
                throw std::runtime_error(error);
            }
            app = std::make_unique<ApplicationReport>(publisher);
            ops = std::make_unique<ApplicationOpCollector>(aclpti::initialization::ApplicationDeviceEpoch);
            report = std::thread([this] { RunApplicationReport(); });
            if (aclpti::initialization::PrepareApplicationSession(&ApplicationRecord, &ApplicationEnded, !fromAcl) !=
                ACLPTI_SUCCESS) {
                throw std::runtime_error("application profiling initialization failed");
            }
            aclPrepared = true;
            auto* params = sections.Params();
            params->cacheControl = CacheMode();
            if (aclptiRangeProfilerSetConfig(params) != ACLPTI_SUCCESS || !ops->Initialize(&Metadata)) {
                throw std::runtime_error("application profiling configuration failed");
            }
        } else {
            kernel = std::make_unique<NpuComputeRuntime>();
            if (aclpti::initialization::PrepareKernelSession(&KernelResult, &KernelEnded) != ACLPTI_SUCCESS) {
                throw std::runtime_error("kernel profiling initialization failed");
            }
            aclPrepared = true;
            if (kernel->Initialize({config.sections, CacheMode()}, publisher, &Metadata) != ACLPTI_SUCCESS) {
                throw std::runtime_error("kernel profiling configuration failed");
            }
        }
        target.store(this, std::memory_order_release);
    }

    auto CacheMode() const -> decltype(RuntimeConfig::cacheControl)
    {
        return config.cacheControlMode == ipc::kCacheControlModeAll ? ACLPTI_CACHE_CONTROL_ALL :
                                                                      ACLPTI_CACHE_CONTROL_NONE;
    }

    int Close(ApplicationEndReason reason, bool ready) noexcept
    {
        if (!ready) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                aborted = true;
            }
            queueChanged.notify_all();
        }
        aclptiResult aclStatus = ACLPTI_SUCCESS;
        if (aclPrepared) {
            aclStatus = aclpti::initialization::ShutdownProcessorSession(reason);
        }
        target.store(nullptr, std::memory_order_release);
        if (app) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!summary) {
                    summary.emplace().endReason = reason;
                }
            }
            queueChanged.notify_all();
            if (report.joinable()) {
                report.join();
            }
            ASCTOOL_INFO("[npu-compute] application report worker joined");
        }
        if (kernel) {
            kernel->Stop();
            if (ready) {
                FinishKernel(aclStatus);
            } else {
                kernel->Rollback();
            }
            ASCTOOL_INFO("[npu-compute] PMU report worker joined");
        }
        if (!ready) {
            return -1;
        }
        const bool delivered = publisher.WaitForFinish();
        ASCTOOL_INFO("[npu-compute] publisher worker joined");
        return delivered ? 0 : -1;
    }

    void RunApplicationReport() noexcept
    {
        reportWorker = true;
        try {
            ApplicationSummary finalSummary;
            uint64_t reportErrors = 0;
            for (;;) {
                ApplicationRawRecord record;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    queueChanged.wait(lock, [this] { return aborted || summary || !records.empty(); });
                    if (aborted) {
                        return;
                    }
                    if (records.empty()) {
                        finalSummary = std::move(*summary);
                        break;
                    }
                    record = std::move(records.front());
                    records.pop_front();
                }
                queueChanged.notify_all();
                try {
                    app->Consume(record);
                } catch (...) {
                    if (record.decodeErrors == 0) {
                        ++reportErrors;
                    }
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        reportFailed = true;
                    }
                    queueChanged.notify_all();
                }
            }
            finalSummary.processingErrors += reportErrors;
            finalSummary.complete = finalSummary.complete && finalSummary.processingErrors == 0;
            FinishApplication(finalSummary);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                reportFailed = true;
            }
            queueChanged.notify_all();
            publisher.Stop();
            ASCTOOL_ERROR("[npu-compute] application reporting failed");
        }
    }

    void FinishApplication(const ApplicationSummary& value)
    {
        if (!value.tailConfirmed && value.endReason != ApplicationEndReason::FinalizeWhileBusy) {
            std::fputs(
                "[WARNING] npu-compute: application profiling tail was not confirmed; "
                "aclFinalize() may be missing or may not have completed before collection closed.\n",
                stderr);
        }
        ipc::Result result;
        result.sections = config.sections;
        result.rowCount = value.processedRecords;
        result.droppedCount = value.droppedRecords;
        result.errorCount = value.invalidRecords + value.processingErrors;
        for (const auto& entry : value.devices) {
            const auto& device = entry.second;
            if (device.state != aclpti::profiling::ApplicationDeviceState::Inactive || device.startStatus != 0 ||
                (device.stopAttempted && device.stopStatus != 0)) {
                ++result.errorCount;
            }
        }
        try {
            app->Finish(value, ops->Snapshot());
        } catch (...) {
            ++result.errorCount;
            ASCTOOL_ERROR("[npu-compute] application artifact delivery failed");
        }
        result.complete = value.complete && value.tailConfirmed && result.errorCount == 0 && !publisher.Cancelled();
        result.status = result.complete ? 0 : 1;
        result.code = result.status;
        publisher.RequestFinish(std::move(result));
    }

    void FinishKernel(aclptiResult status) noexcept
    {
        std::shared_ptr<const aclptiProfilingDataResult> snapshot;
        bool snapshotFailed = false;
        {
            std::unique_lock<std::mutex> lock(kernelMutex);
            if (kernelFinishing) {
                kernelChanged.wait(lock, [this] { return kernelFinished; });
                return;
            }
            kernelFinishing = true;
            snapshot = std::move(kernelResult);
            snapshotFailed = kernelResultFailed;
        }
        try {
            const bool hasResult = snapshot != nullptr;
            if (snapshot && kernel->Submit(std::move(snapshot)) != ACLPTI_SUCCESS) {
                snapshotFailed = true;
            }
            const int reportStatus = kernel->ShutdownAfterPtiDrain();
            ipc::Result result;
            result.sections = config.sections;
            result.rowCount = kernel->RowCount();
            result.errorCount = kernel->ErrorCount() + static_cast<uint64_t>(snapshotFailed);
            result.replayCount = kernel->ReplayCount();
            result.degraded = kernel->Degraded();
            const auto requested = [this](const char* section) {
                return std::find(config.sections.begin(), config.sections.end(), section) != config.sections.end();
            };
            result.complete = hasResult && status == ACLPTI_SUCCESS && reportStatus == 0 && result.errorCount == 0 &&
                              (result.rowCount > 0 || requested("Pipeline") || requested("PmSampling") ||
                               requested("SourceCounter")) &&
                              kernel->PmSamplingComplete() && !publisher.Cancelled();
            result.status = result.complete ? 0 : 1;
            result.code = result.status;
            publisher.RequestFinish(std::move(result));
        } catch (...) {
            kernel->Rollback();
            publisher.Stop();
            ASCTOOL_ERROR("[npu-compute] kernel reporting failed");
        }
        {
            std::lock_guard<std::mutex> lock(kernelMutex);
            kernelFinished = true;
        }
        kernelChanged.notify_all();
    }

    const ipc::Config config;
    ArtifactPublisher publisher;
    std::unique_ptr<ApplicationReport> app;
    std::unique_ptr<ApplicationOpCollector> ops;
    std::unique_ptr<NpuComputeRuntime> kernel;
    bool aclPrepared = false;

    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque<ApplicationRawRecord> records;
    std::optional<ApplicationSummary> summary;
    bool aborted = false;
    bool reportFailed = false;
    std::thread report;

    std::mutex kernelMutex;
    std::condition_variable kernelChanged;
    std::shared_ptr<const aclptiProfilingDataResult> kernelResult;
    bool kernelResultFailed = false;
    bool kernelFinishing = false;
    bool kernelFinished = false;
};

std::atomic<ComputeService::Session*> ComputeService::Session::target{nullptr};

ComputeService& ComputeService::Instance()
{
    static ComputeService service;
    return service;
}

ComputeService::~ComputeService()
{
    {
        std::lock_guard<std::mutex> lock(applicationCallbackMutex);
        if (applicationCallbackTarget == this) {
            applicationCallbackTarget = nullptr;
            applicationCallbacksRetired = true;
        }
    }
    (void)ShutdownAndWait(ApplicationEndReason::ProcessExit);
}

void ComputeService::InitializeFromAcl() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(applicationCallbackMutex);
        if (applicationCallbacksRetired) {
            return;
        }
        auto& service = Instance();
        applicationCallbackTarget = &service;
        (void)service.Initialize(true);
    } catch (...) {
        ASCTOOL_ERROR("[npu-compute] could not start Application session from ACL callback");
    }
}

void ComputeService::FinalizeFromAcl() noexcept
{
    // Keep the callback target alive through the local close. The destructor retires
    // the target before starting its own close and releases this mutex before joining.
    std::lock_guard<std::mutex> lock(applicationCallbackMutex);
    if (applicationCallbackTarget != nullptr) {
        applicationCallbackTarget->FinalizeApplication();
    }
}

int ComputeService::Initialize(bool fromAcl) noexcept
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting; });
        if (state_ != State::Idle) {
            return state_ == State::Running ? 0 : -1;
        }
        state_ = State::Starting;
    }

    UdsServer server;
    bool attached = false;
    try {
        auto config = server.Accept(fromAcl).config;
        if (fromAcl && config.replayMode != "application") {
            throw ipc::Exception({1, "ACL callback requires Application mode", "INJECTION", "initialize"});
        }
        session_ = std::make_unique<Session>(std::move(config));
        session_->Prepare(fromAcl);
        session_->publisher.Attach(server.TakeChannel());
        attached = true;
        session_->publisher.SendReady(server.Deadline());
        aclpti::initialization::ActivateProcessorSession();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::Running;
        }
        changed_.notify_all();
        return 0;
    } catch (const std::exception& error) {
        ReportStartupFailure(server, attached ? &session_->publisher : nullptr, &error);
    } catch (...) {
        ReportStartupFailure(server, attached ? &session_->publisher : nullptr, nullptr);
    }
    if (session_) {
        (void)session_->Close(ApplicationEndReason::StartupRollback, false);
        session_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Stopped;
    }
    changed_.notify_all();
    return -1;
}

int ComputeService::Shutdown() noexcept { return ShutdownAndWait(ApplicationEndReason::LocalShutdown); }

int ComputeService::ShutdownAndWait(ApplicationEndReason reason) noexcept
{
    if (reportWorker || aclpti::initialization::InProcessorCallback()) {
        return -1;
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting && state_ != State::Closing; });
        if (state_ != State::Running) {
            return result_;
        }
        state_ = State::Closing;
        if (finalizeDeferred_) {
            reason = ApplicationEndReason::FinalizeWhileBusy;
        }
    }
    return CloseOwnedSession(reason);
}

void ComputeService::FinalizeApplication() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // ACL holds its lifecycle locks. Another close may be waiting on a Runtime call.
        if (state_ == State::Closing || state_ == State::Starting) {
            ASCTOOL_WARNING("[npu-compute] ACL finalize overlapped with processor startup or shutdown");
            return;
        }
        if (state_ != State::Running || finalizeDeferred_) {
            return;
        }
        if (!aclpti::initialization::CloseApplicationAdmissionForFinalize()) {
            finalizeDeferred_ = true;
            ASCTOOL_WARNING("[npu-compute] ACL finalize overlapped with an admitted Runtime callback");
            return;
        }
        state_ = State::Closing;
    }
    (void)CloseOwnedSession(ApplicationEndReason::AclFinalize);
}

int ComputeService::CloseOwnedSession(ApplicationEndReason reason) noexcept
{
    ASCTOOL_INFO("[npu-compute] processor shutdown begin");
    const int result = session_->Close(reason, true);
    session_.reset();
    ASCTOOL_INFO("[npu-compute] processor session released");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = result;
        state_ = State::Stopped;
    }
    changed_.notify_all();
    return result;
}
} // namespace npucompute
