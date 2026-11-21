/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "raw_data_decoder.h"
#include "npu_compute_log.h"

#include <map>

namespace aclpti::data {
namespace {
constexpr uint32_t FUNCTION_BITS = 6;
constexpr uint32_t SQE_TYPE_SHIFT = 10;
constexpr uint32_t SQE_TYPE_MASK = 0x3fU;
constexpr uint32_t CORE_TYPE_BIT = 0;
constexpr uint32_t CORE_ID_SHIFT = 8;
constexpr uint32_t CORE_TYPE_MASK = 1;
constexpr uint32_t CORE_ID_MASK = 0xffU;
constexpr uint32_t OVERFLOW_BIT = 10;
constexpr uint32_t MAGIC_SHIFT = 16;
constexpr uint32_t WORD_BITS = 32;
} // namespace

uint32_t ReadLittleEndianWord(const uint8_t* bytes) { return ReadLittleEndian<uint32_t>(bytes); }

namespace {
// Word indices in the little-endian task-log / PMU record ABI.
constexpr std::size_t CYCLE_COUNTER_WORD = 2;
constexpr std::size_t OVERFLOW_WORD = 4;
constexpr std::size_t CORE_ID_WORD = 5;
constexpr std::size_t BLOCK_ID_WORD = 6;
constexpr std::size_t PMU_EVENT_COUNTER_WORD = 8;
constexpr std::size_t TASK_START_COUNTER_WORD = 28;
constexpr std::size_t TASK_END_COUNTER_WORD = 30;
constexpr std::size_t COUNTER_WORDS = sizeof(uint64_t) / sizeof(uint32_t);

constexpr uint32_t PMU_MAGIC = 0x6bd3U;
// Chip6 fusion payload word 4 contains engine mask[4:0] and mission[8:5].
constexpr uint32_t FUSION_TYPE_MASK = 0x1fU;
constexpr uint32_t MISSION_SHIFT = 5;
constexpr uint32_t MISSION_MASK = 0xfU;
constexpr std::size_t FUSION_WORD = 4;

// Explicit little-endian reads avoid alignment and native-struct layout assumptions.
uint32_t Word(const std::byte* data, std::size_t index)
{
    return ReadLittleEndian<uint32_t>(data + index * sizeof(uint32_t));
}

uint64_t Counter(const std::byte* data, std::size_t lowWord)
{
    return Word(data, lowWord) | (uint64_t(Word(data, lowWord + 1)) << WORD_BITS);
}

void DecodePmuEvents(const std::byte* data, const PmuSlots& pmuEventIds, PmuRecord128& record)
{
    struct EventAccumulator {
        long double sum = 0.0L;
        std::size_t count = 0;
    };
    // Repeated event IDs occupy multiple hardware slots; average them instead of double counting.
    std::map<uint32_t, EventAccumulator> eventValues;
    for (std::size_t index = 0; index < kMaxPmuSlots; ++index) {
        const uint32_t eventId = pmuEventIds[index];
        if (eventId == kInvalidPmuEvent) {
            break;
        }
        auto& accumulator = eventValues[eventId];
        accumulator.sum += static_cast<long double>(Counter(data, PMU_EVENT_COUNTER_WORD + index * COUNTER_WORDS));
        ++accumulator.count;
    }
    for (const auto& [eventId, accumulator] : eventValues) {
        record.pmuValues.emplace(
            eventId, static_cast<double>(accumulator.sum / static_cast<long double>(accumulator.count)));
    }
}

DecodeResult DecodePmuRecord(
    const std::byte* data, uint64_t recordIndex, uint32_t function, uint16_t taskId, uint16_t streamId,
    const PmuSlots& pmuEventIds)
{
    if (function != kBlockPmuFunctionType && function != kTaskPmuFunctionType) {
        return DecodeResult(ACLPTI_ERROR_DECODE);
    }

    PmuRecord128 record{};
    record.funcType = static_cast<uint8_t>(function);
    record.taskId = taskId;
    record.rtStreamId = streamId;
    record.totalCycles = Counter(data, CYCLE_COUNTER_WORD);
    record.taskStartSystemCounter = Counter(data, TASK_START_COUNTER_WORD);
    record.taskEndSystemCounter = Counter(data, TASK_END_COUNTER_WORD);
    record.overflow = (Word(data, OVERFLOW_WORD) & (1U << OVERFLOW_BIT)) != 0;
    record.coreType =
        (Word(data, CORE_ID_WORD) & CORE_TYPE_MASK) == CORE_TYPE_BIT ? ACLPTI_CORE_TYPE_AIC : ACLPTI_CORE_TYPE_AIV;
    record.coreId = static_cast<uint8_t>(Word(data, CORE_ID_WORD) >> CORE_ID_SHIFT);
    record.blockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD) >> 16U);
    record.subBlockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD));
    DecodePmuEvents(data, pmuEventIds, record);
    return DecodeResult(DecodedRecord{recordIndex, record});
}
} // namespace

aclptiResult DecodeTaskLog(const std::byte* data, std::size_t size, TaskLog32& record)
{
    if (data == nullptr || size != TASK_LOG_BYTES) {
        return ACLPTI_ERROR_INVALID_RAW_DATA;
    }
    const uint32_t header = Word(data, 0);
    if ((header >> MAGIC_SHIFT) != PMU_MAGIC) {
        return ACLPTI_ERROR_DECODE;
    }
    record = {};
    record.funcType = static_cast<uint8_t>(header & ((1U << FUNCTION_BITS) - 1U));
    record.fullTaskId = Word(data, 1);
    record.taskId = static_cast<uint16_t>(record.fullTaskId >> 16U);
    record.rtStreamId = static_cast<uint16_t>(record.fullTaskId);
    record.systemCounter = Counter(data, CYCLE_COUNTER_WORD);
    record.sqeType = static_cast<uint8_t>((header >> SQE_TYPE_SHIFT) & SQE_TYPE_MASK);
    if (record.funcType == kFusionStartFunction || record.funcType == kFusionEndFunction) {
        const uint32_t fusion = Word(data, FUSION_WORD);
        record.fusionType = static_cast<uint8_t>(fusion & FUSION_TYPE_MASK);
        record.missionId = static_cast<uint8_t>((fusion >> MISSION_SHIFT) & MISSION_MASK);
    }
    if (record.IsBlock()) {
        record.blockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD) >> 16U);
        record.subBlockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD));
        record.coreType =
            (Word(data, CORE_ID_WORD) & CORE_TYPE_MASK) == CORE_TYPE_BIT ? ACLPTI_CORE_TYPE_AIC : ACLPTI_CORE_TYPE_AIV;
        record.coreTypeId = static_cast<uint8_t>((Word(data, CORE_ID_WORD) >> CORE_ID_SHIFT) & CORE_ID_MASK);
        // CANN 9.3 BlockLog stores core type at byte 20 and the full core ID at byte 21.
        ASCTOOL_DEBUG(
            "[aclpti-data] block identity word5=0x%08x task=%u stream=%u block=%u subBlock=%u coreId=%u",
            Word(data, CORE_ID_WORD), static_cast<unsigned>(record.taskId), static_cast<unsigned>(record.rtStreamId),
            static_cast<unsigned>(record.blockId), static_cast<unsigned>(record.subBlockId),
            static_cast<unsigned>(record.coreTypeId));
    }
    return ACLPTI_SUCCESS;
}

DecodeResult DecodeRawRecord(const std::byte* data, std::size_t size, uint64_t recordIndex)
{
    PmuSlots noEvents{};
    noEvents.fill(kInvalidPmuEvent);
    return DecodeRawRecord(data, size, recordIndex, noEvents);
}

DecodeResult DecodeRawRecord(const std::byte* data, std::size_t size, uint64_t recordIndex, const PmuSlots& pmuEventIds)
{
    if (data == nullptr || (size != TASK_LOG_BYTES && size != PMU_RECORD_BYTES)) {
        return DecodeResult(ACLPTI_ERROR_INVALID_RAW_DATA);
    }

    if (size == TASK_LOG_BYTES) {
        TaskLog32 record{};
        const auto status = DecodeTaskLog(data, size, record);
        if (status != ACLPTI_SUCCESS) {
            return DecodeResult(status);
        }
        // Replay aggregation consumes ordinary tasks and blocks. Application also consumes fusion tasks.
        if (record.funcType != kTaskStartFunctionType && record.funcType != kTaskEndFunctionType && !record.IsBlock()) {
            return DecodeResult(ACLPTI_ERROR_NOT_SUPPORTED);
        }
        return DecodeResult(DecodedRecord{recordIndex, record});
    }

    const uint32_t header = Word(data, 0);
    if ((header >> MAGIC_SHIFT) != PMU_MAGIC) {
        return DecodeResult(ACLPTI_ERROR_DECODE);
    }
    const uint32_t taskAndStream = Word(data, 1);
    const auto taskId = static_cast<uint16_t>(taskAndStream >> 16U);
    const auto streamId = static_cast<uint16_t>(taskAndStream);
    const uint32_t function = header & ((1U << FUNCTION_BITS) - 1U);
    return DecodePmuRecord(data, recordIndex, function, taskId, streamId, pmuEventIds);
}

} // namespace aclpti::data
