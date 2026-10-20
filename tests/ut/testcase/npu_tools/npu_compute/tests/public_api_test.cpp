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

#include "aclpti/aclpti.h"
#include "aclpti/aclpti_data.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <type_traits>

static_assert(std::is_same_v<decltype(aclptiProfilingDataResult{}.status), aclptiResult>);
static_assert(
    std::is_same_v<decltype(aclptiProfilingDataResult{}.pipelineData), std::map<uint64_t, aclptiPipelineData>>);
static_assert(
    std::is_same_v<decltype(&aclptiRegisterProfilingDataCallback), aclptiResult (*)(aclptiProfilingDataCallback)>);
static_assert(std::is_same_v<
              decltype(&aclptiRegisterDataModuleShutdownCallback),
              aclptiResult (*)(aclptiDataModuleShutdownCallback, void*)>);
static_assert(ACLPTI_ERROR_NOT_INITIALIZED > ACLPTI_ERROR_INTERNAL);
static_assert(ACLPTI_ERROR_CSV_WRITE > ACLPTI_ERROR_NOT_INITIALIZED);
static_assert(ACLPTI_ERROR_RESULT_UNRELIABLE > ACLPTI_ERROR_CSV_WRITE);

std::int32_t MsprofStart(uint32_t, const void*, uint32_t) { return 0; }

std::int32_t MsprofStop(uint32_t, const void*, uint32_t) { return 0; }

std::int32_t MsprofRegisterDataCallback(uint32_t, void* function) { return function == nullptr ? -1 : 0; }

TEST(NpuComputePublicApi, RejectsInvalidSubscribers)
{
    ASSERT_TRUE(aclptiSubscribe(nullptr, nullptr, nullptr, nullptr) == ACLPTI_ERROR_INVALID_PARAMETER);
    ASSERT_TRUE(aclptiActivityEnable(nullptr, ACLPTI_ACTIVITY_KIND_FULL, nullptr) == ACLPTI_ERROR_INVALID_SUBSCRIBER);
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(nullptr) == ACLPTI_ERROR_INVALID_PARAMETER);
    aclptiSubscribeParams invalid_params{1};
    aclptiSubscribeHandle invalid_subscriber = nullptr;
    ASSERT_TRUE(
        aclptiSubscribe(&invalid_subscriber, nullptr, nullptr, &invalid_params) == ACLPTI_ERROR_INVALID_PARAMETER);
}

TEST(NpuComputePublicApi, ReusesProcessSubscriber)
{
    aclptiSubscribeHandle first = nullptr;
    aclptiSubscribeHandle second = nullptr;
    ASSERT_TRUE(aclptiSubscribe(&first, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(first != nullptr);
    ASSERT_TRUE(aclptiSubscribe(&second, nullptr, nullptr, nullptr) == ACLPTI_SUCCESS);
    ASSERT_TRUE(second == first);
}

TEST(NpuComputePublicApi, ValidatesProfilingConfigurations)
{
    aclptiSubscribeHandle subscriber = nullptr;
    ASSERT_EQ(aclptiSubscribe(&subscriber, nullptr, nullptr, nullptr), ACLPTI_SUCCESS);
    aclptiRangeProfilerSetConfigParams empty{};
    empty.sections = nullptr;
    empty.numSections = 0;
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&empty) == ACLPTI_ERROR_INVALID_PARAMETER);

    aclptiRangeProfilerSetConfigParams pipeline_only{nullptr, 0, ACLPTI_BLOCK_RESULT_DISABLED, true, false};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&pipeline_only) == ACLPTI_SUCCESS);

    aclptiRangeProfilerSetConfigParams pc_sampling_only{nullptr, 0, ACLPTI_BLOCK_RESULT_DISABLED, false, true};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&pc_sampling_only) == ACLPTI_SUCCESS);

    aclptiRangeProfilerSetConfigParams instruction_collection{nullptr, 0, ACLPTI_BLOCK_RESULT_ALL, true, true};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&instruction_collection) == ACLPTI_SUCCESS);

    aclptiRangeProfilerSetConfigParams block_only{nullptr, 0, ACLPTI_BLOCK_RESULT_ALL, false, false};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&block_only) == ACLPTI_ERROR_INVALID_PARAMETER);

    aclptiRangeProfilerSetConfigParams invalid_block{nullptr, 0, static_cast<aclptiBlockResultMode>(3), true, false};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&invalid_block) == ACLPTI_ERROR_INVALID_PARAMETER);

    const char* unknown_sections[] = {"UnknownMetric"};
    aclptiRangeProfilerSetConfigParams unknown{unknown_sections, 1};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&unknown) == ACLPTI_ERROR_NOT_SUPPORTED);

    const char* unsupported_sections[] = {"HardwareInfo"};
    aclptiRangeProfilerSetConfigParams unsupported{unsupported_sections, 1};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&unsupported) == ACLPTI_ERROR_NOT_SUPPORTED);

    const char* null_sections[] = {nullptr};
    aclptiRangeProfilerSetConfigParams null_section{null_sections, 1};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&null_section) == ACLPTI_ERROR_INVALID_PARAMETER);

    const char* empty_name_sections[] = {""};
    aclptiRangeProfilerSetConfigParams empty_name{empty_name_sections, 1};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&empty_name) == ACLPTI_ERROR_INVALID_PARAMETER);

    constexpr std::array<const char*, 7> supported_sections = {
        "ArithmeticUtilization",
        "PipeUtilization",
        "ResourceConflictRatio",
        "Memory",
        "MemoryL0",
        "MemoryUB",
        "L2Cache",
    };
    aclptiRangeProfilerSetConfigParams all_supported{supported_sections.data(), supported_sections.size()};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&all_supported) == ACLPTI_SUCCESS);

    for (const char* section : supported_sections) {
        const char* single_section[] = {section};
        aclptiRangeProfilerSetConfigParams single{single_section, 1};
        ASSERT_TRUE(aclptiRangeProfilerSetConfig(&single) == ACLPTI_SUCCESS);
    }

    const char* valid_sections[] = {"PipeUtilization", "Memory", "PipeUtilization"};
    aclptiRangeProfilerSetConfigParams valid{valid_sections, 3};
    ASSERT_TRUE(aclptiRangeProfilerSetConfig(&valid) == ACLPTI_SUCCESS);
}
