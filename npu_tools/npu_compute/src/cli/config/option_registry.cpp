/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "config/option_registry.h"
#include "config/config.h"

#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <utility>

namespace npucompute::cli {
namespace {
constexpr std::size_t LONG_OPTION_PREFIX_LENGTH = 2U;
constexpr std::size_t MIN_LONG_OPTION_NAME_LENGTH = LONG_OPTION_PREFIX_LENGTH;
constexpr std::size_t SHORT_OPTION_TOKEN_LENGTH = 2U;

bool IsNameCharacter(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-';
}
void ValidateDefinition(const OptionSpec& option)
{
    const auto& name = option.long_name;
    if (name.size() <= MIN_LONG_OPTION_NAME_LENGTH || name.compare(0, LONG_OPTION_PREFIX_LENGTH, "--") != 0 ||
        name[LONG_OPTION_PREFIX_LENGTH] == '-' ||
        !std::all_of(name.begin() + LONG_OPTION_PREFIX_LENGTH, name.end(), IsNameCharacter)) {
        throw std::invalid_argument("invalid option name: " + name);
    }
    if (option.short_name != '\0' && (!IsNameCharacter(option.short_name) || option.short_name == '-')) {
        throw std::invalid_argument("invalid short alias for " + name);
    }
    if (!option.apply) {
        throw std::invalid_argument("missing option handler for " + name);
    }
    if (option.arity == OptionArity::RequiredValue && option.missing_message.empty()) {
        throw std::invalid_argument("missing value diagnostic for " + name);
    }
    if (option.protect_from_value && option.arity != OptionArity::Flag) {
        throw std::invalid_argument("only flags may protect a following value: " + name);
    }
}
void ApplyOption(
    const OptionSpec& spec, const OptionValue& value, unsigned occurrence, CliConfig& config,
    std::vector<std::string>& errors)
{
    if (spec.on_seen) {
        spec.on_seen(config);
    }
    // A missing value still counts as an occurrence, but takes precedence over a duplicate error.
    if (value.state == ValueState::Missing) {
        errors.push_back(spec.missing_message);
        return;
    }
    if (spec.repeat == RepeatPolicy::Once && occurrence > 1) {
        errors.push_back(spec.long_name + " may only be specified once");
        return;
    }
    spec.apply(value.text, config, errors);
}

void CollectApplication(ArgumentCursor& cursor, CliConfig& config)
{
    if (!cursor.HasNext()) {
        return;
    }
    config.program = cursor.Take();
    while (cursor.HasNext()) {
        config.program_arguments.push_back(cursor.Take());
    }
}
} // namespace

std::string ArgumentCursor::Peek() const { return !HasNext() || argv_[position_] == nullptr ? "" : argv_[position_]; }
std::string ArgumentCursor::Take()
{
    const auto value = Peek();
    if (HasNext()) {
        ++position_;
    }
    return value;
}

void OptionRegistryBuilder::Add(OptionSpec option)
{
    ValidateDefinition(option);
    for (const auto& existing : options_) {
        if (existing.long_name == option.long_name ||
            (option.short_name != '\0' && existing.short_name == option.short_name)) {
            throw std::invalid_argument("duplicate option name or alias: " + option.long_name);
        }
    }
    options_.push_back(std::move(option));
}
OptionRegistry OptionRegistryBuilder::Build() const { return OptionRegistry(options_); }

OptionRegistry::OptionRegistry(std::vector<OptionSpec> options) : options_(std::move(options))
{
    for (std::size_t id = 0; id < options_.size(); ++id) {
        const auto& option = options_[id];
        names_.emplace(option.long_name, id);
        if (option.short_name != '\0') {
            names_.emplace(std::string("-") + option.short_name, id);
            short_names_.emplace(option.short_name, id);
        }
    }
}

void OptionRegistry::Parse(int argc, char** argv, CliConfig& config, std::vector<std::string>& errors) const
{
    config = CliConfig{};
    errors.clear();
    OccurrenceTracker occurrences(options_.size());
    ArgumentCursor cursor(argc, argv);
    while (cursor.HasNext()) {
        const auto token = Classify(cursor.Peek());
        switch (token.kind) {
            case TokenKind::Separator:
                cursor.Take();
                CollectApplication(cursor, config);
                return;
            case TokenKind::Application:
                CollectApplication(cursor, config);
                return;
            case TokenKind::UnknownOption:
                errors.push_back("unknown option '" + cursor.Take() + "'. Use --help to see supported options.");
                break;
            case TokenKind::Option: {
                cursor.Take();
                const auto& match = *token.match;
                const auto& spec = options_[match.option_id];
                const auto occurrence = occurrences.Record(match.option_id);
                // Invalid flag values count as occurrences, but arity errors take
                // precedence over duplicate errors and must not invoke handlers.
                if (spec.arity == OptionArity::Flag && match.has_inline_value) {
                    errors.push_back(spec.long_name + " does not take a value.");
                    break;
                }
                const auto value = ReadValue(match, cursor);
                ApplyOption(spec, value, occurrence, config, errors);
                break;
            }
        }
    }
}

std::string OptionRegistry::HelpText() const
{
    constexpr std::size_t description_column = 29;
    const std::string continuation_indent(description_column, ' ');
    std::string result;
    for (const auto& option : options_) {
        if (!result.empty()) {
            result += '\n';
        }
        std::string syntax = option.short_name == '\0' ? "      " : std::string("  -") + option.short_name + ", ";
        syntax += option.long_name;
        if (option.arity == OptionArity::RequiredValue) {
            syntax += " arg";
        }
        result += syntax;
        result.append(syntax.size() < description_column ? description_column - syntax.size() : 1, ' ');
        std::istringstream description(option.help);
        std::string line;
        std::getline(description, line);
        result += line + '\n';
        while (std::getline(description, line)) {
            result += continuation_indent + line + '\n';
        }
    }
    return result;
}

std::optional<OptionMatch> OptionRegistry::Match(const std::string& text) const
{
    const auto exact = names_.find(text);
    if (exact != names_.end()) {
        return OptionMatch{exact->second, false, {}};
    }
    if (text.compare(0, LONG_OPTION_PREFIX_LENGTH, "--") == 0) {
        const auto equals = text.find('=');
        if (equals == std::string::npos) {
            return std::nullopt;
        }
        const auto option = names_.find(text.substr(0, equals));
        if (option != names_.end()) {
            return OptionMatch{option->second, true, text.substr(equals + 1)};
        }
        return std::nullopt;
    }
    if (text.size() > SHORT_OPTION_TOKEN_LENGTH && text[0] == '-') {
        const auto option = short_names_.find(text[1]);
        if (option != short_names_.end() && options_[option->second].arity == OptionArity::RequiredValue) {
            return OptionMatch{option->second, true, text.substr(2)};
        }
    }
    return std::nullopt;
}

ClassifiedToken OptionRegistry::Classify(const std::string& text) const
{
    if (text == "--") {
        return {TokenKind::Separator, std::nullopt};
    }
    if (auto match = Match(text)) {
        return {TokenKind::Option, std::move(match)};
    }
    return {text.size() > 1 && text[0] == '-' ? TokenKind::UnknownOption : TokenKind::Application, std::nullopt};
}

bool OptionRegistry::ProtectsValue(const std::string& text) const
{
    if (text == "--") {
        return true;
    }
    const auto exact = names_.find(text);
    return exact != names_.end() && options_[exact->second].protect_from_value;
}

OptionValue OptionRegistry::ReadValue(const OptionMatch& match, ArgumentCursor& cursor) const
{
    if (options_[match.option_id].arity == OptionArity::Flag) {
        return {};
    }
    if (match.has_inline_value) {
        return {ValueState::Present, match.inline_value};
    }
    if (!cursor.HasNext() || ProtectsValue(cursor.Peek())) {
        return {ValueState::Missing, {}};
    }
    return {ValueState::Present, cursor.Take()};
}
} // namespace npucompute::cli
