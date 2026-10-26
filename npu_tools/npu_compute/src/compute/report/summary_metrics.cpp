/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/summary_metrics.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace npucompute {
namespace {
constexpr double AIC_FLOPS_PER_CYCLE = 8192.0;
constexpr double AIV_FLOPS_PER_CYCLE = 256.0;
constexpr double MICROSECONDS_PER_SECOND = 1000000.0;
SummaryMetric Positive(SummaryMetric value)
{
    return value && std::isfinite(*value) && *value > 0 ? value : std::nullopt;
}
bool Nonnegative(double value) { return std::isfinite(value) && value >= 0; }
bool SupportsSummary(const KernelMetadata& metadata, const ReportConfig& config)
{
    const auto soc =
        metadata.hardware.collected ? metadata.hardware.socName : std::optional<std::string>(config.socName);
    return soc && (soc->compare(0, 3, "950") == 0 || soc->compare(0, 11, "Ascend950PR") == 0 ||
                   soc->compare(0, 11, "Ascend950DT") == 0);
}
SummaryMetric Cycles(const aclptiBlockKey& key, const aclptiPmuDataRow& row)
{
    if (row.coreData.empty()) {
        return row.coreType == key.coreType && Nonnegative(row.totalCycles) ? SummaryMetric(row.totalCycles) :
                                                                              std::nullopt;
    }
    SummaryMetric cycles;
    for (const auto& core : row.coreData) {
        if (core.coreType != key.coreType) {
            continue;
        }
        if (!Nonnegative(core.totalCycles) || (cycles && *cycles != core.totalCycles)) {
            return std::nullopt;
        }
        cycles = core.totalCycles;
    }
    return cycles;
}
std::pair<SummaryMetric, SummaryMetric> Parallel(
    const aclptiProfilingDataResult& result, const ReportConfig& config, const KernelMetadata& metadata)
{
    if (!metadata.kind || !metadata.blockDim || *metadata.blockDim == 0) {
        return {};
    }
    const bool hasAic = *metadata.kind != KernelKind::Vector;
    const bool hasAiv = *metadata.kind != KernelKind::Cube;
    uint64_t vectorRatio = 1;
    if (*metadata.kind == KernelKind::Mix) {
        if (!metadata.mixBlockDim || *metadata.mixBlockDim == 0 || *metadata.mixBlockDim % *metadata.blockDim != 0) {
            return {};
        }
        vectorRatio = *metadata.mixBlockDim / *metadata.blockDim;
        if (vectorRatio != 1 && vectorRatio != 2) {
            return {};
        }
    }
    const uint64_t aicCount = hasAic ? *metadata.blockDim : 0;
    const uint64_t aivCount =
        hasAiv ? (*metadata.kind == KernelKind::Mix ? *metadata.mixBlockDim : *metadata.blockDim) : 0;
    if (aicCount > std::numeric_limits<uint64_t>::max() - aivCount) {
        return {};
    }
    const uint64_t expected = aicCount + aivCount;
    using Identity = std::tuple<uint16_t, uint16_t, aclptiCoreType>;
    std::map<Identity, double> objects;
    for (const auto& [key, row] : result.pmuLogs) {
        const bool aic = key.coreType == ACLPTI_CORE_TYPE_AIC;
        if ((!aic && key.coreType != ACLPTI_CORE_TYPE_AIV) || (aic && !hasAic) || (!aic && !hasAiv) ||
            key.blockId >= *metadata.blockDim || key.subBlockId >= (aic ? 1 : vectorRatio)) {
            return {};
        }
        const auto cycles = Cycles(key, row);
        if (!cycles) {
            return {};
        }
        const auto [position, inserted] = objects.emplace(Identity{key.blockId, key.subBlockId, key.coreType}, *cycles);
        if (!inserted && position->second != *cycles) {
            return {};
        }
    }
    // Count plus the legal identity bounds proves every launch object is present.
    if (objects.size() != expected) {
        return {};
    }
    double sum = 0;
    double maximum = 0;
    for (const auto& [identity, cycles] : objects) {
        const auto frequency =
            SummaryEffectiveFrequency(metadata, config, std::get<2>(identity) == ACLPTI_CORE_TYPE_AIC);
        if (!frequency) {
            return {};
        }
        const double time = cycles / *frequency;
        if (!Nonnegative(time)) {
            return {};
        }
        sum += time;
        maximum = std::max(maximum, time);
    }
    if (!std::isfinite(sum)) {
        return {};
    }
    const auto duration = Positive(PmuMetricBuilder::TaskDuration(result));
    return {
        duration ? SummaryMetric(sum / static_cast<double>(expected) / *duration * 100) : std::nullopt,
        maximum > 0 ? SummaryMetric(sum / static_cast<double>(expected) / maximum * 100) : std::nullopt};
}
SummaryMetric Estimate(const aclptiProfilingDataResult& result, const ReportConfig& config, bool aic)
{
    const auto duration = Positive(PmuMetricBuilder::TaskDuration(result));
    if (!duration) {
        return std::nullopt;
    }
    const auto header = PmuMetricBuilder::Header("ArithmeticUtilization", PmuDataLevel::Task);
    const auto fields = PmuMetricBuilder::BuildTask("ArithmeticUtilization", result.taskPmuData, config);
    const std::string cycleName = aic ? "aic_total_cycles" : "aiv_total_cycles";
    const std::string ratioName = aic ? "aic_cube_fp_ratio" : "aiv_vec_ratio";
    const auto cycleIndex = static_cast<size_t>(std::find(header.begin(), header.end(), cycleName) - header.begin());
    const auto ratioIndex = static_cast<size_t>(std::find(header.begin(), header.end(), ratioName) - header.begin());
    const auto cycles = fields.at(cycleIndex).value;
    const auto ratio = fields.at(ratioIndex).value;
    return cycles && ratio && Nonnegative(*cycles) && Nonnegative(*ratio) ?
               SummaryMetric(
                   *cycles * *ratio * (aic ? AIC_FLOPS_PER_CYCLE : AIV_FLOPS_PER_CYCLE) /
                   (*duration * MICROSECONDS_PER_SECOND)) :
               std::nullopt;
}
} // namespace

SummaryMetric SummaryRatedFrequency(const KernelMetadata& metadata, bool aic)
{
    return Positive(
        metadata.hardware.collected ?
            (aic ? metadata.hardware.ratedAicFrequencyMhz : metadata.hardware.ratedAivFrequencyMhz) :
            (aic ? metadata.ratedAicFrequencyMhz : metadata.ratedAivFrequencyMhz));
}
SummaryMetric SummaryEffectiveFrequency(const KernelMetadata& metadata, const ReportConfig& config, bool aic)
{
    SummaryMetric current;
    if (metadata.hardware.collected) {
        current = Positive(aic ? metadata.hardware.currentAicFrequencyMhz : metadata.hardware.currentAivFrequencyMhz);
    } else {
        const double side = aic ? config.aicFrequencyMhz : config.aivFrequencyMhz;
        // Generic frequencyMhz includes the CSV default and cannot prove availability.
        current = Positive(side);
    }
    return current ? current : SummaryRatedFrequency(metadata, aic);
}
ReportConfig SummaryReportConfig(const KernelMetadata& metadata, const ReportConfig& config)
{
    auto summary = config;
    summary.frequencyMhz = 0;
    summary.aicFrequencyMhz = SummaryEffectiveFrequency(metadata, config, true).value_or(0);
    summary.aivFrequencyMhz = SummaryEffectiveFrequency(metadata, config, false).value_or(0);
    if (metadata.hardware.collected) {
        summary.socName = metadata.hardware.socName.value_or("");
    }
    return summary;
}
SummaryFields BuildTaskSummarySection(
    const std::string& section, const aclptiProfilingDataResult& result, const ReportConfig& config)
{
    const auto header = PmuMetricBuilder::Header(section, PmuDataLevel::Task);
    const auto values = PmuMetricBuilder::BuildTask(
        section, result.taskPmuData, config, PmuMetricBuilder::OperationDuration(result, PmuDataLevel::Task));
    SummaryFields fields;
    for (size_t i = 0; i < header.size(); ++i) {
        fields.emplace(header[i], SummaryField{values.at(i).value, values.at(i).integer});
    }
    return fields;
}
SummaryMetrics BuildSummaryMetrics(
    const aclptiProfilingDataResult& result, const ReportConfig& config, const KernelMetadata& metadata)
{
    SummaryMetrics metrics;
    if (!SupportsSummary(metadata, config)) {
        return metrics;
    }
    const auto parallel = Parallel(result, config, metadata);
    metrics.parallelUtilization = parallel.first;
    metrics.parallelBalance = parallel.second;
    const auto theoretical = [&](bool aic) -> SummaryMetric {
        const auto count = aic ? metadata.hardware.cubeCoreCount : metadata.hardware.vectorCoreCount;
        const auto frequency = SummaryEffectiveFrequency(metadata, config, aic);
        return count && *count > 0 && frequency ? SummaryMetric(
                                                      *count * (aic ? AIC_FLOPS_PER_CYCLE : AIV_FLOPS_PER_CYCLE) *
                                                      *frequency / MICROSECONDS_PER_SECOND) :
                                                  std::nullopt;
    };
    metrics.aicFlopsTheoretical = theoretical(true);
    metrics.aivFlopsTheoretical = theoretical(false);
    const bool hasAic = !metadata.kind || *metadata.kind != KernelKind::Vector;
    const bool hasAiv = !metadata.kind || *metadata.kind != KernelKind::Cube;
    metrics.aicFlops = hasAic ? Estimate(result, config, true) : std::nullopt;
    metrics.aivFlops = hasAiv ? Estimate(result, config, false) : std::nullopt;
    return metrics;
}
} // namespace npucompute
