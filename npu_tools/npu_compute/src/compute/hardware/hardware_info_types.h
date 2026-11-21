/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_TYPES_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_TYPES_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace npucompute {

struct HostInfo {
    uint32_t cpuPhysicalCount = 0;
    uint32_t cpuLogicalCount = 0;
    double memoryTotalSizeMb = 0;
    double diskTotalSizeGb = 0;
};

struct DeviceInfo {
    uint32_t npuCount = 0;
    std::string chipInfo;
    std::string archInfo;
};

struct CpuInfo {
    uint32_t controlCpuCount = 0;
    uint32_t aiCpuCount = 0;
    uint32_t aiCpuFrequencyMhz = 0;
};

struct AiCoreInfo {
    uint32_t aiCoreCount = 0;
    uint32_t aiCubeCount = 0;
    uint32_t aiVectorCount = 0;
    uint32_t aiCubeFrequencyMhz = 0;
    uint32_t aiVectorFrequencyMhz = 0;
};

struct MemoryInfo {
    double hbmTotalMb = 0;
    double hbmUsedMb = 0;
    uint32_t hbmFrequencyMhz = 0;
};

struct HardwareInfoSnapshot {
    HostInfo host;
    DeviceInfo device;
    CpuInfo cpu;
    AiCoreInfo aiCore;
    MemoryInfo memory;
    std::optional<int32_t> deviceId;
};

using DiagnosticSink = std::function<void(std::string_view)>;

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_TYPES_H
