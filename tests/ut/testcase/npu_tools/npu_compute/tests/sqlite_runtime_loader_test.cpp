/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "shared/sqlite_runtime_loader.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sqlite = npucompute::sqlite;
namespace {
void Require(bool ok, const char* message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}
void CheckDatabase()
{
    const auto& api = sqlite::GetApi();
    sqlite3* db = nullptr;
    Require(api.openV2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK, "open");
    Require(api.exec(db, "CREATE TABLE Sample(i INTEGER, r REAL, t TEXT, n INTEGER)") == SQLITE_OK, "create");
    sqlite3_stmt* statement = nullptr;
    Require(api.prepareV2(db, "INSERT INTO Sample VALUES(?,?,?,?)", -1, &statement, nullptr) == SQLITE_OK, "prepare");
    Require(api.bindInt64(statement, 1, std::numeric_limits<sqlite3_int64>::max()) == SQLITE_OK, "integer");
    Require(api.bindDouble(statement, 2, 0.5) == SQLITE_OK, "double");
    std::string text("a\0b", 3);
    Require(api.bindText(statement, 3, text.data(), text.size()) == SQLITE_OK, "text");
    text[0] = 'z'; // SQLITE_TRANSIENT must have copied the original bytes.
    Require(api.bindNull(statement, 4) == SQLITE_OK, "null");
    Require(api.step(statement) == SQLITE_DONE, "insert");
    Require(api.reset(statement) == SQLITE_OK && api.clearBindings(statement) == SQLITE_OK, "reset");
    Require(api.finalize(statement) == SQLITE_OK, "finalize");
    Require(api.prepareV2(db, "SELECT * FROM Sample", -1, &statement, nullptr) == SQLITE_OK, "query");
    Require(api.step(statement) == SQLITE_ROW && api.columnCount(statement) == 4, "row");
    Require(
        api.columnType(statement, 0) == SQLITE_INTEGER &&
            api.columnInt64(statement, 0) == std::numeric_limits<sqlite3_int64>::max(),
        "int64 precision");
    Require(api.columnType(statement, 1) == SQLITE_FLOAT && api.columnDouble(statement, 1) == 0.5, "real");
    Require(
        api.columnType(statement, 2) == SQLITE_TEXT && api.columnBytes(statement, 2) == 3 &&
            std::string(reinterpret_cast<const char*>(api.columnText(statement, 2)), 3) == std::string("a\0b", 3),
        "text copy");
    Require(api.columnType(statement, 3) == SQLITE_NULL, "null type");
    Require(api.step(statement) == SQLITE_DONE && api.finalize(statement) == SQLITE_OK, "query end");
    Require(api.close(db) == SQLITE_OK, "close");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "lazy") {
        return 0;
    }
    try {
        if (mode == "unavailable") {
            std::string first;
            for (int attempt = 0; attempt < 2; ++attempt) {
                try {
                    sqlite::GetApi();
                    return 3;
                } catch (const std::exception& error) {
                    if (attempt == 0) {
                        first = error.what();
                    } else {
                        Require(first == error.what(), "failed initialization was not cached");
                    }
                }
                unsetenv("NPU_COMPUTE_SQLITE_STUB_MODE");
            }
            std::cout << first << '\n';
            return 0;
        }
        if (mode == "concurrent") {
            std::atomic<bool> valid{true};
            std::vector<std::thread> threads;
            for (int index = 0; index < 16; ++index) {
                threads.emplace_back([&] {
                    try {
                        CheckDatabase();
                    } catch (...) {
                        valid = false;
                    }
                });
            }
            for (auto& thread : threads) {
                thread.join();
            }
            Require(valid, "concurrent first use failed");
        } else {
            CheckDatabase();
        }
        std::cout << "SQLite ready: " << sqlite::LibraryPath() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
