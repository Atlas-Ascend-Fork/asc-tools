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
#include "acl_pti/profiling/range_profiler.h"
#include "acl_pti/profiling/binary_registry.h"
#include "runtime/section_config.h"
#include "injection/runtime_stub_api.h"
#include <algorithm>
namespace {
unsigned starts = 0;
unsigned pmuStarts = 0;
unsigned launches = 0;
int Device(int* device)
{
    *device = 0;
    return 0;
}
int Sync(aclrtStream) { return 0; }
} // namespace
extern "C" int32_t MsprofStart(uint32_t, const void* config, uint32_t)
{
    const auto& settings = *static_cast<const MsprofConfig*>(config);
    if (settings.configInfo.attrs[0].id == PROF_CONFIG_ATTR_INSTR) {
        EXPECT_EQ(settings.profSwitch, PROF_TASK_TIME_MASK | PROF_INSTR_MASK);
        EXPECT_EQ(settings.configInfo.attrs[0].value.instrMode, PROF_COMPUTE_BIU_PERF);
        EXPECT_EQ(settings.configInfo.numAttrs, 1);
    } else {
        EXPECT_EQ(settings.profSwitch, PROF_TASK_TIME_MASK | PROF_AICORE_METRICS_MASK);
        EXPECT_EQ(settings.configInfo.numAttrs, 2);
        EXPECT_EQ(settings.configInfo.attrs[1].id, PROF_CONFIG_ATTR_TASK_BLOCK);
        EXPECT_EQ(settings.configInfo.attrs[1].value.taskBlockMode, PROF_COMPUTE_ALL_BLOCK);
        ++pmuStarts;
    }
    ++starts;
    return 0;
}
extern "C" int32_t MsprofStop(uint32_t, const void*, uint32_t) { return 0; }
extern "C" int32_t MsprofRegisterDataCallback(uint32_t, void*) { return 0; }

TEST(NpuComputeOccupancyReplay, MetricAndSamplingCollectionsIncludeBlockPmu)
{
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtGetDevice", &Device), 0);
    ASSERT_EQ(RuntimeStubSetOriginFunction("aclrtSynchronizeStream", &Sync), 0);
    for (const auto& sections :
         {std::vector<std::string>{"Memory"},
          {"PipeUtilization"},
          {"MemoryL0"},
          {"MemoryUB"},
          {"L2Cache"},
          {"ArithmeticUtilization"},
          {"ResourceConflictRatio"},
          {"PmSampling"},
          {"Pipeline"},
          {"Roofline"},
          {"Pipeline", "PmSampling"},
          {"PipeUtilization", "Memory", "MemoryL0", "MemoryUB", "L2Cache", "Pipeline", "ArithmeticUtilization",
           "ResourceConflictRatio", "PmSampling", "Roofline"}}) {
        SCOPED_TRACE(::testing::PrintToString(sections));
        const bool pipeline = std::find(sections.begin(), sections.end(), "Pipeline") != sections.end();
        const bool roofline = std::find(sections.begin(), sections.end(), "Roofline") != sections.end();
        npucompute::SectionConfig config;
        std::string error;
        ASSERT_TRUE(config.Load(sections, &error)) << error;
        if (sections.front() == "Pipeline" || sections.front() == "PmSampling") {
            EXPECT_EQ(config.Params()->numSections, 0U);
        }
        aclpti::profiling::RangeProfiler profiler;
        ASSERT_EQ(profiler.SetConfig(config.Params()), ACLPTI_SUCCESS);
        auto& manager = aclpti::data::ProfilingDataManager::Instance();
        std::shared_ptr<const aclptiProfilingDataResult> result;
        ASSERT_EQ(
            manager.Initialize([&result](auto value) {
                result = std::move(value);
                return ACLPTI_SUCCESS;
            }),
            ACLPTI_SUCCESS);
        ASSERT_EQ(profiler.Initialize(), ACLPTI_SUCCESS);
        aclpti::profiling::BinaryRegistry registry;
        aclpti::profiling::ReplayMemory memory;
        starts = 0;
        pmuStarts = 0;
        launches = 0;
        int original;
        ASSERT_EQ(
            profiler.ReplayKernel(
                memory, registry, &original,
                [&original](aclrtFuncHandle h, bool instrumented) {
                    EXPECT_EQ(h, &original);
                    EXPECT_FALSE(instrumented);
                    ++launches;
                    return ACL_SUCCESS;
                },
                nullptr),
            pipeline ? ACLPTI_ERROR_RESULT_UNRELIABLE : ACLPTI_SUCCESS);
        ASSERT_EQ(profiler.Shutdown(), ACLPTI_SUCCESS);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(pmuStarts, launches);
        EXPECT_GE(pmuStarts, 1U);
        if (roofline) {
            ASSERT_GE(result->replayData.size(), 2U);
            auto replay = result->replayData.begin();
            EXPECT_EQ(replay++->second.kind, aclptiReplayKind::RooflinePmu);
            EXPECT_EQ(replay->second.kind, aclptiReplayKind::Pmu);
        }
        EXPECT_EQ(starts, pmuStarts + (pipeline ? 1U : 0U));
    }
}
