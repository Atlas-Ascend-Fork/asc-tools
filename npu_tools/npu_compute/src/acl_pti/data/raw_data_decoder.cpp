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

#include <map>

namespace aclpti::data {
uint32_t ReadLittleEndianWord(const uint8_t* bytes)
{
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8U) | (uint32_t(bytes[2]) << 16U) | (uint32_t(bytes[3]) << 24U);
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
constexpr uint32_t TASK_START_FUNCTION = 0x00U;
constexpr uint32_t TASK_END_FUNCTION = 0x01U;
constexpr uint32_t BLOCK_START_FUNCTION = 0x24U;
constexpr uint32_t BLOCK_END_FUNCTION = 0x25U;

// Explicit little-endian reads avoid alignment and native-struct layout assumptions.
uint32_t Word(const std::byte* data, std::size_t index)
{
    const std::byte* bytes = data + index * sizeof(uint32_t);
    uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= uint32_t(std::to_integer<uint8_t>(bytes[index])) << (index * 8U);
    }
    return value;
}

uint64_t Counter(const std::byte* data, std::size_t lowWord)
{
    return Word(data, lowWord) | (uint64_t(Word(data, lowWord + 1)) << 32U);
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

    const uint32_t function = Word(data, 0) & 0x3fU;
    const uint32_t taskAndStream = Word(data, 1);
    const auto taskId = static_cast<uint16_t>(taskAndStream >> 16U);
    const auto streamId = static_cast<uint16_t>(taskAndStream);
    if (size == TASK_LOG_BYTES) {
        if ((Word(data, 0) >> 16U) != PMU_MAGIC ||
            (function != TASK_START_FUNCTION && function != TASK_END_FUNCTION && function != BLOCK_START_FUNCTION &&
             function != BLOCK_END_FUNCTION)) {
            return DecodeResult(ACLPTI_ERROR_DECODE);
        }

        TaskLog32 record{};
        record.funcType = static_cast<uint8_t>(function);
        record.taskId = taskId;
        record.rtStreamId = streamId;
        record.systemCounter = Counter(data, CYCLE_COUNTER_WORD);
        if (function == BLOCK_START_FUNCTION || function == BLOCK_END_FUNCTION) {
            record.blockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD) >> 16U);
            record.subBlockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD));
            record.coreType = (Word(data, CORE_ID_WORD) & 1U) == 0 ? ACLPTI_CORE_TYPE_AIC : ACLPTI_CORE_TYPE_AIV;
            record.coreTypeId = static_cast<uint8_t>((Word(data, CORE_ID_WORD) >> 1U) & 0x7fU);
        }
        return DecodeResult(DecodedRecord{recordIndex, record});
    }

    if ((Word(data, 0) >> 16U) != PMU_MAGIC ||
        (function != kBlockPmuFunctionType && function != kTaskPmuFunctionType)) {
        return DecodeResult(ACLPTI_ERROR_DECODE);
    }

    PmuRecord128 record{};
    record.funcType = static_cast<uint8_t>(function);
    record.taskId = taskId;
    record.rtStreamId = streamId;
    record.totalCycles = Counter(data, CYCLE_COUNTER_WORD);
    record.taskStartSystemCounter = Counter(data, TASK_START_COUNTER_WORD);
    record.taskEndSystemCounter = Counter(data, TASK_END_COUNTER_WORD);
    record.overflow = (Word(data, OVERFLOW_WORD) & (1U << 10U)) != 0;
    record.coreType = (Word(data, CORE_ID_WORD) & 1U) == 0 ? ACLPTI_CORE_TYPE_AIC : ACLPTI_CORE_TYPE_AIV;
    record.coreId = static_cast<uint8_t>(Word(data, CORE_ID_WORD) >> 8U);
    record.blockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD) >> 16U);
    record.subBlockId = static_cast<uint16_t>(Word(data, BLOCK_ID_WORD));
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
    return DecodeResult(DecodedRecord{recordIndex, record});
}

} // namespace aclpti::data
