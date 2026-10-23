/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/csv_reader.h"

#include <boost/filesystem/path.hpp>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace npucompute::cli {
namespace {
constexpr std::string_view kUtf8Bom = "\xef\xbb\xbf";
constexpr std::string_view kCsvExtension = ".csv";
constexpr int kDecimalBase = 10;
constexpr std::size_t kReliableDecimalDigits = std::numeric_limits<double>::digits10;

bool IsLower(char value) { return value >= 'a' && value <= 'z'; }
bool IsUpper(char value) { return value >= 'A' && value <= 'Z'; }
bool IsDigit(char value) { return value >= '0' && value <= '9'; }
bool IsAlphanumeric(char value) { return IsLower(value) || IsUpper(value) || IsDigit(value); }
char Lower(char value) { return IsUpper(value) ? static_cast<char>(value - 'A' + 'a') : value; }
char Upper(char value) { return IsLower(value) ? static_cast<char>(value - 'a' + 'A') : value; }

std::string Lowercase(std::string_view value)
{
    std::string result(value);
    for (auto& character : result)
        character = Lower(character);
    return result;
}

std::string_view Trim(std::string_view value)
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && space(value.back()))
        value.remove_suffix(1);
    return value;
}

[[noreturn]] void NameError(std::string_view kind, std::string_view name, const std::string& reason)
{
    throw std::runtime_error("Cannot normalize CSV " + std::string(kind) + " '" + std::string(name) + "': " + reason);
}

void ValidateName(std::string_view original, const std::string& name, bool column)
{
    const auto kind = column ? "column" : "table";
    if (original.find('\0') != std::string_view::npos)
        NameError(kind, original, "name contains a NUL character");
    if (name.empty())
        NameError(kind, original, "name has no letters or digits");
    if (IsDigit(name.front()))
        NameError(kind, original, "normalized name '" + name + "' starts with a digit");
    const auto folded = Lowercase(name);
    if (Lowercase(Trim(original)).rfind("sqlite_", 0) == 0 || folded.rfind("sqlite_", 0) == 0)
        NameError(kind, original, "sqlite_ names are reserved by SQLite");
    if (column && (folded == "rowid" || folded == "oid"))
        NameError(kind, original, "normalized name '" + name + "' would hide the CSV record order (SQLite rowid)");
    if (!column && (folded == "pcsampling" || folded == "fieldunitmap"))
        NameError(kind, original, "normalized name '" + name + "' is reserved for a dedicated result table");
}

std::string TableWords(std::string_view value)
{
    std::string result;
    bool startWord = true;
    for (const auto character : value) {
        if (character == '_') {
            startWord = true;
            continue;
        }
        if (!IsAlphanumeric(character))
            NameError("table", value, "filename contains an invalid character");
        result.push_back(startWord ? Upper(character) : character);
        startWord = false;
    }
    return result;
}

std::string ColumnWords(std::string_view value)
{
    std::string result;
    bool startWord = true;
    // Existing CSV headers already use underscores; retain those word
    // boundaries so hardware names such as L0C stay intact.
    const bool explicitWords = value.find('_') != std::string_view::npos;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto character = value[i];
        if (!IsAlphanumeric(character)) {
            startWord = true;
            continue;
        }
        const bool camelBoundary = !explicitWords && i > 0 && IsUpper(character) &&
                                   (IsLower(value[i - 1]) || IsDigit(value[i - 1]) ||
                                    (IsUpper(value[i - 1]) && i + 1 < value.size() && IsLower(value[i + 1])));
        if (!result.empty() && (startWord || camelBoundary))
            result.push_back('_');
        result.push_back(Lower(character));
        startWord = false;
    }
    return result;
}

CsvColumn ParseColumn(std::string_view field)
{
    auto body = Trim(field);
    CsvColumn column;
    column.sourceName = std::string(field);
    const auto unitStart = body.find('(');
    const auto unitEnd = body.find(')');
    if (unitStart != std::string_view::npos || unitEnd != std::string_view::npos) {
        if (unitStart == std::string_view::npos || unitEnd != body.size() - 1 || unitStart >= unitEnd ||
            body.find('(', unitStart + 1) != std::string_view::npos)
            NameError("column", field, "unit must be a single parenthesized suffix");
        const auto unit = body.substr(unitStart + 1, unitEnd - unitStart - 1);
        if (Trim(unit).empty())
            NameError("column", field, "unit suffix is empty");
        column.unit = std::string(unit);
        body = body.substr(0, unitStart);
    }
    column.name = ColumnWords(body);
    ValidateName(field, column.name, true);
    return column;
}

std::string LocationText(const CsvRecordLocation& location)
{
    std::string text = "record " + std::to_string(location.record) + ", physical ";
    if (location.firstLine == location.lastLine)
        return text + "line " + std::to_string(location.firstLine);
    return text + "lines " + std::to_string(location.firstLine) + "-" + std::to_string(location.lastLine);
}

bool IntegerFitsDouble(std::int64_t integer)
{
    auto magnitude = integer < 0 ? static_cast<std::uint64_t>(-(integer + 1)) + 1 : static_cast<std::uint64_t>(integer);
    // Remove powers of two; the remaining significant bits must fit binary64.
    while (magnitude != 0 && (magnitude & 1U) == 0)
        magnitude >>= 1;
    return magnitude <= (UINT64_C(1) << std::numeric_limits<double>::digits);
}

struct ColumnInference {
    bool integer = false;
    bool real = false;
    bool text = false;
    bool exactIntegers = true;

    void Observe(std::string_view value)
    {
        if (value.empty() || value == "NA" || text)
            return;
        std::int64_t parsedInteger = 0;
        double parsedReal = 0;
        if (ParseCsvInteger(value, parsedInteger)) {
            integer = true;
            exactIntegers = exactIntegers && IntegerFitsDouble(parsedInteger);
        } else if (ParseCsvReal(value, parsedReal)) {
            real = true;
        } else {
            text = true;
        }
    }
    CsvColumnType Type() const
    {
        if (text || (!integer && !real) || (real && !exactIntegers))
            return CsvColumnType::Text;
        return real ? CsvColumnType::Real : CsvColumnType::Integer;
    }
};

enum class FieldState { Start, Unquoted, Quoted, AfterQuote };

} // namespace

CsvReader::CsvReader(std::istream& input, std::string sourceName) : input_(input), sourceName_(std::move(sourceName)) {}

[[noreturn]] void CsvReader::Fail(const std::string& reason) const
{
    auto location = location_;
    location.lastLine = std::max(location.lastLine, physicalLine_);
    throw std::runtime_error(
        "Cannot read/parse CSV '" + sourceName_ + "' at " + LocationText(location) + ": " + reason);
}

int CsvReader::ReadCharacter()
{
    if (!initialized_) {
        // Buffer just the first three bytes so BOM detection never needs seeking.
        for (std::size_t i = 0; i < kUtf8Bom.size(); ++i) {
            const int value = input_.get();
            if (input_.bad() || (input_.fail() && !input_.eof()))
                Fail("input stream read failed");
            if (value == std::char_traits<char>::eof())
                break;
            prefix_.push_back(static_cast<char>(value));
        }
        if (prefix_ == kUtf8Bom)
            prefix_.clear();
        initialized_ = true;
    }
    if (prefixPosition_ < prefix_.size())
        return static_cast<unsigned char>(prefix_[prefixPosition_++]);
    const int value = input_.get();
    if (input_.bad() || (input_.fail() && !input_.eof()))
        Fail("input stream read failed");
    return value;
}

int CsvReader::PeekCharacter()
{
    if (!lookahead_)
        lookahead_ = ReadCharacter();
    return *lookahead_;
}

int CsvReader::NextCharacter()
{
    const int value = lookahead_ ? *lookahead_ : ReadCharacter();
    lookahead_.reset();
    if (value != std::char_traits<char>::eof()) {
        location_.lastLine = value == '\n' && previousWasCr_ ? physicalLine_ - 1 : physicalLine_;
        if (value == '\r' || (value == '\n' && !previousWasCr_))
            ++physicalLine_;
        previousWasCr_ = value == '\r';
    }
    return value;
}

bool CsvReader::ReadRecord(std::vector<std::string>& fields)
{
    location_ = {recordsRead_ + 1, physicalLine_, physicalLine_};
    fields.clear();
    std::string field;
    auto state = FieldState::Start;
    bool hasInput = false;
    for (;;) {
        const int value = NextCharacter();
        if (value == std::char_traits<char>::eof()) {
            if (state == FieldState::Quoted)
                Fail("quoted field is not closed before end of file");
            if (!hasInput)
                return false;
            fields.push_back(std::move(field));
            ++recordsRead_;
            return true;
        }
        hasInput = true;
        const char character = static_cast<char>(value);
        if (state == FieldState::Quoted) {
            if (character == '"')
                state = FieldState::AfterQuote;
            else
                field.push_back(character);
        } else if (character == ',' || character == '\r' || character == '\n') {
            fields.push_back(std::move(field));
            field.clear();
            if (character == ',') {
                state = FieldState::Start;
                continue;
            }
            if (character == '\r' && PeekCharacter() == '\n')
                NextCharacter();
            ++recordsRead_;
            return true;
        } else if (character == '"') {
            if (state == FieldState::AfterQuote) {
                field.push_back('"');
                state = FieldState::Quoted;
            } else if (state == FieldState::Start) {
                state = FieldState::Quoted;
            } else {
                Fail("unexpected quote in an unquoted field");
            }
        } else {
            if (state == FieldState::AfterQuote)
                Fail("unexpected character after closing quote");
            field.push_back(character);
            state = FieldState::Unquoted;
        }
    }
}

std::string NormalizeCsvTableName(std::string_view filename)
{
    auto stem = filename;
    if (stem.size() >= kCsvExtension.size() && stem.substr(stem.size() - kCsvExtension.size()) == kCsvExtension)
        stem.remove_suffix(kCsvExtension.size());
    const auto name = TableWords(stem);
    ValidateName(filename, name, false);
    return name;
}

std::string NormalizeCsvColumnName(std::string_view field) { return ParseColumn(field).name; }

bool ParseCsvInteger(std::string_view value, std::int64_t& integer)
{
    if (value.empty())
        return false;
    const std::size_t start = value.front() == '-' ? 1 : 0;
    if (start == value.size())
        return false;
    if (value[start] == '0' && (start != 0 || value.size() != 1))
        return false;
    for (std::size_t i = start; i < value.size(); ++i)
        if (!IsDigit(value[i]))
            return false;
    std::size_t consumed = 0;
    try {
        const auto parsed = std::stoll(std::string(value), &consumed, kDecimalBase);
        if (consumed != value.size())
            return false;
        integer = static_cast<std::int64_t>(parsed);
    } catch (const std::invalid_argument&) {
        return false;
    } catch (const std::out_of_range&) {
        return false;
    }
    return true;
}

bool ParseCsvReal(std::string_view value, double& real)
{
    if (value.empty())
        return false;
    const std::size_t start = value.front() == '-' ? 1 : 0;
    const auto dot = value.find('.');
    if (dot == std::string_view::npos || dot <= start || dot + 1 == value.size())
        return false;
    if (value[start] == '0' && dot != start + 1)
        return false;
    std::size_t first = value.size();
    std::size_t last = 0;
    for (std::size_t i = start; i < value.size(); ++i) {
        if (i == dot)
            continue;
        if (!IsDigit(value[i]))
            return false;
        if (value[i] != '0') {
            if (first == value.size())
                first = i;
            last = i;
        }
    }
    const auto digits = first == value.size() ? 0 : last - first + 1 - (first < dot && dot < last ? 1 : 0);
    if (digits > kReliableDecimalDigits)
        return false;
    std::size_t consumed = 0;
    try {
        const auto parsed = std::stod(std::string(value), &consumed);
        if (consumed != value.size() || !std::isfinite(parsed))
            return false;
        if (parsed == 0 && (first != value.size() || start != 0))
            return false;
        real = parsed;
    } catch (const std::invalid_argument&) {
        return false;
    } catch (const std::out_of_range&) {
        return false;
    }
    return true;
}

CsvSchema AnalyzeCsv(const std::string& path)
{
    if (path.find('\0') != std::string::npos)
        throw std::runtime_error("Cannot open CSV: path contains a NUL character");
    errno = 0;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        const int openError = errno;
        const auto reason = openError != 0 ? std::error_code(openError, std::generic_category()).message() :
                                             "input stream could not be opened";
        throw std::runtime_error("Cannot open CSV '" + path + "' for reading: " + reason);
    }
    CsvSchema schema;
    CsvReader reader(input, path);
    try {
        schema.tableName = NormalizeCsvTableName(boost::filesystem::path(path).filename().string());
        std::vector<std::string> fields;
        if (!reader.ReadRecord(fields))
            return schema;
        std::unordered_map<std::string, std::string> names;
        for (const auto& source : fields) {
            auto column = ParseColumn(source);
            const auto inserted = names.emplace(column.name, source);
            if (!inserted.second) {
                throw std::runtime_error(
                    "columns '" + inserted.first->second + "' and '" + source + "' both map to SQLite column '" +
                    column.name + "'");
            }
            schema.columns.push_back(std::move(column));
        }
        std::vector<ColumnInference> types(schema.columns.size());
        while (reader.ReadRecord(fields)) {
            if (fields.size() != schema.columns.size()) {
                throw std::runtime_error(
                    "expected " + std::to_string(schema.columns.size()) + " fields, found " +
                    std::to_string(fields.size()));
            }
            for (std::size_t i = 0; i < fields.size(); ++i)
                types[i].Observe(fields[i]);
            if (schema.rowCount == std::numeric_limits<std::uint64_t>::max())
                throw std::runtime_error("CSV record count exceeds uint64 range");
            ++schema.rowCount;
        }
        for (std::size_t i = 0; i < schema.columns.size(); ++i)
            schema.columns[i].type = types[i].Type();
        return schema;
    } catch (const std::runtime_error& error) {
        const auto table = schema.tableName.empty() ? "" : " for table '" + schema.tableName + "'";
        const auto location = reader.Location().record == 0 ? "" : " at " + LocationText(reader.Location());
        throw std::runtime_error("Cannot analyze CSV '" + path + "'" + table + location + ": " + error.what());
    }
}

} // namespace npucompute::cli
