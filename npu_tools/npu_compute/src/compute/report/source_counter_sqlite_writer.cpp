/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "source_counter_sqlite_writer.h"
#include "instruction_sqlite.h"

#include <algorithm>
#include <limits>
#include <map>

namespace npucompute {
namespace {
using instruction_sqlite::Database;
using instruction_sqlite::Prepare;
constexpr int kAddressColumn = 1;
constexpr int kAverageCacheLinesColumn = 2;
constexpr int kAverageUtilizationColumn = 3;
constexpr int kBankConflictColumn = 4;

std::map<uint64_t, aclptiSourceCounterRow> MergeInstructions(const aclptiSourceCounterData& data)
{
    std::map<uint64_t, aclptiSourceCounterRow> rows;
    for (const auto& [address, sample] : data.rows) {
        if (sample.memorySampleCount == 0 && !sample.ubBankConflict) {
            continue;
        }
        auto& row = rows[address];
        if (sample.memorySampleCount != 0) {
            row.memorySampleCount += sample.memorySampleCount;
            row.cacheLinesTotal += sample.cacheLinesTotal;
            row.cacheLineUtilizationTotal += sample.cacheLineUtilizationTotal;
        }
        if (sample.ubBankConflict) {
            row.ubBankConflict =
                row.ubBankConflict ? std::max(*row.ubBankConflict, *sample.ubBankConflict) : sample.ubBankConflict;
        }
    }
    return rows;
}

void WriteRows(Database& db, const std::map<uint64_t, aclptiSourceCounterRow>& rows)
{
    auto statement = Prepare(db, "INSERT INTO SourceCounter VALUES(?, ?, ?, ?)");
    const auto& api = sqlite::GetApi();
    for (const auto& [address, row] : rows) {
        constexpr auto maxInteger = static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max());
        if (address > maxInteger || (row.ubBankConflict && *row.ubBankConflict > maxInteger)) {
            throw std::runtime_error("SourceCounter value exceeds SQLite INTEGER range");
        }
        db.Check(api.bindInt64(statement.get(), kAddressColumn, static_cast<sqlite3_int64>(address)));
        if (row.memorySampleCount != 0) {
            db.Check(
                api.bindDouble(statement.get(), kAverageCacheLinesColumn, row.cacheLinesTotal / row.memorySampleCount));
            db.Check(api.bindDouble(
                statement.get(), kAverageUtilizationColumn, row.cacheLineUtilizationTotal / row.memorySampleCount));
        } else {
            db.Check(api.bindNull(statement.get(), kAverageCacheLinesColumn));
            db.Check(api.bindNull(statement.get(), kAverageUtilizationColumn));
        }
        db.Check(
            row.ubBankConflict ?
                api.bindInt64(statement.get(), kBankConflictColumn, static_cast<sqlite3_int64>(*row.ubBankConflict)) :
                api.bindNull(statement.get(), kBankConflictColumn));
        if (api.step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(api.errmsg(db.handle));
        }
        db.Check(api.reset(statement.get()));
    }
}
} // namespace

bool WriteSourceCounterSqlite(
    const std::string& path, const aclptiSourceCounterData& data, uint64_t& writtenRows, std::string& error)
{
    writtenRows = 0;
    error.clear();
    try {
        const auto rows = MergeInstructions(data);
        Database db;
        instruction_sqlite::Open(db, path);
        db.Exec("BEGIN IMMEDIATE");
        db.transaction = true;
        db.Exec("CREATE TABLE SourceCounter(address INTEGER, avg_cache_lines_num REAL, "
                "avg_cache_line_utilization REAL, ub_bank_conflict INTEGER)");
        db.Exec("CREATE TABLE IF NOT EXISTS FieldUnitMap(field_name TEXT PRIMARY KEY, unit TEXT)");
        db.Exec("INSERT OR IGNORE INTO FieldUnitMap(field_name, unit) VALUES "
                "('address', NULL), ('avg_cache_lines_num', 'count'), "
                "('avg_cache_line_utilization', '%'), ('ub_bank_conflict', 'count')");
        WriteRows(db, rows);
        db.Exec("COMMIT");
        db.transaction = false;
        writtenRows = rows.size();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}
} // namespace npucompute
