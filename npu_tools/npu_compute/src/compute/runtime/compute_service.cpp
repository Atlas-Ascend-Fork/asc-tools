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
#include "runtime/npu_compute_runtime.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
namespace npucompute {
ComputeService& ComputeService::Instance()
{
    static ComputeService service;
    return service;
}
int ComputeService::Initialize() noexcept
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (attempted_) {
        return initialized_ && !finalized_ ? 0 : -1;
    }
    attempted_ = true;
    UdsServer server;
    try {
        config_ = server.Accept().config;
        runtime_config_ = {config_.sections, config_.pmuLevel == "block" ? PmuDataLevel::Block : PmuDataLevel::Task};
        // Initialization cannot produce artifacts before the first launch; start worker after READY.
        // A deferred sink bridges this brief initialization interval.
        class DeferredSink final : public ArtifactSink {
        public:
            explicit DeferredSink(std::unique_ptr<ArtifactPublisher>& publisher) : publisher_(publisher) {}
            void Begin(const std::string& name) override { Get().Begin(name); }
            void Write(std::string_view bytes) override { Get().Write(bytes); }
            void Commit(uint64_t records) override { Get().Commit(records); }
            void Abort() noexcept override
            {
                if (publisher_) {
                    publisher_->Abort();
                }
            }

        private:
            ArtifactPublisher& Get()
            {
                if (!publisher_) {
                    throw std::runtime_error("artifact before READY");
                }
                return *publisher_;
            }
            std::unique_ptr<ArtifactPublisher>& publisher_;
        };
        // Keep sink alive for all callbacks; ownership follows service lifetime.
        static DeferredSink sink(publisher_);
        auto& runtime = NpuComputeRuntime::Instance();
        if (runtime.Initialize(runtime_config_, sink, [this](int status) { return FinalizeOnce(status); }) != 0) {
            throw std::runtime_error("hook or profiling initialization failed");
        }
        if (std::atexit([] { ComputeService::Instance().Stop(); }) != 0) {
            throw std::runtime_error("register exit handler failed");
        }
        publisher_ = std::make_unique<ArtifactPublisher>(server.TakeChannel(), server.Deadline());
        initialized_ = true;
        result_ = 0;
        return 0;
    } catch (const std::exception& error) {
        const auto* failure = dynamic_cast<const ipc::Exception*>(&error);
        if (failure) {
            server.Fail(failure->Detail());
        } else {
            server.Fail(error.what());
        }
        std::fprintf(stderr, "[npu-compute] UDS initialization failed: %s\n", error.what());
    } catch (...) {
        server.Fail("unexpected initialization failure");
    }
    finalized_ = true;
    lock.unlock();
    NpuComputeRuntime::Instance().Stop();
    return -1;
}
int ComputeService::FinalizeOnce(int status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) {
        return result_;
    }
    finalized_ = true;
    auto& runtime = NpuComputeRuntime::Instance();
    ipc::Result result;
    result.sections = config_.sections;
    result.rowCount = runtime.RowCount();
    result.errorCount = runtime.ErrorCount();
    result.replayCount = runtime.ReplayCount();
    result.degraded = runtime.Degraded();
    const bool pipelineRequested =
        std::find(config_.sections.begin(), config_.sections.end(), "Pipeline") != config_.sections.end();
    result.complete = status == 0 && result.errorCount == 0 && (result.rowCount > 0 || pipelineRequested) &&
                      runtime.PmSamplingComplete() && publisher_ && !publisher_->Cancelled();
    result.status = result.complete ? 0 : 1;
    result.code = result.status;
    result_ = publisher_ && publisher_->Finish(std::move(result)) ? 0 : -1;
    return result_;
}
int ComputeService::Shutdown() noexcept
{
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!initialized_) {
                return -1;
            }
            if (finalized_) {
                return result_;
            }
        }
        // Only ACLPTI's shutdown callback may drain the consumer. A launch or
        // uploader can still be producing data when this entry is called.
        return -1;
    } catch (...) {
        return -1;
    }
}
void ComputeService::Stop() noexcept
{
    NpuComputeRuntime::Instance().Stop();
    // ACLPTI teardown invokes the shutdown callback even when no kernel ran.
}
} // namespace npucompute
