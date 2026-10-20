/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report_renderer.h"

#include "diagnostic/report/report_fields.h"

#include <cctype>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string_view>

namespace npucheck {
namespace {

const char* RaceKindName(const ReportTemplateKey& key)
{
    if (key.tool != ReportTool::RACECHECK) {
        return nullptr;
    }
    if (key.pattern == "atomic_semantic") {
        return ConflictKindName(ConflictKind::ATOMIC_SEMANTIC);
    }
    if (key.pattern == "hazard_raw") {
        return ConflictKindName(ConflictKind::RAW);
    }
    if (key.pattern == "hazard_war") {
        return ConflictKindName(ConflictKind::WAR);
    }
    if (key.pattern == "hazard_waw") {
        return ConflictKindName(ConflictKind::WAW);
    }
    return nullptr;
}

std::string Trim(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string UnescapeTemplateText(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        switch (value[++i]) {
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case '\\':
                out.push_back('\\');
                break;
            default:
                out.push_back(value[i]);
                break;
        }
    }
    return out;
}

ReportRenderStatus ParseTemplateKey(const std::string& text, ReportTemplateKey* key)
{
    if (key == nullptr) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    const std::size_t dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) {
        return ReportRenderStatus::MALFORMED_TEMPLATE;
    }

    ReportTool tool = ReportTool::MEMCHECK;
    const std::string toolName = text.substr(0, dot);
    bool found = false;
    for (const ReportTool candidate :
         {ReportTool::MEMCHECK, ReportTool::INITCHECK, ReportTool::RACECHECK, ReportTool::SYNCCHECK,
          ReportTool::SOCCHECK}) {
        if (toolName == ReportToolName(candidate)) {
            tool = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return ReportRenderStatus::UNKNOWN_TEMPLATE;
    }
    *key = ReportTemplateKey{tool, text.substr(dot + 1)};
    return ReportRenderStatus::SUCCESS;
}

ReportRenderStatus AppendTemplate(const ReportTemplate& tpl, const ReportFields& fields, std::string* out)
{
    constexpr std::string_view FIELD_OPEN = "{{";
    constexpr std::string_view FIELD_CLOSE = "}}";
    if (out == nullptr) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    std::size_t pos = 0;
    while (pos < tpl.text.size()) {
        const std::size_t open = tpl.text.find(FIELD_OPEN, pos);
        if (open == std::string::npos) {
            out->append(tpl.text, pos, std::string::npos);
            return ReportRenderStatus::SUCCESS;
        }
        out->append(tpl.text, pos, open - pos);
        const std::size_t fieldBegin = open + FIELD_OPEN.size();
        const std::size_t close = tpl.text.find(FIELD_CLOSE, fieldBegin);
        if (close == std::string::npos) {
            return ReportRenderStatus::MALFORMED_TEMPLATE;
        }
        const auto field = fields.find(tpl.text.substr(fieldBegin, close - fieldBegin));
        if (field == fields.end()) {
            return ReportRenderStatus::MISSING_FIELD;
        }
        out->append(field->second);
        pos = close + FIELD_CLOSE.size();
    }
    return ReportRenderStatus::SUCCESS;
}

std::string FieldOr(const ReportFields& fields, const std::string& key, const char* fallback)
{
    const auto field = fields.find(key);
    return field == fields.end() || field->second.empty() ? fallback : field->second;
}

void PutDerivedLocation(const std::string& prefix, const std::string& locationKey, bool includeAt, ReportFields* fields)
{
    if (fields->find(locationKey) != fields->end()) {
        return;
    }
    const std::string functionKey = prefix.empty() ? "function" : prefix + "Function";
    const std::string offsetKey = prefix.empty() ? "offset" : prefix + "Offset";
    const std::string fileKey = prefix.empty() ? "file" : prefix + "File";
    const std::string lineKey = prefix.empty() ? "line" : prefix + "Line";
    const std::string pcKey = prefix.empty() ? "pc" : prefix + "Pc";
    const std::string kernelNameKey = prefix.empty() ? "kernelName" : prefix + "KernelName";
    const auto function = fields->find(functionKey);
    std::ostringstream os;
    if (includeAt) {
        os << "at ";
    }
    if (function != fields->end() && !function->second.empty()) {
        os << function->second << "+0x" << FieldOr(*fields, offsetKey, "0") << " in "
           << FieldOr(*fields, fileKey, "<unknown>") << ":" << FieldOr(*fields, lineKey, "0");
    } else {
        os << "pc 0x" << FieldOr(*fields, pcKey, "0") << " in " << FieldOr(*fields, kernelNameKey, "<unknown>");
    }
    (*fields)[locationKey] = os.str();
}

void PutDerivedLocations(ReportFields* fields)
{
    PutDerivedLocation("", "location", true, fields);
    PutDerivedLocation("first", "firstLocation", false, fields);
    PutDerivedLocation("second", "secondLocation", false, fields);
}

void EnsureTrailingNewline(std::string* out)
{
    if (!out->empty() && out->back() != '\n') {
        out->push_back('\n');
    }
}

void AppendRawStackText(const std::string& rawText, std::string* out)
{
    std::istringstream input(rawText);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        if (line.rfind("=========", 0) != 0) {
            out->append("=========     ");
        }
        out->append(line);
        out->push_back('\n');
    }
}

void AppendFrame(const ReportFrame& frame, std::size_t frameIndex, std::string* out, bool raceFrame)
{
    out->append("=========     #");
    out->append(std::to_string(frameIndex));
    out->append(" ");
    out->append(
        frame.function.empty() && raceFrame ? "pc " + detail::HexWithPrefix(frame.pc) :
                                              (frame.function.empty() ? "<unknown>" : frame.function));
    if (!frame.file.empty()) {
        out->append(" in ");
        out->append(frame.file);
        if (frame.line != 0) {
            out->append(":");
            out->append(std::to_string(frame.line));
        }
        if (frame.line != 0 && frame.column != 0) {
            out->append(":");
            out->append(std::to_string(frame.column));
        }
    }
    out->push_back('\n');
}

void AppendCallStacks(const std::vector<ReportCallStack>& stacks, std::string* out, bool raceHazard)
{
    for (const ReportCallStack& stack : stacks) {
        const bool renderRaw = stack.format == ReportStackFormat::RAW_TEXT && !stack.rawText.empty();
        const bool renderFrames = stack.format == ReportStackFormat::FRAMES && !stack.frames.empty();
        if (!renderRaw && !renderFrames) {
            continue;
        }
        EnsureTrailingNewline(out);
        out->append("=========  ");
        const bool current = raceHazard && stack.role == ReportStackRole::RELATED_ACCESS_B;
        const bool previous = raceHazard && stack.role == ReportStackRole::RELATED_ACCESS_A;
        out->append(
            current  ? "Current Device Frames:" :
            previous ? "Previous Device Frames:" :
                       ReportStackRoleTitle(stack.role));
        out->push_back('\n');
        if (renderRaw) {
            AppendRawStackText(stack.rawText, out);
        }
        if (renderFrames) {
            for (std::size_t index = 0; index < stack.frames.size(); ++index) {
                AppendFrame(stack.frames[index], index, out, current || previous);
            }
        }
    }
}

} // namespace

ReportRenderStatus RenderReportText(const ReportTemplate& tpl, const ReportFields& fields, std::string* out)
{
    if (out == nullptr) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    out->clear();
    return AppendTemplate(tpl, fields, out);
}

ReportRenderStatus RenderReportRecord(
    const ReportRecord& record, const ReportTemplateOverrides& overrides, std::string* out)
{
    if (out == nullptr) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    const auto override = overrides.find(record.key);
    const ReportTemplate* tpl = override == overrides.end() ? FindBuiltinReportTemplate(record.key) : &override->second;
    if (tpl == nullptr) {
        out->clear();
        return ReportRenderStatus::UNKNOWN_TEMPLATE;
    }

    ReportFields fields = record.fields;
    fields.try_emplace("capacityContext", "");
    PutDerivedLocations(&fields);
    fields["Severity"] = ReportSeverityName(record.severity);
    fields["pattern"] = record.key.pattern;
    const char* raceKind = RaceKindName(record.key);
    if (raceKind != nullptr) {
        fields["raceKind"] = raceKind;
    }
    const ReportRenderStatus status = RenderReportText(*tpl, fields, out);
    if (status != ReportRenderStatus::SUCCESS) {
        return status;
    }
    AppendCallStacks(record.stacks, out, raceKind != nullptr);
    return ReportRenderStatus::SUCCESS;
}

ReportRenderStatus WriteReportTextToStream(const std::string& text, std::ostream* out)
{
    if (out == nullptr) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    (*out) << text;
    return out->good() ? ReportRenderStatus::SUCCESS : ReportRenderStatus::WRITE_FAILED;
}

ReportRenderStatus WriteReportTextToFile(const std::string& text, const std::string& path)
{
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return ReportRenderStatus::OPEN_FAILED;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good() ? ReportRenderStatus::SUCCESS : ReportRenderStatus::WRITE_FAILED;
}

ReportRenderStatus LoadReportTemplateOverridesFromFile(const std::string& path, ReportTemplateOverrides* overrides)
{
    if (overrides == nullptr) {
        return ReportRenderStatus::INVALID_ARGUMENT;
    }
    std::ifstream input(path);
    if (!input.is_open()) {
        return ReportRenderStatus::OPEN_FAILED;
    }
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            return ReportRenderStatus::MALFORMED_TEMPLATE;
        }
        ReportTemplateKey key{};
        const ReportRenderStatus status = ParseTemplateKey(Trim(line.substr(0, eq)), &key);
        if (status != ReportRenderStatus::SUCCESS) {
            return status;
        }
        (*overrides)[key] = ReportTemplate{UnescapeTemplateText(line.substr(eq + 1))};
    }
    return ReportRenderStatus::SUCCESS;
}

const char* ReportStackRoleTitle(ReportStackRole role)
{
    switch (role) {
        case ReportStackRole::HOST_LAUNCH:
        case ReportStackRole::HOST_ALLOC:
        case ReportStackRole::HOST_FREE:
        case ReportStackRole::HOST_API_CALL:
            return "Host Frames:";
        case ReportStackRole::NONE:
        case ReportStackRole::FAULT_DEVICE:
        case ReportStackRole::RELATED_ACCESS_A:
        case ReportStackRole::RELATED_ACCESS_B:
        case ReportStackRole::SYNC_PRODUCER:
        case ReportStackRole::SYNC_CONSUMER:
        case ReportStackRole::STATE_PRODUCER:
        case ReportStackRole::STATE_CONSUMER:
        case ReportStackRole::PEER_DEVICE:
        case ReportStackRole::SYNC_TRIGGER:
        case ReportStackRole::SYNC_RELATED:
            return "Device Frames:";
    }
    return "Device Frames:";
}

} // namespace npucheck
