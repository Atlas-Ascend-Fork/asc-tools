/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pm_sampling_sqlite_writer.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include "shared/sqlite_runtime_loader.h"
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

namespace npucompute {
namespace {
constexpr int kSchemaVersion = 1;
constexpr int kAddressParameter = sqlite::kFirstBindParameter;
constexpr int kFirstStateParameter = kAddressParameter + 1;
struct Database {
    sqlite3* handle = nullptr;
    bool transaction = false;
    ~Database()
    {
        if (handle) {
            if (transaction) {
                sqlite::GetApi().exec(handle, "ROLLBACK");
            }
            sqlite::GetApi().closeV2(handle);
        }
    }
    void Check(int status) const
    {
        if (status != SQLITE_OK) {
            throw std::runtime_error(sqlite::GetApi().errmsg(handle));
        }
    }
    void Exec(const char* sql) { Check(sqlite::GetApi().exec(handle, sql)); }
};
using Statement = std::unique_ptr<sqlite3_stmt, decltype(sqlite::GetApi().finalize)>;
Statement Prepare(Database& db, const std::string& sql)
{
    sqlite3_stmt* statement = nullptr;
    const auto status =
        sqlite::GetApi().prepareV2(db.handle, sql.c_str(), sqlite::kNullTerminatedSql, &statement, nullptr);
    Statement owned(statement, sqlite::GetApi().finalize);
    db.Check(status);
    return owned;
}
void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::string Text(sqlite3_stmt* statement, int column)
{
    const auto* text = sqlite::GetApi().columnText(statement, column);
    return text ? reinterpret_cast<const char*>(text) : "";
}
void ValidateSamples(const PmSamplingResult& result)
{
    for (const auto& row : result.rows) {
        Require(
            row.first <= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()),
            "PmSampling address exceeds SQLite INTEGER range");
        for (const auto count : row.second) {
            Require(
                count <= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()),
                "PmSampling count exceeds SQLite INTEGER range");
        }
    }
}
std::string CreateSchema(Database& db)
{
    db.Exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
    const std::array<const char*, kPmSamplingStateCount + 1> columns = {
        "address",
        "ibuf_empty",
        "nop_cycles",
        "scoreboard_not_ready",
        "register_bank_conflict",
        "resource_conflict",
        "warp_level_sync",
        "divergence_stack_spill",
        "others"};
    std::string schema = "CREATE TABLE PcSampling(" + std::string(columns[0]) + " INTEGER NOT NULL PRIMARY KEY";
    std::string insert = "INSERT INTO PcSampling VALUES(?";
    for (std::size_t state = 0; state < kPmSamplingStateCount; ++state) {
        schema += "," + std::string(columns[state + 1]) + " INTEGER NOT NULL CHECK(" + columns[state + 1] + ">=0)";
        insert += ",?";
    }
    db.Exec((schema + ")").c_str());
    return insert + ")";
}
void ConfigureDatabase(Database& db)
{
    auto journal = Prepare(db, "PRAGMA journal_mode=DELETE");
    Require(
        sqlite::GetApi().step(journal.get()) == SQLITE_ROW && Text(journal.get(), 0) == "delete" &&
            sqlite::GetApi().step(journal.get()) == SQLITE_DONE,
        "cannot use SQLite DELETE journal");
    db.Exec("PRAGMA synchronous=FULL");
    db.Exec("BEGIN IMMEDIATE");
    db.transaction = true;
}
void WriteRows(Database& db, const std::string& insert, const PmSamplingResult& result)
{
    auto statement = Prepare(db, insert);
    for (const auto& row : result.rows) {
        db.Check(sqlite::GetApi().bindInt64(statement.get(), kAddressParameter, static_cast<sqlite3_int64>(row.first)));
        for (std::size_t state = 0; state < row.second.size(); ++state) {
            db.Check(sqlite::GetApi().bindInt64(
                statement.get(), static_cast<int>(state) + kFirstStateParameter,
                static_cast<sqlite3_int64>(row.second[state])));
        }
        Require(sqlite::GetApi().step(statement.get()) == SQLITE_DONE, sqlite::GetApi().errmsg(db.handle));
        db.Check(sqlite::GetApi().reset(statement.get()));
        db.Check(sqlite::GetApi().clearBindings(statement.get()));
    }
}
} // namespace

bool WritePmSamplingSqlite(const std::string& path, const PmSamplingResult& result, std::string& error)
{
    error.clear();
    try {
        Require(!path.empty() && path.find('\0') == std::string::npos, "invalid PmSampling SQLite path");
        ValidateSamples(result);
        // The caller owns the private staging directory and excludes failed
        // files from packaging. Leave file cleanup to that collection owner.
        Database db;
        db.Check(sqlite::GetApi().openV2(
            path.c_str(), &db.handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr));
        ConfigureDatabase(db);
        const std::string insert = CreateSchema(db);
        WriteRows(db, insert, result);
        db.Exec("COMMIT");
        db.transaction = false;
        db.Check(sqlite::GetApi().close(db.handle));
        db.handle = nullptr;
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}
} // namespace npucompute
