/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/rep_decoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace npucompute::cli {
namespace {

bool Fail(const std::string& message, std::string* error)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

template <typename Integer>
Integer ReadLittleEndian(const uint8_t* data)
{
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(data[index]) << (index * NPU_REP_BYTE_BITS);
    }
    return value;
}
uint16_t ReadLe16(const uint8_t* data) { return ReadLittleEndian<uint16_t>(data); }

uint32_t ReadLe32(const uint8_t* data) { return ReadLittleEndian<uint32_t>(data); }

uint64_t ReadLe64(const uint8_t* data)
{
    uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<uint64_t>(data[index]) << (index * NPU_REP_BYTE_BITS);
    }
    return value;
}

bool HasMagic(const uint8_t* data) { return std::equal(NPU_REP_MAGIC.begin(), NPU_REP_MAGIC.end(), data); }

bool IsKnownFileType(NpuRepFileType type)
{
    // Keep this allowlist explicit because enum values are part of the external REP format.
    return type == NpuRepFileType::NpuRep || type == NpuRepFileType::Json || type == NpuRepFileType::Jsonl ||
           type == NpuRepFileType::Csv || type == NpuRepFileType::Sqlite || type == NpuRepFileType::Txt;
}

bool ReadFileName(const uint8_t* data, std::string* name, std::string* error)
{
    const char* begin = reinterpret_cast<const char*>(data);
    const char* end = static_cast<const char*>(std::memchr(begin, '\0', NPU_REP_FILE_NAME_SIZE));
    if (end == nullptr) {
        return Fail("rep file info name is not null terminated", error);
    }
    if (end == begin) {
        return Fail("rep file info name is empty", error);
    }
    name->assign(begin, end);
    if (*name == "." || *name == "..") {
        return Fail("rep file info name is unsafe", error);
    }
    if (name->find('/') != std::string::npos || name->find('\\') != std::string::npos) {
        return Fail("rep file info name contains a path separator", error);
    }
    return true;
}

void ClassifyRepError(RepDecodeError* category, RepDecodeError value)
{
    if (category != nullptr) {
        *category = value;
    }
}

struct RepLayout {
    uint32_t version = 0;
    uint16_t origin = 0;
    uint16_t head_length = 0;
    uint32_t file_count = 0;
    uint32_t file_info_length = 0;
    uint64_t payload_start = 0;
};

bool ReadRepVersion(
    const std::vector<uint8_t>& encoded, RepLayout& layout, std::string* error, RepDecodeError* category)
{
    if (encoded.size() < NPU_REP_MAGIC.size() || !HasMagic(encoded.data())) {
        ClassifyRepError(category, RepDecodeError::InvalidFormat);
        return Fail("invalid rep header magic", error);
    }
    const uint8_t* head = encoded.data();
    if (encoded.size() < NPU_REP_HEADER_VERSION_OFFSET + sizeof(uint32_t)) {
        return Fail("truncated rep version", error);
    }
    layout.version = ReadLe32(head + NPU_REP_HEADER_VERSION_OFFSET);
    if (layout.version != NPU_REP_VERSION) {
        ClassifyRepError(category, RepDecodeError::UnsupportedVersion);
        return Fail("unsupported rep version", error);
    }
    // Bound all known fields before reading; this is not an exact header-size requirement.
    if (encoded.size() < NPU_REP_HEADER_FIELDS_END) {
        return Fail("truncated rep header fields", error);
    }
    return true;
}

bool ReadRepLayout(const std::vector<uint8_t>& encoded, RepLayout& layout, std::string* error)
{
    const uint8_t* head = encoded.data();
    layout.origin = ReadLe16(head + NPU_REP_HEADER_ORIGIN_OFFSET);
    layout.head_length = ReadLe16(head + NPU_REP_HEADER_HEADER_LENGTH_OFFSET);
    layout.file_count = ReadLe32(head + NPU_REP_HEADER_FILE_COUNT_OFFSET);
    layout.file_info_length = ReadLe32(head + NPU_REP_HEADER_FILE_INFO_LENGTH_OFFSET);
    const uint32_t reserved = ReadLe32(head + NPU_REP_HEADER_RESERVED_OFFSET);
    const uint64_t rep_length = ReadLe64(head + NPU_REP_HEADER_LENGTH_OFFSET);

    if (layout.origin != NPU_REP_ORIGIN) {
        return Fail("unsupported rep origin", error);
    }
    if (layout.head_length < NPU_REP_HEADER_FIELDS_END || layout.head_length > encoded.size()) {
        return Fail("invalid rep header length", error);
    }
    if (layout.file_info_length != NPU_REP_FILE_INFO_SIZE) {
        return Fail("invalid rep file info length", error);
    }
    if (reserved != 0U) {
        return Fail("rep header reserved field is not zero", error);
    }
    if (rep_length != encoded.size()) {
        return Fail("rep length does not match input size", error);
    }

    if (layout.file_count > (std::numeric_limits<uint64_t>::max() - layout.head_length) / layout.file_info_length) {
        return Fail("rep file info table length overflows", error);
    }
    layout.payload_start = layout.head_length + static_cast<uint64_t>(layout.file_count) * layout.file_info_length;
    if (layout.payload_start > encoded.size()) {
        return Fail("rep file info table exceeds input", error);
    }
    return true;
}

bool ReadRepEntry(
    const std::vector<uint8_t>& encoded, const uint8_t* info, std::unordered_set<std::string>& names,
    uint64_t& expected_payload_offset, DecodedRep* decoded, std::string* error)
{
    if (!HasMagic(info)) {
        *decoded = {};
        return Fail("invalid rep file info magic", error);
    }

    DecodedRepEntry entry;
    if (!ReadFileName(info + NPU_REP_ENTRY_NAME_OFFSET, &entry.file_name, error)) {
        *decoded = {};
        return false;
    }
    if (!names.insert(entry.file_name).second) {
        *decoded = {};
        return Fail("rep contains duplicate file names: " + entry.file_name, error);
    }

    entry.file_type = static_cast<NpuRepFileType>(ReadLe16(info + NPU_REP_ENTRY_TYPE_OFFSET));
    if (!IsKnownFileType(entry.file_type)) {
        *decoded = {};
        return Fail("rep entry has an unknown file type", error);
    }
    if (ReadLe16(info + NPU_REP_ENTRY_RESERVED16_OFFSET) != 0U ||
        ReadLe32(info + NPU_REP_ENTRY_RESERVED32_OFFSET) != 0U) {
        *decoded = {};
        return Fail("rep file info reserved field is not zero", error);
    }

    const uint64_t file_length = ReadLe64(info + NPU_REP_ENTRY_LENGTH_OFFSET);
    const uint64_t file_offset = ReadLe64(info + NPU_REP_ENTRY_PAYLOAD_OFFSET);
    if (file_offset != expected_payload_offset || file_offset > encoded.size() ||
        file_length > encoded.size() - file_offset) {
        *decoded = {};
        return Fail("invalid rep file payload range", error);
    }
    const auto payload_begin = encoded.begin() + static_cast<std::size_t>(file_offset);
    entry.payload.assign(payload_begin, payload_begin + static_cast<std::size_t>(file_length));
    expected_payload_offset += file_length;
    decoded->entries.push_back(std::move(entry));
    return true;
}

bool ReadRepEntries(
    const std::vector<uint8_t>& encoded, const RepLayout& layout, DecodedRep* decoded, std::string* error)
{
    decoded->version = layout.version;
    decoded->origin = layout.origin;
    decoded->entries.reserve(layout.file_count);
    std::unordered_set<std::string> names;
    names.reserve(layout.file_count);
    uint64_t expected_payload_offset = layout.payload_start;
    for (uint32_t index = 0; index < layout.file_count; ++index) {
        const uint64_t info_offset = layout.head_length + static_cast<uint64_t>(index) * layout.file_info_length;
        const uint8_t* info = encoded.data() + info_offset;
        if (!ReadRepEntry(encoded, info, names, expected_payload_offset, decoded, error)) {
            return false;
        }
    }

    if (expected_payload_offset != encoded.size()) {
        *decoded = {};
        return Fail("rep contains unreferenced payload bytes", error);
    }
    return true;
}

} // namespace

bool DecodeRep(const std::vector<uint8_t>& encoded, DecodedRep* decoded, std::string* error, RepDecodeError* category)
{
    ClassifyRepError(category, RepDecodeError::Corrupted);
    if (error != nullptr) {
        error->clear();
    }
    if (decoded == nullptr) {
        ClassifyRepError(category, RepDecodeError::Internal);
        return Fail("decoded rep output is null", error);
    }
    *decoded = {};

    try {
        RepLayout layout;
        if (!ReadRepVersion(encoded, layout, error, category) || !ReadRepLayout(encoded, layout, error) ||
            !ReadRepEntries(encoded, layout, decoded, error)) {
            return false;
        }
        ClassifyRepError(category, RepDecodeError::None);
        return true;
    } catch (const std::bad_alloc&) {
        ClassifyRepError(category, RepDecodeError::Internal);
        *decoded = {};
        return Fail("allocate decoded rep data failed", error);
    }
}

} // namespace npucompute::cli
