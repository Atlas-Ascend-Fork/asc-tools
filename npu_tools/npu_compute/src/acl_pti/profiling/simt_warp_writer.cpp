/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "simt_warp_writer.h"

#include <fstream>

namespace aclpti::profiling {
namespace {

std::string JsonEscape(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    const auto value = static_cast<unsigned char>(character);
                    result += "\\u00";
                    result += hex[value >> 4U];
                    result += hex[value & 0x0fU];
                } else {
                    result += character;
                }
                break;
        }
    }
    return result;
}

} // namespace

bool WriteSimtWarpJson(const SimtWarpData& data, const std::string& path, std::string& error)
{
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        error = "cannot open output file: " + path;
        return false;
    }

    output << "{\n  \"status\": " << static_cast<int>(data.status) << ",\n  \"records\": [\n";
    for (std::size_t index = 0; index < data.records.size(); ++index) {
        const auto& record = data.records[index];
        output << "    {\n"
               << "      \"functionName\": \"" << JsonEscape(record.functionName) << "\",\n"
               << "      \"aicoreId\": " << static_cast<unsigned>(record.aicoreId) << ",\n"
               << "      \"subcoreId\": " << static_cast<unsigned>(record.subcoreId) << ",\n"
               << "      \"vfInstrAddress\": " << record.vfInstrAddress << ",\n"
               << "      \"warpId\": " << record.warpId << ",\n"
               << "      \"startTime\": " << record.startTime << ",\n"
               << "      \"endTime\": " << record.endTime << ",\n"
               << "      \"duration\": " << record.duration << "\n"
               << "    }" << (index + 1 == data.records.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"errors\": [\n";
    for (std::size_t index = 0; index < data.errors.size(); ++index) {
        output << "    \"" << JsonEscape(data.errors[index]) << "\""
               << (index + 1 == data.errors.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    output.flush();
    if (!output.good()) {
        error = "write failed: " + path;
        return false;
    }
    return true;
}

} // namespace aclpti::profiling
