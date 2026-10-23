/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "acl_pti/data/profiling_data_manager.h"
#include "acl_pti/data/raw_data_decoder.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace data = aclpti::data;

void StoreWord(std::byte* data, std::size_t index, uint32_t value)
{
    const std::size_t offset = index * sizeof(value);
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        data[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }
}

template <std::size_t Size>
MsprofRawData RawData(const std::array<std::byte, Size>& bytes, RawDataType type, std::int32_t deviceId = 0)
{
    MsprofRawData raw{};
    raw.isLastChunk = false;
    raw.chunkModule = 7;
    raw.deviceId = deviceId;
    raw.type = type;
    raw.chunkSize = Size;
    std::memcpy(raw.chunk, bytes.data(), Size);
    return raw;
}

std::array<std::byte, 32> TaskLog(
    uint8_t funcType, uint16_t taskId, uint16_t streamId, uint64_t systemCounter, uint16_t blockId, uint16_t subBlockId,
    aclptiCoreType coreType, uint8_t coreTypeId)
{
    std::array<std::byte, 32> bytes{};
    StoreWord(bytes.data(), 0, 0x6bd30000U | funcType);
    StoreWord(bytes.data(), 1, (uint32_t(taskId) << 16U) | streamId);
    StoreWord(bytes.data(), 2, static_cast<uint32_t>(systemCounter));
    StoreWord(bytes.data(), 3, static_cast<uint32_t>(systemCounter >> 32U));
    StoreWord(bytes.data(), 5, (uint32_t(coreTypeId) << 1U) | (coreType == ACLPTI_CORE_TYPE_AIV ? 1U : 0U));
    StoreWord(bytes.data(), 6, (uint32_t(blockId) << 16U) | subBlockId);
    return bytes;
}

std::array<std::byte, 128> PmuRecord(
    uint16_t taskId, uint16_t streamId, uint16_t blockId, uint64_t firstCounter, uint8_t funcType = 0x29U)
{
    std::array<std::byte, 128> bytes{};
    StoreWord(bytes.data(), 0, 0x6bd30000U | funcType);
    StoreWord(bytes.data(), 1, (uint32_t(taskId) << 16U) | streamId);
    StoreWord(bytes.data(), 2, 100U);
    StoreWord(bytes.data(), 5, 3U << 8U);
    StoreWord(bytes.data(), 6, uint32_t(blockId) << 16U);
    for (std::size_t index = 0; index < data::kMaxPmuSlots; ++index) {
        const uint64_t value = firstCounter + index;
        StoreWord(bytes.data(), 8 + index * 2, static_cast<uint32_t>(value));
        StoreWord(bytes.data(), 9 + index * 2, static_cast<uint32_t>(value >> 32U));
    }
    return bytes;
}

data::PmuSlots PmuEvents(std::initializer_list<uint32_t> events)
{
    data::PmuSlots slots{};
    slots.fill(data::kInvalidPmuEvent);
    std::copy(events.begin(), events.end(), slots.begin());
    return slots;
}

class ResultSink {
public:
    aclptiResult Accept(std::shared_ptr<const aclptiProfilingDataResult> result)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back(std::move(result));
        ready_.notify_all();
        return ACLPTI_SUCCESS;
    }

    std::shared_ptr<const aclptiProfilingDataResult> Wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!ready_.wait_for(lock, std::chrono::seconds(3), [this] { return !results_.empty(); })) {
            return nullptr;
        }
        return results_.front();
    }

    std::size_t Count()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return results_.size();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<std::shared_ptr<const aclptiProfilingDataResult>> results_;
};

template <typename Function>
bool CaptureStderr(Function function, std::string* output)
{
    FILE* capture = std::tmpfile();
    if (capture == nullptr) {
        return false;
    }

    const int savedStderr = dup(STDERR_FILENO);
    if (savedStderr < 0) {
        std::fclose(capture);
        return false;
    }

    bool success = std::fflush(stderr) == 0 && dup2(fileno(capture), STDERR_FILENO) >= 0;
    if (success) {
        function();
        success = std::fflush(stderr) == 0;
    }
    success = dup2(savedStderr, STDERR_FILENO) >= 0 && success;
    close(savedStderr);

    if (success) {
        std::rewind(capture);
        char buffer[256];
        std::size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), capture)) != 0) {
            output->append(buffer, count);
        }
        success = std::ferror(capture) == 0;
    }

    success = std::fclose(capture) == 0 && success;
    return success;
}

struct GuardedRawData {
    void* mapping = MAP_FAILED;
    std::size_t mappingSize = 0;
    MsprofRawData* raw = nullptr;

    ~GuardedRawData()
    {
        if (mapping != MAP_FAILED) {
            munmap(mapping, mappingSize);
        }
    }
};

static_assert(sizeof(MsprofRawData) == offsetof(MsprofRawData, chunk) + RAW_DATA_MAXSIZE);

bool MakeGuardedRawData(std::size_t chunkSize, RawDataType type, GuardedRawData* guarded)
{
    if (guarded == nullptr) {
        return false;
    }

    const long pageSizeValue = sysconf(_SC_PAGESIZE);
    if (pageSizeValue <= 0) {
        return false;
    }

    const std::size_t pageSize = static_cast<std::size_t>(pageSizeValue);
    const std::size_t mappingSize = pageSize * 2;
    void* mapping = mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return false;
    }

    if (mprotect(static_cast<char*>(mapping) + pageSize, pageSize, PROT_NONE) != 0) {
        munmap(mapping, mappingSize);
        return false;
    }

    guarded->mapping = mapping;
    guarded->mappingSize = mappingSize;
    guarded->raw = reinterpret_cast<MsprofRawData*>(
        static_cast<char*>(mapping) + pageSize - (offsetof(MsprofRawData, chunk) + RAW_DATA_MAXSIZE));
    guarded->raw->isLastChunk = false;
    guarded->raw->offset = 0;
    guarded->raw->chunkModule = 7;
    guarded->raw->deviceId = 0;
    guarded->raw->type = type;
    guarded->raw->chunkSize = chunkSize;
    std::memset(guarded->raw->chunk, 0, RAW_DATA_MAXSIZE);
    return true;
}

TEST(NpuComputePtiDataModule, RawDataDecoder)
{
    std::array<std::byte, 32> taskLog{};
    StoreWord(taskLog.data(), 0, 0x6bd30000U);
    StoreWord(taskLog.data(), 1, 0x12340056U);
    StoreWord(taskLog.data(), 2, 0x89abcdefU);
    StoreWord(taskLog.data(), 3, 0x01234567U);

    const auto start = data::DecodeRawRecord(taskLog.data(), taskLog.size(), 3);
    ASSERT_TRUE(start.Ok());
    ASSERT_TRUE(start.Value().recordIndex == 3);
    ASSERT_TRUE(std::holds_alternative<data::TaskLog32>(start.Value().payload));
    const auto& startLog = std::get<data::TaskLog32>(start.Value().payload);
    ASSERT_TRUE(startLog.funcType == 0x00);
    ASSERT_TRUE(startLog.taskId == 0x1234);
    ASSERT_TRUE(startLog.rtStreamId == 0x0056);
    ASSERT_TRUE(startLog.systemCounter == 0x0123456789abcdefULL);

    StoreWord(taskLog.data(), 0, 0x6bd30001U);
    const auto end = data::DecodeRawRecord(taskLog.data(), taskLog.size(), 4);
    ASSERT_TRUE(end.Ok());
    ASSERT_TRUE(std::get<data::TaskLog32>(end.Value().payload).funcType == 0x01);

    StoreWord(taskLog.data(), 0, 0x6bd30024U);
    StoreWord(taskLog.data(), 5, (0x5aU << 1U) | 1U);
    StoreWord(taskLog.data(), 6, 0x12345678U);
    const auto block = data::DecodeRawRecord(taskLog.data(), taskLog.size(), 6);
    ASSERT_TRUE(block.Ok());
    const auto& blockLog = std::get<data::TaskLog32>(block.Value().payload);
    ASSERT_TRUE(blockLog.funcType == 0x24);
    ASSERT_TRUE(blockLog.taskId == 0x1234);
    ASSERT_TRUE(blockLog.rtStreamId == 0x0056);
    ASSERT_TRUE(blockLog.systemCounter == 0x0123456789abcdefULL);
    ASSERT_TRUE(blockLog.blockId == 0x1234);
    ASSERT_TRUE(blockLog.subBlockId == 0x5678);
    ASSERT_TRUE(blockLog.coreType == ACLPTI_CORE_TYPE_AIV);
    ASSERT_TRUE(blockLog.coreTypeId == 0x5a);

    std::array<std::byte, 128> pmu{};
    StoreWord(pmu.data(), 0, 0x6bd3002aU);
    StoreWord(pmu.data(), 1, 0x43210078U);
    StoreWord(pmu.data(), 2, 0x76543210U);
    StoreWord(pmu.data(), 3, 0xfedcba98U);
    StoreWord(pmu.data(), 4, 1U << 10U);
    StoreWord(pmu.data(), 5, (0x5aU << 8U) | 1U);
    StoreWord(pmu.data(), 6, 0x12345678U);
    StoreWord(pmu.data(), 28, 0x10203040U);
    StoreWord(pmu.data(), 29, 0x50607080U);
    StoreWord(pmu.data(), 30, 0x90a0b0c0U);
    StoreWord(pmu.data(), 31, 0xd0e0f000U);
    for (std::size_t index = 0; index < data::kMaxPmuSlots; ++index) {
        const uint64_t value = 0x100000000ULL + index;
        StoreWord(pmu.data(), 8 + index * 2, static_cast<uint32_t>(value));
        StoreWord(pmu.data(), 9 + index * 2, static_cast<uint32_t>(value >> 32U));
    }

    const auto decoded = data::DecodeRawRecord(
        pmu.data(), pmu.size(), 5, PmuEvents({0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0}));
    ASSERT_TRUE(decoded.Ok());
    ASSERT_TRUE(std::holds_alternative<data::PmuRecord128>(decoded.Value().payload));
    const auto& record = std::get<data::PmuRecord128>(decoded.Value().payload);
    ASSERT_TRUE(record.funcType == 0x2aU);
    ASSERT_TRUE(record.taskId == 0x4321);
    ASSERT_TRUE(record.rtStreamId == 0x0078);
    ASSERT_TRUE(record.totalCycles == 0xfedcba9876543210ULL);
    ASSERT_TRUE(record.taskStartSystemCounter == 0x5060708010203040ULL);
    ASSERT_TRUE(record.taskEndSystemCounter == 0xd0e0f00090a0b0c0ULL);
    ASSERT_TRUE(record.overflow);
    ASSERT_TRUE(record.coreType == ACLPTI_CORE_TYPE_AIV);
    ASSERT_TRUE(record.coreId == 0x5a);
    ASSERT_TRUE(record.blockId == 0x1234);
    ASSERT_TRUE(record.subBlockId == 0x5678);
    ASSERT_TRUE(record.pmuValues.at(0xa0) == 0x100000009ULL);

    StoreWord(pmu.data(), 0, 0x6bd30029U);
    const auto blockPmu = data::DecodeRawRecord(pmu.data(), pmu.size(), 6, PmuEvents({0x10}));
    ASSERT_TRUE(blockPmu.Ok());
    const auto& blockPmuRecord = std::get<data::PmuRecord128>(blockPmu.Value().payload);
    ASSERT_TRUE(blockPmuRecord.funcType == 0x29U);
    ASSERT_TRUE(blockPmuRecord.blockId == 0x1234);
    ASSERT_TRUE(blockPmuRecord.subBlockId == 0x5678);
    ASSERT_TRUE(blockPmuRecord.pmuValues.at(0x10) == 0x100000000ULL);

    const auto twoSlotRecord = data::DecodeRawRecord(pmu.data(), pmu.size(), 6, PmuEvents({0x11, 0x10}));
    ASSERT_TRUE(twoSlotRecord.Ok());
    const auto& twoSlotPmu = std::get<data::PmuRecord128>(twoSlotRecord.Value().payload);
    ASSERT_TRUE(twoSlotPmu.pmuValues.at(0x11) == 0x100000000ULL);
    ASSERT_TRUE(twoSlotPmu.pmuValues.at(0x10) == 0x100000001ULL);

    StoreWord(pmu.data(), 0, 0x6bd3002aU);

    StoreWord(pmu.data(), 8, 100U);
    StoreWord(pmu.data(), 9, 0U);
    StoreWord(pmu.data(), 10, 300U);
    StoreWord(pmu.data(), 11, 0U);
    const auto duplicateEvent = data::DecodeRawRecord(pmu.data(), pmu.size(), 7, PmuEvents({0x10, 0x10}));
    ASSERT_TRUE(duplicateEvent.Ok());
    const auto& duplicateRecord = std::get<data::PmuRecord128>(duplicateEvent.Value().payload);
    ASSERT_TRUE(duplicateRecord.pmuValues.at(0x10) == 200.0);

    ASSERT_TRUE(data::DecodeRawRecord(nullptr, 32, 0).Status() == ACLPTI_ERROR_INVALID_RAW_DATA);
    ASSERT_TRUE(data::DecodeRawRecord(pmu.data(), 64, 0).Status() == ACLPTI_ERROR_INVALID_RAW_DATA);
    StoreWord(pmu.data(), 0, 0x0000002aU);
    ASSERT_TRUE(data::DecodeRawRecord(pmu.data(), pmu.size(), 0).Status() == ACLPTI_ERROR_DECODE);
    StoreWord(taskLog.data(), 0, 0x6bd30002U);
    ASSERT_TRUE(data::DecodeRawRecord(taskLog.data(), taskLog.size(), 0).Status() == ACLPTI_ERROR_DECODE);
}

TEST(NpuComputePtiDataModule, BlockAndTaskPmuAreAggregatedSeparately)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({41, PmuEvents({0x701})}) == ACLPTI_SUCCESS);

    const MsprofRawDataCallback callback = module.GetRawDataCallback();
    ASSERT_TRUE(callback != nullptr);
    auto blockPayload = PmuRecord(1, 2, 0, 100, 0x29U);
    auto taskPayload = PmuRecord(1, 2, 0, 1000, 0x2aU);
    StoreWord(taskPayload.data(), 2, 1000U);
    auto block = RawData(blockPayload, PMU_DATA_TYPE);
    auto task = RawData(taskPayload, PMU_DATA_TYPE);
    ASSERT_TRUE(callback(&block) == 0);
    ASSERT_TRUE(callback(&task) == 0);
    ASSERT_TRUE(module.RecordReplayStatus({41, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(41) == ACLPTI_SUCCESS);

    ASSERT_TRUE(module.PrepareReplay({42, PmuEvents({0x702})}) == ACLPTI_SUCCESS);
    blockPayload = PmuRecord(1, 2, 0, 200, 0x29U);
    taskPayload = PmuRecord(1, 2, 0, 2000, 0x2aU);
    StoreWord(blockPayload.data(), 2, 200U);
    StoreWord(taskPayload.data(), 2, 2000U);
    block = RawData(blockPayload, PMU_DATA_TYPE);
    task = RawData(taskPayload, PMU_DATA_TYPE);
    ASSERT_TRUE(callback(&block) == 0);
    ASSERT_TRUE(callback(&task) == 0);
    ASSERT_TRUE(module.RecordReplayStatus({42, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(42) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->pmuLogs.size() == 1);
    ASSERT_TRUE(result->taskPmuLogs.size() == 1);
    const aclptiBlockKey key{0, 0, ACLPTI_CORE_TYPE_AIC, 3};
    ASSERT_TRUE(result->pmuLogs.at(key).totalCycles == 200.0);
    ASSERT_TRUE(result->pmuLogs.at(key).values.at(0x701) == 100.0);
    ASSERT_TRUE(result->pmuLogs.at(key).values.at(0x702) == 200.0);
    ASSERT_TRUE(result->taskPmuLogs.at(key).totalCycles == 2000.0);
    ASSERT_TRUE(result->taskPmuLogs.at(key).values.at(0x701) == 1000.0);
    ASSERT_TRUE(result->taskPmuLogs.at(key).values.at(0x702) == 2000.0);
}

TEST(NpuComputePtiDataModule, ReplayLifecycle)
{
    ResultSink sink;
    std::atomic<bool> callbackCompleted{false};
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink, &callbackCompleted](const auto& result) {
        const auto status = sink.Accept(result);
        callbackCompleted.store(true, std::memory_order_release);
        return status;
    };
    ASSERT_TRUE(module.GetRawDataCallback() == nullptr);
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);

    ASSERT_TRUE(&module == &data::ProfilingDataManager::Instance());
    ASSERT_TRUE(module.Initialize() == ACLPTI_SUCCESS);

    data::PmuSlots noEvents{};
    noEvents.fill(data::kInvalidPmuEvent);
    ASSERT_TRUE(
        module.PrepareReplay({40, PmuEvents({0x701}), static_cast<data::ReplayKind>(99)}) ==
        ACLPTI_ERROR_INVALID_PARAMETER);
    ASSERT_TRUE(module.PrepareReplay({40, noEvents}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.RecordReplayStatus({40, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(40) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({40, noEvents}) == ACLPTI_ERROR_INVALID_PARAMETER);
    auto eventHole = PmuEvents({0x701});
    eventHole[2] = 0x22;
    ASSERT_TRUE(module.PrepareReplay({40, eventHole}) == ACLPTI_ERROR_INVALID_PARAMETER);

    const auto eventIds = PmuEvents({0x701, 0x22});
    ASSERT_TRUE(module.PrepareReplay({42, eventIds, data::ReplayKind::Pmu}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({43, PmuEvents({0x1})}) == ACLPTI_ERROR_REPLAY_ACTIVE);

    const MsprofRawDataCallback callback = module.GetRawDataCallback();
    ASSERT_TRUE(callback != nullptr);
    auto firstPayload = PmuRecord(8, 9, 2, 1000);
    StoreWord(firstPayload.data(), 0, 0x6bd30029U);
    auto first = RawData(firstPayload, PMU_DATA_TYPE, 17);
    auto secondRow = RawData(PmuRecord(8, 9, 3, 2000), PMU_DATA_TYPE, 17);
    std::array<std::byte, 256> pmuChunk{};
    const auto thirdRow = PmuRecord(12, 13, 4, 3000);
    std::memcpy(pmuChunk.data(), secondRow.chunk, 128);
    std::memcpy(pmuChunk.data() + 128, thirdRow.data(), 128);
    auto multiRow = RawData(pmuChunk, PMU_DATA_TYPE, 17);
    std::array<std::byte, 64> logChunk{};
    const auto firstLog =
        TaskLog(0x24, 0x1234, 0x0056, 0x0123456789abcdefULL, 0x1111, 0x2222, ACLPTI_CORE_TYPE_AIV, 0x5a);
    const auto secondLog =
        TaskLog(0x24, 0x1234, 0x0056, 0xfedcba9876543210ULL, 0x3333, 0x4444, ACLPTI_CORE_TYPE_AIC, 0x2b);
    std::memcpy(logChunk.data(), firstLog.data(), 32);
    std::memcpy(logChunk.data() + 32, secondLog.data(), 32);
    auto multiLog = RawData(logChunk, LOG_DATA_TYPE);
    multiLog.deviceId = 17;
    multiLog.chunkModule = 1;
    multiLog.offset = 900;
    multiLog.isLastChunk = true;
    multiRow.chunkModule = 99;
    multiRow.offset = 17;
    ASSERT_TRUE(callback(&multiLog) == 0);
    ASSERT_TRUE(callback(&first) == 0);
    ASSERT_TRUE(callback(&multiRow) == 0);

    ASSERT_TRUE(module.ReleaseReplay(42) == ACLPTI_ERROR_INVALID_STATE);
    ASSERT_TRUE(module.RecordReplayStatus({41, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_REPLAY_NOT_FOUND);
    const auto stopped = module.RecordReplayStatus({42, ACLPTI_SUCCESS});
    ASSERT_TRUE(stopped.status == ACLPTI_SUCCESS);
    ASSERT_TRUE(stopped.callbackStats.copiedRecordCount == 5);
    ASSERT_TRUE(stopped.callbackStats.copiedBytes == 448);
    ASSERT_TRUE(stopped.callbackStats.receivedBytes == 448);
    ASSERT_TRUE(module.ReleaseReplay(42) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
    ASSERT_TRUE(callbackCompleted.load(std::memory_order_acquire));

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_SUCCESS);
    ASSERT_TRUE(result->taskLogs.empty());
    ASSERT_TRUE(result->blockLogs.at(aclptiBlockKey{0x1111, 0x2222}).size() == 1);
    ASSERT_TRUE(result->blockLogs.at(aclptiBlockKey{0x3333, 0x4444}).size() == 1);
    ASSERT_TRUE(result->pmuLogs.size() == 3);
    ASSERT_TRUE(result->pmuLogs.at(aclptiBlockKey{2, 0, ACLPTI_CORE_TYPE_AIC, 3}).values.at(0x701) == 1000.0);
    ASSERT_TRUE(result->pmuLogs.at(aclptiBlockKey{2, 0, ACLPTI_CORE_TYPE_AIC, 3}).values.at(0x22) == 1001.0);
    ASSERT_TRUE(result->pmuLogs.at(aclptiBlockKey{3, 0, ACLPTI_CORE_TYPE_AIC, 3}).values.at(0x701) == 2000.0);
    ASSERT_TRUE(result->pmuLogs.at(aclptiBlockKey{3, 0, ACLPTI_CORE_TYPE_AIC, 3}).values.at(0x22) == 2001.0);
    ASSERT_TRUE(result->pmuLogs.at(aclptiBlockKey{4, 0, ACLPTI_CORE_TYPE_AIC, 3}).values.at(0x701) == 3000.0);
    ASSERT_TRUE(sink.Count() == 1);
}

TEST(NpuComputePtiDataModule, FailedReplay)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({7, PmuEvents({0x500})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_ERROR_REPLAY_ACTIVE);

    auto invalidType = RawData(PmuRecord(1, 2, 3, 4), PMU_DATA_TYPE);
    invalidType.type = static_cast<RawDataType>(0);
    ASSERT_TRUE(module.GetRawDataCallback()(&invalidType) == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));
    const auto stopped = module.RecordReplayStatus({7, ACLPTI_SUCCESS});
    ASSERT_TRUE(stopped.status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(7) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(result->stats.failedRecordCount == 1);
    ASSERT_TRUE(result->stats.failedRecordCount == 1);
    ASSERT_TRUE(result->pmuLogs.empty());
    ASSERT_TRUE(sink.Count() == 1);

    ResultSink decodeSink;
    auto& decodeModule = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback decodeModuleCallback = [&decodeSink](const auto& decodedResult) {
        return decodeSink.Accept(decodedResult);
    };
    ASSERT_TRUE(decodeModule.Initialize(std::move(decodeModuleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(decodeModule.PrepareReplay({8, PmuEvents({0x8, 0xa})}) == ACLPTI_SUCCESS);
    auto malformed = PmuRecord(1, 2, 3, 4);
    StoreWord(malformed.data(), 0, 0x0000002aU);
    auto malformedRaw = RawData(malformed, PMU_DATA_TYPE);
    ASSERT_TRUE(decodeModule.GetRawDataCallback()(&malformedRaw) == 0);
    ASSERT_TRUE(decodeModule.RecordReplayStatus({8, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(decodeModule.ReleaseReplay(8) == ACLPTI_SUCCESS);
    ASSERT_TRUE(decodeModule.Shutdown() == ACLPTI_SUCCESS);
    const auto decodeResult = decodeSink.Wait();
    ASSERT_TRUE(decodeResult != nullptr);
    ASSERT_TRUE(decodeResult->status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(decodeResult->stats.failedRecordCount == 1);
    ASSERT_TRUE(decodeResult->stats.failedRecordCount == 1);
    ASSERT_TRUE(decodeResult->pmuLogs.empty());

    setenv("NPU_COMPUTE_SKIP_DATA_PARSE", "1", 1);
    ResultSink skippedSink;
    auto& skippedModule = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback skippedModuleCallback = [&skippedSink](const auto& skippedResult) {
        return skippedSink.Accept(skippedResult);
    };
    ASSERT_TRUE(skippedModule.Initialize(std::move(skippedModuleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(skippedModule.PrepareReplay({9, PmuEvents({0x8, 0xa})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(skippedModule.GetRawDataCallback()(&malformedRaw) == 0);
    ASSERT_TRUE(skippedModule.RecordReplayStatus({9, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(skippedModule.ReleaseReplay(9) == ACLPTI_SUCCESS);
    ASSERT_TRUE(skippedModule.Shutdown() == ACLPTI_SUCCESS);
    unsetenv("NPU_COMPUTE_SKIP_DATA_PARSE");
    const auto skippedResult = skippedSink.Wait();
    ASSERT_TRUE(skippedResult != nullptr);
    ASSERT_TRUE(skippedResult->status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(skippedResult->pmuLogs.empty());
}

TEST(NpuComputePtiDataModule, ForceShutdownActiveReplay)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({9, PmuEvents({0x8})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_ERROR_REPLAY_ACTIVE);
    ASSERT_TRUE(module.ForceShutdown() == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_SUCCESS);
    ASSERT_TRUE(result->pmuLogs.empty());
}

TEST(NpuComputePtiDataModule, ReplayFailureIsPublished)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({12, PmuEvents({0x8})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.RecordReplayStatus({12, ACLPTI_ERROR_PROFILING_FAILED}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(12) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(result->stats.failedRecordCount == 0);
}

TEST(NpuComputePtiDataModule, OpaqueInstructionChunks)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({10, PmuEvents({}), data::ReplayKind::Pipeline}) == ACLPTI_SUCCESS);

    const MsprofRawDataCallback callback = module.GetRawDataCallback();
    ASSERT_TRUE(callback != nullptr);
    const std::array<std::byte, 8> firstBytes{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
        std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
    };
    auto first = RawData(firstBytes, BIU_PERF_DATA_TYPE, 2);
    first.chunkModule = 4;
    first.offset = 0;

    MsprofRawData empty{};
    empty.type = BIU_PERF_DATA_TYPE;
    ASSERT_TRUE(callback(&empty) == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));

    MsprofRawData oversized{};
    oversized.type = BIU_PERF_DATA_TYPE;
    oversized.chunkSize = RAW_DATA_MAXSIZE + 1;
    ASSERT_TRUE(callback(&oversized) == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));

    auto gap = first;
    gap.offset = 4;
    ASSERT_TRUE(callback(&gap) == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));
    ASSERT_TRUE(callback(&first) == static_cast<std::int32_t>(ACLPTI_SUCCESS));

    const std::array<std::byte, 4> secondBytes{std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}};
    auto wrongType = RawData(secondBytes, PC_SAMPLING_DATA_TYPE, 2);
    wrongType.offset = 8;
    ASSERT_TRUE(callback(&wrongType) == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));

    auto second = RawData(secondBytes, BIU_PERF_DATA_TYPE, 2);
    second.chunkModule = 4;
    second.offset = 8;
    second.isLastChunk = true;
    ASSERT_TRUE(callback(&second) == static_cast<std::int32_t>(ACLPTI_SUCCESS));

    const std::array<std::byte, 4> aivBytes{std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01}};
    auto aiv = RawData(aivBytes, BIU_PERF_AIV0_DATA_TYPE, 2);
    aiv.chunkModule = 5;
    aiv.isLastChunk = true;
    ASSERT_TRUE(callback(&aiv) == static_cast<std::int32_t>(ACLPTI_SUCCESS));

    auto afterLast = RawData(secondBytes, BIU_PERF_DATA_TYPE, 2);
    afterLast.chunkModule = 4;
    afterLast.offset = 12;
    ASSERT_TRUE(callback(&afterLast) == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));
    ASSERT_TRUE(module.RecordReplayStatus({10, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(10) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_ERROR_PROFILING_FAILED);
    const auto replay = result->pipelineData.find(10);
    ASSERT_TRUE(replay != result->pipelineData.end());
    ASSERT_TRUE(replay->second.deviceId == 2);
    ASSERT_TRUE(replay->second.format == aclptiBiuFormat::Chip6);
    ASSERT_TRUE(replay->second.channels.size() == 2);
    ASSERT_TRUE(result->pcSamplingData.empty());
    ASSERT_TRUE(
        (replay->second.channels.at({4, aclptiBiuCoreKind::Aic}) ==
         std::vector<uint32_t>{0x44332211U, 0x88776655U, 0xccbbaa99U}));
    ASSERT_TRUE((replay->second.channels.at({5, aclptiBiuCoreKind::Aiv0}) == std::vector<uint32_t>{0x01020304U}));
    ASSERT_TRUE(replay->second.stats.receivedBytes == 28);
    ASSERT_TRUE(replay->second.stats.acceptedBytes == 16);
    ASSERT_TRUE(replay->second.stats.rejectedChunkCount == 5);
    ASSERT_TRUE(result->stats.failedRecordCount == 5);
}

TEST(NpuComputePtiDataModule, OpaquePcSamplingChunk)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({11, PmuEvents({}), data::ReplayKind::PcSampling}) == ACLPTI_SUCCESS);

    const MsprofRawDataCallback callback = module.GetRawDataCallback();
    ASSERT_TRUE(callback != nullptr);
    const std::array<std::byte, 4> bytes{std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}};
    auto raw = RawData(bytes, PC_SAMPLING_DATA_TYPE, 3);
    raw.chunkModule = 6;
    raw.isLastChunk = true;
    ASSERT_TRUE(callback(&raw) == static_cast<std::int32_t>(ACLPTI_SUCCESS));
    ASSERT_TRUE(module.RecordReplayStatus({11, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(11) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_SUCCESS);
    ASSERT_TRUE(result->pcSamplingData.at(0).deviceId == 3);
    ASSERT_TRUE(result->pipelineData.empty());
    ASSERT_TRUE(result->pcSamplingData.size() == 1);
    ASSERT_TRUE(result->pcSamplingData[0].replayId == 11);
    ASSERT_TRUE(result->pcSamplingData[0].deviceId == 3);
    ASSERT_TRUE(result->pcSamplingData[0].chunkModule == 6);
    ASSERT_TRUE(result->pcSamplingData[0].offset == 0);
    ASSERT_TRUE(result->pcSamplingData[0].isLastChunk);
    ASSERT_TRUE((result->pcSamplingData[0].bytes == std::vector<uint8_t>{0xaa, 0xbb, 0xcc, 0xdd}));
}

TEST(NpuComputePtiDataModule, OpaqueAggregationAllocationFailure)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({13, PmuEvents({}), data::ReplayKind::Pipeline}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(setenv("NPU_COMPUTE_TEST_OPAQUE_AGGREGATE_OOM", "1", 1) == 0);

    const std::array<std::byte, 4> bytes{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    auto raw = RawData(bytes, BIU_PERF_DATA_TYPE);
    raw.chunkModule = 0;
    raw.isLastChunk = true;
    ASSERT_TRUE(module.GetRawDataCallback()(&raw) == static_cast<std::int32_t>(ACLPTI_SUCCESS));
    ASSERT_TRUE(module.RecordReplayStatus({13, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(unsetenv("NPU_COMPUTE_TEST_OPAQUE_AGGREGATE_OOM") == 0);
    ASSERT_TRUE(module.ReleaseReplay(13) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(result->pipelineData.at(13).channels.empty());
    ASSERT_TRUE(result->stats.failedRecordCount == 1);
}

TEST(NpuComputePtiDataModule, ReplayDeviceIsInferredFromRawData)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({15, PmuEvents({}), data::ReplayKind::Pipeline}) == ACLPTI_SUCCESS);

    const std::array<std::byte, 4> bytes{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    auto first = RawData(bytes, BIU_PERF_DATA_TYPE, 2);
    first.chunkModule = 0;
    ASSERT_TRUE(module.GetRawDataCallback()(&first) == static_cast<std::int32_t>(ACLPTI_SUCCESS));

    auto wrongDevice = first;
    wrongDevice.deviceId = 3;
    wrongDevice.offset = 4;
    wrongDevice.isLastChunk = true;
    ASSERT_TRUE(module.GetRawDataCallback()(&wrongDevice) == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));

    auto last = first;
    last.offset = 4;
    last.isLastChunk = true;
    ASSERT_TRUE(module.GetRawDataCallback()(&last) == static_cast<std::int32_t>(ACLPTI_SUCCESS));
    ASSERT_TRUE(module.RecordReplayStatus({15, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(15) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    const auto& replay = result->pipelineData.at(15);
    ASSERT_TRUE(replay.deviceId == 2);
    ASSERT_TRUE(replay.stats.receivedBytes == 12);
    ASSERT_TRUE(replay.stats.acceptedBytes == 8);
    ASSERT_TRUE(replay.stats.rejectedChunkCount == 1);
    ASSERT_TRUE(replay.stats.failedRecordCount == 1);
}

TEST(NpuComputePtiDataModule, PipelineReplayWithoutRawDataKeepsUnknownDevice)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({16, PmuEvents({}), data::ReplayKind::Pipeline}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(
        module.RecordReplayStatus({16, ACLPTI_ERROR_RESULT_UNRELIABLE}).status == ACLPTI_ERROR_RESULT_UNRELIABLE);
    ASSERT_TRUE(module.ReleaseReplay(16) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->pipelineData.size() == 1);
    const auto& replay = result->pipelineData.at(16);
    ASSERT_TRUE(replay.deviceId == -1);
    ASSERT_TRUE(replay.status == ACLPTI_ERROR_RESULT_UNRELIABLE);
    ASSERT_TRUE(replay.stats.receivedBytes == 0);
    ASSERT_TRUE(replay.stats.acceptedBytes == 0);
    ASSERT_TRUE(replay.channels.empty());
}

TEST(NpuComputePtiDataModule, DecoderAllocationFailure)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({14, PmuEvents({0x55})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(setenv("NPU_COMPUTE_TEST_DECODE_OOM", "1", 1) == 0);

    auto payload = PmuRecord(1, 2, 8, 77);
    auto raw = RawData(payload, PMU_DATA_TYPE);
    ASSERT_TRUE(module.GetRawDataCallback()(&raw) == static_cast<std::int32_t>(ACLPTI_SUCCESS));
    ASSERT_TRUE(module.RecordReplayStatus({14, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(unsetenv("NPU_COMPUTE_TEST_DECODE_OOM") == 0);
    ASSERT_TRUE(module.ReleaseReplay(14) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(result->pmuLogs.empty());
    ASSERT_TRUE(result->stats.failedRecordCount == 1);
}

TEST(NpuComputePtiDataModule, InvalidChunkDiagnostics)
{
    const char* previousDebug = std::getenv("NPU_COMPUTE_DEBUG");
    const std::string previousDebugValue = previousDebug == nullptr ? std::string() : std::string(previousDebug);

    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({9, PmuEvents({0x55})}) == ACLPTI_SUCCESS);
    const MsprofRawDataCallback callback = module.GetRawDataCallback();
    ASSERT_TRUE(callback != nullptr);

    ASSERT_TRUE(setenv("NPU_COMPUTE_DEBUG", "1", 1) == 0);

    GuardedRawData guarded;
    ASSERT_TRUE(MakeGuardedRawData(RAW_DATA_MAXSIZE + 1, PMU_DATA_TYPE, &guarded));

    std::string output;
    std::int32_t status = 0;
    ASSERT_TRUE(CaptureStderr([&] { status = callback(guarded.raw); }, &output));
    ASSERT_TRUE(status == static_cast<std::int32_t>(ACLPTI_ERROR_INVALID_RAW_DATA));
    ASSERT_TRUE(output.find("[DEBUG-rawdata]") == std::string::npos);
    ASSERT_TRUE(output.find("raw callback invalid chunk") != std::string::npos);

    if (previousDebug == nullptr) {
        ASSERT_TRUE(unsetenv("NPU_COMPUTE_DEBUG") == 0);
    } else {
        ASSERT_TRUE(setenv("NPU_COMPUTE_DEBUG", previousDebugValue.c_str(), 1) == 0);
    }

    ASSERT_TRUE(module.RecordReplayStatus({9, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(9) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
}

TEST(NpuComputePtiDataModule, ProcessCallbackRegistration)
{
    auto& standalone = data::ProfilingDataManager::Instance();
    ASSERT_TRUE(standalone.Initialize() == ACLPTI_SUCCESS);
    ASSERT_TRUE(standalone.Shutdown() == ACLPTI_SUCCESS);

    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    ASSERT_TRUE(aclptiRegisterProfilingDataCallback({}) == ACLPTI_ERROR_INVALID_PARAMETER);
    ASSERT_TRUE(aclptiRegisterProfilingDataCallback([&sink](const auto& result) {
                    return sink.Accept(result);
                }) == ACLPTI_SUCCESS);
    ASSERT_TRUE(
        aclptiRegisterProfilingDataCallback([](const auto&) { return ACLPTI_SUCCESS; }) == ACLPTI_ERROR_INVALID_STATE);

    ASSERT_TRUE(module.Initialize() == ACLPTI_SUCCESS);
    const data::ReplayPrepareInfo replayInfo{99, PmuEvents({0x8})};
    ASSERT_TRUE(module.PrepareReplay(replayInfo) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.RecordReplayStatus({99, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(99) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_SUCCESS);
    ASSERT_TRUE(result->pmuLogs.empty());
}

TEST(NpuComputePtiDataModule, ShutdownCallback)
{
    static std::atomic<int> shutdownCalls{0};
    shutdownCalls.store(0);
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [](const auto&) { return ACLPTI_SUCCESS; };
    ASSERT_TRUE(
        aclptiRegisterDataModuleShutdownCallback(
            [](void* userData) {
                auto* calls = static_cast<std::atomic<int>*>(userData);
                ++(*calls);
                return ACLPTI_SUCCESS;
            },
            &shutdownCalls) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({100, PmuEvents({0x8})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.RecordReplayStatus({100, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(100) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
    ASSERT_TRUE(shutdownCalls.load() == 1);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
    ASSERT_TRUE(shutdownCalls.load() == 1);

    ResultSink sink;
    auto& emptyModule = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback emptyModuleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(emptyModule.Initialize(std::move(emptyModuleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(emptyModule.Shutdown() == ACLPTI_SUCCESS);
    ASSERT_TRUE(shutdownCalls.load() == 2);
    ASSERT_TRUE(sink.Count() == 0);
}

TEST(NpuComputePtiDataModule, CrossReplayAggregate)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);

    const auto firstEvents = PmuEvents({0x10, 0x20});
    ASSERT_TRUE(module.PrepareReplay({201, firstEvents}) == ACLPTI_SUCCESS);
    auto firstPmu = PmuRecord(7, 8, 2, 10);
    StoreWord(firstPmu.data(), 2, 100);
    StoreWord(firstPmu.data(), 5, 1U << 8U);
    StoreWord(firstPmu.data(), 6, (2U << 16U) | 3U);
    StoreWord(firstPmu.data(), 28, 0x1111);
    StoreWord(firstPmu.data(), 30, 0x2222);
    auto firstRaw = RawData(firstPmu, PMU_DATA_TYPE);
    auto taskStart = RawData(TaskLog(0x00, 7, 8, 1, 0, 0, ACLPTI_CORE_TYPE_AIC, 0), LOG_DATA_TYPE);
    auto blockStart = RawData(TaskLog(0x24, 7, 8, 2, 2, 3, ACLPTI_CORE_TYPE_AIC, 1), LOG_DATA_TYPE);
    ASSERT_TRUE(module.GetRawDataCallback()(&firstRaw) == 0);
    ASSERT_TRUE(module.GetRawDataCallback()(&taskStart) == 0);
    ASSERT_TRUE(module.GetRawDataCallback()(&blockStart) == 0);
    ASSERT_TRUE(module.RecordReplayStatus({201, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(201) == ACLPTI_SUCCESS);

    const auto secondEvents = PmuEvents({0x20, 0x30});
    ASSERT_TRUE(module.PrepareReplay({202, secondEvents}) == ACLPTI_SUCCESS);
    auto secondPmu = PmuRecord(9, 10, 2, 30);
    StoreWord(secondPmu.data(), 2, 300);
    StoreWord(secondPmu.data(), 5, (2U << 8U) | 1U);
    StoreWord(secondPmu.data(), 6, (2U << 16U) | 3U);
    StoreWord(secondPmu.data(), 28, 0x3333);
    StoreWord(secondPmu.data(), 30, 0x4444);
    auto secondRaw = RawData(secondPmu, PMU_DATA_TYPE);
    auto secondBlockStart = RawData(TaskLog(0x24, 9, 10, 3, 2, 3, ACLPTI_CORE_TYPE_AIV, 2), LOG_DATA_TYPE);
    ASSERT_TRUE(module.GetRawDataCallback()(&secondRaw) == 0);
    ASSERT_TRUE(module.GetRawDataCallback()(&secondBlockStart) == 0);
    ASSERT_TRUE(module.RecordReplayStatus({202, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(202) == ACLPTI_SUCCESS);

    ASSERT_TRUE(sink.Count() == 0); // No per-replay delivery before the original launch completes.
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
    ASSERT_TRUE(sink.Count() == 1);
    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_SUCCESS);
    ASSERT_TRUE(result->taskLogs.at(7).size() == 1);
    ASSERT_TRUE(result->taskLogs.at(7).at(0).replayId == 201);
    ASSERT_TRUE(result->blockLogs.at(aclptiBlockKey{2, 3}).size() == 2);
    ASSERT_TRUE(result->pmuLogs.size() == 2);
    const auto& aicRow = result->pmuLogs.at(aclptiBlockKey{2, 3, ACLPTI_CORE_TYPE_AIC, 1});
    ASSERT_TRUE(aicRow.totalCycles == 100.0);
    ASSERT_TRUE(aicRow.values.at(0x10) == 10.0);
    ASSERT_TRUE(aicRow.values.at(0x20) == 11.0);
    ASSERT_TRUE(aicRow.systemCounters.size() == 1);
    ASSERT_TRUE(aicRow.systemCounters[0].taskStartSystemCounter == 0x1111);
    ASSERT_TRUE(aicRow.systemCounters[0].taskEndSystemCounter == 0x2222);
    ASSERT_TRUE(aicRow.coreInfos.size() == 1);
    ASSERT_TRUE(aicRow.coreInfos[0].coreType == ACLPTI_CORE_TYPE_AIC);
    ASSERT_TRUE(aicRow.coreInfos[0].coreId == 1);
    ASSERT_TRUE(aicRow.coreInfos[0].count == 1);

    const auto& aivRow = result->pmuLogs.at(aclptiBlockKey{2, 3, ACLPTI_CORE_TYPE_AIV, 2});
    ASSERT_TRUE(aivRow.totalCycles == 300.0);
    ASSERT_TRUE(aivRow.values.at(0x20) == 30.0);
    ASSERT_TRUE(aivRow.values.at(0x30) == 31.0);
    ASSERT_TRUE(aivRow.systemCounters.size() == 1);
    ASSERT_TRUE(aivRow.systemCounters[0].taskStartSystemCounter == 0x3333);
    ASSERT_TRUE(aivRow.systemCounters[0].taskEndSystemCounter == 0x4444);
    ASSERT_TRUE(aivRow.coreInfos.size() == 1);
    ASSERT_TRUE(aivRow.coreInfos[0].coreType == ACLPTI_CORE_TYPE_AIV);
    ASSERT_TRUE(aivRow.coreInfos[0].coreId == 2);
    ASSERT_TRUE(aivRow.coreInfos[0].count == 1);
}

TEST(NpuComputePtiDataModule, MsopprofA5ReplayMerge)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);

    ASSERT_TRUE(module.PrepareReplay({211, PmuEvents({10, 20})}) == ACLPTI_SUCCESS);
    auto first = PmuRecord(7, 8, 4, 10);
    StoreWord(first.data(), 2, 100);
    StoreWord(first.data(), 5, 1U << 8U);
    StoreWord(first.data(), 6, (4U << 16U) | 1U);
    auto firstRaw = RawData(first, PMU_DATA_TYPE);
    ASSERT_TRUE(module.GetRawDataCallback()(&firstRaw) == 0);

    auto duplicate = PmuRecord(7, 8, 4, 20);
    StoreWord(duplicate.data(), 2, 200);
    StoreWord(duplicate.data(), 5, 2U << 8U);
    StoreWord(duplicate.data(), 6, (4U << 16U) | 1U);
    auto duplicateRaw = RawData(duplicate, PMU_DATA_TYPE);
    ASSERT_TRUE(module.GetRawDataCallback()(&duplicateRaw) == 0);
    ASSERT_TRUE(module.RecordReplayStatus({211, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(211) == ACLPTI_SUCCESS);

    ASSERT_TRUE(module.PrepareReplay({212, PmuEvents({20, 30})}) == ACLPTI_SUCCESS);
    auto second = PmuRecord(9, 10, 4, 30);
    StoreWord(second.data(), 2, 300);
    StoreWord(second.data(), 5, 3U << 8U);
    StoreWord(second.data(), 6, (4U << 16U) | 1U);
    auto secondRaw = RawData(second, PMU_DATA_TYPE);
    ASSERT_TRUE(module.GetRawDataCallback()(&secondRaw) == 0);
    ASSERT_TRUE(module.RecordReplayStatus({212, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(212) == ACLPTI_SUCCESS);

    ASSERT_TRUE(module.PrepareReplay({213, PmuEvents({40})}) == ACLPTI_SUCCESS);
    auto failed = PmuRecord(11, 12, 4, 40);
    StoreWord(failed.data(), 2, 400);
    StoreWord(failed.data(), 5, 4U << 8U);
    StoreWord(failed.data(), 6, (4U << 16U) | 1U);
    auto failedRaw = RawData(failed, PMU_DATA_TYPE);
    ASSERT_TRUE(module.GetRawDataCallback()(&failedRaw) == 0);
    ASSERT_TRUE(
        module.RecordReplayStatus({213, ACLPTI_ERROR_RESULT_UNRELIABLE}).status == ACLPTI_ERROR_RESULT_UNRELIABLE);
    ASSERT_TRUE(module.ReleaseReplay(213) == ACLPTI_SUCCESS);

    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->pmuLogs.size() == 1);
    const auto& [key, row] = *result->pmuLogs.begin();
    ASSERT_TRUE(key.blockId == 4);
    ASSERT_TRUE(key.subBlockId == 1);
    ASSERT_TRUE(key.coreType == ACLPTI_CORE_TYPE_AIC);
    ASSERT_TRUE(key.coreId == 3);
    ASSERT_TRUE(row.totalCycles == 300.0);
    ASSERT_TRUE(row.values.size() == 3);
    ASSERT_TRUE(row.values.count(10) == 1);
    ASSERT_TRUE(row.values.count(20) == 1);
    ASSERT_TRUE(row.values.count(30) == 1);
    ASSERT_TRUE(row.values.at(10) == 20.0);
    ASSERT_TRUE(row.values.at(20) == 21.0);
    ASSERT_TRUE(row.values.at(30) == 31.0);
    ASSERT_TRUE(row.values.count(40) == 0);
}

TEST(NpuComputePtiDataModule, FailedPackageIsDropped)
{
    constexpr std::size_t kMalformedRecordCount = 5000;
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({303, PmuEvents({0x55})}) == ACLPTI_SUCCESS);

    auto valid = PmuRecord(1, 2, 8, 77);
    StoreWord(valid.data(), 6, (8U << 16U) | 9U);
    auto malformed = valid;
    StoreWord(malformed.data(), 0, 0x0000002aU);
    auto validRaw = RawData(valid, PMU_DATA_TYPE);
    auto malformedRaw = RawData(malformed, PMU_DATA_TYPE);
    const MsprofRawDataCallback callback = module.GetRawDataCallback();
    ASSERT_TRUE(callback != nullptr);
    ASSERT_TRUE(callback(&validRaw) == 0);
    for (std::size_t index = 0; index < kMalformedRecordCount; ++index) {
        malformedRaw.offset = static_cast<uint64_t>(index * malformedRaw.chunkSize);
        ASSERT_TRUE(callback(&malformedRaw) == 0);
    }
    ASSERT_TRUE(module.RecordReplayStatus({303, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(303) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    ASSERT_TRUE(sink.Count() == 1);
    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(result->stats.failedRecordCount == kMalformedRecordCount);
    ASSERT_TRUE(result->stats.failedRecordCount == kMalformedRecordCount);
    ASSERT_TRUE(result->pmuLogs.empty());
}

TEST(NpuComputePtiDataModule, RawQueueBackpressure)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&sink](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({404, PmuEvents({0x55})}) == ACLPTI_SUCCESS);

    const MsprofRawDataCallback callback = module.GetRawDataCallback();
    ASSERT_TRUE(callback != nullptr);

    auto payload = PmuRecord(1, 2, 8, 77);
    auto raw = RawData(payload, PMU_DATA_TYPE);
    constexpr size_t recordCount = 12000;
    for (std::size_t index = 0; index < recordCount; ++index) {
        raw.offset = static_cast<uint64_t>(index * raw.chunkSize);
        ASSERT_TRUE(callback(&raw) == 0);
    }

    ASSERT_TRUE(module.RecordReplayStatus({404, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(404) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);

    ASSERT_TRUE(sink.Count() == 1);
    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->status == ACLPTI_SUCCESS);
    ASSERT_TRUE(result->stats.failedRecordCount == 0);
    ASSERT_TRUE(result->pmuLogs.begin()->second.systemCounters.size() == recordCount);
    ASSERT_TRUE(result->stats.acceptedBytes == recordCount * 128);
    ASSERT_TRUE(result->pmuLogs.at(aclptiBlockKey{8, 0, ACLPTI_CORE_TYPE_AIC, 3}).values.at(0x55) == 77.0);
}

TEST(NpuComputePtiDataModule, QueueFailureAndForceShutdown)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({501, PmuEvents({0x55})}) == ACLPTI_SUCCESS);
    auto raw = RawData(PmuRecord(1, 2, 8, 77), PMU_DATA_TYPE);
    ASSERT_TRUE(setenv("NPU_COMPUTE_TEST_QUEUE_OOM", "1", 1) == 0);
    ASSERT_TRUE(module.GetRawDataCallback()(&raw) == ACLPTI_ERROR_OUT_OF_MEMORY);
    ASSERT_TRUE(unsetenv("NPU_COMPUTE_TEST_QUEUE_OOM") == 0);
    ASSERT_TRUE(module.RecordReplayStatus({501, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(501) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({502, PmuEvents({0x55})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.GetRawDataCallback()(&raw) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ForceShutdown() == ACLPTI_SUCCESS);
    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->stats.acceptedBytes == 128);
    ASSERT_TRUE(result->errorStats.failedRecordCountByReplay.at(501) == 1);
    ASSERT_TRUE(result->status == ACLPTI_ERROR_RESULT_UNRELIABLE);
    ASSERT_TRUE(result->pmuLogs.empty());
}

TEST(NpuComputePtiDataModule, RejectedChunkDoesNotBindDevice)
{
    ResultSink sink;
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [&](const auto& result) { return sink.Accept(result); };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({503, PmuEvents({}), data::ReplayKind::Pipeline}) == ACLPTI_SUCCESS);
    const std::array<std::byte, 4> bytes{};
    auto raw = RawData(bytes, BIU_PERF_DATA_TYPE, 3);
    raw.chunkModule = 6;
    raw.isLastChunk = true;
    ASSERT_TRUE(module.GetRawDataCallback()(&raw) == ACLPTI_ERROR_INVALID_RAW_DATA);
    raw.deviceId = 2;
    raw.chunkModule = 0;
    ASSERT_TRUE(module.GetRawDataCallback()(&raw) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.RecordReplayStatus({503, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_PROFILING_FAILED);
    ASSERT_TRUE(module.ReleaseReplay(503) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
    const auto result = sink.Wait();
    ASSERT_TRUE(result != nullptr);
    ASSERT_TRUE(result->pipelineData.at(503).deviceId == 2);
    ASSERT_TRUE(result->pipelineData.at(503).channels.at({0, aclptiBiuCoreKind::Aic}).size() == 1);
}

TEST(NpuComputePtiDataModule, ResultAllocationFailure)
{
    auto& module = data::ProfilingDataManager::Instance();
    aclptiProfilingDataCallback moduleCallback = [](const auto&) { return ACLPTI_ERROR_CALLBACK; };
    ASSERT_TRUE(module.Initialize(std::move(moduleCallback)) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.PrepareReplay({504, PmuEvents({})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.RecordReplayStatus({504, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(module.ReleaseReplay(504) == ACLPTI_SUCCESS);
    ASSERT_TRUE(setenv("NPU_COMPUTE_TEST_RESULT_OOM", "1", 1) == 0);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_ERROR_OUT_OF_MEMORY);
    ASSERT_TRUE(unsetenv("NPU_COMPUTE_TEST_RESULT_OOM") == 0);
    ASSERT_TRUE(module.Shutdown() == ACLPTI_ERROR_OUT_OF_MEMORY);
}

// Each collection models one original launch; replay IDs may restart in the next collection.
TEST(NpuComputePtiDataModule, CompleteProfilingIsolation)
{
    std::shared_ptr<const aclptiProfilingDataResult> firstResult;
    for (uint32_t collection = 0; collection < 2; ++collection) {
        ResultSink sink;
        auto& module = data::ProfilingDataManager::Instance();
        ASSERT_TRUE(module.Initialize([&sink](const auto& result) { return sink.Accept(result); }) == ACLPTI_SUCCESS);
        for (uint32_t round = 0; round < 2; ++round) {
            const uint32_t event = 0x700 + collection * 2 + round;
            ASSERT_TRUE(module.PrepareReplay({round, PmuEvents({event})}) == ACLPTI_SUCCESS);
            auto raw = RawData(PmuRecord(7, 8, 2, 100 + collection * 10 + round), PMU_DATA_TYPE);
            ASSERT_TRUE(module.GetRawDataCallback()(&raw) == ACLPTI_SUCCESS);
            ASSERT_TRUE(module.RecordReplayStatus({round, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
            ASSERT_TRUE(module.ReleaseReplay(round) == ACLPTI_SUCCESS);
            ASSERT_TRUE(sink.Count() == 0);
        }
        ASSERT_TRUE(module.Shutdown() == ACLPTI_SUCCESS);
        ASSERT_TRUE(sink.Count() == 1);
        const auto result = sink.Wait();
        ASSERT_TRUE(result != nullptr);
        ASSERT_TRUE(result->pmuLogs.size() == 1);
        const auto& values = result->pmuLogs.begin()->second.values;
        ASSERT_TRUE(values.size() == 2);
        ASSERT_TRUE(values.at(0x700 + collection * 2) == 100 + collection * 10);
        ASSERT_TRUE(values.at(0x701 + collection * 2) == 101 + collection * 10);
        if (collection == 0) {
            firstResult = result;
        }
    }
    ASSERT_TRUE(firstResult->pmuLogs.begin()->second.values.at(0x700) == 100);
    ASSERT_TRUE(firstResult->pmuLogs.begin()->second.values.count(0x702) == 0);
}

TEST(NpuComputePtiDataModule, SingletonRestart)
{
    auto& manager = data::ProfilingDataManager::Instance();
    ASSERT_TRUE(&manager == &data::ProfilingDataManager::Instance());
    MsprofRawDataCallback callback = nullptr;
    for (int round = 0; round < 2; ++round) {
        ResultSink sink;
        ASSERT_TRUE(manager.Initialize([&](const auto& result) { return sink.Accept(result); }) == ACLPTI_SUCCESS);
        callback = manager.GetRawDataCallback();
        ASSERT_TRUE(manager.PrepareReplay({900, PmuEvents({0x701})}) == ACLPTI_SUCCESS);
        auto raw = RawData(PmuRecord(1, 2, 0, 100 + round), PMU_DATA_TYPE);
        ASSERT_TRUE(callback(&raw) == 0);
        ASSERT_TRUE(manager.RecordReplayStatus({900, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
        ASSERT_TRUE(manager.ReleaseReplay(900) == ACLPTI_SUCCESS);
        ASSERT_TRUE(manager.Shutdown() == ACLPTI_SUCCESS);
        const auto result = sink.Wait();
        ASSERT_TRUE(result != nullptr);
        ASSERT_TRUE(result->pmuLogs.size() == 1);
        ASSERT_TRUE(callback(&raw) == ACLPTI_ERROR_NOT_INITIALIZED);
    }
}

TEST(NpuComputePtiDataModule, ProcessorPhaseBoundaries)
{
    data::DataProcessor processor;
    processor.Start();
    auto raw = RawData(PmuRecord(1, 2, 0, 100), PMU_DATA_TYPE);
    ASSERT_TRUE(processor.ReceiveRawData(&raw) == ACLPTI_ERROR_NO_ACTIVE_REPLAY);
    ASSERT_TRUE(processor.PrepareReplay({900, PmuEvents({0x701})}) == ACLPTI_SUCCESS);
    ASSERT_TRUE(processor.PrepareReplay({901, PmuEvents({0x701})}) == ACLPTI_ERROR_REPLAY_ACTIVE);
    ASSERT_TRUE(processor.ReleaseReplay(900) == ACLPTI_ERROR_INVALID_STATE);
    ASSERT_TRUE(processor.RecordReplayStatus({901, ACLPTI_SUCCESS}).status == ACLPTI_ERROR_REPLAY_NOT_FOUND);
    ASSERT_TRUE(processor.ReceiveRawData(&raw) == ACLPTI_SUCCESS);
    ASSERT_TRUE(processor.RecordReplayStatus({900, ACLPTI_SUCCESS}).status == ACLPTI_SUCCESS);
    ASSERT_TRUE(processor.ReceiveRawData(&raw) == ACLPTI_ERROR_INVALID_STATE);
    ASSERT_TRUE(processor.CloseReplay(false) == ACLPTI_ERROR_REPLAY_ACTIVE);
    ASSERT_TRUE(processor.ReleaseReplay(900) == ACLPTI_SUCCESS);
    ASSERT_TRUE(processor.CloseReplay(false) == ACLPTI_SUCCESS);
    auto result = processor.StopAndTakeResult();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->pmuLogs.size() == 1);
}
