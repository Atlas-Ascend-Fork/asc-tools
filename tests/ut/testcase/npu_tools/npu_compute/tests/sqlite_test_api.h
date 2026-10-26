/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef TESTS_UT_TESTCASE_NPU_TOOLS_NPU_COMPUTE_TESTS_SQLITE_TEST_API_H
#define TESTS_UT_TESTCASE_NPU_TOOLS_NPU_COMPUTE_TESTS_SQLITE_TEST_API_H

#include "shared/sqlite_runtime_loader.h"
#include <dlfcn.h>
#include <stdexcept>
#include <string>

// Fault-injection APIs are test-only, not required from a production SQLite DLL.
namespace npucompute::test {
constexpr int SQLITE_TRANSACTION = 22;
constexpr int SQLITE_CREATE_TABLE = 2;
constexpr unsigned SQLITE_TRACE_PROFILE = 0x02;

template <class Function>
Function SqliteSymbol(const char* name)
{
    static auto* library = dlopen(sqlite::LibraryPath().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        throw std::runtime_error("test SQLite library cannot be reopened");
    }
    auto* symbol = dlsym(library, name);
    if (!symbol) {
        throw std::runtime_error("test SQLite library missing symbol: " + std::string(name));
    }
    return reinterpret_cast<Function>(symbol);
}

inline int SqliteAutoExtension(void (*extension)())
{
    return SqliteSymbol<int (*)(void (*)())>("sqlite3_auto_extension")(extension);
}
inline int SqliteCancelAutoExtension(void (*extension)())
{
    return SqliteSymbol<int (*)(void (*)())>("sqlite3_cancel_auto_extension")(extension);
}
inline int SqliteSetAuthorizer(
    sqlite3* db, int (*callback)(void*, int, const char*, const char*, const char*, const char*), void* context)
{
    return SqliteSymbol<int (*)(sqlite3*, decltype(callback), void*)>("sqlite3_set_authorizer")(db, callback, context);
}
inline int SqliteTraceV2(sqlite3* db, unsigned mask, int (*callback)(unsigned, void*, void*, void*), void* context)
{
    return SqliteSymbol<int (*)(sqlite3*, unsigned, decltype(callback), void*)>("sqlite3_trace_v2")(
        db, mask, callback, context);
}
inline const char* SqliteSql(sqlite3_stmt* statement)
{
    return SqliteSymbol<const char* (*)(sqlite3_stmt*)>("sqlite3_sql")(statement);
}
inline sqlite3* SqliteDbHandle(sqlite3_stmt* statement)
{
    return SqliteSymbol<sqlite3* (*)(sqlite3_stmt*)>("sqlite3_db_handle")(statement);
}
inline int SqliteGetAutocommit(sqlite3* db) { return SqliteSymbol<int (*)(sqlite3*)>("sqlite3_get_autocommit")(db); }
} // namespace npucompute::test

#endif // TESTS_UT_TESTCASE_NPU_TOOLS_NPU_COMPUTE_TESTS_SQLITE_TEST_API_H
