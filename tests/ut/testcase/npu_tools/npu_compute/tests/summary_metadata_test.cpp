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

#include <boost/property_tree/json_parser.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>

namespace {

class SummarySink final : public npucompute::ArtifactSink {
public:
    void Begin(const std::string&) override { content.clear(); }
    void Write(std::string_view bytes) override { content.append(bytes); }
    void Commit(uint64_t) override {}
    void Abort() noexcept override {}
    std::string content;
};

boost::property_tree::ptree Summary(
    const npucompute::KernelMetadata& metadata, const npucompute::ReportConfig& config = {},
    const aclptiProfilingDataResult& result = {})
{
    SummarySink sink;
    EXPECT_EQ(npucompute::WriteSummaryJsonl(result, {}, config, metadata, sink), ACLPTI_SUCCESS);
    const auto previous = sink.content.rfind('\n', sink.content.size() - 2);
    std::istringstream input(sink.content.substr(previous + 1));
    boost::property_tree::ptree fields;
    boost::property_tree::read_json(input, fields);
    return fields;
}

TEST(SummaryMetadata, UsesLaunchTypeAndMixDimensionWithoutPmu)
{
    npucompute::KernelMetadata metadata;
    metadata.kind = npucompute::KernelKind::Mix;
    metadata.blockDim = 8;
    metadata.mixBlockDim = 16;
    const auto summary = Summary(metadata);
    EXPECT_EQ(summary.get<std::string>("Op Type"), "mix");
    EXPECT_EQ(summary.get<uint64_t>("Mix Block Dim"), 16U);
    aclptiProfilingDataResult partial;
    partial.taskPmuData.aic = aclptiTaskPmuCoreData{};
    EXPECT_EQ(Summary(metadata, {}, partial).get<std::string>("Op Type"), "mix");
    metadata.kind = npucompute::KernelKind::Cube;
    const auto cube = Summary(metadata);
    EXPECT_EQ(cube.get<std::string>("Op Type"), "cube");
    EXPECT_EQ(cube.get<std::string>("Mix Block Dim"), "NA");
}

TEST(SummaryMetadata, MissingActualFrequenciesDoNotUseCsvDefaults)
{
    npucompute::KernelMetadata metadata;
    metadata.kind = npucompute::KernelKind::Vector;
    metadata.hardware.collected = true;
    metadata.ratedAivFrequencyMhz = 1700; // Old input must not override an actual failed query.
    npucompute::ReportConfig config;
    config.frequencyMhz = 1650;
    aclptiProfilingDataResult result;
    result.taskPmuData.aiv = aclptiTaskPmuCoreData{};
    const auto summary = Summary(metadata, config, result);
    EXPECT_EQ(summary.get<std::string>("Op Type"), "vector");
    EXPECT_EQ(summary.get<std::string>("Current Freq"), "NA");
    EXPECT_EQ(summary.get<std::string>("Rated Freq"), "NA");
}

TEST(SummaryMetadata, FallsBackToRatedFrequencyPerSide)
{
    npucompute::KernelMetadata metadata;
    metadata.kind = npucompute::KernelKind::Mix;
    metadata.hardware.collected = true;
    metadata.hardware.currentAicFrequencyMhz = 1500;
    metadata.hardware.ratedAicFrequencyMhz = 1700;
    metadata.hardware.ratedAivFrequencyMhz = 1500;
    auto summary = Summary(metadata);
    EXPECT_DOUBLE_EQ(summary.get<double>("Current Freq"), 1500);
    EXPECT_EQ(summary.get<std::string>("Rated Freq"), "NA");
    metadata.hardware.currentAivFrequencyMhz = 1600;
    summary = Summary(metadata);
    EXPECT_EQ(summary.get<std::string>("Current Freq"), "NA");
}

TEST(SummaryMetadata, RejectsInvalidFrequencyBeforeRatedFallback)
{
    npucompute::KernelMetadata metadata;
    metadata.kind = npucompute::KernelKind::Cube;
    metadata.hardware.collected = true;
    metadata.hardware.ratedAicFrequencyMhz = 1700;
    for (const double invalid :
         {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        metadata.hardware.currentAicFrequencyMhz = invalid;
        EXPECT_DOUBLE_EQ(Summary(metadata).get<double>("Current Freq"), 1700);
    }
}

} // namespace
