/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/synccheck.h"
#include "diagnostic/pipeline_name.h"

#include <functional>
#include <utility>

namespace npucheck {
namespace {

const char* OperationName(uint32_t syncKind, uint32_t action)
{
    if (syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG) {
        if (action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
            return "SET_FLAG";
        }
        if (action == ACLSAN_DEVICE_SYNC_ACTION_WAIT) {
            return "WAIT_FLAG";
        }
    } else if (syncKind == ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF) {
        if (action == ACLSAN_DEVICE_SYNC_ACTION_GET) {
            return "GET_BUF";
        }
        if (action == ACLSAN_DEVICE_SYNC_ACTION_RELEASE) {
            return "RLS_BUF";
        }
    }
    return "";
}

const char* ExpectedOperation(uint32_t kind, uint32_t reason)
{
    const bool setWait = kind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG;
    if (reason == static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE)) {
        return setWait ? "SET_FLAG" : "GET_BUF";
    }
    return setWait ? "WAIT_FLAG" : "RLS_BUF";
}

NpuCheckReportExecContext ExecContext(const AclsanDeviceSyncData& data)
{
    NpuCheckReportExecContext exec{};
    exec.launchId = data.header.launchId;
    exec.instrExecId = data.header.instrExecId;
    exec.pc = data.header.pc;
    exec.phyCoreId = data.header.phyCoreId;
    exec.blockId = data.header.blockId;
    exec.blockType = data.header.blockType;
    // Sync instructions execute on PIPE_S independently of their srcPipe/dstPipe pairing arguments.
    exec.pipeId = 0;
    exec.pipeName = detail::DevicePipelineName(ACLSAN_DEVICE_PIPE_SCALAR);
    return exec;
}

} // namespace

void Synccheck::HashCombine(size_t& seed, uint64_t value) noexcept
{
    seed ^= std::hash<uint64_t>{}(value) + static_cast<size_t>(0x9e3779b9U) + (seed << 6U) + (seed >> 2U);
}

Synccheck::SyncBufOccupancyKey Synccheck::BuildSyncBufOccupancyKey(const AclsanDeviceSyncData& data)
{
    return {data.objectId, data.header.phyCoreId};
}

void Synccheck::HandleFlagEvent(
    FlagState& state, const AclsanDeviceSyncData& data, const FlagPairKey& key, Reports& reports)
{
    if (data.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
        const auto [open, inserted] = state.pending.emplace(key, data);
        if (!inserted) {
            // Duplicate flag open, for example:
            // set_flag(V, MTE2, 3) -> set_flag(V, MTE2, 3).
            ++stats_.duplicateOpens;
            reports.push_back(BuildMismatch(
                data, &open->second, static_cast<uint32_t>(NpuCheckSyncMismatchReason::DUPLICATE_OPEN), key));
        } else {
            ++stats_.pendingOpens;
        }
        return;
    }

    const auto open = state.pending.find(key);
    if (open == state.pending.end()) {
        // Close without an exact open, for example:
        // wait_flag(V, MTE2, 3), or
        // set_flag(V, MTE3, 3) -> wait_flag(V, MTE2, 3).
        ++stats_.unmatchedCloses;
        reports.push_back(
            BuildMismatch(data, nullptr, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE), key));
    } else {
        state.pending.erase(open);
        ++stats_.matchedPairs;
        --stats_.pendingOpens;
    }
}

void Synccheck::HandleSyncBufEvent(
    SyncBufState& state, const AclsanDeviceSyncData& data, const SyncBufPairKey& key, Reports& reports)
{
    if (data.action == ACLSAN_DEVICE_SYNC_ACTION_GET) {
        const SyncBufOccupancyKey occupancyKey = BuildSyncBufOccupancyKey(data);
        const auto active = state.activeSyncBufs.find(occupancyKey);
        if (active != state.activeSyncBufs.end()) {
            // Within one execution domain, duplicate sync buffer open is keyed by bufId, for example:
            // get_buf(MTE2, 2, 0) -> get_buf(MTE2, 2, 1), or
            // get_buf(MTE2, 2, 1) -> get_buf(MTE3, 2, 1).
            ++stats_.duplicateOpens;
            const auto& open = state.pending.at(active->second);
            reports.push_back(
                BuildMismatch(data, &open, static_cast<uint32_t>(NpuCheckSyncMismatchReason::DUPLICATE_OPEN), key));
        } else {
            state.pending.emplace(key, data);
            state.activeSyncBufs.emplace(occupancyKey, key);
            ++stats_.pendingOpens;
        }
        return;
    }

    const auto open = state.pending.find(key);
    if (open == state.pending.end()) {
        // Close without an exact open, for example:
        // get_buf(MTE2, 2, 0) -> rls_buf(MTE2, 2, 1).
        ++stats_.unmatchedCloses;
        reports.push_back(
            BuildMismatch(data, nullptr, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNMATCHED_CLOSE), key));
    } else {
        state.activeSyncBufs.erase(BuildSyncBufOccupancyKey(data));
        state.pending.erase(open);
        ++stats_.matchedPairs;
        --stats_.pendingOpens;
    }
}

void Synccheck::OnDeviceSync(const AclsanDeviceSyncData& data)
{
    ++stats_.syncEvents;

    const auto state = launchStates_.emplace(LaunchSyncState{data.header.launchId}).first;
    auto& launchState = *state;
    if (data.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG) {
        const FlagPairKey key = FlagPairKey::FromCbdata(data);
        HandleFlagEvent(launchState.flags, data, key, launchState.reports);
    } else {
        const SyncBufPairKey key = SyncBufPairKey::FromCbdata(data);
        HandleSyncBufEvent(launchState.syncBufs, data, key, launchState.reports);
    }
}

std::vector<Synccheck::Report> Synccheck::OnSynchronization()
{
    ++stats_.synchronizationEvents;
    Reports reports;
    const auto reportPending = [this, &reports](const auto& pending) {
        for (const auto& entry : pending) {
            ++stats_.unconsumedOpens;
            reports.push_back(BuildMismatch(
                entry.second, nullptr, static_cast<uint32_t>(NpuCheckSyncMismatchReason::UNCONSUMED_OPEN),
                entry.first));
        }
    };
    // Open left at synchronization, for example:
    // set_flag(V, MTE2, 3) -> synchronize, or
    // get_buf(MTE2, 2, 1) -> synchronize.
    for (const LaunchSyncState& state : launchStates_) {
        for (Report& report : state.reports) {
            reports.push_back(std::move(report));
        }
        reportPending(state.flags.pending);
        reportPending(state.syncBufs.pending);
        stats_.pendingOpens -= state.flags.pending.size() + state.syncBufs.pending.size();
    }
    launchStates_.clear();
    return reports;
}

SynccheckStats Synccheck::Stats() const { return stats_; }

NpuCheckSyncPoint Synccheck::ActualPoint(const AclsanDeviceSyncData& data)
{
    NpuCheckSyncPoint point{};
    point.operation = OperationName(data.syncKind, data.action);
    point.hasExecContext = true;
    point.exec = ExecContext(data);
    return point;
}

NpuCheckSyncPoint Synccheck::ExpectedPoint(const AclsanDeviceSyncData& data, uint32_t reason)
{
    NpuCheckSyncPoint point{};
    point.operation = ExpectedOperation(data.syncKind, reason);
    return point;
}

Synccheck::Report Synccheck::BuildMismatchBase(
    const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason)
{
    Report report{};
    report.common.tool = ReportTool::SYNCCHECK;
    report.common.severity = ReportSeverity::ERROR;
    report.common.pattern = NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    report.primitiveKind = trigger.syncKind == ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG ?
                               NpuCheckSyncPrimitiveKind::SET_WAIT_FLAG :
                               NpuCheckSyncPrimitiveKind::GET_RLS_BUF;
    report.detailKind = NpuCheckSyncDetailKind::PAIRING;
    report.hasRelatedPoint = true;
    report.triggerPoint = ActualPoint(trigger);
    report.common.exec = report.triggerPoint.exec;
    report.relatedPoint = related == nullptr ? ExpectedPoint(trigger, reason) : ActualPoint(*related);

    return report;
}

Synccheck::Report Synccheck::BuildMismatch(
    const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason, const FlagPairKey& key)
{
    Report report = BuildMismatchBase(trigger, related, reason);
    NpuCheckSyncPairingError detail{};
    detail.reason = static_cast<NpuCheckSyncMismatchReason>(reason);
    detail.key.pairKind = NpuCheckSyncPairKind::SET_WAIT_FLAG;
    detail.key.srcPipe = key.srcPipe;
    detail.key.dstPipe = key.dstPipe;
    detail.key.id = key.objectId;
    report.detail = detail;
    return report;
}

Synccheck::Report Synccheck::BuildMismatch(
    const AclsanDeviceSyncData& trigger, const AclsanDeviceSyncData* related, uint32_t reason,
    const SyncBufPairKey& key)
{
    Report report = BuildMismatchBase(trigger, related, reason);
    NpuCheckSyncPairingError detail{};
    detail.reason = static_cast<NpuCheckSyncMismatchReason>(reason);
    detail.key.pairKind = NpuCheckSyncPairKind::GET_RLS_BUF;
    detail.key.dstPipe = key.pipe;
    detail.key.mode = key.mode;
    detail.key.id = key.objectId;
    report.detail = detail;
    return report;
}

} // namespace npucheck
