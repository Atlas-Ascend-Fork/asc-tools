/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_SIMT_MEMORY_RECORD_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_SIMT_MEMORY_RECORD_H

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace npucheck {

// Host representation of one SIMT LDG/STG access before allocation checking.
// The raw base/offset pair is retained so diagnostics can distinguish address
// recovery failures from allocation-boundary failures.
struct SimtMemoryAccessRecord {
    uint64_t launchId = 0;
    uint64_t invocationId = 0;
    uint64_t pc = 0;
    uint32_t deviceId = 0;
    uint32_t phyCoreId = 0;
    uint32_t blockId = 0;
    uint32_t threadX = 0;
    uint32_t threadY = 0;
    uint32_t threadZ = 0;
    uint32_t blockDimX = 0;
    uint32_t blockDimY = 0;
    uint32_t blockDimZ = 0;
    uint64_t base = 0;
    int64_t offset = 0;
    uint32_t elementBytes = 0;
    uint32_t accessBytes = 0;
    uint32_t memorySpace = 0;
    uint32_t accessMode = 0;
    uint32_t instructionId = 0;
};

using SimtAccessRange = std::pair<uint64_t, uint64_t>; // [begin, end)

inline std::optional<uint64_t> ResolveSimtAddress(const SimtMemoryAccessRecord& record) noexcept
{
    if (record.elementBytes == 0) {
        return std::nullopt;
    }
    const __int128 address = static_cast<__int128>(record.base) +
                             static_cast<__int128>(record.offset) * static_cast<__int128>(record.elementBytes);
    if (address < 0 || address > static_cast<__int128>(std::numeric_limits<uint64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(address);
}

inline std::optional<SimtAccessRange> ResolveSimtAccessRange(const SimtMemoryAccessRecord& record) noexcept
{
    if (record.accessBytes == 0) {
        return std::nullopt;
    }
    const auto address = ResolveSimtAddress(record);
    if (!address.has_value() || *address > std::numeric_limits<uint64_t>::max() - record.accessBytes) {
        return std::nullopt;
    }
    return SimtAccessRange{*address, *address + record.accessBytes};
}

} // namespace npucheck

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_SIMT_MEMORY_RECORD_H
