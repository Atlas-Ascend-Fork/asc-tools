/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "data_types.h"

#include "data_processor.h"

#include "raw_data_decoder.h"
#include <cstdlib>
#include <stdexcept>

namespace aclpti::data {
namespace {
void LogDecodedRecord(uint64_t replayId, uint64_t index, const DecodeResult& decoded)
{
    if (!ASCTOOL_DEBUG_ENABLED()) {
        return;
    }
    ASCTOOL_DEBUG(
        "[aclpti-data] decode replay=%llu record=%llu status=%d", static_cast<unsigned long long>(replayId),
        static_cast<unsigned long long>(index), static_cast<int>(decoded.Status()));
    if (!decoded.Ok()) {
        return;
    }
    if (const auto* log = std::get_if<TaskLog32>(&decoded.Value().payload)) {
        ASCTOOL_DEBUG(
            "[aclpti-data] task-log function=%u task=%u stream=%u counter=%llu block=%u subBlock=%u coreType=%d "
            "core=%u",
            log->funcType, log->taskId, log->rtStreamId, static_cast<unsigned long long>(log->systemCounter),
            log->blockId, log->subBlockId, static_cast<int>(log->coreType), log->coreTypeId);
    } else {
        const auto& pmu = std::get<PmuRecord128>(decoded.Value().payload);
        ASCTOOL_DEBUG(
            "[aclpti-data] pmu function=%u task=%u stream=%u cycles=%llu block=%u subBlock=%u coreType=%d core=%u",
            pmu.funcType, pmu.taskId, pmu.rtStreamId, static_cast<unsigned long long>(pmu.totalCycles), pmu.blockId,
            pmu.subBlockId, static_cast<int>(pmu.coreType), pmu.coreId);
    }
}

void AllocationFailure(const char* variable)
{
#if defined(NPU_COMPUTE_ENABLE_TEST_CONTROLS)
    const char* value = std::getenv(variable);
    if (value != nullptr && value[0] != '\0') {
        throw std::bad_alloc();
    }
#else
    static_cast<void>(variable);
#endif
}
} // namespace

DataProcessor::~DataProcessor() { Close(); }

void DataProcessor::Start()
{
    if (thread_.joinable()) {
        throw std::logic_error("DataProcessor is already running");
    }
    // The manager serializes lifecycle calls; the previous thread has already joined.
    result_ = {};
    replays_.clear();
    active_.reset();
    queue_.Reset();
    submitted_ = 0;
    completed_ = 0;
    thread_ = std::thread(&DataProcessor::Run, this);
}

aclptiResult DataProcessor::PrepareReplay(const ReplayPrepareInfo& info)
{
    if (active_) {
        return ACLPTI_ERROR_REPLAY_ACTIVE;
    }
    try {
        if (!PrepareReplayResult(info.replayId, info.kind)) {
            return ACLPTI_ERROR_INVALID_PARAMETER;
        }
        // Keep parsing context for the complete result, independently of per-round data.
        if (info.kind == ReplayKind::PcSampling && info.pmSamplingContext) {
            result_.pmSamplingContexts.emplace(info.replayId, info.pmSamplingContext);
        }
        active_.emplace();
        active_->info = info;
    } catch (const std::bad_alloc&) {
        replays_.erase(info.replayId);
        result_.pmSamplingContexts.erase(info.replayId);
        active_.reset();
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
    return ACLPTI_SUCCESS;
}

ReplayResult DataProcessor::RecordReplayStatus(const ReplayStopInfo& info)
{
    ReplayResult result{info.replayId, CheckActive(info.replayId), {}};
    if (result.status != ACLPTI_SUCCESS) {
        return result;
    }
    result.callbackStats = active_->callbackStats;
    if (active_->closed) {
        result.status = ACLPTI_ERROR_INVALID_STATE;
        return result;
    }
    result.status = FinishActive(info.stopStatus);
    return result;
}

aclptiResult DataProcessor::ReleaseReplay(uint64_t replayId)
{
    const auto status = CheckActive(replayId);
    if (status != ACLPTI_SUCCESS) {
        return status;
    }
    if (!active_->closed) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    active_.reset();
    return ACLPTI_SUCCESS;
}

aclptiResult DataProcessor::CheckActive(uint64_t replayId) const
{
    if (!active_) {
        return ACLPTI_ERROR_NO_ACTIVE_REPLAY;
    }
    if (active_->info.replayId != replayId) {
        return ACLPTI_ERROR_REPLAY_NOT_FOUND;
    }
    return ACLPTI_SUCCESS;
}

// ProfilingDataManager mutex excludes producers; worker never acquires it.
aclptiResult DataProcessor::FinishActive(aclptiResult status)
{
    active_->closed = true;
    if (active_->HasOpenPacket()) {
        ++active_->stats.failedRecordCount;
        if (status == ACLPTI_SUCCESS) {
            status = ACLPTI_ERROR_TRACE_INCOMPLETE;
        }
    }
    Wait();
    if (active_->info.kind == ReplayKind::Pipeline) {
        ASCTOOL_DEBUG(
            "[aclpti-data] pipeline replay finalized: replay=%llu kind=%d device=%d status=%d received_bytes=%llu "
            "accepted_bytes=%llu "
            "rejected_chunks=%llu failed_records=%llu copied_records=%llu copied_bytes=%llu queued_items=%llu",
            static_cast<unsigned long long>(active_->info.replayId), static_cast<int>(active_->info.kind),
            active_->deviceId, static_cast<int>(status), static_cast<unsigned long long>(active_->stats.receivedBytes),
            static_cast<unsigned long long>(active_->stats.acceptedBytes),
            static_cast<unsigned long long>(active_->stats.rejectedChunkCount),
            static_cast<unsigned long long>(active_->stats.failedRecordCount),
            static_cast<unsigned long long>(active_->callbackStats.copiedRecordCount),
            static_cast<unsigned long long>(active_->callbackStats.copiedBytes),
            static_cast<unsigned long long>(submitted_));
    }
    return FinishReplayResult(active_->info.replayId, active_->deviceId, active_->stats, status);
}

aclptiResult DataProcessor::CloseReplay(bool force)
{
    if (active_ && !force) {
        return ACLPTI_ERROR_REPLAY_ACTIVE;
    }
    if (active_) {
        if (!active_->closed) {
            FinishActive(
                active_->callbackStats.copiedRecordCount == 0 ? ACLPTI_SUCCESS : ACLPTI_ERROR_RESULT_UNRELIABLE);
        }
        active_.reset();
    }
    return ACLPTI_SUCCESS;
}

std::optional<aclptiProfilingDataResult> DataProcessor::StopAndTakeResult()
{
    Close();
    active_.reset();
    if (replays_.empty()) {
        return std::nullopt;
    }
    AssembleProfilingResult();
    return std::move(result_);
}

bool DataProcessor::Submit(DataItem item)
{
    AllocationFailure("NPU_COMPUTE_TEST_QUEUE_OOM");
    if (!queue_.Push(std::move(item))) {
        return false;
    }
    ++submitted_;
    return true;
}

void DataProcessor::Wait()
{
    // The caller excludes concurrent Submit calls, so this completion target is stable.
    const uint64_t target = submitted_;
    std::unique_lock<std::mutex> lock(completionMutex_);
    completion_.wait(lock, [&] { return completed_ == target; });
}

void DataProcessor::Close()
{
    queue_.Close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DataProcessor::Process(DataItem& item)
{
    auto& replay = replays_.at(item.replayId);
    if (const auto* raw = std::get_if<RawRecord>(&item.payload)) {
        AllocationFailure("NPU_COMPUTE_TEST_DECODE_OOM");
        auto decoded = DecodeRawRecord(raw->bytes.data(), raw->size, raw->recordIndex, raw->events);
        LogDecodedRecord(item.replayId, raw->recordIndex, decoded);
        if (!decoded.Ok()) {
            ++replay.stats.failedRecordCount;
            return;
        }
        AddDecodedRecord(item.replayId, decoded.Value());
    } else if (const auto* biu = std::get_if<BiuChunk>(&item.payload)) {
        AllocationFailure("NPU_COMPUTE_TEST_OPAQUE_AGGREGATE_OOM");
        // Decode byte order only; BIU timing and interval reconstruction belong to libnpu-compute.
        auto& words = replay.pipelineData[biu->key];
        const size_t begin = words.size();
        const size_t count = biu->bytes.size() / sizeof(uint32_t);
        // Resize once to preserve geometric growth; no intermediate word vector is needed.
        words.resize(begin + count);
        for (size_t index = 0; index < count; ++index) {
            words[begin + index] = ReadLittleEndianWord(biu->bytes.data() + index * sizeof(uint32_t));
        }
        replay.format = aclptiBiuFormat::Chip6;
    } else {
        AllocationFailure("NPU_COMPUTE_TEST_OPAQUE_AGGREGATE_OOM");
        replay.pcSamplingData.push_back(std::move(std::get<aclptiRawDataChunk>(item.payload)));
    }
}

void DataProcessor::Run()
{
    DataItem item;
    while (queue_.Pop(item)) {
        try {
            Process(item);
        } catch (...) {
            ++replays_.at(item.replayId).stats.failedRecordCount;
        }
        {
            std::lock_guard<std::mutex> lock(completionMutex_);
            // Every accepted item advances completion, even if processing throws, to unblock Wait.
            ++completed_;
        }
        completion_.notify_all();
    }
}
} // namespace aclpti::data
