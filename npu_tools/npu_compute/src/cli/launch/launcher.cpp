/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "launch/launcher.h"
#include "file_io.h"
#include "launch/collection_session.h"

#include "launch/injection_path.h"
#include "launch/process_launcher.h"
#include "report/collection_processor.h"
#include "communication/artifact_schema.h"
#include "report/communication_finalizer.h"
#include "report/pipe_trace_finalizer.h"
#include "report/summary_sqlite_finalizer.h"
#include "report/rep_report_writer.h"
#include "report/report_name.h"
#include "launch/staging_directory.h"
#include "launch/disk_output_directory.h"

#include <cerrno>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

namespace npucompute::cli {
namespace {

constexpr char kCollectionActiveEnvironment[] = "NPU_COMPUTE_COLLECTION_ACTIVE";
constexpr char kNestedMarkerEnvironment[] = "NPU_COMPUTE_NESTED_MARKER_PATH";
constexpr char kNestedCollectionMarker[] = ".npu-compute-nested-collection";
constexpr char kNestedCollectionError[] = "nested npu-compute collection is not supported. The target program or "
                                          "script must not start another npu-compute collection.";

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~FileDescriptor()
    {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int Get() const { return descriptor_; }

private:
    int descriptor_ = -1;
};

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool FailErrno(const std::string& message, int error_number, std::string* error)
{
    return Fail(message + ": " + std::string(std::strerror(error_number)), error);
}

bool BuildChildEnvironment(
    const std::string& collection_data_directory, ProcessLaunchRequest& request, std::string* error)
{
    std::string injection_path;
    if (!ResolveInjectionLibraryPath(&injection_path, error)) {
        return false;
    }

    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        request.environment.emplace_back(*entry);
    }
    const std::string replayModePrefix = "NPU_COMPUTE_REPLAY_MODE=";
    request.environment.erase(
        std::remove_if(
            request.environment.begin(), request.environment.end(),
            [&](const auto& entry) { return entry.compare(0, replayModePrefix.size(), replayModePrefix) == 0; }),
        request.environment.end());
    request.SetEnvironment("ACL_API_INJECTION", injection_path);
    request.SetEnvironment(kNestedMarkerEnvironment, collection_data_directory + "/" + kNestedCollectionMarker);
    request.SetEnvironment(kCollectionActiveEnvironment, "1");
    return true;
}

bool IsNestedCollection()
{
    const char* value = std::getenv(kCollectionActiveEnvironment);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool RecordNestedCollection(std::string* error)
{
    const char* markerPathValue = std::getenv(kNestedMarkerEnvironment);
    if (markerPathValue == nullptr || markerPathValue[0] != '/') {
        return Fail("nested collection detection failed: nested marker path is not set", error);
    }

    const boost::filesystem::path marker_path(markerPathValue);
    if (!asctools::security::ValidatePath(marker_path.string(), asctools::security::PathKind::FileOrNew)) {
        return Fail("nested collection detection failed", error);
    }
    FileDescriptor marker(
        ::open(marker_path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, S_IRUSR | S_IWUSR));
    if (marker.Get() < 0) {
        return FailErrno("nested collection detection failed", errno, error);
    }
    return true;
}

bool ConsumeNestedCollectionMarker(
    const boost::filesystem::path& collection_directory, bool* detected, std::string* error)
{
    *detected = false;
    const boost::filesystem::path marker_path = collection_directory / kNestedCollectionMarker;
    if (::unlink(marker_path.c_str()) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        return FailErrno("nested collection detection failed", errno, error);
    }
    *detected = true;
    return true;
}

void SetStageError(const std::string& stage, const std::string& detail, std::string* error)
{
    if (error != nullptr) {
        *error = stage + ": " + detail;
    }
}

class CollectionOutput {
public:
    explicit CollectionOutput(bool noPack) : noPack_(noPack) {}

    int Create(const CliConfig& config, std::string* error)
    {
        boost::filesystem::path disk_parent;
        if (noPack_ ? !ResolveDiskParentDirectory(config.export_path, &disk_parent, &stage_error_) :
                      !ResolveReportTarget(config.export_path, &target_, &stage_error_)) {
            Fail(stage_error_, error);
            return kReportErrorExitCode;
        }
        boost::system::error_code current_directory_error;
        const auto current_directory = boost::filesystem::current_path(current_directory_error);
        if (current_directory_error) {
            SetStageError("get current directory failed", current_directory_error.message(), error);
            return kInternalErrorExitCode;
        }
        if (noPack_) {
            return DiskOutputDirectory::Create(disk_parent, &disk_data_, error) ? 0 : kReportErrorExitCode;
        }
        return StagingDirectory::Create(current_directory, &collection_data_, error) ? 0 : kInternalErrorExitCode;
    }

    const std::string& Path() const { return noPack_ ? disk_data_.Path() : collection_data_.Path(); }

    int Finish(int result, std::string* error, int targetFailure = 0)
    {
        std::string cleanup_error;
        const bool removed = noPack_ ? disk_data_.Remove(&cleanup_error) : collection_data_.Remove(&cleanup_error);
        if (!removed) {
            if (noPack_ && error != nullptr && !error->empty()) {
                *error += "; cleanup failed: " + cleanup_error;
            } else {
                SetStageError("finalize collection data directory failed", cleanup_error, error);
            }
            if (!noPack_ || result == 0) {
                result = kInternalErrorExitCode;
            }
        }
        return targetFailure != 0 ? targetFailure : result;
    }

    int Publish(
        const CliConfig& config, const CollectionOutcome& outcome, int targetFailure, std::string* report_path,
        std::string* error)
    {
        auto issues = outcome.issues;
        if (config.replay_mode == ReplayMode::Kernel) {
            FinalizeKernelResults(config.collect_pipeline, issues);
        } else if (outcome.artifacts > 0) {
            FinalizeApplicationResults(issues);
        }
        return noPack_ ? RetainRawResults(issues, targetFailure, report_path, error) :
                         PublishReport(issues, targetFailure, report_path, error);
    }

private:
    void FinalizeApplicationResults(std::vector<std::string>& issues) const
    {
        using namespace boost::filesystem;
        const path root(Path());
        if (exists(root / communication::kStagingDirectory)) {
            FinalizeCommunication(root, &issues);
            return;
        }
        boost::system::error_code scanError;
        for (directory_iterator it(root, scanError), end; !scanError && it != end; it.increment(scanError)) {
            const auto directory = it->path();
            if (directory.filename().string().rfind("pid-", 0) == 0 && is_directory(symlink_status(directory)) &&
                exists(directory / communication::kStagingDirectory)) {
                FinalizeCommunication(directory, &issues);
            }
        }
        if (scanError) {
            issues.push_back("inspect application report directories failed: " + scanError.message());
        }
    }

    void FinalizeKernelResults(bool collectPipeline, std::vector<std::string>& issues)
    {
        boost::system::error_code hardware_error;
        const auto hardware_status =
            boost::filesystem::symlink_status(boost::filesystem::path(Path()) / "HardwareInfo.jsonl", hardware_error);
        if (hardware_error || !boost::filesystem::is_regular_file(hardware_status)) {
            issues.push_back("HardwareInfo.jsonl: hardware information missing or not a regular file");
        }
        if (!FinalizePipeTrace(Path(), collectPipeline, &stage_error_, &issues)) {
            issues.push_back("finalize PipeTrace failed: " + stage_error_);
        }
        const auto databaseResults = FinalizeSummarySqlite(Path());
        issues.insert(issues.end(), databaseResults.issues.begin(), databaseResults.issues.end());
    }

    int RetainRawResults(
        std::vector<std::string>& issues, int targetFailure, std::string* report_path, std::string* error)
    {
        // Validate every sibling before removing rejected paths, so valid results can still be retained.
        if (!ValidateAndCleanCollectionDirectory(Path(), &stage_error_, &issues)) {
            SetStageError("validate collection results failed", stage_error_, error);
            return Finish(kCollectionErrorExitCode, error, targetFailure);
        }
        for (const auto& issue : issues) {
            std::fprintf(stderr, "[WARNING] npu-compute: %s\n", issue.c_str());
        }
        if (report_path != nullptr) {
            *report_path = Path();
        }
        disk_data_.Retain();
        if (!issues.empty() && targetFailure == 0) {
            Fail("collection incomplete; valid raw results were retained", error);
        }
        return targetFailure != 0 ? targetFailure : (issues.empty() ? 0 : kCollectionErrorExitCode);
    }

    int PublishReport(std::vector<std::string>& issues, int targetFailure, std::string* report_path, std::string* error)
    {
        std::vector<uint8_t> encoded;
        // Packing adds nested rep encoding and collection status to the shared per-file validation result.
        if (!PackDirectoryToRep(Path(), &encoded, &stage_error_, &issues)) {
            SetStageError("pack collection results failed", stage_error_, error);
            return Finish(kReportErrorExitCode, error, targetFailure);
        }
        if (encoded.empty()) {
            Fail(stage_error_, error);
            return Finish(kCollectionErrorExitCode, error, targetFailure);
        }
        if (!PublishRepReport(encoded, target_, &stage_error_)) {
            Fail(stage_error_, error);
            return Finish(kReportErrorExitCode, error, targetFailure);
        }
        if (report_path != nullptr) {
            *report_path = target_.path.string();
        }
        for (const auto& issue : issues) {
            std::fprintf(stderr, "[WARNING] npu-compute: %s\n", issue.c_str());
        }
        if (!issues.empty() && targetFailure == 0) {
            Fail("collection incomplete; valid results were packaged", error);
        }
        return Finish(issues.empty() ? 0 : kCollectionErrorExitCode, error, targetFailure);
    }

    bool noPack_ = false;
    std::string stage_error_;
    ReportTarget target_;
    StagingDirectory collection_data_;
    DiskOutputDirectory disk_data_;
};

bool RunTargetCollection(
    const CliConfig& config, const ipc::Config& collectionConfig, const std::string& collection_path,
    CollectionSession& session, int& appResult, std::string* error)
{
    ProcessLaunchRequest request;
    request.program = config.program;
    request.arguments = config.program_arguments;
    if (!BuildChildEnvironment(collection_path, request, error)) {
        appResult = kInternalErrorExitCode;
        return false;
    }
    appResult = session.Run(std::move(request), collectionConfig, collection_path, error);
    bool nested_collection_detected = false;
    if (!ConsumeNestedCollectionMarker(collection_path, &nested_collection_detected, error)) {
        appResult = kInternalErrorExitCode;
        return false;
    }
    if (nested_collection_detected) {
        Fail(kNestedCollectionError, error);
        appResult = kCollectionErrorExitCode;
        return false;
    }
    return true;
}

} // namespace

int LaunchTarget(const CliConfig& config, std::string* report_path, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (report_path != nullptr) {
        report_path->clear();
    }
    if (IsNestedCollection()) {
        if (!RecordNestedCollection(error)) {
            return kInternalErrorExitCode;
        }
        Fail(kNestedCollectionError, error);
        return kCollectionErrorExitCode;
    }
    ipc::Config collectionConfig;
    try {
        collectionConfig = LoadCollectionConfig(config);
    } catch (const std::exception& exception) {
        Fail(exception.what(), error);
        return kUsageErrorExitCode;
    }
    CollectionOutput output(config.no_pack);
    const int createResult = output.Create(config, error);
    if (createResult != 0) {
        return createResult;
    }
    CollectionSession session;
    int appResult = 0;
    if (!RunTargetCollection(config, collectionConfig, output.Path(), session, appResult, error)) {
        return output.Finish(appResult, error);
    }
    const auto& outcome = session.Outcome();
    const bool retainApplicationArtifacts = config.replay_mode == ReplayMode::Application && outcome.artifacts > 0;
    if (appResult != 0 && !retainApplicationArtifacts) {
        return output.Finish(appResult, error);
    }
    const int targetFailure = outcome.outcome == "app_failed" ? appResult : 0;
    return output.Publish(config, outcome, targetFailure, report_path, error);
}

} // namespace npucompute::cli
