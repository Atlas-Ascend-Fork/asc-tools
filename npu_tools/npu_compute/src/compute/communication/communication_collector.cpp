/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "communication_collector.h"
#include "communication/record_json.h"
#include "communication/artifact_schema.h"
namespace npucompute {

void CommunicationCollector::PublishFragment(const std::string& fragment, uint64_t records)
{
    if (records == 0) {
        return;
    }
    if (sequence_ >= communication::kMaxFragments) {
        throw std::runtime_error("Communication fragment limit exceeded");
    }
    const auto name = "records-" + std::to_string(sequence_) + ".jsonl";
    WriteArtifact(sink_, std::string(communication::kArtifactPrefix) + name, fragment, records);
    fragments_.emplace_back(name, records);
    ++sequence_;
    count_ += records;
}

void CommunicationCollector::ConsumeRecords(const std::vector<aclpti::communication::Record>& records)
{
    std::string fragment;
    uint64_t count = 0;
    for (const auto& record : records) {
        const auto row = communication::Json(communication::RecordTree(record));
        if (row.size() > communication::kMaxFragmentBytes) {
            throw std::runtime_error("Communication record limit exceeded");
        }
        if (fragment.size() > communication::kMaxFragmentBytes - row.size()) {
            PublishFragment(fragment, count);
            fragment.clear();
            count = 0;
        }
        fragment += row;
        ++count;
    }
    PublishFragment(fragment, count);
}

aclptiResult CommunicationCollector::Consume(const CommunicationBatch& result)
{
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (finished_) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    try {
        if (result.status != ACLPTI_SUCCESS) {
            status_ = result.status;
        }
        errors_ += result.errors;
        dropped_ += result.dropped;
        clocks_.insert(clocks_.end(), result.clocks.begin(), result.clocks.end());
        if (!result.diagnostic.empty()) {
            diagnostic_ += (diagnostic_.empty() ? "" : "; ") + result.diagnostic;
        }
        if (failed_) {
            return ACLPTI_ERROR_TRACE_WRITE;
        }
        ConsumeRecords(result.records);
        return ACLPTI_SUCCESS;
    } catch (const std::exception& exception) {
        failed_ = true;
        error_ = exception.what();
        return ACLPTI_ERROR_TRACE_WRITE;
    } catch (...) {
        failed_ = true;
        return ACLPTI_ERROR_TRACE_WRITE;
    }
}

std::string CommunicationCollector::Manifest() const
{
    using namespace communication;
    Tree manifest, files, clocks;
    manifest.put("schema", kManifestSchemaVersion);
    manifest.put("complete", complete_);
    manifest.put("dropped", dropped_);
    manifest.put("errors", errors_);
    manifest.put("diagnostic", diagnostic_ + (error_.empty() ? "" : "; " + error_));
    manifest.put("records", count_);
    for (const auto& [name, count] : fragments_) {
        Tree item;
        item.put("file", name);
        item.put("records", count);
        files.push_back({"", item});
    }
    for (const auto& device : clocks_) {
        Tree item;
        item.put("deviceId", device.deviceId);
        item.add_child("aicpu", ClockTree(device.aicpu));
        item.add_child("task", ClockTree(device.task));
        item.add_child("ccu", ClockTree(device.ccu));
        clocks.push_back({"", item});
    }
    manifest.add_child("files", files);
    manifest.add_child("clocks", clocks);
    return Json(manifest);
}

aclptiResult CommunicationCollector::Finish()
{
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (finished_) {
        return status_;
    }
    finished_ = true;
    complete_ = status_ == ACLPTI_SUCCESS && errors_ == 0 && !failed_ && (count_ == 0 || !clocks_.empty());
    status_ = complete_ ? ACLPTI_SUCCESS : ACLPTI_ERROR_TRACE_INCOMPLETE;
    try {
        WriteArtifact(sink_, std::string(communication::kArtifactPrefix) + "manifest.json", Manifest(), 1);
    } catch (...) {
        failed_ = true;
        complete_ = false;
        status_ = ACLPTI_ERROR_TRACE_WRITE;
    }
    return status_;
}
} // namespace npucompute
