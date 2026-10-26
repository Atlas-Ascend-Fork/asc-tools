/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "performance_hints/performance_hint_analyzer.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using namespace npucompute;
class PerformanceHintAnalyzer : public ::testing::Test {
protected:
    HintSections sections;
    aclptiProfilingDataResult result;
    KernelMetadata metadata;
    ReportConfig config;
    void SetUp() override
    {
        metadata.kind = KernelKind::Mix;
        config.aicFrequencyMhz = config.aivFrequencyMhz = 1000;
        for (const auto* field :
             {"aic_cube_ratio", "aic_scalar_ratio", "aic_mte1_ratio", "aic_mte2_ratio", "aic_mte3_ratio",
              "aic_fixpipe_ratio", "aiv_vec_ratio", "aiv_scalar_ratio", "aiv_mte2_ratio", "aiv_mte3_ratio",
              "aic_icache_miss_rate", "aiv_icache_miss_rate"}) {
            Set(field, .10);
        }
        sections["Memory"]["GM_to_UB_datas(KB)"].value = 2048;
        sections["Memory"]["aiv_mte2_instructions"].value = 128;
    }
    void Set(const std::string& field, double value) { sections["PipeUtilization"][field].value = value; }
    HintAnalysis Analyze() { return AnalyzePerformanceHints(result, sections, config, metadata); }
    bool Has(const std::string& token)
    {
        const auto analysis = Analyze();
        return std::any_of(analysis.hints.begin(), analysis.hints.end(), [&](const auto& text) {
            return text.find(token) != std::string::npos;
        });
    }
    void Block(uint16_t block, uint16_t role, aclptiCoreType core, double time, uint8_t physical = 0)
    {
        aclptiPmuDataRow row{};
        row.coreType = core;
        row.totalCycles = time * 1000;
        result.pmuLogs.emplace(aclptiBlockKey{block, role, core, physical}, row);
    }
};
TEST_F(PerformanceHintAnalyzer, VectorThresholdMaximumTiesAndCompleteSet)
{
    Set("aiv_vec_ratio", .50);
    EXPECT_FALSE(Has("PH01"));
    Set("aiv_vec_ratio", .51);
    EXPECT_TRUE(Has("PH01"));
    Set("aiv_mte2_ratio", .60);
    EXPECT_FALSE(Has("PH01"));
    Set("aiv_vec_ratio", .60);
    EXPECT_TRUE(Has("PH01"));
    sections["PipeUtilization"]["aiv_mte3_ratio"].value.reset();
    EXPECT_FALSE(Has("PH01"));
    EXPECT_FALSE(Has("[PH03][AIV]"));
}
TEST_F(PerformanceHintAnalyzer, ScalarAndIcacheStrictBoundariesAndIndependentCores)
{
    for (const auto core : {"aic_", "aiv_"}) {
        Set(std::string(core) + "scalar_ratio", .30);
        Set(std::string(core) + "icache_miss_rate", .15);
    }
    EXPECT_FALSE(Has("PH02"));
    EXPECT_FALSE(Has("PH05"));
    Set("aic_scalar_ratio", .45);
    Set("aiv_scalar_ratio", .40);
    Set("aic_icache_miss_rate", .16);
    Set("aiv_icache_miss_rate", .20);
    EXPECT_TRUE(Has("[PH02][AIC]"));
    EXPECT_TRUE(Has("[PH02][AIV]"));
    EXPECT_TRUE(Has("[PH05][AIC]"));
    EXPECT_TRUE(Has("[PH05][AIV]"));
    Set("aic_scalar_ratio", std::numeric_limits<double>::quiet_NaN());
    EXPECT_FALSE(Has("[PH02][AIC]"));
    EXPECT_TRUE(Has("[PH02][AIV]"));
}
TEST_F(PerformanceHintAnalyzer, Mte2UsesEachCompletePipelineSet)
{
    Set("aic_mte2_ratio", .50);
    EXPECT_FALSE(Has("[PH03][AIC]"));
    Set("aic_mte2_ratio", .60);
    Set("aic_cube_ratio", .60);
    Set("aiv_mte2_ratio", .70);
    EXPECT_TRUE(Has("[PH03][AIC]"));
    EXPECT_TRUE(Has("[PH03][AIV]"));
    Set("aic_fixpipe_ratio", .80);
    EXPECT_FALSE(Has("[PH03][AIC]"));
    sections["PipeUtilization"].erase("aic_fixpipe_ratio");
    EXPECT_FALSE(Has("[PH03][AIC]"));
    EXPECT_TRUE(Has("[PH03][AIV]"));
}
TEST_F(PerformanceHintAnalyzer, TransferUsesSummaryOperandsAndRejectsInvalidDenominator)
{
    EXPECT_FALSE(Has("PH04"));
    sections["Memory"]["GM_to_UB_datas(KB)"].value = 1024;
    EXPECT_TRUE(Has("为 8 KB/指令"));
    sections["Memory"]["GM_to_UB_datas(KB)"].value = 2047;
    EXPECT_TRUE(Has("PH04"));
    for (double count : {0., -1., std::numeric_limits<double>::infinity()}) {
        sections["Memory"]["aiv_mte2_instructions"].value = count;
        EXPECT_FALSE(Has("PH04"));
    }
}
TEST_F(PerformanceHintAnalyzer, ImbalanceGroupsRolesAndCoresAndUsesMaximum)
{
    Block(0, 0, ACLPTI_CORE_TYPE_AIV, 80);
    Block(1, 0, ACLPTI_CORE_TYPE_AIV, 100);
    Block(0, 1, ACLPTI_CORE_TYPE_AIV, 190);
    Block(1, 1, ACLPTI_CORE_TYPE_AIV, 200);
    Block(0, 0, ACLPTI_CORE_TYPE_AIC, 90);
    Block(1, 0, ACLPTI_CORE_TYPE_AIC, 100);
    const auto analysis = Analyze();
    ASSERT_EQ(analysis.hints.size(), 2U);
    EXPECT_NE(analysis.hints[0].find("[PH06][AIC]"), std::string::npos);
    EXPECT_NE(analysis.hints[0].find("的 10%"), std::string::npos);
    EXPECT_NE(analysis.hints[1].find("[PH06][AIV]"), std::string::npos);
    EXPECT_NE(analysis.hints[1].find("的 20%"), std::string::npos);
}
TEST_F(PerformanceHintAnalyzer, ImbalanceBoundaryInvalidSamplesAndReplayIdentity)
{
    Block(0, 0, ACLPTI_CORE_TYPE_AIV, 90.1);
    Block(1, 0, ACLPTI_CORE_TYPE_AIV, 100);
    EXPECT_FALSE(Has("PH06"));
    result.pmuLogs.clear();
    Block(0, 0, ACLPTI_CORE_TYPE_AIV, 90);
    Block(1, 0, ACLPTI_CORE_TYPE_AIV, 100);
    Block(1, 0, ACLPTI_CORE_TYPE_AIV, 100, 2);
    Block(2, 0, ACLPTI_CORE_TYPE_AIV, 0);
    Block(3, 0, ACLPTI_CORE_TYPE_AIV, -10);
    EXPECT_TRUE(Has("PH06"));
    Block(0, 0, ACLPTI_CORE_TYPE_AIV, 80, 2); // conflicting replay evidence is excluded
    EXPECT_FALSE(Has("PH06"));
    result.pmuLogs.clear();
    Block(0, 0, ACLPTI_CORE_TYPE_AIV, 50);
    Block(0, 0, ACLPTI_CORE_TYPE_AIV, 50, 2); // duplicate cannot create a second sample
    EXPECT_FALSE(Has("PH06"));
}
TEST_F(PerformanceHintAnalyzer, SectionsKindAndInvalidInputSkipOnlyAffectedRules)
{
    Set("aiv_vec_ratio", .80);
    Set("aic_scalar_ratio", .40);
    EXPECT_TRUE(Has("PH01"));
    metadata.kind = KernelKind::Cube;
    EXPECT_FALSE(Has("PH01"));
    EXPECT_TRUE(Has("[PH02][AIC]"));
    metadata.kind = KernelKind::Mix;
    Set("aiv_vec_ratio", 1.5);
    EXPECT_FALSE(Has("PH01"));
    EXPECT_TRUE(Has("[PH02][AIC]"));
    sections.clear();
    const auto analysis = Analyze();
    EXPECT_TRUE(analysis.hints.empty());
    EXPECT_EQ(analysis.evaluations.front().skipReason, "section_not_selected: PipeUtilization");
}
TEST_F(PerformanceHintAnalyzer, MissingSectionIdentifiesDependencyAndPreservesOtherHints)
{
    sections.erase("Memory");
    Set("aiv_scalar_ratio", .40);
    const auto analysis = Analyze();
    ASSERT_EQ(analysis.hints.size(), 1U);
    EXPECT_NE(analysis.hints.front().find("[PH02][AIV]"), std::string::npos);
    const auto transfer = std::find_if(
        analysis.evaluations.begin(), analysis.evaluations.end(), [](const auto& item) { return item.id == "PH04"; });
    ASSERT_NE(transfer, analysis.evaluations.end());
    EXPECT_EQ(transfer->sections, std::vector<std::string>{"Memory"});
    EXPECT_EQ(transfer->skipReason, "section_not_selected: Memory");
}
TEST_F(PerformanceHintAnalyzer, InvalidFieldIdentifiesItsSourceSection)
{
    Set("aiv_scalar_ratio", std::numeric_limits<double>::quiet_NaN());
    const auto analysis = Analyze();
    const auto scalar = std::find_if(analysis.evaluations.begin(), analysis.evaluations.end(), [](const auto& item) {
        return item.id == "PH02" && item.coreType == ACLPTI_CORE_TYPE_AIV;
    });
    ASSERT_NE(scalar, analysis.evaluations.end());
    EXPECT_EQ(scalar->sections, std::vector<std::string>{"PipeUtilization"});
    EXPECT_EQ(scalar->skipReason, "invalid_or_missing_field: PipeUtilization.aiv_scalar_ratio");
}
TEST_F(PerformanceHintAnalyzer, InvalidSummaryFieldDoesNotSuppressUnrelatedRule)
{
    Set("aiv_vec_ratio", .80);
    Set("aiv_icache_miss_rate", std::numeric_limits<double>::quiet_NaN());
    EXPECT_FALSE(Has("PH05"));
    EXPECT_TRUE(Has("PH01"));
}
TEST_F(PerformanceHintAnalyzer, StableRuleOrderAndThresholdAdjacentDisplay)
{
    Set("aiv_vec_ratio", .50000000000001);
    Set("aic_scalar_ratio", .40);
    Set("aiv_scalar_ratio", .40);
    Set("aiv_icache_miss_rate", .16);
    const auto analysis = Analyze();
    ASSERT_EQ(analysis.hints.size(), 4U);
    EXPECT_NE(analysis.hints[0].find("PH01"), std::string::npos);
    EXPECT_EQ(analysis.hints[0].find("为 50%，"), std::string::npos);
    EXPECT_NE(analysis.hints[1].find("[PH02][AIC]"), std::string::npos);
    EXPECT_NE(analysis.hints[2].find("[PH02][AIV]"), std::string::npos);
    EXPECT_NE(analysis.hints[3].find("PH05"), std::string::npos);
}
TEST_F(PerformanceHintAnalyzer, RawTaskCountersDoNotOverrideSummaryValues)
{
    Set("aic_mte2_ratio", .80);
    Set("aic_scalar_ratio", .40);
    Set("aic_icache_miss_rate", .20);
    Set("aiv_vec_ratio", .80);
    Set("aiv_mte2_ratio", .80);
    Set("aiv_scalar_ratio", .40);
    Set("aiv_icache_miss_rate", .20);
    sections["Memory"]["GM_to_UB_datas(KB)"].value = 1024;
    const auto expected = Analyze().hints;
    ASSERT_EQ(expected.size(), 8U);
    aclptiTaskPmuCoreData invalid;
    invalid.totalCycles = std::numeric_limits<double>::quiet_NaN();
    invalid.overflow = true;
    invalid.values = {{0x1, -100}, {0x501, 1500}};
    result.taskPmuData.aic = invalid;
    result.taskPmuData.aiv = invalid;
    EXPECT_EQ(Analyze().hints, expected);
    Set("aiv_vec_ratio", .20);
    EXPECT_FALSE(Has("PH01"));
    EXPECT_TRUE(Has("[PH03][AIV]"));
}
TEST_F(PerformanceHintAnalyzer, ImbalanceExactBoundaryIsIndependentOfFrequency)
{
    for (const auto frequency : {1800., 1650., 1234., 1000., 1.e-100, 1.e100}) {
        config.aicFrequencyMhz = config.aivFrequencyMhz = frequency;
        for (const auto core : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
            SCOPED_TRACE(::testing::Message() << "frequency=" << frequency << " core=" << core);
            result.pmuLogs.clear();
            Block(0, 0, core, 9);
            Block(1, 0, core, 10);
            EXPECT_TRUE(Has("PH06"));
            EXPECT_TRUE(Has("的 10%"));
            result.pmuLogs.begin()->second.totalCycles = std::nextafter(9000., 10000.);
            EXPECT_FALSE(Has("PH06"));
        }
    }
}
TEST_F(PerformanceHintAnalyzer, ImbalanceStillRequiresValidFrequency)
{
    Block(0, 0, ACLPTI_CORE_TYPE_AIV, 9);
    Block(1, 0, ACLPTI_CORE_TYPE_AIV, 10);
    for (const auto frequency :
         {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        config.aivFrequencyMhz = frequency;
        EXPECT_FALSE(Has("PH06"));
    }
}
} // namespace
