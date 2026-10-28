/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include "checker/racecheck.h"
#include "checker/racecheck_reports.h"
#include "npu_tool_log.h"
#include <exception>
#include <limits>
#include <sstream>

namespace npucheck {
Racecheck::Racecheck(RacecheckOrdering* ordering) : ordering_(ordering) {}

const std::vector<CallbackSpec>& Racecheck::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> ids{
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END}};
    return ids;
}

void Racecheck::Log(
    const char* event, const AclsanDeviceEventHeader* header, const char* detail, bool error) const noexcept
{
    if (!error && !ASCTOOL_DEBUG_ENABLED()) {
        return;
    }
    if (error) {
        ASCTOOL_ERROR(
            "racecheck event=%s level=%s device=%u launch=%llu instr=%llu pc=0x%llx accesses=%llu committed=%llu "
            "candidates=%llu reports=%llu failures=%llu detail=%s",
            event, "ERROR", header ? header->deviceId : 0U,
            static_cast<unsigned long long>(header ? header->launchId : 0),
            static_cast<unsigned long long>(header ? header->instrExecId : 0),
            static_cast<unsigned long long>(header ? header->pc : 0), static_cast<unsigned long long>(accesses_),
            static_cast<unsigned long long>(committed_), static_cast<unsigned long long>(candidates_),
            static_cast<unsigned long long>(reportCount_), static_cast<unsigned long long>(failures_), detail);
    } else {
        ASCTOOL_DEBUG(
            "racecheck event=%s level=%s device=%u launch=%llu instr=%llu pc=0x%llx accesses=%llu committed=%llu "
            "candidates=%llu reports=%llu failures=%llu detail=%s",
            event, "DEBUG", header ? header->deviceId : 0U,
            static_cast<unsigned long long>(header ? header->launchId : 0),
            static_cast<unsigned long long>(header ? header->instrExecId : 0),
            static_cast<unsigned long long>(header ? header->pc : 0), static_cast<unsigned long long>(accesses_),
            static_cast<unsigned long long>(committed_), static_cast<unsigned long long>(candidates_),
            static_cast<unsigned long long>(reportCount_), static_cast<unsigned long long>(failures_), detail);
    }
}

// 释放失败 launch 的全部分析状态，保留标记以跳过后续访问。失败路径不申请内存。
void Racecheck::StopLaunch(
    const AclsanDeviceEventHeader& header, const char* stage, const char* reason, const char* detail) noexcept
{
    const auto found = launches_.find({header.deviceId, header.launchId});
    if (found != launches_.end()) {
        found->second.reset();
    } else {
        stopped_ = true;
    }
    ++failures_;
    ASCTOOL_ERROR(
        "racecheck event=launch.stopped level=ERROR device=%u launch=%llu instr=%llu pc=0x%llx "
        "accesses=%llu committed=%llu candidates=%llu reports=%llu failures=%llu "
        "stage=%s reason=%s detail=%.320s; launch analysis stopped, state discarded, results incomplete; %s",
        header.deviceId, static_cast<unsigned long long>(header.launchId),
        static_cast<unsigned long long>(header.instrExecId), static_cast<unsigned long long>(header.pc),
        static_cast<unsigned long long>(accesses_), static_cast<unsigned long long>(committed_),
        static_cast<unsigned long long>(candidates_), static_cast<unsigned long long>(reportCount_),
        static_cast<unsigned long long>(failures_), stage, reason, detail,
        stopped_ ? "unable to retain launch stop marker; all subsequent memory accesses will be skipped" :
                   "subsequent accesses for this launch will be skipped");
}

// 收到一条设备内存访问记录后，检查竞争并更新该 launch 的内存状态。
bool Racecheck::OnMemoryAccess(const AclsanDeviceMemoryAccessData& data, CheckerReportList& reports)
{
    ++accesses_;
    if (stopped_) {
        Log("access.skipped", &data.header, "session stopped after failure to retain launch state");
        return false;
    }
    const auto key = std::make_pair(data.header.deviceId, data.header.launchId);
    auto found = launches_.find(key);
    if (found != launches_.end() && !found->second) {
        Log("access.skipped", &data.header, "launch stopped after a previous analysis failure");
        return false;
    }
    const char* stage = "launch_state_creation";
    try {
        // 先创建停止标记，再申请状态。任何失败都不能让该 launch 被重新分析。
        if (found == launches_.end()) {
            found = launches_.try_emplace(key).first;
        }
        if (!found->second) {
            found->second = std::make_unique<LaunchState>();
        }
        auto& launch = *found->second;
        stage = "callback_validation";
        if (data.header.size != sizeof(data)) {
            throw RacecheckError(RacecheckErrorKind::UnsupportedEvent, "unsupported memory callback size");
        }
        if (!ordering_ && !warnedUnavailable_) {
            Log("ordering.unavailable", &data.header, "no synchronization ordering provider; analysis incomplete");
            warnedUnavailable_ = true;
        }
        stage = "record_storage";
        if (committed_ == std::numeric_limits<uint64_t>::max()) {
            throw RacecheckError(RacecheckErrorKind::CapacityExceeded, "committed operation counter overflow");
        }
        launch.records.push_back(data);
        const auto index = launch.records.size() - 1;
        // 本次回调持有解析结果；检查、报告和更新只引用，不重复展开，也不存入历史。
        stage = "access_preparation";
        const auto prepared = racecheck::PrepareAccess(launch.records[index]);
        // 根据已维护的内存状态，找出与当前访问可能冲突的历史访问及重叠区间。
        // 此时只是候选，还没有结合同步关系确认是否存在竞争。
        stage = "candidate_check";
        const auto candidates = launch.tracker.CheckPotentialRace(index, prepared);
        if (candidates.size() > std::numeric_limits<uint64_t>::max() - candidates_) {
            throw RacecheckError(RacecheckErrorKind::CapacityExceeded, "candidate counter overflow");
        }
        candidates_ += candidates.size();
        // 4. 对每个候选进行同步判序，conflicts 只保存确认存在竞争的访问对。
        std::vector<AccessPair> conflicts;
        stage = "ordering";
        if (ordering_) {
            for (const auto& candidate : candidates) {
                // previous 是候选中的历史访问，current 是刚保存的当前访问。
                const auto order = ordering_->Compare(launch.records[candidate.prevIndex], launch.records[index]);
                if (order == RacecheckOrder::Unordered) {
                    // 没有相应的执行顺序保证：该候选被确认为竞争。
                    conflicts.push_back(candidate);
                } else if (order != RacecheckOrder::Ordered) {
                    // 无法判序时不能当作“无竞争”，抛异常并由下方 catch 停止该 launch。
                    throw RacecheckError(RacecheckErrorKind::IncompleteTrace, "candidate ordering is unknown");
                }
                // Ordered 表示已证明 previous -> current，不加入竞争列表。
            }
        }
        // 5. 根据确认的竞争准备全部报告，不设置固定数量预算。
        stage = "report_preparation";
        auto diagnostics = MakeRacecheckDiagnostics(conflicts, launch.records, index, prepared);
        if (diagnostics.size() > std::numeric_limits<uint64_t>::max() - reportCount_) {
            throw RacecheckError(RacecheckErrorKind::CapacityExceeded, "race report counter overflow");
        }
        // 6. 在临时列表中准备“已有报告 + 本次新增报告”，暂不修改调用方的 reports。
        // 提前完成内存分配，避免内存状态更新成功后，追加报告却因分配失败而中断。
        auto pending = reports;
        if (diagnostics.size() > pending.max_size() - pending.size()) {
            throw RacecheckError(RacecheckErrorKind::CapacityExceeded, "report list maximum capacity exceeded");
        }
        pending.reserve(pending.size() + diagnostics.size());
        for (size_t i = 0; i < diagnostics.size(); ++i) {
            const auto& evidence = conflicts[i];
            auto diagnostic = std::move(diagnostics[i]);
            // 用已提交报告数加上本次已准备的报告数，生成从 1 开始的连续编号。
            diagnostic.reportId = reportCount_ + (pending.size() - reports.size()) + 1;
            const auto& previousHeader = launch.records[evidence.prevIndex].header;
            // 此处仅记录已准备的竞争证据，报告尚未发布。
            ASCTOOL_DEBUG(
                "racecheck event=report.evidence level=DEBUG device=%u launch=%llu instr=%llu pc=0x%llx "
                "report_id=%llu prev_index=%llu cur_index=%llu previous_pc=0x%llx "
                "previous_core=%u previous_pipe=%u overlap=[0x%llx,0x%llx) status=prepared",
                data.header.deviceId, static_cast<unsigned long long>(data.header.launchId),
                static_cast<unsigned long long>(data.header.instrExecId),
                static_cast<unsigned long long>(data.header.pc), static_cast<unsigned long long>(diagnostic.reportId),
                static_cast<unsigned long long>(evidence.prevIndex), static_cast<unsigned long long>(evidence.curIndex),
                static_cast<unsigned long long>(previousHeader.pc), previousHeader.phyCoreId, previousHeader.pipeline,
                static_cast<unsigned long long>(evidence.begin), static_cast<unsigned long long>(evidence.end));
            pending.emplace_back(std::move(diagnostic));
        }
        if (!diagnostics.empty()) {
            Log("report.prepared", &data.header, "confirmed evidence prepared; not yet published");
        }
        // 7. 将当前访问和确认的冲突更新到内存状态，供后续访问检测使用。
        // 更新成功后再交换报告列表，正式发布本次报告并累计成功处理数。
        stage = "memory_state_update";
        launch.tracker.UpdateMemState(index, conflicts, prepared);
        reports.swap(pending);
        reportCount_ += diagnostics.size();
        ++committed_;
        if (!diagnostics.empty()) {
            Log("report.commit", &data.header, "state and checker reports committed");
        }
        // true 表示本次访问处理成功；是否发现竞争，要看新增报告，不能看返回值。
        return true;
    } catch (const RacecheckError& error) {
        const char* reason = error.Kind() == RacecheckErrorKind::MemoryAllocationFailed ?
                                 "host_memory_allocation_failed" :
                             error.Kind() == RacecheckErrorKind::CapacityExceeded ? "capacity_or_count_overflow" :
                                                                                    "invalid_or_incomplete_analysis";
        if (error.Kind() == RacecheckErrorKind::MemoryAllocationFailed) {
            StopLaunch(
                data.header, stage, reason,
                "insufficient host memory for racecheck; check available host memory and process/container memory "
                "limits");
        } else {
            StopLaunch(data.header, stage, reason, error.what());
        }
    } catch (const std::bad_alloc&) {
        StopLaunch(
            data.header, stage, "host_memory_allocation_failed",
            "insufficient host memory for racecheck; check available host memory and process/container memory limits");
    } catch (const std::length_error&) {
        StopLaunch(data.header, stage, "container_capacity_exceeded", "container maximum capacity exceeded");
    } catch (const std::exception& error) {
        StopLaunch(data.header, stage, "analysis_exception", error.what());
    } catch (...) {
        StopLaunch(data.header, stage, "unknown_exception", "unknown failure during launch analysis");
    }
    return false;
}

bool Racecheck::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    if (!data) {
        ++failures_;
        Log("callback.failed", nullptr, "null callback data; analysis incomplete", true);
        return false;
    }
    try {
        if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
            return OnMemoryAccess(*static_cast<const AclsanDeviceMemoryAccessData*>(data), reports);
        }
        if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_SYNC) {
            if (ordering_) {
                ordering_->OnDeviceSync(*static_cast<const AclsanDeviceSyncData*>(data));
            }
        } else if (domain == ACLSAN_CB_DOMAIN_LAUNCH && cbid == ACLSAN_CBID_LAUNCH_KERNEL) {
            if (static_cast<const AclsanLaunchData*>(data)->common.result != 0) {
                ++failures_;
                Log("launch.failed", nullptr, "device launch failed; analysis incomplete", true);
            }
        } else if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE && cbid == ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END) {
            ++syncs_;
            if (static_cast<const AclsanSynchronizeData*>(data)->common.result != 0) {
                ++failures_;
                Log("synchronization.failed", nullptr, "stream synchronization failed; analysis incomplete", true);
            }
        }
        return true;
    } catch (const std::exception& error) {
        ++failures_;
        const auto* header =
            domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION ? static_cast<const AclsanDeviceEventHeader*>(data) : nullptr;
        Log("callback.failed", header, error.what(), true);
        return false;
    } catch (...) {
        ++failures_;
        const auto* header =
            domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION ? static_cast<const AclsanDeviceEventHeader*>(data) : nullptr;
        Log("callback.failed", header, "unknown failure; analysis incomplete", true);
        return false;
    }
}

std::string Racecheck::Summary() const
{
    std::ostringstream output;
    output << "tool=racecheck device_operations=" << accesses_ << " committed_operations=" << committed_
           << " candidates=" << candidates_ << " races=" << reportCount_ << " errors=0 warnings=" << reportCount_
           << " failures=" << failures_ << " synchronizations=" << syncs_
           << " ordering_available=" << (ordering_ ? "true" : "false");
    return output.str();
}

bool Racecheck::HasErrors() const { return reportCount_ != 0; }
bool Racecheck::AnalysisComplete() const { return ordering_ && failures_ == 0 && ordering_->AnalysisComplete(); }
} // namespace npucheck
