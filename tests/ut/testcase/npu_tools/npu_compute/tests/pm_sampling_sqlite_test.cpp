/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pm_sampling/pm_sampling_sqlite_writer.h"
#include "sqlite_test_api.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>

namespace sqlite = npucompute::sqlite;

#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
            return 1;                                            \
        }                                                        \
    } while (false)
using npucompute::PmSamplingResult;
using npucompute::PmSamplingStateCounts;
using npucompute::WritePmSamplingSqlite;
namespace fs = std::filesystem;
namespace {
struct Directory {
    fs::path path;
    ~Directory()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
bool rollbackObserved = false;
bool rollbackCompleted = false;
int Authorize(void*, int action, const char* first, const char*, const char*, const char*)
{
    if (action == npucompute::test::SQLITE_TRANSACTION && first) {
        if (std::string(first) == "COMMIT")
            return SQLITE_DENY;
        if (std::string(first) == "ROLLBACK")
            rollbackObserved = true;
    }
    return SQLITE_OK;
}
int Trace(unsigned event, void*, void* value, void*)
{
    if (event == npucompute::test::SQLITE_TRACE_PROFILE) {
        const auto* sql = npucompute::test::SqliteSql(static_cast<sqlite3_stmt*>(value));
        if (sql && std::string(sql) == "ROLLBACK")
            rollbackCompleted = npucompute::test::SqliteGetAutocommit(
                                    npucompute::test::SqliteDbHandle(static_cast<sqlite3_stmt*>(value))) != 0;
    }
    return 0;
}
int DenyCommit(sqlite3* db, char**, const sqlite3_api_routines*)
{
    const auto status = npucompute::test::SqliteTraceV2(db, npucompute::test::SQLITE_TRACE_PROFILE, Trace, nullptr);
    return status == SQLITE_OK ? npucompute::test::SqliteSetAuthorizer(db, Authorize, nullptr) : status;
}
int CheckDatabase(const fs::path& path, int expectedTables = 1)
{
    sqlite3* db = nullptr;
    CHECK(sqlite::GetApi().openV2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    CHECK(
        sqlite::GetApi().prepareV2(db, "SELECT * FROM PcSampling ORDER BY address", -1, &statement, nullptr) ==
        SQLITE_OK);
    CHECK(sqlite::GetApi().columnCount(statement) == 9);
    CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW);
    CHECK(sqlite::GetApi().columnType(statement, 0) == SQLITE_INTEGER);
    CHECK(sqlite::GetApi().columnInt64(statement, 0) == 0x100);
    const PmSamplingStateCounts expected{45, 47, 127, 0, 21, 0, 0, 0};
    for (size_t state = 0; state < expected.size(); ++state) {
        CHECK(sqlite::GetApi().columnType(statement, state + 1) == SQLITE_INTEGER);
        CHECK(sqlite::GetApi().columnInt64(statement, state + 1) == static_cast<sqlite3_int64>(expected[state]));
    }
    CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW);
    CHECK(sqlite::GetApi().columnInt64(statement, 0) == 0x108);
    for (int column = 1; column < 9; ++column)
        CHECK(sqlite::GetApi().columnInt64(statement, column) == 0);
    CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW);
    CHECK(sqlite::GetApi().columnInt64(statement, 0) == std::numeric_limits<sqlite3_int64>::max());
    CHECK(sqlite::GetApi().columnInt64(statement, 8) == std::numeric_limits<sqlite3_int64>::max());
    CHECK(sqlite::GetApi().step(statement) == SQLITE_DONE);
    CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
    CHECK(
        sqlite::GetApi().prepareV2(
            db, "SELECT name FROM sqlite_master WHERE type='table' AND name='PcSampling'", -1, &statement, nullptr) ==
        SQLITE_OK);
    CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite::GetApi().columnText(statement, 0))) == "PcSampling");
    CHECK(sqlite::GetApi().step(statement) == SQLITE_DONE);
    CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
    CHECK(
        sqlite::GetApi().prepareV2(
            db, "SELECT count(*) FROM sqlite_master WHERE type='table'", -1, &statement, nullptr) == SQLITE_OK);
    CHECK(
        sqlite::GetApi().step(statement) == SQLITE_ROW && sqlite::GetApi().columnInt64(statement, 0) == expectedTables);
    CHECK(sqlite::GetApi().step(statement) == SQLITE_DONE);
    CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
    CHECK(sqlite::GetApi().prepareV2(db, "PRAGMA table_info(PcSampling)", -1, &statement, nullptr) == SQLITE_OK);
    const std::array<const char*, 9> names = {
        "address",
        "ibuf_empty",
        "nop_cycles",
        "scoreboard_not_ready",
        "register_bank_conflict",
        "resource_conflict",
        "warp_level_sync",
        "divergence_stack_spill",
        "others"};
    for (int column = 0; column < 9; ++column) {
        CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW);
        CHECK(std::string(reinterpret_cast<const char*>(sqlite::GetApi().columnText(statement, 1))) == names[column]);
        CHECK(std::string(reinterpret_cast<const char*>(sqlite::GetApi().columnText(statement, 2))) == "INTEGER");
        CHECK(sqlite::GetApi().columnInt64(statement, 3) == 1);
        CHECK(sqlite::GetApi().columnInt64(statement, 5) == (column == 0 ? 1 : 0));
    }
    CHECK(sqlite::GetApi().step(statement) == SQLITE_DONE);
    CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
    CHECK(sqlite::GetApi().prepareV2(db, "PRAGMA quick_check", -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite::GetApi().columnText(statement, 0))) == "ok");
    CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
    CHECK(sqlite::GetApi().prepareV2(db, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW && sqlite::GetApi().columnInt64(statement, 0) == 1);
    CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
    CHECK(sqlite::GetApi().prepareV2(db, "PRAGMA journal_mode", -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite::GetApi().step(statement) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite::GetApi().columnText(statement, 0))) == "delete");
    CHECK(sqlite::GetApi().finalize(statement) == SQLITE_OK);
    CHECK(sqlite::GetApi().close(db) == SQLITE_OK);
    return 0;
}
} // namespace
int main()
{
    char pattern[] = "/tmp/pm-sampling-sqlite-XXXXXX";
    const char* created = ::mkdtemp(pattern);
    CHECK(created != nullptr);
    Directory directory{created};
    const auto path = directory.path / "summary.db";
    PmSamplingResult result;
    result.rows[0x100] = {45, 47, 127, 0, 21, 0, 0, 0};
    result.rows[0x108] = {};
    result.rows[std::numeric_limits<sqlite3_int64>::max()][7] = std::numeric_limits<sqlite3_int64>::max();
    std::string error = "old error";
    CHECK(WritePmSamplingSqlite(path.string(), result, error));
    CHECK(error.empty());
    CHECK(CheckDatabase(path) == 0);
    // Copy just the closed file to a different directory, with no journal/WAL/SHM.
    const auto copyDirectory = directory.path / "copied";
    fs::create_directory(copyDirectory);
    const auto copy = copyDirectory / "summary.db";
    fs::copy_file(path, copy);
    CHECK(CheckDatabase(copy) == 0);
    CHECK(std::distance(fs::directory_iterator(copyDirectory), fs::directory_iterator{}) == 1);
    auto overflow = result;
    overflow.rows[0x100][4] = uint64_t(std::numeric_limits<sqlite3_int64>::max()) + 1;
    CHECK(!WritePmSamplingSqlite((directory.path / "overflow.db").string(), overflow, error));
    CHECK(error.find("INTEGER range") != std::string::npos);
    CHECK(!fs::exists(directory.path / "overflow.db"));
    auto addressOverflow = result;
    addressOverflow.rows[std::uint64_t(std::numeric_limits<sqlite3_int64>::max()) + 1] = {};
    CHECK(!WritePmSamplingSqlite((directory.path / "address-overflow.db").string(), addressOverflow, error));
    CHECK(error.find("address exceeds SQLite INTEGER range") != std::string::npos);
    CHECK(!fs::exists(directory.path / "address-overflow.db"));
    CHECK(!WritePmSamplingSqlite(path.string(), result, error));
    CHECK(CheckDatabase(path) == 0); // Recreating an existing table fails without changing its data.
    CHECK(!WritePmSamplingSqlite((directory.path / "missing" / "file.db").string(), result, error));
    // Real SQLite error after all INSERTs: COMMIT denied by an authorizer. The
    // transaction must roll back. The staging file remains for owner cleanup.
    CHECK(npucompute::test::SqliteAutoExtension(reinterpret_cast<void (*)(void)>(&DenyCommit)) == SQLITE_OK);
    const auto failed = directory.path / "failed.db";
    CHECK(!WritePmSamplingSqlite(failed.string(), result, error));
    npucompute::test::SqliteCancelAutoExtension(reinterpret_cast<void (*)(void)>(&DenyCommit));
    CHECK(rollbackObserved && rollbackCompleted && fs::exists(failed) && !error.empty());
    sqlite3* failedDb = nullptr;
    CHECK(sqlite::GetApi().openV2(failed.c_str(), &failedDb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* failedTables = nullptr;
    CHECK(
        sqlite::GetApi().prepareV2(
            failedDb, "SELECT count(*) FROM sqlite_master WHERE type='table'", -1, &failedTables, nullptr) ==
        SQLITE_OK);
    CHECK(sqlite::GetApi().step(failedTables) == SQLITE_ROW && sqlite::GetApi().columnInt64(failedTables, 0) == 0);
    CHECK(sqlite::GetApi().finalize(failedTables) == SQLITE_OK);
    CHECK(sqlite::GetApi().close(failedDb) == SQLITE_OK);
    CHECK(fs::remove(failed)); // Simulate collection owner's failed-staging cleanup.
    for (const auto& item : fs::directory_iterator(directory.path)) {
        CHECK(item.path().filename() == "summary.db" || item.path().filename() == "copied");
    }
    // Existing unrelated tables must not be rejected by a global "one table" check.
    const auto shared = directory.path / "shared.db";
    sqlite3* sharedDb = nullptr;
    CHECK(
        sqlite::GetApi().openV2(shared.c_str(), &sharedDb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
        SQLITE_OK);
    CHECK(
        sqlite::GetApi().exec(
            sharedDb, "CREATE TABLE ExistingCollection(Value INTEGER); INSERT INTO ExistingCollection VALUES(7)") ==
        SQLITE_OK);
    CHECK(sqlite::GetApi().close(sharedDb) == SQLITE_OK);
    CHECK(WritePmSamplingSqlite(shared.string(), result, error));
    CHECK(CheckDatabase(shared, 2) == 0);
    CHECK(sqlite::GetApi().openV2(shared.c_str(), &sharedDb, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* existing = nullptr;
    CHECK(
        sqlite::GetApi().prepareV2(sharedDb, "SELECT Value FROM ExistingCollection", -1, &existing, nullptr) ==
        SQLITE_OK);
    CHECK(sqlite::GetApi().step(existing) == SQLITE_ROW && sqlite::GetApi().columnInt64(existing, 0) == 7);
    CHECK(sqlite::GetApi().finalize(existing) == SQLITE_OK);
    CHECK(sqlite::GetApi().close(sharedDb) == SQLITE_OK);
    CHECK(fs::remove(shared));
    CHECK(!WritePmSamplingSqlite("", result, error));
    CHECK(!WritePmSamplingSqlite(std::string("bad\0path", 8), result, error));
    std::puts("PASS: one PcSampling table, exact counts, INT64 limits, rollback and standalone SQLite file");
    return 0;
}
