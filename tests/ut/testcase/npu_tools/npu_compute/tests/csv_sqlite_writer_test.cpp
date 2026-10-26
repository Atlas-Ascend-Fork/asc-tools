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
#include "sqlite_test_api.h"

#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using npucompute::cli::AnalyzeCsv;
using npucompute::cli::CsvSqliteWriteResult;
using npucompute::cli::WriteCsvTable;
namespace sqlite = npucompute::sqlite;
using Row = std::vector<std::optional<std::string>>;

class NpuComputeCsvSqliteWriter : public ::testing::Test {
protected:
    void SetUp() override
    {
        directory_ =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("csv-sqlite-%%%%-%%%%-%%%%");
        ASSERT_TRUE(boost::filesystem::create_directory(directory_));
        ASSERT_EQ(
            sqlite::GetApi().openV2(":memory:", &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr),
            SQLITE_OK);
    }
    void TearDown() override
    {
        if (database_) {
            EXPECT_EQ(sqlite::GetApi().close(database_), SQLITE_OK);
        }
        boost::system::error_code ignored;
        boost::filesystem::remove_all(directory_, ignored);
    }
    std::string File(const std::string& name, std::string_view contents)
    {
        const auto path = (directory_ / name).string();
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        EXPECT_FALSE(output.fail());
        return path;
    }
    CsvSqliteWriteResult Write(const std::string& name, std::string_view contents)
    {
        const auto path = File(name, contents);
        return WriteCsvTable(database_, path, AnalyzeCsv(path));
    }
    void Exec(const char* sql)
    {
        ASSERT_EQ(sqlite::GetApi().exec(database_, sql), SQLITE_OK) << sqlite::GetApi().errmsg(database_);
    }
    std::vector<Row> Query(const char* sql)
    {
        const auto& api = sqlite::GetApi();
        sqlite3_stmt* statement = nullptr;
        if (api.prepareV2(database_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(api.errmsg(database_));
        }
        std::vector<Row> rows;
        int status;
        while ((status = api.step(statement)) == SQLITE_ROW) {
            Row row;
            for (int index = 0; index < api.columnCount(statement); ++index) {
                if (api.columnType(statement, index) == SQLITE_NULL) {
                    row.emplace_back(std::nullopt);
                } else {
                    row.emplace_back(std::string(
                        reinterpret_cast<const char*>(api.columnText(statement, index)),
                        api.columnBytes(statement, index)));
                }
            }
            rows.push_back(std::move(row));
        }
        EXPECT_EQ(status, SQLITE_DONE);
        EXPECT_EQ(api.finalize(statement), SQLITE_OK);
        return rows;
    }
    bool HasTable(const std::string& table)
    {
        return !Query(("SELECT name FROM sqlite_master WHERE type='table' AND name='" + table + "'").c_str()).empty();
    }
    void ExpectFailure(const CsvSqliteWriteResult& result, const std::vector<std::string>& details)
    {
        EXPECT_FALSE(result.written);
        EXPECT_TRUE(result.databaseUsable) << result.error;
        for (const auto& detail : details) {
            EXPECT_NE(result.error.find(detail), std::string::npos) << result.error;
        }
    }
    boost::filesystem::path directory_;
    sqlite3* database_ = nullptr;
};

TEST_F(NpuComputeCsvSqliteWriter, WritesTypesUnitsNullsAndDuplicateRowsInCsvOrder)
{
    const auto result = Write(
        "Memory.csv", "block_id,aiv_time(us),sub_block_id,empty_metric\n"
                      "2,4.220000,vector0,\n1,NA,NA,\n2,4.220000,vector0,\n");
    ASSERT_TRUE(result.written) << result.error;
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(
        Query("SELECT name,type FROM pragma_table_info('Memory') ORDER BY cid"),
        (std::vector<Row>{
            {"block_id", "INTEGER"}, {"aiv_time", "REAL"}, {"sub_block_id", "TEXT"}, {"empty_metric", "TEXT"}}));
    EXPECT_EQ(
        Query("SELECT block_id,aiv_time,sub_block_id,empty_metric FROM Memory ORDER BY rowid"),
        (std::vector<Row>{
            {"2", "4.22", "vector0", ""}, {"1", std::nullopt, std::nullopt, ""}, {"2", "4.22", "vector0", ""}}));
    EXPECT_EQ(
        Query("SELECT typeof(block_id),typeof(aiv_time),typeof(sub_block_id) FROM Memory ORDER BY rowid"),
        (std::vector<Row>{{"integer", "real", "text"}, {"integer", "null", "null"}, {"integer", "real", "text"}}));
    EXPECT_EQ(
        Query("SELECT field_name,unit FROM FieldUnitMap ORDER BY field_name"), (std::vector<Row>{
                                                                                   {"aiv_time", "us"},
                                                                                   {"block_id", std::nullopt},
                                                                                   {"empty_metric", std::nullopt},
                                                                                   {"sub_block_id", std::nullopt}}));
}

TEST_F(NpuComputeCsvSqliteWriter, PreservesInt64AndTextPrecisionBoundaries)
{
    const auto result = Write(
        "ArithmeticUtilization.csv", "counter,maximum,minimum,overflow,precise,spelling,mixed,real_value\n"
                                     "9007199254740993,9223372036854775807,-9223372036854775808,9223372036854775808,0."
                                     "1234567890123456,0012,9007199254740993,0.1\n"
                                     "NA,NA,NA,NA,NA,+12,0.5,120.480320\n");
    ASSERT_TRUE(result.written) << result.error;
    EXPECT_EQ(
        Query(
            "SELECT counter,maximum,minimum,overflow,precise,spelling,mixed FROM ArithmeticUtilization ORDER BY rowid"),
        (std::vector<Row>{
            {"9007199254740993", "9223372036854775807", "-9223372036854775808", "9223372036854775808",
             "0.1234567890123456", "0012", "9007199254740993"},
            {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, "+12", "0.5"}}));
    EXPECT_EQ(
        Query("SELECT typeof(counter),typeof(overflow),typeof(precise),typeof(mixed),typeof(real_value) FROM "
              "ArithmeticUtilization LIMIT 1"),
        (std::vector<Row>{{"integer", "text", "text", "text", "real"}}));
}

TEST_F(NpuComputeCsvSqliteWriter, BindsIntegersInRealColumnsAndKeepsEmptyStrings)
{
    const auto result = Write("Memory.csv", "metric,integer_metric\n1,1\n0.5,NA\n,\n");
    ASSERT_TRUE(result.written) << result.error;
    EXPECT_EQ(
        Query("SELECT metric,typeof(metric),integer_metric,typeof(integer_metric) FROM Memory ORDER BY rowid"),
        (std::vector<Row>{
            {"1.0", "real", "1", "integer"}, {"0.5", "real", std::nullopt, "null"}, {"", "text", "", "text"}}));
}

TEST_F(NpuComputeCsvSqliteWriter, PreservesQuotesNewlinesAndEmbeddedNulInText)
{
    std::string contents = "label,value\n\"comma,quote\"\"\",\"first\r\nsecond\"\n";
    contents += std::string("nul,a\0b\n", 8);
    const auto result = Write("Memory.csv", contents);
    ASSERT_TRUE(result.written) << result.error;
    EXPECT_EQ(
        Query("SELECT label,value FROM Memory ORDER BY rowid"),
        (std::vector<Row>{{"comma,quote\"", "first\r\nsecond"}, {"nul", std::string("a\0b", 3)}}));
}

TEST_F(NpuComputeCsvSqliteWriter, ReusesIdenticalUnitMappingsAcrossTables)
{
    ASSERT_TRUE(Write("Memory.csv", "block_id,time(us)\n0,1\n").written);
    const auto result = Write("Pipeline.csv", "block_id,time(us),cycles(cycle)\n1,2,3\n");
    ASSERT_TRUE(result.written) << result.error;
    EXPECT_EQ(
        Query("SELECT field_name,unit FROM FieldUnitMap ORDER BY field_name"),
        (std::vector<Row>{{"block_id", std::nullopt}, {"cycles", "cycle"}, {"time", "us"}}));
    EXPECT_EQ(
        Query("SELECT pk FROM pragma_table_info('FieldUnitMap') WHERE name='field_name'"), (std::vector<Row>{{"1"}}));
}

TEST_F(NpuComputeCsvSqliteWriter, UnitConflictRollsBackOnlyCurrentTableAndNewMappings)
{
    Exec("CREATE TABLE PcSampling(address INTEGER PRIMARY KEY,others INTEGER); INSERT INTO PcSampling VALUES(256,45)");
    ASSERT_TRUE(Write("Memory.csv", "time(us)\n1\n").written);
    const auto result = Write("Pipeline.csv", "new_metric(cycle),time(ms)\n1,2\n");
    ExpectFailure(result, {"Pipeline.csv", "Pipeline", "time(ms)", "time", "us", "ms", "unit"});
    EXPECT_FALSE(HasTable("Pipeline"));
    EXPECT_EQ(Query("SELECT field_name,unit FROM FieldUnitMap"), (std::vector<Row>{{"time", "us"}}));
    EXPECT_EQ(Query("SELECT time FROM Memory"), (std::vector<Row>{{"1"}}));
    EXPECT_EQ(Query("SELECT address,others FROM PcSampling"), (std::vector<Row>{{"256", "45"}}));
    EXPECT_TRUE(Write("MemoryUB.csv", "another\n7\n").written);
}

TEST_F(NpuComputeCsvSqliteWriter, MissingUnitConflictsWithAUnitInEitherDirection)
{
    ASSERT_TRUE(Write("First.csv", "time\n1\n").written);
    ExpectFailure(Write("Second.csv", "time(us)\n1\n"), {"time", "us", "<none>"});
    ASSERT_TRUE(Write("Third.csv", "speed(GB/s)\n1\n").written);
    ExpectFailure(Write("Fourth.csv", "speed\n1\n"), {"speed", "GB/s", "<none>"});
    EXPECT_EQ(Query("SELECT count(*) FROM FieldUnitMap"), (std::vector<Row>{{"2"}}));
}

TEST_F(NpuComputeCsvSqliteWriter, ExistingTableIsNeverReplacedOrAppended)
{
    ASSERT_TRUE(Write("Memory.csv", "metric(us)\n1\n").written);
    ExpectFailure(Write("memory.csv", "other_field\n2\n"), {"memory.csv", "Memory", "already exists"});
    EXPECT_EQ(Query("SELECT metric FROM Memory"), (std::vector<Row>{{"1"}}));
    EXPECT_EQ(Query("SELECT field_name FROM FieldUnitMap"), (std::vector<Row>{{"metric"}}));
}

TEST_F(NpuComputeCsvSqliteWriter, EmptyCsvDoesNotCreateTablesOrUnitMap)
{
    for (const auto* contents : {"", "field(us)\n"}) {
        const auto result = Write("Memory.csv", contents);
        EXPECT_FALSE(result.written);
        EXPECT_TRUE(result.error.empty());
        EXPECT_TRUE(result.databaseUsable);
        EXPECT_TRUE(Query("SELECT name FROM sqlite_master WHERE type='table'").empty());
    }
}

TEST_F(NpuComputeCsvSqliteWriter, DetectsRowsAddedAfterAnEmptyCsvWasAnalyzed)
{
    for (const auto* contents : {"", "field(us)\n"}) {
        const auto path = File("Memory.csv", contents);
        const auto schema = AnalyzeCsv(path);
        File("Memory.csv", "field(us)\n1\n");
        ExpectFailure(WriteCsvTable(database_, path, schema), {"Memory.csv", "changed after analysis"});
        EXPECT_FALSE(HasTable("Memory"));
        EXPECT_FALSE(HasTable("FieldUnitMap"));
    }
}

TEST_F(NpuComputeCsvSqliteWriter, BindFailureRemovesPartialRowsAndItsNewMappings)
{
    ASSERT_TRUE(Write("First.csv", "existing\n7\n").written);
    // Use SQLite's actual connection limit to fail after a valid first row.
    const auto limit = npucompute::test::SqliteSymbol<int (*)(sqlite3*, int, int)>("sqlite3_limit");
    constexpr int limitLength = 0;
    const auto previous = limit(database_, limitLength, 2048);
    const auto result = Write("Memory.csv", "new_metric(us)\nshort\n" + std::string(4096, 'x') + "\n");
    limit(database_, limitLength, previous);
    ExpectFailure(result, {"Memory.csv", "Memory", "record 3", "physical line 3", "new_metric", "too big"});
    EXPECT_FALSE(HasTable("Memory"));
    EXPECT_EQ(Query("SELECT field_name FROM FieldUnitMap"), (std::vector<Row>{{"existing"}}));
    EXPECT_EQ(Query("SELECT existing FROM First"), (std::vector<Row>{{"7"}}));
}

TEST_F(NpuComputeCsvSqliteWriter, SavepointsKeepTheCallersTransactionOpen)
{
    Exec("BEGIN");
    ASSERT_TRUE(Write("First.csv", "time(us)\n1\n").written);
    ExpectFailure(Write("Second.csv", "new_field,time(ms)\n2,3\n"), {"unit"});
    ASSERT_TRUE(Write("Third.csv", "third\n4\n").written);
    EXPECT_EQ(npucompute::test::SqliteGetAutocommit(database_), 0);
    EXPECT_EQ(Query("SELECT time FROM First"), (std::vector<Row>{{"1"}}));
    EXPECT_EQ(Query("SELECT third FROM Third"), (std::vector<Row>{{"4"}}));
    Exec("ROLLBACK");
    EXPECT_TRUE(Query("SELECT name FROM sqlite_master WHERE type='table'").empty());
}

TEST_F(NpuComputeCsvSqliteWriter, DetectsChangedHeaderAndRemovedSource)
{
    const auto path = File("Memory.csv", "field(us)\n1\n");
    const auto schema = AnalyzeCsv(path);
    File("Memory.csv", "changed(us)\n1\n");
    ExpectFailure(WriteCsvTable(database_, path, schema), {"Memory.csv", "header", "changed"});
    EXPECT_FALSE(HasTable("Memory"));
    boost::filesystem::remove(path);
    ExpectFailure(WriteCsvTable(database_, path, schema), {"Memory.csv", "open"});
    EXPECT_FALSE(HasTable("FieldUnitMap"));
}

TEST_F(NpuComputeCsvSqliteWriter, ChangedFieldCountHasRecordAndPhysicalRange)
{
    const auto path = File("Memory.csv", "name,value\na,1\n");
    const auto schema = AnalyzeCsv(path);
    File("Memory.csv", "name,value\n\"first\nsecond\",1,2\n");
    ExpectFailure(
        WriteCsvTable(database_, path, schema),
        {"Memory.csv", "Memory", "record 2", "physical lines 2-3", "expected 2 fields", "found 3"});
    EXPECT_FALSE(HasTable("Memory"));
    EXPECT_FALSE(HasTable("FieldUnitMap"));
}

TEST_F(NpuComputeCsvSqliteWriter, ChangedTypesCannotSilentlyTruncateOrRoundIntegers)
{
    for (const auto* original : {"field\n1\n", "field\n0.5\n"}) {
        const auto path = File("Memory.csv", original);
        const auto schema = AnalyzeCsv(path);
        File("Memory.csv", "field\n9007199254740993\n");
        if (schema.columns.front().type == npucompute::cli::CsvColumnType::Integer) {
            File("Memory.csv", "field\n1.5\n");
        }
        ExpectFailure(WriteCsvTable(database_, path, schema), {"Memory.csv", "record 2", "field", "type"});
        EXPECT_FALSE(HasTable("Memory"));
        EXPECT_FALSE(HasTable("FieldUnitMap"));
    }
}

TEST_F(NpuComputeCsvSqliteWriter, ChangedRowCountRollsBackInsertedRows)
{
    const auto path = File("Memory.csv", "field\n1\n");
    const auto schema = AnalyzeCsv(path);
    File("Memory.csv", "field\n1\n2\n");
    ExpectFailure(WriteCsvTable(database_, path, schema), {"Memory.csv", "record count", "expected 1", "found 2"});
    EXPECT_FALSE(HasTable("Memory"));
    EXPECT_FALSE(HasTable("FieldUnitMap"));
}

struct SavepointFault {
    bool denyRelease = true;
    bool denyRollback = false;
    static int Authorize(void* context, int operation, const char* action, const char*, const char*, const char*)
    {
        constexpr int sqliteSavepoint = 32;
        if (operation != sqliteSavepoint || action == nullptr) {
            return SQLITE_OK;
        }
        auto& fault = *static_cast<SavepointFault*>(context);
        if (std::string_view(action) == "RELEASE" && fault.denyRelease) {
            fault.denyRelease = false;
            return SQLITE_DENY;
        }
        return std::string_view(action) == "ROLLBACK" && fault.denyRollback ? SQLITE_DENY : SQLITE_OK;
    }
};

TEST_F(NpuComputeCsvSqliteWriter, ReleaseFailureRollsBackTableAndReportsCompletionStage)
{
    SavepointFault fault;
    ASSERT_EQ(npucompute::test::SqliteSetAuthorizer(database_, SavepointFault::Authorize, &fault), SQLITE_OK);
    const auto result = Write("Memory.csv", "field(us)\n1\n");
    npucompute::test::SqliteSetAuthorizer(database_, nullptr, nullptr);
    ExpectFailure(result, {"Memory.csv", "finish", "not authorized"});
    EXPECT_FALSE(HasTable("Memory"));
    EXPECT_FALSE(HasTable("FieldUnitMap"));
    EXPECT_EQ(npucompute::test::SqliteGetAutocommit(database_), 1);
}

TEST_F(NpuComputeCsvSqliteWriter, FailedRollbackMarksWorkingDatabaseUnusable)
{
    SavepointFault fault{true, true};
    ASSERT_EQ(npucompute::test::SqliteSetAuthorizer(database_, SavepointFault::Authorize, &fault), SQLITE_OK);
    const auto result = Write("Memory.csv", "field(us)\n1\n");
    npucompute::test::SqliteSetAuthorizer(database_, nullptr, nullptr);
    EXPECT_FALSE(result.written);
    EXPECT_FALSE(result.databaseUsable);
    EXPECT_NE(result.error.find("rollback"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("discard"), std::string::npos) << result.error;
    Exec("ROLLBACK");
}
} // namespace
