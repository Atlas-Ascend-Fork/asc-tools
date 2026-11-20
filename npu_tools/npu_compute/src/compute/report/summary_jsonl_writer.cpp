/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "report/report_writer.h"
#include "artifact_sink.h"
#include "pmu/pmu_metric_builder.h"
#include "report/summary_metrics.h"
#include "performance_hints/performance_hint_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cxxabi.h>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <unistd.h>
namespace npucompute {
namespace {

using Metric = std::optional<double>;
constexpr int kSummaryDecimalPlaces = 6;
constexpr uint32_t kB8BitWidth = 8U;
constexpr uint32_t kB16BitWidth = 16U;
constexpr uint32_t kB32BitWidth = 32U;
constexpr double kBitsPerByte = 8.0;
constexpr int kRooflineDecimalPlaces = 17;

std::string DemangleName(const std::string& name)
{
    constexpr std::string_view kMangledNamePrefix = "_Z";
    if (name.compare(0, kMangledNamePrefix.size(), kMangledNamePrefix) != 0) {
        return name;
    }
    int status = 0;
    const std::unique_ptr<char, decltype(&std::free)> demangled(
        abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status), &std::free);
    return status == 0 && demangled ? std::string(demangled.get()) : name;
}

std::string Quote(std::string_view value)
{
    constexpr unsigned char kAsciiControlCharacterEnd = 0x20;
    constexpr int kUnicodeEscapeHexDigits = 4;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << '"';
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output << '\\' << character;
        } else if (character < kAsciiControlCharacterEnd) {
            output << "\\u" << std::hex << std::setw(kUnicodeEscapeHexDigits) << std::setfill('0')
                   << static_cast<unsigned>(character);
        } else {
            output << character;
        }
    }
    output << '"';
    return output.str();
}

std::string Number(Metric value, bool integer = false, int precision = kSummaryDecimalPlaces)
{
    if (!value || !std::isfinite(*value)) {
        return "\"NA\"";
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(integer ? 0 : precision) << *value;
    return output.str();
}

using Fields = SummaryFields;

Fields Section(
    std::ostream& output, const std::string& section, const aclptiProfilingDataResult& result,
    const ReportConfig& config)
{
    const auto header = PmuMetricBuilder::Header(section, PmuDataLevel::Task);
    auto fields = BuildTaskSummarySection(section, result, config);
    output << "{\"category\":" << Quote(section);
    for (const auto& name : header) {
        const auto& field = fields.at(name);
        output << ',' << Quote(name) << ':' << Number(field.value, field.integer);
    }
    output << "}\n";
    return fields;
}

Metric Sum(Metric left, Metric right)
{
    // OpInfo bandwidth totals retain any available side, treating an unavailable side as zero.
    // This does not distinguish an absent core side from missing events on a present side,
    // unlike the stricter main-memory traffic totals in MemoryRow.
    return !left && !right ? std::nullopt : Metric(left.value_or(0) + right.value_or(0));
}

Metric Frequency(bool hasAic, bool hasAiv, Metric aic, Metric aiv)
{
    if (hasAic && hasAiv) {
        return aic && aiv && std::abs(*aic - *aiv) <= std::numeric_limits<double>::epsilon() ? aic : std::nullopt;
    }
    return hasAic ? aic : hasAiv ? aiv : std::nullopt;
}

void WriteSamplingContexts(std::ostream& output, const aclptiProfilingDataResult& result)
{
    if (!result.pmSamplingContexts.empty()) {
        output << ",\"PmSamplingContexts\":[";
        bool first = true;
        for (const auto& entry : result.pmSamplingContexts) {
            if (!entry.second) {
                continue;
            }
            const auto& context = *entry.second;
            output << (first ? "" : ",") << "{\"replay_id\":" << Quote(std::to_string(entry.first))
                   << ",\"original_entry\":" << Quote(std::to_string(context.originalEntry))
                   << ",\"companion_entry\":" << Quote(std::to_string(context.elfEntry))
                   << ",\"pc_offset\":" << Quote(std::to_string(context.pcOffset)) << '}';
            first = false;
        }
        output << ']';
    }
}

void WriteOpPerformance(
    std::ostream& output, const aclptiProfilingDataResult& result, const std::vector<std::string>& sections,
    const ReportConfig& config, const KernelMetadata& metadata, Fields memory)
{
    const auto selected = [&sections](std::string_view section) {
        return std::find(sections.begin(), sections.end(), section) != sections.end();
    };
    const auto metrics = BuildSummaryMetrics(result, config, metadata);
    const Metric read = Sum(memory["aic_main_mem_read_bw(GB/s)"].value, memory["aiv_main_mem_read_bw(GB/s)"].value);
    const Metric write = Sum(memory["aic_main_mem_write_bw(GB/s)"].value, memory["aiv_main_mem_write_bw(GB/s)"].value);
    constexpr double kTheoreticalGmBandwidthGbps = 1600.0;
    constexpr double kPercentScale = 100.0;
    const Metric usage =
        read && write ? Metric((*read + *write) / kTheoreticalGmBandwidthGbps * kPercentScale) : std::nullopt;
    const auto field =
        [&output](std::string_view name, Metric value, bool integer = false, int precision = kSummaryDecimalPlaces) {
            output << ',' << Quote(name) << ':' << Number(value, integer, precision);
        };
    constexpr int kTheoreticalFlopsDecimalPlaces = 2;
    field("aic_flops_theoretical", metrics.aicFlopsTheoretical, false, kTheoreticalFlopsDecimalPlaces);
    field("aiv_flops_theoretical", metrics.aivFlopsTheoretical, false, kTheoreticalFlopsDecimalPlaces);
    field("aic_flops", metrics.aicFlops);
    field("aiv_flops", metrics.aivFlops);
    field("aicore_parallel_utilization", metrics.parallelUtilization);
    field("aicore_parallel_balance", metrics.parallelBalance);
    field(
        "aicore_gm_bw_theoretical(GB/s)", selected("Memory") ? Metric(kTheoreticalGmBandwidthGbps) : std::nullopt,
        true);
    field("aicore_gm_read_bw(GB/s)", read);
    field("aicore_gm_write_bw(GB/s)", write);
    field("aicore_gm_bw_usage_rate(%)", usage);
}

double RooflineClock(const KernelMetadata& metadata, const ReportConfig& config, bool aic)
{
    constexpr double kFallbackFrequencyMhz = 1650.0;
    const auto current = metadata.hardware.collected ? (aic ? metadata.hardware.currentAicFrequencyMhz :
                                                              metadata.hardware.currentAivFrequencyMhz) :
                                                       Metric(aic ? config.aicFrequencyMhz : config.aivFrequencyMhz);
    if (current && std::isfinite(*current) && *current > 0) {
        return *current;
    }
    const auto rated = SummaryRatedFrequency(metadata, aic);
    if (rated) {
        return *rated;
    }
    ASCTOOL_DEBUG(
        "[npu-compute] Roofline %s frequency source=fallback valueMHz=%f", aic ? "AIC" : "AIV", kFallbackFrequencyMhz);
    return kFallbackFrequencyMhz;
}

using RooflineWork = std::map<std::pair<aclptiRooflineChannel, uint32_t>, double>;

RooflineWork MergeRooflineWork(const aclptiRooflineData* data, bool& complete)
{
    RooflineWork work;
    if (data == nullptr) {
        return work;
    }
    for (const auto& item : data->work) {
        if ((item.bitWidth != kB8BitWidth && item.bitWidth != kB16BitWidth && item.bitWidth != kB32BitWidth) ||
            !std::isfinite(item.operations) || item.operations < 0 ||
            (item.channel != aclptiRooflineChannel::Simd && item.channel != aclptiRooflineChannel::Simt &&
             item.channel != aclptiRooflineChannel::Cube)) {
            complete = false;
            continue;
        }
        if (item.operations > 0) {
            work[{item.channel, item.bitWidth}] += item.operations;
        }
    }
    return work;
}

SummaryFields RooflinePoint(
    aclptiRooflineChannel channel, std::optional<uint32_t> bitWidth, double operations, Metric bytes,
    const KernelMetadata& metadata, double frequencyMhz, Metric taskDuration)
{
    constexpr double kHzPerMhz = 1000000.0;
    constexpr double kVectorB8OperationsPerCycle = 256.0 * 2.0;
    constexpr double kCubeB8OperationsPerCycle = 8192.0 * 2.0;
    constexpr double kSimdAndCubeBandwidthGbps = 1600.0;
    constexpr double kSimtBandwidthGbps = 800.0;
    const bool cube = channel == aclptiRooflineChannel::Cube;
    const double bandwidthGbps =
        channel == aclptiRooflineChannel::Simt ? kSimtBandwidthGbps : kSimdAndCubeBandwidthGbps;
    const auto cores = cube ? metadata.hardware.cubeCoreCount : metadata.hardware.vectorCoreCount;
    const Metric peak = cores && *cores > 0 && bitWidth ?
                            Metric(
                                *cores * (cube ? kCubeB8OperationsPerCycle : kVectorB8OperationsPerCycle) *
                                kBitsPerByte / *bitWidth * frequencyMhz * kHzPerMhz) :
                            std::nullopt;
    return {
        {"p_peak", {peak}},
        {"b_peak", {bandwidthGbps}},
        {"w", {std::isfinite(operations) ? Metric(operations) : std::nullopt}},
        {"t", {taskDuration}},
        {"q", {bytes}},
    };
}

void WriteRooflinePoint(std::ostream& output, const std::string& name, const SummaryFields& fields)
{
    output << ',' << Quote(name) << ":{";
    bool first = true;
    for (const char* field : {"p_peak", "b_peak", "w", "t", "q"}) {
        const auto& value = fields.at(field);
        output << (first ? "" : ",") << Quote(field) << ':';
        if (value.value && std::isfinite(*value.value)) {
            // Default notation retains sub-microsecond t without decimal truncation.
            output << std::setprecision(kRooflineDecimalPlaces) << *value.value;
        } else {
            output << "\"NA\"";
        }
        first = false;
    }
    output << '}';
}

bool Roofline(
    std::ostream& output, const aclptiProfilingDataResult& result, const ReportConfig& config,
    const KernelMetadata& metadata)
{
    const auto aicClock = RooflineClock(metadata, config, true);
    const auto aivClock = RooflineClock(metadata, config, false);
    constexpr double kMicrosecondsPerSecond = 1000000.0;
    const auto durationUs =
        result.rooflineReplayId ? PmuMetricBuilder::TaskDuration(result, *result.rooflineReplayId) : std::nullopt;
    const Metric taskDuration = durationUs ? Metric(*durationUs / kMicrosecondsPerSecond) : std::nullopt;
    const auto* data = result.rooflineData ? &*result.rooflineData : nullptr;
    Metric bytes = data ? data->gmBytes : std::nullopt;
    if (bytes && (!std::isfinite(*bytes) || *bytes < 0)) {
        bytes.reset();
    }
    bool complete = data && data->status == ACLPTI_SUCCESS && data->errors.empty() && bytes.has_value();
    const auto work = MergeRooflineWork(data, complete);
    bool any = false;
    output << "{\"category\":\"Roofline\"";
    // 按位宽输出
    for (const auto& [key, operations] : work) {
        const auto channel = key.first;
        const uint32_t bw = key.second;
        std::string name;
        double clock = 0.0;
        if (channel == aclptiRooflineChannel::Simd) {
            name = "aiv_simd_b" + std::to_string(bw);
            clock = aivClock;
        } else if (channel == aclptiRooflineChannel::Simt) {
            name = "aiv_simt_b" + std::to_string(bw);
            clock = aivClock;
        } else if (channel == aclptiRooflineChannel::Cube) {
            name = "aic_b" + std::to_string(bw);
            clock = aicClock;
        } else {
            continue;
        }
        const auto fields = RooflinePoint(channel, bw, operations, bytes, metadata, clock, taskDuration);
        for (const auto& [field, value] : fields) {
            complete = complete && value.value && std::isfinite(*value.value);
        }
        WriteRooflinePoint(output, name, fields);
        any = true;
    }
    complete = complete && any;
    output << "}\n";
    return complete;
}

void OpInfo(
    std::ostream& output, const aclptiProfilingDataResult& result, const std::vector<std::string>& sections,
    const ReportConfig& config, const KernelMetadata& metadata, Fields memory)
{
    bool hasAic = result.taskPmuData.aic.has_value();
    bool hasAiv = result.taskPmuData.aiv.has_value();
    if (metadata.kind) {
        hasAic = *metadata.kind != KernelKind::Vector;
        hasAiv = *metadata.kind != KernelKind::Cube;
    }
    const Metric ratedAic = SummaryRatedFrequency(metadata, true);
    const Metric ratedAiv = SummaryRatedFrequency(metadata, false);
    const Metric currentAic = SummaryEffectiveFrequency(metadata, config, true);
    const Metric currentAiv = SummaryEffectiveFrequency(metadata, config, false);
    output << "{\"category\":\"OpInfoSummary\",\"Op Name\":"
           << (metadata.name ? Quote(DemangleName(*metadata.name)) : "\"NA\"") << ",\"Op Type\":"
           << (hasAic ? (hasAiv ? "\"mix\"" : "\"cube\"") :
               hasAiv ? "\"vector\"" :
                        "\"NA\"");
    const auto field =
        [&output](std::string_view name, Metric value, bool integer = false, int precision = kSummaryDecimalPlaces) {
            output << ',' << Quote(name) << ':' << Number(value, integer, precision);
        };
    field("Task Duration(us)", PmuMetricBuilder::TaskDuration(result));
    field("Block Dim", metadata.blockDim ? Metric(*metadata.blockDim) : std::nullopt, true);
    field(
        "Mix Block Dim", hasAic && hasAiv && metadata.mixBlockDim ? Metric(*metadata.mixBlockDim) : std::nullopt, true);
    field("Device Id", metadata.deviceId ? Metric(*metadata.deviceId) : std::nullopt, true);
    field("Pid", ::getpid(), true);
    field("Current Freq", Frequency(hasAic, hasAiv, currentAic, currentAiv));
    field("Rated Freq", Frequency(hasAic, hasAiv, ratedAic, ratedAiv));
    WriteOpPerformance(output, result, sections, config, metadata, std::move(memory));
    WriteSamplingContexts(output, result);
    output << "}\n";
}

} // namespace

aclptiResult WriteSummaryJsonl(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    const KernelMetadata& metadata, ArtifactSink& sink, std::string_view artifactPrefix, bool* degraded)
{
    bool artifactStarted = false;
    try {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        auto selectedSections = sections;
        std::sort(selectedSections.begin(), selectedSections.end());
        selectedSections.erase(std::unique(selectedSections.begin(), selectedSections.end()), selectedSections.end());
        const auto summaryConfig = SummaryReportConfig(metadata, config);
        HintSections summarySections;
        for (const auto& section : selectedSections) {
            if (section == "Roofline") {
                const bool complete = Roofline(output, result, config, metadata);
                if (!complete && degraded != nullptr) {
                    *degraded = true;
                }
                continue;
            }
            if (PmuMetricBuilder::Header(section).empty()) {
                return ACLPTI_ERROR_NOT_SUPPORTED;
            }
            summarySections.emplace(section, Section(output, section, result, summaryConfig));
        }
        std::vector<std::string> hints;
        try {
            hints = AnalyzePerformanceHints(result, summarySections, config, metadata).hints;
        } catch (const std::exception& error) {
            ASCTOOL_WARNING("[npu-compute] performance hint analysis failed: %s", error.what());
        } catch (...) {
            ASCTOOL_WARNING("[npu-compute] performance hint analysis failed: unexpected exception");
        }
        output << "{\"category\":\"PerformanceHints\",\"hints\":[";
        for (size_t index = 0; index < hints.size(); ++index) {
            output << (index == 0 ? "" : ",") << Quote(hints[index]);
        }
        output << "]}\n";
        OpInfo(output, result, selectedSections, config, metadata, summarySections["Memory"]);
        const std::string content = output.str();
        sink.Begin(std::string(artifactPrefix) + "summary.jsonl");
        artifactStarted = true;
        sink.Write(content);
        // PerformanceHints and OpInfoSummary are emitted in addition to the selected sections.
        constexpr std::size_t kNonSectionSummaryRecords = 2;
        sink.Commit(selectedSections.size() + kNonSectionSummaryRecords);
        artifactStarted = false;
        return ACLPTI_SUCCESS;
    } catch (const std::exception& error) {
        if (artifactStarted) {
            sink.Abort();
        }
        ASCTOOL_ERROR(
            "[npu-compute] summary failed: %s"
            " code=%u",
            error.what(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_INTERNAL;
    }
}

} // namespace npucompute
