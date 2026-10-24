/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_REP_FORMAT_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_REP_FORMAT_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace npucompute::cli {

inline constexpr std::array<uint8_t, 8> NPU_REP_MAGIC = {'n', 'p', 'u', '-', 'r', 'e', 'p', '\0'};
inline constexpr uint32_t NPU_REP_VERSION = 0x00010000U;
inline constexpr uint16_t NPU_REP_ORIGIN = 1U;
// Bytes emitted by the current encoder. Readers may accept header extensions.
inline constexpr std::size_t NPU_REP_HEAD_SIZE = 36U;
// End of the last field understood by the current version.
inline constexpr std::size_t NPU_REP_HEADER_FIELDS_END = 28U + sizeof(uint64_t);
inline constexpr std::size_t NPU_REP_FILE_INFO_SIZE = 160U;
inline constexpr std::size_t NPU_REP_FILE_NAME_SIZE = 128U;
// Includes the outermost report (or the collection root directory when packing).
inline constexpr std::size_t NPU_REP_MAX_DEPTH = 64U;

// Little-endian wire offsets; independent of native structure padding.
inline constexpr std::size_t NPU_REP_HEADER_VERSION_OFFSET = 8;
inline constexpr std::size_t NPU_REP_HEADER_ORIGIN_OFFSET = 12;
inline constexpr std::size_t NPU_REP_HEADER_HEADER_LENGTH_OFFSET = 14;
inline constexpr std::size_t NPU_REP_HEADER_FILE_COUNT_OFFSET = 16;
inline constexpr std::size_t NPU_REP_HEADER_FILE_INFO_LENGTH_OFFSET = 20;
inline constexpr std::size_t NPU_REP_HEADER_RESERVED_OFFSET = 24;
inline constexpr std::size_t NPU_REP_HEADER_LENGTH_OFFSET = 28;
inline constexpr std::size_t NPU_REP_ENTRY_NAME_OFFSET = 8;
inline constexpr std::size_t NPU_REP_ENTRY_TYPE_OFFSET = 136;
inline constexpr std::size_t NPU_REP_ENTRY_RESERVED16_OFFSET = 138;
inline constexpr std::size_t NPU_REP_ENTRY_RESERVED32_OFFSET = 140;
inline constexpr std::size_t NPU_REP_ENTRY_LENGTH_OFFSET = 144;
inline constexpr std::size_t NPU_REP_ENTRY_PAYLOAD_OFFSET = 152;
inline constexpr unsigned NPU_REP_BYTE_BITS = 8;

enum class NpuRepFileType : uint16_t {
    NpuRep = 1,
    Json = 2,
    Jsonl = 3,
    Csv = 4,
    Sqlite = 5,
    Txt = 6,
};

} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_REP_FORMAT_H
