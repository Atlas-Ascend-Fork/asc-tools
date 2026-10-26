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

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using npucompute::cli::AnalyzeCsv;
using npucompute::cli::CsvColumnType;
using npucompute::cli::CsvReader;
using npucompute::cli::NormalizeCsvColumnName;
using npucompute::cli::NormalizeCsvTableName;
using npucompute::cli::ParseCsvInteger;
using npucompute::cli::ParseCsvReal;

class NpuComputeCsvReader : public ::testing::Test {
protected:
    void SetUp() override
    {
        directory_ =
            boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("npu-compute-csv-%%%%-%%%%-%%%%");
        ASSERT_TRUE(boost::filesystem::create_directory(directory_));
    }

    void TearDown() override
    {
        boost::system::error_code ignored;
        boost::filesystem::remove_all(directory_, ignored);
    }

    std::string Write(std::string_view contents)
    {
        const auto path = directory_ / "Memory.csv";
        std::ofstream output(path.string(), std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        EXPECT_FALSE(output.fail());
        return path.string();
    }

    boost::filesystem::path directory_;
};

TEST_F(NpuComputeCsvReader, NormalizesSectionTableNames)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"ArithmeticUtilization.csv", "ArithmeticUtilization"},
        {"L2Cache.csv", "L2Cache"},
        {"Memory.csv", "Memory"},
        {"MemoryL0.csv", "MemoryL0"},
        {"MemoryUB.csv", "MemoryUB"},
        {"PipeUtilization.csv", "PipeUtilization"},
        {"PmSampling", "PmSampling"},
        {"pipe_utilization.csv", "PipeUtilization"},
        {"ResourceConflictRatio.csv", "ResourceConflictRatio"}};
    for (const auto& item : cases) {
        EXPECT_EQ(NormalizeCsvTableName(item.first), item.second);
    }
}

TEST_F(NpuComputeCsvReader, NormalizesFieldsAndRemovesUnitSuffixes)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"block_id", "block_id"},
        {"aiv_time(us)", "aiv_time"},
        {"aic_l1_read_bw(GB/s)", "aic_l1_read_bw"},
        {"GM_to_L1_datas(KB)", "gm_to_l1_datas"},
        {"GM_to_L1_bw_usage_rate(%)", "gm_to_l1_bw_usage_rate"},
        {"a-b", "a_b"},
        {" a__b ", "a_b"}};
    for (const auto& item : cases) {
        EXPECT_EQ(NormalizeCsvColumnName(item.first), item.second);
    }
}

TEST_F(NpuComputeCsvReader, RejectsInvalidOrReservedNames)
{
    for (const auto* name : {"", "---", "12name", "sqlite_master"}) {
        EXPECT_THROW(NormalizeCsvTableName(name), std::runtime_error) << name;
        EXPECT_THROW(NormalizeCsvColumnName(name), std::runtime_error) << name;
    }
    for (const auto* name : {"rowid", "OID", "_rowid_"}) {
        EXPECT_THROW(NormalizeCsvColumnName(name), std::runtime_error) << name;
    }
    EXPECT_EQ(NormalizeCsvColumnName("RowId"), "row_id");
    EXPECT_THROW(NormalizeCsvColumnName(std::string("x\0y", 3)), std::runtime_error);
}

TEST_F(NpuComputeCsvReader, ReadsQuotedFieldsAndPreservesEmbeddedNewlines)
{
    std::istringstream input("\xef\xbb\xbf"
                             "a,b,c\r\n\"comma,value\",\"first\r\nsecond\",\"say \"\"hi\"\"\"\r\n,,");
    CsvReader reader(input, "Memory.csv");
    std::vector<std::string> fields;
    ASSERT_TRUE(reader.ReadRecord(fields));
    EXPECT_EQ(fields, (std::vector<std::string>{"a", "b", "c"}));
    ASSERT_TRUE(reader.ReadRecord(fields));
    EXPECT_EQ(fields, (std::vector<std::string>{"comma,value", "first\r\nsecond", "say \"hi\""}));
    ASSERT_TRUE(reader.ReadRecord(fields));
    EXPECT_EQ(fields, (std::vector<std::string>{"", "", ""}));
    EXPECT_FALSE(reader.ReadRecord(fields));
}

TEST_F(NpuComputeCsvReader, HandlesShortInputsAndEveryRecordTerminator)
{
    for (const std::string contents : {"", "x", "x\n", "x\r", "x\r\n", "\"\""}) {
        std::istringstream input(contents);
        CsvReader reader(input, "sample.csv");
        std::vector<std::string> fields;
        EXPECT_EQ(reader.ReadRecord(fields), !contents.empty());
        if (!contents.empty()) {
            EXPECT_EQ(fields, (std::vector<std::string>{contents == "\"\"" ? "" : "x"}));
        }
        EXPECT_FALSE(reader.ReadRecord(fields));
    }
    std::istringstream input("a\rb\nc\r\nd\n\n");
    CsvReader reader(input, "sample.csv");
    std::vector<std::string> fields;
    for (const auto* expected : {"a", "b", "c", "d", ""}) {
        ASSERT_TRUE(reader.ReadRecord(fields));
        EXPECT_EQ(fields, (std::vector<std::string>{expected}));
    }
    EXPECT_FALSE(reader.ReadRecord(fields));
}

TEST_F(NpuComputeCsvReader, DoesNotRemoveBomInsideData)
{
    std::istringstream input("h\n\xef\xbb\xbf"
                             "value\n");
    CsvReader reader(input, "sample.csv");
    std::vector<std::string> fields;
    ASSERT_TRUE(reader.ReadRecord(fields));
    ASSERT_TRUE(reader.ReadRecord(fields));
    EXPECT_EQ(
        fields.front(), "\xef\xbb\xbf"
                        "value");
}

TEST_F(NpuComputeCsvReader, RejectsMalformedQuotesWithSourceAndReason)
{
    for (const auto* text : {"\"unterminated", "un\"quoted\n", "\"closed\"extra\n"}) {
        std::istringstream input(text);
        CsvReader reader(input, "broken.csv");
        std::vector<std::string> fields;
        try {
            reader.ReadRecord(fields);
            FAIL() << "Accepted malformed CSV: " << text;
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("broken.csv"), std::string::npos);
            EXPECT_NE(std::string(error.what()).find("quot"), std::string::npos);
        }
    }
}

TEST_F(NpuComputeCsvReader, ReportsInputFailureRatherThanEndOfFile)
{
    std::istringstream input("header\n1\n");
    input.setstate(std::ios::badbit);
    CsvReader reader(input, "unreadable.csv");
    std::vector<std::string> fields;
    EXPECT_THROW(reader.ReadRecord(fields), std::runtime_error);
}

TEST_F(NpuComputeCsvReader, ParsesCanonicalInt64WithoutFloatingPoint)
{
    for (const auto& item : std::vector<std::pair<std::string, std::int64_t>>{
             {"0", 0},
             {"12", 12},
             {"-12", -12},
             {"9007199254740993", INT64_C(9007199254740993)},
             {"9223372036854775807", std::numeric_limits<std::int64_t>::max()},
             {"-9223372036854775808", std::numeric_limits<std::int64_t>::min()}}) {
        std::int64_t value = 123;
        ASSERT_TRUE(ParseCsvInteger(item.first, value)) << item.first;
        EXPECT_EQ(value, item.second);
    }
}

TEST_F(NpuComputeCsvReader, RejectsNonCanonicalOrOutOfRangeIntegers)
{
    for (const auto* text :
         {"", "NA", "+12", "0012", "-0", "-01", "1e3", "4.220000", " 12", "12 ", "9223372036854775808",
          "-9223372036854775809", "-", "12x"}) {
        std::int64_t value = 123;
        EXPECT_FALSE(ParseCsvInteger(text, value)) << text;
        EXPECT_EQ(value, 123) << "Failed parsing must not modify the integer output";
    }
}

TEST_F(NpuComputeCsvReader, ScansWholeColumnsAndRetainsOriginalHeaderOrder)
{
    const auto schema = AnalyzeCsv(Write("block_id,wide,decimal,late,missing,mixed\n"
                                         "2,9007199254740993,4.224242,12,NA,12\n"
                                         "0,-9223372036854775808,4.220000,9223372036854775808,,NA\n"
                                         "2,9223372036854775807,NA,0,,\n"));
    ASSERT_EQ(schema.columns.size(), 6U);
    EXPECT_EQ(schema.tableName, "Memory");
    EXPECT_EQ(schema.rowCount, 3U);
    const std::vector<CsvColumnType> expected = {CsvColumnType::Integer, CsvColumnType::Integer,
                                                 CsvColumnType::Real,    CsvColumnType::Text,
                                                 CsvColumnType::Text,    CsvColumnType::Integer};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(schema.columns[i].type, expected[i]);
    }
    EXPECT_EQ(schema.columns[0].sourceName, "block_id");
    EXPECT_EQ(schema.columns[0].name, "block_id");
    EXPECT_EQ(schema.columns[5].name, "mixed");
}

TEST_F(NpuComputeCsvReader, NonCanonicalNumbersAndTextMakeEntireColumnText)
{
    for (const auto* value : {"0012", "+12", "-0", "1e3", "text", "-9223372036854775809"}) {
        const auto schema = AnalyzeCsv(Write(std::string("metric\n12\n") + value + "\n"));
        ASSERT_EQ(schema.columns.size(), 1U);
        EXPECT_EQ(schema.columns[0].type, CsvColumnType::Text) << value;
    }
}

TEST_F(NpuComputeCsvReader, EmptyAndHeaderOnlyInputsHaveNoDataRows)
{
    auto schema = AnalyzeCsv(Write(""));
    EXPECT_TRUE(schema.columns.empty());
    EXPECT_EQ(schema.rowCount, 0U);
    schema = AnalyzeCsv(Write("\xef\xbb\xbf"
                              "aiv_time(us),block_id\r\n"));
    ASSERT_EQ(schema.columns.size(), 2U);
    EXPECT_EQ(schema.columns[0].name, "aiv_time");
    EXPECT_EQ(schema.rowCount, 0U);
}

TEST_F(NpuComputeCsvReader, RejectsCaseInsensitiveNormalizedHeaderCollisions)
{
    for (const auto* text : {"a-b,a_b\n1,2\n", "a_b,a_B\n1,2\n", "a_b(us),a_b(ms)\n1,2\n"}) {
        try {
            AnalyzeCsv(Write(text));
            FAIL() << "Accepted conflicting header";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("Memory.csv"), std::string::npos);
            EXPECT_NE(std::string(error.what()).find("a_b"), std::string::npos);
        }
    }
}

TEST_F(NpuComputeCsvReader, RejectsEmptyHeaderAndWrongFieldCounts)
{
    for (const auto* text : {"a,,b\n1,2,3\n", "a,b\n1\n", "a,b\n1,2,3\n"}) {
        EXPECT_THROW(AnalyzeCsv(Write(text)), std::runtime_error);
    }
}

TEST_F(NpuComputeCsvReader, ReportsMissingFileWithItsPath)
{
    const auto path = (directory_ / "Missing.csv").string();
    try {
        AnalyzeCsv(path);
        FAIL() << "Accepted missing CSV";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(path), std::string::npos);
        EXPECT_NE(
            std::string(error.what()).find(std::make_error_code(std::errc::no_such_file_or_directory).message()),
            std::string::npos);
    }
}

TEST_F(NpuComputeCsvReader, RejectsDedicatedTableNamesAndUnsafeFilenames)
{
    for (const auto* name :
         {"PcSampling.csv", "pc_sampling.csv", "PCSAMPLING.csv", "FieldUnitMap.csv", "field_unit_map.csv",
          "../Memory.csv", "a/b.csv", "a\\b.csv", "a.b.csv", "Memory?.csv", "sqlite_master.csv", "SQLITE_temp.csv"}) {
        EXPECT_THROW(NormalizeCsvTableName(name), std::runtime_error) << name;
    }
    EXPECT_EQ(NormalizeCsvTableName("arithmetic_utilization.csv"), "ArithmeticUtilization");
    EXPECT_EQ(NormalizeCsvTableName("memory_ub.csv"), "MemoryUb");
    EXPECT_EQ(NormalizeCsvColumnName("IBuf_Empty"), "ibuf_empty");
    EXPECT_EQ(NormalizeCsvColumnName("L0C_to_GM_datas(KB)"), "l0c_to_gm_datas");
    EXPECT_EQ(NormalizeCsvColumnName("aicL1ReadBW(GB/s)"), "aic_l1_read_bw");
    EXPECT_EQ(NormalizeCsvColumnName("HTTPServerID"), "http_server_id");
}

TEST_F(NpuComputeCsvReader, ExtractsUnitMetadataWithoutChangingItsSpelling)
{
    const auto schema = AnalyzeCsv(Write("block_id,aiv_time(us),aic_l1_read_bw(GB/s),rate(%)\n1,4.220000,0.1,12\n"));
    ASSERT_EQ(schema.columns.size(), 4U);
    EXPECT_FALSE(schema.columns[0].unit);
    ASSERT_TRUE(schema.columns[1].unit);
    ASSERT_TRUE(schema.columns[2].unit);
    ASSERT_TRUE(schema.columns[3].unit);
    EXPECT_EQ(*schema.columns[1].unit, "us");
    EXPECT_EQ(*schema.columns[2].unit, "GB/s");
    EXPECT_EQ(*schema.columns[3].unit, "%");
    EXPECT_EQ(schema.columns[1].sourceName, "aiv_time(us)");
    EXPECT_EQ(schema.columns[1].name, "aiv_time");
    EXPECT_EQ(schema.columns[2].name, "aic_l1_read_bw");
    EXPECT_EQ(schema.columns[3].name, "rate");
    for (const auto& header :
         std::vector<std::string>{"metric()", "metric(us", "metric)us(", std::string("metric(u\0s)", 11)}) {
        EXPECT_THROW(AnalyzeCsv(Write(header + "\n1\n")), std::runtime_error);
    }
}

TEST_F(NpuComputeCsvReader, ParsesOrdinaryDecimalsAsReal)
{
    for (const auto* text :
         {"0.1", "0.5", "4.224242", "4.220000", "120.480320", "-0.5", "0.000000", "0.123456789012345",
          "1000000000000000.000"}) {
        double real = 123;
        EXPECT_TRUE(ParseCsvReal(text, real)) << text;
        const auto schema = AnalyzeCsv(Write(std::string("metric\n12\nNA\n\n") + text + "\n"));
        ASSERT_EQ(schema.columns.size(), 1U);
        EXPECT_EQ(schema.columns[0].type, CsvColumnType::Real) << text;
    }
    double real = 123;
    ASSERT_TRUE(ParseCsvReal("4.220000", real));
    EXPECT_DOUBLE_EQ(real, 4.22);
}

TEST_F(NpuComputeCsvReader, RetainsHighPrecisionAndNonCanonicalDecimalsAsText)
{
    std::vector<std::string> values = {
        "0.1234567890123456",
        "9007199254740993.0",
        "001.2",
        "+0.5",
        "-0.0",
        ".5",
        "1.",
        "1.0e3",
        "nan",
        "inf",
        " 0.5",
        "0.5 ",
        "na",
        "N/A",
        " NA "};
    values.push_back("1" + std::string(400, '0') + ".0");
    values.push_back("0." + std::string(400, '0') + "1");
    for (const auto& value : values) {
        double real = 123;
        EXPECT_FALSE(ParseCsvReal(value, real)) << value;
        EXPECT_EQ(real, 123);
        const auto schema = AnalyzeCsv(Write("metric\n12\n" + value + "\n"));
        EXPECT_EQ(schema.columns[0].type, CsvColumnType::Text) << value;
    }
}

TEST_F(NpuComputeCsvReader, ChecksAllIntegersBeforePromotingAMixedColumnToReal)
{
    for (const auto* integer : {"9007199254740993", "9223372036854775807", "-9223372036854775807"}) {
        for (bool integerFirst : {true, false}) {
            const std::string contents =
                std::string("metric\n") +
                (integerFirst ? std::string(integer) + "\n0.5\n" : "0.5\n" + std::string(integer) + "\n");
            EXPECT_EQ(AnalyzeCsv(Write(contents)).columns[0].type, CsvColumnType::Text) << integer;
        }
    }
    for (const auto* integer : {"9007199254740992", "1152921504606846976", "-9223372036854775808"}) {
        const auto schema = AnalyzeCsv(Write(std::string("metric\n") + integer + "\n0.5\n"));
        EXPECT_EQ(schema.columns[0].type, CsvColumnType::Real) << integer;
    }
}

TEST_F(NpuComputeCsvReader, IgnoresOnlyExactNaAndEmptyFieldsDuringTypeInference)
{
    const std::string contents = "integer,real,missing,empty\n1,4.220000,NA,\n\"NA\",\"NA\",,\"\"\n1,4.220000,NA,\n";
    const auto schema = AnalyzeCsv(Write(contents));
    ASSERT_EQ(schema.columns.size(), 4U);
    EXPECT_EQ(schema.rowCount, 3U); // Duplicate rows must not be removed.
    EXPECT_EQ(schema.columns[0].type, CsvColumnType::Integer);
    EXPECT_EQ(schema.columns[1].type, CsvColumnType::Real);
    EXPECT_EQ(schema.columns[2].type, CsvColumnType::Text);
    EXPECT_EQ(schema.columns[3].type, CsvColumnType::Text);
    std::istringstream input(contents);
    CsvReader reader(input, "Memory.csv");
    std::vector<std::string> fields;
    ASSERT_TRUE(reader.ReadRecord(fields));
    ASSERT_TRUE(reader.ReadRecord(fields));
    const auto first = fields;
    ASSERT_TRUE(reader.ReadRecord(fields));
    EXPECT_EQ(fields, (std::vector<std::string>{"NA", "NA", "", ""}));
    ASSERT_TRUE(reader.ReadRecord(fields));
    EXPECT_EQ(fields, first);
}

TEST_F(NpuComputeCsvReader, TracksLogicalRecordsAndPhysicalLines)
{
    std::istringstream input("a,b\r\n1,\"two\r\nthree\"\r\n2,3\r4,5\n6,7");
    CsvReader reader(input, "Memory.csv");
    std::vector<std::string> fields;
    const std::vector<std::pair<std::size_t, std::size_t>> lines = {{1, 1}, {2, 3}, {4, 4}, {5, 5}, {6, 6}};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        ASSERT_TRUE(reader.ReadRecord(fields));
        EXPECT_EQ(reader.Location().record, i + 1);
        EXPECT_EQ(reader.Location().firstLine, lines[i].first);
        EXPECT_EQ(reader.Location().lastLine, lines[i].second);
    }
    EXPECT_FALSE(reader.ReadRecord(fields));
}

TEST_F(NpuComputeCsvReader, ReportsMalformedMultilineFieldsWithPhysicalRange)
{
    for (const auto* contents : {"a,b\n1,\"two\nthree", "a,b\r\n1,\"two\r\nthree\"extra"}) {
        try {
            AnalyzeCsv(Write(contents));
            FAIL() << "Accepted malformed multiline field";
        } catch (const std::runtime_error& error) {
            const std::string text(error.what());
            EXPECT_NE(text.find("Memory.csv"), std::string::npos);
            EXPECT_NE(text.find("Memory"), std::string::npos);
            EXPECT_NE(text.find("record 2"), std::string::npos);
            EXPECT_NE(text.find("physical lines 2-3"), std::string::npos);
            EXPECT_NE(text.find("quot"), std::string::npos);
        }
    }
}

TEST_F(NpuComputeCsvReader, ReportsWrongFieldCountWithTableAndPhysicalRange)
{
    try {
        AnalyzeCsv(Write("a,b\n1,\"two\nthree\",extra\n"));
        FAIL() << "Accepted extra field";
    } catch (const std::runtime_error& error) {
        const std::string text(error.what());
        for (const auto* expected :
             {"Memory.csv", "table 'Memory'", "record 2", "physical lines 2-3", "expected 2 fields", "found 3"}) {
            EXPECT_NE(text.find(expected), std::string::npos) << text;
        }
    }
}

} // namespace
