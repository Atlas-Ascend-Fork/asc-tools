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

inline constexpr std::array<uint8_t, 8> kNpuRepMagic = {'n', 'p', 'u', '-', 'r', 'e', 'p', '\0'};
inline constexpr uint32_t kNpuRepVersion = 0x00010000U;
inline constexpr uint16_t kNpuRepOrigin = 1U;
// Bytes emitted by the current encoder. Readers may accept header extensions.
inline constexpr std::size_t kNpuRepHeadSize = 36U;
// End of the last field understood by the current version.
inline constexpr std::size_t kNpuRepHeaderFieldsEnd = 28U + sizeof(uint64_t);
inline constexpr std::size_t kNpuRepFileInfoSize = 160U;
inline constexpr std::size_t kNpuRepFileNameSize = 128U;
// Includes the outermost report (or the collection root directory when packing).
inline constexpr std::size_t kNpuRepMaxDepth = 64U;

enum class NpuRepFileType : uint16_t {
    NpuRep = 1,
    Json = 2,
    Jsonl = 3,
    Csv = 4,
    Sqlite3 = 5,
    Protobuf = 6,
};

} // namespace npucompute::cli

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_CLI_REPORT_REP_FORMAT_H
