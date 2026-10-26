/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/collection_file_validator.h"

#include <boost/filesystem.hpp>
#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace npucompute::cli {
namespace {

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

std::string Text(const boost::json::value* value)
{
    if (value == nullptr || value->is_null()) {
        return "NA";
    }
    if (value->is_string()) {
        return std::string(value->as_string());
    }
    if (value->is_number()) {
        return boost::json::serialize(*value);
    }
    throw std::runtime_error("invalid OpInfoSummary context field");
}

SummaryJsonlInfo ParseSummaryJsonl(std::ifstream& input)
{
    SummaryJsonlInfo info;
    std::string line;
    std::string previous;
    std::set<std::string> categories;
    while (std::getline(input, line)) {
        if (input.eof()) {
            throw std::runtime_error("summary must end with a newline");
        }
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        const auto record = boost::json::parse(line);
        if (!record.is_object()) {
            throw std::runtime_error("summary record must be an object");
        }
        const auto& object = record.as_object();
        const auto* categoryValue = object.if_contains("category");
        if (categoryValue == nullptr || !categoryValue->is_string()) {
            throw std::runtime_error("summary category must be a string");
        }
        const std::string category(categoryValue->as_string());
        if (category.empty() || !categories.insert(category).second) {
            throw std::runtime_error("empty or duplicate summary category");
        }
        if (previous == "OpInfoSummary" || (previous == "PerformanceHints" && category != "OpInfoSummary")) {
            throw std::runtime_error("invalid PerformanceHints/OpInfoSummary ordering");
        }
        if (category == "PerformanceHints") {
            const auto* hints = object.if_contains("hints");
            if (hints == nullptr || !hints->is_array()) {
                throw std::runtime_error("PerformanceHints.hints must be a string array");
            }
            info.hasHintsRecord = true;
            for (const auto& hint : hints->as_array()) {
                if (!hint.is_string()) {
                    throw std::runtime_error("PerformanceHints.hints contains a non-string item");
                }
                info.hints.emplace_back(hint.as_string());
            }
        } else if (category == "OpInfoSummary") {
            info.opName = Text(object.if_contains("Op Name"));
            info.device = Text(object.if_contains("Device Id"));
        }
        previous = category;
    }
    if (input.bad() || previous != "OpInfoSummary") {
        throw std::runtime_error("summary is incomplete or missing final OpInfoSummary");
    }
    return info;
}

bool ValidateJsonl(
    std::ifstream* input, const boost::filesystem::path& path, std::string* error, SummaryJsonlInfo* summary)
{
    if (path.filename() == "summary.jsonl") {
        try {
            auto parsed = ParseSummaryJsonl(*input);
            if (summary != nullptr) {
                *summary = std::move(parsed);
            }
            return true;
        } catch (const std::exception& exception) {
            return Fail("invalid summary JSONL: " + path.string() + ": " + exception.what(), error);
        }
    }

    std::size_t line_count = 0;
    bool line_has_content = false;
    char last = '\0';
    char character = '\0';
    while (input->get(character)) {
        last = character;
        if (character == '\n') {
            if (line_has_content) {
                ++line_count;
            }
            line_has_content = false;
        } else if (character != '\r') {
            line_has_content = true;
        }
    }
    if (input->bad()) {
        return Fail("read JSONL collection file failed: " + path.string(), error);
    }
    if (last != '\n') {
        return Fail("JSONL collection file does not end with a newline: " + path.string(), error);
    }
    if (path.filename() == "HardwareInfo.jsonl" && line_count < 5U) {
        return Fail("JSONL collection file contains fewer than five lines: " + path.string(), error);
    }
    if (line_count == 0) {
        return Fail("JSONL has no records: " + path.string(), error);
    }
    return true;
}

bool HasContent(std::string line)
{
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return !line.empty();
}

bool ValidateCsv(std::ifstream* input, const boost::filesystem::path& path, std::string* error)
{
    std::string line;
    if (!std::getline(*input, line) || !HasContent(line)) {
        return Fail("CSV collection file has no header: " + path.string(), error);
    }
    while (std::getline(*input, line)) {
        if (HasContent(line)) {
            return true;
        }
    }
    if (input->bad()) {
        return Fail("read CSV collection file failed: " + path.string(), error);
    }
    return Fail("CSV collection file has no data row: " + path.string(), error);
}

} // namespace

bool ResolveCollectionFileType(const boost::filesystem::path& path, NpuRepFileType* type, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (type == nullptr) {
        return Fail("collection file type output is null", error);
    }

    const std::string extension = path.extension().string();
    if (extension == ".json") {
        *type = NpuRepFileType::Json;
    } else if (extension == ".jsonl") {
        *type = NpuRepFileType::Jsonl;
    } else if (extension == ".csv") {
        *type = NpuRepFileType::Csv;
    } else if (extension == ".db") {
        *type = NpuRepFileType::Sqlite;
    } else if (extension == ".txt") {
        *type = NpuRepFileType::Txt;
    } else {
        return Fail("unknown collection file type: " + path.string(), error);
    }
    return true;
}

bool ValidateCollectionFile(
    const boost::filesystem::path& path, NpuRepFileType type, std::string* error, SummaryJsonlInfo* summary)
{
    if (error != nullptr) {
        error->clear();
    }
    boost::system::error_code status_error;
    const boost::filesystem::file_status status = boost::filesystem::symlink_status(path, status_error);
    if (status_error) {
        return Fail("inspect collection file failed: " + path.string() + ": " + status_error.message(), error);
    }
    if (!boost::filesystem::is_regular_file(status)) {
        return Fail("collection path is not a regular file: " + path.string(), error);
    }

    boost::system::error_code size_error;
    const uintmax_t size = boost::filesystem::file_size(path, size_error);
    if (size_error) {
        return Fail("read collection file size failed: " + path.string() + ": " + size_error.message(), error);
    }
    if (size == 0U) {
        return Fail("collection file is empty: " + path.string(), error);
    }

    switch (type) {
        case NpuRepFileType::Json:
        case NpuRepFileType::Sqlite:
        case NpuRepFileType::Txt:
            return true;
        case NpuRepFileType::Jsonl:
        case NpuRepFileType::Csv:
            break;
        case NpuRepFileType::NpuRep:
            return Fail("nested rep is not a collection file: " + path.string(), error);
        default:
            return Fail("unknown collection file type for: " + path.string(), error);
    }

    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return Fail("open collection file failed: " + path.string(), error);
    }
    if (type == NpuRepFileType::Jsonl) {
        return ValidateJsonl(&input, path, error, summary);
    }
    return ValidateCsv(&input, path, error);
}

} // namespace npucompute::cli
