/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/collection_processor.h"

#include "report/collection_file_validator.h"
#include "report/pm_sampling_file_validator.h"
#include "report/rep_encoder.h"

#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace npucompute::cli {
namespace {

const std::string kHardwareInfoLockName = ".hardware_info.lock";
const std::string kStagingDirectoryName = ".biu-staging";
const std::string kHardwareInfoName = "HardwareInfo.jsonl";
const std::string kCollectionStatusName = "collection-status.json";

struct DirectoryItem {
    boost::filesystem::path path;
    std::string stored_name;
    bool is_directory = false;
};

enum class CollectionDirectoryMode {
    Pack,
    Retain,
};

struct ProcessingContext {
    CollectionDirectoryMode mode;
    std::vector<std::string>& issues;
    std::vector<boost::filesystem::path>& discarded;
    bool include_status = false;
    std::size_t file_count = 0;
};

bool ProcessCollectionFile(
    const boost::filesystem::path& path, NpuRepFileType* type, std::vector<uint8_t>* payload, std::string* error);

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
    const std::string temporary_suffix = ".tmp";
    return name.size() >= temporary_suffix.size() &&
           name.compare(name.size() - temporary_suffix.size(), temporary_suffix.size(), temporary_suffix) == 0;
}

bool StoredNameLess(const DirectoryItem& left, const DirectoryItem& right)
{
    return std::lexicographical_compare(
        left.stored_name.begin(), left.stored_name.end(), right.stored_name.begin(), right.stored_name.end(),
        [](unsigned char left_byte, unsigned char right_byte) { return left_byte < right_byte; });
}

void AddIssue(ProcessingContext& context, const std::string& issue) { context.issues.push_back(issue); }

bool RecordInvalidItem(
    const std::string& message, const boost::filesystem::path& path, ProcessingContext& context, std::string* error)
{
    if (context.mode == CollectionDirectoryMode::Retain) {
        return Fail(message + ": " + path.string(), error);
    }
    AddIssue(context, message + ": " + path.string());
    return true;
}

std::optional<bool> ProcessInternalItem(
    const boost::filesystem::path& path, const std::string& name, const boost::filesystem::file_status& status,
    ProcessingContext& context, std::string* error)
{
    if (name == kHardwareInfoLockName) {
        if (!boost::filesystem::is_regular_file(status)) {
            return RecordInvalidItem("HardwareInfo lock path is not a regular file", path, context, error);
        } else if (context.mode == CollectionDirectoryMode::Retain) {
            context.discarded.push_back(path);
        }
        return true;
    }
    if (name == kStagingDirectoryName) {
        if (context.mode == CollectionDirectoryMode::Retain) {
            context.discarded.push_back(path);
            AddIssue(context, "unfinished internal collection item removed: " + path.string());
        }
        return true;
    }
    if (name == kHardwareInfoName && !boost::filesystem::is_regular_file(status)) {
        return RecordInvalidItem("HardwareInfo path is not a regular file", path, context, error);
    }
    if (IsTemporaryName(name)) {
        if (context.mode == CollectionDirectoryMode::Retain) {
            context.discarded.push_back(path);
            AddIssue(context, "unfinished internal collection item removed: " + path.string());
        } else {
            AddIssue(context, "temporary collection item remains: " + path.string());
        }
        return true;
    }

    return std::nullopt;
}

bool CollectDirectoryItem(
    const boost::filesystem::path& path, std::vector<DirectoryItem>* items, ProcessingContext& context,
    std::string* error)
{
    const std::string name = path.filename().string();
    boost::system::error_code status_error;
    const boost::filesystem::file_status status = boost::filesystem::symlink_status(path, status_error);
    if (status_error) {
        return RecordInvalidItem("inspect collection item failed: " + status_error.message(), path, context, error);
    }
    if (boost::filesystem::is_symlink(status)) {
        return RecordInvalidItem("symbolic link is not allowed in collection directory", path, context, error);
    }
    const auto internal = ProcessInternalItem(path, name, status, context, error);
    if (internal) {
        return *internal;
    }

    DirectoryItem item;
    item.path = path;
    if (boost::filesystem::is_directory(status)) {
        item.is_directory = true;
        item.stored_name = name + ".npu.rep";
    } else if (boost::filesystem::is_regular_file(status)) {
        item.stored_name = name;
    } else {
        return RecordInvalidItem("unsupported collection directory item", path, context, error);
    }

    // Names are constrained by the rep format only while packing. Raw
    // output keeps the original names and therefore does not need this
    // encoding-specific conflict check.
    if (context.mode == CollectionDirectoryMode::Pack &&
        (item.stored_name.size() >= NPU_REP_FILE_NAME_SIZE || item.stored_name.find('\\') != std::string::npos ||
         name == kCollectionStatusName)) {
        AddIssue(context, "invalid or reserved collection name: " + path.string());
        return true;
    }
    items->push_back(std::move(item));
    return true;
}

void RemoveStoredNameConflicts(std::vector<DirectoryItem>* items, ProcessingContext& context)
{
    std::unordered_set<std::string> stored_names;
    stored_names.reserve(items->size());
    std::unordered_set<std::string> conflicts;
    for (const DirectoryItem& item : *items) {
        if (!stored_names.insert(item.stored_name).second) {
            conflicts.insert(item.stored_name);
            AddIssue(context, "collection stored name conflict: " + item.stored_name);
        }
    }
    items->erase(
        std::remove_if(
            items->begin(), items->end(),
            [&conflicts](const DirectoryItem& item) { return conflicts.count(item.stored_name) != 0; }),
        items->end());
    std::sort(items->begin(), items->end(), StoredNameLess);
}

bool CollectDirectoryItems(
    const boost::filesystem::path& directory, std::vector<DirectoryItem>* items, ProcessingContext& context,
    std::string* error)
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
        if (!CollectDirectoryItem(iterator->path(), items, context, error)) {
            return false;
        }
    }
    if (iterator_error) {
        if (context.mode == CollectionDirectoryMode::Retain) {
            return Fail("iterate collection directory failed: " + iterator_error.message(), error);
        }
        AddIssue(
            context, "iterate collection directory failed: " + directory.string() + ": " + iterator_error.message());
    }

    if (context.mode == CollectionDirectoryMode::Pack) {
        RemoveStoredNameConflicts(items, context);
    }
    return true;
}

bool AddCollectionStatus(const std::vector<std::string>& issues, std::vector<RepEntry>* entries, std::string* error)
{
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
    const std::string json_text = json.str();
    if (json_text.empty()) {
        return Fail("encode collection status failed", error);
    }
    const std::string bytes = "{\"complete\":false," + json_text.substr(1);
    entries->push_back({kCollectionStatusName, NpuRepFileType::Json, {bytes.begin(), bytes.end()}});
    std::sort(entries->begin(), entries->end(), [](const RepEntry& left, const RepEntry& right) {
        return left.file_name < right.file_name;
    });
    return true;
}

bool ProcessDirectoryInternal(
    const boost::filesystem::path& directory, ProcessingContext& context, std::vector<uint8_t>* encoded,
    std::string* error, bool root, std::size_t depth);

bool ProcessDirectoryItem(
    const DirectoryItem& item, ProcessingContext& context, std::vector<RepEntry>& entries, std::string* error,
    std::size_t depth)
{
    if (item.is_directory) {
        if (context.mode == CollectionDirectoryMode::Retain) {
            return ProcessDirectoryInternal(item.path, context, nullptr, error, false, depth + 1U);
        }

        RepEntry entry;
        entry.file_name = item.stored_name;
        entry.file_type = NpuRepFileType::NpuRep;
        if (!ProcessDirectoryInternal(item.path, context, &entry.payload, error, false, depth + 1U)) {
            return false;
        }
        if (entry.payload.empty()) {
            return true;
        }
        entries.push_back(std::move(entry));
        return true;
    }

    NpuRepFileType type;
    std::vector<uint8_t> payload;
    std::string item_error;
    if (!ProcessCollectionFile(
            item.path, &type, context.mode == CollectionDirectoryMode::Pack ? &payload : nullptr, &item_error)) {
        if (context.mode == CollectionDirectoryMode::Retain) {
            context.discarded.push_back(item.path);
        }
        AddIssue(context, item_error);
        return true;
    }
    ++context.file_count;
    if (context.mode == CollectionDirectoryMode::Pack) {
        entries.push_back({item.stored_name, type, std::move(payload)});
    }
    return true;
}

bool ProcessDirectoryInternal(
    const boost::filesystem::path& directory, ProcessingContext& context, std::vector<uint8_t>* encoded,
    std::string* error, bool root, std::size_t depth)
{
    if (context.mode == CollectionDirectoryMode::Pack && depth > NPU_REP_MAX_DEPTH) {
        return Fail("maximum nested report depth exceeded: " + directory.string(), error);
    }

    std::vector<DirectoryItem> items;
    std::string item_error;
    if (!CollectDirectoryItems(directory, &items, context, &item_error)) {
        if (root || context.mode == CollectionDirectoryMode::Retain) {
            return Fail(item_error, error);
        }
        AddIssue(context, item_error);
        return true;
    }

    std::vector<RepEntry> entries;
    entries.reserve(items.size());
    for (const DirectoryItem& item : items) {
        if (!ProcessDirectoryItem(item, context, entries, error, depth)) {
            return false;
        }
    }

    if (context.mode == CollectionDirectoryMode::Retain) {
        // The retain wrapper removes rejected paths before checking whether
        // any valid result file remains.
        return true;
    }
    if (root && context.file_count == 0U) {
        // Preserve the packing API contract: an empty report is reported by
        // the error text and empty output, while directory/encoding failures
        // are the cases that return false.
        Fail(
            "no valid collection files" + (context.issues.empty() ? std::string() : ": " + context.issues.front()),
            error);
        return true;
    }
    if (root && context.include_status && !context.issues.empty() && encoded != nullptr) {
        if (!AddCollectionStatus(context.issues, &entries, error)) {
            return false;
        }
    }
    std::vector<uint8_t> complete_rep;
    if (!EncodeRep(entries, &complete_rep, error)) {
        return false;
    }
    if (encoded == nullptr) {
        return Fail("encoded rep output is null", error);
    }
    *encoded = std::move(complete_rep);
    return true;
}

bool ReadCollectionFile(const boost::filesystem::path& path, std::vector<uint8_t>* payload, std::string* error)
{
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return Fail("open collection file for processing failed: " + path.string(), error);
    }
    if (payload != nullptr) {
        payload->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (input.bad()) {
            payload->clear();
            return Fail("read collection file for processing failed: " + path.string(), error);
        }
        if (payload->empty()) {
            return Fail("collection file became empty while processing: " + path.string(), error);
        }
        return true;
    }

    std::array<char, 4096> buffer{};
    bool has_content = false;
    while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
        has_content = true;
    }
    return (!input.bad() && has_content) ||
           Fail("read collection file failed or file became empty: " + path.string(), error);
}

std::string SafeText(const std::string& text)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto character = static_cast<unsigned char>(text[i]);
        const auto next = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
        if (character < 0x20 || character == 0x7f) {
            output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character);
        } else if (character == 0xc2 && next >= 0x80 && next <= 0x9f) {
            output << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(next);
            ++i;
        } else {
            output << text[i];
        }
    }
    return output.str();
}

void PrintSummaryPerformanceHints(const SummaryJsonlInfo& display, std::ostream& output)
{
    if (!display.hasHintsRecord || display.hints.empty()) {
        // CLI diagnostics go to stderr; result output below uses the caller's stdout stream.
        // Keep CLI independent of the collector's PLOG backend.
        const char* debug = std::getenv("NPU_COMPUTE_DEBUG");
        if (debug != nullptr && std::strcmp(debug, "1") == 0) {
            std::fprintf(
                stderr, "[npu-compute] performance hints reason=%s\n",
                display.hasHintsRecord ? "empty_hints" : "historical_summary_without_hints");
        }
        return;
    }
    output << "npu-compute: Performance Hints\n"
           << "  op=" << SafeText(display.opName) << "  device=" << SafeText(display.device) << '\n';
    for (const auto& hint : display.hints) {
        output << "  " << SafeText(hint) << '\n';
    }
    output.flush();
}

bool ProcessCollectionFile(
    const boost::filesystem::path& path, NpuRepFileType* type, std::vector<uint8_t>* payload, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (type == nullptr) {
        return Fail("collection file type output is null", error);
    }
    SummaryJsonlInfo summary;
    if (!ResolveCollectionFileType(path, type, error) || !ValidateCollectionFile(path, *type, error, &summary) ||
        !ValidatePmSamplingFile(path, error) || !ReadCollectionFile(path, payload, error)) {
        return false;
    }
    if (path.filename() == "summary.jsonl") {
        PrintSummaryPerformanceHints(summary, std::cout);
    }
    return true;
}

bool ProcessCollectionDirectory(
    const boost::filesystem::path& directory, CollectionDirectoryMode mode, std::vector<uint8_t>* encoded,
    std::vector<boost::filesystem::path>* discarded, std::string* error, std::vector<std::string>* issues,
    std::size_t* valid_count)
{
    if (error != nullptr) {
        error->clear();
    }
    if (mode == CollectionDirectoryMode::Pack && encoded == nullptr) {
        return Fail("encoded rep output is null", error);
    }
    if (encoded != nullptr) {
        encoded->clear();
    }
    if (valid_count != nullptr) {
        *valid_count = 0U;
    }

    boost::system::error_code status_error;
    const boost::filesystem::file_status status = boost::filesystem::symlink_status(directory, status_error);
    if (status_error) {
        return Fail(
            "inspect collection directory failed: " + directory.string() + ": " + status_error.message(), error);
    }
    if (!boost::filesystem::is_directory(status)) {
        return Fail("collection path is not a directory: " + directory.string(), error);
    }

    std::vector<std::string> local_issues;
    std::vector<boost::filesystem::path> local_discarded;
    ProcessingContext context{
        mode, issues != nullptr ? *issues : local_issues, discarded != nullptr ? *discarded : local_discarded,
        mode == CollectionDirectoryMode::Pack && issues != nullptr};
    try {
        const bool result = ProcessDirectoryInternal(directory, context, encoded, error, true, 1U);
        if (valid_count != nullptr) {
            *valid_count = context.file_count;
        }
        return result;
    } catch (const std::bad_alloc&) {
        if (encoded != nullptr) {
            encoded->clear();
        }
        return Fail("allocate recursive rep buffer failed", error);
    }
}

} // namespace

bool ValidateAndCleanCollectionDirectory(
    const boost::filesystem::path& directory, std::string* error, std::vector<std::string>* issues)
{
    // Retain mode uses the shared traversal and per-file processing. Cleanup
    // is deliberately kept at this semantic entry point and runs only after
    // the complete scan has succeeded.
    if (error != nullptr) {
        error->clear();
    }
    std::vector<boost::filesystem::path> discarded;
    std::size_t valid_count = 0U;
    if (!ProcessCollectionDirectory(
            directory, CollectionDirectoryMode::Retain, nullptr, &discarded, error, issues, &valid_count)) {
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
    return valid_count != 0U || Fail("no valid collection result files", error);
}

bool PackDirectoryToRep(
    const boost::filesystem::path& directory, std::vector<uint8_t>* encoded, std::string* error,
    std::vector<std::string>* issues)
{
    // Pack mode uses the shared traversal and per-file processing, then
    // returns the encoded nested report to the caller.
    return ProcessCollectionDirectory(
        directory, CollectionDirectoryMode::Pack, encoded, nullptr, error, issues, nullptr);
}

} // namespace npucompute::cli
