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
#include "config/option_registry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace npucompute::cli {
namespace {

constexpr std::array kSupportedSections = {
    "PipeUtilization",       "Memory",     "MemoryL0", "MemoryUB", "L2Cache", "Pipeline", "ArithmeticUtilization",
    "ResourceConflictRatio", "PmSampling",
};

void AddError(const std::string& message, std::vector<std::string>* errors) { errors->push_back(message); }

constexpr std::array<const char*, 2> kSupportedSets = {"basic", "full"};

bool AlphabeticalSectionLess(std::string_view lhs, std::string_view rhs)
{
    return std::lexicographical_compare(
        lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](unsigned char left, unsigned char right) { return std::tolower(left) < std::tolower(right); });
}

std::vector<std::string> SetSections(const std::string& name)
{
    std::vector<std::string> sections;
    // Pipeline leads each set; all other members retain registry order.
    for (const char* section : kSupportedSections) {
        if (std::string(section) == "Pipeline") {
            sections.emplace_back(section);
        }
    }
    for (const char* section : kSupportedSections) {
        if (std::string(section) != "Pipeline" && (name == "full" || (std::string(section) != "ResourceConflictRatio" &&
                                                                      std::string(section) != "PmSampling"))) {
            sections.emplace_back(section);
        }
    }
    return sections;
}

bool IsSupportedSection(const std::string& section)
{
    return std::find(kSupportedSections.begin(), kSupportedSections.end(), section) != kSupportedSections.end();
}

void AddSection(
    const std::string& section, const char* missing_message, CliConfig* config, std::vector<std::string>* errors)
{
    if (section.empty()) {
        AddError(missing_message, errors);
    } else if (!IsSupportedSection(section)) {
        const std::string hint = section.find(',') != std::string::npos ?
                                     "Specify each group separately, for example: --section Memory --section L2Cache." :
                                     "Names are case-sensitive; use --list-sections to see supported names.";
        AddError("unsupported section name '" + section + "'. " + hint, errors);
    } else if (std::find(config->sections.begin(), config->sections.end(), section) == config->sections.end()) {
        config->sections.push_back(section);
        if (section == "Pipeline") {
            config->collect_pipeline = true;
        }
    }
}

bool SkipCombinationValidation(const CliConfig& config, const std::vector<std::string>& errors)
{
    return config.show_help || !errors.empty();
}

bool ValidateListSectionsMode(const CliConfig& config, std::vector<std::string>* errors)
{
    if (!config.list_sections) {
        return false;
    }
    if (config.replay_mode_specified || config.import_path.has_value() || config.export_path.has_value() ||
        !config.sections.empty() || !config.program.empty()) {
        AddError("use --list-sections as a standalone command.", errors);
    }
    return true;
}

bool ValidateImportMode(const CliConfig& config, std::vector<std::string>* errors)
{
    if (!config.import_path.has_value()) {
        return false;
    }
    if (config.list_sets || config.list_sections || config.replay_mode_specified || !config.sections.empty() ||
        !config.program.empty()) {
        AddError("Use --import alone or with --export as the only additional option.", errors);
    }
    return true;
}

void ValidateExportRequirement(const CliConfig& config, std::vector<std::string>* errors)
{
    if (config.export_path.has_value() && config.sections.empty() && config.program.empty()) {
        AddError("--export requires a collection command or --import.", errors);
    }
}

void ValidateTargetProgram(const CliConfig& config, std::vector<std::string>* errors)
{
    if (config.program.empty()) {
        AddError("collection requires a target program after the tool options.", errors);
    }
}

void ValidateCombinations(const CliConfig& config, std::vector<std::string>* errors)
{
    if (SkipCombinationValidation(config, *errors)) {
        return;
    }
    if (ValidateImportMode(config, errors)) {
        return;
    }
    if (config.list_sets) {
        if (config.list_sections || config.replay_mode_specified || config.import_path || config.export_path ||
            !config.sections.empty() || !config.program.empty()) {
            AddError("use --list-sets as a standalone command.", errors);
        }
        return;
    }
    if (ValidateListSectionsMode(config, errors)) {
        return;
    }
    ValidateExportRequirement(config, errors);
    if (!errors->empty()) {
        return;
    }
    ValidateTargetProgram(config, errors);
}

OptionSpec FlagOption(std::string name, char alias, bool CliConfig::*field, RepeatPolicy repeat)
{
    OptionSpec option;
    option.long_name = std::move(name);
    option.short_name = alias;
    option.repeat = repeat;
    option.apply = [field](const std::string&, CliConfig& config, std::vector<std::string>&) { config.*field = true; };
    return option;
}

OptionRegistry RegisterOptions()
{
    OptionRegistryBuilder builder;
    auto help = FlagOption("--help", 'h', &CliConfig::show_help, RepeatPolicy::Allow);
    help.protect_from_value = true;
    help.help = "Show help information.";
    builder.Add(std::move(help));

    OptionSpec section;
    section.long_name = "--section";
    section.arity = OptionArity::RequiredValue;
    section.missing_message = "--section requires a section name. Use --list-sections to see supported names.";
    section.apply = [missing = section.missing_message](
                        const std::string& value, CliConfig& config, std::vector<std::string>& errors) {
        AddSection(value, missing.c_str(), &config, &errors);
    };
    section.help = "Select a metric group by name (case-sensitive).\n"
                   "Use --list-sections to see supported names.\n"
                   "May be combined with --set.\n"
                   "Defaults to basic when no set or section is given.\n"
                   "Specify different groups separately:\n"
                   "  --section Memory --section L2Cache";
    builder.Add(std::move(section));

    auto list = FlagOption("--list-sections", '\0', &CliConfig::list_sections, RepeatPolicy::Once);
    list.help = "List supported section names.";
    builder.Add(std::move(list));

    OptionSpec set;
    set.long_name = "--set";
    set.arity = OptionArity::RequiredValue;
    set.missing_message = "--set requires a set name. Use --list-sets to see supported names.";
    set.apply = [missing = set.missing_message](
                    const std::string& value, CliConfig& config, std::vector<std::string>& errors) {
        if (value.empty()) {
            AddError(missing.c_str(), &errors);
            return;
        }
        if (std::find(kSupportedSets.begin(), kSupportedSets.end(), value) == kSupportedSets.end()) {
            const std::string hint = value.find(',') != std::string::npos ?
                                         "Specify each set separately, for example: --set basic --set full." :
                                         "Names are case-sensitive; use --list-sets to see supported names.";
            AddError("unsupported set name '" + value + "'. " + hint, &errors);
            return;
        }
        auto& sets = config.sets;
        if (std::find(sets.begin(), sets.end(), value) != sets.end()) {
            return;
        }
        sets.push_back(value);
        for (const auto& section : SetSections(value)) {
            AddSection(section, missing.c_str(), &config, &errors);
        }
    };
    set.help = "Select a predefined section set: basic or full.\n"
               "May be repeated or combined with --section.\n"
               "Sections are deduplicated in first-occurrence order.\n"
               "Defaults to basic when no set or section is given.";
    builder.Add(std::move(set));

    auto list_sets = FlagOption("--list-sets", '\0', &CliConfig::list_sets, RepeatPolicy::Once);
    list_sets.help = "List supported section sets and their sections.";
    builder.Add(std::move(list_sets));

    OptionSpec replay;
    replay.long_name = "--replay-mode";
    replay.arity = OptionArity::RequiredValue;
    replay.repeat = RepeatPolicy::Once;
    replay.missing_message = "--replay-mode requires a mode. Supported value: kernel.";
    replay.on_seen = [](CliConfig& config) { config.replay_mode_specified = true; };
    replay.apply = [missing = replay.missing_message](
                       const std::string& value, CliConfig&, std::vector<std::string>& errors) {
        if (value != "kernel") {
            errors.push_back(
                value.empty() ? missing : "unsupported replay mode '" + value + "'. Supported value: kernel.");
        }
    };
    replay.help = "Kernel replay mode.\n"
                  "Value: kernel. Default: kernel.";
    builder.Add(std::move(replay));

    auto add_path = [&builder](
                        const char* name, char alias, const char* missing, const char* empty,
                        std::optional<std::string> CliConfig::*field, const char* help_text) {
        OptionSpec option;
        option.long_name = name;
        option.short_name = alias;
        option.arity = OptionArity::RequiredValue;
        option.repeat = RepeatPolicy::Once;
        option.missing_message = missing;
        option.help = help_text;
        option.apply = [field, empty](const std::string& value, CliConfig& config, std::vector<std::string>& errors) {
            if (value.empty()) {
                errors.push_back(empty);
            } else {
                config.*field = value;
            }
        };
        builder.Add(std::move(option));
    };
    add_path(
        "--import", 'i', "--import expects a report file, but no file was provided.",
        "--import expects a report file, but no file was specified.", &CliConfig::import_path,
        "Import and unpack a report generated by npu-compute.\n"
        "Use --import alone or with --export as the only additional option.\n"
        "--export specifies the directory where the report is unpacked.");
    add_path(
        "--export", 'o', "--export requires an output path: a report file or directory.",
        "--export requires a non-empty output path.", &CliConfig::export_path,
        "Specify the output file or directory.\n"
        "Collection: a new .npu-rep file or an existing\n"
        "  directory. The parent directory must exist.\n"
        "Import: an existing directory for unpacking.\n"
        "Default location: current directory.\n"
        "Existing files are not overwritten.");
    return builder.Build();
}

void ApplyDefaultSet(CliConfig* config, std::vector<std::string>* errors)
{
    if (config->show_help || !errors->empty() || config->list_sets || config->list_sections || config->import_path ||
        !config->sections.empty() || config->program.empty()) {
        return;
    }
    config->sets.push_back("basic");
    for (const auto& section : SetSections("basic")) {
        AddSection(section, "", config, errors);
    }
}

} // namespace

bool ParseCli(int argc, char** argv, CliConfig* config, std::vector<std::string>* errors)
{
    if (errors == nullptr) {
        return false;
    }
    errors->clear();
    if (config == nullptr) {
        AddError("internal error: config is null", errors);
        return false;
    }
    const auto registry = RegisterOptions();
    registry.Parse(argc, argv, *config, *errors);

    ApplyDefaultSet(config, errors);
    ValidateCombinations(*config, errors);
    return errors->empty();
}

const char* ReplayModeName(ReplayMode mode)
{
    switch (mode) {
        case ReplayMode::Kernel:
            return "kernel";
    }
    return "kernel";
}

void PrintUsage(FILE* stream, const char* program)
{
    if (stream == nullptr) {
        return;
    }
    std::fprintf(
        stream,
        "Usage: %s [options] [--] [program] [program-arguments]\n"
        "\n",
        program == nullptr ? "npu-compute" : program);
    const auto registry = RegisterOptions();
    std::fputs(registry.HelpText().c_str(), stream);
    std::fputs(
        "\n"
        "      --                     Optional separator between npu-compute options and the program.\n"
        "                             Required if the program name starts with '-'.\n\n",
        stream);
}

void PrintSections(FILE* stream)
{
    if (stream == nullptr) {
        return;
    }
    auto sections = kSupportedSections;
    std::sort(sections.begin(), sections.end(), AlphabeticalSectionLess);
    for (const char* section : sections) {
        std::fprintf(stream, "%s\n", section);
    }
}

void PrintSets(FILE* stream)
{
    if (stream == nullptr) {
        return;
    }
    for (const char* name : kSupportedSets) {
        std::fprintf(stream, "%s:\n", name);
        auto sections = SetSections(name);
        std::sort(sections.begin(), sections.end(), AlphabeticalSectionLess);
        for (const auto& section : sections) {
            std::fprintf(stream, "  %s\n", section.c_str());
        }
    }
}

} // namespace npucompute::cli
