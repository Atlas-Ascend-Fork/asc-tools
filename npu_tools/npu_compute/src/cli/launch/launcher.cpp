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
#include "launch/collection_session.h"

#include "launch/injection_path.h"
#include "launch/process_launcher.h"
#include "report/rep_directory_packer.h"
#include "report/pipe_trace_finalizer.h"
#include "report/summary_sqlite_finalizer.h"
#include "report/rep_report_writer.h"
#include "report/report_name.h"
#include "launch/staging_directory.h"
#include "launch/disk_output_directory.h"
#include "report/collection_directory_validator.h"

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

void SetEnvironmentValue(const std::string& name, const std::string& value, std::vector<std::string>* environment)
{
    const std::string prefix = name + "=";
    for (auto iterator = environment->begin(); iterator != environment->end();) {
        if (iterator->compare(0, prefix.size(), prefix) == 0) {
            iterator = environment->erase(iterator);
        } else {
            ++iterator;
        }
    }
    environment->push_back(prefix + value);
}

bool BuildChildEnvironment(
    const std::string& collection_data_directory, std::vector<std::string>* environment, std::string* error)
{
    std::string injection_path;
    if (!ResolveInjectionLibraryPath(&injection_path, error)) {
        return false;
    }

    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        environment->emplace_back(*entry);
    }
    for (const char* name : {"NPU_COMPUTE_REPLAY_MODE", "NPU_COMPUTE_PMU_LEVEL"}) {
        const std::string prefix = std::string(name) + "=";
        environment->erase(
            std::remove_if(
                environment->begin(), environment->end(),
                [&](const auto& entry) { return entry.compare(0, prefix.size(), prefix) == 0; }),
            environment->end());
    }
    SetEnvironmentValue("ACL_API_INJECTION", injection_path, environment);
    SetEnvironmentValue(
        kNestedMarkerEnvironment, collection_data_directory + "/" + kNestedCollectionMarker, environment);
    SetEnvironmentValue(kCollectionActiveEnvironment, "1", environment);
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
        if (error != nullptr) {
            *error = kNestedCollectionError;
        }
        return kCollectionErrorExitCode;
    }

    ipc::Config collectionConfig;
    try {
        collectionConfig = LoadCollectionConfig(config);
    } catch (const std::exception& exception) {
        Fail(exception.what(), error);
        return kUsageErrorExitCode;
    }

    std::string stage_error;
    ReportTarget target;
    boost::filesystem::path disk_parent;
    if (config.no_pack ? !ResolveDiskParentDirectory(config.export_path, &disk_parent, &stage_error) :
                         !ResolveReportTarget(config.export_path, &target, &stage_error)) {
        Fail(stage_error, error);
        return kReportErrorExitCode;
    }

    boost::system::error_code current_directory_error;
    const boost::filesystem::path current_directory = boost::filesystem::current_path(current_directory_error);
    if (current_directory_error) {
        SetStageError("get current directory failed", current_directory_error.message(), error);
        return kInternalErrorExitCode;
    }

    StagingDirectory collection_data;
    DiskOutputDirectory disk_data;
    if (config.no_pack) {
        if (!DiskOutputDirectory::Create(disk_parent, &disk_data, error)) {
            return kReportErrorExitCode;
        }
    } else if (!StagingDirectory::Create(current_directory, &collection_data, error)) {
        return kInternalErrorExitCode;
    }
    const std::string& collection_path = config.no_pack ? disk_data.Path() : collection_data.Path();
    const auto finishCollection = [&](int result) {
        std::string cleanup_error;
        const bool removed = config.no_pack ? disk_data.Remove(&cleanup_error) : collection_data.Remove(&cleanup_error);
        if (!removed) {
            if (config.no_pack && error != nullptr && !error->empty()) {
                *error += "; cleanup failed: " + cleanup_error;
            } else {
                SetStageError("finalize collection data directory failed", cleanup_error, error);
            }
            if (!config.no_pack || result == 0) {
                result = kInternalErrorExitCode;
            }
        }
        return result;
    };

    ProcessLaunchRequest request;
    request.program = config.program;
    request.arguments = config.program_arguments;
    if (!BuildChildEnvironment(collection_path, &request.environment, error)) {
        return finishCollection(kInternalErrorExitCode);
    }
    CollectionSession session;
    const int appResult = session.Run(std::move(request), collectionConfig, collection_path, error);
    bool nested_collection_detected = false;
    if (!ConsumeNestedCollectionMarker(collection_path, &nested_collection_detected, error)) {
        return finishCollection(kInternalErrorExitCode);
    }
    if (nested_collection_detected) {
        if (error != nullptr) {
            *error = kNestedCollectionError;
        }
        return finishCollection(kCollectionErrorExitCode);
    }
    if (appResult != 0) {
        return finishCollection(appResult);
    }

    auto issues = session.Outcome().issues;
    boost::system::error_code hardware_error;
    const auto hardware_status = boost::filesystem::symlink_status(
        boost::filesystem::path(collection_path) / "HardwareInfo.jsonl", hardware_error);
    const bool hardware_info_valid = !hardware_error && boost::filesystem::is_regular_file(hardware_status);
    if (!hardware_info_valid) {
        issues.push_back("HardwareInfo.jsonl: hardware information missing or not a regular file");
    }
    if (!FinalizePipeTrace(collection_path, config.collect_pipeline, &stage_error, &issues)) {
        issues.push_back("finalize PipeTrace failed: " + stage_error);
    }
    const auto databaseResults = FinalizeSummarySqlite(collection_path);
    issues.insert(issues.end(), databaseResults.issues.begin(), databaseResults.issues.end());

    if (config.no_pack) {
        if (!ValidateCollectionDirectory(collection_path, &stage_error, &issues)) {
            SetStageError("validate collection results failed", stage_error, error);
            return finishCollection(kCollectionErrorExitCode);
        }
        for (const auto& issue : issues) {
            std::fprintf(stderr, "[WARNING] npu-compute: %s\n", issue.c_str());
        }
        if (report_path != nullptr) {
            *report_path = collection_path;
        }
        disk_data.Retain();
        if (!issues.empty()) {
            Fail("collection incomplete; valid raw results were retained", error);
        }
        return issues.empty() ? 0 : kCollectionErrorExitCode;
    }

    std::vector<uint8_t> encoded;
    if (!PackDirectoryToRep(collection_path, &encoded, &stage_error, &issues)) {
        SetStageError("pack collection results failed", stage_error, error);
        return finishCollection(kReportErrorExitCode);
    }
    if (encoded.empty()) {
        Fail(stage_error, error);
        return finishCollection(kCollectionErrorExitCode);
    }

    if (!PublishRepReport(encoded, target, &stage_error)) {
        Fail(stage_error, error);
        return finishCollection(kReportErrorExitCode);
    }
    if (report_path != nullptr) {
        *report_path = target.path.string();
    }
    for (const auto& issue : issues) {
        std::fprintf(stderr, "[WARNING] npu-compute: %s\n", issue.c_str());
    }
    if (!issues.empty()) {
        Fail("collection incomplete; valid results were packaged", error);
    }
    return finishCollection(issues.empty() ? 0 : kCollectionErrorExitCode);
}

} // namespace npucompute::cli
