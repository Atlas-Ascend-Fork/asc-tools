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
#include "securec.h"

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
    output[1] = static_cast<uint8_t>(value >> NPU_REP_BYTE_BITS);
}

void WriteLe32(uint32_t value, uint8_t* output)
{
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * NPU_REP_BYTE_BITS));
    }
}

void WriteLe64(uint64_t value, uint8_t* output)
{
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<uint8_t>(value >> (index * NPU_REP_BYTE_BITS));
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
    if (name.size() >= NPU_REP_FILE_NAME_SIZE) {
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

bool CopyPayload(uint8_t* destination, std::size_t capacity, const std::vector<uint8_t>& payload)
{
    if (payload.size() > capacity) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < payload.size()) {
        // Secure C limits both the destination capacity and the copy length per call.
        const std::size_t remainingCapacity = std::min(capacity - offset, std::size_t{SECUREC_MEM_MAX_LEN});
        const std::size_t count = std::min(payload.size() - offset, remainingCapacity);
        if (memcpy_s(destination + offset, remainingCapacity, payload.data() + offset, count) != EOK) {
            return false;
        }
        offset += count;
    }
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
        uint64_t total_length = NPU_REP_HEAD_SIZE;
        const uint64_t entry_count = entries.size();
        if (entry_count > (std::numeric_limits<uint64_t>::max() - total_length) / NPU_REP_FILE_INFO_SIZE) {
            return Fail("rep file info table length overflows", error);
        }
        total_length += entry_count * NPU_REP_FILE_INFO_SIZE;

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
        if (memcpy_s(output, NPU_REP_HEAD_SIZE, NPU_REP_MAGIC.data(), NPU_REP_MAGIC.size()) != EOK) {
            encoded->clear();
            return Fail("copy rep header magic failed", error);
        }
        WriteLe32(NPU_REP_VERSION, output + NPU_REP_HEADER_VERSION_OFFSET);
        WriteLe16(NPU_REP_ORIGIN, output + NPU_REP_HEADER_ORIGIN_OFFSET);
        WriteLe16(static_cast<uint16_t>(NPU_REP_HEAD_SIZE), output + NPU_REP_HEADER_HEADER_LENGTH_OFFSET);
        WriteLe32(static_cast<uint32_t>(entries.size()), output + NPU_REP_HEADER_FILE_COUNT_OFFSET);
        WriteLe32(static_cast<uint32_t>(NPU_REP_FILE_INFO_SIZE), output + NPU_REP_HEADER_FILE_INFO_LENGTH_OFFSET);
        WriteLe32(0, output + NPU_REP_HEADER_RESERVED_OFFSET);
        WriteLe64(total_length, output + NPU_REP_HEADER_LENGTH_OFFSET);

        std::size_t payload_offset = NPU_REP_HEAD_SIZE + entries.size() * NPU_REP_FILE_INFO_SIZE;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const RepEntry& entry = entries[index];
            uint8_t* info = output + NPU_REP_HEAD_SIZE + index * NPU_REP_FILE_INFO_SIZE;
            if (memcpy_s(info, NPU_REP_FILE_INFO_SIZE, NPU_REP_MAGIC.data(), NPU_REP_MAGIC.size()) != EOK) {
                encoded->clear();
                return Fail("copy rep entry magic failed", error);
            }
            if (memcpy_s(
                    info + NPU_REP_ENTRY_NAME_OFFSET, NPU_REP_FILE_NAME_SIZE, entry.file_name.data(),
                    entry.file_name.size()) != EOK) {
                encoded->clear();
                return Fail("copy rep entry file name failed", error);
            }
            WriteLe16(static_cast<uint16_t>(entry.file_type), info + NPU_REP_ENTRY_TYPE_OFFSET);
            WriteLe16(0, info + NPU_REP_ENTRY_RESERVED16_OFFSET);
            WriteLe32(0, info + NPU_REP_ENTRY_RESERVED32_OFFSET);
            WriteLe64(static_cast<uint64_t>(entry.payload.size()), info + NPU_REP_ENTRY_LENGTH_OFFSET);
            WriteLe64(static_cast<uint64_t>(payload_offset), info + NPU_REP_ENTRY_PAYLOAD_OFFSET);
            if (!CopyPayload(output + payload_offset, encoded->size() - payload_offset, entry.payload)) {
                encoded->clear();
                return Fail("copy rep entry payload failed", error);
            }
            payload_offset += entry.payload.size();
        }
        return true;
    } catch (const std::bad_alloc&) {
        encoded->clear();
        return Fail("allocate rep buffer failed", error);
    }
}

} // namespace npucompute::cli
