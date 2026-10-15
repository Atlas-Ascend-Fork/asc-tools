/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/report_writer.h"
#include "artifact_sink.h"
#include "pmu/pmu_metric_builder.h"
#include "report/summary_metrics.h"
#include "common/debug_log.h"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cxxabi.h>
#include <fcntl.h>
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
    const auto header = PmuMetricBuilder::Header(section);
    auto fields = AggregateSummarySection(section, result, config);
    output << "{\"category\":" << Quote(section);
    for (std::size_t index = 2; index < header.size(); ++index) {
        const auto& field = fields.at(header[index]);
        output << ',' << Quote(header[index]) << ':' << Number(field.value, field.integer);
    }
    output << "}\n";
    return fields;
}

Metric Sum(Metric left, Metric right)
{
    return !left && !right ? std::nullopt : Metric(left.value_or(0) + right.value_or(0));
}

Metric Frequency(bool hasAic, bool hasAiv, Metric aic, Metric aiv)
{
    if (hasAic && hasAiv) {
        return aic && aiv && *aic == *aiv ? aic : std::nullopt;
    }
    return hasAic ? aic : hasAiv ? aiv : std::nullopt;
}

void OpInfo(
    std::ostream& output, const aclptiProfilingDataResult& result, const std::vector<std::string>& sections,
    const ReportConfig& config, const KernelMetadata& metadata, Fields memory)
{
    bool hasAic = false;
    bool hasAiv = false;
    for (const auto& [key, row] : result.taskPmuLogs) {
        hasAic = hasAic || key.coreType == ACLPTI_CORE_TYPE_AIC;
        hasAiv = hasAiv || key.coreType == ACLPTI_CORE_TYPE_AIV;
    }
    if (metadata.kind) {
        hasAic = *metadata.kind != KernelKind::Vector;
        hasAiv = *metadata.kind != KernelKind::Cube;
    }
    const Metric ratedAic = SummaryRatedFrequency(metadata, true);
    const Metric ratedAiv = SummaryRatedFrequency(metadata, false);
    const Metric currentAic = SummaryEffectiveFrequency(metadata, config, true);
    const Metric currentAiv = SummaryEffectiveFrequency(metadata, config, false);
    const auto selected = [&](std::string_view section) {
        return std::find(sections.begin(), sections.end(), section) != sections.end();
    };
    const auto metrics = BuildSummaryMetrics(result, config, metadata);
    const Metric read = Sum(memory["aic_main_mem_read_bw(GB/s)"].value, memory["aiv_main_mem_read_bw(GB/s)"].value);
    const Metric write = Sum(memory["aic_main_mem_write_bw(GB/s)"].value, memory["aiv_main_mem_write_bw(GB/s)"].value);
    const Metric usage = read && write ? Metric((*read + *write) / 1600.0 * 100.0) : std::nullopt;
    output << "{\"category\":\"OpInfoSummary\",\"Op Name\":"
           << (metadata.name ? Quote(DemangleName(*metadata.name)) : "\"NA\"") << ",\"Op Type\":"
           << (hasAic ? (hasAiv ? "\"mix\"" : "\"cube\"") :
               hasAiv ? "\"vector\"" :
                        "\"NA\"");
    const auto field = [&](std::string_view name, Metric value, bool integer = false, int precision = 6) {
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
    output << "}\n";
}

bool Publish(const boost::filesystem::path& directory, const std::string& content)
{
    std::string temporary = (directory / "summary.jsonl.tmp.XXXXXX").string();
    const int descriptor = ::mkstemp(temporary.data());
    if (descriptor < 0) {
        return false;
    }
    bool success = true;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = ::write(descriptor, content.data() + offset, content.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            success = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
        success = false;
    }
    if (::close(descriptor) != 0) {
        success = false;
    }
    if (success) {
        success = ::rename(temporary.c_str(), (directory / "summary.jsonl").c_str()) == 0;
    }
    if (!success) {
        ::unlink(temporary.c_str());
        return false;
    }
    const int directoryFd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directoryFd < 0) {
        return false;
    }
    success = ::fsync(directoryFd) == 0;
    return ::close(directoryFd) == 0 && success;
}

} // namespace

aclptiResult WriteSummaryJsonl(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    const KernelMetadata& metadata, ArtifactSink* sink, std::string_view artifactPrefix)
{
    bool artifactStarted = false;
    try {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        auto selectedSections = sections;
        std::sort(selectedSections.begin(), selectedSections.end());
        selectedSections.erase(std::unique(selectedSections.begin(), selectedSections.end()), selectedSections.end());
        const auto summaryConfig = SummaryReportConfig(metadata, config);
        Fields memory;
        for (const auto& section : selectedSections) {
            if (PmuMetricBuilder::Header(section).empty()) {
                return ACLPTI_ERROR_NOT_SUPPORTED;
            }
            auto values = Section(output, section, result, summaryConfig);
            if (section == "Memory") {
                memory = std::move(values);
            }
        }
        OpInfo(output, result, selectedSections, config, metadata, std::move(memory));
        const std::string content = output.str();
        if (sink != nullptr) {
            sink->Begin(std::string(artifactPrefix) + "summary.jsonl");
            artifactStarted = true;
            sink->Write(content);
            sink->Commit(selectedSections.size() + 1U);
            artifactStarted = false;
        } else if (!Publish(config.outputDirectory, content)) {
            detail::DebugLog(
                "npu-compute", "summary publication failed: %s errno=%d", config.outputDirectory.c_str(), errno);
            return ACLPTI_ERROR_INTERNAL;
        }
        return ACLPTI_SUCCESS;
    } catch (const std::exception& error) {
        if (artifactStarted) {
            sink->Abort();
        }
        detail::DebugLog("npu-compute", "summary failed: %s", error.what());
        return ACLPTI_ERROR_INTERNAL;
    }
}

} // namespace npucompute
