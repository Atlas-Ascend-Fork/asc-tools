/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "hardware/hardware_info_json.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace npucompute {
namespace {
constexpr int CAPACITY_DECIMAL_PLACES = 2;
constexpr std::uint32_t DECIMAL_RADIX = 10;
constexpr std::size_t JSON_ESCAPE_BYTES = 2; // Backslash plus escaped character.

void SetError(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool ValidateCapacity(double value, std::string_view field, std::string* error)
{
    if (std::isfinite(value) && value >= 0.0) {
        return true;
    }
    SetError(std::string(field) + " must be a finite non-negative value", error);
    return false;
}

std::string FormatCapacity(double value)
{
    if (value == 0.0) {
        return "0";
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(CAPACITY_DECIMAL_PLACES) << value;
    std::string result = stream.str();
    while (!result.empty() && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result;
}

std::string EscapeJson(std::string_view value)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (character < 0x20) {
                    escaped += "\\u00";
                    escaped.push_back(HEX[(character >> 4) & 0x0f]);
                    escaped.push_back(HEX[character & 0x0f]);
                } else {
                    escaped.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    return escaped;
}

bool ParsePositiveUintField(std::string_view line, std::string_view field, std::uint32_t* value)
{
    if (value == nullptr) {
        return false;
    }
    const std::string key = "\"" + std::string(field) + "\":";
    const std::size_t keyPosition = line.find(key);
    if (keyPosition == std::string_view::npos) {
        return false;
    }
    std::size_t begin = keyPosition + key.size();
    while (begin < line.size() && line[begin] == ' ') {
        ++begin;
    }
    std::size_t end = begin;
    std::uint32_t parsed = 0;
    const std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max();
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
        const char digitCharacter = line[end];
        const std::uint32_t digit = static_cast<std::uint32_t>(digitCharacter - '0');
        if (parsed > (maximum - digit) / DECIMAL_RADIX) {
            return false;
        }
        parsed = parsed * DECIMAL_RADIX + digit;
        ++end;
    }
    if (end == begin || parsed == 0) {
        return false;
    }
    *value = parsed;
    return true;
}

bool ParseNonEmptyStringField(std::string_view line, std::string_view field, std::string* value)
{
    if (value == nullptr) {
        return false;
    }
    const std::string key = "\"" + std::string(field) + "\":\"";
    const std::size_t keyPosition = line.find(key);
    if (keyPosition == std::string_view::npos) {
        return false;
    }
    const std::size_t begin = keyPosition + key.size();
    std::size_t end = begin;
    while (end < line.size()) {
        if (line[end] == '\\') {
            end += JSON_ESCAPE_BYTES;
            continue;
        }
        if (line[end] == '\"') {
            break;
        }
        ++end;
    }
    if (end == begin || end >= line.size()) {
        return false;
    }
    value->assign(line.substr(begin, end - begin));
    return true;
}

} // namespace

bool SerializeHardwareInfoJsonl(const HardwareInfoSnapshot& snapshot, std::string* jsonl, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (jsonl == nullptr) {
        SetError("HardwareInfo JSONL output is null", error);
        return false;
    }
    jsonl->clear();

    if (!ValidateCapacity(snapshot.host.memoryTotalSizeMb, "memory total size(MB)", error) ||
        !ValidateCapacity(snapshot.host.diskTotalSizeGb, "disk total size(GB)", error) ||
        !ValidateCapacity(snapshot.memory.hbmTotalMb, "hbm total(MB)", error) ||
        !ValidateCapacity(snapshot.memory.hbmUsedMb, "hbm used(MB)", error)) {
        return false;
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"category\":\"Host Info\",\"cpu physical count\":" << snapshot.host.cpuPhysicalCount
           << ",\"cpu logical count\":" << snapshot.host.cpuLogicalCount
           << ",\"memory total size(MB)\":" << FormatCapacity(snapshot.host.memoryTotalSizeMb)
           << ",\"disk total size(GB)\":" << FormatCapacity(snapshot.host.diskTotalSizeGb) << "}\n";
    output << "{\"category\":\"Device Info\",\"npu count\":" << snapshot.device.npuCount << ",\"chip info\":\""
           << EscapeJson(snapshot.device.chipInfo) << "\",\"arch info\":\"" << EscapeJson(snapshot.device.archInfo)
           << "\"}\n";
    output << "{\"category\":\"CPU Information\",\"control cpu count\":" << snapshot.cpu.controlCpuCount
           << ",\"ai cpu count\":" << snapshot.cpu.aiCpuCount
           << ",\"ai cpu frequency(MHZ)\":" << snapshot.cpu.aiCpuFrequencyMhz << "}\n";
    output << "{\"category\":\"AI Core Information\",\"ai core count\":" << snapshot.aiCore.aiCoreCount
           << ",\"ai cube count\":" << snapshot.aiCore.aiCubeCount
           << ",\"ai vector count\":" << snapshot.aiCore.aiVectorCount
           << ",\"ai cube frequency(MHZ)\":" << snapshot.aiCore.aiCubeFrequencyMhz
           << ",\"ai vector frequency(MHZ)\":" << snapshot.aiCore.aiVectorFrequencyMhz << "}\n";
    output << "{\"category\":\"Memory Information\",\"hbm total(MB)\":" << FormatCapacity(snapshot.memory.hbmTotalMb)
           << ",\"hbm used(MB)\":" << FormatCapacity(snapshot.memory.hbmUsedMb)
           << ",\"hbm frequency(MHZ)\":" << snapshot.memory.hbmFrequencyMhz << "}\n";

    *jsonl = output.str();
    return true;
}

bool ParseHardwareInfoFrequenciesJsonl(std::string_view jsonl, HardwareInfoFrequencies* frequencies, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (frequencies == nullptr) {
        SetError("HardwareInfo frequencies output is null", error);
        return false;
    }
    *frequencies = {};

    std::size_t begin = 0;
    while (begin <= jsonl.size()) {
        const std::size_t end = jsonl.find('\n', begin);
        const std::string_view line =
            end == std::string_view::npos ? jsonl.substr(begin) : jsonl.substr(begin, end - begin);
        if (line.find("\"category\":\"AI Core Information\"") != std::string_view::npos) {
            if (!ParsePositiveUintField(line, "ai cube count", &frequencies->aiCubeCount) ||
                !ParsePositiveUintField(line, "ai vector count", &frequencies->aiVectorCount) ||
                !ParsePositiveUintField(line, "ai cube frequency(MHZ)", &frequencies->aiCubeFrequencyMhz) ||
                !ParsePositiveUintField(line, "ai vector frequency(MHZ)", &frequencies->aiVectorFrequencyMhz)) {
                *frequencies = {};
                SetError("HardwareInfo AI Core counts or frequencies are missing or invalid", error);
                return false;
            }
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }

    SetError("HardwareInfo AI Core Information row is missing", error);
    return false;
}

bool ParseHardwareInfoSocNameJsonl(std::string_view jsonl, std::string* socName, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (socName == nullptr) {
        SetError("HardwareInfo SoC output is null", error);
        return false;
    }
    socName->clear();

    std::size_t begin = 0;
    while (begin <= jsonl.size()) {
        const std::size_t end = jsonl.find('\n', begin);
        const std::string_view line =
            end == std::string_view::npos ? jsonl.substr(begin) : jsonl.substr(begin, end - begin);
        if (line.find("\"category\":\"Device Info\"") != std::string_view::npos) {
            if (!ParseNonEmptyStringField(line, "chip info", socName)) {
                SetError("HardwareInfo Device Info chip info is missing or empty", error);
                return false;
            }
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }

    SetError("HardwareInfo Device Info row is missing", error);
    return false;
}

} // namespace npucompute
