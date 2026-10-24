/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_COLLECTION_DIRECTORY_VALIDATOR_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_COLLECTION_DIRECTORY_VALIDATOR_H

#include <boost/filesystem.hpp>
#include <string>
#include <vector>

namespace npucompute::cli {

// After writers stop, remove invalid files and internal staging; retain valid siblings.
// False means an unsafe/unreadable directory, failed cleanup, or no valid result files.
bool ValidateCollectionDirectory(
    const boost::filesystem::path& directory, std::string* error, std::vector<std::string>* issues = nullptr);

} // namespace npucompute::cli
#endif
