/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "config/config.h"
#include "launch/launcher.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

int Check(bool condition, const char* expression, int line)
{
    if (condition) {
        return 0;
    }
    ADD_FAILURE_AT(__FILE__, line) << expression;
    return 1;
}

#define CHECK(expression)                               \
    do {                                                \
        if (Check((expression), #expression, __LINE__)) \
            return 1;                                   \
    } while (false)

using npucompute::cli::CliConfig;
using npucompute::cli::ParseCli;
using npucompute::cli::PrintUsage;

bool Parse(const std::vector<std::string>& arguments, CliConfig* config, std::vector<std::string>* errors)
{
    std::vector<std::string> storage = arguments;
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& argument : storage) {
        argv.push_back(argument.data());
    }
    return ParseCli(static_cast<int>(argv.size()), argv.data(), config, errors);
}

std::string ReadStream(FILE* stream)
{
    std::string content;
    std::rewind(stream);
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), stream) != nullptr) {
        content += buffer;
    }
    return content;
}

int TestCollectionExport()
{
    CliConfig config;
    std::vector<std::string> errors;
    CHECK(Parse(
        {"npu-compute", "--section", "PipeUtilization", "-o", "result.npu-rep", "./app", "--app-value"}, &config,
        &errors));
    CHECK(errors.empty());
    CHECK(config.export_path == "result.npu-rep");
    CHECK(config.program == "./app");
    CHECK(config.program_arguments == std::vector<std::string>{"--app-value"});

    CHECK(Parse({"npu-compute", "--section", "Memory", "--export", "reports", "./app"}, &config, &errors));
    CHECK(config.export_path == "reports");
    CHECK(Parse({"npu-compute", "--section", "Memory", "./app"}, &config, &errors));
    CHECK(!config.export_path);
    CHECK(!config.import_path);
    CHECK(!Parse({"npu-compute", "--section", "Memory", "--export", "", "./app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>({"--export requires a non-empty output path."}));
    CHECK(!config.export_path);
    return 0;
}

int TestPipelineOption()
{
    CliConfig config;
    std::vector<std::string> errors;
    CHECK(Parse({"npu-compute", "--section", "Pipeline", "./app"}, &config, &errors));
    CHECK(errors.empty());
    CHECK(config.collect_pipeline);
    CHECK(config.sections == std::vector<std::string>{"Pipeline"});
    CHECK(Parse({"npu-compute", "--section=Pipeline", "--section", "Memory", "./app"}, &config, &errors));
    CHECK(config.collect_pipeline);
    CHECK(Parse(
        {"npu-compute", "--section", "Pipeline", "--section", "ArithmeticUtilization", "--section",
         "ResourceConflictRatio", "./app"},
        &config, &errors));
    CHECK(config.collect_pipeline);
    CHECK(config.sections == std::vector<std::string>({"Pipeline", "ArithmeticUtilization", "ResourceConflictRatio"}));
    CHECK(!Parse({"npu-compute", "--pipeline", "--section", "Memory", "./app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>({"unknown option '--pipeline'. Use --help to see supported options."}));
    return 0;
}

int TestBusinessExitCodes()
{
    CHECK(npucompute::cli::kUsageErrorExitCode == 2);
    CHECK(npucompute::cli::kCollectionErrorExitCode == 3);
    CHECK(npucompute::cli::kReportErrorExitCode == 4);
    CHECK(npucompute::cli::kInternalErrorExitCode == 5);
    return 0;
}

int TestImportExportParsing()
{
    CliConfig config;
    std::vector<std::string> errors;
    CHECK(Parse({"npu-compute", "--import", "old.npu-rep", "--export", "new.npu-rep"}, &config, &errors));
    CHECK(config.import_path == "old.npu-rep");
    CHECK(config.export_path == "new.npu-rep");
    CHECK(config.program.empty());

    CHECK(Parse({"npu-compute", "-i", "old.npu-rep", "-o", "new.npu-rep"}, &config, &errors));
    CHECK(config.import_path == "old.npu-rep");
    CHECK(config.export_path == "new.npu-rep");

    CHECK(Parse({"npu-compute", "-iold.npu-rep", "-onew.npu-rep"}, &config, &errors));
    CHECK(config.import_path == "old.npu-rep");
    CHECK(config.export_path == "new.npu-rep");
    CHECK(Parse({"npu-compute", "--import", "old.npu-rep"}, &config, &errors));
    CHECK(config.import_path == "old.npu-rep");
    CHECK(!config.export_path);
    CHECK(!Parse({"npu-compute", "--import", ""}, &config, &errors));
    CHECK(errors == std::vector<std::string>({"--import expects a report file, but no file was specified."}));
    CHECK(!config.import_path);
    return 0;
}

int TestInlineLongOptionValues()
{
    CliConfig config;
    std::vector<std::string> errors;
    CHECK(Parse(
        {"npu-compute", "--section=Memory", "--replay-mode=kernel", "--export=result.npu-rep", "./app"}, &config,
        &errors));
    CHECK(errors.empty());
    CHECK(config.sections == std::vector<std::string>({"Memory"}));
    CHECK(config.replay_mode_specified);
    CHECK(config.export_path == "result.npu-rep");
    CHECK(config.program == "./app");
    return 0;
}

int TestForceOptionsAreRejected()
{
    CliConfig config;
    std::vector<std::string> errors;
    CHECK(!Parse({"npu-compute", "--section", "Memory", "-f", "./app"}, &config, &errors));
    CHECK(!errors.empty());
    errors.clear();
    CHECK(!Parse(
        {"npu-compute", "--section", "Memory", "-o", "result.npu-rep", "--force-overwrite", "./app"}, &config,
        &errors));
    CHECK(!errors.empty());
    return 0;
}

int TestExistingCliBehavior()
{
    CliConfig config;
    std::vector<std::string> errors;
    const bool parsed = Parse(
        {"npu-compute",
         "--section",
         "PipeUtilization",
         "--section",
         "Memory",
         "--section",
         "MemoryL0",
         "--section",
         "MemoryUB",
         "--section",
         "L2Cache",
         "--section",
         "ArithmeticUtilization",
         "--section",
         "ResourceConflictRatio",
         "--section",
         "ArithmeticUtilization",
         "./app",
         "--export",
         "app-owned",
         "--force-overwrite"},
        &config, &errors);
    if (!parsed) {
        for (const std::string& error : errors) {
            std::fprintf(stderr, "APP argv parse error: %s\n", error.c_str());
        }
    }
    CHECK(parsed);
    CHECK(
        config.sections == std::vector<std::string>(
                               {"PipeUtilization", "Memory", "MemoryL0", "MemoryUB", "L2Cache", "ArithmeticUtilization",
                                "ResourceConflictRatio"}));
    CHECK(!config.export_path.has_value());
    CHECK(config.program_arguments == std::vector<std::string>({"--export", "app-owned", "--force-overwrite"}));
    return 0;
}

int TestHelpWithoutErrors()
{
    const std::vector<std::vector<std::string>> help_arguments = {
        {"npu-compute", "-h"},
        {"npu-compute", "--help"},
        {"npu-compute", "-h", "--help"},
        {"npu-compute", "--section", "Memory", "--help", "--export", "result.npu-rep"},
    };
    for (const auto& arguments : help_arguments) {
        CliConfig config;
        std::vector<std::string> errors;
        CHECK(Parse(arguments, &config, &errors));
        CHECK(errors.empty());
        CHECK(config.show_help);
        CHECK(config.program.empty());
        CHECK(config.program_arguments.empty());
    }
    return 0;
}

int TestHelpReportsAllOptionErrors()
{
    CliConfig config;
    std::vector<std::string> errors;

    CHECK(!Parse({"npu-compute", "--bad-option", "--help"}, &config, &errors));
    CHECK(errors == std::vector<std::string>({"unknown option '--bad-option'. Use --help to see supported options."}));
    CHECK(config.show_help);

    errors.clear();
    CHECK(!Parse({"npu-compute", "--section", "Invalid", "--help"}, &config, &errors));
    CHECK(
        errors == std::vector<std::string>({"unsupported section name 'Invalid'. Names are case-sensitive; use "
                                            "--list-sections to see supported names."}));
    CHECK(config.show_help);

    errors.clear();
    CHECK(!Parse({"npu-compute", "--section", "--help"}, &config, &errors));
    CHECK(
        errors ==
        std::vector<std::string>({"--section requires a section name. Use --list-sections to see supported names."}));
    CHECK(config.show_help);

    errors.clear();
    CHECK(!Parse({"npu-compute", "--bad-one", "--bad-two", "--help"}, &config, &errors));
    CHECK(
        errors == std::vector<std::string>(
                      {"unknown option '--bad-one'. Use --help to see supported options.",
                       "unknown option '--bad-two'. Use --help to see supported options."}));
    CHECK(config.show_help);

    errors.clear();
    CHECK(!Parse({"npu-compute", "--bad-one", "--bad-two"}, &config, &errors));
    CHECK(
        errors == std::vector<std::string>(
                      {"unknown option '--bad-one'. Use --help to see supported options.",
                       "unknown option '--bad-two'. Use --help to see supported options."}));
    CHECK(!config.show_help);

    errors.clear();
    CHECK(Parse({"npu-compute", "hh", "-h", "/path/to/run.sh"}, &config, &errors));
    CHECK(errors.empty());
    CHECK(config.sets == std::vector<std::string>({"basic"}));
    CHECK(config.program == "hh");
    CHECK(config.program_arguments == std::vector<std::string>({"-h", "/path/to/run.sh"}));

    errors.clear();
    CHECK(!Parse({"npu-compute", "--section", "Invalid", "--replay-mode", "invalid", "--help"}, &config, &errors));
    CHECK(
        errors == std::vector<std::string>(
                      {"unsupported section name 'Invalid'. Names are case-sensitive; use --list-sections to see "
                       "supported names.",
                       "unsupported replay mode 'invalid'. Supported value: kernel."}));
    CHECK(config.show_help);
    return 0;
}

int TestHelpAcceptsListSectionsCombination()
{
    const std::vector<std::vector<std::string>> arguments = {
        {"npu-compute", "--list-sections", "--help"},
        {"npu-compute", "--help", "--list-sections"},
    };
    for (const auto& argument : arguments) {
        CliConfig config;
        std::vector<std::string> errors;
        CHECK(Parse(argument, &config, &errors));
        CHECK(errors.empty());
        CHECK(config.show_help);
        CHECK(config.list_sections);
    }
    return 0;
}

int TestHelpMatchingAndProgramBoundary()
{
    CliConfig config;
    std::vector<std::string> errors;
    CHECK(!Parse({"npu-compute", "--help=value"}, &config, &errors));
    CHECK(!errors.empty());

    errors.clear();
    CHECK(!Parse({"npu-compute", "-hh"}, &config, &errors));
    CHECK(!errors.empty());

    errors.clear();
    CHECK(Parse({"npu-compute", "--section", "Memory", "./app", "--bad-option", "--help", "-h"}, &config, &errors));
    CHECK(errors.empty());
    CHECK(!config.show_help);
    CHECK(config.program == "./app");
    CHECK(config.program_arguments == std::vector<std::string>({"--bad-option", "--help", "-h"}));
    return 0;
}

int TestMissingValueAndDuplicateState()
{
    CliConfig config;
    std::vector<std::string> errors;
    for (const std::string option : {"--import", "--export"}) {
        CHECK(!Parse({"npu-compute", option, "--help", option, "file.npu-rep"}, &config, &errors));
        CHECK(errors.size() == 2);
        CHECK(errors[1] == option + " may only be specified once");
        CHECK(!config.import_path && !config.export_path);
    }
    CHECK(!Parse({"npu-compute", "--replay-mode", "--help"}, &config, &errors));
    CHECK(config.replay_mode_specified);
    CHECK(errors == std::vector<std::string>({"--replay-mode requires a mode. Supported value: kernel."}));
    CHECK(!Parse({"npu-compute", "--replay-mode", "--help", "--replay-mode", "kernel"}, &config, &errors));
    CHECK(config.replay_mode_specified);
    CHECK(errors.size() == 2);
    CHECK(errors[1] == "--replay-mode may only be specified once");
    CHECK(!Parse({"npu-compute", "--replay-mode=", "--replay-mode", "kernel"}, &config, &errors));
    CHECK(errors.size() == 2);
    CHECK(errors[1] == "--replay-mode may only be specified once");
    return 0;
}

int TestNullArgumentsAndStateReset()
{
    CliConfig config;
    std::vector<std::string> errors;
    char name[] = "npu-compute";
    char section[] = "--section";
    char memory[] = "Memory";
    char help[] = "--help";
    char* missing_value[] = {name, section, nullptr};
    CHECK(!ParseCli(3, missing_value, &config, &errors));
    CHECK(
        errors ==
        std::vector<std::string>({"--section requires a section name. Use --list-sections to see supported names."}));
    char* empty_program[] = {name, section, memory, nullptr, help};
    CHECK(!ParseCli(5, empty_program, &config, &errors));
    CHECK(!config.show_help);
    CHECK(config.program.empty());
    CHECK(config.program_arguments == std::vector<std::string>({"--help"}));
    CHECK(Parse(
        {"npu-compute", "--section", "Memory", "--section", "L2Cache", "--section", "Memory", "app"}, &config,
        &errors));
    CHECK(config.sections == std::vector<std::string>({"Memory", "L2Cache"}));
    CHECK(Parse({"npu-compute", "--help"}, &config, &errors));
    CHECK(config.show_help && errors.empty());
    CHECK(config.sections.empty() && config.program.empty() && config.program_arguments.empty());
    CHECK(!config.import_path && !config.export_path && !config.replay_mode_specified && !config.list_sections);
    return 0;
}

int TestSets()
{
    CliConfig config;
    std::vector<std::string> errors;
    const std::vector<std::string> basic = {"Pipeline", "PipeUtilization",      "Memory", "MemoryL0", "MemoryUB",
                                            "L2Cache",  "ArithmeticUtilization"};
    auto full = basic;
    full.push_back("ResourceConflictRatio");
    full.push_back("PmSampling");
    CHECK(Parse({"npu-compute", "--set", "basic", "./app"}, &config, &errors));
    CHECK(config.sections == basic && config.collect_pipeline);
    CHECK(Parse({"npu-compute", "./app"}, &config, &errors));
    CHECK(config.sets == std::vector<std::string>({"basic"}));
    CHECK(config.sections == basic && config.collect_pipeline);
    CHECK(Parse({"npu-compute", "--set=full", "--set", "basic", "--section", "Memory", "./app"}, &config, &errors));
    CHECK(config.sections == full);
    auto reordered = basic;
    reordered.erase(reordered.begin() + 2);
    reordered.insert(reordered.begin(), "Memory");
    CHECK(Parse({"npu-compute", "--section", "Memory", "--set", "basic", "./app"}, &config, &errors));
    CHECK(config.sections == reordered);
    CHECK(Parse(
        {"npu-compute", "--set", "basic", "--section", "ResourceConflictRatio", "--section", "PmSampling",
         "--replay-mode=kernel", "-o", "x.npu-rep", "./app", "--set", "full"},
        &config, &errors));
    CHECK(config.sections == full);
    CHECK(config.program_arguments == std::vector<std::string>({"--set", "full"}));
    CHECK(Parse({"npu-compute", "--set", "full", "--", "--app", "--set", "basic"}, &config, &errors));
    CHECK(config.sections == full && config.program == "--app");
    CHECK(config.program_arguments == std::vector<std::string>({"--set", "basic"}));
    CHECK(Parse({"npu-compute", "--", "--app"}, &config, &errors));
    CHECK(config.sections == basic && config.program == "--app");
    CHECK(!Parse({"npu-compute", "--set", "--", "--app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"--set requires a set name. Use --list-sets to see supported names."});
    CHECK(config.program == "--app" && config.sections.empty());
    CHECK(Parse({"npu-compute", "--list-sets", "--"}, &config, &errors));
    CHECK(config.list_sets && config.program.empty());
    CHECK(!Parse({"npu-compute", "--list-sets", "--", "--app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"use --list-sets as a standalone command."});
    CHECK(Parse({"npu-compute", "--list-sets"}, &config, &errors));
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--set"},
             {"--set="},
             {"--set", ""},
             {"--set", "Basic"},
             {"--set", "basic,full"},
             {"--set", "basic"},
             {"--set", "basic", "--import", "x.npu-rep"},
             {"--list-sets", "--list-sets"},
             {"--list-sets=basic"},
             {"--list-sets", "--list-sections"},
             {"--list-sets", "--set", "basic"},
             {"--list-sets", "--section", "Memory"},
             {"--list-sets", "--replay-mode", "kernel"},
             {"--list-sets", "-o", "x.npu-rep"},
             {"--list-sets", "--import", "x.npu-rep"},
             {"--list-sets", "./app"}}) {
        auto command = args;
        command.insert(command.begin(), "npu-compute");
        CHECK(!Parse(command, &config, &errors));
        CHECK(!errors.empty());
    }
    CHECK(Parse({"npu-compute", "-h", "--set", "basic"}, &config, &errors));
    CHECK(Parse({"npu-compute", "--list-sets", "--help"}, &config, &errors));
    CHECK(!Parse({"npu-compute", "--help", "--set", "invalid"}, &config, &errors));
    return 0;
}

int TestSeparatorBoundary()
{
    CliConfig config;
    std::vector<std::string> errors;
    const std::vector<std::string> app_arguments = {"--help", "-h", "--section", "Pipeline", "--export",
                                                    "x",      "--", "",          "two words"};
    for (const std::string program : {"./app", "--app", "-app", "--help", "-h", "--", "-"}) {
        std::vector<std::string> arguments = {"npu-compute", "--section", "Memory", "--", program};
        arguments.insert(arguments.end(), app_arguments.begin(), app_arguments.end());
        CHECK(Parse(arguments, &config, &errors));
        CHECK(errors.empty());
        CHECK(config.program == program);
        CHECK(config.program_arguments == app_arguments);
        CHECK(config.sections == std::vector<std::string>{"Memory"});
        CHECK(!config.show_help && !config.list_sections && !config.collect_pipeline);
        CHECK(!config.export_path && !config.import_path && !config.replay_mode_specified);
    }
    for (bool separator : {false, true}) {
        std::vector<std::string> arguments = {
            "npu-compute", "--section", "Memory", "--export=result.npu-rep", "--replay-mode=kernel"};
        if (separator) {
            arguments.push_back("--");
        }
        arguments.push_back("./app");
        arguments.insert(arguments.end(), app_arguments.begin(), app_arguments.end());
        CHECK(Parse(arguments, &config, &errors));
        CHECK(errors.empty());
        CHECK(config.program == "./app" && config.program_arguments == app_arguments);
        CHECK(config.sections == std::vector<std::string>{"Memory"});
        CHECK(config.export_path == "result.npu-rep" && !config.import_path);
        CHECK(config.replay_mode_specified && config.replay_mode == npucompute::cli::ReplayMode::Kernel);
        CHECK(!config.show_help && !config.list_sections && !config.collect_pipeline);
    }
    CHECK(Parse({"npu-compute", "--help"}, &config, &errors));
    CHECK(config.show_help && config.program.empty() && config.program_arguments.empty());
    CHECK(config.sections.empty() && !config.export_path && !config.replay_mode_specified);
    return 0;
}

int TestSeparatorMissingValues()
{
    struct MissingValueCase {
        const char* option;
        const char* error;
    };
    const MissingValueCase cases[] = {
        {"--section", "--section requires a section name. Use --list-sections to see supported names."},
        {"--replay-mode", "--replay-mode requires a mode. Supported value: kernel."},
        {"--import", "--import expects a report file, but no file was provided."},
        {"-i", "--import expects a report file, but no file was provided."},
        {"--export", "--export requires an output path: a report file or directory."},
        {"-o", "--export requires an output path: a report file or directory."},
    };
    for (const auto& test : cases) {
        CliConfig config;
        std::vector<std::string> errors;
        CHECK(!Parse({"npu-compute", test.option, "--", "--app", "--help"}, &config, &errors));
        CHECK(errors == std::vector<std::string>{test.error});
        CHECK(config.program == "--app");
        CHECK(config.program_arguments == std::vector<std::string>{"--help"});
        CHECK(!config.show_help && !config.import_path && !config.export_path && config.sections.empty());
        CHECK(config.replay_mode_specified == (std::string(test.option) == "--replay-mode"));
        CHECK(!Parse({"npu-compute", test.option, "--"}, &config, &errors));
        CHECK(errors == std::vector<std::string>{test.error});
        CHECK(config.program.empty() && config.program_arguments.empty());
    }
    return 0;
}

int TestSeparatorValidation()
{
    CliConfig config;
    std::vector<std::string> errors;
    for (const std::vector<std::string> arguments :
         {std::vector<std::string>{"npu-compute", "--section", "Memory", "--"},
          std::vector<std::string>{"npu-compute", "--section", "Memory", "--", "", "--help"}}) {
        CHECK(!Parse(arguments, &config, &errors));
        CHECK(errors == std::vector<std::string>{"collection requires a target program after the tool options."});
        CHECK(!config.show_help && config.program.empty());
    }
    CHECK(config.program_arguments == std::vector<std::string>{"--help"});
    CHECK(!Parse({"npu-compute", "--import", "report.npu-rep", "--", "./app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"Use --import alone or with --export as the only additional option."});
    CHECK(!Parse({"npu-compute", "--list-sections", "--", "./app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"use --list-sections as a standalone command."});
    CHECK(Parse({"npu-compute", "--list-sections", "--"}, &config, &errors));
    CHECK(config.list_sections && config.program.empty());
    CHECK(Parse({"npu-compute", "--help", "--"}, &config, &errors));
    CHECK(config.show_help && config.program.empty());
    CHECK(Parse({"npu-compute", "--import", "report.npu-rep", "--"}, &config, &errors));
    CHECK(config.import_path == "report.npu-rep" && config.program.empty());
    for (const std::string option : {"--bad", "---"}) {
        CHECK(!Parse({"npu-compute", option, "--", "./app", "--help"}, &config, &errors));
        CHECK(
            errors ==
            std::vector<std::string>{"unknown option '" + option + "'. Use --help to see supported options."});
        CHECK(config.program == "./app" && !config.show_help);
        CHECK(config.program_arguments == std::vector<std::string>{"--help"});
    }
    CHECK(!Parse({"npu-compute", "--bad", "--help", "--", "./app"}, &config, &errors));
    CHECK(config.show_help && errors.size() == 1 && config.program == "./app");
    return 0;
}

int TestSeparatorLiteralValues()
{
    CliConfig config;
    std::vector<std::string> errors;
    for (const std::string option : {"--export=--", "-o--"}) {
        CHECK(Parse({"npu-compute", "--section", "Memory", option, "--", "./app"}, &config, &errors));
        CHECK(config.export_path == "--" && config.program == "./app");
    }
    for (const std::string option : {"--import=--", "-i--"}) {
        CHECK(Parse({"npu-compute", option, "--"}, &config, &errors));
        CHECK(config.import_path == "--" && config.program.empty());
    }
    CHECK(Parse({"npu-compute", "--section", "Memory", "--export", "./--", "--", "./app"}, &config, &errors));
    CHECK(config.export_path == "./--");
    CHECK(!Parse({"npu-compute", "--section=--", "--", "./app"}, &config, &errors));
    CHECK(
        errors ==
        std::vector<std::string>{
            "unsupported section name '--'. Names are case-sensitive; use --list-sections to see supported names."});
    CHECK(!Parse({"npu-compute", "--replay-mode=--", "--", "./app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"unsupported replay mode '--'. Supported value: kernel."});
    return 0;
}

int TestCombinationValidationOrder()
{
    CliConfig config;
    std::vector<std::string> errors;
    CHECK(!Parse({"npu-compute", "--export", "result.npu-rep"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"--export requires a collection command or --import."});
    CHECK(!Parse({"npu-compute"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"collection requires a target program after the tool options."});
    CHECK(!Parse({"npu-compute", "--section", "Memory"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"collection requires a target program after the tool options."});
    CHECK(!Parse({"npu-compute", "--list-sections", "--import", "report.npu-rep", "./app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"Use --import alone or with --export as the only additional option."});
    CHECK(!Parse({"npu-compute", "--import", "report.npu-rep", "./app"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"Use --import alone or with --export as the only additional option."});
    CHECK(Parse({"npu-compute", "--import", "report.npu-rep"}, &config, &errors));
    CHECK(errors.empty());
    CHECK(Parse({"npu-compute", "--help", "--export", "result.npu-rep"}, &config, &errors));
    CHECK(errors.empty());
    CHECK(!Parse({"npu-compute", "--bad", "--export", "result.npu-rep"}, &config, &errors));
    CHECK(errors == std::vector<std::string>{"unknown option '--bad'. Use --help to see supported options."});
    return 0;
}

int TestHelpText()
{
    FILE* stream = std::tmpfile();
    CHECK(stream != nullptr);
    PrintUsage(stream, "npu-compute");
    CHECK(std::fflush(stream) == 0);
    const std::string usage = ReadStream(stream);
    CHECK(std::fclose(stream) == 0);
    CHECK(usage.find("[options] [--] [program] [program-arguments]") != std::string::npos);
    CHECK(usage.find("Optional '--' separates") == std::string::npos);
    CHECK(usage.find("Optional separator between npu-compute options and the program.") != std::string::npos);
    CHECK(usage.find("Required if the program name starts with '-'.") != std::string::npos);
    CHECK(usage.find("force-overwrite") == std::string::npos);
    CHECK(usage.find("-o, --export") != std::string::npos);
    CHECK(usage.find("-i, --import") != std::string::npos);
    return 0;
}

} // namespace

static int RunSuiteMain()
{
    if (TestSets() != 0 || TestCombinationValidationOrder() != 0 || TestSeparatorBoundary() != 0 ||
        TestSeparatorMissingValues() != 0 || TestSeparatorValidation() != 0 || TestSeparatorLiteralValues() != 0 ||
        TestBusinessExitCodes() != 0 || TestCollectionExport() != 0 || TestPipelineOption() != 0 ||
        TestImportExportParsing() != 0 || TestInlineLongOptionValues() != 0 || TestForceOptionsAreRejected() != 0 ||
        TestExistingCliBehavior() != 0 || TestHelpWithoutErrors() != 0 || TestHelpReportsAllOptionErrors() != 0 ||
        TestHelpAcceptsListSectionsCombination() != 0 || TestHelpMatchingAndProgramBoundary() != 0 ||
        TestMissingValueAndDuplicateState() != 0 || TestNullArgumentsAndStateReset() != 0 || TestHelpText() != 0) {
        return 1;
    }
    return 0;
}

TEST(NpuComputeCli, Main) { ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeCli reported failure"; }

TEST(NpuComputeCli, HelpDefinesEachOptionOnce)
{
    FILE* stream = std::tmpfile();
    ASSERT_NE(stream, nullptr);
    PrintUsage(stream, "npu-compute");
    ASSERT_EQ(std::fflush(stream), 0);
    const std::string usage = ReadStream(stream);
    EXPECT_EQ(std::fclose(stream), 0);
    for (const char* option :
         {"--help", "--list-sections", "--list-sets", "--set", "--section", "--replay-mode", "--import", "--export"}) {
        std::istringstream lines(usage);
        std::string line;
        unsigned definitions = 0;
        while (std::getline(lines, line)) {
            const auto start = line.find_first_not_of(' ');
            if (start != 2 && start != 6) {
                continue;
            }
            std::istringstream words(line);
            std::string name;
            words >> name;
            if (name.size() == 3 && name.front() == '-' && name.back() == ',') {
                words >> name;
            }
            if (name == option) {
                ++definitions;
            }
        }
        EXPECT_EQ(definitions, 1U) << option;
    }
    EXPECT_EQ(usage.find("No section is selected by default"), std::string::npos);
    EXPECT_EQ(usage.find("Required for collection."), std::string::npos);
    EXPECT_EQ(usage.find("file path"), std::string::npos);
    EXPECT_NE(usage.find("Defaults to basic when no set or section is given."), std::string::npos);
    EXPECT_NE(usage.find("Import and unpack a report generated by npu-compute."), std::string::npos);
    EXPECT_NE(usage.find("Use --import alone or with --export as the only additional option."), std::string::npos);
    EXPECT_NE(usage.find("--export specifies the directory where the report is unpacked."), std::string::npos);
    EXPECT_NE(usage.find("file or directory"), std::string::npos);

    CliConfig config;
    std::vector<std::string> errors;
    ASSERT_TRUE(Parse({"npu-compute", "./app"}, &config, &errors));
    EXPECT_EQ(config.sets, std::vector<std::string>{"basic"});
    EXPECT_FALSE(config.sections.empty());
}

TEST(NpuComputeCli, ImportCombinationsUseConsistentGuidance)
{
    CliConfig config;
    std::vector<std::string> errors;
    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"npu-compute", "--import", "report.npu-rep", "--list-sets"},
             {"npu-compute", "--import", "report.npu-rep", "--list-sections"},
             {"npu-compute", "--import", "report.npu-rep", "--set", "basic"},
             {"npu-compute", "--import", "report.npu-rep", "--section", "Memory"},
             {"npu-compute", "--import", "report.npu-rep", "--replay-mode", "kernel"},
             {"npu-compute", "--list-sets", "--import", "report.npu-rep"},
             {"npu-compute", "--import", "report.npu-rep", "--export", "restored", "--list-sets"}}) {
        EXPECT_FALSE(Parse(arguments, &config, &errors));
        EXPECT_EQ(
            errors, std::vector<std::string>{"Use --import alone or with --export as the only additional option."});
    }
    EXPECT_TRUE(Parse({"npu-compute", "--import", "report.npu-rep", "--help"}, &config, &errors));
    EXPECT_TRUE(errors.empty());
}

TEST(NpuComputeCli, ImportReportsMissingAndEmptyFile)
{
    CliConfig config;
    std::vector<std::string> errors;
    for (const char* option : {"--import", "-i"}) {
        EXPECT_FALSE(Parse({"npu-compute", option}, &config, &errors));
        EXPECT_EQ(errors, std::vector<std::string>{"--import expects a report file, but no file was provided."});
        EXPECT_FALSE(Parse({"npu-compute", option, ""}, &config, &errors));
        EXPECT_EQ(errors, std::vector<std::string>{"--import expects a report file, but no file was specified."});
    }
    EXPECT_FALSE(Parse({"npu-compute", "--import="}, &config, &errors));
    EXPECT_EQ(errors, std::vector<std::string>{"--import expects a report file, but no file was specified."});
}

TEST(NpuComputeCli, FlagValuesReportSpecificErrors)
{
    CliConfig config;
    std::vector<std::string> errors;
    for (const std::string option : {"--help", "--list-sets", "--list-sections"}) {
        for (const std::string suffix : {"=", "=value"}) {
            EXPECT_FALSE(Parse({"npu-compute", option + suffix, "--import"}, &config, &errors));
            EXPECT_EQ(
                errors,
                (std::vector<std::string>{
                    option + " does not take a value.", "--import expects a report file, but no file was provided."}));
            EXPECT_FALSE(config.show_help);
            EXPECT_FALSE(config.list_sets);
            EXPECT_FALSE(config.list_sections);
        }
    }
    EXPECT_FALSE(Parse({"npu-compute", "--list-sets", "--list-sets=x"}, &config, &errors));
    EXPECT_EQ(errors, std::vector<std::string>{"--list-sets does not take a value."});
    EXPECT_TRUE(Parse({"npu-compute", "app", "--help=x"}, &config, &errors));
    EXPECT_EQ(config.program_arguments, std::vector<std::string>{"--help=x"});
}

TEST(NpuComputeCli, NoPackAcceptsCollectionAndPreservesAppArgumentBoundary)
{
    CliConfig config;
    std::vector<std::string> errors;
    ASSERT_TRUE(Parse({"npu-compute", "--no-pack", "--export", "reports", "./app", "--no-pack"}, &config, &errors));
    EXPECT_EQ(config.export_path, "reports");
    EXPECT_EQ(config.program_arguments, std::vector<std::string>{"--no-pack"});
    EXPECT_EQ(config.sets, std::vector<std::string>{"basic"});
    EXPECT_TRUE(Parse({"npu-compute", "--no-pack", "--help"}, &config, &errors));
    EXPECT_TRUE(Parse({"npu-compute", "--export", "reports", "--no-pack", "./app"}, &config, &errors));
    EXPECT_EQ(config.export_path, "reports");
}

TEST(NpuComputeCli, NoPackRejectsIncompatibleModesAndValues)
{
    CliConfig config;
    std::vector<std::string> errors;
    EXPECT_FALSE(Parse({"npu-compute", "--no-pack", "--import", "old.npu-rep"}, &config, &errors));
    EXPECT_NE(errors.front().find("cannot be combined with --import"), std::string::npos);
    for (const char* option : {"--list-sections", "--list-sets"}) {
        EXPECT_FALSE(Parse({"npu-compute", "--no-pack", option}, &config, &errors));
        EXPECT_NE(errors.front().find("standalone"), std::string::npos);
    }
    EXPECT_FALSE(Parse({"npu-compute", "--no-pack", "--no-pack", "app"}, &config, &errors));
    EXPECT_NE(errors.front().find("only be specified once"), std::string::npos);
    EXPECT_FALSE(Parse({"npu-compute", "--no-pack=value", "app"}, &config, &errors));
    EXPECT_NE(errors.front().find("does not take a value"), std::string::npos);
}

TEST(NpuComputeCli, NoPackRejectsLegacyDiskOptionAndPreservesAppArguments)
{
    CliConfig config;
    std::vector<std::string> errors;
    EXPECT_FALSE(Parse({"npu-compute", "--disk", "app"}, &config, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("unknown option"), std::string::npos);
    EXPECT_TRUE(Parse({"npu-compute", "app", "--disk", "--no-pack"}, &config, &errors));
    EXPECT_EQ(config.program_arguments, (std::vector<std::string>{"--disk", "--no-pack"}));
}
