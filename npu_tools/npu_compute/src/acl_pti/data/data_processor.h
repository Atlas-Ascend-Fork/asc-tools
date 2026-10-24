/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_DATA_PROCESSOR_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_DATA_PROCESSOR_H

#include "bounded_queue.h"
#include "data_types.h"
#include "profiling/prof_api.h"
#include "raw_data_decoder.h"
#include <array>
#include <map>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace aclpti::data {
// The manager serializes all public calls. Only the worker writes results between
// PrepareReplay and RecordReplayStatus; Wait transfers access back to the manager thread.
class DataProcessor {
public:
    ~DataProcessor();
    void Start();
    // The manager validates replay configuration before calling this internal entry point.
    aclptiResult PrepareReplay(const ReplayPrepareInfo& info);
    ReplayResult RecordReplayStatus(const ReplayStopInfo& info);
    aclptiResult ReleaseReplay(uint64_t replayId);
    aclptiResult ReceiveRawData(const MsprofRawData* raw);
    aclptiResult CloseReplay(bool force);
    // Manager finishes any active replay first; joining transfers result ownership to the caller.
    // No result means no original launch started; an empty prepared collection still has a result.
    std::optional<aclptiProfilingDataResult> StopAndTakeResult();

private:
    // Owns one PMU/task-log record and a copy of its replay's PMU slot configuration.
    struct RawRecord {
        // GCC 7 evaluates variant's constructor traits before the enclosing class is complete.
        // Use a constructor instead of default member initializers for this nested alternative.
        RawRecord() : bytes{}, size(0), recordIndex(0), events{} {}

        std::array<std::byte, PMU_RECORD_BYTES> bytes;
        size_t size;
        uint64_t recordIndex;
        PmuSlots events;
    };
    // Receive-side bytes are owned here; the worker converts them to little-endian words.
    struct BiuChunk {
        aclptiPipelineKey key;
        std::vector<uint8_t> bytes;
    };
    // Queue entries never retain Msprof callback memory or references to ActiveReplay.
    struct DataItem {
        uint64_t replayId = 0;
        std::variant<RawRecord, BiuChunk, aclptiRawDataChunk> payload{RawRecord{}};
    };

    // Transport continuity for one source channel. A new packet must restart at offset zero.
    struct ReceiveState {
        size_t expectedOffset = 0;
        bool packetOpen = false;
    };

    // Owned exclusively by DataProcessor; access serialized by the manager mutex; never referenced by queued work.
    struct ActiveReplay {
        ReplayPrepareInfo info;
        bool closed = false;
        int32_t deviceId = -1; // Bound by the first successfully queued item; later callbacks must match.
        CallbackStats callbackStats{};
        aclptiReplayStats stats;
        uint64_t nextRecordIndex = 0;
        std::map<aclptiPipelineKey, ReceiveState> biuChannels;
        std::map<int32_t, ReceiveState> pcChannels;

        // An unfinished transport packet is different from an unclosed BIU busy interval.
        bool HasOpenPacket() const;
    };

    // Per-round staging stays internal; only successful PMU rounds enter the complete launch result.
    struct ReplayData {
        int32_t deviceId = -1;
        ReplayKind kind = ReplayKind::Pmu;
        aclptiResult status = ACLPTI_SUCCESS;
        aclptiReplayStats stats;
        aclptiBiuFormat format = aclptiBiuFormat::Unknown;
        std::map<uint16_t, std::vector<aclptiTaskLogRow>> taskLogs;
        std::map<aclptiBlockKey, std::vector<aclptiTaskLogRow>> blockLogs;
        std::map<aclptiBlockKey, aclptiPmuDataRow> pmuLogs;
        std::map<aclptiBlockKey, aclptiPmuDataRow> taskPmuLogs;
        std::map<aclptiPipelineKey, std::vector<uint32_t>> pipelineData;
        std::vector<aclptiRawDataChunk> pcSamplingData;
    };

    void AssembleProfilingResult();

    // Result access follows the same phase ownership as PrepareReplay/Process/FinishActive.
    bool PrepareReplayResult(uint64_t replayId, ReplayKind kind);
    void AddDecodedRecord(uint64_t replayId, const DecodedRecord& record);
    aclptiResult FinishReplayResult(
        uint64_t replayId, int32_t deviceId, const aclptiReplayStats& stats, aclptiResult status);

    aclptiResult CheckActive(uint64_t replayId) const;
    aclptiResult FinishActive(aclptiResult status);
    aclptiResult ReceiveDataChunk(const MsprofRawData& raw, std::optional<aclptiBiuCoreKind> core);
    bool Submit(DataItem item);
    void Run();
    void Process(DataItem& item);
    void Wait();
    void Close();

    std::optional<ActiveReplay> active_;
    std::map<uint64_t, ReplayData> replays_;
    aclptiProfilingDataResult result_;
    BoundedQueue<DataItem> queue_{4096};
    std::thread thread_;
    uint64_t submitted_ = 0; // Public calls are serialized by the manager.
    uint64_t completed_ = 0; // Protected by completionMutex_; includes failed work.
    std::mutex completionMutex_;
    std::condition_variable completion_;
};
} // namespace aclpti::data

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_DATA_DATA_PROCESSOR_H
