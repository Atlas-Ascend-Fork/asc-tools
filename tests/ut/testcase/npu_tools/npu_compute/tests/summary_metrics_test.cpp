/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "artifact_sink.h"
#include "report/report_writer.h"
#include <boost/json/src.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace {
using namespace npucompute;
class MetricsSink final : public ArtifactSink {
public:
    void Begin(const std::string&) override { content.clear(); }
    void Write(std::string_view bytes) override { content.append(bytes); }
    void Commit(uint64_t count) override { rows = count; }
    void Abort() noexcept override {}
    std::string content;
    uint64_t rows = 0;
};
struct SummaryOutput {
    std::vector<boost::json::object> records;
    std::string text;
    uint64_t rows;
};
SummaryOutput Render(
    const aclptiProfilingDataResult& result, const KernelMetadata& metadata,
    const std::vector<std::string>& sections = {}, const ReportConfig& config = {})
{
    MetricsSink sink;
    EXPECT_EQ(WriteSummaryJsonl(result, sections, config, metadata, &sink), ACLPTI_SUCCESS);
    SummaryOutput out{{}, sink.content, sink.rows};
    std::istringstream stream(sink.content);
    std::string line;
    while (std::getline(stream, line)) {
        out.records.push_back(boost::json::parse(line).as_object());
    }
    return out;
}
KernelMetadata Metadata(KernelKind kind, uint64_t blocks, uint64_t mixed = 0)
{
    KernelMetadata metadata;
    metadata.kind = kind;
    metadata.blockDim = blocks;
    metadata.mixBlockDim = mixed;
    metadata.hardware.collected = true;
    metadata.hardware.socName = "950X";
    metadata.hardware.currentAicFrequencyMhz = 1000;
    metadata.hardware.currentAivFrequencyMhz = 1000;
    metadata.hardware.cubeCoreCount = 24;
    metadata.hardware.vectorCoreCount = 48;
    return metadata;
}
void Duration(aclptiProfilingDataResult& result, double us = 10)
{
    result.taskLogs[1] = {
        {0, 0, 1, 0, 100, 0, 0, ACLPTI_CORE_TYPE_AIC, 0},
        {0, 1, 1, 0, 100 + static_cast<uint64_t>(us * 1000), 0, 0, ACLPTI_CORE_TYPE_AIC, 0}};
}
void Row(
    aclptiProfilingDataResult& result, uint16_t block, uint16_t sub, aclptiCoreType side, double cycles,
    uint8_t physical = 0)
{
    aclptiPmuDataRow row{};
    row.coreType = side;
    row.totalCycles = cycles;
    result.pmuLogs.emplace(aclptiBlockKey{block, sub, side, physical}, row);
}
void Near(const boost::json::object& object, const char* name, double expected)
{
    const auto& value = object.at(name);
    ASSERT_TRUE(value.is_number()) << name << ": " << value;
    EXPECT_NEAR(value.to_number<double>(), expected, 0.000001) << name;
}
void Na(const boost::json::object& object, const char* name)
{
    ASSERT_TRUE(object.at(name).is_string()) << name << ": " << object.at(name);
    EXPECT_EQ(object.at(name).as_string(), "NA");
}
TEST(SummaryMetrics, MixUsesEveryLogicalObjectAndTaskDuration)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 6000);
    Row(result, 0, 1, ACLPTI_CORE_TYPE_AIV, 4000);
    const auto info = Render(result, Metadata(KernelKind::Mix, 1, 2)).records.back();
    Near(info, "aicore_parallel_utilization", 60);
    Near(info, "aicore_parallel_balance", 75);
}
TEST(SummaryMetrics, VectorBalanceAndUtilizationArePercentages)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 2000);
    Row(result, 1, 0, ACLPTI_CORE_TYPE_AIV, 4000);
    const auto info = Render(result, Metadata(KernelKind::Vector, 2)).records.back();
    Near(info, "aicore_parallel_utilization", 30);
    Near(info, "aicore_parallel_balance", 75);
    result.taskLogs.clear();
    const auto noDuration = Render(result, Metadata(KernelKind::Vector, 2)).records.back();
    Na(noDuration, "aicore_parallel_utilization");
    Near(noDuration, "aicore_parallel_balance", 75);
}
TEST(SummaryMetrics, MixedFourObjectBalanceUsesMeanOverMaximum)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    Row(result, 1, 0, ACLPTI_CORE_TYPE_AIC, 4000);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 6000);
    Row(result, 1, 0, ACLPTI_CORE_TYPE_AIV, 2000);
    Near(Render(result, Metadata(KernelKind::Mix, 2, 2)).records.back(), "aicore_parallel_balance", 62.5);
}
TEST(SummaryMetrics, CompletenessCannotComeFromObservedOrHardwareCounts)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 4000);
    auto metadata = Metadata(KernelKind::Vector, 2);
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
    metadata.blockDim.reset();
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
    metadata.blockDim = 1;
    metadata.kind.reset();
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
}
TEST(SummaryMetrics, MixRejectsMissingSidesAndInvalidSubblocks)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 6000);
    auto metadata = Metadata(KernelKind::Mix, 1, 2);
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
    Row(result, 0, 2, ACLPTI_CORE_TYPE_AIV, 4000);
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
    result.pmuLogs.clear();
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 6000);
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
}
TEST(SummaryMetrics, RejectsUnsupportedMixedVectorRatio)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 6000);
    Row(result, 0, 1, ACLPTI_CORE_TYPE_AIV, 4000);
    Row(result, 0, 2, ACLPTI_CORE_TYPE_AIV, 2000);
    Na(Render(result, Metadata(KernelKind::Mix, 1, 3)).records.back(), "aicore_parallel_balance");
}
TEST(SummaryMetrics, PhysicalReplaysDoNotAddObjectsAndConflictsInvalidate)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 2000);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 2000, 7);
    Row(result, 1, 0, ACLPTI_CORE_TYPE_AIV, 4000);
    auto metadata = Metadata(KernelKind::Vector, 2);
    Near(Render(result, metadata).records.back(), "aicore_parallel_utilization", 30);
    result.pmuLogs.at({0, 0, ACLPTI_CORE_TYPE_AIV, 7}).totalCycles = 3000;
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
}
TEST(SummaryMetrics, LegacyFrequencyRequiresExplicitSideOrRatedMetadata)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    KernelMetadata metadata;
    metadata.kind = KernelKind::Cube;
    metadata.blockDim = 1;
    const auto unavailable = Render(result, metadata, {"ArithmeticUtilization"}).records.back();
    Na(unavailable, "Current Freq");
    Na(unavailable, "aicore_parallel_utilization");
    Na(unavailable, "aicore_parallel_balance");
    ReportConfig config;
    config.frequencyMhz = 1700;
    Na(Render(result, metadata, {}, config).records.back(), "Current Freq");
    config.aicFrequencyMhz = 1000;
    const auto explicitSide = Render(result, metadata, {}, config).records.back();
    Near(explicitSide, "Current Freq", 1000);
    Near(explicitSide, "aicore_parallel_utilization", 80);
    metadata.ratedAicFrequencyMhz = 2000;
    const auto rated = Render(result, metadata).records.back();
    Near(rated, "Current Freq", 2000);
    Near(rated, "aicore_parallel_utilization", 40);
}
TEST(SummaryMetrics, ZeroAndInvalidFrequenciesKeepAvailabilityDistinct)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 0);
    auto metadata = Metadata(KernelKind::Cube, 1);
    auto info = Render(result, metadata).records.back();
    Near(info, "aicore_parallel_utilization", 0);
    Na(info, "aicore_parallel_balance");
    for (double frequency :
         {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        metadata.hardware.currentAicFrequencyMhz = frequency;
        Na(Render(result, metadata).records.back(), "aicore_parallel_utilization");
    }
    metadata.hardware.ratedAicFrequencyMhz = 1000;
    Near(Render(result, metadata).records.back(), "aicore_parallel_utilization", 0);
}
TEST(SummaryMetrics, FlopsUseTaskMeansWithoutLaunchDimensionAndIgnoreBlockRows)
{
    aclptiProfilingDataResult result;
    Duration(result);
    aclptiPmuDataRow aic{};
    aic.coreType = ACLPTI_CORE_TYPE_AIC;
    aic.totalCycles = 1000;
    aic.values = {{808, 250}, {810, 900}};
    result.taskPmuLogs.emplace(aclptiBlockKey{}, aic);
    aic.totalCycles = 3000;
    aic.values[808] = 2250;
    result.taskPmuLogs.emplace(aclptiBlockKey{1, 0, ACLPTI_CORE_TYPE_AIC, 0}, aic);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 99999);
    result.pmuLogs.begin()->second.values[808] = 99999;
    auto metadata = Metadata(KernelKind::Cube, 100);
    auto info = Render(result, metadata).records.back();
    Near(info, "aic_flops", 0.8192);
    Na(info, "aiv_flops");
    metadata.blockDim = 1;
    Near(Render(result, metadata).records.back(), "aic_flops", 0.8192);
    result.taskPmuLogs.begin()->second.values.clear();
    Na(Render(result, metadata).records.back(), "aic_flops");
    result.taskPmuLogs.clear();
    Na(Render(result, metadata).records.back(), "aic_flops");
}
TEST(SummaryMetrics, FlopsKeepZeroAndRejectInvalidSamples)
{
    aclptiProfilingDataResult result;
    Duration(result);
    aclptiPmuDataRow row{};
    row.coreType = ACLPTI_CORE_TYPE_AIV;
    row.totalCycles = 1000;
    row.values = {{1281, 0}};
    result.taskPmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, row);
    auto metadata = Metadata(KernelKind::Vector, 1);
    Near(Render(result, metadata).records.back(), "aiv_flops", 0);
    Na(Render(result, metadata).records.back(), "aic_flops");
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        result.taskPmuLogs.begin()->second.values[1281] = invalid;
        Na(Render(result, metadata).records.back(), "aiv_flops");
    }
}
TEST(SummaryMetrics, EstimatesAndRatiosUseRawTaskValuesWithoutFrequency)
{
    aclptiProfilingDataResult result;
    Duration(result);
    aclptiPmuDataRow row{};
    row.coreType = ACLPTI_CORE_TYPE_AIC;
    row.totalCycles = 3000000;
    row.values = {{808, 1000000}, {810, 2000000}};
    result.taskPmuLogs.emplace(aclptiBlockKey{}, row);
    auto metadata = Metadata(KernelKind::Cube, 1);
    metadata.hardware.currentAicFrequencyMhz.reset();
    metadata.hardware.currentAivFrequencyMhz.reset();
    const auto output = Render(result, metadata, {"ArithmeticUtilization"});
    Near(output.records.back(), "aic_flops", 819.2);
    Na(output.records[0], "aic_time(us)");
    Near(output.records[0], "aic_cube_fp_ratio", 0.333333);
    EXPECT_TRUE(output.records[0].at("aic_total_cycles").is_int64());
    result.taskLogs.clear();
    Na(Render(result, metadata).records.back(), "aic_flops");
}
TEST(SummaryMetrics, TheoreticalFlopsUseDeviceCountsAndEffectiveFrequency)
{
    auto metadata = Metadata(KernelKind::Vector, 1);
    metadata.hardware.currentAicFrequencyMhz = 2475;
    metadata.hardware.currentAivFrequencyMhz = 2475;
    auto output = Render({}, metadata);
    Near(output.records.back(), "aic_flops_theoretical", 486.60);
    Near(output.records.back(), "aiv_flops_theoretical", 30.41);
    EXPECT_NE(output.text.find("\"aic_flops_theoretical\":486.60"), std::string::npos);
    metadata.hardware.cubeCoreCount.reset();
    Na(Render({}, metadata).records.back(), "aic_flops_theoretical");
}
TEST(SummaryMetrics, SortsDeduplicatesSelectionsAndPreservesJsonTypes)
{
    const std::vector<std::string> sections = {
        "ResourceConflictRatio", "PipeUtilization", "MemoryUB", "MemoryL0", "Memory", "L2Cache",
        "ArithmeticUtilization", "Memory"};
    const auto output = Render({}, Metadata(KernelKind::Cube, 3), sections);
    const std::vector<std::string> sorted = {
        "ArithmeticUtilization", "L2Cache",      "Memory", "MemoryL0", "MemoryUB", "PipeUtilization",
        "ResourceConflictRatio", "OpInfoSummary"};
    ASSERT_EQ(output.records.size(), sorted.size());
    EXPECT_EQ(output.rows, sorted.size());
    for (size_t i = 0; i < sorted.size(); ++i) {
        EXPECT_EQ(output.records[i].at("category").as_string(), sorted[i]);
    }
    const auto& info = output.records.back();
    EXPECT_EQ(info.size(), 20U);
    EXPECT_TRUE(info.at("Block Dim").is_int64() || info.at("Block Dim").is_uint64());
    Na(info, "Op Name");
    Na(Render({}, {}).records.back(), "Op Type");
    const auto subset = Render({}, {}, {"MemoryUB", "ArithmeticUtilization", "MemoryUB"});
    ASSERT_EQ(subset.records.size(), 3U);
    EXPECT_EQ(subset.records[0].at("category").as_string(), "ArithmeticUtilization");
    EXPECT_EQ(subset.records[1].at("category").as_string(), "MemoryUB");
    EXPECT_EQ(subset.rows, 3U);
    const auto empty = Render({}, {});
    EXPECT_EQ(empty.records.size(), 1U);
    EXPECT_EQ(empty.rows, 1U);
}
TEST(SummaryMetrics, SupportsActual950ProductNames)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    auto metadata = Metadata(KernelKind::Cube, 1);
    for (const auto* product : {"Ascend950PR_9599", "Ascend950DT_9599"}) {
        metadata.hardware.socName = product;
        auto info = Render(result, metadata).records.back();
        Near(info, "aicore_parallel_utilization", 80);
        Near(info, "aic_flops_theoretical", 196.61);
    }
    metadata.hardware.socName = "Ascend910B1";
    Na(Render(result, metadata).records.back(), "aic_flops_theoretical");
}
TEST(SummaryMetrics, FixedMetricsAreIndependentOfSelectedSections)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    auto metadata = Metadata(KernelKind::Cube, 1);
    const auto noSections = Render(result, metadata).records.back();
    const auto selected = Render(result, metadata, {"Memory"}).records.back();
    Near(noSections, "aicore_parallel_utilization", 80);
    EXPECT_EQ(noSections.at("aicore_parallel_utilization"), selected.at("aicore_parallel_utilization"));
    EXPECT_EQ(noSections.at("aic_flops_theoretical"), selected.at("aic_flops_theoretical"));
}
TEST(SummaryMetrics, AivTaskMeansRemainIndependentOfMixRatioAndBlockInputs)
{
    aclptiProfilingDataResult result;
    Duration(result);
    aclptiPmuDataRow row{};
    row.coreType = ACLPTI_CORE_TYPE_AIV;
    row.totalCycles = 2000;
    row.values = {{1281, 1000}};
    result.taskPmuLogs.emplace(aclptiBlockKey{0, 0, ACLPTI_CORE_TYPE_AIV, 0}, row);
    row.totalCycles = 4000;
    row.values = {{1281, 3200}};
    result.taskPmuLogs.emplace(aclptiBlockKey{1, 0, ACLPTI_CORE_TYPE_AIV, 0}, row);
    auto metadata = Metadata(KernelKind::Mix, 1, 1);
    const auto first = Render(result, metadata, {"ArithmeticUtilization"});
    Near(first.records.front(), "aiv_total_cycles", 3000);
    Near(first.records.front(), "aiv_vec_ratio", 0.65);
    Near(first.records.back(), "aiv_flops", 0.049920);
    metadata.mixBlockDim = 2;
    metadata.hardware.currentAicFrequencyMhz = 2000;
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 99999);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 11111);
    Near(Render(result, metadata).records.back(), "aiv_flops", 0.049920);
    result.taskPmuLogs.clear();
    Na(Render(result, metadata).records.back(), "aiv_flops");
}
TEST(SummaryMetrics, UsesSideFrequenciesAndRejectsInvalidBlockTimesOrIdentities)
{
    aclptiProfilingDataResult result;
    Duration(result);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 4000);
    auto metadata = Metadata(KernelKind::Mix, 1, 1);
    metadata.hardware.currentAicFrequencyMhz = 2000;
    auto info = Render(result, metadata).records.back();
    Near(info, "aicore_parallel_utilization", 40);
    Near(info, "aicore_parallel_balance", 100);
    Na(info, "Current Freq");
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        result.pmuLogs.at({0, 0, ACLPTI_CORE_TYPE_AIV, 0}).totalCycles = invalid;
        Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
    }
    result.pmuLogs.clear();
    Row(result, 1, 0, ACLPTI_CORE_TYPE_AIC, 8000);
    Row(result, 0, 0, ACLPTI_CORE_TYPE_AIV, 4000);
    Na(Render(result, metadata).records.back(), "aicore_parallel_balance");
}
TEST(SummaryMetrics, LocalAndSinkPayloadsMatchAndFailedSinkAborts)
{
    char directory[] = "/tmp/npu-summary-payload-XXXXXX";
    ASSERT_NE(::mkdtemp(directory), nullptr);
    ReportConfig config;
    config.outputDirectory = directory;
    const auto metadata = Metadata(KernelKind::Vector, 2);
    const auto expected = Render({}, metadata, {"Memory", "ArithmeticUtilization"}, config);
    ASSERT_EQ(WriteSummaryJsonl({}, {"Memory", "ArithmeticUtilization"}, config, metadata), ACLPTI_SUCCESS);
    std::ifstream file(std::string(directory) + "/summary.jsonl");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), expected.text);
    EXPECT_EQ(expected.text.back(), '\n');
    std::filesystem::remove_all(directory);
    class FailedSink final : public ArtifactSink {
    public:
        void Begin(const std::string&) override {}
        void Write(std::string_view) override { throw std::runtime_error("write failure"); }
        void Commit(uint64_t) override { committed = true; }
        void Abort() noexcept override { aborted = true; }
        bool committed = false;
        bool aborted = false;
    } sink;
    EXPECT_EQ(WriteSummaryJsonl({}, {}, config, metadata, &sink), ACLPTI_ERROR_INTERNAL);
    EXPECT_TRUE(sink.aborted);
    EXPECT_FALSE(sink.committed);
}
} // namespace
