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

__aicore__ inline uint32_t CaptureScalarAtomRedSysVaWindow()
{
    const uint64_t sysVaBase = static_cast<uint64_t>(__cce_scalar::get_sys_va_base());
    return static_cast<uint32_t>((sysVaBase >> ASCSAN_SCALAR_SYS_VA_WINDOW_SHIFT) & ASCSAN_SCALAR_SYS_VA_WINDOW_MASK);
}

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
    const bool validTail = header->simtSegmentBytes == 0U ?
                               header->simtSegmentOffset == 0U && header->segmentBytes == ASCSAN_SIMD_TRACE_END :
                               header->simtSegmentOffset == ASCSAN_SIMT_SEGMENT_OFFSET &&
                                   header->simtSegmentBytes == ASCSAN_SIMT_SEGMENT_BYTES &&
                                   header->segmentBytes == ASCSAN_TRACE_END_WITH_SIMT;
    return header->magic == ASCSAN_TRACE_BUFFER_MAGIC && header->launchId != 0U && header->physicalCoreCount == slots &&
           phyCoreId < slots && header->blockCount != 0U &&
           header->simdSegmentOffset == sizeof(AclsanTraceBufferHeader) &&
           header->simdSegmentBytes == ASCSAN_SIMD_SEGMENT_BYTES && validTail;
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
    uint16_t pipeline, uint32_t blockId, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
    uint32_t atomicMetadata = 0U)
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
    record->atomicMetadata = atomicMetadata;
    record->padding = 0U;
    record->parameterBase = static_cast<uint64_t>(get_para_base());
}

// memInfo is the launch-owned segment passed through the hidden probe argument.
__aicore__ inline void WriteTraceRecord(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, DeviceInstructionCategory category, uint16_t pipeline,
    uint16_t apiId, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4,
    uint32_t atomicMetadata = 0U)
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
    WriteTracePayload(record, pc, apiId, category, pipeline, blockId, arg0, arg1, arg2, arg3, arg4, atomicMetadata);
    PublishTraceRecord(record, writeControl, sequence);
}

// A scalar caller normally updates invocation state before the SIMT function.
// Pure SIMT kernels can establish their first invocation lazily in the lane
// writer below, so they do not depend on an application-side scalar call.
__aicore__ inline void WriteSimtCallContext(__gm__ uint8_t* memInfo, int64_t pc)
{
    if (memInfo == nullptr) {
        return;
    }
    auto* trace = reinterpret_cast<__gm__ AclsanTraceBufferHeader*>(memInfo);
    const uint32_t coreId = static_cast<uint32_t>(get_coreid());
    if (!IsTraceSegmentValid(trace, coreId) || trace->simtSegmentBytes != ASCSAN_SIMT_SEGMENT_BYTES) {
        return;
    }
    auto* sidecar = reinterpret_cast<__gm__ const AclsanSimtTraceHeader*>(memInfo + ASCSAN_SIMT_SEGMENT_OFFSET);
    if (sidecar->magic != ASCSAN_SIMT_TRACE_MAGIC || sidecar->version != ASCSAN_SIMT_TRACE_VERSION ||
        sidecar->bytes != ASCSAN_SIMT_SEGMENT_BYTES || sidecar->physicalCoreCount != ASCSAN_PHYSICAL_CORE_SLOT_COUNT ||
        sidecar->threadsPerCore != ASCSAN_SIMT_THREADS_PER_CORE ||
        sidecar->recordsPerThread != ASCSAN_SIMT_RECORDS_PER_THREAD) {
        return;
    }
    auto* core = reinterpret_cast<__gm__ AclsanSimtCoreHeader*>(
        memInfo + ASCSAN_SIMT_SEGMENT_OFFSET + sizeof(AclsanSimtTraceHeader) +
        static_cast<uint64_t>(coreId) * ASCSAN_SIMT_CORE_BYTES);
    if (core->invocationId >= ASCSAN_SIMT_COMPLETION_BITS) {
        core->flags = ASCSAN_SIMT_COVERAGE_INCOMPLETE;
    } else {
        core->invocationId = core->invocationId + 1U;
    }
    core->scalarPc = static_cast<uint64_t>(pc);
    dcci(core, cache_line_t::ENTIRE_DATA_CACHE, dcci_dst_t::CACHELINE_OUT);
}

#if defined(NPU_CHECK_SIMT_PROBE) && defined(__NPU_ARCH__) && __NPU_ARCH__ == 3510 && defined(__DAV_VEC__)
// SIMT helpers must use the same execution mode as the injected callee. All
// stores are lane-private; the scalar writer and its cache intrinsics are not
// called from this path.
__simt_callee__ __aicore__ inline void WriteSimtTraceRecord(
    __gm__ uint8_t* memInfo, int64_t pc, uint32_t bid, uint32_t instructionId, uint64_t base, int64_t offset,
    uint32_t elementBytes)
{
    (void)bid;
    if (memInfo == nullptr) {
        return;
    }
    auto* trace = reinterpret_cast<__gm__ const AclsanTraceBufferHeader*>(memInfo);
    const uint32_t coreId = __cce_simt_get_COREID();
    // The SIMT callee cannot call the scalar layout validator.
    if (trace->magic != ASCSAN_TRACE_BUFFER_MAGIC || trace->launchId == 0U || trace->blockCount == 0U ||
        trace->physicalCoreCount != ASCSAN_PHYSICAL_CORE_SLOT_COUNT || coreId >= ASCSAN_PHYSICAL_CORE_SLOT_COUNT ||
        trace->simdSegmentOffset != sizeof(AclsanTraceBufferHeader) ||
        trace->simdSegmentBytes != ASCSAN_SIMD_SEGMENT_BYTES ||
        trace->simtSegmentOffset != ASCSAN_SIMT_SEGMENT_OFFSET ||
        trace->simtSegmentBytes != ASCSAN_SIMT_SEGMENT_BYTES || trace->segmentBytes != ASCSAN_TRACE_END_WITH_SIMT) {
        return;
    }
    auto* sidecar = reinterpret_cast<__gm__ const AclsanSimtTraceHeader*>(memInfo + ASCSAN_SIMT_SEGMENT_OFFSET);
    if (sidecar->magic != ASCSAN_SIMT_TRACE_MAGIC || sidecar->version != ASCSAN_SIMT_TRACE_VERSION ||
        sidecar->bytes != ASCSAN_SIMT_SEGMENT_BYTES || sidecar->physicalCoreCount != ASCSAN_PHYSICAL_CORE_SLOT_COUNT ||
        sidecar->threadsPerCore != ASCSAN_SIMT_THREADS_PER_CORE ||
        sidecar->recordsPerThread != ASCSAN_SIMT_RECORDS_PER_THREAD) {
        return;
    }
    const uint32_t x = __cce_simt_get_TID_X();
    const uint32_t y = __cce_simt_get_TID_Y();
    const uint32_t z = __cce_simt_get_TID_Z();
    const uint32_t dimX = __cce_simt_get_BLOCK_DIM_X();
    const uint32_t dimY = __cce_simt_get_BLOCK_DIM_Y();
    const uint32_t dimZ = __cce_simt_get_BLOCK_DIM_Z();
    const uint64_t linear = x + static_cast<uint64_t>(dimX) * (y + static_cast<uint64_t>(dimY) * z);
    auto* coreAddress = memInfo + ASCSAN_SIMT_SEGMENT_OFFSET + sizeof(AclsanSimtTraceHeader) +
                        static_cast<uint64_t>(coreId) * ASCSAN_SIMT_CORE_BYTES;
    auto* core = reinterpret_cast<__gm__ AclsanSimtCoreHeader*>(coreAddress);
    // Out-of-budget lanes only set an idempotent sticky flag in partition 0.
    // They never modify its cursor or overwrite another lane's access records.
    auto* thread = reinterpret_cast<__gm__ AclsanSimtThreadHeader*>(
        coreAddress + sizeof(AclsanSimtCoreHeader) +
        (linear < ASCSAN_SIMT_THREADS_PER_CORE ? linear : 0U) * ASCSAN_SIMT_THREAD_BYTES);
    if (linear >= ASCSAN_SIMT_THREADS_PER_CORE || dimX == 0 || dimY == 0 || dimZ == 0 || dimX > UINT16_MAX ||
        dimY > UINT16_MAX || dimZ > UINT16_MAX) {
        thread->invalidContext = 1U;
        return;
    }
    if (core->invocationId == 0) {
        // All lanes store the same first invocation value. This is the pure
        // SIMT kernel-entry path; a scalar SIMT_CALL has already initialized
        // the field for hybrid launches.
        core->invocationId = 1U;
        core->scalarPc = 0U;
    }
    const uint64_t invocation = core->invocationId;
    if (invocation > ASCSAN_SIMT_COMPLETION_BITS) {
        thread->invalidContext = 1U;
        return;
    }
    if (thread->lastInvocation != invocation && thread->lastInvocation > thread->completedInvocation) {
        thread->invalidContext = 1U;
    }
    thread->lastInvocation = invocation;
    if (instructionId == ASCSAN_SIMT_END_INSTRUCTION_ID) {
        thread->completedInvocation = invocation;
        thread->completedInvocations |= 1ULL << (invocation - 1U);
        return;
    }
    if (elementBytes == 0) {
        thread->invalidContext = 1U; // Executed GM-capable instruction outside this phase's coverage.
        return;
    }
    const uint32_t index = thread->recordCount;
    if (index >= ASCSAN_SIMT_RECORDS_PER_THREAD) {
        if (thread->overflowCount != UINT32_MAX) {
            thread->overflowCount = thread->overflowCount + 1U;
        }
        return;
    }
    auto* record = reinterpret_cast<__gm__ AclsanSimtRawTraceRecord*>(
                       reinterpret_cast<__gm__ uint8_t*>(thread) + sizeof(AclsanSimtThreadHeader)) +
                   index;
    record->pc = static_cast<uint64_t>(pc);
    record->base = base;
    record->offset = offset;
    record->invocationId = invocation;
    record->scalarPc = core->scalarPc;
    record->instructionId = instructionId;
    record->blockId = __cce_simt_get_BLOCKID();
    record->threadX = x;
    record->threadY = y;
    record->threadZ = z;
    record->blockDimX = dimX;
    record->blockDimY = dimY;
    record->blockDimZ = dimZ;
    record->reserved = 0;
    // Host consumes this sidecar only after successful stream synchronization.
    thread->recordCount = index + 1U;
}
#endif

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_TRACE_RECORD_H
