/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "launch/disk_output_directory.h"

#include <cerrno>
#include <system_error>
#include <utility>
#include <sys/stat.h>

namespace npucompute::cli {
namespace {

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool CheckParent(const boost::filesystem::path& parent, std::string* error)
{
    boost::system::error_code status_error;
    const auto status = boost::filesystem::symlink_status(parent, status_error);
    if (status_error || !parent.is_absolute() || !boost::filesystem::is_directory(status)) {
        return Fail(
            "--export must specify an existing directory when --no-pack is used: " + parent.string() +
                (status_error ? ": " + status_error.message() : ""),
            error);
    }
    return true;
}

bool CreateDirectory(
    const boost::filesystem::path& parent, const ReportNameSources* sources, std::string* created, std::string* error)
{
    if (!CheckParent(parent, error)) {
        return false;
    }
    for (std::size_t attempt = 0; attempt < 128U; ++attempt) {
        std::string name;
        const bool named = sources != nullptr ? GenerateReportBaseNameWithSources(*sources, &name, error) :
                                                GenerateReportBaseName(&name, error);
        if (!named) {
            return false;
        }
        // Allocate the path before mkdir, so a successful mkdir cannot be orphaned by allocation failure.
        std::string candidate = (parent / name).string();
        if (::mkdir(candidate.c_str(), S_IRWXU) == 0) {
            created->swap(candidate);
            return true;
        }
        const int code = errno;
        if (code != EEXIST) {
            return Fail(
                "create disk output directory failed: " + candidate + ": " +
                    std::error_code(code, std::generic_category()).message(),
                error);
        }
    }
    return Fail("unable to create a unique disk output directory after 128 attempts", error);
}

} // namespace

bool ResolveDiskParentDirectory(
    const std::optional<std::string>& export_path, boost::filesystem::path* parent, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (parent == nullptr) {
        return Fail("disk parent output is null", error);
    }
    parent->clear();
    boost::system::error_code current_error;
    const auto current = boost::filesystem::current_path(current_error);
    if (current_error) {
        return Fail("get current directory failed: " + current_error.message(), error);
    }
    if (export_path && export_path->empty()) {
        return Fail("disk export path is empty", error);
    }
    const boost::filesystem::path requested = export_path ? boost::filesystem::path(*export_path) : current;
    // A trailing separator must not turn a symlink into a followed parent directory.
    auto resolved = (requested.is_absolute() ? requested : current / requested).lexically_normal();
    while (resolved.filename().empty() && resolved != resolved.root_path()) {
        resolved = resolved.parent_path();
    }
    if (!CheckParent(resolved, error)) {
        return false;
    }
    *parent = std::move(resolved);
    return true;
}

DiskOutputDirectory::~DiskOutputDirectory()
{
    if (!retained_) {
        Remove(nullptr);
    }
}

bool DiskOutputDirectory::Create(const boost::filesystem::path& parent, DiskOutputDirectory* result, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (result == nullptr || !result->path_.empty()) {
        return Fail("disk output directory result is null or already owns a directory", error);
    }
    return CreateDirectory(parent, nullptr, &result->path_, error);
}

bool DiskOutputDirectory::CreateWithSources(
    const boost::filesystem::path& parent, const ReportNameSources& sources, DiskOutputDirectory* result,
    std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (result == nullptr || !result->path_.empty()) {
        return Fail("disk output directory result is null or already owns a directory", error);
    }
    return CreateDirectory(parent, &sources, &result->path_, error);
}

bool DiskOutputDirectory::Remove(std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (retained_ || path_.empty()) {
        return true;
    }
    boost::system::error_code remove_error;
    boost::filesystem::remove_all(path_, remove_error);
    if (remove_error) {
        return Fail("remove disk output directory failed: " + path_ + ": " + remove_error.message(), error);
    }
    path_.clear();
    return true;
}

} // namespace npucompute::cli
