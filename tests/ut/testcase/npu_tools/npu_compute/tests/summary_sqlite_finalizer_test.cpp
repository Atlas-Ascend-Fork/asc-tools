/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/summary_sqlite_finalizer.h"
#include "report/rep_decoder.h"
#include "report/rep_directory_packer.h"
#include "shared/sqlite_runtime_loader.h"
#include "sqlite_test_api.h"

#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = boost::filesystem;
namespace sqlite = npucompute::sqlite;
using npucompute::cli::FinalizeSummarySqlite;
using npucompute::cli::SummarySqliteFinalizationResult;
using Rows = std::vector<std::vector<std::string>>;

std::string Bytes(const fs::path& path)
{
    std::ifstream input(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class NpuComputeSummarySqliteFinalizer : public ::testing::Test {
protected:
    void SetUp() override
    {
        directory_ = fs::temp_directory_path() / fs::unique_path("summary-sqlite-%%%%-%%%%-%%%%");
        ASSERT_TRUE(fs::create_directory(directory_));
    }
    void TearDown() override
    {
        boost::system::error_code ignored;
        fs::remove_all(directory_, ignored);
    }
    fs::path File(const fs::path& name, std::string_view contents)
    {
        const auto path = directory_ / name;
        fs::create_directories(path.parent_path());
        std::ofstream output(path.string(), std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        EXPECT_FALSE(output.fail());
        return path;
    }
    void Seed(const fs::path& path, const char* sql)
    {
        fs::create_directories(path.parent_path());
        const auto& api = sqlite::GetApi();
        sqlite3* db = nullptr;
        ASSERT_EQ(
            api.openV2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), SQLITE_OK);
        EXPECT_EQ(api.exec(db, sql), SQLITE_OK) << api.errmsg(db);
        EXPECT_EQ(api.close(db), SQLITE_OK);
    }
    fs::path SeedPc(const fs::path& subdirectory = {})
    {
        const auto path = directory_ / subdirectory / "summary.db";
        Seed(
            path, "CREATE TABLE PcSampling(address INTEGER PRIMARY KEY,ibuf_empty INTEGER,nop_cycles INTEGER,"
                  "scoreboard_not_ready INTEGER,register_bank_conflict INTEGER,resource_conflict INTEGER,"
                  "warp_level_sync INTEGER,divergence_stack_spill INTEGER,others INTEGER);"
                  "INSERT INTO PcSampling VALUES(256,1,2,3,4,5,6,7,8)");
        return path;
    }
    Rows Query(const fs::path& path, const char* sql)
    {
        const auto& api = sqlite::GetApi();
        sqlite3* db = nullptr;
        if (api.openV2(path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (db) {
                api.closeV2(db);
            }
            throw std::runtime_error("cannot query test database: " + path.string());
        }
        sqlite3_stmt* statement = nullptr;
        if (api.prepareV2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
            const std::string error = api.errmsg(db);
            api.closeV2(db);
            throw std::runtime_error(error);
        }
        Rows rows;
        int status;
        while ((status = api.step(statement)) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int index = 0; index < api.columnCount(statement); ++index) {
                if (api.columnType(statement, index) == SQLITE_NULL) {
                    row.emplace_back("<NULL>");
                } else {
                    row.emplace_back(
                        reinterpret_cast<const char*>(api.columnText(statement, index)),
                        api.columnBytes(statement, index));
                }
            }
            rows.push_back(std::move(row));
        }
        EXPECT_EQ(status, SQLITE_DONE);
        EXPECT_EQ(api.finalize(statement), SQLITE_OK);
        EXPECT_EQ(api.close(db), SQLITE_OK);
        return rows;
    }
    void ExpectNoTemporaryFiles()
    {
        for (fs::recursive_directory_iterator it(directory_), end; it != end; ++it) {
            const auto name = it->path().filename().string();
            EXPECT_EQ(name.find(".tmp."), std::string::npos) << it->path();
            EXPECT_EQ(name.find(".db-journal"), std::string::npos) << it->path();
            EXPECT_EQ(name.find(".db-wal"), std::string::npos) << it->path();
            EXPECT_EQ(name.find(".db-shm"), std::string::npos) << it->path();
        }
    }
    void ExpectIssue(const SummarySqliteFinalizationResult& result, const std::vector<std::string>& details)
    {
        std::string message;
        for (const auto& issue : result.issues) {
            message += issue + '\n';
        }
        for (const auto& detail : details) {
            EXPECT_NE(message.find(detail), std::string::npos) << message;
        }
    }
    fs::path directory_;
};

TEST_F(NpuComputeSummarySqliteFinalizer, NoCsvDoesNotCreateDatabase)
{
    File("objdump.txt", "100: 00 00 00 00 nop\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_FALSE(fs::exists(directory_ / "summary.db"));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, EmptyAndHeaderOnlyCsvDoNotCreateDatabase)
{
    File("Memory.csv", "");
    File("MemoryUB.csv", "time(us)\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_FALSE(fs::exists(directory_ / "summary.db"));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, NoCsvLeavesPcSamplingByteForByteUnchanged)
{
    const auto database = SeedPc();
    const auto before = Bytes(database);
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(Bytes(database), before);
    EXPECT_EQ(Query(database, "SELECT address,others FROM PcSampling"), (Rows{{"256", "8"}}));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, WritesMultipleTablesAndUnitsIntoOneDatabase)
{
    const auto memory = File("Memory.csv", "block_id,time(us)\n2,4.220000\n1,NA\n2,4.220000\n");
    File("PipeUtilization.csv", "block_id,cycles\n0,9007199254740993\n");
    const auto csvBefore = Bytes(memory);
    const auto result = FinalizeSummarySqlite(directory_);
    ASSERT_EQ(result.databasesUpdated, 1U);
    ASSERT_TRUE(result.issues.empty());
    const auto database = directory_ / "summary.db";
    EXPECT_EQ(
        Query(database, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"),
        (Rows{{"FieldUnitMap"}, {"Memory"}, {"PipeUtilization"}}));
    EXPECT_EQ(
        Query(database, "SELECT block_id,time FROM Memory ORDER BY rowid"),
        (Rows{{"2", "4.22"}, {"1", "<NULL>"}, {"2", "4.22"}}));
    EXPECT_EQ(Query(database, "SELECT cycles FROM PipeUtilization"), (Rows{{"9007199254740993"}}));
    EXPECT_EQ(Query(database, "SELECT unit FROM FieldUnitMap WHERE field_name='time'"), (Rows{{"us"}}));
    EXPECT_EQ(Query(database, "PRAGMA journal_mode"), (Rows{{"delete"}}));
    EXPECT_EQ(Bytes(memory), csvBefore);
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, AppendsToCopyAndPreservesPcSamplingData)
{
    const auto database = SeedPc();
    File("Memory.csv", "time(us)\n4.220000\n");
    const auto result = FinalizeSummarySqlite(directory_);
    ASSERT_EQ(result.databasesUpdated, 1U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(Query(database, "SELECT * FROM PcSampling"), (Rows{{"256", "1", "2", "3", "4", "5", "6", "7", "8"}}));
    EXPECT_EQ(Query(database, "SELECT time FROM Memory"), (Rows{{"4.22"}}));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, ParseFailureDoesNotBlockOtherCsv)
{
    File("Memory.csv", "block_id\n7\n");
    File("PipeUtilization.csv", "a,b\n1\n");
    File("MemoryUB.csv", "value\n8\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 1U);
    ExpectIssue(result, {"PipeUtilization.csv", "PipeUtilization", "record 2", "expected 2 fields", "found 1"});
    EXPECT_EQ(
        Query(directory_ / "summary.db", "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"),
        (Rows{{"FieldUnitMap"}, {"Memory"}, {"MemoryUB"}}));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, UnitConflictKeepsSuccessfulTablesAndPcSampling)
{
    const auto database = SeedPc();
    File("Memory.csv", "time(us)\n1\n");
    File("PipeUtilization.csv", "new_metric(cycle),time(ms)\n2,3\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 1U);
    ExpectIssue(result, {"PipeUtilization.csv", "unit conflict", "us", "ms"});
    EXPECT_EQ(Query(database, "SELECT address,others FROM PcSampling"), (Rows{{"256", "8"}}));
    EXPECT_EQ(Query(database, "SELECT time FROM Memory"), (Rows{{"1"}}));
    EXPECT_EQ(Query(database, "SELECT field_name FROM FieldUnitMap"), (Rows{{"time"}}));
    EXPECT_TRUE(Query(database, "SELECT name FROM sqlite_master WHERE name='PipeUtilization'").empty());
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, AllInvalidCsvDoesNotPublishAnEmptyDatabase)
{
    File("Memory.csv", "a,b\n1\n");
    File("PcSampling.csv", "address\n1\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    EXPECT_EQ(result.issues.size(), 2U);
    EXPECT_FALSE(fs::exists(directory_ / "summary.db"));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, ExistingTableConflictDoesNotReplaceOrAppendIt)
{
    const auto database = directory_ / "summary.db";
    Seed(database, "CREATE TABLE Memory(value INTEGER); INSERT INTO Memory VALUES(7)");
    const auto before = Bytes(database);
    File("Memory.csv", "new_field(us)\n2\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"Memory.csv", "Memory", "already exists"});
    EXPECT_EQ(Bytes(database), before);
    EXPECT_EQ(Query(database, "SELECT value FROM Memory"), (Rows{{"7"}}));
    EXPECT_TRUE(Query(database, "SELECT name FROM sqlite_master WHERE name='FieldUnitMap'").empty());
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, NestedResultsNeverMergeSameNamedTables)
{
    File("Memory.csv", "value\n1\n");
    File("collection-p1-0001/Memory.csv", "value\n2\n");
    File("collection-p2-0002/Memory.csv", "value\n3\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 3U);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(Query(directory_ / "summary.db", "SELECT value FROM Memory"), (Rows{{"1"}}));
    EXPECT_EQ(Query(directory_ / "collection-p1-0001/summary.db", "SELECT value FROM Memory"), (Rows{{"2"}}));
    EXPECT_EQ(Query(directory_ / "collection-p2-0002/summary.db", "SELECT value FROM Memory"), (Rows{{"3"}}));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, InvalidExistingDatabaseIsNotOverwritten)
{
    const auto database = File("summary.db", "not an SQLite database");
    const auto before = Bytes(database);
    File("Memory.csv", "value\n1\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"summary.db", "database", "original"});
    EXPECT_EQ(Bytes(database), before);
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, RejectsDatabaseSymlinkAndDoesNotTouchItsTarget)
{
    const auto outside = directory_ / "outside.db";
    Seed(outside, "CREATE TABLE Existing(value INTEGER); INSERT INTO Existing VALUES(9)");
    const auto before = Bytes(outside);
    fs::create_directory(directory_ / "collection-p1");
    fs::create_symlink(outside, directory_ / "collection-p1/summary.db");
    File("collection-p1/Memory.csv", "value\n1\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"summary.db", "regular file"});
    EXPECT_EQ(Bytes(outside), before);
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(directory_ / "collection-p1/summary.db")));
}

TEST_F(NpuComputeSummarySqliteFinalizer, ExistingDatabaseSidecarsPreventAnUnsafeCopy)
{
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        const auto group = fs::path(std::string("collection") + suffix);
        const auto database = SeedPc(group);
        const auto before = Bytes(database);
        const auto sidecar = File(group / (std::string("summary.db") + suffix), "unfinished SQLite data");
        File(group / "Memory.csv", "value\n1\n");
        const auto result = FinalizeSummarySqlite(directory_ / group);
        EXPECT_EQ(result.databasesUpdated, 0U);
        ExpectIssue(result, {sidecar.string(), "sidecar", "original"});
        EXPECT_EQ(Bytes(database), before);
        EXPECT_EQ(Bytes(sidecar), "unfinished SQLite data");
        fs::remove(sidecar);
    }
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, OrphanSidecarPreventsPublishingANewMainDatabase)
{
    const auto sidecar = File("summary.db-wal", "unfinished SQLite data");
    File("Memory.csv", "value\n1\n");
    const auto result = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {sidecar.string(), "sidecar"});
    EXPECT_FALSE(fs::exists(directory_ / "summary.db"));
    EXPECT_EQ(Bytes(sidecar), "unfinished SQLite data");
    fs::remove(sidecar);
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, SkipsInternalAndTemporaryDirectoriesAndDoesNotFollowSymlinks)
{
    File(".biu-staging/Memory.csv", "value\n1\n");
    File(".summary.tmp.leftover/Memory.csv", "value\n2\n");
    File("outside/Memory.csv", "value\n3\n");
    fs::create_directory(directory_ / "root");
    fs::create_directory_symlink(directory_ / "outside", directory_ / "root/linked");
    fs::create_symlink(directory_ / "outside/Memory.csv", directory_ / "root/Memory.csv");
    const auto result = FinalizeSummarySqlite(directory_ / "root");
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"symbolic link"});
    EXPECT_FALSE(fs::exists(directory_ / "outside/summary.db"));
    const auto all = FinalizeSummarySqlite(directory_);
    EXPECT_EQ(all.databasesUpdated, 1U); // Only outside is newly processed.
    EXPECT_FALSE(fs::exists(directory_ / ".biu-staging/summary.db"));
    EXPECT_FALSE(fs::exists(directory_ / ".summary.tmp.leftover/summary.db"));
}

TEST_F(NpuComputeSummarySqliteFinalizer, MissingRootReturnsALocatableIssue)
{
    const auto result = FinalizeSummarySqlite(directory_ / "missing");
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"missing", "directory"});
}

// Hooks are resolved only by tests; production requires no authorizer/trace APIs.
struct DatabaseFault {
    enum class Mode { DenyCommit, DenyMemoryTable, CloseBusy, MoveWorkingFile, DenyTableRollback };
    explicit DatabaseFault(Mode selected, std::string directoryFilter = {})
        : mode(selected), filter(std::move(directoryFilter))
    {
        active = this;
        EXPECT_EQ(npucompute::test::SqliteAutoExtension(reinterpret_cast<void (*)()>(&Initialize)), SQLITE_OK);
    }
    ~DatabaseFault() { Stop(); }
    void Stop()
    {
        if (active != this) {
            return;
        }
        npucompute::test::SqliteCancelAutoExtension(reinterpret_cast<void (*)()>(&Initialize));
        if (leaked) {
            EXPECT_EQ(sqlite::GetApi().finalize(leaked), SQLITE_OK);
            leaked = nullptr;
        }
        active = nullptr;
    }
    static int Initialize(sqlite3* db, char**, const sqlite3_api_routines*)
    {
        if (!active) {
            return SQLITE_OK;
        }
        const auto filename =
            npucompute::test::SqliteSymbol<const char* (*)(sqlite3*, const char*)>("sqlite3_db_filename");
        const auto* name = filename(db, "main");
        if (!name || (!active->filter.empty() && std::string(name).find(active->filter) == std::string::npos)) {
            return SQLITE_OK;
        }
        active->path = name;
        if (active->mode == Mode::CloseBusy) {
            return sqlite::GetApi().prepareV2(db, "SELECT 1", -1, &active->leaked, nullptr);
        }
        if (active->mode == Mode::MoveWorkingFile) {
            return npucompute::test::SqliteTraceV2(db, npucompute::test::SQLITE_TRACE_PROFILE, Trace, active);
        }
        return npucompute::test::SqliteSetAuthorizer(db, Authorize, active);
    }
    static int Authorize(void* context, int operation, const char* first, const char*, const char*, const char*)
    {
        auto& fault = *static_cast<DatabaseFault*>(context);
        const std::string_view value = first ? first : "";
        if (fault.mode == Mode::DenyCommit && operation == npucompute::test::SQLITE_TRANSACTION && value == "COMMIT") {
            return SQLITE_DENY;
        }
        if ((fault.mode == Mode::DenyMemoryTable || fault.mode == Mode::DenyTableRollback) &&
            operation == npucompute::test::SQLITE_CREATE_TABLE && value == "Memory") {
            return SQLITE_DENY;
        }
        constexpr int sqliteSavepoint = 32;
        if (fault.mode == Mode::DenyTableRollback && operation == sqliteSavepoint && value == "ROLLBACK") {
            return SQLITE_DENY;
        }
        return SQLITE_OK;
    }
    static int Trace(unsigned, void* context, void* statement, void*)
    {
        auto& fault = *static_cast<DatabaseFault*>(context);
        if (std::string_view(npucompute::test::SqliteSql(static_cast<sqlite3_stmt*>(statement))) == "COMMIT") {
            boost::system::error_code error;
            fs::rename(fault.path, fault.path.parent_path() / "moved.db", error);
            fault.moved = !error;
        }
        return 0;
    }
    Mode mode;
    std::string filter;
    fs::path path;
    sqlite3_stmt* leaked = nullptr;
    bool moved = false;
    static DatabaseFault* active;
};
DatabaseFault* DatabaseFault::active = nullptr;

TEST_F(NpuComputeSummarySqliteFinalizer, DatabaseInsertFailureKeepsOtherSuccessfulTables)
{
    File("Memory.csv", "memory_metric(us)\n1\n");
    File("PipeUtilization.csv", "cycles\n2\n");
    DatabaseFault fault(DatabaseFault::Mode::DenyMemoryTable);
    const auto result = FinalizeSummarySqlite(directory_);
    fault.Stop();
    EXPECT_EQ(result.databasesUpdated, 1U);
    ExpectIssue(result, {"Memory.csv", "create CSV table", "not authorized"});
    EXPECT_EQ(Query(directory_ / "summary.db", "SELECT cycles FROM PipeUtilization"), (Rows{{"2"}}));
    EXPECT_EQ(Query(directory_ / "summary.db", "SELECT field_name FROM FieldUnitMap"), (Rows{{"cycles"}}));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, OnlyFailedTableLeavesNoDatabaseOrOrphanUnitMappings)
{
    File("Memory.csv", "time(us)\n1\n");
    DatabaseFault fault(DatabaseFault::Mode::DenyMemoryTable);
    const auto result = FinalizeSummarySqlite(directory_);
    fault.Stop();
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"Memory.csv", "not authorized"});
    EXPECT_FALSE(fs::exists(directory_ / "summary.db"));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, CommitFailurePreservesOriginalDatabaseBytes)
{
    const auto database = SeedPc();
    const auto before = Bytes(database);
    File("Memory.csv", "value\n1\n");
    DatabaseFault fault(DatabaseFault::Mode::DenyCommit);
    const auto result = FinalizeSummarySqlite(directory_);
    fault.Stop();
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"summary.db", "commit", "not authorized", "original"});
    EXPECT_EQ(Bytes(database), before);
    EXPECT_EQ(Query(database, "SELECT address,others FROM PcSampling"), (Rows{{"256", "8"}}));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, CloseFailurePreservesOriginalAndDoesNotPublishWorkingCopy)
{
    const auto database = SeedPc();
    const auto before = Bytes(database);
    File("Memory.csv", "value\n1\n");
    DatabaseFault fault(DatabaseFault::Mode::CloseBusy);
    const auto result = FinalizeSummarySqlite(directory_);
    fault.Stop();
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"summary.db", "close", "original"});
    EXPECT_EQ(Bytes(database), before);
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, PublishFailurePreservesOriginalAndCleansWorkingFiles)
{
    const auto database = SeedPc();
    const auto before = Bytes(database);
    File("Memory.csv", "value\n1\n");
    DatabaseFault fault(DatabaseFault::Mode::MoveWorkingFile);
    const auto result = FinalizeSummarySqlite(directory_);
    fault.Stop();
    EXPECT_TRUE(fault.moved);
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"summary.db", "publish", "original"});
    EXPECT_EQ(Bytes(database), before);
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, RollbackFailureDiscardsTheWholeWorkingCopy)
{
    const auto database = SeedPc();
    const auto before = Bytes(database);
    File("ArithmeticUtilization.csv", "earlier\n1\n");
    File("Memory.csv", "later\n2\n");
    DatabaseFault fault(DatabaseFault::Mode::DenyTableRollback);
    const auto result = FinalizeSummarySqlite(directory_);
    fault.Stop();
    EXPECT_EQ(result.databasesUpdated, 0U);
    ExpectIssue(result, {"Memory.csv", "rollback", "discard"});
    EXPECT_EQ(Bytes(database), before);
    EXPECT_TRUE(Query(database, "SELECT name FROM sqlite_master WHERE name='ArithmeticUtilization'").empty());
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, AFailedResultDirectoryDoesNotBlockOtherDirectories)
{
    const auto database = SeedPc("collection-p1-bad");
    const auto before = Bytes(database);
    File("collection-p1-bad/Memory.csv", "value\n1\n");
    File("collection-p2-good/Memory.csv", "value\n2\n");
    DatabaseFault fault(DatabaseFault::Mode::DenyCommit, "collection-p1-bad");
    const auto result = FinalizeSummarySqlite(directory_);
    fault.Stop();
    EXPECT_EQ(result.databasesUpdated, 1U);
    ExpectIssue(result, {"collection-p1-bad", "commit"});
    EXPECT_EQ(Bytes(database), before);
    EXPECT_EQ(Query(directory_ / "collection-p2-good/summary.db", "SELECT value FROM Memory"), (Rows{{"2"}}));
    ExpectNoTemporaryFiles();
}

TEST_F(NpuComputeSummarySqliteFinalizer, GeneratedDatabasePacksWithoutTemporaryArtifacts)
{
    File("Memory.csv", "value\n1\n");
    ASSERT_EQ(FinalizeSummarySqlite(directory_).databasesUpdated, 1U);
    std::vector<std::uint8_t> encoded;
    std::string error;
    std::vector<std::string> issues;
    ASSERT_TRUE(npucompute::cli::PackDirectoryToRep(directory_, &encoded, &error, &issues)) << error;
    EXPECT_TRUE(issues.empty());
    npucompute::cli::DecodedRep report;
    ASSERT_TRUE(npucompute::cli::DecodeRep(encoded, &report, &error)) << error;
    ASSERT_EQ(report.entries.size(), 2U);
    EXPECT_EQ(report.entries[0].file_name, "Memory.csv");
    EXPECT_EQ(report.entries[1].file_name, "summary.db");
    EXPECT_EQ(report.entries[1].file_type, npucompute::cli::NpuRepFileType::Sqlite);
    ExpectNoTemporaryFiles();
}
} // namespace
