/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "injection/prof_api_stub.h"

#include "debug_log.h"
#include "profiling/prof_api.h"

#include <dlfcn.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void* g_injectionHandle = nullptr;
bool g_injectionInitialized = false;
MsprofRawDataCallback g_rawDataCallback = nullptr;
RawDataType g_activeRawDataType = PMU_DATA_TYPE;
bool g_collectBlock = false;

using AcltoolInitializeFn = int (*)();

std::size_t CountConfiguredPmus(const MsprofConfig& config)
{
    if (config.configInfo.attrs == nullptr) {
        return 0;
    }

    const MsprofConfigAttr* metrics = nullptr;
    for (std::size_t i = 0; i < config.configInfo.numAttrs; ++i) {
        if (config.configInfo.attrs[i].id == PROF_CONFIG_ATTR_AICORE_METRICS) {
            metrics = &config.configInfo.attrs[i];
            break;
        }
    }
    if (metrics == nullptr) {
        return 0;
    }

    std::size_t pmuCount = 0;
    for (uint32_t event : metrics->value.aicoreMetrics) {
        if (event != MSPROF_INVALID_AICORE_METRIC) {
            ++pmuCount;
        }
    }
    return pmuCount;
}

RawDataType ResolveRawDataType(const MsprofConfig& config)
{
    if (config.configInfo.attrs == nullptr) {
        return PMU_DATA_TYPE;
    }
    for (std::size_t i = 0; i < config.configInfo.numAttrs; ++i) {
        const MsprofConfigAttr& attr = config.configInfo.attrs[i];
        if (attr.id != PROF_CONFIG_ATTR_INSTR) {
            continue;
        }
        if (attr.value.instrMode == PROF_COMPUTE_BIU_PERF) {
            return BIU_PERF_DATA_TYPE;
        }
        if (attr.value.instrMode == PROF_COMPUTE_PC_SAMPLING) {
            return PC_SAMPLING_DATA_TYPE;
        }
    }
    return PMU_DATA_TYPE;
}

} // namespace

extern "C" ACL_TOOL_INJECTION_EXPORT int ProfApiLoadApiInjectionFromEnv()
{
    const char* injectionPath = std::getenv("ACL_API_INJECTION");
    if (injectionPath == nullptr || injectionPath[0] == '\0') {
        injection::detail::DebugLog("prof_api_stub", "ProfApiLoadApiInjectionFromEnv path is not set");
        return 0;
    }
    if (g_injectionInitialized) {
        injection::detail::DebugLog("prof_api_stub", "ProfApiLoadApiInjectionFromEnv already initialized");
        return 0;
    }

    void* handle = g_injectionHandle;
    bool openedNow = false;
    if (handle == nullptr) {
        injection::detail::DebugLog("prof_api_stub", "ProfApiLoadApiInjectionFromEnv loading path=%s", injectionPath);
        handle = dlopen(injectionPath, RTLD_NOW | RTLD_GLOBAL);
        if (handle == nullptr) {
            std::fprintf(stderr, "[prof_api] dlopen(%s) failed: %s\n", injectionPath, dlerror());
            return -1;
        }
        openedNow = true;
    } else {
        injection::detail::DebugLog("prof_api_stub", "ProfApiLoadApiInjectionFromEnv reusing loaded handle");
    }

    dlerror();
    auto init = reinterpret_cast<AcltoolInitializeFn>(dlsym(handle, "acltoolInitialize"));
    const char* error = dlerror();
    if (error != nullptr || init == nullptr) {
        std::fprintf(
            stderr, "[prof_api] dlsym(acltoolInitialize) failed: %s\n", error == nullptr ? "symbol is null" : error);
        if (openedNow) {
            dlclose(handle);
        }
        return -1;
    }
    if (openedNow) {
        g_injectionHandle = handle;
    }

    const int result = init();
    injection::detail::DebugLog("prof_api_stub", "ProfApiLoadApiInjectionFromEnv init result=%d", result);
    if (result == 0) {
        g_injectionInitialized = true;
    }
    return result;
}

extern "C" ACL_TOOL_INJECTION_EXPORT void ProfApiReset()
{
    if (g_injectionHandle != nullptr) {
        dlclose(g_injectionHandle);
    }
    g_injectionHandle = nullptr;
    g_injectionInitialized = false;
    g_rawDataCallback = nullptr;
    g_activeRawDataType = PMU_DATA_TYPE;
    g_collectBlock = false;
}

extern "C" ACL_TOOL_INJECTION_EXPORT std::int32_t MsprofRegisterDataCallback(uint32_t type, void* callback)
{
    injection::detail::DebugLog("prof_api_stub", "MsprofRegisterDataCallback type=%u callback=%p", type, callback);
    if (callback == nullptr) {
        return -1;
    }
    g_rawDataCallback = reinterpret_cast<MsprofRawDataCallback>(callback);
    return 0;
}

extern "C" ACL_TOOL_INJECTION_EXPORT std::int32_t MsprofStart(uint32_t dataType, const void* data, uint32_t length)
{
    if (data == nullptr || length != sizeof(MsprofConfig)) {
        injection::detail::DebugLog(
            "prof_api_stub", "MsprofStart rejected type=%u data=%p length=%u", dataType, const_cast<void*>(data),
            length);
        return -1;
    }

    const auto* config = static_cast<const MsprofConfig*>(data);
    const std::size_t pmuCount = CountConfiguredPmus(*config);
    g_activeRawDataType = ResolveRawDataType(*config);
    g_collectBlock = false;
    for (std::size_t i = 0; config->configInfo.attrs != nullptr && i < config->configInfo.numAttrs; ++i) {
        if (config->configInfo.attrs[i].id == PROF_CONFIG_ATTR_TASK_BLOCK) {
            g_collectBlock = true;
        }
    }
    injection::detail::DebugLog(
        "prof_api_stub", "MsprofStart type=%u profSwitch=0x%llx pmuCount=%zu rawDataType=%d", dataType,
        static_cast<unsigned long long>(config->profSwitch), pmuCount, static_cast<int>(g_activeRawDataType));
    return 0;
}

extern "C" ACL_TOOL_INJECTION_EXPORT std::int32_t MsprofStop(uint32_t dataType, const void*, uint32_t length)
{
    injection::detail::DebugLog("prof_api_stub", "MsprofStop type=%u length=%u", dataType, length);

    MsprofRawData rawData{};
    rawData.isLastChunk = true;
    rawData.offset = 0;
    rawData.deviceId = 0;
    rawData.type = g_activeRawDataType;
    if (g_activeRawDataType == PMU_DATA_TYPE) {
        // Emit one structurally valid PMU record so the integration stub exercises
        // the same decoder path as the production data module.
        std::array<uint32_t, 32> record{};
        record[0] = 0x6bd3002aU;
        record[1] = (1U << 16U) | 1U;
        record[2] = 100U;
        record[5] = 1U << 8U;
        record[6] = 1U << 16U;
        rawData.chunkSize = sizeof(record);
        std::memcpy(rawData.chunk, record.data(), sizeof(record));
        if (g_collectBlock) {
            record[0] = 0x6bd30029U;
            std::memcpy(rawData.chunk + sizeof(record), record.data(), sizeof(record));
            rawData.chunkSize += sizeof(record);
        }
    } else {
        constexpr std::array<char, 4> kOpaqueData = {'a', 'c', 'l', 'p'};
        rawData.chunkSize = kOpaqueData.size();
        std::memcpy(rawData.chunk, kOpaqueData.data(), kOpaqueData.size());
    }
    if (g_rawDataCallback != nullptr) {
        g_rawDataCallback(&rawData);
    }
    return 0;
}
