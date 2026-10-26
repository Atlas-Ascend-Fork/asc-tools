/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "hardware/hardware_info_collector.h"
#include "npu_compute_log.h"

#include "hardware/hardware_device_api.h"
#include "hardware/hardware_info_host.h"
#include "hardware/hardware_info_json.h"

#include <boost/filesystem.hpp>

#include <condition_variable>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace npucompute {
namespace {

void DefaultDiagnostic(std::string_view message)
{
    ASCTOOL_WARNING("[HardwareInfo] %.*s", static_cast<int>(message.size()), message.data());
}

HardwareInfoDependencies MakeDefaultDependencies()
{
    HardwareInfoDependencies dependencies;
    dependencies.collectHostInfo = [](const boost::filesystem::path& outputDirectory, HostInfo* host,
                                      DiagnosticSink* diagnostics) {
        return CollectHostInfo(outputDirectory, host, diagnostics);
    };
    dependencies.deviceApi = std::make_shared<DynamicHardwareDeviceApi>();
    dependencies.publish = PublishHardwareInfoJsonl;
    dependencies.diagnostics = DefaultDiagnostic;
    return dependencies;
}

} // namespace

class HardwareInfoCollector::Impl {
public:
    explicit Impl(HardwareInfoDependencies dependencies) : dependencies_(std::move(dependencies)) {}

    ~Impl() { Stop(); }

    bool Initialize(const boost::filesystem::path& outputDirectory, std::string* error)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (error != nullptr) {
            error->clear();
        }
        if (state_ != HardwareCollectionState::Created) {
            SetError(error, "HardwareInfoCollector is already initialized");
            return false;
        }
        if (!dependencies_.collectHostInfo || dependencies_.deviceApi == nullptr || !dependencies_.publish) {
            state_ = HardwareCollectionState::Failed;
            SetError(error, "HardwareInfoCollector dependencies are incomplete");
            return false;
        }

        try {
            outputDirectory_ = outputDirectory;
            state_ = HardwareCollectionState::WaitingKernel;
        } catch (const std::exception& exception) {
            state_ = HardwareCollectionState::Failed;
            SetError(error, std::string("initialize HardwareInfo collector failed: ") + exception.what());
            return false;
        } catch (...) {
            state_ = HardwareCollectionState::Failed;
            SetError(error, "initialize HardwareInfo collector failed: unknown exception");
            return false;
        }
        return true;
    }

    void CollectOnKernelLaunch() noexcept
    {
        try {
            {
                std::unique_lock<std::mutex> lock(stateMutex_);
                if (state_ == HardwareCollectionState::Collecting) {
                    stateCondition_.wait(lock, [this] { return state_ != HardwareCollectionState::Collecting; });
                    return;
                }
                if (state_ != HardwareCollectionState::WaitingKernel) {
                    return;
                }
                state_ = HardwareCollectionState::Collecting;
            }

            HardwareCollectionState finalState = HardwareCollectionState::Failed;
            try {
                finalState = Collect() ? HardwareCollectionState::Completed : HardwareCollectionState::Failed;
            } catch (const std::exception& exception) {
                Report(std::string("HardwareInfo collection failed: ") + exception.what());
            } catch (...) {
                Report("HardwareInfo collection failed: unknown exception");
            }
            SetFinalState(finalState);
        } catch (...) {
            SetFinalState(HardwareCollectionState::Failed);
            Report("HardwareInfo collection synchronization failed");
        }
    }

    void Stop() noexcept
    {
        try {
            std::unique_lock<std::mutex> lock(stateMutex_);
            if (state_ == HardwareCollectionState::WaitingKernel) {
                state_ = HardwareCollectionState::NoKernelLaunch;
                lock.unlock();
                stateCondition_.notify_all();
                return;
            }
            stateCondition_.wait(lock, [this] { return state_ != HardwareCollectionState::Collecting; });
        } catch (...) {
            Report("stop HardwareInfo collector failed");
        }
    }

    HardwareCollectionState State() const noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(stateMutex_);
            return state_;
        } catch (...) {
            return HardwareCollectionState::Failed;
        }
    }

private:
    static void SetError(std::string* error, const std::string& message)
    {
        if (error != nullptr) {
            *error = message;
        }
    }

    void Report(std::string_view message) noexcept
    {
        ASCTOOL_ERROR(
            "[HardwareInfo] %.*s"
            " code=%u",
            static_cast<int>(message.size()), message.data(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        try {
            const auto* defaultSink = dependencies_.diagnostics.target<void (*)(std::string_view)>();
            if (dependencies_.diagnostics && (defaultSink == nullptr || *defaultSink != DefaultDiagnostic)) {
                dependencies_.diagnostics(message);
            }
        } catch (...) {
        }
    }

    void SetFinalState(HardwareCollectionState finalState) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (state_ == HardwareCollectionState::Collecting) {
                state_ = finalState;
            }
        } catch (...) {
        }
        stateCondition_.notify_all();
    }

    bool Collect()
    {
        HardwareInfoSnapshot snapshot;
        DiagnosticSink* diagnostics = dependencies_.diagnostics ? &dependencies_.diagnostics : nullptr;
        if (!dependencies_.collectHostInfo(outputDirectory_, &snapshot.host, diagnostics)) {
            Report("Host HardwareInfo collection failed");
            return false;
        }
        if (!CollectDevice0Info(
                *dependencies_.deviceApi, &snapshot.device, &snapshot.cpu, &snapshot.aiCore, &snapshot.memory,
                diagnostics)) {
            Report("Device HardwareInfo collection failed");
            return false;
        }

        std::string jsonl;
        std::string error;
        if (!SerializeHardwareInfoJsonl(snapshot, &jsonl, &error)) {
            Report("HardwareInfo JSONL serialization failed: " + error);
            return false;
        }

        const PublishResult result = dependencies_.publish(outputDirectory_, jsonl, &error);
        if (result == PublishResult::Failed) {
            Report("HardwareInfo publication failed: " + error);
            return false;
        }
        return true;
    }

    HardwareInfoDependencies dependencies_;
    boost::filesystem::path outputDirectory_;
    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    HardwareCollectionState state_ = HardwareCollectionState::Created;
};

HardwareInfoCollector::HardwareInfoCollector() : impl_(std::make_unique<Impl>(MakeDefaultDependencies())) {}

HardwareInfoCollector::HardwareInfoCollector(HardwareInfoDependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(dependencies)))
{}

HardwareInfoCollector::~HardwareInfoCollector() = default;

bool HardwareInfoCollector::Initialize(const boost::filesystem::path& outputDirectory, std::string* error)
{
    return impl_->Initialize(outputDirectory, error);
}

void HardwareInfoCollector::CollectOnKernelLaunch() noexcept { impl_->CollectOnKernelLaunch(); }

bool HardwareInfoCollector::Initialize(
    const boost::filesystem::path& probePath, HardwareInfoPublishFunction publish, std::string* error)
{
    if (State() != HardwareCollectionState::Created) {
        if (error != nullptr) {
            *error = "HardwareInfoCollector is already initialized";
        }
        return false;
    }
    auto dependencies = MakeDefaultDependencies();
    dependencies.publish = std::move(publish);
    impl_ = std::make_unique<Impl>(std::move(dependencies));
    return impl_->Initialize(probePath, error);
}

void HardwareInfoCollector::Stop() noexcept { impl_->Stop(); }

HardwareCollectionState HardwareInfoCollector::State() const noexcept { return impl_->State(); }

} // namespace npucompute
