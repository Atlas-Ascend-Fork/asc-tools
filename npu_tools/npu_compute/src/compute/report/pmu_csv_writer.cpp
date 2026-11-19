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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace npucompute {
namespace {

using CsvRow = PmuMetricRow;
using Metric = std::optional<double>;
struct CsvSectionStats {
    std::size_t rows = 0;
    std::size_t totalFields = 0;
    std::size_t missingFields = 0;
    std::size_t missingRows = 0;
    std::size_t mismatchedRows = 0;
    std::map<std::string, std::size_t> missingColumns;
    std::map<std::string, std::size_t> missingReasons;
    std::map<std::string, std::size_t> missingColumnReasons;
};

struct CsvWriteContext {
    const ReportConfig& config;
    ArtifactSink& sink;
    std::string_view artifactPrefix;
    bool* degraded = nullptr;
    bool artifactStarted = false;
};

double AicFrequencyMhz(const ReportConfig& config)
{
    return config.aicFrequencyMhz > 0.0 ? config.aicFrequencyMhz : config.frequencyMhz;
}
double AivFrequencyMhz(const ReportConfig& config)
{
    return config.aivFrequencyMhz > 0.0 ? config.aivFrequencyMhz : config.frequencyMhz;
}
void WriteCsvLine(std::ostream& output, const std::vector<std::string>& values)
{
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << values[index];
    }
    output << '\n';
}

void WriteCsvLine(std::ostream& output, const CsvRow& values)
{
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << values[index].text;
    }
    output << '\n';
}

void UpdateMissingStats(const std::vector<std::string>& header, const CsvRow& values, CsvSectionStats* stats)
{
    if (stats == nullptr) {
        return;
    }
    ++stats->rows;
    stats->totalFields += header.size();
    bool rowMissing = false;
    if (values.size() != header.size()) {
        ++stats->mismatchedRows;
    }
    const std::size_t commonSize = std::min(header.size(), values.size());
    for (std::size_t index = 0; index < commonSize; ++index) {
        if (values[index].text != "NA") {
            continue;
        }
        ++stats->missingFields;
        ++stats->missingColumns[header[index]];
        const char* reasonName = PmuMetricBuilder::ReasonName(values[index].missingReason);
        ++stats->missingReasons[reasonName];
        ++stats->missingColumnReasons[header[index] + "@" + reasonName];
        rowMissing = true;
    }
    for (std::size_t index = values.size(); index < header.size(); ++index) {
        ++stats->missingFields;
        ++stats->missingColumns[header[index]];
        const char* reasonName = PmuMetricBuilder::ReasonName(MissingReason::RowSizeMismatch);
        ++stats->missingReasons[reasonName];
        ++stats->missingColumnReasons[header[index] + "@" + reasonName];
        rowMissing = true;
    }
    if (rowMissing) {
        ++stats->missingRows;
    }
}

std::string FormatMissingColumns(const std::map<std::string, std::size_t>& missingColumns)
{
    if (missingColumns.empty()) {
        return "none";
    }
    std::ostringstream output;
    bool first = true;
    for (const auto& [column, count] : missingColumns) {
        if (!first) {
            output << ';';
        }
        output << column << ':' << count;
        first = false;
    }
    return output.str();
}

std::string FormatMissingReasons(const std::map<std::string, std::size_t>& missingReasons)
{
    if (missingReasons.empty()) {
        return "none";
    }
    std::ostringstream output;
    bool first = true;
    for (const auto& [reason, count] : missingReasons) {
        if (!first) {
            output << ';';
        }
        output << reason << ':' << count;
        first = false;
    }
    return output.str();
}

bool IsEmptySuccessfulResult(const aclptiProfilingDataResult& result)
{
    return result.status == ACLPTI_SUCCESS && result.taskLogs.empty() && result.blockLogs.empty() &&
           result.pmuLogs.empty() && !result.taskPmuData.aic && !result.taskPmuData.aiv &&
           result.errorStats.failedRecordCount == 0;
}

void LogSectionStats(const std::string& section, const CsvSectionStats& stats)
{
    const std::string missingColumns = FormatMissingColumns(stats.missingColumns);
    const std::string missingReasons = FormatMissingReasons(stats.missingReasons);
    const std::string missingColumnReasons = FormatMissingReasons(stats.missingColumnReasons);
    ASCTOOL_DEBUG(
        "[npu-compute] CSV section data availability: section=%s rows=%zu fields=%zu missingFields=%zu "
        "missingRows=%zu "
        "mismatchedRows=%zu missingColumns=%s missingReasons=%s missingColumnReasons=%s",
        section.c_str(), stats.rows, stats.totalFields, stats.missingFields, stats.missingRows, stats.mismatchedRows,
        missingColumns.c_str(), missingReasons.c_str(), missingColumnReasons.c_str());
    ASCTOOL_DEBUG("[npu-compute] CSV section write complete: section=%s", section.c_str());
}

aclptiResult WriteCsvSection(
    const aclptiProfilingDataResult& result, const std::string& section, CsvWriteContext& context,
    Metric operationDurationUs)
{
    ASCTOOL_DEBUG("[npu-compute] CSV section write start: section=%s rows=%zu", section.c_str(), result.pmuLogs.size());
    context.sink.Begin(std::string(context.artifactPrefix) + section + ".csv");
    context.artifactStarted = true;
    ArtifactStreamBuffer sinkBuffer(context.sink);
    std::ostream output(&sinkBuffer);
    const std::vector<std::string> header = PmuMetricBuilder::Header(section);
    CsvSectionStats stats;
    WriteCsvLine(output, header);
    for (const auto& [key, row] : result.pmuLogs) {
        const auto values = PmuMetricBuilder::BuildBlock(section, key, row, context.config, operationDurationUs);
        UpdateMissingStats(header, values, &stats);
        WriteCsvLine(output, values);
    }

    output.flush();
    if (!output.good()) {
        context.sink.Abort();
        context.artifactStarted = false;
        ASCTOOL_ERROR(
            "[npu-compute] CSV section write failed: flush section=%s code=%u", section.c_str(),
            static_cast<unsigned>(ACLPTI_ERROR_CSV_WRITE));
        return ACLPTI_ERROR_CSV_WRITE;
    }
    context.sink.Commit(stats.rows);
    context.artifactStarted = false;
    if (context.degraded != nullptr && (stats.missingFields != 0 || stats.mismatchedRows != 0)) {
        *context.degraded = true;
    }
    LogSectionStats(section, stats);
    return ACLPTI_SUCCESS;
}

Metric CsvOperationDuration(const aclptiProfilingDataResult& result)
{
    bool hasAic = false;
    bool hasAiv = false;
    for (const auto& [key, row] : result.pmuLogs) {
        hasAic = hasAic || key.coreType == ACLPTI_CORE_TYPE_AIC;
        hasAiv = hasAiv || key.coreType == ACLPTI_CORE_TYPE_AIV;
    }

    const bool isMixedKernel = hasAic && hasAiv;
    const Metric operationDurationUs = isMixedKernel ? PmuMetricBuilder::TaskDuration(result) : std::nullopt;
    const char* kernelType = isMixedKernel ? "mix" : (hasAic ? "aic" : "aiv");
    ASCTOOL_DEBUG(
        "[npu-compute] CSV operation duration: kernelType=%s valueUs=%f source=%s", kernelType,
        operationDurationUs.value_or(0.0), operationDurationUs.has_value() ? "task-log-median" : "pmu-cycles");
    return operationDurationUs;
}

aclptiResult WriteCsvSections(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, CsvWriteContext& context)
{
    const auto operationDurationUs = CsvOperationDuration(result);
    try {
        for (const auto& section : sections) {
            if (PmuMetricBuilder::Header(section).empty()) {
                ASCTOOL_ERROR(
                    "[npu-compute] CSV write rejected: unsupported section=%s"
                    " code=%u",
                    section.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
                return ACLPTI_ERROR_NOT_SUPPORTED;
            }
        }
        for (const auto& section : sections) {
            const auto status = WriteCsvSection(result, section, context, operationDurationUs);
            if (status != ACLPTI_SUCCESS) {
                return status;
            }
        }
    } catch (const std::bad_alloc&) {
        if (context.artifactStarted) {
            context.sink.Abort();
        }
        ASCTOOL_ERROR(
            "[npu-compute] CSV write failed: out of memory"
            " code=%u",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_INTERNAL;
    } catch (const std::exception& error) {
        if (context.artifactStarted) {
            context.sink.Abort();
        }
        ASCTOOL_ERROR(
            "[npu-compute] artifact write failed: %s"
            " code=%u",
            error.what(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_CSV_WRITE;
    }
    ASCTOOL_DEBUG("[npu-compute] CSV write complete");
    return ACLPTI_SUCCESS;
}

} // namespace

aclptiResult WritePmuCsv(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    ArtifactSink& sink, std::string_view artifactPrefix, bool* degraded)
{
    const auto& pmuLogs = result.pmuLogs;
    const size_t rowCount = pmuLogs.size();
    ASCTOOL_DEBUG(
        "[npu-compute] CSV write requested: sections=%zu pmuRows=%zu status=%d "
        "failedRecords=%llu",
        sections.size(), rowCount, static_cast<int>(result.status),
        static_cast<unsigned long long>(result.errorStats.failedRecordCount));
    if (IsEmptySuccessfulResult(result)) {
        ASCTOOL_DEBUG("[npu-compute] CSV write skipped: empty successful result");
        return ACLPTI_SUCCESS;
    }
    if (rowCount == 0) {
        ASCTOOL_DEBUG(
            "[npu-compute] CSV write skipped: no PMU rows status=%d failedRecords=%llu",
            static_cast<int>(result.status), static_cast<unsigned long long>(result.errorStats.failedRecordCount));
        return ACLPTI_SUCCESS;
    }
    const double aicFrequencyMhz = AicFrequencyMhz(config);
    const double aivFrequencyMhz = AivFrequencyMhz(config);
    if (aicFrequencyMhz <= 0.0 || !std::isfinite(aicFrequencyMhz) || aivFrequencyMhz <= 0.0 ||
        !std::isfinite(aivFrequencyMhz)) {
        ASCTOOL_ERROR(
            "[npu-compute] CSV write rejected: invalid config frequency=%f aicFrequency=%f aivFrequency=%f"
            " code=%u",
            config.frequencyMhz, config.aicFrequencyMhz, config.aivFrequencyMhz, NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    CsvWriteContext context{config, sink, artifactPrefix, degraded};
    return WriteCsvSections(result, sections, context);
}

} // namespace npucompute
