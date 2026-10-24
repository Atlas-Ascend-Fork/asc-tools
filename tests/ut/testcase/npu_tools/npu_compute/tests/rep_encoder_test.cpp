/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "report/rep_encoder.h"
#include "rep_test_decoder.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int Check(bool condition, const char* expression, int line)
{
    if (condition) {
        return 0;
    }
    ADD_FAILURE_AT(__FILE__, line) << expression;
    return 1;
}

#define CHECK(expression)                               \
    do {                                                \
        if (Check((expression), #expression, __LINE__)) \
            return 1;                                   \
    } while (false)

using npucompute::cli::EncodeRep;
using npucompute::cli::NPU_REP_HEAD_SIZE;
using npucompute::cli::NPU_REP_ORIGIN;
using npucompute::cli::NPU_REP_VERSION;
using npucompute::cli::NpuRepFileType;
using npucompute::cli::RepEntry;
using npucompute::cli::test::DecodedRep;
using npucompute::cli::test::DecodeRep;

int TestEmptyRep()
{
    std::vector<uint8_t> encoded;
    std::string error;
    CHECK(EncodeRep({}, &encoded, &error));
    CHECK(error.empty());
    CHECK(encoded.size() == NPU_REP_HEAD_SIZE);

    DecodedRep decoded;
    CHECK(DecodeRep(encoded, &decoded, &error));
    CHECK(decoded.version == NPU_REP_VERSION);
    CHECK(decoded.origin == NPU_REP_ORIGIN);
    CHECK(decoded.file_info_count == 0);
    CHECK(decoded.rep_length == NPU_REP_HEAD_SIZE);
    CHECK(decoded.entries.empty());
    return 0;
}

int TestMultipleEntries()
{
    const std::vector<uint8_t> jsonl = {'{', '}', '\n'};
    const std::vector<uint8_t> csv = {'a', ',', 'b', '\n', '1', ',', '2', '\n'};
    const std::vector<RepEntry> entries = {
        {"HardwareInfo.jsonl", NpuRepFileType::Jsonl, jsonl},
        {"PipeUtilization.csv", NpuRepFileType::Csv, csv},
    };
    std::vector<uint8_t> encoded;
    std::string error;
    CHECK(EncodeRep(entries, &encoded, &error));

    DecodedRep decoded;
    CHECK(DecodeRep(encoded, &decoded, &error));
    CHECK(decoded.entries.size() == entries.size());
    CHECK(decoded.entries[0].file_name == entries[0].file_name);
    CHECK(decoded.entries[0].file_type == entries[0].file_type);
    CHECK(decoded.entries[0].payload == jsonl);
    CHECK(decoded.entries[1].file_name == entries[1].file_name);
    CHECK(decoded.entries[1].file_type == entries[1].file_type);
    CHECK(decoded.entries[1].payload == csv);
    CHECK(decoded.entries[1].file_offset == decoded.entries[0].file_offset + jsonl.size());
    return 0;
}

int TestInvalidEntries()
{
    std::vector<uint8_t> encoded = {1, 2, 3};
    std::string error;
    CHECK(!EncodeRep({{"", NpuRepFileType::Csv, {1}}}, &encoded, &error));
    CHECK(encoded.empty());
    CHECK(!error.empty());

    std::string long_name(128, 'x');
    CHECK(!EncodeRep({{long_name, NpuRepFileType::Csv, {1}}}, &encoded, &error));
    CHECK(!EncodeRep({{"bad/name.csv", NpuRepFileType::Csv, {1}}}, &encoded, &error));
    CHECK(!EncodeRep({{"bad\\name.csv", NpuRepFileType::Csv, {1}}}, &encoded, &error));
    CHECK(
        !EncodeRep({{"same.csv", NpuRepFileType::Csv, {1}}, {"same.csv", NpuRepFileType::Csv, {2}}}, &encoded, &error));
    std::string embedded_null("bad\0name.csv", 12);
    CHECK(!EncodeRep({{embedded_null, NpuRepFileType::Csv, {1}}}, &encoded, &error));
    CHECK(!EncodeRep({}, nullptr, &error));
    return 0;
}

} // namespace

static int RunSuiteMain()
{
    if (TestEmptyRep() != 0 || TestMultipleEntries() != 0 || TestInvalidEntries() != 0) {
        return 1;
    }
    return 0;
}

TEST(NpuComputeRepEncoder, Main) { ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeRepEncoder reported failure"; }
