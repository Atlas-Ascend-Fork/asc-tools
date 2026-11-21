/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_INSTRUCTION_SQLITE_H
#define NPU_COMPUTE_INSTRUCTION_SQLITE_H

#include "shared/sqlite_runtime_loader.h"
#include <memory>
#include <stdexcept>
#include <string>

namespace npucompute::instruction_sqlite {
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
    void Exec(const char* sql) const { Check(sqlite::GetApi().exec(handle, sql)); }
};
using Statement = std::unique_ptr<sqlite3_stmt, decltype(sqlite::GetApi().finalize)>;
inline Statement Prepare(Database& db, const std::string& sql)
{
    sqlite3_stmt* statement = nullptr;
    const auto status =
        sqlite::GetApi().prepareV2(db.handle, sql.c_str(), sqlite::kNullTerminatedSql, &statement, nullptr);
    Statement owned(statement, sqlite::GetApi().finalize);
    db.Check(status);
    return owned;
}
inline void Open(Database& db, const std::string& path)
{
    db.Check(sqlite::GetApi().openV2(
        path.c_str(), &db.handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr));
}
} // namespace npucompute::instruction_sqlite

#endif
