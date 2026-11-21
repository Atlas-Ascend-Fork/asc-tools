/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "application_summary_writer.h"
#include "report/report_writer.h"
#include "acl_pti/data/data_types.h"
#include "npu_compute_log.h"
#include <algorithm>
#include <sstream>
#include <string_view>
#include <unistd.h>

namespace npucompute {
namespace {
std::string Quote(std::string_view value) { return QuoteJsonString(value, std::locale()); }

std::string OpDirectory(const ApplicationOpInfo& op)
{
    static constexpr char kHex[] = "0123456789abcdef";
    constexpr unsigned kBitsPerHexDigit = 4U;
    constexpr unsigned kHexDigitMask = 0xfU;
    constexpr size_t kMaxEncodedNameBytes = 64;
    std::string component;
    for (const unsigned char ch : *op.metadata.name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-') {
            component.push_back(ch);
        } else {
            component += '_';
            component += kHex[ch >> kBitsPerHexDigit];
            component += kHex[ch & kHexDigitMask];
        }
    }
    // Long C++ symbols cannot fit in the existing IPC/rep filename fields; the manifest retains the full name.
    component =
        component.size() <= kMaxEncodedNameBytes ? "op-" + component : "instance-" + std::to_string(op.instanceIndex);
    return "device" + std::to_string(*op.metadata.deviceId) + '/' + component + '/' + std::to_string(op.launchIndex) +
           '/';
}

void AddPmuRecord(aclptiProfilingDataResult& result, const aclpti::data::PmuRecord128& pmu)
{
    using namespace aclpti::data;
    if (pmu.funcType == kTaskPmuFunctionType) {
        auto& side = pmu.coreType == ACLPTI_CORE_TYPE_AIC ? result.taskPmuData.aic : result.taskPmuData.aiv;
        if (side) {
            result.status = ACLPTI_ERROR_INVALID_RAW_DATA;
        }
        side = aclptiTaskPmuCoreData{static_cast<double>(pmu.totalCycles), pmu.overflow, pmu.pmuValues};
        return;
    }
    const aclptiBlockKey key{pmu.blockId, pmu.subBlockId, pmu.coreType, pmu.coreId};
    if (result.pmuLogs.count(key) != 0) {
        result.status = ACLPTI_ERROR_INVALID_RAW_DATA;
    }
    auto& row = result.pmuLogs[key];
    row.blockId = pmu.blockId;
    row.subBlockId = pmu.subBlockId;
    row.coreType = pmu.coreType;
    row.coreId = pmu.coreId;
    row.totalCycles = static_cast<double>(pmu.totalCycles);
    row.overflow = pmu.overflow;
    row.values = pmu.pmuValues;
    row.coreInfos.push_back({pmu.coreType, pmu.coreId, 1});
    row.systemCounters.push_back({pmu.taskStartSystemCounter, pmu.taskEndSystemCounter});
    aclptiPmuDataRow::CoreData core{};
    core.coreType = row.coreType;
    core.coreId = row.coreId;
    core.sampleCount = 1;
    core.totalCycles = row.totalCycles;
    core.overflow = row.overflow;
    core.values = row.values;
    core.systemCounters = row.systemCounters;
    for (const auto& [event, value] : core.values) {
        (void)value;
        core.valueCounts.emplace(event, 1);
    }
    row.coreData.push_back(std::move(core));
}

aclptiProfilingDataResult TaskData(const std::vector<aclpti::data::DecodedRecord>* records)
{
    constexpr uint64_t kApplicationReplayId = 0;
    aclptiProfilingDataResult result;
    if (records == nullptr) {
        return result;
    }
    for (const auto& record : *records) {
        if (const auto* log = std::get_if<aclpti::data::TaskLog32>(&record.payload)) {
            aclptiTaskLogRow row{kApplicationReplayId, log->funcType, log->taskId,     log->rtStreamId,
                                 log->systemCounter,   log->blockId,  log->subBlockId, log->coreType,
                                 log->coreTypeId,      log->sqeType};
            if (log->funcType == aclpti::data::kTaskStartFunctionType ||
                log->funcType == aclpti::data::kTaskEndFunctionType) {
                result.taskLogs[log->taskId].push_back(row);
            } else {
                result.blockLogs[{log->blockId, log->subBlockId, log->coreType, log->coreTypeId}].push_back(row);
            }
        } else {
            AddPmuRecord(result, std::get<aclpti::data::PmuRecord128>(record.payload));
        }
    }
    if (result.status != ACLPTI_SUCCESS) {
        result.pmuLogs.clear();
        result.taskPmuData = {};
    }
    return result;
}

std::string UnavailableReasons(const std::vector<std::string>& unavailableFields, const std::string& associationReason)
{
    std::ostringstream reasons;
    reasons << "{\"communication_time\":\"communication_intervals_not_parsed\","
               "\"compute_communication_overlap_time\":\"communication_intervals_not_parsed\","
               "\"compute_overhead_time\":\"communication_intervals_not_parsed\"";
    const char* fields[] = {
        "Task Duration(us)",
        "Op Type",
        "Block Dim",
        "Mix Block Dim",
        "Current Freq",
        "Rated Freq",
        "aic_flops_theoretical",
        "aiv_flops_theoretical",
        "aic_flops",
        "aiv_flops",
        "aicore_parallel_utilization",
        "aicore_parallel_balance",
        "aicore_gm_read_bw(GB/s)",
        "aicore_gm_write_bw(GB/s)",
        "aicore_gm_bw_usage_rate(%)"};
    for (const auto* field : fields) {
        if (std::find(unavailableFields.begin(), unavailableFields.end(), field) != unavailableFields.end()) {
            reasons << ',' << Quote(field) << ':'
                    << Quote(
                           associationReason.empty() ? "required_task_pmu_or_hardware_metadata_unavailable" :
                                                       associationReason);
        }
    }
    reasons << '}';
    return reasons.str();
}
} // namespace

std::string WriteApplicationOpSummary(
    const ApplicationOpInfo& op, const ApplicationTaskAssociation& association,
    const aclpti::profiling::ApplicationSummary& summary, ArtifactSink& sink)
{
    constexpr std::string_view kJsonlObjectSuffix = "}\n";
    std::string reason = association.unavailableReason;
    const auto* records = association.records;
    auto result = TaskData(reason.empty() ? records : nullptr);
    if (result.status != ACLPTI_SUCCESS) {
        reason = "duplicate_pmu_records";
    }
    const auto directory = OpDirectory(op);
    const auto id = "p" + std::to_string(getpid()) + "_d" + std::to_string(*op.metadata.deviceId) + "_e" +
                    std::to_string(op.deviceEpoch) + "_k" + std::to_string(op.instanceIndex);
    const auto instance = "k" + std::to_string(op.instanceIndex);
    ASCTOOL_INFO(
        "[npu-compute diagnostic] op_summary begin: id=%s name=%s task=%s stream=%s records=%zu "
        "task_logs=%zu block_pmu=%zu task_aic=%d task_aiv=%d association_reason=%s",
        id.c_str(), op.metadata.name->c_str(), op.taskId ? std::to_string(*op.taskId).c_str() : "NA",
        op.streamId ? std::to_string(*op.streamId).c_str() : "NA", records == nullptr ? 0 : records->size(),
        result.taskLogs.size(), result.pmuLogs.size(), result.taskPmuData.aic.has_value(),
        result.taskPmuData.aiv.has_value(), reason.c_str());
    const auto row = FormatOpInfoSummary(result, ReportConfig{}, op.metadata);
    // The diagnostic retains the size of the original complete JSONL record.
    ASCTOOL_DEBUG(
        "[npu-compute diagnostic] op_summary formatted: id=%s bytes=%zu", id.c_str(),
        row.fields.size() + kJsonlObjectSuffix.size());
    std::ostringstream output;
    output << row.fields << ",\"result_unit_id\":" << Quote(id) << ",\"kernel_instance_id\":" << Quote(instance)
           << ",\"device_epoch\":" << op.deviceEpoch << ",\"launch_index\":" << op.launchIndex
           << ",\"Task Id\":" << (op.taskId ? std::to_string(*op.taskId) : "\"NA\"")
           << ",\"Stream Id\":" << (op.streamId ? std::to_string(*op.streamId) : "\"NA\"")
           << ",\"communication_time\":\"NA\",\"compute_communication_overlap_time\":\"NA\","
              "\"compute_overhead_time\":\"NA\",\"communication_semantics\":\"api_call\",\"collection_status\":"
           << (summary.complete && summary.tailConfirmed ? "\"complete\"" : "\"partial\"")
           << ",\"unavailable_reasons\":" << UnavailableReasons(row.unavailableFields, reason) << kJsonlObjectSuffix;
    sink.Begin(directory + "summary.jsonl");
    try {
        sink.Write(output.str());
        sink.Commit(1);
        ASCTOOL_INFO("[npu-compute diagnostic] op_summary committed: id=%s", id.c_str());
    } catch (...) {
        sink.Abort();
        throw;
    }
    std::ostringstream unit;
    unit << "{\"result_unit_id\":" << Quote(id) << ",\"kernel_instance_id\":" << Quote(instance)
         << ",\"device_id\":" << *op.metadata.deviceId << ",\"device_epoch\":" << op.deviceEpoch
         << ",\"op_name\":" << Quote(*op.metadata.name) << ",\"launch_index\":" << op.launchIndex
         << ",\"summary_path\":" << Quote(directory + "summary.jsonl") << '}';
    return unit.str();
}
} // namespace npucompute
