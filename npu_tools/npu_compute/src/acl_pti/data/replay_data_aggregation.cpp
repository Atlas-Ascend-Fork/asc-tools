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
#include "data_processor.h"
#include <algorithm>
#include <iterator>

namespace aclpti::data {
namespace {
struct LogicalKey {
    uint16_t blockId = 0;
    uint16_t subBlockId = 0;
    aclptiCoreType coreType = ACLPTI_CORE_TYPE_AIC;

    bool operator<(const LogicalKey& other) const
    {
        return std::tie(blockId, subBlockId, coreType) < std::tie(other.blockId, other.subBlockId, other.coreType);
    }
};

struct RowAccumulator {
    bool initialized = false;
    aclptiPmuDataRow row;
    std::map<std::pair<aclptiCoreType, uint8_t>, uint64_t> coreCounts;

    void Add(const aclptiPmuDataRow& source)
    {
        if (!initialized) {
            row.blockId = source.blockId;
            row.subBlockId = source.subBlockId;
            row.coreType = source.coreType;
            initialized = true;
        }
        row.coreId = source.coreId;
        row.totalCycles = source.totalCycles;
        row.overflow = row.overflow || source.overflow;
        row.systemCounters.insert(row.systemCounters.end(), source.systemCounters.begin(), source.systemCounters.end());
        for (const auto& core : source.coreInfos) {
            coreCounts[{core.coreType, core.coreId}] += core.count;
        }
        if (source.coreInfos.empty()) {
            ++coreCounts[{source.coreType, source.coreId}];
        }
        for (const auto& [eventId, value] : source.values) {
            row.values.emplace(eventId, value);
        }
    }

    aclptiPmuDataRow Finish()
    {
        for (const auto& [core, count] : coreCounts) {
            row.coreInfos.push_back({core.first, core.second, count});
        }
        aclptiPmuDataRow::CoreData core;
        core.coreType = row.coreType;
        core.coreId = row.coreId;
        core.sampleCount = 1;
        core.totalCycles = row.totalCycles;
        core.overflow = row.overflow;
        core.values = row.values;
        core.systemCounters = row.systemCounters;
        for (const auto& [eventId, value] : core.values) {
            static_cast<void>(value);
            core.valueCounts.emplace(eventId, 1);
        }
        row.coreData.push_back(std::move(core));
        return std::move(row);
    }
};

void MergeRows(
    const std::map<aclptiBlockKey, aclptiPmuDataRow>& source, std::map<LogicalKey, RowAccumulator>* destination)
{
    for (const auto& [key, row] : source) {
        static_cast<void>(key);
        (*destination)[{row.blockId, row.subBlockId, row.coreType}].Add(row);
    }
}

void FinishMergedRows(
    std::map<LogicalKey, RowAccumulator>* source, std::map<aclptiBlockKey, aclptiPmuDataRow>* destination)
{
    for (auto& [key, accumulator] : *source) {
        static_cast<void>(key);
        aclptiPmuDataRow row = accumulator.Finish();
        destination->emplace(aclptiBlockKey{row.blockId, row.subBlockId, row.coreType, row.coreId}, std::move(row));
    }
}

void AddPmu(std::map<aclptiBlockKey, aclptiPmuDataRow>& rows, const PmuRecord128& record)
{
    // The physical core can change within a logical block. Normalize the map key at Finish.
    auto& row = rows[{record.blockId, record.subBlockId, record.coreType, 0}];
    row.blockId = record.blockId;
    row.subBlockId = record.subBlockId;
    row.coreType = record.coreType;
    row.coreId = record.coreId;
    row.totalCycles = static_cast<double>(record.totalCycles);
    row.values = record.pmuValues;
    row.overflow = row.overflow || record.overflow;
    row.systemCounters.push_back({record.taskStartSystemCounter, record.taskEndSystemCounter});
    auto core = std::find_if(row.coreInfos.begin(), row.coreInfos.end(), [&](const auto& value) {
        return value.coreType == record.coreType && value.coreId == record.coreId;
    });
    if (core == row.coreInfos.end()) {
        row.coreInfos.push_back({record.coreType, record.coreId, 1});
    } else {
        ++core->count;
    }
}

void FinishRows(std::map<aclptiBlockKey, aclptiPmuDataRow>& rows)
{
    std::map<aclptiBlockKey, aclptiPmuDataRow> normalized;
    while (!rows.empty()) {
        auto node = rows.extract(rows.begin());
        auto& row = node.mapped();
        std::sort(row.coreInfos.begin(), row.coreInfos.end(), [](const auto& a, const auto& b) {
            return std::tie(a.coreType, a.coreId) < std::tie(b.coreType, b.coreId);
        });
        aclptiPmuDataRow::CoreData core{};
        core.coreType = row.coreType;
        core.coreId = row.coreId;
        core.sampleCount = 1;
        core.totalCycles = row.totalCycles;
        core.overflow = row.overflow;
        core.values = row.values;
        core.systemCounters = row.systemCounters;
        for (const auto& entry : row.values) {
            core.valueCounts.emplace(entry.first, 1);
        }
        row.coreData.push_back(std::move(core));
        node.key().coreId = row.coreId;
        normalized.insert(std::move(node));
    }
    rows = std::move(normalized);
}
} // namespace

bool DataProcessor::PrepareReplayResult(uint64_t replayId, ReplayKind kind)
{
    auto [it, inserted] = replays_.try_emplace(replayId);
    if (inserted) {
        it->second.kind = kind;
    }
    return inserted;
}

void DataProcessor::AddDecodedRecord(uint64_t replayId, const DecodedRecord& record)
{
    auto& replay = replays_.at(replayId);
    if (const auto* log = std::get_if<TaskLog32>(&record.payload)) {
        aclptiTaskLogRow row{replayId,     log->funcType,   log->taskId,   log->rtStreamId, log->systemCounter,
                             log->blockId, log->subBlockId, log->coreType, log->coreTypeId};
        if (log->funcType == 0 || log->funcType == 1) {
            replay.taskLogs[log->taskId].push_back(row);
        } else {
            replay.blockLogs[{log->blockId, log->subBlockId}].push_back(row);
        }
    } else {
        const auto& pmu = std::get<PmuRecord128>(record.payload);
        AddPmu(pmu.funcType == kTaskPmuFunctionType ? replay.taskPmuLogs : replay.pmuLogs, pmu);
    }
}

aclptiResult DataProcessor::FinishReplayResult(
    uint64_t replayId, int32_t deviceId, const aclptiReplayStats& stats, aclptiResult status)
{
    auto& replay = replays_.at(replayId);
    const auto workerFailures = replay.stats.failedRecordCount;
    replay.deviceId = deviceId;
    replay.stats = stats;
    replay.stats.failedRecordCount += workerFailures;
    if (status == ACLPTI_SUCCESS && replay.stats.failedRecordCount != 0) {
        status = ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (status == ACLPTI_SUCCESS) {
        try {
            FinishRows(replay.pmuLogs);
            FinishRows(replay.taskPmuLogs);
        } catch (const std::bad_alloc&) {
            ++replay.stats.failedRecordCount;
            status = ACLPTI_ERROR_OUT_OF_MEMORY;
        }
    }
    if (status != ACLPTI_SUCCESS) {
        replay.pmuLogs.clear();
        replay.taskPmuLogs.clear();
    }
    replay.status = status;
    if (replay.kind == ReplayKind::Pipeline) {
        ASCTOOL_DEBUG(
            "[aclpti-data] pipeline replay result staged: replay=%llu device=%d status=%d received_bytes=%llu "
            "accepted_bytes=%llu "
            "rejected_chunks=%llu failed_records=%llu channels=%zu",
            static_cast<unsigned long long>(replayId), replay.deviceId, static_cast<int>(replay.status),
            static_cast<unsigned long long>(replay.stats.receivedBytes),
            static_cast<unsigned long long>(replay.stats.acceptedBytes),
            static_cast<unsigned long long>(replay.stats.rejectedChunkCount),
            static_cast<unsigned long long>(replay.stats.failedRecordCount), replay.pipelineData.size());
    }
    if (result_.status == ACLPTI_SUCCESS || status == ACLPTI_ERROR_RESULT_UNRELIABLE) {
        result_.status = status;
    }
    return status;
}

void DataProcessor::AssembleProfilingResult()
{
    std::map<LogicalKey, RowAccumulator> pmuRows;
    std::map<LogicalKey, RowAccumulator> taskPmuRows;
    for (auto& [replayId, replay] : replays_) {
        result_.stats.receivedBytes += replay.stats.receivedBytes;
        result_.stats.acceptedBytes += replay.stats.acceptedBytes;
        result_.stats.rejectedChunkCount += replay.stats.rejectedChunkCount;
        result_.stats.failedRecordCount += replay.stats.failedRecordCount;
        result_.errorStats.failedRecordCount += replay.stats.failedRecordCount;
        if (replay.stats.failedRecordCount != 0) {
            result_.errorStats.failedRecordCountByReplay[replayId] = replay.stats.failedRecordCount;
        }
        for (auto& [key, rows] : replay.taskLogs) {
            auto& destination = result_.taskLogs[key];
            destination.insert(destination.end(), rows.begin(), rows.end());
        }
        for (auto& [key, rows] : replay.blockLogs) {
            auto& destination = result_.blockLogs[key];
            destination.insert(destination.end(), rows.begin(), rows.end());
        }
        if (replay.kind == ReplayKind::Pmu && replay.status == ACLPTI_SUCCESS) {
            MergeRows(replay.pmuLogs, &pmuRows);
            MergeRows(replay.taskPmuLogs, &taskPmuRows);
        }
        if (replay.kind == ReplayKind::Pipeline) {
            result_.pipelineData.emplace(
                replayId,
                aclptiPipelineData{
                    replay.deviceId, replay.status, replay.stats, replay.format, std::move(replay.pipelineData)});
        }
        result_.pcSamplingData.insert(
            result_.pcSamplingData.end(), std::make_move_iterator(replay.pcSamplingData.begin()),
            std::make_move_iterator(replay.pcSamplingData.end()));
    }
    FinishMergedRows(&pmuRows, &result_.pmuLogs);
    FinishMergedRows(&taskPmuRows, &result_.taskPmuLogs);
    replays_.clear();
}
} // namespace aclpti::data
