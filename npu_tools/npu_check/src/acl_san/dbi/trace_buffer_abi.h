/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_BUFFER_ABI_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_BUFFER_ABI_H

#include <cstddef>
#include <cstdint>

namespace aclsan {

// args[3] carries SYS_VA_BASE when args[4] carries this protocol marker.
constexpr uint64_t ASCSAN_SCALAR_ADDRESS_CONTEXT_V1 = 0x5343414C41520001ULL;

constexpr uint64_t ASCSAN_TRACE_BUFFER_MAGIC = 0x41534353414E303AULL;
constexpr uint32_t ASCSAN_TRACE_RECORDS_PER_CORE = 13107U;
// dav-3510 的 108 个物理核槽位均分到两个 die，每个 die 有 108 / 2 = 54 个槽位。
// 每个 die 内 AIC:AIV = 1:2，前 54 / (1 + 2) = 18 个槽位为 AIC，后 36 个为 AIV。
// 全局物理 ID 分布：die 0 为 AIC [0, 17]、AIV [18, 53]；
//                    die 1 为 AIC [54, 71]、AIV [72, 107]。
// dav-3510 因核禁用导致的可用核数变化不影响物理 ID 和拓扑布局；
// 禁用核对应的槽位保留为空洞，不重新编号或调整分区边界。
constexpr uint32_t ASCSAN_PHYSICAL_CORE_SLOT_COUNT = 108U;
constexpr uint32_t ASCSAN_PHYSICAL_CORE_PART_COUNT = 2U;
constexpr uint32_t ASCSAN_AIC_CORE_RATIO_DENOMINATOR = 3U;
constexpr uint32_t ASCSAN_CORES_PER_DIE = ASCSAN_PHYSICAL_CORE_SLOT_COUNT / ASCSAN_PHYSICAL_CORE_PART_COUNT;
constexpr uint32_t ASCSAN_AIC_CORES_PER_DIE = ASCSAN_CORES_PER_DIE / ASCSAN_AIC_CORE_RATIO_DENOMINATOR;
constexpr uint32_t ASCSAN_PHYSICAL_CORE_TOPOLOGY_UNIT =
    ASCSAN_PHYSICAL_CORE_PART_COUNT * ASCSAN_AIC_CORE_RATIO_DENOMINATOR;

enum class DeviceInstructionCategory : uint16_t {
    Invalid = 0,
    MemoryAccess,
    Synchronization,
    RegisterState,
};

constexpr bool IsAicPhysicalCore(uint32_t phyCoreId)
{
    if (phyCoreId >= ASCSAN_PHYSICAL_CORE_SLOT_COUNT) {
        return false;
    }
    const uint32_t dieLocalCoreId = phyCoreId % ASCSAN_CORES_PER_DIE;
    return dieLocalCoreId < ASCSAN_AIC_CORES_PER_DIE;
}

// dav-3510 maps each AIC to two adjacent AIVs in the same die partition.
// Return UINT32_MAX when the core ID or selector is invalid.
constexpr uint32_t AivPhysicalCoreForAic(uint32_t aicPhyCoreId, uint32_t subBlockId)
{
    if (subBlockId > 1U || !IsAicPhysicalCore(aicPhyCoreId)) {
        return UINT32_MAX;
    }
    const uint32_t dieBase = (aicPhyCoreId / ASCSAN_CORES_PER_DIE) * ASCSAN_CORES_PER_DIE;
    const uint32_t aicLocal = aicPhyCoreId % ASCSAN_CORES_PER_DIE;
    return dieBase + ASCSAN_AIC_CORES_PER_DIE + 2U * aicLocal + subBlockId;
}

constexpr uint32_t AicPhysicalCoreForAiv(uint32_t aivPhyCoreId)
{
    if (aivPhyCoreId >= ASCSAN_PHYSICAL_CORE_SLOT_COUNT) {
        return UINT32_MAX;
    }
    const uint32_t dieBase = (aivPhyCoreId / ASCSAN_CORES_PER_DIE) * ASCSAN_CORES_PER_DIE;
    const uint32_t aivLocal = aivPhyCoreId % ASCSAN_CORES_PER_DIE;
    if (aivLocal < ASCSAN_AIC_CORES_PER_DIE) {
        return UINT32_MAX;
    }
    return dieBase + (aivLocal - ASCSAN_AIC_CORES_PER_DIE) / 2U;
}

constexpr uint32_t AivBlockIdForAic(uint32_t aicBlockId, uint32_t subBlockId)
{
    return subBlockId <= 1U && aicBlockId <= (UINT32_MAX - subBlockId) / 2U ? 2U * aicBlockId + subBlockId : UINT32_MAX;
}

constexpr bool IsTraceBlockIdValid(uint64_t blockId, bool isAic, uint32_t blockCount)
{
    if (blockCount == 0U || blockId > UINT32_MAX) {
        return false;
    }
    const uint64_t blockLimit = isAic ? static_cast<uint64_t>(blockCount) : 2ULL * blockCount;
    return blockId < blockLimit;
}

// SIMD starts with interleaved device- and host-owned cache lines.
struct alignas(64) AclsanTraceBufferHeader {
    uint64_t magic;
    uint64_t launchId;
    uint64_t segmentBytes;
    uint32_t blockCount;
    uint32_t physicalCoreCount;
    uint64_t simdSegmentOffset;
    uint64_t simdSegmentBytes;
    uint64_t simtSegmentOffset;
    uint64_t simtSegmentBytes;
};

struct alignas(64) AclsanTraceWriteLine {
    uint32_t writeSequence;
    uint32_t droppedCount;
    uint8_t reserved[56];
};

struct alignas(64) AclsanTraceReadLine {
    uint32_t readSequence;
    uint8_t reserved[60];
};

struct alignas(64) AclsanTraceControl {
    AclsanTraceWriteLine write;
    AclsanTraceReadLine read;
};

struct AclsanRawTraceRecord {
    uint64_t pc;
    uint64_t args[5];
    uint32_t instrId;
    uint32_t siteId;
    DeviceInstructionCategory category;
    uint16_t pipeline;
    uint32_t blockId;
    uint32_t reserved;
    uint32_t padding;
    uint64_t parameterBase;
};

constexpr size_t TraceWriteControlOffset(uint32_t slot)
{
    return slot < ASCSAN_PHYSICAL_CORE_SLOT_COUNT ?
               sizeof(AclsanTraceBufferHeader) + static_cast<size_t>(slot) * sizeof(AclsanTraceControl) :
               SIZE_MAX;
}

constexpr size_t TraceReadControlOffset(uint32_t slot)
{
    return slot < ASCSAN_PHYSICAL_CORE_SLOT_COUNT ? TraceWriteControlOffset(slot) + sizeof(AclsanTraceWriteLine) :
                                                    SIZE_MAX;
}

constexpr size_t TraceRecordOffset(uint32_t slot, uint32_t sequence)
{
    if (slot >= ASCSAN_PHYSICAL_CORE_SLOT_COUNT) {
        return SIZE_MAX;
    }
    return sizeof(AclsanTraceBufferHeader) +
           static_cast<size_t>(ASCSAN_PHYSICAL_CORE_SLOT_COUNT) * sizeof(AclsanTraceControl) +
           (static_cast<size_t>(slot) * ASCSAN_TRACE_RECORDS_PER_CORE + sequence % ASCSAN_TRACE_RECORDS_PER_CORE) *
               sizeof(AclsanRawTraceRecord);
}

constexpr bool TraceBufferBytes(uint32_t physicalCoreCount, size_t* bytes)
{
    if (bytes == nullptr || physicalCoreCount != ASCSAN_PHYSICAL_CORE_SLOT_COUNT) {
        return false;
    }
    *bytes = sizeof(AclsanTraceBufferHeader) +
             static_cast<size_t>(physicalCoreCount) *
                 (sizeof(AclsanTraceControl) + ASCSAN_TRACE_RECORDS_PER_CORE * sizeof(AclsanRawTraceRecord));
    return true;
}

static_assert(sizeof(AclsanTraceBufferHeader) == 64U && alignof(AclsanTraceBufferHeader) == 64U);
static_assert(
    offsetof(AclsanTraceBufferHeader, blockCount) == 24U &&
    offsetof(AclsanTraceBufferHeader, physicalCoreCount) == 28U);
static_assert(
    offsetof(AclsanTraceBufferHeader, simdSegmentOffset) == 32U &&
    offsetof(AclsanTraceBufferHeader, simdSegmentBytes) == 40U &&
    offsetof(AclsanTraceBufferHeader, simtSegmentOffset) == 48U &&
    offsetof(AclsanTraceBufferHeader, simtSegmentBytes) == 56U);
static_assert(sizeof(AclsanTraceWriteLine) == 64U && sizeof(AclsanTraceReadLine) == 64U);
static_assert(sizeof(AclsanTraceControl) == 128U && offsetof(AclsanTraceControl, read) == 64U);
static_assert(
    sizeof(AclsanRawTraceRecord) == 80U && offsetof(AclsanRawTraceRecord, reserved) == 64U &&
    offsetof(AclsanRawTraceRecord, padding) == 68U && offsetof(AclsanRawTraceRecord, parameterBase) == 72U);
static_assert(TraceRecordOffset(0U, 0U) == 13888U);

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_BUFFER_ABI_H
