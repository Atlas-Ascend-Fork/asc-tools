/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_DEVICE_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_DEVICE_H

#include "hardware/hardware_info_types.h"
#include "compute_types.h"

#include <cstdint>
#include <string>

namespace npucompute {

class HardwareDeviceApi {
public:
    virtual ~HardwareDeviceApi() = default;

    virtual bool GetDeviceCount(std::int32_t* value) = 0;
    virtual bool GetSocName(std::string* value) = 0;
    virtual bool GetDeviceAttribute(std::int32_t deviceId, std::int32_t attribute, std::int64_t* value) = 0;
    virtual bool GetPlatformValue(std::int32_t type, std::string* value) = 0;
    virtual bool GetControlCpuCount(std::int32_t deviceId, uint32_t* value) = 0;
    virtual bool GetAiCpuFrequency(std::int32_t deviceId, uint32_t* value) = 0;
    virtual bool GetAiCoreFrequencies(std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz) = 0;
    virtual bool GetCurrentAiCoreFrequencies(std::int32_t, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz)
    {
        *aicFrequencyMhz = 0;
        *aivFrequencyMhz = 0;
        return false;
    }
    virtual bool GetRatedAiCoreFrequencies(std::int32_t, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz)
    {
        *aicFrequencyMhz = 0;
        *aivFrequencyMhz = 0;
        return false;
    }
    virtual bool GetChipVersion(std::int32_t deviceId, std::string* value) = 0;
    virtual bool GetHbmUsage(std::int32_t deviceId, uint64_t* freeBytes, uint64_t* totalBytes) = 0;
    virtual bool GetHbmFrequency(std::int32_t deviceId, uint32_t* value) = 0;
};

bool CollectDevice0Info(
    HardwareDeviceApi& api, DeviceInfo* device, CpuInfo* cpu, AiCoreInfo* aiCore, MemoryInfo* memory,
    DiagnosticSink* diagnostics);

bool CollectAiCoreCounts(
    HardwareDeviceApi& api, std::uint32_t* aiCubeCount, std::uint32_t* aiVectorCount, DiagnosticSink* diagnostics);

bool CollectAiCoreFrequencies(
    HardwareDeviceApi& api, std::uint32_t* aiCubeFrequencyMhz, std::uint32_t* aiVectorFrequencyMhz,
    DiagnosticSink* diagnostics);

SummaryHardwareInfo CollectSummaryHardwareInfo(HardwareDeviceApi& api, DiagnosticSink* diagnostics = nullptr);

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_DEVICE_H
