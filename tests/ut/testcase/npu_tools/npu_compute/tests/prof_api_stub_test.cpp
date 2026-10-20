/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "profiling/prof_api.h"
#include "injection/prof_api_stub.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {

int g_callbackCount = 0;
MsprofRawData g_rawData{};

std::int32_t CaptureRawData(MsprofRawData* rawData)
{
    if (rawData == nullptr) {
        return -1;
    }
    ++g_callbackCount;
    g_rawData = *rawData;
    return 0;
}

} // namespace

TEST(NpuComputeProfApiStub, EmitsConfiguredDataAndClearsCallbackOnReset)
{
    ProfApiReset();
    g_callbackCount = 0;
    g_rawData = {};
    MsprofConfig defaultConfig{};
    ASSERT_TRUE(MsprofStart(8, &defaultConfig, sizeof(defaultConfig)) == 0);

    MsprofConfigAttr instrModeAttr{};
    instrModeAttr.id = PROF_CONFIG_ATTR_INSTR;
    MsprofConfig instrModeConfig{};
    instrModeConfig.configInfo.attrs = &instrModeAttr;
    instrModeConfig.configInfo.numAttrs = 1;
    ASSERT_TRUE(MsprofStart(8, &instrModeConfig, sizeof(instrModeConfig)) == 0);

    MsprofConfigAttr attr{};
    attr.id = PROF_CONFIG_ATTR_AICORE_METRICS;
    std::fill(std::begin(attr.value.aicoreMetrics), std::end(attr.value.aicoreMetrics), MSPROF_INVALID_AICORE_METRIC);
    attr.value.aicoreMetrics[0] = 0;

    MsprofConfig config{};
    config.profSwitch = PROF_AICORE_METRICS_MASK;
    config.configInfo.attrs = &attr;
    config.configInfo.numAttrs = 1;

    ASSERT_TRUE(MsprofRegisterDataCallback(8, reinterpret_cast<void*>(&CaptureRawData)) == 0);

    instrModeAttr.value.instrMode = PROF_COMPUTE_BIU_PERF;
    ASSERT_TRUE(MsprofStart(8, &instrModeConfig, sizeof(instrModeConfig)) == 0);
    ASSERT_TRUE(MsprofStop(8, &instrModeConfig, sizeof(instrModeConfig)) == 0);
    ASSERT_TRUE(g_callbackCount == 1);
    ASSERT_TRUE(g_rawData.type == BIU_PERF_DATA_TYPE);

    instrModeAttr.value.instrMode = PROF_COMPUTE_PC_SAMPLING;
    ASSERT_TRUE(MsprofStart(8, &instrModeConfig, sizeof(instrModeConfig)) == 0);
    ASSERT_TRUE(MsprofStop(8, &instrModeConfig, sizeof(instrModeConfig)) == 0);
    ASSERT_TRUE(g_callbackCount == 2);
    ASSERT_TRUE(g_rawData.type == PC_SAMPLING_DATA_TYPE);

    ASSERT_TRUE(MsprofStart(8, &config, sizeof(config)) == 0);
    ASSERT_TRUE(MsprofStop(8, &config, sizeof(config)) == 0);
    ASSERT_TRUE(g_callbackCount == 3);
    ASSERT_TRUE(g_rawData.isLastChunk);
    ASSERT_TRUE(g_rawData.type == PMU_DATA_TYPE);
    ASSERT_TRUE(g_rawData.deviceId == 0);
    ASSERT_TRUE(g_rawData.offset == 0);
    ASSERT_TRUE(g_rawData.chunkModule == 0);
    ASSERT_TRUE(g_rawData.chunkSize == 128);
    uint32_t magic = 0;
    std::memcpy(&magic, g_rawData.chunk, sizeof(magic));
    ASSERT_TRUE(magic == 0x6bd3002aU);
    MsprofConfigAttr blockAttr{};
    blockAttr.id = PROF_CONFIG_ATTR_TASK_BLOCK;
    blockAttr.value.taskBlockMode = PROF_COMPUTE_ALL_BLOCK;
    config.configInfo.attrs = &blockAttr;
    ASSERT_TRUE(MsprofStart(8, &config, sizeof(config)) == 0);
    ASSERT_TRUE(MsprofStop(8, &config, sizeof(config)) == 0);
    ASSERT_TRUE(g_callbackCount == 4);
    ASSERT_TRUE(g_rawData.chunkSize == 256);
    std::memcpy(&magic, g_rawData.chunk + 128, sizeof(magic));
    ASSERT_TRUE(magic == 0x6bd30029U);
    ProfApiReset();
    ASSERT_TRUE(MsprofStart(8, &config, sizeof(config)) == 0);
    ASSERT_TRUE(MsprofStop(8, &config, sizeof(config)) == 0);
    ASSERT_TRUE(g_callbackCount == 4);
}
