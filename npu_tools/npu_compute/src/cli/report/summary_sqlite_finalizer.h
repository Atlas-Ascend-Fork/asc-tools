/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_SUMMARY_SQLITE_FINALIZER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_SUMMARY_SQLITE_FINALIZER_H

#include <boost/filesystem/path.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace npucompute::cli {
struct SummarySqliteFinalizationResult {
    std::size_t databasesUpdated = 0;
    std::vector<std::string> issues;
};

// Called after writers have closed their files in the private collection tree.
// Each directory's CSVs update only its own summary.db, using a working copy.
// All failures are returned as issues; successful directories are retained.
SummarySqliteFinalizationResult FinalizeSummarySqlite(const boost::filesystem::path& collectionDirectory);
} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_SUMMARY_SQLITE_FINALIZER_H
