/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_RECORD_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_RECORD_H

#include "kernel_operator.h"
#include "trace_buffer_abi.h"

#define BUILD_DYNAMIC_PROBE 1

namespace aclsan {

// Device probe 执行时调用该函数，将记录直接写入当前 launch 独占的 Device GM trace buffer。
// memInfo 是 Host 在 kernel launch 时注入的隐藏参数；binary load 阶段只插入 probe 调用，不产生记录。
__aicore__ inline void WriteTraceRecord(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, DeviceInstructionCategory category, uint16_t pipeline,
    uint16_t apiId, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    if (memInfo == nullptr) {
        return;
    }

    (void)bid;
    __gm__ aclsan::AclsanTraceBufferHeader* header = reinterpret_cast<__gm__ aclsan::AclsanTraceBufferHeader*>(memInfo);
    const uint32_t phyCoreId = static_cast<uint32_t>(get_coreid());
    const uint32_t blockCount = header->blockCount;
    const uint32_t physicalCoreSlotCount = header->physicalCoreSlotCount;
    if (header->magic != aclsan::ASCSAN_TRACE_BUFFER_MAGIC || blockCount == 0U ||
        physicalCoreSlotCount != aclsan::ASCSAN_PHYSICAL_CORE_SLOT_COUNT || phyCoreId >= physicalCoreSlotCount) {
        return;
    }

    const uint32_t blockId = static_cast<uint32_t>(AscendC::GetBlockIdx());
    // dav-3510 numbers each die as one AIC partition followed by two AIV
    // partitions. Keep this expression local to Device code because calling
    // the Host constexpr helper can leave an unresolved symbol in the probe
    // object.
    const uint32_t coresPerDie = aclsan::ASCSAN_CORES_PER_DIE;
    const uint32_t aicCoresPerDie = aclsan::ASCSAN_AIC_CORES_PER_DIE;
    const uint32_t dieLocalCoreId = phyCoreId % coresPerDie;
    const bool isAic = dieLocalCoreId < aicCoresPerDie;
    const uint64_t blockLimit = isAic ? static_cast<uint64_t>(blockCount) : 2ULL * blockCount;
    if (static_cast<uint64_t>(blockId) >= blockLimit) {
        return;
    }

    constexpr uint64_t sliceBytes = aclsan::ASCSAN_TRACE_BYTES_PER_CORE;
    if (header->segmentBytes !=
        sizeof(aclsan::AclsanTraceBufferHeader) + static_cast<uint64_t>(physicalCoreSlotCount) * sliceBytes) {
        return;
    }
    __gm__ uint8_t* sliceAddress =
        memInfo + sizeof(aclsan::AclsanTraceBufferHeader) + static_cast<uint64_t>(phyCoreId) * sliceBytes;
    __gm__ aclsan::AclsanTraceSliceHeader* slice =
        reinterpret_cast<__gm__ aclsan::AclsanTraceSliceHeader*>(sliceAddress);
    const uint32_t index = slice->recordCount;
    if (index >= aclsan::ASCSAN_TRACE_RECORDS_PER_CORE) {
        if (slice->overflowCount != UINT32_MAX) {
            slice->overflowCount = slice->overflowCount + 1U;
        }
        dcci(slice, cache_line_t::ENTIRE_DATA_CACHE, dcci_dst_t::CACHELINE_OUT);
        return;
    }

    __gm__ aclsan::AclsanRawTraceRecord* record =
        reinterpret_cast<__gm__ aclsan::AclsanRawTraceRecord*>(sliceAddress + sizeof(aclsan::AclsanTraceSliceHeader)) +
        index;
    record->pc = static_cast<uint64_t>(pc);
    record->args[0] = arg0;
    record->args[1] = arg1;
    record->args[2] = arg2;
    record->args[3] = arg3;
    record->args[4] = arg4;
    record->instrId = apiId;
    record->siteId = 0U;
    record->category = category;
    record->pipeline = pipeline;
    record->blockId = blockId;
    record->reserved = 0U;
    record->parameterBase = static_cast<uint64_t>(get_para_base());
    dcci(record, cache_line_t::ENTIRE_DATA_CACHE, dcci_dst_t::CACHELINE_OUT);
    slice->phyCoreId = phyCoreId;
    slice->recordCount = index + 1U;
    dcci(slice, cache_line_t::ENTIRE_DATA_CACHE, dcci_dst_t::CACHELINE_OUT);
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_RECORD_H
