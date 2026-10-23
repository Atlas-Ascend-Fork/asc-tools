/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pmu/pmu_metric_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace npucompute {
namespace {

using Metric = std::optional<double>;

constexpr double kBytesPerGb = 1024.0 * 1024.0 * 1024.0;
constexpr double kBytesPerKb = 1024.0;
constexpr double kA5TaskSchedulerFrequencyKHz = 1000000.0;

struct TypeMetrics {
    bool present = false;
    double totalCycles = 0.0;
    bool overflow = false;
    std::map<uint32_t, double> values;

    double Cycles() const { return totalCycles; }

    Metric Value(uint32_t eventId) const
    {
        const auto value = values.find(eventId);
        if (value == values.end()) {
            return std::nullopt;
        }
        return value->second;
    }
};

struct RowMetrics {
    aclptiBlockKey key{};
    TypeMetrics aic;
    TypeMetrics aiv;
};

using CsvCell = PmuMetricField;
using CsvRow = PmuMetricRow;
using CsvFields = std::map<std::string, CsvCell>;

const char* MissingReasonName(MissingReason reason)
{
    switch (reason) {
        case MissingReason::None:
            return "None";
        case MissingReason::Unknown:
            return "Unknown";
        case MissingReason::CoreTypeNotApplicable:
            return "CoreTypeNotApplicable";
        case MissingReason::EventMissing:
            return "EventMissing";
        case MissingReason::InvalidDenominator:
            return "InvalidDenominator";
        case MissingReason::InvalidFrequencyOrDuration:
            return "InvalidFrequencyOrDuration";
        case MissingReason::DbiUnavailable:
            return "DbiUnavailable";
        case MissingReason::UnsupportedSocBandwidth:
            return "UnsupportedSocBandwidth";
        case MissingReason::RowSizeMismatch:
            return "RowSizeMismatch";
    }
    return "Unknown";
}

std::string Number(double value)
{
    if (!std::isfinite(value)) {
        return "NA";
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(6) << value;
    return output.str();
}

std::string Number(const Metric& value) { return value.has_value() ? Number(*value) : "NA"; }

std::string Integer(uint16_t value) { return std::to_string(value); }

std::string Counter(double value)
{
    if (!std::isfinite(value) || value < 0.0) {
        return "NA";
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(0) << value;
    return output.str();
}

CsvCell ValueCell(std::string value) { return CsvCell{std::move(value), MissingReason::None}; }

CsvCell MissingCell(MissingReason reason)
{
    return CsvCell{"NA", reason == MissingReason::None ? MissingReason::Unknown : reason};
}

CsvCell MetricCell(const Metric& value, MissingReason missingReason)
{
    return value.has_value() && std::isfinite(*value) ? CsvCell{Number(*value), MissingReason::None, value, false} :
                                                        MissingCell(missingReason);
}

CsvCell MetricCell(double value) { return CsvCell{Number(value), MissingReason::None, value, false}; }

CsvCell MissingMetricCell(MissingReason reason) { return CsvCell{"NA", reason, std::nullopt, false}; }

CsvCell CounterCell(const Metric& value, MissingReason missingReason)
{
    return value.has_value() && std::isfinite(*value) && *value >= 0.0 ?
               CsvCell{Counter(*value), MissingReason::None, value, true} :
               MissingCell(missingReason);
}

bool HasEvent(const TypeMetrics& metrics, uint32_t eventId) { return metrics.Value(eventId).has_value(); }

bool HasEvents(const TypeMetrics& metrics, std::initializer_list<uint32_t> events)
{
    for (const uint32_t event : events) {
        if (!HasEvent(metrics, event)) {
            return false;
        }
    }
    return true;
}

MissingReason ValidDenominatorReason(std::initializer_list<double> paras)
{
    for (const double& para : paras) {
        if (para <= 0.0 || !std::isfinite(para)) {
            return MissingReason::InvalidDenominator;
        }
    }
    return MissingReason::Unknown;
}

MissingReason EventReason(const TypeMetrics& metrics, std::initializer_list<uint32_t> events)
{
    if (!metrics.present) {
        return MissingReason::CoreTypeNotApplicable;
    }
    return HasEvents(metrics, events) ? MissingReason::Unknown : MissingReason::EventMissing;
}

MissingReason TimeReason(const TypeMetrics& metrics, double frequencyMhz)
{
    if (!metrics.present) {
        return MissingReason::CoreTypeNotApplicable;
    }
    return frequencyMhz <= 0.0 || !std::isfinite(frequencyMhz) ? MissingReason::InvalidFrequencyOrDuration :
                                                                 MissingReason::Unknown;
}

MissingReason RatioReason(const TypeMetrics& metrics, std::initializer_list<uint32_t> events)
{
    const MissingReason eventReason = EventReason(metrics, events);
    if (eventReason != MissingReason::Unknown) {
        return eventReason;
    }
    return metrics.Cycles() == 0.0 ? MissingReason::InvalidDenominator : MissingReason::Unknown;
}

MissingReason EventBandwidthReason(
    const TypeMetrics& metrics, std::initializer_list<uint32_t> events, double frequencyMhz,
    Metric operationDurationUs = std::nullopt)
{
    const MissingReason eventReason = EventReason(metrics, events);
    if (eventReason != MissingReason::Unknown) {
        return eventReason;
    }
    if (operationDurationUs.has_value()) {
        return MissingReason::Unknown;
    }
    const MissingReason timeReason = TimeReason(metrics, frequencyMhz);
    return timeReason == MissingReason::Unknown && metrics.Cycles() == 0.0 ? MissingReason::InvalidFrequencyOrDuration :
                                                                             timeReason;
}

MissingReason UsageRateReason(MissingReason bandwidthReason, double maxBandwidth)
{
    if (maxBandwidth <= 0.0) {
        return MissingReason::UnsupportedSocBandwidth;
    }
    return bandwidthReason == MissingReason::None ? MissingReason::Unknown : bandwidthReason;
}

std::string CoreSubBlockLabel(const aclptiBlockKey& key)
{
    std::ostringstream output;
    output << (key.coreType == ACLPTI_CORE_TYPE_AIC ? "cube" : "vector") << key.subBlockId;
    return output.str();
}

void AddCore(TypeMetrics& metrics, const aclptiPmuDataRow::CoreData& core)
{
    metrics.present = true;
    metrics.totalCycles = core.totalCycles;
    metrics.overflow = metrics.overflow || core.overflow;
    for (const auto& [eventId, value] : core.values) {
        metrics.values.emplace(eventId, value);
    }
}

RowMetrics MakeRowMetrics(aclptiBlockKey key, const aclptiPmuDataRow& row)
{
    RowMetrics metrics;
    metrics.key = key;
    if (!row.coreData.empty()) {
        for (const auto& core : row.coreData) {
            if (core.coreType == key.coreType) {
                AddCore(key.coreType == ACLPTI_CORE_TYPE_AIC ? metrics.aic : metrics.aiv, core);
            }
        }
        return metrics;
    }

    aclptiPmuDataRow::CoreData fallback{};
    fallback.coreType = row.coreType;
    fallback.coreId = row.coreId;
    fallback.sampleCount = 1;
    fallback.totalCycles = row.totalCycles;
    fallback.overflow = row.overflow;
    fallback.values = row.values;
    for (const auto& [eventId, value] : row.values) {
        static_cast<void>(value);
        fallback.valueCounts.emplace(eventId, 1);
    }
    AddCore(row.coreType == ACLPTI_CORE_TYPE_AIC ? metrics.aic : metrics.aiv, fallback);
    return metrics;
}

std::optional<double> Ratio(double numerator, double denominator)
{
    if (denominator == 0.0) {
        return std::nullopt;
    }
    return numerator / denominator;
}

std::optional<double> Time(const TypeMetrics& metrics, double frequencyMhz)
{
    if (!metrics.present || frequencyMhz <= 0.0) {
        return std::nullopt;
    }
    return metrics.Cycles() / frequencyMhz;
}

struct TaskLogCounterPair {
    uint16_t taskId = 0;
    uint16_t streamId = 0;
    bool hasIdentity = false;
    bool invalid = false;
    std::optional<uint64_t> start;
    std::optional<uint64_t> end;
};

Metric TaskLogDurationUs(const aclptiProfilingDataResult& result)
{
    std::map<uint64_t, TaskLogCounterPair> pairs;
    for (const auto& [taskId, logs] : result.taskLogs) {
        static_cast<void>(taskId);
        for (const aclptiTaskLogRow& log : logs) {
            TaskLogCounterPair& pair = pairs[log.replayId];
            if (!pair.hasIdentity) {
                pair.taskId = log.taskId;
                pair.streamId = log.rtStreamId;
                pair.hasIdentity = true;
            } else if (pair.taskId != log.taskId || pair.streamId != log.rtStreamId) {
                pair.invalid = true;
            }
            if (log.funcType == 0x00U) {
                if (pair.start.has_value()) {
                    pair.invalid = true;
                } else {
                    pair.start = log.systemCounter;
                }
            } else if (log.funcType == 0x01U) {
                if (pair.end.has_value()) {
                    pair.invalid = true;
                } else {
                    pair.end = log.systemCounter;
                }
            }
        }
    }

    std::vector<double> durations;
    for (const auto& [replayId, pair] : pairs) {
        static_cast<void>(replayId);
        if (pair.invalid || !pair.start.has_value() || !pair.end.has_value() || *pair.end <= *pair.start) {
            continue;
        }
        const double durationUs = static_cast<double>(*pair.end - *pair.start) * 1000.0 / kA5TaskSchedulerFrequencyKHz;
        if (std::isfinite(durationUs) && durationUs > 0.0) {
            durations.push_back(durationUs);
        }
    }
    if (durations.empty()) {
        return std::nullopt;
    }
    std::sort(durations.begin(), durations.end());
    const std::size_t middle = durations.size() / 2;
    return durations.size() % 2U == 0U ? (durations[middle - 1] + durations[middle]) / 2.0 : durations[middle];
}

std::optional<double> ValueRatio(const TypeMetrics& metrics, uint32_t eventId)
{
    const auto value = metrics.Value(eventId);
    if (!metrics.present || !value.has_value()) {
        return std::nullopt;
    }
    return Ratio(*value, metrics.Cycles());
}

std::optional<double> IcacheMissRate(const TypeMetrics& metrics)
{
    const auto numerator = metrics.Value(0x35U);
    const auto denominator = metrics.Value(0x34U);
    if (!metrics.present || !numerator.has_value() || !denominator.has_value()) {
        return std::nullopt;
    }
    return Ratio(*numerator, *denominator);
}

std::optional<double> EventTime(const TypeMetrics& metrics, uint32_t eventId, double frequencyMhz)
{
    const auto value = metrics.Value(eventId);
    if (!metrics.present || !value.has_value() || frequencyMhz <= 0.0) {
        return std::nullopt;
    }
    return *value / frequencyMhz;
}

std::optional<double> Bandwidth(double bytes, double durationUs)
{
    if (durationUs <= 0.0) {
        return std::nullopt;
    }
    return bytes * 1000000.0 / kBytesPerGb / durationUs;
}

std::optional<double> EventBandwidth(
    const TypeMetrics& metrics, uint32_t eventId, double bytesPerEvent, double frequencyMhz,
    Metric operationDurationUs = std::nullopt)
{
    const auto value = metrics.Value(eventId);
    const auto duration = operationDurationUs.has_value() ? operationDurationUs : Time(metrics, frequencyMhz);
    if (!value.has_value() || !duration.has_value()) {
        return std::nullopt;
    }
    return Bandwidth(*value * bytesPerEvent, *duration);
}

std::optional<double> ActiveBandwidth(double bytes, double activeCycles, double frequencyMhz)
{
    if (frequencyMhz <= 0.0 || activeCycles <= 0.0) {
        return std::nullopt;
    }
    return Bandwidth(bytes, activeCycles / frequencyMhz);
}

Metric SumEvents(const TypeMetrics& metrics, std::initializer_list<uint32_t> events)
{
    double sum = 0.0;
    for (const uint32_t event : events) {
        const auto value = metrics.Value(event);
        if (!value.has_value()) {
            return std::nullopt;
        }
        sum += *value;
    }
    return sum;
}

Metric NonNegativeDifference(const TypeMetrics& metrics, uint32_t minuend, std::initializer_list<uint32_t> subtrahends)
{
    const auto first = metrics.Value(minuend);
    if (!first.has_value()) {
        return std::nullopt;
    }
    double result = *first;
    for (const uint32_t event : subtrahends) {
        const auto value = metrics.Value(event);
        if (!value.has_value()) {
            return std::nullopt;
        }
        result -= *value;
    }
    return std::max(0.0, result);
}

Metric Scale(const Metric& value, double factor) { return value.has_value() ? Metric(*value * factor) : std::nullopt; }

Metric ActiveBandwidthMetric(const Metric& bytes, const Metric& activeCycles, double frequencyMhz)
{
    if (!bytes.has_value() || !activeCycles.has_value()) {
        return std::nullopt;
    }
    return ActiveBandwidth(*bytes, *activeCycles, frequencyMhz);
}

std::optional<double> UsageRate(const std::optional<double>& bandwidth, double maxBandwidth)
{
    if (!bandwidth.has_value() || maxBandwidth <= 0.0) {
        return std::nullopt;
    }
    return 100.0 * std::min(*bandwidth, maxBandwidth) / maxBandwidth;
}

double AicFrequencyMhz(const ReportConfig& config)
{
    return config.aicFrequencyMhz > 0.0 ? config.aicFrequencyMhz : config.frequencyMhz;
}

double AivFrequencyMhz(const ReportConfig& config)
{
    return config.aivFrequencyMhz > 0.0 ? config.aivFrequencyMhz : config.frequencyMhz;
}

double MaxBandwidth(std::string_view soc, std::string_view kind)
{
    const bool is959 = soc.find("959") != std::string_view::npos || soc.find("95A") != std::string_view::npos;
    if (kind == "GM_TO_L1") {
        return is959 ? 177.57 : 162.77;
    }
    if (kind == "L0C_TO_L1") {
        return is959 ? 412.15 : 377.80;
    }
    if (kind == "L0C_TO_GM") {
        return is959 ? 142.05 : 130.21;
    }
    if (kind == "GM_TO_UB") {
        return is959 ? 177.96 : 163.13;
    }
    if (kind == "UB_TO_GM") {
        return is959 ? 143.74 : 131.76;
    }
    return 0.0;
}

void SetMetric(CsvFields& fields, std::string name, const Metric& metric, MissingReason missingReason)
{
    fields.emplace(std::move(name), MetricCell(metric, missingReason));
}

void SetEvent(CsvFields& fields, std::string name, const TypeMetrics& metrics, uint32_t eventId)
{
    fields.emplace(std::move(name), CounterCell(metrics.Value(eventId), EventReason(metrics, {eventId})));
}

CsvFields CommonFields(const RowMetrics& row, const ReportConfig& config)
{
    CsvFields fields;
    fields.emplace("block_id", ValueCell(Integer(row.key.blockId)));
    fields.emplace("sub_block_id", ValueCell(CoreSubBlockLabel(row.key)));
    const double aicFrequencyMhz = AicFrequencyMhz(config);
    const double aivFrequencyMhz = AivFrequencyMhz(config);
    SetMetric(fields, "aic_time(us)", Time(row.aic, aicFrequencyMhz), TimeReason(row.aic, aicFrequencyMhz));
    fields.emplace(
        "aic_total_cycles", row.aic.present ? CounterCell(row.aic.Cycles(), MissingReason::Unknown) :
                                              MissingCell(MissingReason::CoreTypeNotApplicable));
    SetMetric(fields, "aiv_time(us)", Time(row.aiv, aivFrequencyMhz), TimeReason(row.aiv, aivFrequencyMhz));
    fields.emplace(
        "aiv_total_cycles", row.aiv.present ? CounterCell(row.aiv.Cycles(), MissingReason::Unknown) :
                                              MissingCell(MissingReason::CoreTypeNotApplicable));
    return fields;
}

CsvRow BuildRow(const std::vector<std::string>& header, const CsvFields& fields)
{
    CsvRow row;
    row.reserve(header.size());
    for (const auto& name : header) {
        const auto value = fields.find(name);
        row.push_back(value == fields.end() ? MissingCell(MissingReason::CoreTypeNotApplicable) : value->second);
    }
    return row;
}

std::vector<std::string> L2Header()
{
    const std::vector<std::string> suffixes = {
        "read_close_hit",  "read_close_miss",  "read_close_victim", "read_far_hit",     "read_far_miss",
        "read_far_victim", "read_hit_rate(%)", "write_close_hit",   "write_close_miss", "write_close_victim",
        "write_far_hit",   "write_far_miss",   "write_far_victim",  "write_hit_rate(%)"};
    std::vector<std::string> header = {"block_id", "sub_block_id"};
    for (const char* prefix : {"aic_", "aiv_"}) {
        header.emplace_back(std::string(prefix) + "time(us)");
        header.emplace_back(std::string(prefix) + "total_cycles");
        for (const auto& suffix : suffixes) {
            header.emplace_back(std::string(prefix) + suffix);
        }
    }
    return header;
}

CsvRow L2Row(const RowMetrics& row, const ReportConfig& config)
{
    CsvFields fields = CommonFields(row, config);
    for (const auto& [prefix, metrics] :
         {std::pair<const char*, const TypeMetrics*>{"aic_", &row.aic}, {"aiv_", &row.aiv}}) {
        if (!metrics->present) {
            continue;
        }
        const std::vector<std::string> names = {"read_close_hit", "read_close_miss", "read_close_victim",
                                                "read_far_hit",   "read_far_miss",   "read_far_victim"};
        std::size_t index = 0;
        for (const uint32_t event : {0x424U, 0x425U, 0x426U, 0x427U, 0x428U, 0x429U}) {
            SetEvent(fields, std::string(prefix) + names[index++], *metrics, event);
        }
        const auto readHit = SumEvents(*metrics, {0x424U, 0x427U});
        const auto readTotal = SumEvents(*metrics, {0x424U, 0x425U, 0x426U, 0x427U, 0x428U, 0x429U});
        SetMetric(
            fields, std::string(prefix) + "read_hit_rate(%)",
            !readHit.has_value() || !readTotal.has_value() ? std::nullopt :
            *readTotal == 0.0                              ? Metric(0.0) :
                                                             Metric(100.0 * *readHit / *readTotal),
            EventReason(*metrics, {0x424U, 0x425U, 0x426U, 0x427U, 0x428U, 0x429U}));
        const std::vector<std::string> writeNames = {"write_close_hit", "write_close_miss", "write_close_victim",
                                                     "write_far_hit",   "write_far_miss",   "write_far_victim"};
        index = 0;
        for (const uint32_t event : {0x42aU, 0x42bU, 0x42cU, 0x42dU, 0x42eU, 0x42fU}) {
            SetEvent(fields, std::string(prefix) + writeNames[index++], *metrics, event);
        }
        const auto writeHit = SumEvents(*metrics, {0x42aU, 0x42dU});
        const auto writeTotal = SumEvents(*metrics, {0x42aU, 0x42bU, 0x42cU, 0x42dU, 0x42eU, 0x42fU});
        SetMetric(
            fields, std::string(prefix) + "write_hit_rate(%)",
            !writeHit.has_value() || !writeTotal.has_value() ? std::nullopt :
            *writeTotal == 0.0                               ? Metric(0.0) :
                                                               Metric(100.0 * *writeHit / *writeTotal),
            EventReason(*metrics, {0x42aU, 0x42bU, 0x42cU, 0x42dU, 0x42eU, 0x42fU}));
    }
    return BuildRow(L2Header(), fields);
}

std::vector<std::string> MemoryHeader()
{
    return {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_l1_read_bw(GB/s)",
        "aic_l1_write_bw(GB/s)",
        "aic_main_mem_read_bw(GB/s)",
        "aic_main_mem_write_bw(GB/s)",
        "aic_mte1_instructions",
        "aic_mte1_ratio",
        "aic_mte2_instructions",
        "aic_mte2_ratio",
        "aic_mte3_instructions",
        "aic_mte3_ratio",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_ub_to_gm_bw(GB/s)",
        "aiv_gm_to_ub_bw(GB/s)",
        "aiv_main_mem_read_bw(GB/s)",
        "aiv_main_mem_write_bw(GB/s)",
        "aiv_mte2_instructions",
        "aiv_mte2_ratio",
        "aiv_mte3_instructions",
        "aiv_mte3_ratio",
        "read_main_memory_datas(KB)",
        "write_main_memory_datas(KB)",
        "GM_to_L1_datas(KB)",
        "GM_to_L1_bw_usage_rate(%)",
        "L0C_to_L1_datas(KB)",
        "L0C_to_L1_bw_usage_rate(%)",
        "L0C_to_GM_datas(KB)",
        "L0C_to_GM_bw_usage_rate(%)",
        "GM_to_UB_datas(KB)",
        "GM_to_UB_bw_usage_rate(%)",
        "UB_to_GM_datas(KB)",
        "UB_to_GM_bw_usage_rate(%)"};
}

CsvRow MemoryRow(const RowMetrics& row, const ReportConfig& config, Metric operationDurationUs)
{
    CsvFields fields = CommonFields(row, config);
    const double aicFrequencyMhz = AicFrequencyMhz(config);
    const double aivFrequencyMhz = AivFrequencyMhz(config);
    const auto duration = operationDurationUs.has_value() ? operationDurationUs : Time(row.aiv, aivFrequencyMhz);
    const auto xgu = NonNegativeDifference(row.aiv, 0x422U, {0x57fU, 0x580U});
    const MissingReason xguReason =
        EventBandwidthReason(row.aiv, {0x422U, 0x57fU, 0x580U}, aivFrequencyMhz, operationDurationUs);
    SetMetric(
        fields, "aiv_ub_to_gm_bw(GB/s)", std::nullopt,
        row.aiv.present ? MissingReason::DbiUnavailable : MissingReason::CoreTypeNotApplicable);
    SetMetric(
        fields, "aiv_gm_to_ub_bw(GB/s)",
        xgu.has_value() && duration.has_value() ? Bandwidth(*xgu * 128.0, *duration) : std::nullopt, xguReason);
    SetMetric(
        fields, "aic_l1_read_bw(GB/s)", EventBandwidth(row.aic, 0x707U, 256.0, aicFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aic, {0x707U}, aicFrequencyMhz, operationDurationUs));
    SetMetric(
        fields, "aic_l1_write_bw(GB/s)", EventBandwidth(row.aic, 0x709U, 128.0, aicFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aic, {0x709U}, aicFrequencyMhz, operationDurationUs));
    SetMetric(
        fields, "aic_main_mem_read_bw(GB/s)",
        EventBandwidth(row.aic, 0x422U, 128.0, aicFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aic, {0x422U}, aicFrequencyMhz, operationDurationUs));
    SetMetric(
        fields, "aic_main_mem_write_bw(GB/s)",
        EventBandwidth(row.aic, 0x423U, 128.0, aicFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aic, {0x423U}, aicFrequencyMhz, operationDurationUs));
    SetEvent(fields, "aic_mte1_instructions", row.aic, 0x700U);
    SetMetric(fields, "aic_mte1_ratio", ValueRatio(row.aic, 0x702U), RatioReason(row.aic, {0x702U}));
    SetEvent(fields, "aic_mte2_instructions", row.aic, 0x200U);
    SetMetric(fields, "aic_mte2_ratio", ValueRatio(row.aic, 0x202U), RatioReason(row.aic, {0x202U}));
    SetEvent(fields, "aic_mte3_instructions", row.aic, 0x201U);
    SetMetric(fields, "aic_mte3_ratio", ValueRatio(row.aic, 0x203U), RatioReason(row.aic, {0x203U}));
    SetMetric(
        fields, "aiv_main_mem_read_bw(GB/s)",
        EventBandwidth(row.aiv, 0x422U, 128.0, aivFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aiv, {0x422U}, aivFrequencyMhz, operationDurationUs));
    SetMetric(
        fields, "aiv_main_mem_write_bw(GB/s)",
        EventBandwidth(row.aiv, 0x423U, 128.0, aivFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aiv, {0x423U}, aivFrequencyMhz, operationDurationUs));
    SetEvent(fields, "aiv_mte2_instructions", row.aiv, 0x200U);
    SetMetric(fields, "aiv_mte2_ratio", ValueRatio(row.aiv, 0x202U), RatioReason(row.aiv, {0x202U}));
    SetEvent(fields, "aiv_mte3_instructions", row.aiv, 0x201U);
    SetMetric(fields, "aiv_mte3_ratio", ValueRatio(row.aiv, 0x203U), RatioReason(row.aiv, {0x203U}));
    const TypeMetrics& current = row.key.coreType == ACLPTI_CORE_TYPE_AIC ? row.aic : row.aiv;
    SetMetric(
        fields, "read_main_memory_datas(KB)", Scale(current.Value(0x422U), 128.0 / kBytesPerKb),
        EventReason(current, {0x422U}));
    SetMetric(
        fields, "write_main_memory_datas(KB)", Scale(current.Value(0x423U), 128.0 / kBytesPerKb),
        EventReason(current, {0x423U}));
    SetMetric(
        fields, "GM_to_L1_datas(KB)", Scale(row.aic.Value(0x422U), 128.0 / kBytesPerKb),
        EventReason(row.aic, {0x422U}));
    SetMetric(
        fields, "L0C_to_L1_datas(KB)", Scale(row.aic.Value(0x70eU), 128.0 / kBytesPerKb),
        EventReason(row.aic, {0x70eU}));
    SetMetric(
        fields, "L0C_to_GM_datas(KB)", Scale(row.aic.Value(0x423U), 128.0 / kBytesPerKb),
        EventReason(row.aic, {0x423U}));
    SetMetric(
        fields, "GM_to_UB_datas(KB)", Scale(xgu, 128.0 / kBytesPerKb), EventReason(row.aiv, {0x422U, 0x57fU, 0x580U}));
    SetMetric(
        fields, "UB_to_GM_datas(KB)", std::nullopt,
        row.aiv.present ? MissingReason::DbiUnavailable : MissingReason::CoreTypeNotApplicable);
    const double gmToL1Max = MaxBandwidth(config.socName, "GM_TO_L1");
    const double l0cToL1Max = MaxBandwidth(config.socName, "L0C_TO_L1");
    const double l0cToGmMax = MaxBandwidth(config.socName, "L0C_TO_GM");
    const double gmToUbMax = MaxBandwidth(config.socName, "GM_TO_UB");
    SetMetric(
        fields, "GM_to_L1_bw_usage_rate(%)",
        UsageRate(EventBandwidth(row.aic, 0x422U, 128.0, aicFrequencyMhz, operationDurationUs), gmToL1Max),
        UsageRateReason(EventBandwidthReason(row.aic, {0x422U}, aicFrequencyMhz, operationDurationUs), gmToL1Max));
    SetMetric(
        fields, "L0C_to_L1_bw_usage_rate(%)",
        UsageRate(EventBandwidth(row.aic, 0x70eU, 128.0, aicFrequencyMhz, operationDurationUs), l0cToL1Max),
        UsageRateReason(EventBandwidthReason(row.aic, {0x70eU}, aicFrequencyMhz, operationDurationUs), l0cToL1Max));
    SetMetric(
        fields, "L0C_to_GM_bw_usage_rate(%)",
        UsageRate(EventBandwidth(row.aic, 0x423U, 128.0, aicFrequencyMhz, operationDurationUs), l0cToGmMax),
        UsageRateReason(EventBandwidthReason(row.aic, {0x423U}, aicFrequencyMhz, operationDurationUs), l0cToGmMax));
    SetMetric(
        fields, "GM_to_UB_bw_usage_rate(%)",
        UsageRate(
            xgu.has_value() && duration.has_value() ? Bandwidth(*xgu * 128.0, *duration) : std::nullopt, gmToUbMax),
        UsageRateReason(xguReason, gmToUbMax));
    SetMetric(
        fields, "UB_to_GM_bw_usage_rate(%)", std::nullopt,
        row.aiv.present ? MissingReason::DbiUnavailable : MissingReason::CoreTypeNotApplicable);
    return BuildRow(MemoryHeader(), fields);
}

std::vector<std::string> MemoryL0Header()
{
    return {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_l0a_read_bw(GB/s)",
        "aic_l0a_write_bw(GB/s)",
        "aic_l0b_read_bw(GB/s)",
        "aic_l0b_write_bw(GB/s)",
        "aic_l0c_read_bw_cube(GB/s)",
        "aic_l0c_write_bw_cube(GB/s)",
        "aiv_time(us)",
        "aiv_total_cycles"};
}

CsvRow MemoryL0Row(const RowMetrics& row, const ReportConfig& config, Metric operationDurationUs)
{
    CsvFields fields = CommonFields(row, config);
    const double aicFrequencyMhz = AicFrequencyMhz(config);
    const std::vector<std::string> names = {"aic_l0a_read_bw(GB/s)",      "aic_l0a_write_bw(GB/s)",
                                            "aic_l0b_read_bw(GB/s)",      "aic_l0b_write_bw(GB/s)",
                                            "aic_l0c_read_bw_cube(GB/s)", "aic_l0c_write_bw_cube(GB/s)"};
    std::size_t index = 0;
    for (const auto& [event, bytes] : std::initializer_list<std::pair<uint32_t, double>>{
             {0x304U, 64.0}, {0x703U, 256.0}, {0x306U, 256.0}, {0x705U, 256.0}, {0x30aU, 1024.0}, {0x308U, 1024.0}}) {
        SetMetric(
            fields, names[index++], EventBandwidth(row.aic, event, bytes, aicFrequencyMhz, operationDurationUs),
            EventBandwidthReason(row.aic, {event}, aicFrequencyMhz, operationDurationUs));
    }
    return BuildRow(MemoryL0Header(), fields);
}

std::vector<std::string> MemoryUBHeader()
{
    return {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_ub_read_bw_vector(GB/s)",
        "aiv_ub_write_bw_vector(GB/s)",
        "aiv_ub_read_bw_gm(GB/s)",
        "aiv_ub_write_bw_gm(GB/s)"};
}

CsvRow MemoryUBRow(const RowMetrics& row, const ReportConfig& config, Metric operationDurationUs)
{
    CsvFields fields = CommonFields(row, config);
    const double aivFrequencyMhz = AivFrequencyMhz(config);
    SetMetric(
        fields, "aiv_ub_read_bw_vector(GB/s)",
        EventBandwidth(row.aiv, 0x571U, 256.0, aivFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aiv, {0x571U}, aivFrequencyMhz, operationDurationUs));
    SetMetric(
        fields, "aiv_ub_write_bw_vector(GB/s)",
        EventBandwidth(row.aiv, 0x572U, 256.0, aivFrequencyMhz, operationDurationUs),
        EventBandwidthReason(row.aiv, {0x572U}, aivFrequencyMhz, operationDurationUs));
    const auto xgu = NonNegativeDifference(row.aiv, 0x422U, {0x57fU, 0x580U});
    const auto duration = operationDurationUs.has_value() ? operationDurationUs : Time(row.aiv, aivFrequencyMhz);
    SetMetric(
        fields, "aiv_ub_read_bw_gm(GB/s)", std::nullopt,
        row.aiv.present ? MissingReason::DbiUnavailable : MissingReason::CoreTypeNotApplicable);
    SetMetric(
        fields, "aiv_ub_write_bw_gm(GB/s)",
        xgu.has_value() && duration.has_value() ? Bandwidth(*xgu * 128.0, *duration) : std::nullopt,
        EventBandwidthReason(row.aiv, {0x422U, 0x57fU, 0x580U}, aivFrequencyMhz, operationDurationUs));
    return BuildRow(MemoryUBHeader(), fields);
}

std::vector<std::string> PipeHeader()
{
    return {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_cube_time(us)",
        "aic_cube_ratio",
        "aic_scalar_time(us)",
        "aic_scalar_ratio",
        "aic_mte1_time(us)",
        "aic_mte1_ratio",
        "aic_mte1_active_bw(GB/s)",
        "aic_mte2_time(us)",
        "aic_mte2_ratio",
        "aic_mte2_active_bw(GB/s)",
        "aic_mte3_time(us)",
        "aic_mte3_ratio",
        "aic_mte3_active_bw(GB/s)",
        "aic_fixpipe_time(us)",
        "aic_fixpipe_ratio",
        "aic_fixpipe_active_bw(GB/s)",
        "aic_icache_miss_rate",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_vec_time(us)",
        "aiv_vec_ratio",
        "aiv_scalar_time(us)",
        "aiv_scalar_ratio",
        "aiv_mte2_time(us)",
        "aiv_mte2_ratio",
        "aiv_mte2_active_bw(GB/s)",
        "aiv_mte3_time(us)",
        "aiv_mte3_ratio",
        "aiv_mte3_active_bw(GB/s)",
        "aiv_icache_miss_rate",
    };
}

CsvRow PipeRow(const RowMetrics& row, const ReportConfig& config)
{
    CsvFields fields = CommonFields(row, config);
    const double aicFrequencyMhz = AicFrequencyMhz(config);
    const double aivFrequencyMhz = AivFrequencyMhz(config);
    constexpr uint32_t kA5CubeEvent = 810U;
    const auto setPipePair = [&](const char* timeName, const char* ratioName, const TypeMetrics& metrics,
                                 uint32_t eventId, double frequencyMhz) {
        SetMetric(
            fields, timeName, EventTime(metrics, eventId, frequencyMhz),
            EventBandwidthReason(metrics, {eventId}, frequencyMhz));
        SetMetric(fields, ratioName, ValueRatio(metrics, eventId), RatioReason(metrics, {eventId}));
    };
    setPipePair("aic_cube_time(us)", "aic_cube_ratio", row.aic, kA5CubeEvent, aicFrequencyMhz);
    setPipePair("aic_scalar_time(us)", "aic_scalar_ratio", row.aic, 0x1U, aicFrequencyMhz);
    setPipePair("aic_mte1_time(us)", "aic_mte1_ratio", row.aic, 0x702U, aicFrequencyMhz);
    setPipePair("aic_mte2_time(us)", "aic_mte2_ratio", row.aic, 0x202U, aicFrequencyMhz);
    setPipePair("aic_mte3_time(us)", "aic_mte3_ratio", row.aic, 0x203U, aicFrequencyMhz);
    setPipePair("aic_fixpipe_time(us)", "aic_fixpipe_ratio", row.aic, 0x714U, aicFrequencyMhz);
    SetMetric(fields, "aic_icache_miss_rate", IcacheMissRate(row.aic), EventReason(row.aic, {0x35U, 0x34U}));
    setPipePair("aiv_vec_time(us)", "aiv_vec_ratio", row.aiv, 0x501U, aivFrequencyMhz);
    setPipePair("aiv_scalar_time(us)", "aiv_scalar_ratio", row.aiv, 0x1U, aivFrequencyMhz);
    setPipePair("aiv_mte2_time(us)", "aiv_mte2_ratio", row.aiv, 0x202U, aivFrequencyMhz);
    setPipePair("aiv_mte3_time(us)", "aiv_mte3_ratio", row.aiv, 0x203U, aivFrequencyMhz);
    SetMetric(fields, "aiv_icache_miss_rate", IcacheMissRate(row.aiv), EventReason(row.aiv, {0x35U, 0x34U}));

    const auto xgu = NonNegativeDifference(row.aiv, 0x422U, {0x57fU, 0x580U});
    const auto xulBase = NonNegativeDifference(row.aic, 0x709U, {0x70eU});
    const auto aicMainRead = row.aic.Value(0x422U);
    const auto xul = xulBase.has_value() && aicMainRead.has_value() ?
                         Metric(std::max(0.0, *xulBase - std::floor(*aicMainRead / 2.0))) :
                         std::nullopt;
    const auto xfp = SumEvents(row.aic, {0x70cU, 0x70eU, 0x717U, 0x423U});
    SetMetric(
        fields, "aic_mte3_active_bw(GB/s)",
        ActiveBandwidthMetric(Scale(xul, 256.0), row.aic.Value(0x203U), aicFrequencyMhz),
        EventBandwidthReason(row.aic, {0x709U, 0x70eU, 0x422U, 0x203U}, aicFrequencyMhz));
    SetMetric(
        fields, "aiv_mte3_active_bw(GB/s)", std::nullopt,
        row.aiv.present ? MissingReason::DbiUnavailable : MissingReason::CoreTypeNotApplicable);
    SetMetric(
        fields, "aic_fixpipe_active_bw(GB/s)",
        ActiveBandwidthMetric(Scale(xfp, 128.0), row.aic.Value(0x714U), aicFrequencyMhz),
        EventBandwidthReason(row.aic, {0x70cU, 0x70eU, 0x717U, 0x423U, 0x714U}, aicFrequencyMhz));
    SetMetric(
        fields, "aiv_mte2_active_bw(GB/s)",
        ActiveBandwidthMetric(Scale(xgu, 128.0), row.aiv.Value(0x202U), aivFrequencyMhz),
        EventBandwidthReason(row.aiv, {0x422U, 0x57fU, 0x580U, 0x202U}, aivFrequencyMhz));
    SetMetric(
        fields, "aic_mte1_active_bw(GB/s)",
        ActiveBandwidthMetric(Scale(row.aic.Value(0x707U), 256.0), row.aic.Value(0x702U), aicFrequencyMhz),
        EventBandwidthReason(row.aic, {0x707U, 0x702U}, aicFrequencyMhz));
    SetMetric(
        fields, "aic_mte2_active_bw(GB/s)",
        ActiveBandwidthMetric(Scale(row.aic.Value(0x422U), 128.0), row.aic.Value(0x202U), aicFrequencyMhz),
        EventBandwidthReason(row.aic, {0x422U, 0x202U}, aicFrequencyMhz));
    return BuildRow(PipeHeader(), fields);
}

std::vector<std::string> ArithmeticHeader()
{
    return {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_cube_ratio",
        "aic_cube_fp_ratio",
        "aic_cube_int_ratio",
        "aic_cube_total_instr_number",
        "aic_cube_fp_instr_number",
        "aic_cube_int_instr_number",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_vec_ratio",
        "aiv_simd_vf_ratio",
        "aiv_vec_sfu_ratio",
        "aiv_simt_vf_ratio",
        "aiv_simd_vf_time",
        "aiv_simd_vf_ipc",
        "aiv_simt_vf_time",
        "aiv_simt_vf_ipc"};
}

CsvCell ComputeVfTime(const TypeMetrics& aiv, uint32_t cycleEvent, double frequencyMhz)
{
    MissingReason eventReason = EventReason(aiv, {cycleEvent});
    if (eventReason != MissingReason::Unknown) {
        return MissingMetricCell(eventReason);
    }

    eventReason = TimeReason(aiv, frequencyMhz);
    if (eventReason != MissingReason::Unknown) {
        return MissingMetricCell(eventReason);
    }

    const double cycles = *aiv.Value(cycleEvent);
    const double value = cycles / frequencyMhz;
    if (!std::isfinite(value)) {
        return MissingMetricCell(MissingReason::Unknown);
    }
    return MetricCell(value);
}

CsvCell ComputeVfIpc(const TypeMetrics& aiv, uint32_t cycleEvent, Metric count)
{
    MissingReason eventReason = EventReason(aiv, {cycleEvent});
    if (eventReason != MissingReason::Unknown) {
        return MissingMetricCell(eventReason);
    }

    if (!count) {
        return MissingMetricCell(MissingReason::EventMissing);
    }

    const Metric cycles = aiv.Value(cycleEvent);
    eventReason = ValidDenominatorReason({*cycles});
    if (eventReason != MissingReason::Unknown) {
        return MissingMetricCell(eventReason);
    }
    const double value = *count / *cycles;
    if (!std::isfinite(value)) {
        return MissingMetricCell(MissingReason::Unknown);
    }
    return MetricCell(value);
}

CsvCell ComputeSimdVfTime(const TypeMetrics& aiv, double frequencyMhz)
{
    return ComputeVfTime(aiv, 1282, frequencyMhz);
}

CsvCell ComputeSimdVfIpc(const TypeMetrics& aiv) { return ComputeVfIpc(aiv, 1282, aiv.Value(1320)); }

CsvCell ComputeSimtVfTime(const TypeMetrics& aiv, double frequencyMhz)
{
    return ComputeVfTime(aiv, 1284, frequencyMhz);
}

CsvCell ComputeSimtVfIpc(const TypeMetrics& aiv)
{
    return ComputeVfIpc(aiv, 1284, Scale(SumEvents(aiv, {1466, 1467, 1468}), 0.25));
}

CsvRow ArithmeticRow(const RowMetrics& row, const ReportConfig& config)
{
    CsvFields fields = CommonFields(row, config);
    for (const auto& [name, event] : std::initializer_list<std::pair<const char*, uint32_t>>{
             {"aic_cube_ratio", 810}, {"aic_cube_fp_ratio", 808}, {"aic_cube_int_ratio", 809}}) {
        SetMetric(fields, name, ValueRatio(row.aic, event), RatioReason(row.aic, {event}));
    }
    SetEvent(fields, "aic_cube_total_instr_number", row.aic, 768);
    SetEvent(fields, "aic_cube_fp_instr_number", row.aic, 789);
    SetEvent(fields, "aic_cube_int_instr_number", row.aic, 790);
    const Metric aivRatio = ValueRatio(row.aiv, 1281);
    const Metric simdVfRatio = ValueRatio(row.aiv, 1282);
    const Metric aivSfuRatio = ValueRatio(row.aiv, 1283);
    const Metric simtVfRatio = ValueRatio(row.aiv, 1284);
    for (const auto& [name, event, metric] : std::initializer_list<std::tuple<const char*, uint32_t, Metric>>{
             {"aiv_vec_ratio", 1281, aivRatio},
             {"aiv_simd_vf_ratio", 1282, simdVfRatio},
             {"aiv_vec_sfu_ratio", 1283, aivSfuRatio},
             {"aiv_simt_vf_ratio", 1284, simtVfRatio}}) {
        SetMetric(fields, name, metric, RatioReason(row.aiv, {event}));
    }
    const double frequencyMhz = AivFrequencyMhz(config);
    fields.emplace("aiv_simd_vf_time", ComputeSimdVfTime(row.aiv, frequencyMhz));
    fields.emplace("aiv_simd_vf_ipc", ComputeSimdVfIpc(row.aiv));
    fields.emplace("aiv_simt_vf_time", ComputeSimtVfTime(row.aiv, frequencyMhz));
    fields.emplace("aiv_simt_vf_ipc", ComputeSimtVfIpc(row.aiv));
    return BuildRow(ArithmeticHeader(), fields);
}

std::vector<std::string> ResourceConflictHeader()
{
    return {
        "block_id",
        "sub_block_id",
        "aic_time(us)",
        "aic_total_cycles",
        "aic_cube_wait_ratio",
        "aic_mte1_wait_ratio",
        "aic_mte2_wait_ratio",
        "aic_mte3_wait_ratio",
        "aiv_time(us)",
        "aiv_total_cycles",
        "aiv_vec_stu_cflt_ratio",
        "aiv_vec_ldu_cflt_ratio",
        "aiv_vec_sfu_cflt_ratio",
        "aiv_vec_wait_ratio",
        "aiv_mte2_wait_ratio",
        "aiv_mte3_wait_ratio"};
}

CsvRow ResourceConflictRow(const RowMetrics& row, const ReportConfig& config)
{
    CsvFields fields = CommonFields(row, config);
    for (const auto& [name, event] : std::initializer_list<std::pair<const char*, uint32_t>>{
             {"aic_cube_wait_ratio", 11},
             {"aic_mte1_wait_ratio", 13},
             {"aic_mte2_wait_ratio", 14},
             {"aic_mte3_wait_ratio", 15}}) {
        SetMetric(fields, name, ValueRatio(row.aic, event), RatioReason(row.aic, {event}));
    }
    for (const auto& [name, event] : std::initializer_list<std::pair<const char*, uint32_t>>{
             {"aiv_vec_stu_cflt_ratio", 1366},
             {"aiv_vec_ldu_cflt_ratio", 1344},
             {"aiv_vec_wait_ratio", 12},
             {"aiv_mte2_wait_ratio", 14},
             {"aiv_mte3_wait_ratio", 15}}) {
        SetMetric(fields, name, ValueRatio(row.aiv, event), RatioReason(row.aiv, {event}));
    }
    // Ascend950 SFU conflicts combine four counters before normalization.
    const auto sfu = SumEvents(row.aiv, {1376, 1377, 1378, 1379});
    SetMetric(
        fields, "aiv_vec_sfu_cflt_ratio", sfu.has_value() ? Ratio(*sfu, row.aiv.Cycles()) : std::nullopt,
        RatioReason(row.aiv, {1376, 1377, 1378, 1379}));
    return BuildRow(ResourceConflictHeader(), fields);
}

struct SectionWriter {
    std::vector<std::string> (*header)();
    std::function<CsvRow(const RowMetrics&, const ReportConfig&, Metric)> row;
};

const std::map<std::string_view, SectionWriter>& SectionWriters()
{
    static const std::map<std::string_view, SectionWriter> writers = {
        {"ArithmeticUtilization",
         {&ArithmeticHeader,
          [](const RowMetrics& row, const ReportConfig& config, Metric) { return ArithmeticRow(row, config); }}},
        {"ResourceConflictRatio",
         {&ResourceConflictHeader,
          [](const RowMetrics& row, const ReportConfig& config, Metric) { return ResourceConflictRow(row, config); }}},
        {"L2Cache",
         {&L2Header, [](const RowMetrics& row, const ReportConfig& config, Metric) { return L2Row(row, config); }}},
        {"Memory",
         {&MemoryHeader, [](const RowMetrics& row, const ReportConfig& config,
                            Metric duration) { return MemoryRow(row, config, duration); }}},
        {"MemoryL0",
         {&MemoryL0Header, [](const RowMetrics& row, const ReportConfig& config,
                              Metric duration) { return MemoryL0Row(row, config, duration); }}},
        {"MemoryUB",
         {&MemoryUBHeader, [](const RowMetrics& row, const ReportConfig& config,
                              Metric duration) { return MemoryUBRow(row, config, duration); }}},
        {"PipeUtilization",
         {&PipeHeader, [](const RowMetrics& row, const ReportConfig& config, Metric) { return PipeRow(row, config); }}},
    };
    return writers;
}

const SectionWriter* FindWriter(std::string_view section)
{
    const auto& writers = SectionWriters();
    const auto it = writers.find(section);
    return it == writers.end() ? nullptr : &it->second;
}

} // namespace

std::vector<std::string> PmuMetricBuilder::Header(std::string_view section)
{
    const auto* writer = FindWriter(section);
    return writer == nullptr ? std::vector<std::string>{} : writer->header();
}

PmuMetricRow PmuMetricBuilder::Build(
    std::string_view section, const aclptiBlockKey& key, const aclptiPmuDataRow& row, const ReportConfig& config,
    std::optional<double> duration)
{
    const auto* writer = FindWriter(section);
    return writer == nullptr ? PmuMetricRow{} : writer->row(MakeRowMetrics(key, row), config, duration);
}

std::optional<double> PmuMetricBuilder::TaskDuration(const aclptiProfilingDataResult& result)
{
    return TaskLogDurationUs(result);
}

std::optional<double> PmuMetricBuilder::OperationDuration(const aclptiProfilingDataResult& result, PmuDataLevel level)
{
    const auto& logs = level == PmuDataLevel::Task ? result.taskPmuLogs : result.pmuLogs;
    bool hasAic = false;
    bool hasAiv = false;
    for (const auto& [key, row] : logs) {
        hasAic = hasAic || key.coreType == ACLPTI_CORE_TYPE_AIC;
        hasAiv = hasAiv || key.coreType == ACLPTI_CORE_TYPE_AIV;
    }
    return hasAic && hasAiv ? TaskDuration(result) : std::nullopt;
}

const char* PmuMetricBuilder::ReasonName(MissingReason reason) { return MissingReasonName(reason); }

} // namespace npucompute
