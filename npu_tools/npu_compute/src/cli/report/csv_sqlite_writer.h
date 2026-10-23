/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_CSV_SQLITE_WRITER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_CSV_SQLITE_WRITER_H

#include "report/csv_reader.h"
#include <string>

struct sqlite3;

namespace npucompute::cli {

struct CsvSqliteWriteResult {
    bool written = false;       // Empty/header-only CSVs are skipped without an error.
    bool databaseUsable = true; // False if the caller must discard the working database.
    std::string error;
};

// The caller owns/serializes the connection and supplies AnalyzeCsv's schema.
// A savepoint isolates this table and its new FieldUnitMap entries, including
// when the caller has already started a transaction. Does not open/publish files.
CsvSqliteWriteResult WriteCsvTable(sqlite3* database, const std::string& path, const CsvSchema& schema);

} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_CSV_SQLITE_WRITER_H
