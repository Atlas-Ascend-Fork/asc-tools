/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/csv_sqlite_writer.h"
#include "shared/sqlite_runtime_loader.h"

#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace npucompute::cli {
namespace {
constexpr int kFieldNameParameter = sqlite::kFirstBindParameter;
constexpr int kUnitParameter = kFieldNameParameter + 1;
constexpr int kUnitResultColumn = sqlite::kFirstResultColumn;
constexpr const char* SAVEPOINT = "SAVEPOINT npu_compute_csv_table";
constexpr const char* ROLLBACK = "ROLLBACK TO npu_compute_csv_table";
constexpr const char* RELEASE = "RELEASE npu_compute_csv_table";

void Check(sqlite3* database, int status)
{
    if (status != SQLITE_OK)
        throw std::runtime_error(sqlite::GetApi().errmsg(database));
}

std::string QuoteIdentifier(const std::string& name)
{
    std::string result = "\"";
    for (const auto character : name) {
        if (character == '"')
            result += '"';
        result += character;
    }
    return result + '"';
}

class Statement {
public:
    Statement(sqlite3* database, const std::string& sql) : database_(database)
    {
        const auto status =
            sqlite::GetApi().prepareV2(database, sql.c_str(), sqlite::kNullTerminatedSql, &statement_, nullptr);
        if (status != SQLITE_OK) {
            if (statement_)
                sqlite::GetApi().finalize(statement_);
            Check(database, status);
        }
    }
    ~Statement()
    {
        if (statement_)
            sqlite::GetApi().finalize(statement_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* Get() const { return statement_; }
    void BindText(int index, const std::string& text)
    {
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("text exceeds SQLite's supported binding length");
        Check(database_, sqlite::GetApi().bindText(statement_, index, text.data(), static_cast<int>(text.size())));
    }
    void Reset()
    {
        Check(database_, sqlite::GetApi().reset(statement_));
        Check(database_, sqlite::GetApi().clearBindings(statement_));
    }
    void Complete()
    {
        const auto status = sqlite::GetApi().step(statement_);
        if (status != SQLITE_DONE)
            Check(database_, status);
    }
    void Finish()
    {
        auto* statement = std::exchange(statement_, nullptr);
        Check(database_, sqlite::GetApi().finalize(statement));
    }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

std::string UnitLabel(const std::optional<std::string>& unit) { return unit ? "'" + *unit + "'" : "<none>"; }

void WriteUnits(sqlite3* database, const CsvSchema& schema)
{
    const auto& api = sqlite::GetApi();
    Check(
        database, api.exec(database, "CREATE TABLE IF NOT EXISTS FieldUnitMap(field_name TEXT PRIMARY KEY,unit TEXT)"));
    Statement find(database, "SELECT unit FROM FieldUnitMap WHERE field_name=?");
    Statement insert(database, "INSERT INTO FieldUnitMap(field_name,unit) VALUES(?,?)");
    for (const auto& column : schema.columns) {
        find.BindText(kFieldNameParameter, column.name);
        const auto status = api.step(find.Get());
        if (status == SQLITE_ROW) {
            std::optional<std::string> existing;
            if (api.columnType(find.Get(), kUnitResultColumn) != SQLITE_NULL) {
                const auto* text = api.columnText(find.Get(), kUnitResultColumn);
                if (!text)
                    throw std::runtime_error("cannot read existing unit for field '" + column.name + "'");
                existing =
                    std::string(reinterpret_cast<const char*>(text), api.columnBytes(find.Get(), kUnitResultColumn));
            }
            if (existing != column.unit)
                throw std::runtime_error(
                    "unit conflict for CSV field '" + column.sourceName + "' (normalized field '" + column.name +
                    "'): existing unit " + UnitLabel(existing) + ", incoming unit " + UnitLabel(column.unit));
            find.Complete();
        } else {
            if (status != SQLITE_DONE)
                Check(database, status);
            insert.BindText(kFieldNameParameter, column.name);
            if (column.unit)
                insert.BindText(kUnitParameter, *column.unit);
            else
                Check(database, api.bindNull(insert.Get(), kUnitParameter));
            insert.Complete();
            insert.Reset();
        }
        find.Reset();
    }
    find.Finish();
    insert.Finish();
}

const char* SqlType(CsvColumnType type)
{
    switch (type) {
        case CsvColumnType::Integer:
            return "INTEGER";
        case CsvColumnType::Real:
            return "REAL";
        case CsvColumnType::Text:
            return "TEXT";
    }
    throw std::runtime_error("unknown CSV column type");
}

std::string CreateSql(const CsvSchema& schema)
{
    std::string sql = "CREATE TABLE " + QuoteIdentifier(schema.tableName) + "(";
    for (std::size_t index = 0; index < schema.columns.size(); ++index) {
        if (index)
            sql += ',';
        sql += QuoteIdentifier(schema.columns[index].name) + ' ' + SqlType(schema.columns[index].type);
    }
    return sql + ')';
}

std::string InsertSql(const CsvSchema& schema)
{
    std::string sql = "INSERT INTO " + QuoteIdentifier(schema.tableName) + " VALUES(";
    for (std::size_t index = 0; index < schema.columns.size(); ++index)
        sql += index ? ",?" : "?";
    return sql + ')';
}

bool IntegerFitsReal(std::int64_t integer)
{
    // Count significant binary digits, including values near both int64 limits.
    auto magnitude =
        integer < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(integer) : static_cast<std::uint64_t>(integer);
    if (magnitude == 0)
        return true;
    while ((magnitude & 1) == 0)
        magnitude >>= 1;
    return magnitude <= (std::uint64_t{1} << std::numeric_limits<double>::digits);
}

void BindValue(sqlite3* database, Statement& insert, int index, const CsvColumn& column, const std::string& value)
{
    const auto& api = sqlite::GetApi();
    if (value == "NA") {
        Check(database, api.bindNull(insert.Get(), index));
    } else if (value.empty() || column.type == CsvColumnType::Text) {
        insert.BindText(index, value);
    } else {
        std::int64_t integer = 0;
        const auto isInteger = ParseCsvInteger(value, integer);
        if (column.type == CsvColumnType::Integer && isInteger) {
            Check(database, api.bindInt64(insert.Get(), index, integer));
            return;
        }
        if (column.type == CsvColumnType::Real) {
            double real = 0;
            if (isInteger && IntegerFitsReal(integer))
                real = static_cast<double>(integer);
            else if (isInteger || !ParseCsvReal(value, real))
                throw std::runtime_error("value no longer matches analyzed REAL type without integer precision loss; "
                                         "CSV changed after analysis");
            Check(database, api.bindDouble(insert.Get(), index, real));
            return;
        }
        throw std::runtime_error("value no longer matches analyzed INTEGER type; CSV changed after analysis");
    }
}

std::string RecordContext(const CsvRecordLocation& location)
{
    if (location.record == 0)
        return {};
    const auto lines =
        location.firstLine == location.lastLine ?
            "physical line " + std::to_string(location.firstLine) :
            "physical lines " + std::to_string(location.firstLine) + '-' + std::to_string(location.lastLine);
    return " at record " + std::to_string(location.record) + ", " + lines;
}
} // namespace

CsvSqliteWriteResult WriteCsvTable(sqlite3* database, const std::string& path, const CsvSchema& schema)
{
    CsvSqliteWriteResult result;
    bool savepoint = false;
    std::string stage = "open CSV";
    CsvRecordLocation location{};
    std::string field;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open CSV for reading");
        CsvReader reader(input, path);
        std::vector<std::string> fields;
        stage = "read CSV header";
        const auto hasHeader = reader.ReadRecord(fields);
        location = reader.Location();
        if (hasHeader != !schema.columns.empty() || fields.size() != schema.columns.size())
            throw std::runtime_error("CSV header changed after analysis");
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (fields[index] != schema.columns[index].sourceName)
                throw std::runtime_error(
                    "CSV header changed after analysis at field '" + schema.columns[index].sourceName + "'");
        }
        if (schema.rowCount == 0) {
            stage = "check empty CSV";
            if (reader.ReadRecord(fields)) {
                location = reader.Location();
                throw std::runtime_error("CSV record count changed after analysis: expected 0, found at least 1");
            }
            return result;
        }
        location = {};
        stage = "start CSV table transaction";
        if (!database) {
            result.databaseUsable = false;
            throw std::runtime_error("SQLite connection is not open");
        }
        const auto& api = sqlite::GetApi();
        const auto status = api.exec(database, SAVEPOINT);
        if (status != SQLITE_OK) {
            result.databaseUsable = false;
            Check(database, status);
        }
        savepoint = true;
        stage = "write FieldUnitMap";
        WriteUnits(database, schema);
        stage = "create CSV table";
        Check(database, api.exec(database, CreateSql(schema).c_str()));
        stage = "prepare CSV row insertion";
        Statement insert(database, InsertSql(schema));
        std::uint64_t rows = 0;
        for (;;) {
            stage = "read CSV record";
            field.clear();
            location = {};
            if (!reader.ReadRecord(fields))
                break;
            location = reader.Location();
            if (fields.size() != schema.columns.size())
                throw std::runtime_error(
                    "expected " + std::to_string(schema.columns.size()) + " fields, found " +
                    std::to_string(fields.size()));
            stage = "bind CSV field";
            for (std::size_t index = 0; index < fields.size(); ++index) {
                field = schema.columns[index].sourceName;
                BindValue(
                    database, insert, static_cast<int>(index) + sqlite::kFirstBindParameter, schema.columns[index],
                    fields[index]);
            }
            field.clear();
            stage = "insert CSV record";
            insert.Complete();
            stage = "reset CSV insertion";
            insert.Reset();
            ++rows;
        }
        stage = "finish CSV table";
        if (rows != schema.rowCount)
            throw std::runtime_error(
                "CSV record count changed after analysis: expected " + std::to_string(schema.rowCount) + ", found " +
                std::to_string(rows));
        insert.Finish();
        Check(database, api.exec(database, RELEASE));
        savepoint = false;
        result.written = true;
    } catch (const std::exception& error) {
        result.error = "Cannot import CSV '" + path + "' into SQLite table '" + schema.tableName + "' during " + stage +
                       RecordContext(location) + (field.empty() ? "" : ", field '" + field + "'") + ": " + error.what();
        if (savepoint) {
            const auto& api = sqlite::GetApi();
            const auto rollbackStatus = api.exec(database, ROLLBACK);
            const std::string rollbackError = rollbackStatus == SQLITE_OK ? "" : api.errmsg(database);
            const auto releaseStatus = rollbackStatus == SQLITE_OK ? api.exec(database, RELEASE) : rollbackStatus;
            if (releaseStatus != SQLITE_OK) {
                result.databaseUsable = false;
                result.error += "; rollback of the CSV table failed: " +
                                (rollbackError.empty() ? api.errmsg(database) : rollbackError);
            }
        }
        result.error += result.databaseUsable ?
                            ". This CSV table and its new unit mappings were not kept." :
                            ". The working database is unusable; discard it and preserve the original database.";
    }
    return result;
}
} // namespace npucompute::cli
