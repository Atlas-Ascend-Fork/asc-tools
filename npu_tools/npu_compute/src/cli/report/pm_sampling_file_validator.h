/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_PM_SAMPLING_FILE_VALIDATOR_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_PM_SAMPLING_FILE_VALIDATOR_H

#include <boost/filesystem/path.hpp>
#include <string>

namespace npucompute::cli {
// Validate one result independently. A missing or invalid sibling does not
// invalidate this file. CSV-only databases do not require a PcSampling table.
bool ValidatePmSamplingFile(const boost::filesystem::path& path, std::string* error);

// Return whether a validated database contains the empty PcSampling table.
// A database with other CSV tables but no PcSampling table returns false.
bool IsEmptyPmSamplingDatabase(const boost::filesystem::path& path, bool* empty, std::string* error);
} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_PM_SAMPLING_FILE_VALIDATOR_H
