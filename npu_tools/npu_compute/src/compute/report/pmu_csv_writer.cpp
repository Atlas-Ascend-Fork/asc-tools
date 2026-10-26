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
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

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

std::string ResolveOutputDirectory(const ReportConfig& config) { return config.outputDirectory; }

const char* PmuDataLevelName(PmuDataLevel level) { return level == PmuDataLevel::Task ? "task" : "block"; }

bool IsEmptySuccessfulResult(const aclptiProfilingDataResult& result)
{
    return result.status == ACLPTI_SUCCESS && result.taskLogs.empty() && result.blockLogs.empty() &&
           result.pmuLogs.empty() && !result.taskPmuData.aic && !result.taskPmuData.aiv &&
           result.errorStats.failedRecordCount == 0;
}

bool HasRootSectionCsv(const boost::filesystem::path& outputDirectory)
{
    for (const auto& section :
         {"L2Cache", "Memory", "MemoryL0", "MemoryUB", "PipeUtilization", "ArithmeticUtilization",
          "ResourceConflictRatio"}) {
        if (boost::filesystem::exists(outputDirectory / (std::string(section) + ".csv"))) {
            return true;
        }
    }
    return false;
}

std::string CollectionDirectoryName(uint64_t sequence)
{
    std::ostringstream output;
    output << "collection-p" << static_cast<long long>(::getpid()) << '-' << std::setw(4) << std::setfill('0')
           << sequence;
    return output.str();
}

boost::filesystem::path CreateUniqueCollectionDirectory(const boost::filesystem::path& outputDirectory)
{
    static std::atomic<uint64_t> sequence{0};
    for (std::size_t attempt = 0; attempt < 1024; ++attempt) {
        const boost::filesystem::path collectionDirectory = outputDirectory / CollectionDirectoryName(++sequence);
        if (boost::filesystem::create_directory(collectionDirectory)) {
            return collectionDirectory;
        }
    }
    throw boost::filesystem::filesystem_error(
        "create unique CSV collection directory failed", outputDirectory,
        boost::system::errc::make_error_code(boost::system::errc::file_exists));
}

boost::filesystem::path ResolveCsvWriteDirectory(const boost::filesystem::path& outputDirectory)
{
    if (!HasRootSectionCsv(outputDirectory)) {
        return outputDirectory;
    }
    return CreateUniqueCollectionDirectory(outputDirectory);
}

aclptiResult EnsureCsvOutputDirectory(const boost::filesystem::path& outputDirectory)
{
    if (outputDirectory.empty()) {
        ASCTOOL_ERROR(
            "[npu-compute] CSV write rejected: output path is empty"
            " code=%u",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    if (!outputDirectory.is_absolute()) {
        ASCTOOL_ERROR(
            "[npu-compute] CSV write rejected: output path must be absolute path=%s"
            " code=%u",
            outputDirectory.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }

    boost::system::error_code filesystemError;
    const boost::filesystem::file_status outputStatus =
        boost::filesystem::symlink_status(outputDirectory, filesystemError);
    if (filesystemError == boost::system::errc::no_such_file_or_directory) {
        filesystemError.clear();
    }
    if (filesystemError) {
        ASCTOOL_ERROR(
            "[npu-compute] CSV write rejected: inspect output path failed path=%s code=%d reason=%s"
            " code=%u",
            outputDirectory.c_str(), filesystemError.value(), filesystemError.message().c_str(),
            static_cast<unsigned>(static_cast<uint32_t>(filesystemError.value())));
        return ACLPTI_ERROR_CSV_WRITE;
    }
    const bool exists = boost::filesystem::exists(outputStatus);
    if (exists && !boost::filesystem::is_directory(outputDirectory, filesystemError)) {
        if (filesystemError) {
            ASCTOOL_ERROR(
                "[npu-compute] CSV write rejected: inspect output path failed path=%s code=%d reason=%s"
                " code=%u",
                outputDirectory.c_str(), filesystemError.value(), filesystemError.message().c_str(),
                static_cast<unsigned>(static_cast<uint32_t>(filesystemError.value())));
        } else {
            ASCTOOL_ERROR(
                "[npu-compute] CSV write rejected: output path is not a directory path=%s reason=not a directory"
                " code=%u",
                outputDirectory.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        }
        return ACLPTI_ERROR_CSV_WRITE;
    }
    if (!exists) {
        boost::filesystem::create_directories(outputDirectory, filesystemError);
        if (filesystemError) {
            ASCTOOL_ERROR(
                "[npu-compute] CSV write rejected: create output directory failed path=%s code=%d reason=%s"
                " code=%u",
                outputDirectory.c_str(), filesystemError.value(), filesystemError.message().c_str(),
                static_cast<unsigned>(static_cast<uint32_t>(filesystemError.value())));
            return ACLPTI_ERROR_CSV_WRITE;
        }
    }
    if (::access(outputDirectory.c_str(), W_OK | X_OK) != 0) {
        const int errorNumber = errno;
        ASCTOOL_ERROR(
            "[npu-compute] CSV write rejected: output path is not writable path=%s errno=%d reason=%s"
            " code=%u",
            outputDirectory.c_str(), errorNumber, std::strerror(errorNumber),
            static_cast<unsigned>(static_cast<uint32_t>(errorNumber)));
        return ACLPTI_ERROR_CSV_WRITE;
    }
    return ACLPTI_SUCCESS;
}

} // namespace

aclptiResult WritePmuCsv(
    const aclptiProfilingDataResult& result, const std::vector<std::string>& sections, const ReportConfig& config,
    ArtifactSink* sink, std::string_view artifactPrefix, bool* degraded)
{
    const std::string outputDirectory = ResolveOutputDirectory(config);
    const bool taskLevel = config.pmuDataLevel == PmuDataLevel::Task;
    const auto& pmuLogs = result.pmuLogs;
    const size_t rowCount =
        taskLevel ? static_cast<size_t>(result.taskPmuData.aic || result.taskPmuData.aiv) : pmuLogs.size();
    const char* pmuLevel = PmuDataLevelName(config.pmuDataLevel);
    ASCTOOL_DEBUG(
        "[npu-compute] CSV write requested: output=%s sections=%zu pmuLevel=%s pmuRows=%zu status=%d "
        "failedRecords=%llu",
        outputDirectory.c_str(), sections.size(), pmuLevel, rowCount, static_cast<int>(result.status),
        static_cast<unsigned long long>(result.errorStats.failedRecordCount));
    if (IsEmptySuccessfulResult(result)) {
        ASCTOOL_DEBUG("[npu-compute] CSV write skipped: empty successful result");
        return ACLPTI_SUCCESS;
    }
    if (rowCount == 0) {
        ASCTOOL_DEBUG(
            "[npu-compute] CSV write skipped: no PMU rows pmuLevel=%s status=%d failedRecords=%llu", pmuLevel,
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
    bool hasAic = result.taskPmuData.aic.has_value();
    bool hasAiv = result.taskPmuData.aiv.has_value();
    if (!taskLevel) {
        hasAic = false;
        hasAiv = false;
        for (const auto& [key, row] : pmuLogs) {
            hasAic = hasAic || key.coreType == ACLPTI_CORE_TYPE_AIC;
            hasAiv = hasAiv || key.coreType == ACLPTI_CORE_TYPE_AIV;
        }
    }
    const bool isMixedKernel = hasAic && hasAiv;
    const Metric operationDurationUs = isMixedKernel ? PmuMetricBuilder::TaskDuration(result) : std::nullopt;
    const char* kernelType = isMixedKernel ? "mix" : (hasAic ? "aic" : "aiv");
    ASCTOOL_DEBUG(
        "[npu-compute] CSV operation duration: kernelType=%s valueUs=%f source=%s", kernelType,
        operationDurationUs.value_or(0.0), operationDurationUs.has_value() ? "task-log-median" : "pmu-cycles");
    bool artifactStarted = false;
    try {
        const boost::filesystem::path rootDirectory(outputDirectory);
        const aclptiResult outputDirectoryStatus =
            sink == nullptr ? EnsureCsvOutputDirectory(rootDirectory) : ACLPTI_SUCCESS;
        if (outputDirectoryStatus != ACLPTI_SUCCESS) {
            return outputDirectoryStatus;
        }
        for (const auto& section : sections) {
            if (PmuMetricBuilder::Header(section).empty()) {
                ASCTOOL_ERROR(
                    "[npu-compute] CSV write rejected: unsupported section=%s"
                    " code=%u",
                    section.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
                return ACLPTI_ERROR_NOT_SUPPORTED;
            }
        }
        const boost::filesystem::path writeDirectory =
            sink != nullptr || config.fixedOutputDirectory ? rootDirectory : ResolveCsvWriteDirectory(rootDirectory);
        if (writeDirectory != rootDirectory) {
            ASCTOOL_DEBUG("[npu-compute] CSV write routed to collection directory: path=%s", writeDirectory.c_str());
        }
        for (const auto& section : sections) {
            const boost::filesystem::path path = writeDirectory / (section + ".csv");
            ASCTOOL_DEBUG(
                "[npu-compute] CSV section write start: section=%s path=%s pmuLevel=%s rows=%zu", section.c_str(),
                path.c_str(), pmuLevel, rowCount);
            std::ofstream file;
            std::unique_ptr<ArtifactStreamBuffer> sinkBuffer;
            if (sink != nullptr) {
                sink->Begin(std::string(artifactPrefix) + section + ".csv");
                artifactStarted = true;
                sinkBuffer = std::make_unique<ArtifactStreamBuffer>(*sink);
            } else {
                file.open(path.string(), std::ios::out | std::ios::trunc);
            }
            std::ostream output(sinkBuffer ? static_cast<std::streambuf*>(sinkBuffer.get()) : file.rdbuf());
            if (sink == nullptr && !file.is_open()) {
                const int errorNumber = errno;
                ASCTOOL_ERROR(
                    "[npu-compute] CSV section write failed: open path=%s errno=%d reason=%s"
                    " code=%u",
                    path.c_str(), errorNumber, std::strerror(errorNumber),
                    static_cast<unsigned>(static_cast<uint32_t>(errorNumber)));
                return ACLPTI_ERROR_CSV_WRITE;
            }
            const std::vector<std::string> header = PmuMetricBuilder::Header(section, config.pmuDataLevel);
            CsvSectionStats stats;
            WriteCsvLine(output, header);
            const auto writeRow = [&](const CsvRow& values) {
                UpdateMissingStats(header, values, &stats);
                WriteCsvLine(output, values);
            };
            if (taskLevel) {
                writeRow(PmuMetricBuilder::BuildTask(section, result.taskPmuData, config, operationDurationUs));
            } else {
                for (const auto& [key, row] : pmuLogs) {
                    writeRow(PmuMetricBuilder::BuildBlock(section, key, row, config, operationDurationUs));
                }
            }
            output.flush();
            if (!output.good()) {
                if (artifactStarted) {
                    sink->Abort();
                    artifactStarted = false;
                }
                const int errorNumber = errno;
                ASCTOOL_ERROR(
                    "[npu-compute] CSV section write failed: flush path=%s errno=%d reason=%s"
                    " code=%u",
                    path.c_str(), errorNumber, std::strerror(errorNumber),
                    static_cast<unsigned>(static_cast<uint32_t>(errorNumber)));
                return ACLPTI_ERROR_CSV_WRITE;
            }
            if (sink != nullptr) {
                sink->Commit(stats.rows);
                artifactStarted = false;
            }
            if (degraded != nullptr && (stats.missingFields != 0 || stats.mismatchedRows != 0)) {
                *degraded = true;
            }
            const std::string missingColumns = FormatMissingColumns(stats.missingColumns);
            const std::string missingReasons = FormatMissingReasons(stats.missingReasons);
            const std::string missingColumnReasons = FormatMissingReasons(stats.missingColumnReasons);
            ASCTOOL_DEBUG(
                "[npu-compute] CSV section data availability: section=%s rows=%zu fields=%zu missingFields=%zu "
                "missingRows=%zu "
                "mismatchedRows=%zu missingColumns=%s missingReasons=%s missingColumnReasons=%s",
                section.c_str(), stats.rows, stats.totalFields, stats.missingFields, stats.missingRows,
                stats.mismatchedRows, missingColumns.c_str(), missingReasons.c_str(), missingColumnReasons.c_str());
            ASCTOOL_DEBUG(
                "[npu-compute] CSV section write complete: section=%s path=%s", section.c_str(), path.c_str());
        }
    } catch (const boost::filesystem::filesystem_error& error) {
        if (artifactStarted) {
            sink->Abort();
        }
        const boost::filesystem::path errorPath =
            error.path1().empty() ? boost::filesystem::path(outputDirectory) : error.path1();
        ASCTOOL_ERROR(
            "[npu-compute] CSV write failed: filesystem error path=%s code=%d reason=%s detail=%s"
            " code=%u",
            errorPath.c_str(), error.code().value(), error.code().message().c_str(), error.what(),
            static_cast<unsigned>(static_cast<uint32_t>(error.code().value())));
        return ACLPTI_ERROR_CSV_WRITE;
    } catch (const std::bad_alloc&) {
        if (artifactStarted) {
            sink->Abort();
        }
        ASCTOOL_ERROR(
            "[npu-compute] CSV write failed: out of memory"
            " code=%u",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_INTERNAL;
    } catch (const std::exception& error) {
        if (artifactStarted) {
            sink->Abort();
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

} // namespace npucompute
