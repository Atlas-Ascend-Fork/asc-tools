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
constexpr uint32_t RAW_BYTE_BITS = 8;
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

uint32_t ReadLittleEndianWord(const uint8_t* bytes)
{
    uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= uint32_t(bytes[index]) << (index * RAW_BYTE_BITS);
    }
    return value;
}

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
constexpr uint32_t BLOCK_START_FUNCTION = 0x24U;
constexpr uint32_t BLOCK_END_FUNCTION = 0x25U;

// Explicit little-endian reads avoid alignment and native-struct layout assumptions.
uint32_t Word(const std::byte* data, std::size_t index)
{
    const std::byte* bytes = data + index * sizeof(uint32_t);
    uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= uint32_t(std::to_integer<uint8_t>(bytes[index])) << (index * RAW_BYTE_BITS);
    }
    return value;
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

DecodeResult DecodeTaskLog(
    const std::byte* data, uint64_t recordIndex, uint32_t header, uint16_t taskId, uint16_t streamId)
{
    const uint32_t function = header & ((1U << FUNCTION_BITS) - 1U);
    if (function != kTaskStartFunctionType && function != kTaskEndFunctionType && function != BLOCK_START_FUNCTION &&
        function != BLOCK_END_FUNCTION) {
        return DecodeResult(ACLPTI_ERROR_DECODE);
    }

    TaskLog32 record{};
    record.funcType = static_cast<uint8_t>(function);
    record.sqeType = static_cast<uint8_t>((header >> SQE_TYPE_SHIFT) & SQE_TYPE_MASK);
    record.taskId = taskId;
    record.rtStreamId = streamId;
    record.systemCounter = Counter(data, CYCLE_COUNTER_WORD);
    if (function == BLOCK_START_FUNCTION || function == BLOCK_END_FUNCTION) {
        record.blockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD) >> 16U);
        record.subBlockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD));
        record.coreType =
            (Word(data, CORE_ID_WORD) & CORE_TYPE_MASK) == CORE_TYPE_BIT ? ACLPTI_CORE_TYPE_AIC : ACLPTI_CORE_TYPE_AIV;
        record.coreTypeId = static_cast<uint8_t>((Word(data, CORE_ID_WORD) >> CORE_ID_SHIFT) & CORE_ID_MASK);
        // CANN 9.3 BlockLog stores core type at byte 20 and the full core ID at byte 21.
        ASCTOOL_DEBUG(
            "[aclpti-data] block identity word5=0x%08x task=%u stream=%u block=%u subBlock=%u coreId=%u",
            Word(data, CORE_ID_WORD), static_cast<unsigned>(taskId), static_cast<unsigned>(streamId),
            static_cast<unsigned>(record.blockId), static_cast<unsigned>(record.subBlockId),
            static_cast<unsigned>(record.coreTypeId));
    }
    return DecodeResult(DecodedRecord{recordIndex, record});
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

    const uint32_t header = Word(data, 0);
    if ((header >> MAGIC_SHIFT) != PMU_MAGIC) {
        return DecodeResult(ACLPTI_ERROR_DECODE);
    }
    const uint32_t taskAndStream = Word(data, 1);
    const auto taskId = static_cast<uint16_t>(taskAndStream >> 16U);
    const auto streamId = static_cast<uint16_t>(taskAndStream);
    if (size == TASK_LOG_BYTES) {
        return DecodeTaskLog(data, recordIndex, header, taskId, streamId);
    }
    const uint32_t function = header & ((1U << FUNCTION_BITS) - 1U);
    return DecodePmuRecord(data, recordIndex, function, taskId, streamId, pmuEventIds);
}

} // namespace aclpti::data
