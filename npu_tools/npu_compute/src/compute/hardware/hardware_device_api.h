/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_DEVICE_API_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_DEVICE_API_H

#include "hardware/hardware_info_device.h"

#include <cstdint>
#include <memory>
#include <string>

namespace npucompute {

class DynamicSymbolResolver {
public:
    virtual ~DynamicSymbolResolver() = default;

    virtual void* FindLoadedSymbol(const char* name) = 0;
    virtual void* OpenLibrary(const char* soname) = 0;
    virtual void* FindLibrarySymbol(void* handle, const char* name) = 0;
    virtual void CloseLibrary(void* handle) = 0;
};

class DynamicHardwareDeviceApi final : public HardwareDeviceApi {
public:
    DynamicHardwareDeviceApi();
    explicit DynamicHardwareDeviceApi(std::shared_ptr<DynamicSymbolResolver> resolver);
    ~DynamicHardwareDeviceApi() override;

    DynamicHardwareDeviceApi(const DynamicHardwareDeviceApi&) = delete;
    DynamicHardwareDeviceApi& operator=(const DynamicHardwareDeviceApi&) = delete;
    DynamicHardwareDeviceApi(DynamicHardwareDeviceApi&&) = delete;
    DynamicHardwareDeviceApi& operator=(DynamicHardwareDeviceApi&&) = delete;

    bool GetDeviceCount(std::int32_t* value) override;
    bool GetSocName(std::string* value) override;
    bool GetDeviceAttribute(std::int32_t deviceId, std::int32_t attribute, std::int64_t* value) override;
    bool GetPlatformValue(std::int32_t type, std::string* value) override;
    bool GetControlCpuCount(std::int32_t deviceId, uint32_t* value) override;
    bool GetAiCpuFrequency(std::int32_t deviceId, uint32_t* value) override;
    bool GetAiCoreFrequencies(std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz) override;
    bool GetCurrentAiCoreFrequencies(
        std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz) override;
    bool GetRatedAiCoreFrequencies(
        std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz) override;
    // Device oscillator query returns kHz; expose Hz for BIU syscnt conversion.
    bool GetSyscntFrequencyHz(std::int32_t deviceId, double* frequencyHz);
    bool GetChipVersion(std::int32_t deviceId, std::string* value) override;
    bool GetHbmUsage(std::int32_t deviceId, uint64_t* freeBytes, uint64_t* totalBytes) override;
    bool GetHbmFrequency(std::int32_t deviceId, uint32_t* value) override;

private:
    bool ReadAiCoreFrequencies(
        std::int32_t deviceId, uint32_t* aicFrequencyMhz, uint32_t* aivFrequencyMhz, bool currentFirst,
        bool allowRatedFallback = true);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_DEVICE_API_H
