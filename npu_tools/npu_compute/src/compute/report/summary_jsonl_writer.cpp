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

std::string DemangleName(const std::string& name)
{
    if (name.compare(0, 2, "_Z") != 0) {
        return name;
    }
    int status = 0;
    const std::unique_ptr<char, decltype(&std::free)> demangled(
        abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status), &std::free);
    return status == 0 && demangled ? std::string(demangled.get()) : name;
}

std::string Quote(std::string_view value)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << '"';
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') {
            output << '\\' << character;
        } else if (character < 0x20) {
            output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character);
        } else {
            output << character;
        }
    }
    output << '"';
    return output.str();
}

std::string Number(Metric value, bool integer = false, int precision = 6)
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
        return aic && aiv && *aic == *aiv ? aic : std::nullopt;
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
    const auto selected = [&](std::string_view section) {
        return std::find(sections.begin(), sections.end(), section) != sections.end();
    };
    const auto metrics = BuildSummaryMetrics(result, config, metadata);
    const Metric read = Sum(memory["aic_main_mem_read_bw(GB/s)"].value, memory["aiv_main_mem_read_bw(GB/s)"].value);
    const Metric write = Sum(memory["aic_main_mem_write_bw(GB/s)"].value, memory["aiv_main_mem_write_bw(GB/s)"].value);
    const Metric usage = read && write ? Metric((*read + *write) / 1600.0 * 100.0) : std::nullopt;
    const auto field = [&output](std::string_view name, Metric value, bool integer = false, int precision = 6) {
        output << ',' << Quote(name) << ':' << Number(value, integer, precision);
    };
    field("aic_flops_theoretical", metrics.aicFlopsTheoretical, false, 2);
    field("aiv_flops_theoretical", metrics.aivFlopsTheoretical, false, 2);
    field("aic_flops", metrics.aicFlops);
    field("aiv_flops", metrics.aivFlops);
    field("aicore_parallel_utilization", metrics.parallelUtilization);
    field("aicore_parallel_balance", metrics.parallelBalance);
    field("aicore_gm_bw_theoretical(GB/s)", selected("Memory") ? Metric(1600) : std::nullopt, true);
    field("aicore_gm_read_bw(GB/s)", read);
    field("aicore_gm_write_bw(GB/s)", write);
    field("aicore_gm_bw_usage_rate(%)", usage);
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
    const auto field = [&output](std::string_view name, Metric value, bool integer = false, int precision = 6) {
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
    const KernelMetadata& metadata, ArtifactSink& sink, std::string_view artifactPrefix)
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
        sink.Commit(selectedSections.size() + 2U);
        artifactStarted = false;
        return ACLPTI_SUCCESS;
    } catch (const std::exception& error) {
        if (artifactStarted) {
            sink.Abort();
        }
        ASCTOOL_ERROR(
            "[npu-compute] summary failed: %s"
            " code=%u",
            error.what(), static_cast<unsigned>(1));
        return ACLPTI_ERROR_INTERNAL;
    }
}

} // namespace npucompute
