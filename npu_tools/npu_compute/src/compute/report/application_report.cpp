/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "application_report.h"
#include "acl_pti/data/communication_protocol.h"
#include "application_summary_writer.h"
#include "communication/artifact_schema.h"
#include "npu_compute_log.h"
#include <algorithm>
#include <sstream>

namespace npucompute {
namespace {
constexpr unsigned kTaskSnLowBits = 16U;
constexpr size_t kHexDigitsPerByte = 2;
constexpr unsigned kBitsPerHexDigit = 4U;
constexpr unsigned kHexDigitMask = 0x0fU;
} // namespace

void ApplicationReport::WriteTaskTrackChunk(const aclpti::profiling::ApplicationRawRecord& record)
{
    namespace wire = aclpti::communication::protocol;
    if (record.deviceId != wire::kHostDeviceId || (record.type != wire::kCompact && record.type != wire::kAdditional)) {
        return;
    }
    if (taskTrackChunks_ == 0) {
        sink_.Begin(std::string(communication::kArtifactPrefix) + communication::kTaskTracksFileName);
    }
    try {
        static constexpr char digits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(record.bytes.size() * kHexDigitsPerByte);
        for (const auto byte : record.bytes) {
            hex.push_back(digits[byte >> kBitsPerHexDigit]);
            hex.push_back(digits[byte & kHexDigitMask]);
        }
        std::ostringstream row;
        row << "{\"deviceId\":" << record.deviceId << ",\"deviceEpoch\":" << record.deviceEpoch
            << ",\"rawType\":" << record.type << ",\"chunkModule\":" << record.chunkModule
            << ",\"offset\":" << record.offset << ",\"isLastChunk\":" << (record.isLastChunk ? "true" : "false")
            << ",\"payloadHex\":\"" << hex << "\"}\n";
        sink_.Write(row.str());
        ++taskTrackChunks_;
    } catch (...) {
        sink_.Abort();
        throw;
    }
}

void ApplicationReport::Consume(const aclpti::profiling::ApplicationRawRecord& record)
{
    for (const auto& decoded : record.decoded) {
        std::visit(
            [&](const auto& value) {
                // David raw fields hold the high/low halves of taskSn, not the exposed stream ID.
                const uint32_t taskSn = (static_cast<uint32_t>(value.taskId) << kTaskSnLowBits) | value.rtStreamId;
                tasks_[{record.deviceId, record.deviceEpoch, taskSn}].push_back(decoded);
            },
            decoded.payload);
    }
    WriteTaskTrackChunk(record);
}

void ApplicationReport::WriteOpSummaries(
    const aclpti::profiling::ApplicationSummary& summary, const ApplicationOpSnapshot& snapshot)
{
    std::map<TaskKey, size_t> identities;
    const auto hasIdentity = [](const ApplicationOpInfo& op) { return op.taskId.has_value(); };
    const auto key = [](const ApplicationOpInfo& op) {
        return TaskKey{*op.metadata.deviceId, op.deviceEpoch, *op.taskId};
    };
    for (const auto& op : snapshot.ops) {
        if (hasIdentity(op)) {
            ++identities[key(op)];
        }
    }
    for (const auto& op : snapshot.ops) {
        const bool validIdentity = hasIdentity(op);
        const bool ambiguous = !validIdentity || identities[key(op)] != 1;
        const auto found = validIdentity && !ambiguous ? tasks_.find(key(op)) : tasks_.end();
        ApplicationTaskAssociation association;
        association.records = found == tasks_.end() ? nullptr : &found->second;
        if (op.deviceEpoch == 0) {
            association.unavailableReason = "device_collection_epoch_unavailable";
        } else if (!op.taskId || !op.streamId) {
            association.unavailableReason = "runtime_task_identity_unavailable";
        } else if (ambiguous) {
            association.unavailableReason = "task_identity_reused";
        } else if (association.records == nullptr) {
            association.unavailableReason = "task_data_not_received_for_device_epoch";
        }
        WriteApplicationOpSummary(op, association, summary, sink_);
    }
}

void ApplicationReport::Finish(
    const aclpti::profiling::ApplicationSummary& summary, const ApplicationOpSnapshot& snapshot)
{
    ASCTOOL_INFO(
        "[npu-compute diagnostic] application_report finish begin: task_track_chunks=%llu task_keys=%zu ops=%zu "
        "unidentified_launches=%llu hardware_bytes=%zu tail_confirmed=%d complete=%d",
        static_cast<unsigned long long>(taskTrackChunks_), tasks_.size(), snapshot.ops.size(),
        static_cast<unsigned long long>(snapshot.unidentifiedLaunches), snapshot.hardwareJsonl.size(),
        summary.tailConfirmed, summary.complete);
    try {
        if (taskTrackChunks_ != 0) {
            sink_.Commit(taskTrackChunks_);
        }
        if (!snapshot.hardwareJsonl.empty()) {
            sink_.Begin("HardwareInfo.jsonl");
            sink_.Write(snapshot.hardwareJsonl);
            sink_.Commit(
                static_cast<uint64_t>(std::count(snapshot.hardwareJsonl.begin(), snapshot.hardwareJsonl.end(), '\n')));
            ASCTOOL_INFO("[npu-compute diagnostic] application_report hardware committed");
        }
        WriteOpSummaries(summary, snapshot);
    } catch (...) {
        ASCTOOL_ERROR("[npu-compute diagnostic] application_report finish failed; aborting publisher");
        sink_.Abort();
        throw;
    }
}
} // namespace npucompute
