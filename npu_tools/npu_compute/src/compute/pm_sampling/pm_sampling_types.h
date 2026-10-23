/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_TYPES_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_TYPES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace npucompute {
// The wire record contains eight reportable states followed by Active. Active
// is consumed for record alignment but is intentionally not part of output.
inline constexpr std::size_t kPmSamplingStateCount = 8;
inline constexpr std::size_t kPmSamplingRecordBytes = 16;
inline constexpr std::size_t kPmSamplingEncodedPcBytes = 3;
inline constexpr std::size_t kPmSamplingStateOffset = kPmSamplingEncodedPcBytes;
inline constexpr std::size_t kPmSamplingActiveOffset = kPmSamplingStateOffset + kPmSamplingStateCount;
inline constexpr unsigned kPmSamplingByteBits = 8;
inline constexpr unsigned kPmSamplingAddressShift = 3;
inline constexpr std::uint64_t kPmSamplingAddressScale = UINT64_C(1) << kPmSamplingAddressShift;
inline constexpr unsigned kPmSamplingAddressBits =
    kPmSamplingEncodedPcBytes * kPmSamplingByteBits + kPmSamplingAddressShift;
inline constexpr std::uint64_t kPmSamplingPcMask = (UINT64_C(1) << kPmSamplingAddressBits) - 1;
inline constexpr std::size_t kPmSamplingRawStateCount = kPmSamplingStateCount + 1;
using PmSamplingStateCounts = std::array<std::uint64_t, kPmSamplingStateCount>;

// PCs are original ELF instruction addresses. Physical sources merge into one row.
struct PmSamplingResult {
    std::map<std::uint64_t, PmSamplingStateCounts> rows;
    std::uint64_t receivedBytes = 0;
    std::uint64_t decodedRecords = 0;
    std::uint64_t businessRecords = 0;
    std::uint64_t excludedRecords = 0;
};

// Overflow leaves the destination unchanged.
bool AddPmSamplingStates(PmSamplingStateCounts& destination, const PmSamplingStateCounts& source);
} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_TYPES_H
