/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_CONFIG_CONFIG_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_CONFIG_CONFIG_H

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace npucompute::cli {

enum class ReplayMode {
    Kernel,
};

struct CliConfig {
    bool show_help = false;
    bool no_pack = false;
    bool list_sections = false;
    bool list_sets = false;
    std::vector<std::string> sets;
    bool collect_pipeline = false;
    std::vector<std::string> sections;
    ReplayMode replay_mode = ReplayMode::Kernel;
    bool replay_mode_specified = false;
    std::optional<std::string> import_path;
    std::optional<std::string> export_path;
    std::string program;
    std::vector<std::string> program_arguments;
};

bool ParseCli(int argc, char** argv, CliConfig* config, std::vector<std::string>* errors);

const char* ReplayModeName(ReplayMode mode);

void PrintUsage(FILE* stream, const char* program);

void PrintSections(FILE* stream);
void PrintSets(FILE* stream);

} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_CONFIG_CONFIG_H
