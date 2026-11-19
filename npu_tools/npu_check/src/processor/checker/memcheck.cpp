/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/memcheck.h"
#include "checker/memory_access_layout.h"
#include "diagnostic/pipeline_name.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace npucheck {
namespace {

using npucheck::NpuCheckMemcheckReport;
using npucheck::NpuCheckReportAccessMode;
using npucheck::NpuCheckReportAllocation;
using npucheck::NpuCheckReportDistanceKind;
using npucheck::NpuCheckReportExecContext;
using npucheck::NpuCheckReportMemorySpace;
using npucheck::NpuCheckReportPattern;
using npucheck::ReportSeverity;
using npucheck::ReportTool;

constexpr uint32_t ALLOCATION_STATE_LIVE = 1;
constexpr uint32_t ALLOCATION_STATE_FREED = 2;
// Capacities are shared with initialization and range classification.
uint64_t CubeCapacity(uint32_t space) { return npucheck::InitStateTableIndex::LocalCapacity(space); }

NpuCheckReportMemorySpace CubeReportSpace(uint32_t space)
{
    switch (space) {
        case ACLSAN_DEVICE_MEMORY_SPACE_UB:
            return NpuCheckReportMemorySpace::UB;
        case ACLSAN_DEVICE_MEMORY_SPACE_L1:
            return NpuCheckReportMemorySpace::L1;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0A:
            return NpuCheckReportMemorySpace::L0_A;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0B:
            return NpuCheckReportMemorySpace::L0_B;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0C:
            return NpuCheckReportMemorySpace::L0_C;
        default:
            return NpuCheckReportMemorySpace::UNKNOWN;
    }
}

uint64_t TimestampNs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::optional<uint64_t> RangeEnd(uint64_t base, uint64_t bytes)
{
    if (base > std::numeric_limits<uint64_t>::max() - bytes) {
        return std::nullopt;
    }
    return base + bytes;
}

NpuCheckReportAllocation ToReportAllocation(const std::optional<Allocation>& allocation)
{
    NpuCheckReportAllocation result{};
    if (!allocation.has_value()) {
        return result;
    }
    result.allocId = allocation->resourceId;
    result.base = allocation->base;
    result.bytes = allocation->bytes;
    result.allocSerialNo = allocation->allocSequence;
    result.freeSerialNo = allocation->freeSequence;
    result.memorySpace = NpuCheckReportMemorySpace::GM;
    result.deviceId = allocation->deviceId;
    result.state = allocation->freeSequence == 0 ? ALLOCATION_STATE_LIVE : ALLOCATION_STATE_FREED;
    return result;
}

NpuCheckReportExecContext ToExecContext(const AclsanDeviceMemoryAccessData& data)
{
    NpuCheckReportExecContext exec{};
    exec.launchId = data.header.launchId;
    exec.instrExecId = data.header.instrExecId;
    exec.serialNo = data.header.serialNo;
    exec.pc = data.header.pc;
    exec.deviceId = data.header.deviceId;
    exec.phyCoreId = data.header.executor.phyCoreId;
    exec.blockId = data.header.executor.blockId;
    exec.blockType = data.header.executor.blockType;
    exec.pipeId = data.header.pipeline;
    exec.siteId = data.header.siteId;
    exec.pipeName = detail::DevicePipelineName(data.header.pipeline);
    return exec;
}

int64_t ClampDistance(uint64_t distance)
{
    constexpr uint64_t MAXIMUM = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return static_cast<int64_t>(distance > MAXIMUM ? MAXIMUM : distance);
}

void SetDistance(
    uint64_t address, uint64_t bytes, const std::optional<Allocation>& allocation, NpuCheckMemcheckReport& report)
{
    if (!allocation) {
        return;
    }
    const auto allocationEnd = RangeEnd(allocation->base, allocation->bytes);
    const auto accessEnd = RangeEnd(address, bytes);
    if (!allocationEnd) {
        return;
    }
    if (address < allocation->base) {
        report.distanceKind = NpuCheckReportDistanceKind::BEFORE;
        report.distanceBytes = ClampDistance(allocation->base - address);
        return;
    }
    if (!accessEnd) {
        report.distanceKind = NpuCheckReportDistanceKind::AFTER;
        report.distanceBytes = std::numeric_limits<int64_t>::max();
        return;
    }
    if (address >= *allocationEnd) {
        report.distanceKind = NpuCheckReportDistanceKind::AFTER;
        report.distanceBytes = ClampDistance(address - *allocationEnd);
        return;
    }
    if (*accessEnd > *allocationEnd) {
        report.distanceKind = NpuCheckReportDistanceKind::AFTER;
        report.distanceBytes = ClampDistance(*accessEnd - *allocationEnd);
        return;
    }
    report.distanceKind = NpuCheckReportDistanceKind::INSIDE;
}

} // namespace

void Memcheck::OnAllocation(const AclsanResourceData& data)
{
    if (data.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE || data.common.result != 0) {
        return;
    }
    const auto status = analysis_->Managed() ?
                            analysis_->ResourceUpdate() :
                            memoryState_->Register(
                                {data.deviceId, 0}, data.resourceId, reinterpret_cast<uint64_t>(data.ptr), data.bytes);
    if (status == AllocationUpdateStatus::OK) {
        ++stats_.allocations;
    }
}

void Memcheck::OnFree(const AclsanResourceData& data)
{
    if (data.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE || data.common.result != 0) {
        return;
    }
    const auto status =
        analysis_->Managed() ?
            analysis_->ResourceUpdate() :
            memoryState_->Release({data.deviceId, 0}, data.resourceId, reinterpret_cast<uint64_t>(data.ptr));
    if (status == AllocationUpdateStatus::OK) {
        ++stats_.frees;
    }
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckAccess(
    const AclsanDeviceMemoryAccessData& data, NpuCheckReportAccessMode accessMode, uint64_t address, uint64_t bytes,
    uint64_t groupId)
{
    const bool preload =
        (data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED)) != 0 && bytes == 0;
    const uint64_t capacity = CubeCapacity(data.memorySpace);
    RangeResult range{};
    if (capacity != 0) {
        range.status =
            address <= capacity && bytes <= capacity - address ? RangeStatus::VALID : RangeStatus::OUT_OF_BOUNDS;
        range.allocation = Allocation{0, 0, capacity, data.header.deviceId, 0, 0};
    } else {
        range = analysis_->ClassifyAccessRange(data, address, preload ? 1 : bytes);
    }
    if (range.status == RangeStatus::VALID) {
        return {};
    }
    if (analysis_->Managed() && range.status == RangeStatus::AMBIGUOUS) {
        ++stats_.droppedDeviceOperations;
        return {};
    }

    NpuCheckMemcheckReport report{};
    report.common.reportId = nextReportId_++;
    report.common.groupId = groupId;
    report.common.timestampNs = TimestampNs();
    report.common.tool = ReportTool::MEMCHECK;
    report.common.severity = ReportSeverity::ERROR;
    report.common.flags = npucheck::kNpuCheckReportCommonHasExecContext;
    report.common.exec = ToExecContext(data);
    report.common.pattern = range.status == RangeStatus::USE_AFTER_FREE ?
                                NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE :
                                NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS;

    report.access.memorySpace = capacity == 0 ? NpuCheckReportMemorySpace::GM : CubeReportSpace(data.memorySpace);
    report.access.accessMode = accessMode;
    report.access.accessBytes = bytes > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() :
                                                                               static_cast<uint32_t>(bytes);
    report.access.requiredAlign = data.alignSize;
    report.access.address = address;
    report.access.rangeBegin = address;
    report.access.rangeEnd = RangeEnd(address, bytes).value_or(std::numeric_limits<uint64_t>::max());

    report.allocation = ToReportAllocation(range.allocation);
    const auto nearest =
        range.allocation ? range.allocation :
                           memoryState_->Allocations({data.header.deviceId, 0}).Nearest(data.header.deviceId, address);
    report.nearestAllocation = ToReportAllocation(nearest);
    if (capacity != 0) {
        report.allocation.memorySpace = report.access.memorySpace;
        report.nearestAllocation.memorySpace = report.access.memorySpace;
    }
    SetDistance(address, bytes, nearest, report);
    return {std::move(report)};
}

std::vector<NpuCheckMemcheckReport> Memcheck::QueueDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& data)
{
    // ATOMIC_CONFIG_UNKNOWN does not affect bounds; ESTIMATED makes the range uncertain.
    if ((data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED)) != 0) {
        ++stats_.inexactDeviceOperations;
        return {};
    }
    if (data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM && CubeCapacity(data.memorySpace) == 0) {
        return {};
    }
    ++stats_.deviceOperations;
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB) {
        return {};
    }
    if (!analysis_->Managed()) {
        pendingDeviceAccesses_.push_back(data);
        stats_.pendingDeviceOperations = pendingDeviceAccesses_.size();
        return {};
    }
    observedLaunches_.insert(data.header.launchId);
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
        ++stats_.pendingDeviceOperations;
        return {};
    }
    auto reports = CheckObservedAccess(data);
    Count(reports);
    return reports;
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckDeviceMemoryAccess(
    const AclsanDeviceMemoryAccessData& data, uint64_t groupId)
{
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB) {
        return {};
    }
    const auto markIncomplete = [this]() {
        ++stats_.droppedDeviceOperations;
        return std::vector<NpuCheckMemcheckReport>{};
    };

    NpuCheckReportAccessMode accessMode;
    switch (data.accessMode) {
        case ACLSAN_DEVICE_MEMORY_ACCESS_READ:
            accessMode = NpuCheckReportAccessMode::READ;
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_WRITE:
            accessMode = NpuCheckReportAccessMode::WRITE;
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD:
            accessMode = NpuCheckReportAccessMode::ATOMIC_WRITE_ADD;
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN:
            accessMode = NpuCheckReportAccessMode::ATOMIC_WRITE_MIN;
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX:
            accessMode = NpuCheckReportAccessMode::ATOMIC_WRITE_MAX;
            break;
        default:
            return markIncomplete();
    }

    if ((data.header.flags & kDeviceEventFlagPredicated) != 0 && data.predicateMask0 == 0 && data.predicateMask1 == 0) {
        return {};
    }

    std::vector<NpuCheckMemcheckReport> reports;
    auto appendAccess = [this, &data, accessMode, &reports, groupId](uint64_t address, uint64_t bytes) {
        auto found = CheckAccess(data, accessMode, address, bytes, groupId);
        reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    };

    switch (data.layoutKind) {
        case ACLSAN_MEM_LAYOUT_SCALAR:
            if (data.layout.scalar.bytes == 0) {
                return markIncomplete();
            }
            appendAccess(data.address, data.layout.scalar.bytes);
            break;
        case ACLSAN_MEM_LAYOUT_RANGE:
            if (data.layout.range.bytes == 0) {
                return markIncomplete();
            }
            appendAccess(data.address, data.layout.range.bytes);
            break;
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT: {
            constexpr uint32_t BLOCK_REPEAT_RANK = 2;
            const auto& layout = data.layout.blockRepeat;
            memory_layout::AffineLayout affine{};
            affine.base = data.address;
            affine.rank = BLOCK_REPEAT_RANK;
            affine.elementBytes = layout.blockSize;
            affine.dimensions[0] = layout.blockNum;
            affine.dimensions[1] = layout.repeatTimes;
            affine.strides[0] = layout.blockStride;
            affine.strides[1] = layout.repeatStride;
            if (!memory_layout::VisitAffineSegments(affine, appendAccess)) {
                return markIncomplete();
            }
            break;
        }
        case ACLSAN_MEM_LAYOUT_ND_AFFINE: {
            const auto& layout = data.layout.ndAffine;
            memory_layout::AffineLayout affine{};
            affine.base = data.address;
            affine.rank = layout.rank;
            affine.elementBytes = layout.elementBytes;
            for (uint32_t dimension = 0; dimension < memory_layout::kMaxAffineRank; ++dimension) {
                affine.dimensions[dimension] = layout.dims[dimension];
                affine.strides[dimension] = layout.strides[dimension];
            }
            if (!memory_layout::VisitAffineSegments(affine, appendAccess)) {
                return markIncomplete();
            }
            break;
        }
        default:
            return markIncomplete();
    }

    return reports;
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckObservedAccess(const AclsanDeviceMemoryAccessData& access)
{
    const InstructionIdentity identity{
        access.header.deviceId, access.header.executor.blockType, access.header.executor.blockId,
        access.header.instrExecId};
    auto launch = instructionGroups_.find(access.header.launchId);
    auto groupId = nextGroupId_;
    if (launch != instructionGroups_.end()) {
        const auto group = launch->second.find(identity);
        if (group != launch->second.end()) {
            groupId = group->second;
        }
    }
    auto reports = CheckDeviceMemoryAccess(access, groupId);
    if (!reports.empty() && groupId == nextGroupId_) {
        instructionGroups_[access.header.launchId].emplace(identity, nextGroupId_++);
    }
    return reports;
}

void Memcheck::CollectLaunch(uint64_t launchId, std::vector<NpuCheckMemcheckReport>& reports)
{
    const auto& shared = analysis_->Get(launchId);
    for (const auto& access : shared.memory) {
        if (access.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM ||
            (access.header.flags & ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) != 0) {
            continue;
        }
        auto found = CheckObservedAccess(access);
        reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        --stats_.pendingDeviceOperations;
    }
}

std::vector<NpuCheckMemcheckReport> Memcheck::CollectCompletedLaunch(uint64_t launchId)
{
    std::vector<NpuCheckMemcheckReport> reports;
    if (analysis_->Managed()) {
        if (observedLaunches_.erase(launchId) == 0) {
            unfinishedLaunches_ = true;
            return reports;
        }
        CollectLaunch(launchId, reports);
    } else {
        const auto end = std::remove_if(
            pendingDeviceAccesses_.begin(), pendingDeviceAccesses_.end(),
            [this, launchId, &reports](const auto& access) {
                if (access.header.launchId != launchId)
                    return false;
                auto found = CheckObservedAccess(access);
                reports.insert(
                    reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
                --stats_.pendingDeviceOperations;
                return true;
            });
        pendingDeviceAccesses_.erase(end, pendingDeviceAccesses_.end());
    }
    instructionGroups_.erase(launchId);
    Count(reports);
    return reports;
}

std::vector<NpuCheckMemcheckReport> Memcheck::OnSynchronization()
{
    ++stats_.synchronizationEvents;
    std::vector<NpuCheckMemcheckReport> reports;
    if (analysis_->Managed()) {
        for (const auto id : analysis_->Closing()) {
            if (observedLaunches_.erase(id) != 0) {
                CollectLaunch(id, reports);
                instructionGroups_.erase(id);
            }
        }
    } else {
        for (const auto& access : pendingDeviceAccesses_) {
            auto found = CheckObservedAccess(access);
            reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        }
        pendingDeviceAccesses_.clear();
        instructionGroups_.clear();
        stats_.pendingDeviceOperations = 0;
    }
    Count(reports);
    return reports;
}

void Memcheck::Finalize(CheckerReportList& reports)
{
    unfinishedLaunches_ = unfinishedLaunches_ || !observedLaunches_.empty() || !pendingDeviceAccesses_.empty();
    std::vector<NpuCheckMemcheckReport> completed;
    for (const auto id : observedLaunches_) {
        CollectLaunch(id, completed);
    }
    for (const auto& access : pendingDeviceAccesses_) {
        auto found = CheckObservedAccess(access);
        completed.insert(completed.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    observedLaunches_.clear();
    pendingDeviceAccesses_.clear();
    instructionGroups_.clear();
    stats_.pendingDeviceOperations = 0;
    Count(completed);
    AppendCheckerReports(std::move(completed), reports);
}

MemcheckStats Memcheck::Stats() const { return stats_; }

void Memcheck::Count(const std::vector<NpuCheckMemcheckReport>& reports)
{
    for (const auto& report : reports) {
        if (report.common.severity == ReportSeverity::ERROR) {
            ++stats_.errors;
        } else if (report.common.severity == ReportSeverity::WARNING) {
            ++stats_.warnings;
        }
    }
}

} // namespace npucheck
