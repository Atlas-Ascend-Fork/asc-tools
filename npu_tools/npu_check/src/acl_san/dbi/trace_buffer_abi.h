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

constexpr uint64_t ASCSAN_TRACE_BUFFER_MAGIC = 0x41534353414E3038ULL;
constexpr size_t ASCSAN_TRACE_BYTES_PER_CORE = 1024U * 1024U;
// dav-3510 has two dies.  Each die numbers its physical cores as one AIC
// partition followed by two AIV partitions.  The global order is therefore
// AIC:AIV | AIC:AIV, with a 1:2 ratio inside each die.
constexpr uint32_t ASCSAN_PHYSICAL_CORE_PART_COUNT = 2U;
constexpr uint32_t ASCSAN_AIC_CORE_RATIO_DENOMINATOR = 3U;
constexpr uint32_t ASCSAN_PHYSICAL_CORE_TOPOLOGY_UNIT =
    ASCSAN_PHYSICAL_CORE_PART_COUNT * ASCSAN_AIC_CORE_RATIO_DENOMINATOR;

enum class DeviceInstructionCategory : uint16_t {
    Invalid = 0,
    MemoryAccess,
    Synchronization,
    RegisterState,
};

constexpr bool IsTracePhysicalCoreTopologyValid(uint32_t physicalCoreCount)
{
    return physicalCoreCount != 0U && physicalCoreCount % ASCSAN_PHYSICAL_CORE_TOPOLOGY_UNIT == 0U;
}

constexpr bool IsAicPhysicalCore(uint32_t phyCoreId, uint32_t physicalCoreCount)
{
    if (!IsTracePhysicalCoreTopologyValid(physicalCoreCount) || phyCoreId >= physicalCoreCount) {
        return false;
    }
    const uint32_t coresPerDie = physicalCoreCount / ASCSAN_PHYSICAL_CORE_PART_COUNT;
    const uint32_t aicCoresPerDie = coresPerDie / ASCSAN_AIC_CORE_RATIO_DENOMINATOR;
    const uint32_t dieLocalCoreId = phyCoreId % coresPerDie;
    return dieLocalCoreId < aicCoresPerDie;
}

// dav-3510 maps each AIC to two adjacent AIVs in the same die partition.
// Return UINT32_MAX when the topology or selector is invalid.
constexpr uint32_t AivPhysicalCoreForAic(uint32_t aicPhyCoreId, uint32_t subBlockId, uint32_t physicalCoreCount)
{
    if (subBlockId > 1U || !IsAicPhysicalCore(aicPhyCoreId, physicalCoreCount)) {
        return UINT32_MAX;
    }
    const uint32_t coresPerDie = physicalCoreCount / ASCSAN_PHYSICAL_CORE_PART_COUNT;
    const uint32_t aicCoresPerDie = coresPerDie / ASCSAN_AIC_CORE_RATIO_DENOMINATOR;
    const uint32_t dieBase = (aicPhyCoreId / coresPerDie) * coresPerDie;
    const uint32_t aicLocal = aicPhyCoreId % coresPerDie;
    return dieBase + aicCoresPerDie + 2U * aicLocal + subBlockId;
}

constexpr uint32_t AicPhysicalCoreForAiv(uint32_t aivPhyCoreId, uint32_t physicalCoreCount)
{
    if (!IsTracePhysicalCoreTopologyValid(physicalCoreCount) || aivPhyCoreId >= physicalCoreCount) {
        return UINT32_MAX;
    }
    const uint32_t coresPerDie = physicalCoreCount / ASCSAN_PHYSICAL_CORE_PART_COUNT;
    const uint32_t aicCoresPerDie = coresPerDie / ASCSAN_AIC_CORE_RATIO_DENOMINATOR;
    const uint32_t dieBase = (aivPhyCoreId / coresPerDie) * coresPerDie;
    const uint32_t aivLocal = aivPhyCoreId % coresPerDie;
    if (aivLocal < aicCoresPerDie) {
        return UINT32_MAX;
    }
    return dieBase + (aivLocal - aicCoresPerDie) / 2U;
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

// 每次已插桩 kernel launch 当前独占一个 trace segment。Host 在 launch 前初始化 header 并拷贝到
// Device GM，Device probe 执行时只写入本次 launch 注入的 segment。segmentBytes 明确给出
// [segment base, segment base + segmentBytes) 边界；未来多个 segment 可在共享 buffer 中连续存放。
// Buffer 的三层布局：
//   [AclsanTraceBufferHeader]
//   [physical core slice 0: AclsanTraceSliceHeader][records][padding to ASCSAN_TRACE_BYTES_PER_CORE]
//   ...
//
// 大小及偏移计算：
//   sliceBytes  = ASCSAN_TRACE_BYTES_PER_CORE
//   sliceCount  = physicalCoreCount
//   segmentBytes = sizeof(AclsanTraceBufferHeader) + sliceCount * sliceBytes
//   sliceOffset(sliceIndex) = sizeof(AclsanTraceBufferHeader) + sliceIndex * sliceBytes
//
// blockCount 保存 launch blockDim A，只用于校验 record 携带的逻辑 blockId。
// dav-3510 的物理核按两个 die 分段排列：每个 die 内前 1/3 为 AIC、后 2/3 为 AIV，
// 即 AIC、AIV | AIC、AIV。
// sliceIndex 等于 get_coreid()。每个物理核执行的所有逻辑 block 共享一个 slice，
// 有效记录下标范围为 [0, recordCount)。
// recordCount 即该 slice 的有效结尾，不额外写入 record 或 launch 尾标志。
struct AclsanTraceBufferHeader {
    uint64_t magic;        // Trace segment 格式标识，固定为 ASCSAN_TRACE_BUFFER_MAGIC。
    uint64_t launchId;     // Host 为本次 launch 分配的标识；该 segment 内所有 record 均继承此归属。
    uint64_t segmentBytes; // 包含 header 和所有定长 slice 的完整 segment 字节数。
    uint32_t blockCount;   // 本次 kernel launch 的 blockDim，即 A。
    uint32_t physicalCoreCount; // Host 查询得到的 Cube 与 Vector 物理核数量之和。
};

// 新代码可使用 segment 术语；保留 AclsanTraceBufferHeader 名称以兼容现有调用点。
using AclsanTraceSegmentHeader = AclsanTraceBufferHeader;

// 单个物理 slice 的写入状态。phyCoreId 仅在 recordCount 大于 0 时有效。
struct AclsanTraceSliceHeader {
    uint32_t recordCount;   // 当前 slice 中已写入的有效记录数。
    uint32_t overflowCount; // Slice 容量用尽后丢弃的记录数，最大饱和至 UINT32_MAX。
    uint32_t phyCoreId;     // Device 通过 get_coreid() 获取的物理核 ID。
    uint32_t reserved;      // 保持后续 AclsanRawTraceRecord 的 64 位对齐，固定写 0。
};

// Device probe 生成的一条原始 trace 记录，固定占用 80 字节。record 不重复保存 launchId；Host 解析时
// 从所属 AclsanTraceBufferHeader 取得 launchId，并附加到 ParsedTraceRecord。
//   [pc: 8][args[0..4]: 40][instrId: 4][siteId: 4][category: 2][pipeline: 2][blockId: 4][reserved: 4][padding:
//   4][parameterBase: 8]
struct AclsanRawTraceRecord {
    uint64_t pc;                        // 被插桩指令的 PC。
    uint64_t args[5];                   // Probe 捕获的指令参数，具体含义由对应指令协议定义。
    uint32_t instrId;                   // 指令标识；probe 写入 apiId，Host 按 InstructionId 解析。
    uint32_t siteId;                    // 插桩点标识，用于关联 patch 元数据；当前写入固定填 0。
    DeviceInstructionCategory category; // 指令功能分类。
    uint16_t pipeline;                  // 指令实际执行的 Device PIPE_* 值。
    uint32_t blockId;                   // AscendC::GetBlockIdx() 返回的逻辑 block ID。
    uint32_t reserved;                  // 显式保留字段，固定写 0。
    uint64_t parameterBase;             // Internal launch parameter region, captured on the executing core.
};

constexpr uint32_t ASCSAN_TRACE_RECORDS_PER_CORE = static_cast<uint32_t>(
    (ASCSAN_TRACE_BYTES_PER_CORE - sizeof(AclsanTraceSliceHeader)) / sizeof(AclsanRawTraceRecord));

constexpr bool TraceBufferBytes(uint32_t physicalCoreCount, size_t* bytes)
{
    if (bytes == nullptr || !IsTracePhysicalCoreTopologyValid(physicalCoreCount)) {
        return false;
    }
    *bytes = sizeof(AclsanTraceBufferHeader) + static_cast<size_t>(physicalCoreCount) * ASCSAN_TRACE_BYTES_PER_CORE;
    return true;
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_BUFFER_ABI_H
