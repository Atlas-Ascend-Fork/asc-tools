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
#include <cstdlib>
#include <stdexcept>
#include <iostream>

namespace {
void Check(bool value)
{
    if (!value) {
        throw std::runtime_error("PmSampling config check failed");
    }
}
npucompute::cli::CliConfig Parse(std::vector<std::string> args)
{
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    npucompute::cli::CliConfig config;
    std::vector<std::string> errors;
    Check(npucompute::cli::ParseCli(argv.size(), argv.data(), &config, &errors));
    Check(errors.empty());
    return config;
}
} // namespace

int main()
{
    namespace ipc = npucompute::ipc;
    namespace cli = npucompute::cli;
    using npucompute::SectionConfig;
    for (const std::vector<std::string>& sections :
         {std::vector<std::string>{"PmSampling"},
          {"Memory", "PmSampling"},
          {"Pipeline", "PmSampling"},
          {"Memory", "Pipeline", "PmSampling"}}) {
        std::vector<std::string> args{"npu-compute"};
        for (const auto& section : sections) {
            args.push_back("--section");
            args.push_back(section);
        }
        args.push_back("demo");
        auto cli = Parse(args);
        Check(cli.sections == sections);
        ipc::Config wire{};
        wire.sections = cli.sections;
        auto decoded = ipc::DecodeConfig(ipc::EncodeConfig(wire));
        Check(decoded.sections == sections);
        std::vector<std::string> invalidArgs{"npu-compute", "--section", "PcSampling", "demo"};
        std::vector<char*> invalidArgv;
        for (auto& arg : invalidArgs) {
            invalidArgv.push_back(arg.data());
        }
        cli::CliConfig invalidConfig;
        std::vector<std::string> errors;
        Check(!cli::ParseCli(invalidArgv.size(), invalidArgv.data(), &invalidConfig, &errors));
        Check(!errors.empty());
        SectionConfig runtime;
        std::string error;
        Check(runtime.Load(decoded.sections, &error));
        auto* params = runtime.Params();
        Check(params->collectPcSampling);
        const bool memory = sections.front() == "Memory";
        Check(params->numSections == (memory ? 1 : 0));
        if (memory)
            Check(std::string(params->sections[0]) == "Memory");
        Check(runtime.JoinedSections().find("PmSampling") != std::string::npos);
        Check(params->collectPipeline == (std::find(sections.begin(), sections.end(), "Pipeline") != sections.end()));
    }
    for (const auto& set : {"basic", "full"}) {
        auto cli = Parse({"npu-compute", "--set", set, "demo"});
        const bool full = std::string(set) == "full";
        Check((std::find(cli.sections.begin(), cli.sections.end(), "PmSampling") != cli.sections.end()) == full);
        Check(std::find(cli.sections.begin(), cli.sections.end(), "Pipeline") != cli.sections.end());
        Check(cli.sections.size() == (full ? 9 : 7));
    }
    std::vector<std::string> invalidArgs{"npu-compute", "--section", "PcSampling", "demo"};
    std::vector<char*> invalidArgv;
    for (auto& arg : invalidArgs) {
        invalidArgv.push_back(arg.data());
    }
    cli::CliConfig invalidConfig;
    std::vector<std::string> errors;
    Check(!cli::ParseCli(invalidArgv.size(), invalidArgv.data(), &invalidConfig, &errors));
    Check(!errors.empty());
    SectionConfig runtime;
    std::string error;
    Check(!runtime.Load({"PcSampling"}, &error));
    Check(runtime.Load({"PmSampling"}, &error));
    Check(runtime.Load({"Memory"}, &error));
    Check(!runtime.Params()->collectPcSampling);
    Check(runtime.Load({"Memory", "PmSampling"}, &error));
    Check(runtime.Params()->numSections == 1 && runtime.Params()->collectPcSampling);
    ipc::Config all{};
    all.sections = {
        "PipeUtilization",       "Memory",    "MemoryL0", "MemoryUB", "L2Cache", "Pipeline", "ArithmeticUtilization",
        "ResourceConflictRatio", "PmSampling"};
    Check(ipc::DecodeConfig(ipc::EncodeConfig(all)).sections == all.sections);
    std::cout << "PASS: CLI/runtime/IPC combinations; 9 sections; defaults; unknown name; reset\n";
}
