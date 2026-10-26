/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace npucompute {

enum class PmuDataLevel {
    Block,
    Task,
};

struct ReportConfig {
    std::string outputDirectory;
    double frequencyMhz = 1000.0;
    double aicFrequencyMhz = 0.0;
    double aivFrequencyMhz = 0.0;
    std::string socName = "950X"; // Selects model-specific theoretical transfer bandwidths.
    PmuDataLevel pmuDataLevel = PmuDataLevel::Block;
    bool fixedOutputDirectory = false;
};

enum class KernelKind {
    Cube,
    Vector,
    Mix,
};

// Actual device queries only: report/CSV default frequencies are not evidence of availability.
struct SummaryHardwareInfo {
    bool collected = false;
    std::optional<std::string> socName;
    std::optional<uint32_t> cubeCoreCount;
    std::optional<uint32_t> vectorCoreCount;
    std::optional<double> currentAicFrequencyMhz;
    std::optional<double> currentAivFrequencyMhz;
    std::optional<double> ratedAicFrequencyMhz;
    std::optional<double> ratedAivFrequencyMhz;
};

struct KernelMetadata {
    std::optional<std::string> name;
    std::optional<uint64_t> blockDim;
    std::optional<int32_t> deviceId;
    std::optional<double> ratedAicFrequencyMhz;
    std::optional<double> ratedAivFrequencyMhz;
    std::optional<KernelKind> kind;
    std::optional<uint64_t> mixBlockDim;
    SummaryHardwareInfo hardware;
};

} // namespace npucompute
