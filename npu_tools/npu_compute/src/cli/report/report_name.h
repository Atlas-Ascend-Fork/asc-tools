/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_REPORT_NAME_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_REPORT_NAME_H

#include <array>
#include <cstdint>
#include <boost/filesystem/path.hpp>
#include <optional>
#include <string>

namespace npucompute::cli {

struct ReportTarget {
    boost::filesystem::path path;
};

using EpochMillisecondsSource = bool (*)(uint64_t* value, void* context, std::string* error);
using RandomBytesSource = bool (*)(std::array<uint8_t, 4>* value, void* context, std::string* error);

struct ReportNameSources {
    boost::filesystem::path current_directory;
    EpochMillisecondsSource epoch_milliseconds = nullptr;
    RandomBytesSource random_bytes = nullptr;
    void* context = nullptr;
};

bool GenerateReportBaseName(std::string* name, std::string* error);

bool GenerateReportBaseNameWithSources(const ReportNameSources& sources, std::string* name, std::string* error);

bool ResolveReportTarget(const std::optional<std::string>& export_path, ReportTarget* target, std::string* error);

bool ResolveReportTargetWithSources(
    const std::optional<std::string>& export_path, const ReportNameSources& sources, ReportTarget* target,
    std::string* error);

} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_REPORT_NAME_H
