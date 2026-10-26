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
constexpr std::size_t SOC_NAME_PREFIX_BYTES = 3;
constexpr std::size_t ASCEND950_SOC_NAME_BYTES = 11;
constexpr std::size_t SUMMARY_METADATA_FIELDS = 2;
constexpr uint64_t SINGLE_CORE_RATIO = 1;
constexpr uint64_t MIX_VECTOR_RATIO = 2;
constexpr double AIC_FLOPS_PER_CYCLE = 8192;
constexpr double AIV_FLOPS_PER_CYCLE = 256;
constexpr double MICROSECONDS_PER_SECOND = 1000000;
constexpr double PERCENT_SCALE = 100;
SummaryMetric Positive(SummaryMetric value)
{
    return value && std::isfinite(*value) && *value > 0 ? value : std::nullopt;
}
bool Nonnegative(double value) { return std::isfinite(value) && value >= 0; }
struct Average {
    double sum = 0;
    size_t count = 0;
    void Add(SummaryMetric value)
    {
        if (value && std::isfinite(*value)) {
            sum += *value;
            ++count;
        }
    }
    SummaryMetric Value() const { return count ? SummaryMetric(sum / count) : std::nullopt; }
};
bool SupportsSummary(const KernelMetadata& metadata, const ReportConfig& config)
{
    const auto soc =
        metadata.hardware.collected ? metadata.hardware.socName : std::optional<std::string>(config.socName);
    return soc && (soc->compare(0, SOC_NAME_PREFIX_BYTES, "950") == 0 ||
                   soc->compare(0, ASCEND950_SOC_NAME_BYTES, "Ascend950PR") == 0 ||
                   soc->compare(0, ASCEND950_SOC_NAME_BYTES, "Ascend950DT") == 0);
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
        if (vectorRatio != SINGLE_CORE_RATIO && vectorRatio != MIX_VECTOR_RATIO) {
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
        duration ? SummaryMetric(sum / static_cast<double>(expected) / *duration * PERCENT_SCALE) : std::nullopt,
        maximum > 0 ? SummaryMetric(sum / static_cast<double>(expected) / maximum * PERCENT_SCALE) : std::nullopt};
}
SummaryMetric Estimate(const aclptiProfilingDataResult& result, const ReportConfig& config, bool aic)
{
    const auto duration = Positive(PmuMetricBuilder::TaskDuration(result));
    if (!duration) {
        return std::nullopt;
    }
    const auto header = PmuMetricBuilder::Header("ArithmeticUtilization");
    const std::string cycleName = aic ? "aic_total_cycles" : "aiv_total_cycles";
    const std::string ratioName = aic ? "aic_cube_fp_ratio" : "aiv_vec_ratio";
    const auto cycleIndex = static_cast<size_t>(std::find(header.begin(), header.end(), cycleName) - header.begin());
    const auto ratioIndex = static_cast<size_t>(std::find(header.begin(), header.end(), ratioName) - header.begin());
    Average cycles;
    Average ratios;
    for (const auto& [key, row] : result.taskPmuLogs) {
        if (key.coreType != (aic ? ACLPTI_CORE_TYPE_AIC : ACLPTI_CORE_TYPE_AIV)) {
            continue;
        }
        const auto fields = PmuMetricBuilder::Build("ArithmeticUtilization", key, row, config);
        if (fields.size() <= std::max(cycleIndex, ratioIndex)) {
            return std::nullopt;
        }
        const auto cycle = fields[cycleIndex].value;
        const auto ratio = fields[ratioIndex].value;
        // No missing task sample may silently shrink the averaging population.
        if (!cycle || !ratio || !Nonnegative(*cycle) || !Nonnegative(*ratio)) {
            return std::nullopt;
        }
        cycles.Add(cycle);
        ratios.Add(ratio);
    }
    const auto meanCycles = cycles.Value();
    const auto meanRatio = ratios.Value();
    return meanCycles && meanRatio ? SummaryMetric(
                                         *meanCycles * *meanRatio * (aic ? AIC_FLOPS_PER_CYCLE : AIV_FLOPS_PER_CYCLE) /
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
SummaryFields AggregateSummarySection(
    const std::string& section, const aclptiProfilingDataResult& result, const ReportConfig& config)
{
    const auto header = PmuMetricBuilder::Header(section);
    std::vector<Average> averages(header.size());
    std::vector<bool> integers(header.size(), false);
    const auto duration = PmuMetricBuilder::OperationDuration(result, PmuDataLevel::Task);
    for (const auto& [key, row] : result.taskPmuLogs) {
        const auto fields = PmuMetricBuilder::Build(section, key, row, config, duration);
        for (size_t i = SUMMARY_METADATA_FIELDS; i < fields.size(); ++i) {
            averages[i].Add(fields[i].value);
            integers[i] = integers[i] || fields[i].integer;
        }
    }
    SummaryFields fields;
    for (size_t i = SUMMARY_METADATA_FIELDS; i < header.size(); ++i) {
        fields.emplace(header[i], SummaryField{averages[i].Value(), integers[i]});
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
