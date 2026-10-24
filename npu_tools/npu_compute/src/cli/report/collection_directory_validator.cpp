/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/collection_directory_validator.h"
#include "report/collection_file_validator.h"
#include "report/pm_sampling_file_validator.h"

#include <array>
#include <fstream>

namespace npucompute::cli {
namespace {
bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool Readable(const boost::filesystem::path& path, std::string* error)
{
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return Fail("open collection file failed: " + path.string(), error);
    }
    std::array<char, 4096> buffer{};
    bool has_content = false;
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        has_content = true;
    }
    return (!input.bad() && has_content) ||
           Fail("read collection file failed or file became empty: " + path.string(), error);
}

bool ValidateDirectory(
    const boost::filesystem::path& directory, std::size_t* valid_count, std::vector<boost::filesystem::path>* discarded,
    std::vector<std::string>* issues, std::string* error)
{
    boost::system::error_code status_error;
    const auto status = boost::filesystem::symlink_status(directory, status_error);
    if (status_error || !boost::filesystem::is_directory(status)) {
        return Fail("collection path is not a directory: " + directory.string(), error);
    }
    boost::system::error_code iterate_error;
    boost::filesystem::directory_iterator it(directory, iterate_error), end;
    if (iterate_error) {
        return Fail("open collection directory failed: " + directory.string() + ": " + iterate_error.message(), error);
    }
    for (; it != end; it.increment(iterate_error)) {
        if (iterate_error) {
            break;
        }
        const auto path = it->path();
        const auto item_status = boost::filesystem::symlink_status(path, status_error);
        if (status_error || boost::filesystem::is_symlink(item_status)) {
            return Fail("invalid collection directory item: " + path.string(), error);
        }
        const std::string name = path.filename().string();
        if (name == ".hardware_info.lock") {
            if (!boost::filesystem::is_regular_file(item_status)) {
                return Fail("HardwareInfo lock path is not a regular file: " + path.string(), error);
            }
            discarded->push_back(path);
            continue;
        }
        if (name == ".biu-staging" || name.find(".tmp.") != std::string::npos ||
            (name.size() >= 4U && name.compare(name.size() - 4U, 4U, ".tmp") == 0)) {
            discarded->push_back(path);
            if (issues != nullptr) {
                issues->push_back("unfinished internal collection item removed: " + path.string());
            }
            continue;
        }
        if (boost::filesystem::is_directory(item_status)) {
            if (!ValidateDirectory(path, valid_count, discarded, issues, error)) {
                return false;
            }
            continue;
        }
        if (!boost::filesystem::is_regular_file(item_status)) {
            return Fail("unsupported collection directory item: " + path.string(), error);
        }
        NpuRepFileType type;
        std::string item_error;
        if (!ResolveCollectionFileType(path, &type, &item_error) || !ValidateCollectionFile(path, type, &item_error) ||
            !ValidatePmSamplingFile(path, &item_error) || !Readable(path, &item_error)) {
            discarded->push_back(path);
            if (issues != nullptr) {
                issues->push_back(item_error);
            }
            continue;
        }
        ++*valid_count;
    }
    return !iterate_error || Fail("iterate collection directory failed: " + iterate_error.message(), error);
}
} // namespace

bool ValidateCollectionDirectory(
    const boost::filesystem::path& directory, std::string* error, std::vector<std::string>* issues)
{
    if (error != nullptr) {
        error->clear();
    }
    std::vector<boost::filesystem::path> discarded;
    std::size_t valid_count = 0;
    if (!ValidateDirectory(directory, &valid_count, &discarded, issues, error)) {
        return false;
    }
    for (const auto& path : discarded) {
        boost::system::error_code remove_error;
        boost::filesystem::remove_all(path, remove_error);
        if (remove_error) {
            return Fail(
                "remove invalid/internal collection item failed: " + path.string() + ": " + remove_error.message(),
                error);
        }
    }
    return valid_count != 0 || Fail("no valid collection result files", error);
}
} // namespace npucompute::cli
