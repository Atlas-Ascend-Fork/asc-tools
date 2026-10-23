/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_NPU_COMPUTE_RUNTIME_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_NPU_COMPUTE_RUNTIME_H

#include "aclpti/aclpti.h"
#include "biu/biu_pipeline.h"
#include "hardware/hardware_info_collector.h"
#include "pmu/pmu_data_consumer.h"
#include "compute_types.h"
#include "runtime/section_config.h"
#include "runtime/kernel_metadata_collector.h"
#include "runtime/runtime_config.h"

#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace npucompute {

class ArtifactSink;

inline constexpr int kInitializeFailed = -1;

class NpuComputeRuntime {
public:
    static NpuComputeRuntime& Instance();

    int Initialize(const RuntimeConfig& config, ArtifactSink& sink, std::function<int(int)> onDrained);
    void Stop() noexcept;
    int ShutdownAfterPtiDrain();
    std::uint64_t RowCount() const { return row_count_; }
    std::uint64_t ErrorCount() const { return error_count_; }
    std::uint64_t ReplayCount() const { return replay_count_; }
    bool Degraded() const { return degraded_; }
    // Read after worker drain, together with RowCount/ErrorCount.
    bool PmSamplingComplete() const
    {
        return !pm_sampling_enabled_ || (pm_sampling_completed_ > 0 && pm_sampling_failures_ == 0);
    }

private:
    static void HardwareInfoTriggerCallback(
        void* userData, aclptiCallbackDomain domain, aclptiCallbackId cbid,
        const aclptiCallbackData* callbackData) noexcept;

    void DisableHardwareCallbacks() noexcept;
    aclptiResult ProcessPmuData(std::shared_ptr<const aclptiProfilingDataResult> result);
    aclptiResult PublishPipelineArtifacts();
    aclptiResult ProcessPmSampling(const aclptiProfilingDataResult& result, const std::string& prefix);

    std::mutex mutex_;
    std::shared_ptr<PmuDataConsumer> pmu_consumer_;
    aclptiSubscribeHandle subscriber_ = nullptr;
    std::size_t enabled_hardware_callback_count_ = 0;
    bool csv_frequency_override_ = false;
    bool csv_device_info_loaded_ = false;
    bool pm_sampling_enabled_ = false;
    std::uint64_t pm_sampling_completed_ = 0;
    std::uint64_t pm_sampling_failures_ = 0;
    bool pipeline_enabled_ = false;
    bool pipeline_finalized_ = false;
    uint64_t pipeline_result_sequence_ = 0;
    SectionConfig section_config_;
    ReportConfig csv_config_;
    BiuClockConfig biu_clock_config_;
    boost::filesystem::path pipeline_process_directory_;
    boost::filesystem::path pipeline_temporary_root_;
    std::vector<PipeTraceFragmentInfo> pipeline_fragments_;
    HardwareInfoCollector hardware_info_collector_;
    KernelMetadataCollector kernel_metadata_collector_;
    ArtifactSink* artifact_sink_ = nullptr;
    std::function<int(int)> on_drained_;
    std::uint64_t row_count_ = 0;
    std::uint64_t error_count_ = 0;
    std::uint64_t replay_count_ = 0;
    std::uint64_t report_sequence_ = 0;
    bool degraded_ = false;
    std::atomic<bool> initializing_{false};
};

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_NPU_COMPUTE_RUNTIME_H
