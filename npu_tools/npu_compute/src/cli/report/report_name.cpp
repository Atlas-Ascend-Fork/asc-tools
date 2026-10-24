/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/report_name.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace npucompute::cli {
namespace {

constexpr char REPORT_SUFFIX[] = ".npu-rep";
constexpr std::size_t MAXIMUM_NAME_ATTEMPTS = 128U;

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

class UniqueFd {
public:
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd()
    {
        if (fd_ >= 0) {
            (void)close(fd_);
        }
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int Get() const noexcept { return fd_; }

private:
    int fd_;
};

bool ReadRandomDevice(uint8_t* output, std::size_t size, std::string* error)
{
    int fd = -1;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return Fail("open /dev/urandom failed: " + std::error_code(errno, std::generic_category()).message(), error);
    }

    UniqueFd randomDevice(fd);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = read(randomDevice.Get(), output + offset, size - offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Fail(
                "read /dev/urandom failed: " + std::error_code(errno, std::generic_category()).message(), error);
        }
        if (result == 0) {
            return Fail("read /dev/urandom returned zero bytes", error);
        }
        offset += static_cast<std::size_t>(result);
    }
    return true;
}

bool SystemEpochMilliseconds(uint64_t* value, void*, std::string* error)
{
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    if (milliseconds.count() < 0) {
        return Fail("system time is before the Unix epoch", error);
    }
    *value = static_cast<uint64_t>(milliseconds.count());
    return true;
}

bool SystemRandomBytes(std::array<uint8_t, 4>* value, void*, std::string* error)
{
    std::size_t offset = 0;
#ifdef SYS_getrandom
    while (offset < value->size()) {
        const ssize_t result = syscall(SYS_getrandom, value->data() + offset, value->size() - offset, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOSYS) {
                break;
            }
            return Fail(
                "getrandom syscall failed: " + std::error_code(errno, std::generic_category()).message(), error);
        }
        if (result == 0) {
            return Fail("getrandom syscall returned zero bytes", error);
        }
        offset += static_cast<std::size_t>(result);
    }
    if (offset == value->size()) {
        return true;
    }
#endif
    return ReadRandomDevice(value->data() + offset, value->size() - offset, error);
}

bool HasReportSuffix(const boost::filesystem::path& path)
{
    const std::string value = path.filename().string();
    return value.size() >= sizeof(REPORT_SUFFIX) - 1U &&
           value.compare(value.size() - (sizeof(REPORT_SUFFIX) - 1U), sizeof(REPORT_SUFFIX) - 1U, REPORT_SUFFIX) == 0;
}

boost::filesystem::path ResolvePath(
    const boost::filesystem::path& path, const boost::filesystem::path& current_directory)
{
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (current_directory / path).lexically_normal();
}

bool ReadStatus(const boost::filesystem::path& path, boost::filesystem::file_status* status, std::string* error)
{
    boost::system::error_code status_error;
    *status = boost::filesystem::symlink_status(path, status_error);
    if (status_error == boost::system::errc::no_such_file_or_directory) {
        *status = boost::filesystem::file_status(boost::filesystem::file_not_found);
        return true;
    }
    if (status_error) {
        return Fail("inspect report path failed: " + path.string() + ": " + status_error.message(), error);
    }
    return true;
}

std::string FormatRandomId(const std::array<uint8_t, 4>& bytes)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result(8U, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2U] = HEX[bytes[index] >> 4U];
        result[index * 2U + 1U] = HEX[bytes[index] & 0x0fU];
    }
    return result;
}

bool GenerateTargetInDirectory(
    const boost::filesystem::path& directory, const ReportNameSources& sources, ReportTarget* target,
    std::string* error)
{
    boost::filesystem::file_status directory_status;
    if (!ReadStatus(directory, &directory_status, error)) {
        return false;
    }
    if (!boost::filesystem::is_directory(directory_status)) {
        return Fail("report output path is not a directory: " + directory.string(), error);
    }

    uint64_t epoch_milliseconds = 0;
    if (!sources.epoch_milliseconds(&epoch_milliseconds, sources.context, error)) {
        return false;
    }
    for (std::size_t attempt = 0; attempt < MAXIMUM_NAME_ATTEMPTS; ++attempt) {
        std::array<uint8_t, 4> random_bytes{};
        if (!sources.random_bytes(&random_bytes, sources.context, error)) {
            return false;
        }
        const boost::filesystem::path candidate = directory / ("report_" + std::to_string(epoch_milliseconds) + "_" +
                                                               FormatRandomId(random_bytes) + REPORT_SUFFIX);
        boost::filesystem::file_status candidate_status;
        if (!ReadStatus(candidate, &candidate_status, error)) {
            return false;
        }
        if (!boost::filesystem::exists(candidate_status)) {
            target->path = candidate;
            return true;
        }
    }
    return Fail("unable to generate a unique report name after 128 attempts", error);
}

bool ResolveExplicitTarget(const boost::filesystem::path& path, ReportTarget* target, std::string* error)
{
    const boost::filesystem::path parent = path.parent_path();
    boost::filesystem::file_status parent_status;
    if (!ReadStatus(parent, &parent_status, error)) {
        return false;
    }
    if (!boost::filesystem::exists(parent_status)) {
        return Fail(
            "output directory '" + parent.string() + "' does not exist. Create it before using --export.", error);
    }
    if (!boost::filesystem::is_directory(parent_status)) {
        return Fail("output parent path '" + parent.string() + "' is not a directory.", error);
    }

    boost::filesystem::file_status target_status;
    if (!ReadStatus(path, &target_status, error)) {
        return false;
    }
    if (boost::filesystem::exists(target_status)) {
        if (!boost::filesystem::is_regular_file(target_status)) {
            return Fail("report target exists but is not a regular file: " + path.string(), error);
        }
        return Fail("output report '" + path.string() + "' already exists. Choose a different file name.", error);
    }

    target->path = path;
    return true;
}

} // namespace

bool ResolveReportTarget(const std::optional<std::string>& export_path, ReportTarget* target, std::string* error)
{
    boost::system::error_code current_path_error;
    const boost::filesystem::path current_directory = boost::filesystem::current_path(current_path_error);
    if (current_path_error) {
        if (target != nullptr) {
            *target = {};
        }
        return Fail("get current directory failed: " + current_path_error.message(), error);
    }
    ReportNameSources sources;
    sources.current_directory = current_directory;
    sources.epoch_milliseconds = &SystemEpochMilliseconds;
    sources.random_bytes = &SystemRandomBytes;
    return ResolveReportTargetWithSources(export_path, sources, target, error);
}

bool ResolveReportTargetWithSources(
    const std::optional<std::string>& export_path, const ReportNameSources& sources, ReportTarget* target,
    std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (target == nullptr) {
        return Fail("report target output is null", error);
    }
    *target = {};
    if (sources.current_directory.empty() || !sources.current_directory.is_absolute()) {
        return Fail("report current directory must be an absolute path", error);
    }
    if (sources.epoch_milliseconds == nullptr || sources.random_bytes == nullptr) {
        return Fail("report name sources are incomplete", error);
    }

    if (!export_path.has_value()) {
        return GenerateTargetInDirectory(sources.current_directory, sources, target, error);
    }
    if (export_path->empty()) {
        return Fail("report export path is empty", error);
    }

    const boost::filesystem::path resolved = ResolvePath(*export_path, sources.current_directory);
    boost::filesystem::file_status resolved_status;
    if (!ReadStatus(resolved, &resolved_status, error)) {
        return false;
    }
    if (boost::filesystem::is_directory(resolved_status)) {
        return GenerateTargetInDirectory(resolved, sources, target, error);
    }
    if (!HasReportSuffix(resolved)) {
        return Fail(
            "--export expects a new .npu-rep file or an existing directory: '" + resolved.string() + "'.", error);
    }
    return ResolveExplicitTarget(resolved, target, error);
}

} // namespace npucompute::cli
