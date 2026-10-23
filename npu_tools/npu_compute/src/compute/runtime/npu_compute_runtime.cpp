/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime/npu_compute_runtime.h"
#include <vector>

#include "artifact_sink.h"
#include "common/debug_log.h"
#include "hardware/hardware_device_api.h"
#include "report/report_writer.h"
#include "pm_sampling/pm_sampling_decoder.h"
#include "pm_sampling/pm_sampling_sqlite_writer.h"
#include "pm_sampling/pm_sampling_artifacts.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace npucompute {
namespace {

constexpr std::array<aclptiCallbackId, 9> kHardwareInfoTriggerCallbackIds = {
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernel,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry,
    ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad,
    ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol,
};
constexpr double kMsopprofA5FallbackFrequencyMhz = 1650.0;

bool IsHardwareInfoTriggerCallback(aclptiCallbackId cbid)
{
    return std::find(kHardwareInfoTriggerCallbackIds.begin(), kHardwareInfoTriggerCallbackIds.end(), cbid) !=
           kHardwareInfoTriggerCallbackIds.end();
}

bool DebugEnabled()
{
    const char* value = std::getenv("NPU_COMPUTE_DEBUG");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool CreatePrivatePipelineRoot(boost::filesystem::path* root, std::string* error)
{
    std::string path = "/tmp/npu-compute-pipeline-XXXXXX";
    if (::mkdtemp(path.data()) == nullptr) {
        *error = "create private Pipeline staging failed: " + std::string(std::strerror(errno));
        return false;
    }
    *root = std::move(path);
    return true;
}

std::string ReportPrefix(std::uint64_t sequence)
{
    if (sequence == 0) {
        return {};
    }
    std::ostringstream output;
    output << "collection-p" << static_cast<long long>(::getpid()) << '-' << std::setw(4) << std::setfill('0')
           << sequence << '/';
    return output.str();
}

std::string_view Trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

bool ParsePositiveDouble(std::string_view text, double* result)
{
    const std::string_view trimmed = Trim(text);
    if (trimmed.empty() || result == nullptr) {
        return false;
    }
    std::string owned(trimmed);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(owned.c_str(), &end);
    if (errno != 0 || end == owned.c_str() || *end != '\0' || parsed <= 0.0 || !std::isfinite(parsed)) {
        return false;
    }
    *result = parsed;
    return true;
}

void LoadCsvDeviceInfo(ReportConfig* config, bool loadFrequencies)
{
    if (config == nullptr) {
        return;
    }

    DynamicHardwareDeviceApi api;
    DiagnosticSink diagnostics = [](std::string_view message) {
        npucompute::detail::DebugLog(
            "npu-compute", "CSV hardware config diagnostic: %.*s", static_cast<int>(message.size()), message.data());
    };

    std::string detectedSoc;
    if (api.GetSocName(&detectedSoc) && !detectedSoc.empty()) {
        config->socName = std::move(detectedSoc);
    }
    if (loadFrequencies) {
        std::uint32_t aiCubeFrequencyMhz = 0;
        std::uint32_t aiVectorFrequencyMhz = 0;
        CollectAiCoreFrequencies(api, &aiCubeFrequencyMhz, &aiVectorFrequencyMhz, &diagnostics);
        if (aiCubeFrequencyMhz != 0) {
            config->aicFrequencyMhz = static_cast<double>(aiCubeFrequencyMhz);
        }
        if (aiVectorFrequencyMhz != 0) {
            config->aivFrequencyMhz = static_cast<double>(aiVectorFrequencyMhz);
        }
    }
    npucompute::detail::DebugLog(
        "npu-compute",
        "CSV hardware config: source=DeviceApi socName=%s fallbackFrequency=%f aicFrequency=%f "
        "aivFrequency=%f",
        config->socName.c_str(), config->frequencyMhz, config->aicFrequencyMhz, config->aivFrequencyMhz);
}

} // namespace

namespace {

aclptiResult OnDataModuleShutdown(void* userData)
{
    auto* runtime = static_cast<NpuComputeRuntime*>(userData);
    if (runtime == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    return runtime->ShutdownAfterPtiDrain() == 0 ? ACLPTI_SUCCESS : ACLPTI_ERROR_INTERNAL;
}

} // namespace

NpuComputeRuntime& NpuComputeRuntime::Instance()
{
    static NpuComputeRuntime instance;
    return instance;
}

int NpuComputeRuntime::Initialize(const RuntimeConfig& config, ArtifactSink& sink, std::function<int(int)> onDrained)
{
    artifact_sink_ = &sink;
    on_drained_ = std::move(onDrained);
    initializing_ = true;
    struct InitializationGuard {
        std::atomic<bool>& value;
        ~InitializationGuard() { value = false; }
    } guard{initializing_};
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    if (!section_config_.Load(config.sections, &error)) {
        std::fprintf(stderr, "[libnpu-compute] invalid sections: %s\n", error.c_str());
        return kInitializeFailed;
    }
    csv_config_.pmuDataLevel = config.pmuLevel;
    csv_frequency_override_ = false;
    csv_device_info_loaded_ = false;
    csv_config_.frequencyMhz = kMsopprofA5FallbackFrequencyMhz;
    csv_config_.aicFrequencyMhz = 0.0;
    csv_config_.aivFrequencyMhz = 0.0;
    if (const char* frequency = std::getenv("NPU_COMPUTE_FREQUENCY_MHZ");
        frequency != nullptr && frequency[0] != '\0') {
        double parsed = 0.0;
        if (!ParsePositiveDouble(frequency, &parsed)) {
            std::fprintf(stderr, "[libnpu-compute] invalid NPU_COMPUTE_FREQUENCY_MHZ: %s\n", frequency);
            return kInitializeFailed;
        }
        csv_config_.frequencyMhz = parsed;
        csv_config_.aicFrequencyMhz = parsed;
        csv_config_.aivFrequencyMhz = parsed;
        csv_frequency_override_ = true;
    }
    pm_sampling_enabled_ = section_config_.Params()->collectPcSampling;
    pipeline_enabled_ = section_config_.PipelineEnabled();
    if (pipeline_enabled_) {
        if (!CreatePrivatePipelineRoot(&pipeline_temporary_root_, &error)) {
            std::fprintf(stderr, "[libnpu-compute] initialize PipeTrace staging failed: %s\n", error.c_str());
            return kInitializeFailed;
        }
        if (CreatePipeTraceProcessStaging(pipeline_temporary_root_, &pipeline_process_directory_, &error) !=
                ACLPTI_SUCCESS ||
            WritePipeTraceManifest(pipeline_process_directory_, "collecting", ACLPTI_SUCCESS, pipeline_fragments_) !=
                ACLPTI_SUCCESS) {
            std::fprintf(stderr, "[libnpu-compute] initialize PipeTrace staging failed: %s\n", error.c_str());
            return kInitializeFailed;
        }
    }
    npucompute::detail::DebugLog(
        "npu-compute", "PMU data level configured: %s",
        csv_config_.pmuDataLevel == PmuDataLevel::Task ? "task" : "block");

    auto consumer = PmuDataConsumer::Create(
        [this](std::shared_ptr<const aclptiProfilingDataResult> result) { return ProcessPmuData(std::move(result)); });
    if (consumer == nullptr || consumer->Start() != ACLPTI_SUCCESS) {
        return kInitializeFailed;
    }
    std::weak_ptr<PmuDataConsumer> weakConsumer = consumer;
    const aclptiResult registerStatus =
        aclptiRegisterProfilingDataCallback([weakConsumer](std::shared_ptr<const aclptiProfilingDataResult> result) {
            const auto activeConsumer = weakConsumer.lock();
            return activeConsumer == nullptr ? ACLPTI_ERROR_INVALID_STATE : activeConsumer->Submit(std::move(result));
        });
    if (registerStatus != ACLPTI_SUCCESS) {
        consumer->ShutdownAndDrain();
        return kInitializeFailed;
    }
    const aclptiResult shutdownRegisterStatus = aclptiRegisterDataModuleShutdownCallback(&OnDataModuleShutdown, this);
    if (shutdownRegisterStatus != ACLPTI_SUCCESS) {
        consumer->ShutdownAndDrain();
        return kInitializeFailed;
    }
    pmu_consumer_ = std::move(consumer);

    const bool hardwareInitialized = hardware_info_collector_.Initialize(
        boost::filesystem::current_path(),
        [this](const boost::filesystem::path&, std::string_view jsonl, std::string* publishError) {
            bool artifactStarted = false;
            try {
                artifact_sink_->Begin("HardwareInfo.jsonl");
                artifactStarted = true;
                artifact_sink_->Write(jsonl);
                artifact_sink_->Commit(1);
                return PublishResult::Published;
            } catch (const std::exception& exception) {
                if (artifactStarted) {
                    artifact_sink_->Abort();
                }
                *publishError = exception.what();
                return PublishResult::Failed;
            }
        },
        &error);
    if (!hardwareInitialized) {
        std::fprintf(stderr, "[libnpu-compute] initialize HardwareInfo collector failed: %s\n", error.c_str());
        pmu_consumer_->ShutdownAndDrain();
        pmu_consumer_.reset();
        return kInitializeFailed;
    }

    aclptiResult result = aclptiSubscribe(&subscriber_, &NpuComputeRuntime::HardwareInfoTriggerCallback, this, nullptr);
    if (result != ACLPTI_SUCCESS) {
        std::fprintf(stderr, "[libnpu-compute] aclptiSubscribe failed: %d\n", result);
        pmu_consumer_->ShutdownAndDrain();
        pmu_consumer_.reset();
        hardware_info_collector_.Stop();
        return result;
    }
    if (subscriber_ == nullptr) {
        std::fprintf(stderr, "[libnpu-compute] aclptiSubscribe returned a null handle\n");
        pmu_consumer_->ShutdownAndDrain();
        pmu_consumer_.reset();
        hardware_info_collector_.Stop();
        return kInitializeFailed;
    }
    std::fprintf(stderr, "[libnpu-compute] subscriber initialized\n");

    enabled_hardware_callback_count_ = 0;
    for (aclptiCallbackId cbid : kHardwareInfoTriggerCallbackIds) {
        result = aclptiEnableCallback(true, subscriber_, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid);
        if (result != ACLPTI_SUCCESS) {
            std::fprintf(stderr, "[libnpu-compute] aclptiEnableCallback failed for cbid=%u: %d\n", cbid, result);
            DisableHardwareCallbacks();
            pmu_consumer_->ShutdownAndDrain();
            pmu_consumer_.reset();
            hardware_info_collector_.Stop();
            return result;
        }
        ++enabled_hardware_callback_count_;
        if (DebugEnabled()) {
            std::fprintf(stderr, "[libnpu-compute] enabled ACL PTI callback cbid=%u\n", cbid);
        }
    }

    result = aclptiRangeProfilerSetConfig(section_config_.Params());
    if (result != ACLPTI_SUCCESS) {
        std::fprintf(stderr, "[libnpu-compute] aclptiRangeProfilerSetConfig failed: %d\n", result);
        DisableHardwareCallbacks();
        pmu_consumer_->ShutdownAndDrain();
        pmu_consumer_.reset();
        hardware_info_collector_.Stop();
        return result;
    }
    std::fprintf(stderr, "[libnpu-compute] configured sections=%s\n", section_config_.JoinedSections().c_str());
    return ACLPTI_SUCCESS;
}

void NpuComputeRuntime::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        DisableHardwareCallbacks();
    }
    hardware_info_collector_.Stop();
}

void NpuComputeRuntime::DisableHardwareCallbacks() noexcept
{
    while (enabled_hardware_callback_count_ > 0) {
        const aclptiCallbackId cbid = kHardwareInfoTriggerCallbackIds[enabled_hardware_callback_count_ - 1];
        const aclptiResult result = aclptiEnableCallback(false, subscriber_, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid);
        if (result != ACLPTI_SUCCESS) {
            std::fprintf(stderr, "[libnpu-compute] disable ACL PTI callback failed for cbid=%u: %d\n", cbid, result);
        } else if (DebugEnabled()) {
            std::fprintf(stderr, "[libnpu-compute] disabled ACL PTI callback cbid=%u\n", cbid);
        }
        --enabled_hardware_callback_count_;
    }
}

int NpuComputeRuntime::ShutdownAfterPtiDrain()
{
    if (initializing_) {
        return 0;
    }
    std::shared_ptr<PmuDataConsumer> consumer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        consumer = std::move(pmu_consumer_);
    }
    aclptiResult status = ACLPTI_SUCCESS;
    if (consumer != nullptr) {
        status = consumer->ShutdownAndDrain();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (pipeline_enabled_ && !pipeline_finalized_) {
        if (status == ACLPTI_SUCCESS && pipeline_fragments_.empty()) {
            status = ACLPTI_ERROR_TRACE_INCOMPLETE;
        }
        const std::string state = status == ACLPTI_SUCCESS ? "complete" : "failed";
        const aclptiResult manifestStatus = WritePipeTraceManifest(
            pipeline_process_directory_, state, status, pipeline_fragments_,
            status == ACLPTI_SUCCESS ? std::string() : "pipeline collection or processing failed");
        if (manifestStatus != ACLPTI_SUCCESS) {
            status = manifestStatus;
        }
        if (manifestStatus == ACLPTI_SUCCESS && artifact_sink_ != nullptr) {
            const aclptiResult publishStatus = PublishPipelineArtifacts();
            if (status == ACLPTI_SUCCESS) {
                status = publishStatus;
            }
        }
        pipeline_finalized_ = true;
    }
    if (!pipeline_temporary_root_.empty()) {
        boost::system::error_code cleanupError;
        boost::filesystem::remove_all(pipeline_temporary_root_, cleanupError);
        pipeline_temporary_root_.clear();
        if (cleanupError && status == ACLPTI_SUCCESS) {
            status = ACLPTI_ERROR_TRACE_WRITE;
        }
    }
    if (pm_sampling_enabled_ && (!PmSamplingComplete() || error_count_ != 0)) {
        if (error_count_ == 0)
            ++error_count_;
        status = ACLPTI_ERROR_TRACE_INCOMPLETE;
    }
    const int result = status == ACLPTI_SUCCESS ? 0 : kInitializeFailed;
    return on_drained_ ? on_drained_(result) : result;
}

aclptiResult NpuComputeRuntime::ProcessPmuData(std::shared_ptr<const aclptiProfilingDataResult> result)
{
    if (result == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    for (const auto* pmuLogs : {&result->pmuLogs, &result->taskPmuLogs}) {
        for (const auto& [blockKey, row] : *pmuLogs) {
            if (blockKey.blockId != row.blockId || blockKey.subBlockId != row.subBlockId ||
                blockKey.coreType != row.coreType || blockKey.coreId != row.coreId) {
                return ACLPTI_ERROR_ASSEMBLE;
            }
        }
    }
    ReportConfig csvConfig;
    bool frequencyOverride = false;
    // Kernel EXIT arrives after replay PMU data, so publish HardwareInfo before processing CSV output.
    hardware_info_collector_.CollectOnKernelLaunch();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!csv_device_info_loaded_) {
            LoadCsvDeviceInfo(&csv_config_, !csv_frequency_override_);
            csv_device_info_loaded_ = true;
        }
        csvConfig = csv_config_;
        frequencyOverride = csv_frequency_override_;
    }
    KernelMetadata metadata = kernel_metadata_collector_.Snapshot();
    DynamicHardwareDeviceApi api;
    metadata.hardware = CollectSummaryHardwareInfo(api);
    metadata.ratedAicFrequencyMhz = metadata.hardware.ratedAicFrequencyMhz;
    metadata.ratedAivFrequencyMhz = metadata.hardware.ratedAivFrequencyMhz;
    if (frequencyOverride) {
        // An explicit validated NPU_COMPUTE_FREQUENCY_MHZ remains an authoritative input.
        metadata.hardware.currentAicFrequencyMhz = csvConfig.aicFrequencyMhz;
        metadata.hardware.currentAivFrequencyMhz = csvConfig.aivFrequencyMhz;
    }
    const auto& rows = csvConfig.pmuDataLevel == PmuDataLevel::Block ? result->pmuLogs : result->taskPmuLogs;
    row_count_ += rows.size();
    error_count_ += result->errorStats.failedRecordCount;
    if (result->status != ACLPTI_SUCCESS && result->errorStats.failedRecordCount == 0) {
        ++error_count_;
    }
    std::set<std::uint64_t> replayIds;
    for (const auto& entry : result->taskLogs) {
        for (const auto& row : entry.second) {
            replayIds.insert(row.replayId);
        }
    }
    for (const auto& entry : result->pipelineData)
        replayIds.insert(entry.first);
    for (const auto& entry : result->pmSamplingContexts)
        replayIds.insert(entry.first);
    for (const auto& chunk : result->pcSamplingData)
        replayIds.insert(chunk.replayId);
    replay_count_ += replayIds.size();
    const auto prefix = ReportPrefix(report_sequence_);
    aclptiResult csvStatus = ACLPTI_SUCCESS;
    if (!section_config_.Sections().empty() || pipeline_enabled_) {
        csvStatus = WritePmuReport(
            *result, section_config_.Sections(), csvConfig, metadata, artifact_sink_, prefix, &degraded_);
    }

    ++report_sequence_;
    aclptiResult firstStatus = result->status == ACLPTI_SUCCESS ? csvStatus : result->status;

    if (pipeline_enabled_) {
        biu_clock_config_.aicFrequencyHz = csvConfig.aicFrequencyMhz * 1000000.0;
        biu_clock_config_.aivFrequencyHz = csvConfig.aivFrequencyMhz * 1000000.0;
        DynamicHardwareDeviceApi deviceApi;
        bool pipelineReplaySeen = false;
        for (const auto& [replayId, replay] : result->pipelineData) {
            pipelineReplaySeen = true;
            npucompute::detail::DebugLog(
                "npu-compute",
                "BIU replay input: replay=%llu device=%d status=%d receivedBytes=%llu acceptedBytes=%llu "
                "rejectedChunks=%llu failedRecords=%llu channels=%zu",
                static_cast<unsigned long long>(replayId), replay.deviceId, static_cast<int>(replay.status),
                static_cast<unsigned long long>(replay.stats.receivedBytes),
                static_cast<unsigned long long>(replay.stats.acceptedBytes),
                static_cast<unsigned long long>(replay.stats.rejectedChunkCount),
                static_cast<unsigned long long>(replay.stats.failedRecordCount), replay.channels.size());
            const std::string fragmentName = "result-" + std::to_string(pipeline_result_sequence_) + "-device-" +
                                             std::to_string(replay.deviceId) + "-replay-" + std::to_string(replayId) +
                                             ".json";
            PipeTraceWriter writer;
            std::string writerError;
            aclptiResult parseStatus = writer.Begin(pipeline_process_directory_ / fragmentName, &writerError);
            BiuParseStats stats;
            if (parseStatus == ACLPTI_SUCCESS) {
                const bool frequencyAvailable =
                    deviceApi.GetSyscntFrequencyHz(replay.deviceId, &biu_clock_config_.syscntFrequencyHz);
                npucompute::detail::DebugLog(
                    "npu-compute", "BIU oscillator frequency query: replay=%llu device=%d success=%d frequencyHz=%.3f",
                    static_cast<unsigned long long>(replayId), replay.deviceId, frequencyAvailable ? 1 : 0,
                    biu_clock_config_.syscntFrequencyHz);
                if (!frequencyAvailable) {
                    std::fprintf(
                        stderr,
                        "[libnpu-compute] failed to query positive device oscillator frequency "
                        "(halGetDeviceInfo INFO_TYPE_DEV_OSC_FREQUE) for device %d\n",
                        replay.deviceId);
                    parseStatus = ACLPTI_ERROR_INVALID_PARAMETER;
                }
            }
            biu_clock_config_.source = "halGetDeviceInfo device oscillator and device AI-core frequencies";
            if (parseStatus == ACLPTI_SUCCESS) {
                parseStatus = ParsePipelineReplay(
                    replayId, replay, biu_clock_config_,
                    [&writer](const BiuTraceEvent& event) { return writer.Append(event); }, &stats);
            }
            if (stats.incompleteIntervalCount != 0) {
                std::fprintf(
                    stderr,
                    "[libnpu-compute] WARNING: BIU device=%d replay=%llu: skipped %llu incomplete intervals "
                    "(missing start or end); retained %llu pipe intervals and %llu DFX trace events. "
                    "Pipeline trace may be partial.\n",
                    replay.deviceId, static_cast<unsigned long long>(replayId),
                    static_cast<unsigned long long>(stats.incompleteIntervalCount),
                    static_cast<unsigned long long>(stats.intervalCount),
                    static_cast<unsigned long long>(stats.dfxEventCount));
            }
            if (parseStatus == ACLPTI_SUCCESS) {
                parseStatus = writer.Commit();
            }
            if (parseStatus == ACLPTI_SUCCESS) {
                pipeline_fragments_.push_back(PipeTraceFragmentInfo{
                    pipeline_result_sequence_,
                    replayId,
                    replay.deviceId,
                    fragmentName,
                    writer.EventCount(),
                });
            } else {
                writer.Abort();
            }
            npucompute::detail::DebugLog(
                "npu-compute",
                "BIU replay processed: device=%d replay=%llu status=%d payloadBytes=%llu paddingBytes=%llu "
                "intervals=%llu invalid=%llu incomplete=%llu",
                replay.deviceId, static_cast<unsigned long long>(replayId), static_cast<int>(parseStatus),
                static_cast<unsigned long long>(stats.payloadBytes),
                static_cast<unsigned long long>(stats.paddingBytes),
                static_cast<unsigned long long>(stats.intervalCount),
                static_cast<unsigned long long>(stats.invalidRecordCount),
                static_cast<unsigned long long>(stats.incompleteIntervalCount));
            if (firstStatus == ACLPTI_SUCCESS && parseStatus != ACLPTI_SUCCESS) {
                firstStatus = parseStatus;
            }
        }
        if (!pipelineReplaySeen && firstStatus == ACLPTI_SUCCESS) {
            firstStatus = ACLPTI_ERROR_TRACE_INCOMPLETE;
        }
        ++pipeline_result_sequence_;
    }
    if (pm_sampling_enabled_) {
        const auto samplingStatus = ProcessPmSampling(*result, prefix);
        if (samplingStatus != ACLPTI_SUCCESS) {
            ++pm_sampling_failures_;
            ++error_count_;
            if (firstStatus == ACLPTI_SUCCESS)
                firstStatus = samplingStatus;
        }
    }
    // Preserve the source failure while still writing data from successful replays.
    return firstStatus;
}

aclptiResult NpuComputeRuntime::ProcessPmSampling(const aclptiProfilingDataResult& result, const std::string& prefix)
{
    // Only this consumer worker writes sampling files. Keep both ELF images alive
    // through symbolization, even if the application has already unloaded them.
    struct Staging {
        boost::filesystem::path directory;
        ~Staging()
        {
            boost::system::error_code ignored;
            if (!directory.empty())
                boost::filesystem::remove_all(directory, ignored);
        }
    } staging;
    try {
        // Current replay configuration creates exactly one PmSampling replay for
        // each original launch. Never merge PC keys from unrelated ELF contexts.
        if (result.pmSamplingContexts.size() != 1 || !result.pmSamplingContexts.begin()->second) {
            throw std::runtime_error("missing or ambiguous PmSampling ELF context");
        }
        const auto& context = *result.pmSamplingContexts.begin();
        PmSamplingResult sampling;
        bool complete = true;
        const auto failed = [&](const std::string& file, const std::string& stage, const std::string& reason) {
            complete = false;
            std::fprintf(
                stderr,
                "[libnpu-compute] %s%s: PmSampling %s failed: %s; "
                "excluded this file; other valid results are retained\n",
                prefix.c_str(), file.c_str(), stage.c_str(), reason.c_str());
        };
        try {
            if (!context.second->instructionMapError.empty()) {
                throw std::runtime_error("instruction mapping failed: " + context.second->instructionMapError);
            }
            const auto failedReplay = result.errorStats.failedRecordCountByReplay.find(context.first);
            if (failedReplay != result.errorStats.failedRecordCountByReplay.end() && failedReplay->second != 0) {
                throw std::runtime_error(
                    "sampling replay " + std::to_string(context.first) + " contains " +
                    std::to_string(failedReplay->second) + " failed records");
            }
            PmSamplingDecoder decoder(context.first, context.second);
            for (const auto& chunk : result.pcSamplingData) {
                if (!decoder.Consume(chunk))
                    throw std::runtime_error(decoder.Error());
            }
            if (!decoder.Finish(sampling))
                throw std::runtime_error(decoder.Error());
            if (sampling.rows.empty())
                throw std::runtime_error(
                    result.pcSamplingData.empty() ?
                        "Msprof returned no PC sampling records; PC/state sampling requires SIMT instructions" :
                        "no business samples received after excluding instrumentation instructions");
        } catch (const std::exception& error) {
            sampling = {};
            failed("summary.db", "sample decoding", error.what());
        }

        const boost::filesystem::path parent("/tmp");
        std::string pattern = (parent / ".npu-compute-pm-sampling-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("cannot create PmSampling staging: " + std::string(std::strerror(errno)));
        }
        staging.directory = pattern;
        std::string error;
        const auto database = staging.directory / "summary.db";
        std::vector<boost::filesystem::path> files;
        bool databaseReady = false;
        if (!sampling.rows.empty()) {
            databaseReady = WritePmSamplingSqlite(database.string(), sampling, error);
            if (databaseReady)
                files.push_back(database);
            else
                failed("summary.db", "database writing", error);
        }
        PmSamplingArtifacts artifacts;
        // Both LLVM inputs default to the original ELF, matching the normalized
        // sampling addresses. Each output can succeed independently.
        if (!GeneratePmSamplingArtifacts(
                staging.directory.string(), *context.second, sampling, PmSamplingArtifactOptions{}, artifacts, error)) {
            complete = false;
            std::fprintf(stderr, "[libnpu-compute] PmSampling LLVM generation: %s\n", error.c_str());
        }
        if (!artifacts.disassemblyPath.empty())
            files.emplace_back(artifacts.disassemblyPath);
        if (!artifacts.pcStackPath.empty())
            files.emplace_back(artifacts.pcStackPath);
        // Transport errors still abort this collection: file boundaries and the
        // receiver state cannot be trusted after a failed Begin/Write/Commit.
        for (const auto& file : files) {
            bool started = false;
            try {
                std::ifstream input(file.string(), std::ios::binary);
                if (!input)
                    throw std::runtime_error("cannot read PmSampling artifact");
                artifact_sink_->Begin(prefix + file.filename().string());
                started = true;
                constexpr std::size_t kArtifactTransferBufferBytes = 16 * 1024;
                std::array<char, kArtifactTransferBufferBytes> buffer{};
                while (input) {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    if (input.gcount() > 0) {
                        artifact_sink_->Write(
                            std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())));
                    }
                }
                if (!input.eof())
                    throw std::runtime_error("cannot read complete PmSampling artifact");
                artifact_sink_->Commit(file == database ? sampling.rows.size() : 0);
                started = false;
            } catch (...) {
                if (started)
                    artifact_sink_->Abort();
                throw;
            }
        }
        boost::filesystem::remove_all(staging.directory);
        staging.directory.clear();
        if (databaseReady)
            row_count_ += sampling.rows.size();
        if (complete)
            ++pm_sampling_completed_;
        detail::DebugLog(
            "npu-compute", "PmSampling completed: rows=%llu unresolvedPCs=%llu",
            static_cast<unsigned long long>(sampling.rows.size()),
            static_cast<unsigned long long>(artifacts.unresolvedPcCount));
        return complete ? ACLPTI_SUCCESS : ACLPTI_ERROR_TRACE_INCOMPLETE;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[libnpu-compute] PmSampling processing failed: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "[libnpu-compute] PmSampling processing failed: unexpected exception\n");
    }
    return ACLPTI_ERROR_TRACE_WRITE;
}

aclptiResult NpuComputeRuntime::PublishPipelineArtifacts()
{
    if (artifact_sink_ == nullptr || pipeline_process_directory_.empty()) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    try {
        std::vector<boost::filesystem::path> files{pipeline_process_directory_ / "manifest.json"};
        for (const auto& fragment : pipeline_fragments_) {
            files.push_back(pipeline_process_directory_ / fragment.file);
        }
        aclptiResult result = ACLPTI_SUCCESS;
        for (const auto& path : files) {
            const std::string artifactName = ".biu-staging/process-uds/" + path.filename().string();
            boost::system::error_code statusError;
            const auto status = boost::filesystem::symlink_status(path, statusError);
            if (statusError || !boost::filesystem::is_regular_file(status)) {
                if (path.filename() == "manifest.json") {
                    return ACLPTI_ERROR_TRACE_WRITE;
                }
                result = ACLPTI_ERROR_TRACE_WRITE;
                continue;
            }
            std::ifstream input(path.string(), std::ios::binary);
            if (!input.is_open()) {
                if (path.filename() == "manifest.json") {
                    return ACLPTI_ERROR_TRACE_WRITE;
                }
                result = ACLPTI_ERROR_TRACE_WRITE;
                continue;
            }
            bool artifactStarted = false;
            try {
                artifact_sink_->Begin(artifactName);
                artifactStarted = true;
                constexpr std::size_t kArtifactTransferBufferBytes = 16 * 1024;
                std::array<char, kArtifactTransferBufferBytes> buffer{};
                while (input.good()) {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = input.gcount();
                    if (count > 0) {
                        artifact_sink_->Write(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
                    }
                }
                if (!input.eof()) {
                    throw std::runtime_error("read Pipeline artifact failed");
                }
                artifact_sink_->Commit(0);
                artifactStarted = false;
            } catch (...) {
                if (artifactStarted) {
                    artifact_sink_->Abort();
                }
                throw;
            }
        }
        return result;
    } catch (const std::exception& error) {
        detail::DebugLog("npu-compute", "Pipeline artifact publication failed: %s", error.what());
        return ACLPTI_ERROR_TRACE_WRITE;
    }
}

void NpuComputeRuntime::HardwareInfoTriggerCallback(
    void* userData, aclptiCallbackDomain domain, aclptiCallbackId cbid, const aclptiCallbackData* callbackData) noexcept
{
    if (userData == nullptr) {
        std::fprintf(stderr, "[libnpu-compute] HardwareInfo trigger callback received null userData\n");
        return;
    }
    if (domain != ACLPTI_CB_DOMAIN_RUNTIME_API || callbackData == nullptr || callbackData->domain != domain ||
        callbackData->cbid != cbid || !IsHardwareInfoTriggerCallback(cbid)) {
        return;
    }
    try {
        static_cast<NpuComputeRuntime*>(userData)->kernel_metadata_collector_.OnCallback(cbid, *callbackData);
    } catch (...) {
        std::fprintf(stderr, "[libnpu-compute] Kernel metadata collection failed\n");
    }
    const bool isLaunch = cbid != ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction &&
                          cbid != ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry &&
                          cbid != ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad &&
                          cbid != ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol;
    const bool accepted =
        isLaunch && callbackData->callbackSite == ACLPTI_API_EXIT && callbackData->retval == ACL_SUCCESS;
    if (DebugEnabled()) {
        std::fprintf(
            stderr, "[libnpu-compute] runtime callback domain=%d cbid=%u site=%d retval=%d accepted=%d\n",
            static_cast<int>(domain), cbid, static_cast<int>(callbackData->callbackSite),
            static_cast<int>(callbackData->retval), accepted ? 1 : 0);
    }
    if (!accepted) {
        return;
    }
    try {
        static_cast<NpuComputeRuntime*>(userData)->hardware_info_collector_.CollectOnKernelLaunch();
    } catch (...) {
        std::fprintf(stderr, "[libnpu-compute] HardwareInfo trigger callback failed\n");
    }
}

} // namespace npucompute
