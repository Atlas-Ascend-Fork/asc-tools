/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_SHARED_SQLITE_RUNTIME_LOADER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_SHARED_SQLITE_RUNTIME_LOADER_H

#include "shared/minimal_sqlite.h"
#include <functional>
#include <string>

namespace npucompute::sqlite {

// Callables normalize cdecl SQLite and Windows system SQLite's stdcall ABI.
struct Api {
    std::function<int(const char*, sqlite3**, int, const char*)> openV2;
    std::function<int(sqlite3*)> close;
    std::function<int(sqlite3*)> closeV2;
    std::function<int(sqlite3*, const char*, int, sqlite3_stmt**, const char**)> prepareV2;
    std::function<int(sqlite3_stmt*)> step;
    std::function<int(sqlite3_stmt*)> finalize;
    std::function<int(sqlite3_stmt*)> reset;
    std::function<int(sqlite3_stmt*)> clearBindings;
    std::function<int(sqlite3_stmt*, int, sqlite3_int64)> bindInt64;
    std::function<int(sqlite3_stmt*, int, double)> bindDouble;
    std::function<int(sqlite3_stmt*, int)> bindNull;
    std::function<const char*(sqlite3*)> errmsg;
    std::function<int(sqlite3_stmt*, int)> columnType;
    std::function<sqlite3_int64(sqlite3_stmt*, int)> columnInt64;
    std::function<double(sqlite3_stmt*, int)> columnDouble;
    std::function<const unsigned char*(sqlite3_stmt*, int)> columnText;
    std::function<int(sqlite3_stmt*, int)> columnBytes;
    std::function<int(sqlite3_stmt*)> columnCount;
    // Internal callers do not use SQL callbacks or custom text destructors.
    std::function<int(sqlite3*, const char*)> exec;
    std::function<int(sqlite3_stmt*, int, const char*, int)> bindText;
};

// First access loads and resolves the complete API once, safely across threads.
// Failure is cached and reported as an exception caught at the result boundary.
// A successfully loaded library remains loaded for the lifetime of the process.
const Api& GetApi();
const std::string& LibraryPath();

} // namespace npucompute::sqlite

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_SHARED_SQLITE_RUNTIME_LOADER_H
