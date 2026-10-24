/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <cstdio>
#include <string>
#include <vector>

#include "config/config.h"
#include "import/import_output_directory.h"
#include "import/imported_profile_results.h"
#include "launch/launcher.h"

int main(int argc, char** argv)
{
    npucompute::cli::CliConfig config;
    std::vector<std::string> parse_errors;
    const bool parsed = npucompute::cli::ParseCli(argc, argv, &config, &parse_errors);
    for (const std::string& error : parse_errors) {
        std::fprintf(stderr, "[ERROR] npu-compute: %s\n", error.c_str());
    }
    if (!parse_errors.empty()) {
        std::fflush(stderr);
    }
    if (config.show_help) {
        npucompute::cli::PrintUsage(stdout, "npu-compute");
    }
    if (!parsed) {
        return npucompute::cli::kUsageErrorExitCode;
    }

    if (config.show_help) {
        return 0;
    }

    if (config.list_sets) {
        npucompute::cli::PrintSets(stdout);
        return 0;
    }
    if (config.list_sections) {
        npucompute::cli::PrintSections(stdout);
        return 0;
    }

    if (config.import_path.has_value()) {
        std::string error;
        std::vector<npucompute::cli::ImportedProfileEntry> results;
        if (!npucompute::cli::ReadImportedProfileResults(*config.import_path, &results, &error)) {
            std::fprintf(stderr, "[ERROR] npu-compute: %s\n", error.c_str());
            return npucompute::cli::kReportErrorExitCode;
        }
        npucompute::cli::ImportOutputDirectory outputDirectory;
        if (!npucompute::cli::ImportOutputDirectory::Create(
                *config.import_path, config.export_path, &outputDirectory, &error) ||
            !npucompute::cli::UnpackImportedProfileResults(results, outputDirectory.TemporaryPath(), &error) ||
            !outputDirectory.Publish(&error)) {
            std::fprintf(stderr, "[ERROR] npu-compute: %s\n", error.c_str());
            return npucompute::cli::kReportErrorExitCode;
        }
        std::fprintf(stderr, "npu-compute: unpacked= %s\n", outputDirectory.FinalPath().c_str());
        return 0;
    }

    std::string report_path;
    std::string error;
    int result = npucompute::cli::LaunchTarget(config, &report_path, &error);
    if (result != 0) {
        if (!report_path.empty()) {
            std::fprintf(stderr, "npu-compute: %s= %s\n", config.no_pack ? "directory" : "report", report_path.c_str());
        }
        if (!error.empty()) {
            std::fprintf(stderr, "[ERROR] npu-compute: %s\n", error.c_str());
        }
        return result;
    }
    if (!report_path.empty()) {
        std::fprintf(stderr, "npu-compute: %s= %s\n", config.no_pack ? "directory" : "report", report_path.c_str());
    }
    return 0;
}
