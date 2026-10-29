/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/pm_sampling_file_validator.h"
#include <string>
#include <string_view>

#define BOOST_JSON_NO_LIB
#include <boost/json/src.hpp>
#include <boost/filesystem.hpp>
#include "shared/sqlite_runtime_loader.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

namespace npucompute::cli {
namespace {
constexpr std::string_view kHexPrefix = "0x";
constexpr unsigned kHexBase = 16;
constexpr unsigned kDecimalBase = 10;
constexpr int kAddressColumn = sqlite::kFirstResultColumn;
void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
bool ParsePc(const std::string& text, std::uint64_t& pc)
{
    if (text.size() <= kHexPrefix.size() || text.compare(0, kHexPrefix.size(), kHexPrefix) != 0) {
        return false;
    }
    pc = 0;
    for (std::size_t index = kHexPrefix.size(); index < text.size(); ++index) {
        const auto c = text[index];
        const unsigned digit =
            c >= '0' && c <= '9' ? static_cast<unsigned>(c) - static_cast<unsigned>('0') :
            c >= 'a' && c <= 'f' ? static_cast<unsigned>(c) - static_cast<unsigned>('a') + kDecimalBase :
            c >= 'A' && c <= 'F' ? static_cast<unsigned>(c) - static_cast<unsigned>('A') + kDecimalBase :
                                   kHexBase;
        if (digit >= kHexBase || pc > (std::numeric_limits<std::uint64_t>::max() - digit) / kHexBase) {
            return false;
        }
        pc = pc * kHexBase + digit;
    }
    return true;
}
std::set<std::uint64_t> DatabasePcs(const boost::filesystem::path& path, bool* hasTable)
{
    if (hasTable != nullptr) {
        *hasTable = false;
    }
    std::ifstream input(path.string(), std::ios::binary);
    std::array<char, sizeof(sqlite::kDatabaseHeader)> magic{};
    input.read(magic.data(), magic.size());
    Require(
        input.gcount() == static_cast<std::streamsize>(magic.size()) &&
            std::memcmp(magic.data(), sqlite::kDatabaseHeader, magic.size()) == 0,
        "invalid PmSampling SQLite header");
    sqlite3* raw = nullptr;
    const auto opened =
        sqlite::GetApi().openV2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    const std::unique_ptr<sqlite3, decltype(sqlite::GetApi().closeV2)> db(raw, sqlite::GetApi().closeV2);
    Require(opened == SQLITE_OK, "cannot open PmSampling SQLite");
    // A shared database may contain only CSV tables. Reading sqlite_master
    // also detects an unreadable database without requiring PmSampling data.
    sqlite3_stmt* rawTable = nullptr;
    const auto inspected = sqlite::GetApi().prepareV2(
        db.get(), "SELECT name FROM sqlite_master WHERE type='table' AND name='PcSampling'", sqlite::kNullTerminatedSql,
        &rawTable, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(sqlite::GetApi().finalize)> table(rawTable, sqlite::GetApi().finalize);
    Require(inspected == SQLITE_OK, "cannot inspect result database");
    const auto tableStatus = sqlite::GetApi().step(table.get());
    Require(tableStatus == SQLITE_ROW || tableStatus == SQLITE_DONE, "cannot read database tables");
    if (tableStatus == SQLITE_DONE) {
        return {};
    }
    if (hasTable != nullptr) {
        *hasTable = true;
    }
    sqlite3_stmt* statement = nullptr;
    const auto prepared = sqlite::GetApi().prepareV2(
        db.get(), "SELECT address FROM PcSampling", sqlite::kNullTerminatedSql, &statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(sqlite::GetApi().finalize)> query(
        statement, sqlite::GetApi().finalize);
    Require(prepared == SQLITE_OK, "cannot read PmSampling addresses");
    std::set<std::uint64_t> pcs;
    int status = SQLITE_OK;
    while ((status = sqlite::GetApi().step(query.get())) == SQLITE_ROW) {
        Require(
            sqlite::GetApi().columnType(query.get(), kAddressColumn) == SQLITE_INTEGER,
            "invalid PmSampling Address type");
        const auto value = sqlite::GetApi().columnInt64(query.get(), kAddressColumn);
        Require(
            value >= 0 && pcs.insert(static_cast<std::uint64_t>(value)).second,
            "invalid or duplicate PmSampling Address");
    }
    Require(status == SQLITE_DONE, "unreadable PmSampling addresses");
    return pcs;
}
void ValidateDisassembly(const boost::filesystem::path& path)
{
    std::ifstream input(path.string());
    Require(bool(input), "cannot read PmSampling disassembly");
    // Accept the CANN objdump byte/word forms, ignoring labels and source lines.
    const std::regex instruction("^[ \\t]*[0-9a-fA-F]+:[ \\t]+[0-9a-fA-F]{2}");
    std::string line;
    while (std::getline(input, line)) {
        if (std::regex_search(line, instruction)) {
            return;
        }
    }
    throw std::runtime_error("PmSampling disassembly contains no instruction");
}
void ValidateStack(const boost::filesystem::path& path)
{
    std::ifstream input(path.string(), std::ios::binary);
    Require(bool(input), "cannot read PmSampling PC stack");
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Require(!input.bad(), "cannot read complete PmSampling PC stack");
    const auto data = boost::json::parse(contents);
    Require(data.is_array(), "PmSampling PC stack is not a JSON array");
    std::set<std::uint64_t> addresses;
    for (const auto& record : data.as_array()) {
        Require(record.is_object(), "invalid PmSampling PC stack record");
        const auto* address = record.as_object().if_contains("Address");
        const auto* symbols = record.as_object().if_contains("Symbol");
        Require(
            address && address->is_string() && symbols && symbols->is_array(), "invalid PmSampling PC stack fields");
        const std::string text(address->as_string().data(), address->as_string().size());
        std::uint64_t pc = 0;
        Require(
            ParsePc(text, pc) && addresses.insert(pc).second, "unknown or duplicate Address in PmSampling PC stack");
        for (const auto& symbol : symbols->as_array()) {
            Require(symbol.is_object(), "invalid PmSampling inline frame");
        }
    }
    Require(!addresses.empty(), "PmSampling PC stack contains no addresses");
}
} // namespace

bool ValidatePmSamplingFile(const boost::filesystem::path& path, std::string* error)
{
    if (error) {
        error->clear();
    }
    const auto name = path.filename().string();
    if (name != "summary.db" && name != "objdump.txt" && name != "symbolizer.json") {
        return true;
    }
    try {
        Require(
            boost::filesystem::is_regular_file(boost::filesystem::symlink_status(path)), "not a regular result file");
        Require(boost::filesystem::file_size(path) > 0, "empty result file");
        if (name == "summary.db") {
            DatabasePcs(path, nullptr);
        } else if (name == "objdump.txt") {
            ValidateDisassembly(path);
        } else {
            ValidateStack(path);
        }
        return true;
    } catch (const std::exception& failure) {
        if (error) {
            *error = "PmSampling file validation failed: " + path.string() + ": " + failure.what() +
                     "; excluded this file; other valid results are retained";
        }
        return false;
    }
}

bool IsEmptyPmSamplingDatabase(const boost::filesystem::path& path, bool* empty, std::string* error)
{
    if (empty != nullptr) {
        *empty = false;
    }
    if (error != nullptr) {
        error->clear();
    }
    try {
        Require(
            boost::filesystem::is_regular_file(boost::filesystem::symlink_status(path)), "not a regular result file");
        Require(boost::filesystem::file_size(path) > 0, "empty result file");
        bool hasTable = false;
        const auto pcs = DatabasePcs(path, &hasTable);
        if (empty != nullptr) {
            *empty = hasTable && pcs.empty();
        }
        return true;
    } catch (const std::exception& failure) {
        if (error != nullptr) {
            *error = "PmSampling database inspection failed: " + path.string() + ": " + failure.what();
        }
        return false;
    }
}
} // namespace npucompute::cli
