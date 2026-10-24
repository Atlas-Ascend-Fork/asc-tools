/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "rep_test_decoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

namespace npucompute::cli::test {
namespace {

void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

template <typename Value>
Value ReadLittleEndian(const uint8_t* data)
{
    Value value = 0;
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        value |= static_cast<Value>(data[index]) << (index * 8U);
    }
    return value;
}

uint16_t ReadLe16(const uint8_t* data) { return ReadLittleEndian<uint16_t>(data); }

uint32_t ReadLe32(const uint8_t* data) { return ReadLittleEndian<uint32_t>(data); }

uint64_t ReadLe64(const uint8_t* data) { return ReadLittleEndian<uint64_t>(data); }

bool HasMagic(const uint8_t* data) { return std::equal(NPU_REP_MAGIC.begin(), NPU_REP_MAGIC.end(), data); }

} // namespace

bool DecodeRep(const std::vector<uint8_t>& encoded, DecodedRep* decoded, std::string* error)
{
    if (decoded == nullptr) {
        SetError(error, "decoded rep is null");
        return false;
    }
    *decoded = {};
    if (encoded.size() < NPU_REP_HEAD_SIZE || !HasMagic(encoded.data())) {
        SetError(error, "invalid rep header");
        return false;
    }

    decoded->version = ReadLe32(encoded.data() + 8);
    decoded->origin = ReadLe16(encoded.data() + 12);
    decoded->head_length = ReadLe16(encoded.data() + 14);
    decoded->file_info_count = ReadLe32(encoded.data() + 16);
    decoded->file_info_length = ReadLe32(encoded.data() + 20);
    decoded->rep_length = ReadLe64(encoded.data() + 28);
    if (decoded->head_length != NPU_REP_HEAD_SIZE || decoded->file_info_length != NPU_REP_FILE_INFO_SIZE ||
        decoded->rep_length != encoded.size()) {
        SetError(error, "invalid rep header lengths");
        return false;
    }

    const uint64_t table_size = static_cast<uint64_t>(decoded->file_info_count) * decoded->file_info_length;
    const uint64_t payload_start = decoded->head_length + table_size;
    if (payload_start > encoded.size()) {
        SetError(error, "rep file info table exceeds input");
        return false;
    }

    uint64_t expected_offset = payload_start;
    decoded->entries.reserve(decoded->file_info_count);
    for (uint32_t index = 0; index < decoded->file_info_count; ++index) {
        const uint64_t info_offset = decoded->head_length + static_cast<uint64_t>(index) * decoded->file_info_length;
        const uint8_t* info = encoded.data() + info_offset;
        if (!HasMagic(info)) {
            SetError(error, "invalid file info magic");
            return false;
        }
        const auto* name_begin = reinterpret_cast<const char*>(info + 8);
        const auto* name_end = static_cast<const char*>(std::memchr(name_begin, '\0', NPU_REP_FILE_NAME_SIZE));
        if (name_end == nullptr || name_end == name_begin) {
            SetError(error, "invalid file info name");
            return false;
        }

        DecodedRepEntry entry;
        entry.file_name.assign(name_begin, name_end);
        entry.file_type = static_cast<NpuRepFileType>(ReadLe16(info + 136));
        entry.file_length = ReadLe64(info + 144);
        entry.file_offset = ReadLe64(info + 152);
        if (entry.file_offset != expected_offset || entry.file_length > encoded.size() - entry.file_offset) {
            SetError(error, "invalid file payload range");
            return false;
        }
        const auto payload_begin = encoded.begin() + entry.file_offset;
        entry.payload.assign(payload_begin, payload_begin + entry.file_length);
        expected_offset += entry.file_length;
        decoded->entries.push_back(std::move(entry));
    }
    if (expected_offset != encoded.size()) {
        SetError(error, "rep contains unreferenced payload bytes");
        return false;
    }
    return true;
}

} // namespace npucompute::cli::test
