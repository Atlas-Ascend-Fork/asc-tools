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
#if defined(NPU_CHECK_SIMT_PROBE) && defined(__NPU_ARCH__) && __NPU_ARCH__ == 3510 && defined(__DAV_VEC__)
#include "__clang_cce_simt_atomic.h"
#endif

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
    const bool fifo = sidecar->version == ASCSAN_SIMT_FIFO_VERSION;
    if (sidecar->magic != ASCSAN_SIMT_TRACE_MAGIC || (!fifo && sidecar->version != ASCSAN_SIMT_TRACE_VERSION) ||
        sidecar->bytes != ASCSAN_SIMT_SEGMENT_BYTES || sidecar->physicalCoreCount != ASCSAN_PHYSICAL_CORE_SLOT_COUNT ||
        sidecar->threadsPerCore != ASCSAN_SIMT_THREADS_PER_CORE ||
        sidecar->recordsPerThread != (fifo ? ASCSAN_SIMT_FIFO_CAPACITY : ASCSAN_SIMT_RECORDS_PER_THREAD)) {
        return;
    }
    auto* core = reinterpret_cast<__gm__ AclsanSimtCoreHeader*>(
        memInfo + ASCSAN_SIMT_SEGMENT_OFFSET + sizeof(AclsanSimtTraceHeader) +
        static_cast<uint64_t>(coreId) * ASCSAN_SIMT_CORE_BYTES);
    if (core->invocationId == UINT64_MAX || (!fifo && core->invocationId >= ASCSAN_SIMT_COMPLETION_BITS)) {
        core->flags = ASCSAN_SIMT_COVERAGE_INCOMPLETE;
    } else {
        core->invocationId = core->invocationId + 1U;
    }
    core->scalarPc = static_cast<uint64_t>(pc);
    dcci(core, cache_line_t::ENTIRE_DATA_CACHE, dcci_dst_t::CACHELINE_OUT);
}

#if defined(NPU_CHECK_SIMT_PROBE) && defined(__NPU_ARCH__) && __NPU_ARCH__ == 3510 && defined(__DAV_VEC__)
constexpr uint32_t SIMT_FIFO_LANE_ACTIVE = 1U;
constexpr uint32_t SIMT_FIFO_LANE_COLLIDED = 2U;

__simt_callee__ __aicore__ inline uint32_t LoadSimtControl(const __gm__ uint32_t* value)
{
    return static_cast<uint32_t>(__builtin_cce_ldg_uncache_s32(
        reinterpret_cast<__gm__ int32_t*>(const_cast<__gm__ uint32_t*>(value)),
        static_cast<uint32_t>(LD_L2CacheType::L2_CACHE_HINT_NOTALLOC_KEEP)));
}

__simt_callee__ __aicore__ inline void StoreSimtControl(__gm__ uint32_t* value, uint32_t data)
{
    __builtin_cce_stg_uncache_b32(
        reinterpret_cast<__gm__ int32_t*>(value), static_cast<int32_t>(data),
        static_cast<uint32_t>(ST_L2CacheType::L2_CACHE_HINT_WTS_FV));
}

__simt_callee__ __aicore__ inline void StoreSimtFifoPayload(
    __gm__ AclsanSimtRawTraceRecord* record, const AclsanSimtRawTraceRecord& data)
{
    // Non-cacheable, write-through stores allow the DMA consumer to read the payload after publication.
    auto* words = reinterpret_cast<__gm__ int64_t*>(record);
    constexpr auto shared = static_cast<uint32_t>(ST_L2CacheType::L2_CACHE_HINT_WTS_FV);
    __builtin_cce_stg_uncache_b64(words, static_cast<int64_t>(data.pc), shared);
    __builtin_cce_stg_uncache_b64(words + 1, static_cast<int64_t>(data.base), shared);
    __builtin_cce_stg_uncache_b64(words + 2, data.offset, shared);
    __builtin_cce_stg_uncache_b64(words + 3, static_cast<int64_t>(data.invocationId), shared);
    __builtin_cce_stg_uncache_b64(words + 4, static_cast<int64_t>(data.scalarPc), shared);
    const uint64_t instructionAndBlock = data.instructionId | (static_cast<uint64_t>(data.blockId) << 32U);
    const uint64_t lane = data.threadX | (static_cast<uint64_t>(data.threadY) << 16U) |
                          (static_cast<uint64_t>(data.threadZ) << 32U) | (static_cast<uint64_t>(data.blockDimX) << 48U);
    const uint64_t dimensionsAndKind =
        data.blockDimY | (static_cast<uint64_t>(data.blockDimZ) << 16U) | (static_cast<uint64_t>(data.reserved) << 32U);
    __builtin_cce_stg_uncache_b64(words + 5, static_cast<int64_t>(instructionAndBlock), shared);
    __builtin_cce_stg_uncache_b64(words + 6, static_cast<int64_t>(lane), shared);
    __builtin_cce_stg_uncache_b64(words + 7, static_cast<int64_t>(dimensionsAndKind), shared);
}

__simt_callee__ __aicore__ inline void WriteSimtFifoRecord(
    __gm__ uint8_t* coreAddress, AclsanSimtRawTraceRecord payload, uint32_t elementBytes)
{
    const uint32_t lanes = static_cast<uint32_t>(payload.blockDimX) * payload.blockDimY * payload.blockDimZ;
    const uint32_t linear =
        payload.threadX + static_cast<uint32_t>(payload.blockDimX) *
                              (payload.threadY + static_cast<uint32_t>(payload.blockDimY) * payload.threadZ);
    const uint32_t slot = (static_cast<uint64_t>(payload.blockId) * lanes + linear) % ASCSAN_SIMT_THREADS_PER_CORE;
    auto* write = reinterpret_cast<__gm__ AclsanSimtFifoWriteControl*>(coreAddress + SimtFifoWriteOffset) + slot;
    auto* read = reinterpret_cast<__gm__ const uint32_t*>(coreAddress + SimtFifoReadOffset) + slot;
    auto* lane = reinterpret_cast<__gm__ AclsanSimtFifoLane*>(coreAddress + SimtFifoLaneOffset) + slot;
    const bool begin = payload.instructionId == ASCSAN_SIMT_START_INSTRUCTION_ID;
    const bool end = payload.instructionId == ASCSAN_SIMT_END_INSTRUCTION_ID;
    constexpr auto shared = ST_L2CacheType::L2_CACHE_HINT_WTS_FV;
    if (begin) {
        if (atomicCAS<shared>(&lane->owner, 0U, SIMT_FIFO_LANE_ACTIVE) != 0U) {
            // Without an unambiguous owner, discard later records from this slot.
            atomicExch<shared>(&lane->owner, SIMT_FIFO_LANE_COLLIDED);
            StoreSimtControl(&write->droppedCount, 1U);
            return;
        }
        auto* core = reinterpret_cast<__gm__ const volatile AclsanSimtCoreHeader*>(coreAddress);
        const uint64_t invocation = core->invocationId;
        lane->scalarPc = core->scalarPc;
        lane->invocationId = invocation == 0U ? 1U : invocation;
        lane->blockId = payload.blockId;
        if (core->flags != 0U || core->invocationId != invocation) {
            atomicExch<shared>(&lane->owner, SIMT_FIFO_LANE_COLLIDED);
            StoreSimtControl(&write->droppedCount, 1U);
            return;
        }
    } else if (
        LoadSimtControl(&lane->owner) != SIMT_FIFO_LANE_ACTIVE || lane->blockId != payload.blockId ||
        (!end && elementBytes == 0U)) {
        StoreSimtControl(&write->droppedCount, 1U);
        return;
    }

    const uint32_t sequence = LoadSimtControl(&write->writeSequence);
    const uint32_t ack = LoadSimtControl(read);
    if (sequence == UINT32_MAX || ack > sequence || sequence - ack >= ASCSAN_SIMT_FIFO_CAPACITY) {
        StoreSimtControl(&write->droppedCount, 1U);
    } else {
        payload.invocationId = lane->invocationId;
        payload.scalarPc = lane->scalarPc;
        payload.reserved = static_cast<uint32_t>(
            begin ? AclsanSimtRecordKind::Begin :
            end   ? AclsanSimtRecordKind::End :
                    AclsanSimtRecordKind::Memory);
        auto* record = reinterpret_cast<__gm__ AclsanSimtRawTraceRecord*>(coreAddress + SimtFifoRecordsOffset) +
                       slot * ASCSAN_SIMT_FIFO_CAPACITY + sequence % ASCSAN_SIMT_FIFO_CAPACITY;
        StoreSimtFifoPayload(record, payload);
        __threadfence();
        StoreSimtControl(&write->writeSequence, sequence + 1U);
    }
    if (end) {
        __threadfence();
        atomicCAS<shared>(&lane->owner, SIMT_FIFO_LANE_ACTIVE, 0U);
    }
}

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
    const bool fifo = sidecar->version == ASCSAN_SIMT_FIFO_VERSION;
    if (sidecar->magic != ASCSAN_SIMT_TRACE_MAGIC || (!fifo && sidecar->version != ASCSAN_SIMT_TRACE_VERSION) ||
        sidecar->bytes != ASCSAN_SIMT_SEGMENT_BYTES || sidecar->physicalCoreCount != ASCSAN_PHYSICAL_CORE_SLOT_COUNT ||
        sidecar->threadsPerCore != ASCSAN_SIMT_THREADS_PER_CORE ||
        sidecar->recordsPerThread != (fifo ? ASCSAN_SIMT_FIFO_CAPACITY : ASCSAN_SIMT_RECORDS_PER_THREAD)) {
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
    if (fifo) {
        if (dimX == 0U || dimY == 0U || dimZ == 0U || x >= dimX || y >= dimY || z >= dimZ ||
            static_cast<uint64_t>(dimX) * dimY * dimZ > ASCSAN_SIMT_THREADS_PER_CORE) {
            auto* control = reinterpret_cast<__gm__ AclsanSimtFifoWriteControl*>(coreAddress + SimtFifoWriteOffset);
            StoreSimtControl(&control->droppedCount, 1U);
            return;
        }
        AclsanSimtRawTraceRecord payload{
            static_cast<uint64_t>(pc),
            base,
            offset,
            0U,
            0U,
            instructionId,
            static_cast<uint32_t>(__cce_simt_get_BLOCKID()),
            static_cast<uint16_t>(x),
            static_cast<uint16_t>(y),
            static_cast<uint16_t>(z),
            static_cast<uint16_t>(dimX),
            static_cast<uint16_t>(dimY),
            static_cast<uint16_t>(dimZ),
            0U};
        WriteSimtFifoRecord(coreAddress, payload, elementBytes);
        return;
    }
    if (instructionId == ASCSAN_SIMT_START_INSTRUCTION_ID) {
        return;
    }
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
