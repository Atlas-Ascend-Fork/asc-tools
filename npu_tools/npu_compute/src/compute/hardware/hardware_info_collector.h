/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_COLLECTOR_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_COLLECTOR_H

#include "hardware/hardware_info_device.h"
#include "hardware/hardware_info_types.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace npucompute {

enum class HardwareCollectionState {
    Created,
    WaitingKernel,
    Collecting,
    Completed,
    Failed,
    NoKernelLaunch,
};

enum class PublishResult {
    Published,
    Failed,
};

using HostInfoCollectionFunction = std::function<bool(const boost::filesystem::path&, HostInfo*, DiagnosticSink*)>;
using HardwareInfoPublishFunction =
    std::function<PublishResult(const boost::filesystem::path&, std::string_view, std::string*)>;

struct HardwareInfoDependencies {
    HostInfoCollectionFunction collectHostInfo;
    std::shared_ptr<HardwareDeviceApi> deviceApi;
    HardwareInfoPublishFunction publish;
    DiagnosticSink diagnostics;
};

class HardwareInfoCollector {
public:
    HardwareInfoCollector();
    explicit HardwareInfoCollector(HardwareInfoDependencies dependencies);
    ~HardwareInfoCollector();

    HardwareInfoCollector(const HardwareInfoCollector&) = delete;
    HardwareInfoCollector& operator=(const HardwareInfoCollector&) = delete;
    HardwareInfoCollector(HardwareInfoCollector&&) = delete;
    HardwareInfoCollector& operator=(HardwareInfoCollector&&) = delete;

    bool Initialize(const boost::filesystem::path& probePath, HardwareInfoPublishFunction publish, std::string* error);
    void CollectOnKernelLaunch() noexcept;
    void Stop() noexcept;
    HardwareCollectionState State() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_COLLECTOR_H
