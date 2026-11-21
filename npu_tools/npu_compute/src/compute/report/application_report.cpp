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
#include "application_summary_writer.h"
#include "npu_compute_log.h"
#include <algorithm>
#include <sstream>
#include <unistd.h>

namespace npucompute {
namespace {
constexpr unsigned kTaskSnLowBits = 16U;
constexpr size_t kHexDigitsPerByte = 2;
constexpr unsigned kBitsPerHexDigit = 4U;
constexpr unsigned kHexDigitMask = 0x0fU;
constexpr uint64_t kMetadataRecordCount = 1;
constexpr char kApplicationSchemaVersionJson[] = "1";
constexpr char kApplicationPassIdJson[] = "0";

const char* StateName(aclpti::profiling::ApplicationDeviceState state)
{
    using State = aclpti::profiling::ApplicationDeviceState;
    switch (state) {
        case State::Inactive:
            return "inactive";
        case State::Starting:
            return "starting";
        case State::Active:
            return "active";
        case State::Stopping:
            return "stopping";
        case State::Unknown:
            return "unknown";
    }
    return "unknown";
}

const char* EndReasonName(aclpti::profiling::ApplicationEndReason reason)
{
    using Reason = aclpti::profiling::ApplicationEndReason;
    switch (reason) {
        case Reason::AclFinalize:
            return "acl_finalize";
        case Reason::ProcessExit:
            return "process_exit_without_finalize";
        case Reason::LocalShutdown:
            return "local_shutdown_without_finalize";
        case Reason::StartupRollback:
            return "startup_rollback";
        case Reason::FinalizeWhileBusy:
            return "acl_finalize_with_inflight_access";
        case Reason::Unconfirmed:
            return "unconfirmed";
    }
    return "unconfirmed";
}

const char* FailureReason(const aclpti::profiling::ApplicationSummary& summary)
{
    if (summary.tailConfirmed) {
        return "null";
    }
    if (summary.endReason == aclpti::profiling::ApplicationEndReason::FinalizeWhileBusy) {
        return R"("acl_finalize_with_inflight_access")";
    }
    return R"("acl_finalize_not_observed")";
}

const char* EndReasonName(aclpti::profiling::ApplicationDeviceEndReason reason)
{
    using Reason = aclpti::profiling::ApplicationDeviceEndReason;
    switch (reason) {
        case Reason::ResetStop:
            return "reset_stop";
        case Reason::RuntimeFinalize:
            return "runtime_finalize";
        case Reason::Unconfirmed:
            return "unconfirmed";
    }
    return "unconfirmed";
}
} // namespace

void ApplicationReport::Begin()
{
    if (begun_) {
        return;
    }
    sink_.Begin("application_raw.jsonl");
    try {
        sink_.Write(
            std::string("{\"recordType\":\"metadata\",\"schemaVersion\":") + kApplicationSchemaVersionJson +
            ",\"replayMode\":\"application\",\"collectionKind\":\"communication\",\"processId\":" +
            std::to_string(getpid()) + ",\"passId\":" + kApplicationPassIdJson + "}\n");
        begun_ = true;
    } catch (...) {
        sink_.Abort();
        throw;
    }
}

void ApplicationReport::Consume(const aclpti::profiling::ApplicationRawRecord& record)
{
    Begin();
    for (const auto& decoded : record.decoded) {
        std::visit(
            [&](const auto& value) {
                // David raw fields hold the high/low halves of taskSn, not the exposed stream ID.
                const uint32_t taskSn = (static_cast<uint32_t>(value.taskId) << kTaskSnLowBits) | value.rtStreamId;
                tasks_[{record.deviceId, record.deviceEpoch, taskSn}].push_back(decoded);
            },
            decoded.payload);
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
        row << "{\"recordType\":\"raw\",\"passId\":" << kApplicationPassIdJson << ",\"deviceId\":" << record.deviceId
            << ",\"deviceEpoch\":" << record.deviceEpoch << ",\"rawType\":" << record.type
            << ",\"chunkModule\":" << record.chunkModule << ",\"offset\":" << record.offset
            << ",\"isLastChunk\":" << (record.isLastChunk ? "true" : "false") << ",\"payloadHex\":\"" << hex << "\"}\n";
        sink_.Write(row.str());
        ++records_;
    } catch (...) {
        sink_.Abort();
        throw;
    }
}

std::string ApplicationReport::WriteOpSummaries(
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
    std::string units;
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
        units += (units.empty() ? "" : ",") + WriteApplicationOpSummary(op, association, summary, sink_);
    }
    return units;
}

void ApplicationReport::Finish(
    const aclpti::profiling::ApplicationSummary& summary, const ApplicationOpSnapshot& snapshot)
{
    ASCTOOL_INFO(
        "[npu-compute diagnostic] application_report finish begin: raw_records=%llu task_keys=%zu ops=%zu "
        "hardware_bytes=%zu tail_confirmed=%d complete=%d",
        static_cast<unsigned long long>(records_), tasks_.size(), snapshot.ops.size(), snapshot.hardwareJsonl.size(),
        summary.tailConfirmed, summary.complete);
    Begin();
    try {
        sink_.Commit(records_ + kMetadataRecordCount);
        ASCTOOL_INFO("[npu-compute diagnostic] application_report raw committed");
        if (!snapshot.hardwareJsonl.empty()) {
            sink_.Begin("HardwareInfo.jsonl");
            sink_.Write(snapshot.hardwareJsonl);
            sink_.Commit(
                static_cast<uint64_t>(std::count(snapshot.hardwareJsonl.begin(), snapshot.hardwareJsonl.end(), '\n')));
            ASCTOOL_INFO("[npu-compute diagnostic] application_report hardware committed");
        }
        const auto units = WriteOpSummaries(summary, snapshot);
        const auto manifest = FormatManifest(summary, snapshot, units);
        sink_.Begin("application_manifest.json");
        sink_.Write(manifest);
        sink_.Commit(1);
        ASCTOOL_INFO("[npu-compute diagnostic] application_report manifest committed");
    } catch (...) {
        ASCTOOL_ERROR("[npu-compute diagnostic] application_report finish failed; aborting publisher");
        sink_.Abort();
        throw;
    }
}

std::string ApplicationReport::FormatManifest(
    const aclpti::profiling::ApplicationSummary& summary, const ApplicationOpSnapshot& snapshot,
    const std::string& units)
{
    std::ostringstream manifest;
    manifest << "{\"schemaVersion\":" << kApplicationSchemaVersionJson
             << ",\"replayMode\":\"application\",\"collectionKind\":\"communication\",\"processId\":" << getpid()
             << ",\"passId\":" << kApplicationPassIdJson << ",\"receivedRecords\":" << summary.receivedRecords
             << ",\"processedRecords\":" << summary.processedRecords << ",\"droppedRecords\":" << summary.droppedRecords
             << ",\"invalidRecords\":" << summary.invalidRecords << ",\"processingErrors\":" << summary.processingErrors
             << ",\"localQueueDrained\":true,\"tailConfirmed\":" << (summary.tailConfirmed ? "true" : "false")
             << ",\"localCollectionHealthy\":" << (summary.complete ? "true" : "false")
             << ",\"complete\":" << (summary.complete && summary.tailConfirmed ? "true" : "false")
             << ",\"endReason\":\"" << EndReasonName(summary.endReason) << "\""
             << ",\"failureReason\":" << FailureReason(summary) << ",\"devices\":[";
    bool first = true;
    for (const auto& [deviceId, device] : summary.devices) {
        if (!first) {
            manifest << ',';
        }
        first = false;
        manifest << "{\"deviceId\":" << deviceId << ",\"deviceEpoch\":" << device.deviceEpoch << ",\"state\":\""
                 << StateName(device.state) << "\",\"startAttempted\":" << (device.startAttempted ? "true" : "false")
                 << ",\"startStatus\":" << device.startStatus
                 << ",\"stopAttempted\":" << (device.stopAttempted ? "true" : "false")
                 << ",\"stopStatus\":" << (device.stopAttempted ? std::to_string(device.stopStatus) : "null")
                 << ",\"endReason\":\"" << EndReasonName(device.endReason) << "\"}";
    }
    manifest << "],\"unidentifiedLaunches\":" << snapshot.unidentifiedLaunches << ",\"result_units\":[" << units
             << "]}\n";
    return manifest.str();
}
} // namespace npucompute
