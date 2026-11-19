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

constexpr uint32_t ASCSAN_TRACE_WAIT_ROUNDS = 15U;
constexpr uint64_t ASCSAN_TRACE_WAIT_CYCLES = 300000000ULL;

__aicore__ inline void PublishTraceWriteControl(__gm__ AclsanTraceWriteLine* info)
{
    dcci(info, cache_line_t::ENTIRE_DATA_CACHE, dcci_dst_t::CACHELINE_OUT);
    AscendC::DataSyncBarrier<AscendC::MemDsbT::DDR>();
}

__aicore__ inline void DropTraceRecord(__gm__ AclsanTraceWriteLine* info)
{
    if (info->droppedCount != UINT32_MAX) {
        info->droppedCount = info->droppedCount + 1U;
    }
    PublishTraceWriteControl(info);
}

__aicore__ inline bool WaitForTraceSpace(__gm__ AclsanTraceReadLine* readControl, uint32_t sequence)
{
    for (uint32_t round = 0; round <= ASCSAN_TRACE_WAIT_ROUNDS; ++round) {
        const uint32_t readSequence = AscendC::ReadGmBypassDCache<uint32_t>(&readControl->readSequence);
        if (readSequence > sequence || sequence - readSequence > ASCSAN_TRACE_RECORDS_PER_CORE) {
            return false;
        }
        if (sequence - readSequence < ASCSAN_TRACE_RECORDS_PER_CORE) {
            return true;
        }
        if (round == ASCSAN_TRACE_WAIT_ROUNDS) {
            return false;
        }
        const uint64_t start = get_sys_cnt();
        while (static_cast<uint64_t>(get_sys_cnt() - start) < ASCSAN_TRACE_WAIT_CYCLES) {
        }
    }
    return false;
}

__aicore__ inline bool IsTraceSegmentValid(__gm__ AclsanTraceBufferHeader* header, uint32_t phyCoreId)
{
    constexpr uint64_t slots = ASCSAN_PHYSICAL_CORE_SLOT_COUNT;
    constexpr uint64_t simdBytes =
        slots * (sizeof(AclsanTraceControl) +
                 static_cast<uint64_t>(ASCSAN_TRACE_RECORDS_PER_CORE) * sizeof(AclsanRawTraceRecord));
    return header->magic == ASCSAN_TRACE_BUFFER_MAGIC && header->launchId != 0U && header->physicalCoreCount == slots &&
           phyCoreId < slots && header->blockCount != 0U &&
           header->segmentBytes == sizeof(AclsanTraceBufferHeader) + simdBytes &&
           header->simdSegmentOffset == sizeof(AclsanTraceBufferHeader) && header->simdSegmentBytes == simdBytes &&
           header->simtSegmentOffset == 0U && header->simtSegmentBytes == 0U;
}

__aicore__ inline void PublishTraceRecord(
    __gm__ AclsanRawTraceRecord* record, __gm__ AclsanTraceWriteLine* writeControl, uint32_t sequence)
{
    // ENTIRE_DATA_CACHE covers every line of an 80-byte record; E7 validates visibility on hardware.
    dcci(record, cache_line_t::ENTIRE_DATA_CACHE, dcci_dst_t::CACHELINE_OUT);
    AscendC::DataSyncBarrier<AscendC::MemDsbT::DDR>();
    writeControl->writeSequence = sequence + 1U;
    PublishTraceWriteControl(writeControl);
}

__aicore__ inline void WriteTracePayload(
    __gm__ AclsanRawTraceRecord* record, int64_t pc, uint16_t apiId, DeviceInstructionCategory category,
    uint16_t pipeline, uint32_t blockId, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
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
    record->padding = 0U;
    record->parameterBase = static_cast<uint64_t>(get_para_base());
}

// memInfo is the launch-owned segment passed through the hidden probe argument.
__aicore__ inline void WriteTraceRecord(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, DeviceInstructionCategory category, uint16_t pipeline,
    uint16_t apiId, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    if (memInfo == nullptr) {
        return;
    }
    (void)bid;
    __gm__ AclsanTraceBufferHeader* header = reinterpret_cast<__gm__ AclsanTraceBufferHeader*>(memInfo);
    const uint32_t phyCoreId = static_cast<uint32_t>(get_coreid());
    const uint32_t blockCount = header->blockCount;
    constexpr uint64_t slots = ASCSAN_PHYSICAL_CORE_SLOT_COUNT;
    constexpr uint64_t controlBytes = slots * sizeof(AclsanTraceControl);
    if (!IsTraceSegmentValid(header, phyCoreId)) {
        return;
    }
    const uint32_t blockId = static_cast<uint32_t>(AscendC::GetBlockIdx());
    const bool isAic = phyCoreId % ASCSAN_CORES_PER_DIE < ASCSAN_AIC_CORES_PER_DIE;
    const uint64_t blockLimit = isAic ? static_cast<uint64_t>(blockCount) : 2ULL * blockCount;
    if (static_cast<uint64_t>(blockId) >= blockLimit) {
        return;
    }
    __gm__ uint8_t* control =
        memInfo + sizeof(AclsanTraceBufferHeader) + static_cast<uint64_t>(phyCoreId) * sizeof(AclsanTraceControl);
    __gm__ AclsanTraceWriteLine* writeControl = reinterpret_cast<__gm__ AclsanTraceWriteLine*>(control);
    __gm__ AclsanTraceReadLine* readControl =
        reinterpret_cast<__gm__ AclsanTraceReadLine*>(control + sizeof(AclsanTraceWriteLine));
    const uint32_t sequence = writeControl->writeSequence;
    if (sequence == UINT32_MAX || !WaitForTraceSpace(readControl, sequence)) {
        DropTraceRecord(writeControl);
        return;
    }
    __gm__ uint8_t* ringAddress =
        memInfo + sizeof(AclsanTraceBufferHeader) + controlBytes +
        static_cast<uint64_t>(phyCoreId) * ASCSAN_TRACE_RECORDS_PER_CORE * sizeof(AclsanRawTraceRecord);
    __gm__ AclsanRawTraceRecord* record =
        reinterpret_cast<__gm__ AclsanRawTraceRecord*>(ringAddress) + sequence % ASCSAN_TRACE_RECORDS_PER_CORE;
    WriteTracePayload(record, pc, apiId, category, pipeline, blockId, arg0, arg1, arg2, arg3, arg4);
    PublishTraceRecord(record, writeControl, sequence);
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_RECORD_H
