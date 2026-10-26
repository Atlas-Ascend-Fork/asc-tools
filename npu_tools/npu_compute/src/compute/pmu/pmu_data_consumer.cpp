/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "pmu/pmu_data_consumer.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace npucompute {
namespace {

constexpr std::size_t kQueueCapacity = 1024;

enum class ConsumerState { Created, Running, Stopping, Stopped };

} // namespace

class PmuDataConsumer::Impl {
public:
    explicit Impl(Processor processor) : processor_(std::move(processor)) {}

    ~Impl() { ShutdownAndDrain(); }

    aclptiResult Start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ConsumerState::Running) {
            ASCTOOL_DEBUG("[npu-compute] PMU consumer start skipped: already running");
            return ACLPTI_SUCCESS;
        }
        if (state_ != ConsumerState::Created || !processor_) {
            ASCTOOL_ERROR(
                "[npu-compute] PMU consumer start rejected"
                " code=%u",
                NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return ACLPTI_ERROR_INVALID_STATE;
        }
        state_ = ConsumerState::Running;
        try {
            worker_ = std::thread(&Impl::ConsumeLoop, this);
        } catch (...) {
            state_ = ConsumerState::Stopped;
            ASCTOOL_ERROR(
                "[npu-compute] PMU consumer start failed: worker thread"
                " code=%u",
                NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return ACLPTI_ERROR_INTERNAL;
        }
        ASCTOOL_DEBUG("[npu-compute] PMU consumer started");
        return ACLPTI_SUCCESS;
    }

    aclptiResult Submit(std::shared_ptr<const aclptiProfilingDataResult> result)
    {
        if (!result) {
            ASCTOOL_ERROR(
                "[npu-compute] PMU consumer submit rejected: null result"
                " code=%u",
                NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return ACLPTI_ERROR_INVALID_PARAMETER;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ConsumerState::Running) {
            ASCTOOL_ERROR(
                "[npu-compute] PMU consumer submit rejected: not running"
                " code=%u",
                NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return ACLPTI_ERROR_INVALID_STATE;
        }
        if (queue_.size() == kQueueCapacity) {
            if (status_ == ACLPTI_SUCCESS) {
                status_ = ACLPTI_ERROR_QUEUE_FULL;
            }
            ASCTOOL_ERROR(
                "[npu-compute] PMU consumer submit rejected: queue full"
                " code=%u",
                NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return ACLPTI_ERROR_QUEUE_FULL;
        }
        queue_.push_back(std::move(result));
        ASCTOOL_DEBUG("[npu-compute] PMU consumer submit accepted: queue=%zu", queue_.size());
        readable_.notify_one();
        return ACLPTI_SUCCESS;
    }

    aclptiResult ShutdownAndDrain()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == ConsumerState::Created) {
                state_ = ConsumerState::Stopped;
                ASCTOOL_DEBUG("[npu-compute] PMU consumer shutdown skipped: never started");
                return ACLPTI_SUCCESS;
            }
            if (state_ == ConsumerState::Stopped) {
                ASCTOOL_DEBUG(
                    "[npu-compute] PMU consumer shutdown skipped: stopped status=%d", static_cast<int>(status_));
                return status_;
            }
            state_ = ConsumerState::Stopping;
            ASCTOOL_DEBUG("[npu-compute] PMU consumer shutdown requested: queue=%zu", queue_.size());
            readable_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
            ASCTOOL_DEBUG("[npu-compute] PMU consumer worker joined");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ConsumerState::Stopped;
        ASCTOOL_DEBUG("[npu-compute] PMU consumer shutdown complete status=%d", static_cast<int>(status_));
        return status_;
    }

private:
    void ConsumeLoop()
    {
        ASCTOOL_DEBUG("[npu-compute] PMU consumer thread started");
        while (true) {
            std::shared_ptr<const aclptiProfilingDataResult> result;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                readable_.wait(lock, [this] { return state_ != ConsumerState::Running || !queue_.empty(); });
                if (queue_.empty()) {
                    ASCTOOL_DEBUG("[npu-compute] PMU consumer thread stopped");
                    return;
                }
                result = std::move(queue_.front());
                queue_.pop_front();
                ASCTOOL_DEBUG("[npu-compute] PMU consumer processing item: queue=%zu", queue_.size());
            }

            aclptiResult status = ACLPTI_ERROR_INTERNAL;
            try {
                status = processor_(std::move(result));
            } catch (...) {
                ASCTOOL_ERROR(
                    "[npu-compute] PMU consumer processor threw"
                    " code=%u",
                    NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            }
            ASCTOOL_DEBUG("[npu-compute] PMU consumer processor status=%d", static_cast<int>(status));
            if (status != ACLPTI_SUCCESS) {
                ASCTOOL_ERROR(
                    "[npu-compute] PMU processing failed: status=%d"
                    " code=%u",
                    static_cast<int>(status), static_cast<unsigned>(static_cast<uint32_t>(status)));
                std::lock_guard<std::mutex> lock(mutex_);
                if (status_ == ACLPTI_SUCCESS) {
                    status_ = status;
                }
            }
        }
    }

    Processor processor_;
    std::mutex mutex_;
    std::condition_variable readable_;
    std::deque<std::shared_ptr<const aclptiProfilingDataResult>> queue_;
    ConsumerState state_ = ConsumerState::Created;
    aclptiResult status_ = ACLPTI_SUCCESS;
    std::thread worker_;
};

std::shared_ptr<PmuDataConsumer> PmuDataConsumer::Create(Processor processor)
{
    if (!processor) {
        return nullptr;
    }
    return std::shared_ptr<PmuDataConsumer>(new PmuDataConsumer(std::move(processor)));
}

PmuDataConsumer::PmuDataConsumer(Processor processor) : impl_(std::make_unique<Impl>(std::move(processor))) {}

PmuDataConsumer::~PmuDataConsumer() = default;

aclptiResult PmuDataConsumer::Start() { return impl_->Start(); }

aclptiResult PmuDataConsumer::Submit(std::shared_ptr<const aclptiProfilingDataResult> result)
{
    return impl_->Submit(std::move(result));
}

aclptiResult PmuDataConsumer::ShutdownAndDrain() { return impl_->ShutdownAndDrain(); }

} // namespace npucompute
