/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/rep_directory_packer.h"

#include "report/collection_file_validator.h"
#include "report/pm_sampling_file_validator.h"
#include "report/rep_encoder.h"

#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <new>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace npucompute::cli {
namespace {

constexpr char kHardwareInfoLockName[] = ".hardware_info.lock";

struct DirectoryItem {
    boost::filesystem::path path;
    std::string stored_name;
    bool is_directory = false;
};

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool IsTemporaryName(const std::string& name)
{
    if (name.find(".tmp.") != std::string::npos) {
        return true;
    }
    constexpr char kTemporarySuffix[] = ".tmp";
    return name.size() >= sizeof(kTemporarySuffix) - 1U &&
           name.compare(
               name.size() - (sizeof(kTemporarySuffix) - 1U), sizeof(kTemporarySuffix) - 1U, kTemporarySuffix) == 0;
}

bool StoredNameLess(const DirectoryItem& left, const DirectoryItem& right)
{
    return std::lexicographical_compare(
        left.stored_name.begin(), left.stored_name.end(), right.stored_name.begin(), right.stored_name.end(),
        [](unsigned char left_byte, unsigned char right_byte) { return left_byte < right_byte; });
}

bool ReadFile(const boost::filesystem::path& path, std::vector<uint8_t>* payload, std::string* error)
{
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return Fail("open collection file for packing failed: " + path.string(), error);
    }
    payload->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        payload->clear();
        return Fail("read collection file for packing failed: " + path.string(), error);
    }
    if (payload->empty()) {
        return Fail("collection file became empty while packing: " + path.string(), error);
    }
    return true;
}

bool CollectDirectoryItems(
    const boost::filesystem::path& directory, std::vector<DirectoryItem>* items, std::string* error,
    std::vector<std::string>& issues)
{
    boost::system::error_code iterator_error;
    boost::filesystem::directory_iterator iterator(directory, iterator_error);
    const boost::filesystem::directory_iterator end;
    if (iterator_error) {
        return Fail("open collection directory failed: " + directory.string() + ": " + iterator_error.message(), error);
    }

    for (; iterator != end; iterator.increment(iterator_error)) {
        if (iterator_error) {
            break;
        }
        const boost::filesystem::path path = iterator->path();
        const std::string name = path.filename().string();
        boost::system::error_code status_error;
        const boost::filesystem::file_status status = boost::filesystem::symlink_status(path, status_error);
        if (status_error) {
            issues.push_back("inspect collection item failed: " + path.string() + ": " + status_error.message());
            continue;
        }
        if (boost::filesystem::is_symlink(status)) {
            issues.push_back("symbolic link is not allowed in collection directory: " + path.string());
            continue;
        }
        if (name == kHardwareInfoLockName) {
            if (!boost::filesystem::is_regular_file(status)) {
                issues.push_back("HardwareInfo lock path is not a regular file: " + path.string());
            }
            continue;
        }
        if (name == ".biu-staging") {
            continue;
        }
        if (name == "HardwareInfo.jsonl" && !boost::filesystem::is_regular_file(status)) {
            issues.push_back("HardwareInfo path is not a regular file: " + path.string());
            continue;
        }
        if (IsTemporaryName(name)) {
            issues.push_back("temporary collection item remains: " + path.string());
            continue;
        }

        DirectoryItem item;
        item.path = path;
        if (boost::filesystem::is_directory(status)) {
            item.is_directory = true;
            item.stored_name = name + ".npu.rep";
        } else if (boost::filesystem::is_regular_file(status)) {
            item.stored_name = name;
        } else {
            issues.push_back("unsupported collection directory item: " + path.string());
            continue;
        }
        if (item.stored_name.size() >= kNpuRepFileNameSize || item.stored_name.find('\\') != std::string::npos ||
            name == "collection-status.json") {
            issues.push_back("invalid or reserved collection name: " + path.string());
            continue;
        }
        items->push_back(std::move(item));
    }
    if (iterator_error) {
        issues.push_back(
            "iterate collection directory failed: " + directory.string() + ": " + iterator_error.message());
    }

    std::unordered_set<std::string> stored_names;
    stored_names.reserve(items->size());
    std::unordered_set<std::string> conflicts;
    for (const DirectoryItem& item : *items) {
        if (!stored_names.insert(item.stored_name).second) {
            conflicts.insert(item.stored_name);
            issues.push_back("collection stored name conflict: " + item.stored_name);
        }
    }
    items->erase(
        std::remove_if(
            items->begin(), items->end(),
            [&](const DirectoryItem& item) { return conflicts.count(item.stored_name) != 0; }),
        items->end());
    std::sort(items->begin(), items->end(), StoredNameLess);
    return true;
}

bool PackDirectoryInternal(
    const boost::filesystem::path& directory, std::vector<uint8_t>* encoded, std::string* error,
    std::vector<std::string>& issues, std::size_t& fileCount, bool root, bool includeStatus, std::size_t depth = 1U)
{
    if (depth > kNpuRepMaxDepth) {
        return Fail("maximum nested report depth exceeded: " + directory.string(), error);
    }
    std::vector<DirectoryItem> items;
    std::string itemError;
    if (!CollectDirectoryItems(directory, &items, &itemError, issues)) {
        if (root) {
            return Fail(itemError, error);
        }
        issues.push_back(itemError);
        return true;
    }

    std::vector<RepEntry> entries;
    entries.reserve(items.size());
    for (const DirectoryItem& item : items) {
        RepEntry entry;
        entry.file_name = item.stored_name;
        if (item.is_directory) {
            entry.file_type = NpuRepFileType::NpuRep;
            if (!PackDirectoryInternal(item.path, &entry.payload, error, issues, fileCount, false, false, depth + 1U)) {
                return false;
            }
            if (entry.payload.empty()) {
                continue;
            }
        } else {
            if (!ResolveCollectionFileType(item.path, &entry.file_type, &itemError) ||
                !ValidateCollectionFile(item.path, entry.file_type, &itemError) ||
                !ValidatePmSamplingFile(item.path, &itemError) || !ReadFile(item.path, &entry.payload, &itemError)) {
                issues.push_back(itemError);
                continue;
            }
            ++fileCount;
        }
        entries.push_back(std::move(entry));
    }
    if (root && fileCount == 0) {
        Fail("no valid collection files" + (issues.empty() ? std::string() : ": " + issues.front()), error);
        return true;
    }
    if (root && includeStatus && !issues.empty()) {
        boost::property_tree::ptree status;
        boost::property_tree::ptree messages;
        for (const auto& issue : issues) {
            boost::property_tree::ptree message;
            message.put_value(issue);
            messages.push_back({"", message});
        }
        status.add_child("issues", messages);
        std::ostringstream json;
        boost::property_tree::write_json(json, status, false);
        const std::string bytes = "{\"complete\":false," + json.str().substr(1);
        entries.push_back({"collection-status.json", NpuRepFileType::Json, {bytes.begin(), bytes.end()}});
        std::sort(entries.begin(), entries.end(), [](const RepEntry& left, const RepEntry& right) {
            return left.file_name < right.file_name;
        });
    }

    std::vector<uint8_t> complete_rep;
    if (!EncodeRep(entries, &complete_rep, error)) {
        return false;
    }
    *encoded = std::move(complete_rep);
    return true;
}

} // namespace

bool PackDirectoryToRep(
    const boost::filesystem::path& directory, std::vector<uint8_t>* encoded, std::string* error,
    std::vector<std::string>* issues)
{
    if (error != nullptr) {
        error->clear();
    }
    if (encoded == nullptr) {
        return Fail("encoded rep output is null", error);
    }
    encoded->clear();

    try {
        boost::system::error_code status_error;
        const boost::filesystem::file_status status = boost::filesystem::symlink_status(directory, status_error);
        if (status_error) {
            return Fail(
                "inspect collection directory failed: " + directory.string() + ": " + status_error.message(), error);
        }
        if (!boost::filesystem::is_directory(status)) {
            return Fail("collection path is not a directory: " + directory.string(), error);
        }
        std::vector<std::string> localIssues;
        std::size_t fileCount = 0;
        return PackDirectoryInternal(
            directory, encoded, error, issues ? *issues : localIssues, fileCount, true, issues != nullptr);
    } catch (const std::bad_alloc&) {
        encoded->clear();
        return Fail("allocate recursive rep buffer failed", error);
    }
}

} // namespace npucompute::cli
