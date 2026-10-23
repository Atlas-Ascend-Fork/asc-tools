/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_SHARED_MINIMAL_SQLITE_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_SHARED_MINIMAL_SQLITE_H

// Opaque ABI types and constants only. No SQLite implementation or link symbols.
struct sqlite3;
struct sqlite3_stmt;
struct sqlite3_api_routines;
using sqlite3_int64 = long long;

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ERROR = 1;
constexpr int SQLITE_DENY = 1;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_INTEGER = 1;
constexpr int SQLITE_FLOAT = 2;
constexpr int SQLITE_TEXT = 3;
constexpr int SQLITE_NULL = 5;
constexpr int SQLITE_OPEN_READONLY = 0x00000001;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;
constexpr int SQLITE_OPEN_NOMUTEX = 0x00008000;

namespace npucompute::sqlite {
inline constexpr int kNullTerminatedSql = -1;
inline constexpr int kTransientDestructor = -1;
inline constexpr int kFirstBindParameter = 1;
inline constexpr int kFirstResultColumn = 0;
inline constexpr char kDatabaseHeader[] = "SQLite format 3";
} // namespace npucompute::sqlite

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_SHARED_MINIMAL_SQLITE_H
