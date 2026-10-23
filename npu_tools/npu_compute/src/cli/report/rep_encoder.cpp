/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/rep_encoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>

namespace npucompute::cli {
namespace {

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

void WriteLe16(uint16_t value, uint8_t* output)
{
    output[0] = static_cast<uint8_t>(value);
    output[1] = static_cast<uint8_t>(value >> kNpuRepByteBits);
}

void WriteLe32(uint32_t value, uint8_t* output)
{
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * kNpuRepByteBits));
    }
}

void WriteLe64(uint64_t value, uint8_t* output)
{
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * kNpuRepByteBits));
    }
}

bool IsKnownFileType(NpuRepFileType type)
{
    switch (type) {
        case NpuRepFileType::NpuRep:
        case NpuRepFileType::Json:
        case NpuRepFileType::Jsonl:
        case NpuRepFileType::Csv:
        case NpuRepFileType::Sqlite:
        case NpuRepFileType::Txt:
            return true;
    }
    return false;
}

bool ValidateFileName(const std::string& name, std::string* error)
{
    if (name.empty()) {
        return Fail("rep entry file name is empty", error);
    }
    if (name.size() >= kNpuRepFileNameSize) {
        return Fail("rep entry file name exceeds 127 bytes", error);
    }
    if (name.find('\0') != std::string::npos) {
        return Fail("rep entry file name contains a null byte", error);
    }
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return Fail("rep entry file name contains a path separator", error);
    }
    return true;
}

bool AddChecked(uint64_t value, uint64_t* total, std::string* error)
{
    if (value > std::numeric_limits<uint64_t>::max() - *total) {
        return Fail("rep length exceeds uint64_t", error);
    }
    *total += value;
    return true;
}

} // namespace

bool EncodeRep(const std::vector<RepEntry>& entries, std::vector<uint8_t>* encoded, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (encoded == nullptr) {
        return Fail("encoded rep output is null", error);
    }
    encoded->clear();

    try {
        if (entries.size() > std::numeric_limits<uint32_t>::max()) {
            return Fail("rep entry count exceeds uint32_t", error);
        }

        std::unordered_set<std::string> names;
        names.reserve(entries.size());
        uint64_t total_length = kNpuRepHeadSize;
        const uint64_t entry_count = entries.size();
        if (entry_count > (std::numeric_limits<uint64_t>::max() - total_length) / kNpuRepFileInfoSize) {
            return Fail("rep file info table length overflows", error);
        }
        total_length += entry_count * kNpuRepFileInfoSize;

        for (const RepEntry& entry : entries) {
            if (!ValidateFileName(entry.file_name, error)) {
                return false;
            }
            if (!IsKnownFileType(entry.file_type)) {
                return Fail("rep entry has an unknown file type", error);
            }
            if (!names.insert(entry.file_name).second) {
                return Fail("rep contains duplicate file names: " + entry.file_name, error);
            }
            if (!AddChecked(static_cast<uint64_t>(entry.payload.size()), &total_length, error)) {
                return false;
            }
        }
        if (total_length > std::numeric_limits<std::size_t>::max()) {
            return Fail("rep length exceeds addressable memory", error);
        }

        encoded->assign(static_cast<std::size_t>(total_length), 0);
        uint8_t* output = encoded->data();
        std::copy(kNpuRepMagic.begin(), kNpuRepMagic.end(), output);
        WriteLe32(kNpuRepVersion, output + kNpuRepHeaderVersionOffset);
        WriteLe16(kNpuRepOrigin, output + kNpuRepHeaderOriginOffset);
        WriteLe16(static_cast<uint16_t>(kNpuRepHeadSize), output + kNpuRepHeaderHeaderLengthOffset);
        WriteLe32(static_cast<uint32_t>(entries.size()), output + kNpuRepHeaderFileCountOffset);
        WriteLe32(static_cast<uint32_t>(kNpuRepFileInfoSize), output + kNpuRepHeaderFileInfoLengthOffset);
        WriteLe32(0, output + kNpuRepHeaderReservedOffset);
        WriteLe64(total_length, output + kNpuRepHeaderLengthOffset);

        std::size_t payload_offset = kNpuRepHeadSize + entries.size() * kNpuRepFileInfoSize;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const RepEntry& entry = entries[index];
            uint8_t* info = output + kNpuRepHeadSize + index * kNpuRepFileInfoSize;
            std::copy(kNpuRepMagic.begin(), kNpuRepMagic.end(), info);
            std::copy(entry.file_name.begin(), entry.file_name.end(), info + kNpuRepEntryNameOffset);
            WriteLe16(static_cast<uint16_t>(entry.file_type), info + kNpuRepEntryTypeOffset);
            WriteLe16(0, info + kNpuRepEntryReserved16Offset);
            WriteLe32(0, info + kNpuRepEntryReserved32Offset);
            WriteLe64(static_cast<uint64_t>(entry.payload.size()), info + kNpuRepEntryLengthOffset);
            WriteLe64(static_cast<uint64_t>(payload_offset), info + kNpuRepEntryPayloadOffset);
            std::copy(entry.payload.begin(), entry.payload.end(), output + payload_offset);
            payload_offset += entry.payload.size();
        }
        return true;
    } catch (const std::bad_alloc&) {
        encoded->clear();
        return Fail("allocate rep buffer failed", error);
    }
}

} // namespace npucompute::cli
