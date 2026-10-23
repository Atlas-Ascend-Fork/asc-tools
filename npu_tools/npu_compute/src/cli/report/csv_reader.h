/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_CSV_READER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_CSV_READER_H

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace npucompute::cli {

enum class CsvColumnType { Integer, Real, Text };

struct CsvColumn {
    std::string sourceName;
    std::string name;
    std::optional<std::string> unit;
    CsvColumnType type = CsvColumnType::Text;
};

struct CsvSchema {
    std::string tableName;
    std::vector<CsvColumn> columns;
    std::uint64_t rowCount = 0;
};

struct CsvRecordLocation {
    std::size_t record = 0; // One-based logical record, including the header.
    std::size_t firstLine = 1;
    std::size_t lastLine = 1;
};

// Consumes one logical record at a time. The input must outlive this reader.
// Returns false only at EOF; I/O and malformed CSV errors include the source name.
class CsvReader {
public:
    CsvReader(std::istream& input, std::string sourceName);
    bool ReadRecord(std::vector<std::string>& fields);
    const CsvRecordLocation& Location() const { return location_; }

private:
    int ReadCharacter();
    int PeekCharacter();
    int NextCharacter();
    [[noreturn]] void Fail(const std::string& reason) const;

    std::istream& input_;
    std::string sourceName_;
    std::string prefix_;
    std::size_t prefixPosition_ = 0;
    std::size_t recordsRead_ = 0;
    bool initialized_ = false;
    bool previousWasCr_ = false;
    std::size_t physicalLine_ = 1;
    CsvRecordLocation location_;
    std::optional<int> lookahead_;
};

std::string NormalizeCsvTableName(std::string_view filename);
std::string NormalizeCsvColumnName(std::string_view field);
bool ParseCsvInteger(std::string_view value, std::int64_t& integer);
// Canonical fixed-point decimals only; rejects excessive significant digits,
// overflow, nonzero underflow and forms whose spelling must remain TEXT.
bool ParseCsvReal(std::string_view value, double& real);

// Scans the entire file without retaining its data rows. Empty/header-only
// files have rowCount == 0. Callers decide whether to publish a database.
CsvSchema AnalyzeCsv(const std::string& path);

} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_CSV_READER_H
