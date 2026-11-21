/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "application_profiler.h"
#include "npu_compute_log.h"
#include "acl_pti/data/pmu_events.h"
#include "acl_pti/data/raw_data_decoder.h"
#include "injection/injection_hook.h"
#include "profiling/prof_api.h"
#include "profiling/prof_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace aclpti::profiling {
namespace {
constexpr uint32_t kComputeCollectionType = 8;
constexpr size_t kMaxQueuedRecords = 16384;
constexpr uint64_t kCommunicationProfSwitch = PROF_OP_TIMESTAMP_MASK | PROF_TASK_TIME_MASK | PROF_TASK_TIME_L1_MASK |
                                              PROF_AICPU_TRACE_MASK | PROF_AICORE_METRICS_MASK;

// One hardware round: OpInfoSummary needs FP/vector activity and GM read/write traffic.
constexpr data::PmuSlots kApplicationSummaryPmuSlots = {
    data::pmu::CUBE_FP_ACTIVE_CYCLES_EVENT,
    data::pmu::VECTOR_ACTIVE_CYCLES_EVENT,
    data::pmu::GM_TO_L1_EVENT,
    data::pmu::L0C_TO_GM_EVENT,
    data::kInvalidPmuEvent,
    data::kInvalidPmuEvent,
    data::kInvalidPmuEvent,
    data::kInvalidPmuEvent,
    data::kInvalidPmuEvent,
    data::kInvalidPmuEvent};

std::atomic<ApplicationProfiler*> activeProfiler{nullptr};

bool IsValidRangeConfig(const aclptiRangeProfilerSetConfigParams* params)
{
    if (params == nullptr || params->numSections > ACLPTI_MAX_NUM_SECTIONS ||
        (params->numSections != 0 && params->sections == nullptr) ||
        (params->numSections == 0 && !params->collectPipeline && !params->collectPcSampling) ||
        (params->blockResult != ACLPTI_BLOCK_RESULT_DISABLED && params->blockResult != ACLPTI_BLOCK_RESULT_ALL &&
         params->blockResult != ACLPTI_BLOCK_RESULT_SHRINK) ||
        (params->cacheControl != ACLPTI_CACHE_CONTROL_NONE && params->cacheControl != ACLPTI_CACHE_CONTROL_ALL)) {
        return false;
    }
    for (size_t index = 0; index < params->numSections; ++index) {
        const char* name = params->sections[index];
        if (name == nullptr) {
            return false;
        }
        const size_t length = ::strnlen(name, ACLPTI_MAX_SECTION_NAME_LENGTH + 1);
        if (length == 0 || length > ACLPTI_MAX_SECTION_NAME_LENGTH) {
            return false;
        }
    }
    return true;
}

aclptiResult ValidateCommunicationConfig(const aclptiRangeProfilerSetConfigParams* params)
{
    if (!IsValidRangeConfig(params)) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    if (params->numSections != 1 || std::strcmp(params->sections[0], "Communication") != 0 ||
        params->blockResult != ACLPTI_BLOCK_RESULT_ALL || params->cacheControl != ACLPTI_CACHE_CONTROL_NONE ||
        params->collectPipeline || params->collectPcSampling) {
        return ACLPTI_ERROR_NOT_SUPPORTED;
    }
    return ACLPTI_SUCCESS;
}

bool IsCollectionComplete(const ApplicationSummary& summary)
{
    return summary.droppedRecords == 0 && summary.invalidRecords == 0 && summary.processingErrors == 0 &&
           std::all_of(summary.devices.begin(), summary.devices.end(), [](const auto& item) {
               const auto& device = item.second;
               return device.startAttempted && device.startStatus == 0 &&
                      device.state == ApplicationDeviceState::Inactive &&
                      (!device.stopAttempted || device.stopStatus == 0);
           });
}

MsprofConfig MakeDeviceConfig(int32_t deviceId)
{
    MsprofConfig result{};
    result.profSwitch = kCommunicationProfSwitch;
    // Runtime may inspect the configuration while delivering callbacks; immutable attrs outlive all sessions.
    static const std::array<MsprofConfigAttr, 2> attrs = [] {
        std::array<MsprofConfigAttr, 2> value{};
        value[0].id = PROF_CONFIG_ATTR_AICORE_METRICS;
        std::copy(kApplicationSummaryPmuSlots.begin(), kApplicationSummaryPmuSlots.end(), value[0].value.aicoreMetrics);
        value[1].id = PROF_CONFIG_ATTR_TASK_BLOCK;
        value[1].value.taskBlockMode = PROF_COMPUTE_ALL_BLOCK;
        return value;
    }();
    result.configInfo.numAttrs = attrs.size();
    result.configInfo.attrs = attrs.data();
    result.devNums = 1;
    result.devIdList[0] = static_cast<uint32_t>(deviceId);
    return result;
}

} // namespace

struct ApplicationProfiler::Impl {
    enum class State { Created, Prepared, Closing, Stopped };
    enum class ConfigurationState { Unset, Ready, Frozen };
    struct CallbackRoute {
        std::mutex mutex;
        Impl* target = nullptr;
    };

    static CallbackRoute& Route()
    {
        static_assert(std::is_trivially_destructible_v<CallbackRoute>);
        static CallbackRoute route;
        return route;
    }

    static int32_t ReceiveThunk(MsprofRawData* raw) noexcept
    {
        try {
            auto& route = Route();
            std::lock_guard<std::mutex> lock(route.mutex);
            return route.target != nullptr ? route.target->Receive(raw) : ACLPTI_ERROR_NOT_INITIALIZED;
        } catch (...) {
            return ACLPTI_ERROR_INTERNAL;
        }
    }

    int32_t Receive(const MsprofRawData* raw)
    {
        std::lock_guard<std::mutex> lock(receiveMutex);
        if (!accepting) {
            return ACLPTI_ERROR_INVALID_STATE;
        }
        if (raw == nullptr || raw->chunkSize == 0 || raw->chunkSize > RAW_DATA_MAXSIZE ||
            raw->offset > std::numeric_limits<size_t>::max() - raw->chunkSize) {
            ++summary.invalidRecords;
            return ACLPTI_ERROR_INVALID_RAW_DATA;
        }
        ++summary.receivedRecords;
        if (queue.size() >= kMaxQueuedRecords) {
            ++summary.droppedRecords;
            return ACLPTI_ERROR_QUEUE_FULL;
        }
        ApplicationRawRecord record;
        record.deviceId = raw->deviceId;
        record.type = static_cast<int32_t>(raw->type);
        record.chunkModule = raw->chunkModule;
        record.offset = raw->offset;
        record.isLastChunk = raw->isLastChunk;
        const auto device = receiveEpochs.find(raw->deviceId);
        record.deviceEpoch = device == receiveEpochs.end() ? 0 : device->second;
        try {
            record.bytes.assign(raw->chunk, raw->chunk + raw->chunkSize);
            queue.push_back(std::move(record));
        } catch (...) {
            ++summary.droppedRecords;
            return ACLPTI_ERROR_OUT_OF_MEMORY;
        }
        ready.notify_one();
        return ACLPTI_SUCCESS;
    }

    void Decode(ApplicationRawRecord& record) const
    {
        if (record.type != LOG_DATA_TYPE && record.type != PMU_DATA_TYPE) {
            return;
        }
        const size_t size = record.type == LOG_DATA_TYPE ? data::TASK_LOG_BYTES : data::PMU_RECORD_BYTES;
        if (record.bytes.size() % size != 0) {
            ++record.decodeErrors;
            return;
        }
        for (size_t offset = 0; offset < record.bytes.size(); offset += size) {
            const auto result = data::DecodeRawRecord(
                reinterpret_cast<const std::byte*>(record.bytes.data() + offset), size, offset / size,
                kApplicationSummaryPmuSlots);
            // Unsupported scheduler records remain in the raw artifact, like unknown raw types.
            if (result.Ok()) {
                record.decoded.push_back(result.Value());
            } else if (record.type != LOG_DATA_TYPE || result.Status() != ACLPTI_ERROR_NOT_SUPPORTED) {
                // Bad framing and errors in supported records still make collection incomplete.
                ++record.decodeErrors;
            }
        }
    }

    void Consume() noexcept
    {
        for (;;) {
            ApplicationRawRecord record;
            {
                std::unique_lock<std::mutex> lock(receiveMutex);
                ready.wait(lock, [this] { return !queue.empty() || drainRequested; });
                if (queue.empty()) {
                    break;
                }
                record = std::move(queue.front());
                queue.pop_front();
            }
            bool failed = false;
            try {
                Decode(record);
                consumer(record);
                failed = record.decodeErrors != 0;
            } catch (...) {
                failed = true;
            }
            std::lock_guard<std::mutex> lock(receiveMutex);
            ++summary.processedRecords;
            if (failed) {
                ++summary.processingErrors;
            }
        }
        ASCTOOL_INFO(
            "[npu-compute diagnostic] application consumer drained: received=%llu processed=%llu "
            "processing_errors=%llu publish=%d tail_confirmed=%d",
            static_cast<unsigned long long>(summary.receivedRecords),
            static_cast<unsigned long long>(summary.processedRecords),
            static_cast<unsigned long long>(summary.processingErrors), publishOnClose, summary.tailConfirmed);
        if (!publishOnClose) {
            return;
        }
        summary.complete = IsCollectionComplete(summary);
        // This notification follows every decoded record in the handoff order.
        try {
            ASCTOOL_INFO("[npu-compute diagnostic] application on_drained begin");
            onDrained(summary);
            ASCTOOL_INFO("[npu-compute diagnostic] application on_drained end");
        } catch (...) {
            ASCTOOL_ERROR("[npu-compute diagnostic] application on_drained threw");
            shutdownStatus = ACLPTI_ERROR_CALLBACK;
        }
    }

    aclptiResult StartDevice(int32_t deviceId);
    aclptiResult StopDevice(int32_t deviceId);
    aclptiResult NotifyDeviceResetFailed(int32_t deviceId) noexcept;

    aclptiResult StopLocked(int32_t deviceId)
    {
        const auto found = devices.find(deviceId);
        if (found == devices.end()) {
            return ACLPTI_SUCCESS;
        }
        auto& device = found->second;
        if (device.state == ApplicationDeviceState::Unknown) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        if (device.state == ApplicationDeviceState::Inactive) {
            return ACLPTI_SUCCESS;
        }
        device.state = ApplicationDeviceState::Stopping;
        device.stopAttempted = true;
        const auto msprof = MakeDeviceConfig(deviceId);
        device.stopStatus = MsprofStop(kComputeCollectionType, &msprof, sizeof(msprof));
        device.state = device.stopStatus == 0 ? ApplicationDeviceState::Inactive : ApplicationDeviceState::Unknown;
        if (device.stopStatus == 0) {
            device.endReason = ApplicationDeviceEndReason::ResetStop;
        }
        return device.stopStatus == 0 ? ACLPTI_SUCCESS : ACLPTI_ERROR_PROFILING_FAILED;
    }

    void CloseLocked(ApplicationEndReason reason)
    {
        if (state == State::Closing || state == State::Stopped) {
            return;
        }
        std::lock_guard<std::mutex> lock(receiveMutex);
        state = State::Closing;
        if (finalized) {
            reason = ApplicationEndReason::AclFinalize;
        }
        for (auto& item : devices) {
            auto& device = item.second;
            if (reason == ApplicationEndReason::AclFinalize && device.state == ApplicationDeviceState::Active) {
                device.state = ApplicationDeviceState::Inactive;
                device.endReason = ApplicationDeviceEndReason::RuntimeFinalize;
            }
            if (device.state != ApplicationDeviceState::Inactive) {
                shutdownStatus = ACLPTI_ERROR_PROFILING_FAILED;
            }
        }
        summary.endReason = reason;
        summary.tailConfirmed = reason == ApplicationEndReason::AclFinalize;
        summary.devices = std::move(devices);
        accepting = false;
        drainRequested = true;
        publishOnClose = reason != ApplicationEndReason::StartupRollback;
        ready.notify_all();
    }

    // Serialize device reference tracking through Set/Reset. Runtime may call user code reentrantly.
    std::recursive_mutex deviceMutex;
    std::map<int32_t, uint64_t> deviceReferences;
    std::mutex controlMutex;
    std::mutex shutdownMutex;
    std::mutex receiveMutex;
    std::condition_variable ready;
    State state = State::Created;
    bool accepting = false;
    bool drainRequested = false;
    bool finalized = false;
    bool publishOnClose = false;
    ConfigurationState configuration = ConfigurationState::Unset;
    ApplicationConsumer consumer;
    ApplicationDrainedCallback onDrained;
    ApplicationDeviceStartCallback beforeDeviceStart = nullptr;
    std::map<int32_t, ApplicationDeviceSummary> devices;
    std::map<int32_t, uint64_t> receiveEpochs;
    std::deque<ApplicationRawRecord> queue;
    ApplicationSummary summary;
    std::thread worker;
    std::thread::id workerId;
    aclptiResult shutdownStatus = ACLPTI_SUCCESS;
};

ApplicationProfiler::ApplicationProfiler() : impl_(std::make_unique<Impl>()) {}

ApplicationProfiler::~ApplicationProfiler()
{
    (void)Shutdown();
    auto& route = Impl::Route();
    std::lock_guard<std::mutex> lock(route.mutex);
    if (route.target == impl_.get()) {
        route.target = nullptr;
    }
}

aclptiResult ApplicationProfiler::Initialize(
    ApplicationConsumer consumer, ApplicationDrainedCallback onDrained, UploaderRegistration registerUploader,
    ApplicationDeviceStartCallback beforeDeviceStart)
{
    if (!consumer || !onDrained) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> control(impl_->controlMutex);
    if (impl_->state != Impl::State::Created) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    auto& route = Impl::Route();
    {
        std::lock_guard<std::mutex> lock(route.mutex);
        if (route.target != nullptr && route.target != impl_.get()) {
            return ACLPTI_ERROR_INVALID_STATE;
        }
        route.target = impl_.get();
    }
    impl_->consumer = std::move(consumer);
    impl_->onDrained = std::move(onDrained);
    impl_->beforeDeviceStart = beforeDeviceStart;
    impl_->accepting = true;
    try {
        impl_->worker = std::thread([this] { impl_->Consume(); });
        impl_->workerId = impl_->worker.get_id();
    } catch (...) {
        impl_->accepting = false;
        return ACLPTI_ERROR_INITIALIZATION_FAILED;
    }
    if (registerUploader == nullptr) {
        registerUploader = &acltoolUploaderInit;
    }
    if (registerUploader(&Impl::ReceiveThunk) != 0) {
        {
            std::lock_guard<std::mutex> lock(impl_->receiveMutex);
            impl_->accepting = false;
            impl_->drainRequested = true;
        }
        impl_->ready.notify_all();
        impl_->worker.join();
        impl_->state = Impl::State::Stopped;
        impl_->shutdownStatus = ACLPTI_ERROR_INITIALIZATION_FAILED;
        return impl_->shutdownStatus;
    }
    impl_->state = Impl::State::Prepared;
    return ACLPTI_SUCCESS;
}

aclptiResult ApplicationProfiler::SetConfig(const aclptiRangeProfilerSetConfigParams* params)
{
    const auto status = ValidateCommunicationConfig(params);
    if (status != ACLPTI_SUCCESS) {
        return status;
    }
    std::lock_guard<std::mutex> control(impl_->controlMutex);
    if (impl_->state != Impl::State::Prepared || impl_->configuration == Impl::ConfigurationState::Frozen) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    impl_->configuration = Impl::ConfigurationState::Ready;
    return ACLPTI_SUCCESS;
}

aclptiResult ApplicationProfiler::Impl::StartDevice(int32_t deviceId)
{
    if (deviceId < 0) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> control(controlMutex);
    if (state != Impl::State::Prepared || configuration == ConfigurationState::Unset) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    {
        std::lock_guard<std::mutex> lock(receiveMutex);
        if (!accepting) {
            return ACLPTI_ERROR_INVALID_STATE;
        }
    }
    auto& device = devices[deviceId];
    if (device.state == ApplicationDeviceState::Active) {
        return ACLPTI_SUCCESS;
    }
    if (device.state == ApplicationDeviceState::Unknown) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    device.state = ApplicationDeviceState::Starting;
    device.startAttempted = true;
    configuration = ConfigurationState::Frozen;
    {
        std::lock_guard<std::mutex> lock(receiveMutex);
        // Data arriving inside Start has no confirmed successful epoch yet.
        receiveEpochs[deviceId] = 0;
    }
    const auto msprof = MakeDeviceConfig(deviceId);
    if (beforeDeviceStart != nullptr) {
        beforeDeviceStart(deviceId);
    }
    device.startStatus = MsprofStart(kComputeCollectionType, &msprof, sizeof(msprof));
    device.state = device.startStatus == 0 ? ApplicationDeviceState::Active : ApplicationDeviceState::Unknown;
    if (device.startStatus == 0) {
        ++device.deviceEpoch;
        device.stopAttempted = false;
        device.stopStatus = 0;
        device.endReason = ApplicationDeviceEndReason::Unconfirmed;
        std::lock_guard<std::mutex> lock(receiveMutex);
        // A restarted device can still deliver its previous epoch's tail. Runtime supplies no epoch fence.
        receiveEpochs[deviceId] = device.deviceEpoch == 1 ? device.deviceEpoch : 0;
    }
    return device.startStatus == 0 ? ACLPTI_SUCCESS : ACLPTI_ERROR_PROFILING_FAILED;
}

aclError ApplicationProfiler::SetDevice(aclError (*original)(int32_t), int32_t deviceId)
{
    std::lock_guard<std::recursive_mutex> lock(impl_->deviceMutex);
    const auto result = original(deviceId);
    if (result == ACL_SUCCESS) {
        try {
            auto& references = impl_->deviceReferences[deviceId];
            if (++references == 1) {
                (void)impl_->StartDevice(deviceId);
            }
        } catch (...) {
            ASCTOOL_ERROR(
                "[aclpti] application start failed device=%d code=%u", deviceId, NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        }
    }
    return result;
}

aclError ApplicationProfiler::ResetDevice(aclError (*original)(int32_t), int32_t deviceId, bool force)
{
    std::lock_guard<std::recursive_mutex> lock(impl_->deviceMutex);
    const auto found = impl_->deviceReferences.find(deviceId);
    if (found == impl_->deviceReferences.end()) {
        return original(deviceId);
    }
    const bool lastRelease = force || found->second == 1;
    // Stop before the resetting API releases the device resources.
    bool stopped = false;
    if (lastRelease) {
        try {
            stopped = impl_->StopDevice(deviceId) == ACLPTI_SUCCESS;
        } catch (...) {
            ASCTOOL_ERROR(
                "[aclpti] application stop failed device=%d code=%u", deviceId, NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        }
    }
    const auto result = original(deviceId);
    if (result == ACL_SUCCESS) {
        // A synchronous device-state callback may have changed the reference map.
        const auto current = impl_->deviceReferences.find(deviceId);
        if (current != impl_->deviceReferences.end()) {
            if (force || current->second == 1) {
                impl_->deviceReferences.erase(current);
            } else {
                --current->second;
            }
        }
    } else if (stopped) {
        const auto status = impl_->NotifyDeviceResetFailed(deviceId);
        ASCTOOL_ERROR(
            "[aclpti] application reset failed after stopping collection device=%d tracking_status=%d code=%u",
            deviceId, status, static_cast<unsigned>(result));
    }
    return result;
}

uint64_t ApplicationProfiler::DeviceEpoch(int32_t deviceId) const
{
    std::lock_guard<std::mutex> control(impl_->controlMutex);
    const auto found = impl_->devices.find(deviceId);
    return found != impl_->devices.end() && found->second.state == ApplicationDeviceState::Active ?
               found->second.deviceEpoch :
               0;
}

void ApplicationProfiler::NoteProfilingFinalized() noexcept
{
    {
        std::lock_guard<std::mutex> lock(impl_->receiveMutex);
        if (!impl_->accepting) {
            return;
        }
        impl_->finalized = true;
        impl_->accepting = false;
    }
    impl_->ready.notify_all();
}

aclptiResult ApplicationProfiler::Impl::StopDevice(int32_t deviceId)
{
    if (deviceId < 0) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> control(controlMutex);
    if (state != Impl::State::Prepared) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    return StopLocked(deviceId);
}

aclptiResult ApplicationProfiler::Impl::NotifyDeviceResetFailed(int32_t deviceId) noexcept
{
    try {
        std::lock_guard<std::mutex> control(controlMutex);
        const auto found = devices.find(deviceId);
        if (found != devices.end() && found->second.endReason == ApplicationDeviceEndReason::ResetStop) {
            // A failed reset can leave the device usable after its collection has already stopped.
            found->second.state = ApplicationDeviceState::Unknown;
            found->second.endReason = ApplicationDeviceEndReason::Unconfirmed;
        }
        return ACLPTI_SUCCESS;
    } catch (...) {
        return ACLPTI_ERROR_INTERNAL;
    }
}

aclptiResult ApplicationProfiler::Shutdown(ApplicationEndReason reason) noexcept
{
    try {
        // Consumers must not join their own worker. They may report errors by throwing instead.
        if (std::this_thread::get_id() == impl_->workerId) {
            return ACLPTI_ERROR_INVALID_STATE;
        }
        std::lock_guard<std::mutex> shutdown(impl_->shutdownMutex);
        {
            std::lock_guard<std::mutex> control(impl_->controlMutex);
            if (impl_->state == Impl::State::Stopped || impl_->state == Impl::State::Created) {
                return impl_->shutdownStatus;
            }
            if (impl_->state == Impl::State::Prepared) {
                impl_->CloseLocked(reason);
            }
        }
        if (impl_->worker.joinable()) {
            ASCTOOL_INFO("[npu-compute diagnostic] application worker join begin: reason=%d", static_cast<int>(reason));
            impl_->worker.join();
            ASCTOOL_INFO("[npu-compute diagnostic] application worker join end");
        }
        {
            std::lock_guard<std::mutex> control(impl_->controlMutex);
            impl_->state = Impl::State::Stopped;
        }
        return impl_->shutdownStatus;
    } catch (...) {
        return ACLPTI_ERROR_INTERNAL;
    }
}

ApplicationProfiler& GetApplicationProfiler()
{
    if (auto* profiler = activeProfiler.load(std::memory_order_acquire)) {
        return *profiler;
    }
    static ApplicationProfiler standaloneProfiler;
    return standaloneProfiler;
}

void BindApplicationProfiler(ApplicationProfiler* profiler) noexcept
{
    activeProfiler.store(profiler, std::memory_order_release);
}

} // namespace aclpti::profiling
