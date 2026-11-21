/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "config/config.h"
#include "runtime/section_config.h"
#include "ipc/ipc.h"
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <string>
namespace {
void Check(bool v)
{
    if (!v)
        throw std::runtime_error("Communication config expectation failed");
}
bool Parse(std::vector<std::string> args, npucompute::cli::CliConfig& config)
{
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    std::vector<std::string> errors;
    return npucompute::cli::ParseCli(static_cast<int>(argv.size()), argv.data(), &config, &errors);
}
} // namespace
int main()
{
    using npucompute::cli::CliConfig;
    CliConfig config;
    Check(Parse({"npu-compute", "--replay-mode", "application", "--section", "Communication", "demo"}, config));
    Check(config.sections == std::vector<std::string>{"Communication"});
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"npu-compute", "--communication", "demo"},
             {"npu-compute", "--section", "Communication", "--section", "Memory", "demo"},
             {"npu-compute", "--section", "Communication", "--set", "full", "demo"},
             {"npu-compute", "--section", "Communication", "--replay-mode", "kernel", "demo"},
             {"npu-compute", "--section", "Communication", "--cache-control", "all", "demo"}}) {
        CliConfig invalid;
        Check(!Parse(args, invalid));
    }
    for (const auto& set : {"basic", "full"}) {
        CliConfig normal;
        Check(Parse({"npu-compute", "--set", set, "demo"}, normal));
        Check(std::find(normal.sections.begin(), normal.sections.end(), "Communication") == normal.sections.end());
    }
    for (const char* name :
         {"PipeUtilization", "Memory", "MemoryL0", "MemoryUB", "L2Cache", "Pipeline", "ArithmeticUtilization",
          "ResourceConflictRatio", "PmSampling"}) {
        CliConfig legacy;
        Check(Parse({"npu-compute", "--section", name, "demo"}, legacy));
        npucompute::SectionConfig legacySection;
        std::string error;
        Check(legacySection.Load(legacy.sections, &error));
        npucompute::ipc::Config wire;
        wire.sections = legacy.sections;
        Check(npucompute::ipc::DecodeConfig(npucompute::ipc::EncodeConfig(wire)).sections == legacy.sections);
    }
    CliConfig duplicate;
    Check(Parse(
        {"npu-compute", "--replay-mode", "application", "--section=Communication", "--section", "Communication",
         "demo"},
        duplicate));
    Check(duplicate.sections.size() == 1);
    npucompute::SectionConfig section;
    std::string error;
    Check(section.Load({"Communication"}, &error));
    Check(
        section.Params()->numSections == 1 && !section.Params()->collectPipeline &&
        !section.Params()->collectPcSampling);
    Check(!section.Load({"Communication", "Memory"}, &error));
    namespace ipc = npucompute::ipc;
    ipc::Config wire;
    wire.sections = {"Communication"};
    wire.replayMode = "application";
    Check(ipc::DecodeConfig(ipc::EncodeConfig(wire)).sections == wire.sections);
    Check(ipc::KindForName(".communication-staging/process-uds/records-0.jsonl") == ipc::DataKind::Communication);
    Check(ipc::ArtifactTypeForName(".communication-staging/process-uds/manifest.json") == ipc::ArtifactType::Json);
    Check(ipc::ArtifactTypeForName("app_summary.jsonl") == ipc::ArtifactType::Jsonl);
    Check(ipc::TypeForKind(ipc::DataKind::Communication) == ipc::Type::Communication);
    for (const auto* path :
         {".communication-staging/../records-0.jsonl", ".communication-staging/process-uds/result-0.json",
          "/app_summary.jsonl", ".biu-staging/process-uds/records-0.jsonl"}) {
        bool rejected = false;
        try {
            (void)ipc::KindForName(path);
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected);
    }
    wire.sections.push_back("Memory");
    bool rejected = false;
    try {
        ipc::ValidateConfig(wire);
    } catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected);
}
