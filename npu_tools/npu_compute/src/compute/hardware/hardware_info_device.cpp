/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "hardware/hardware_info_device.h"

#include <acl/acl_rt.h>
#include <acl/acl_platform.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace npucompute {
namespace {
constexpr std::uint32_t DECIMAL_RADIX = 10;

constexpr std::int32_t DEVICE_ID = 0;
constexpr std::uint32_t AI_CORE_FREQUENCY_FALLBACK_MHZ = 1650;
constexpr double BYTES_PER_MB = 1024.0 * 1024.0;

void Diagnose(DiagnosticSink* diagnostics, const std::string& message)
{
    if (diagnostics != nullptr && *diagnostics) {
        (*diagnostics)(message);
    }
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

template <typename Integer>
bool ParseUnsigned(std::string_view text, Integer* value)
{
    const std::string_view trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }
    Integer parsedValue = 0;
    const Integer maximum = std::numeric_limits<Integer>::max();
    for (const char character : trimmed) {
        if (character < '0' || character > '9') {
            return false;
        }
        const Integer digit = static_cast<Integer>(character - '0');
        if (parsedValue > (maximum - digit) / DECIMAL_RADIX) {
            return false;
        }
        parsedValue = parsedValue * DECIMAL_RADIX + digit;
    }
    *value = parsedValue;
    return true;
}

bool ReadCountAttribute(
    HardwareDeviceApi& api, std::int32_t attribute, std::string_view fieldName, uint32_t* result,
    DiagnosticSink* diagnostics)
{
    std::int64_t value = 0;
    if (!api.GetDeviceAttribute(DEVICE_ID, attribute, &value)) {
        Diagnose(diagnostics, "GetDeviceAttribute failed for Device 0: " + std::string(fieldName));
        return false;
    }
    if (value < 0 || static_cast<uint64_t>(value) > std::numeric_limits<uint32_t>::max()) {
        Diagnose(diagnostics, "invalid " + std::string(fieldName) + " returned for Device 0");
        return false;
    }
    *result = static_cast<uint32_t>(value);
    return true;
}

void CollectChipInfo(HardwareDeviceApi& api, DeviceInfo* device, DiagnosticSink* diagnostics)
{
    std::string socName;
    if (!api.GetSocName(&socName) || socName.empty()) {
        Diagnose(diagnostics, "GetSocName failed or returned an empty value");
    } else {
        device->chipInfo = socName;
    }

    std::string chipVersion;
    if (!api.GetChipVersion(DEVICE_ID, &chipVersion)) {
        Diagnose(diagnostics, "GetChipVersion failed for Device 0");
    } else if (!device->chipInfo.empty() && !chipVersion.empty()) {
        device->chipInfo += " " + chipVersion;
    }
}

void CollectArchitecture(HardwareDeviceApi& api, DeviceInfo* device, DiagnosticSink* diagnostics)
{
    std::int64_t value = 0;
    if (!api.GetDeviceAttribute(DEVICE_ID, ACL_DEV_ATTR_NPU_ARCH, &value)) {
        Diagnose(diagnostics, "GetDeviceAttribute failed for Device 0: NPU architecture");
        return;
    }
    if (value < 0) {
        Diagnose(diagnostics, "invalid NPU architecture returned for Device 0");
        return;
    }
    device->archInfo = std::to_string(value);
}

void CollectCpuInfo(HardwareDeviceApi& api, CpuInfo* cpu, DiagnosticSink* diagnostics)
{
    if (!api.GetControlCpuCount(DEVICE_ID, &cpu->controlCpuCount)) {
        Diagnose(diagnostics, "GetControlCpuCount failed for Device 0");
    }
    ReadCountAttribute(api, ACL_DEV_ATTR_AICPU_CORE_NUM, "AI CPU core count", &cpu->aiCpuCount, diagnostics);
    if (!api.GetAiCpuFrequency(DEVICE_ID, &cpu->aiCpuFrequencyMhz)) {
        Diagnose(diagnostics, "GetAiCpuFrequency failed for Device 0");
    }
}

void CollectAiCoreInfo(HardwareDeviceApi& api, AiCoreInfo* aiCore, DiagnosticSink* diagnostics)
{
    ReadCountAttribute(api, ACL_DEV_ATTR_AICORE_CORE_NUM, "AI Core count", &aiCore->aiCoreCount, diagnostics);
    ReadCountAttribute(api, ACL_DEV_ATTR_CUBE_CORE_NUM, "Cube Core count", &aiCore->aiCubeCount, diagnostics);
    ReadCountAttribute(api, ACL_DEV_ATTR_VECTOR_CORE_NUM, "Vector Core count", &aiCore->aiVectorCount, diagnostics);
    CollectAiCoreFrequencies(api, &aiCore->aiCubeFrequencyMhz, &aiCore->aiVectorFrequencyMhz, diagnostics);
}

void CollectMemoryInfo(HardwareDeviceApi& api, MemoryInfo* memory, DiagnosticSink* diagnostics)
{
    std::string totalBytesText;
    uint64_t totalBytes = 0;
    if (!api.GetPlatformValue(ACL_PLATFORM_MEMORY_SIZE, &totalBytesText)) {
        Diagnose(diagnostics, "GetPlatformValue failed: HBM total size");
    } else if (!ParseUnsigned(totalBytesText, &totalBytes)) {
        Diagnose(diagnostics, "invalid HBM total size");
    } else {
        memory->hbmTotalMb = static_cast<double>(totalBytes) / BYTES_PER_MB;
    }

    uint64_t freeBytes = 0;
    uint64_t allocatableBytes = 0;
    if (!api.GetHbmUsage(DEVICE_ID, &freeBytes, &allocatableBytes)) {
        Diagnose(diagnostics, "GetHbmUsage failed for Device 0");
    } else if (freeBytes > allocatableBytes) {
        Diagnose(diagnostics, "invalid HBM usage returned for Device 0");
    } else {
        memory->hbmUsedMb = static_cast<double>(allocatableBytes - freeBytes) / BYTES_PER_MB;
    }

    if (!api.GetHbmFrequency(DEVICE_ID, &memory->hbmFrequencyMhz)) {
        Diagnose(diagnostics, "GetHbmFrequency failed for Device 0");
    }
}

} // namespace

bool CollectDevice0Info(
    HardwareDeviceApi& api, DeviceInfo* device, CpuInfo* cpu, AiCoreInfo* aiCore, MemoryInfo* memory,
    DiagnosticSink* diagnostics)
{
    if (device == nullptr || cpu == nullptr || aiCore == nullptr || memory == nullptr) {
        Diagnose(diagnostics, "HardwareInfo output is null");
        return false;
    }

    *device = {};
    *cpu = {};
    *aiCore = {};
    *memory = {};

    std::int32_t deviceCount = 0;
    if (!api.GetDeviceCount(&deviceCount)) {
        Diagnose(diagnostics, "GetDeviceCount failed");
        return true;
    }
    if (deviceCount < 0) {
        Diagnose(diagnostics, "invalid device count");
        return true;
    }
    device->npuCount = static_cast<uint32_t>(deviceCount);
    if (deviceCount == 0) {
        return true;
    }

    CollectChipInfo(api, device, diagnostics);
    CollectArchitecture(api, device, diagnostics);
    CollectCpuInfo(api, cpu, diagnostics);
    CollectAiCoreInfo(api, aiCore, diagnostics);
    CollectMemoryInfo(api, memory, diagnostics);
    return true;
}

bool CollectAiCoreCounts(
    HardwareDeviceApi& api, std::uint32_t* aiCubeCount, std::uint32_t* aiVectorCount, DiagnosticSink* diagnostics)
{
    if (aiCubeCount == nullptr || aiVectorCount == nullptr) {
        Diagnose(diagnostics, "AI Core count output is null");
        return false;
    }

    std::uint32_t cubeCount = 0;
    std::uint32_t vectorCount = 0;
    const bool readCube =
        ReadCountAttribute(api, ACL_DEV_ATTR_CUBE_CORE_NUM, "Cube Core count", &cubeCount, diagnostics);
    const bool readVector =
        ReadCountAttribute(api, ACL_DEV_ATTR_VECTOR_CORE_NUM, "Vector Core count", &vectorCount, diagnostics);
    *aiCubeCount = cubeCount;
    *aiVectorCount = vectorCount;
    return readCube && readVector;
}

bool CollectAiCoreFrequencies(
    HardwareDeviceApi& api, std::uint32_t* aiCubeFrequencyMhz, std::uint32_t* aiVectorFrequencyMhz,
    DiagnosticSink* diagnostics)
{
    if (aiCubeFrequencyMhz == nullptr || aiVectorFrequencyMhz == nullptr) {
        Diagnose(diagnostics, "AI Core frequency output is null");
        return false;
    }

    *aiCubeFrequencyMhz = AI_CORE_FREQUENCY_FALLBACK_MHZ;
    *aiVectorFrequencyMhz = AI_CORE_FREQUENCY_FALLBACK_MHZ;
    if (api.GetAiCoreFrequencies(DEVICE_ID, aiCubeFrequencyMhz, aiVectorFrequencyMhz)) {
        return true;
    }
    *aiCubeFrequencyMhz = AI_CORE_FREQUENCY_FALLBACK_MHZ;
    *aiVectorFrequencyMhz = AI_CORE_FREQUENCY_FALLBACK_MHZ;
    Diagnose(diagnostics, "GetAiCoreFrequencies failed for Device 0; use 1650 MHz fallback");
    return false;
}

SummaryHardwareInfo CollectSummaryHardwareInfo(HardwareDeviceApi& api, DiagnosticSink* diagnostics)
{
    SummaryHardwareInfo summary;
    summary.collected = true;
    std::string soc;
    if (api.GetSocName(&soc) && !soc.empty()) {
        summary.socName = std::move(soc);
    }
    uint32_t aic = 0;
    uint32_t aiv = 0;
    CollectAiCoreCounts(api, &aic, &aiv, diagnostics);
    if (aic > 0) {
        summary.cubeCoreCount = aic;
    }
    if (aiv > 0) {
        summary.vectorCoreCount = aiv;
    }
    aic = 0;
    aiv = 0;
    api.GetCurrentAiCoreFrequencies(DEVICE_ID, &aic, &aiv);
    if (aic > 0) {
        summary.currentAicFrequencyMhz = aic;
    }
    if (aiv > 0) {
        summary.currentAivFrequencyMhz = aiv;
    }
    aic = 0;
    aiv = 0;
    api.GetRatedAiCoreFrequencies(DEVICE_ID, &aic, &aiv);
    if (aic > 0) {
        summary.ratedAicFrequencyMhz = aic;
    }
    if (aiv > 0) {
        summary.ratedAivFrequencyMhz = aiv;
    }
    return summary;
}

} // namespace npucompute
