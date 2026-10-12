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

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
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

constexpr uint32_t kAllocationStateLive = 1;
constexpr uint32_t kAllocationStateFreed = 2;
constexpr uint32_t kMaxAffineRank = 5;
constexpr uint64_t kMaxLayoutSegments = 1u << 20u;

// Cube decoding currently supports only dav3510 (Ascend 950). Capacities match
// CANN platform_config and mssanitizer CHIP_INFO_MAP, not sample tensor sizes.
uint64_t CubeCapacity(uint32_t space)
{
    switch (space) {
        case ACLSAN_DEVICE_MEMORY_SPACE_L1:
            return 512 * 1024;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0A:
        case ACLSAN_DEVICE_MEMORY_SPACE_L0B:
            return 64 * 1024;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0C:
            return 256 * 1024;
        default:
            return 0;
    }
}

NpuCheckReportMemorySpace CubeReportSpace(uint32_t space)
{
    switch (space) {
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

struct AffineLayout {
    uint64_t base = 0;
    uint32_t rank = 0;
    uint64_t elementBytes = 0;
    std::array<uint64_t, kMaxAffineRank> dimensions{};
    std::array<int64_t, kMaxAffineRank> strides{};
};

struct AffineAxis {
    uint64_t count = 0;
    int64_t stride = 0;
    uint64_t absoluteStride = 0;
};

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

uint64_t AbsoluteStride(int64_t stride)
{
    return stride < 0 ? static_cast<uint64_t>(-(stride + 1)) + 1 : static_cast<uint64_t>(stride);
}

bool AddOffset(__int128 left, __int128 right, __int128& result)
{
    return !__builtin_add_overflow(left, right, &result);
}

bool AxisExtent(const AffineAxis& axis, __int128& extent)
{
    return !__builtin_mul_overflow(static_cast<__int128>(axis.count - 1), static_cast<__int128>(axis.stride), &extent);
}

std::optional<uint64_t> AddressWithOffset(uint64_t base, __int128 offset)
{
    if (offset < -static_cast<__int128>(base) ||
        offset > static_cast<__int128>(std::numeric_limits<uint64_t>::max() - base)) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(static_cast<__int128>(base) + offset);
}

template <typename Visitor>
bool VisitAffineSegments(const AffineLayout& layout, Visitor&& visitor)
{
    if (layout.rank == 0 || layout.rank > kMaxAffineRank || layout.elementBytes == 0) {
        return false;
    }

    std::array<AffineAxis, kMaxAffineRank> axes{};
    uint32_t axisCount = 0;
    for (uint32_t dimension = 0; dimension < layout.rank; ++dimension) {
        const uint64_t count = layout.dimensions[dimension];
        if (count == 0) {
            return false;
        }
        if (count > 1) {
            const int64_t stride = layout.strides[dimension];
            axes[axisCount++] = {count, stride, AbsoluteStride(stride)};
        }
    }
    for (uint32_t index = 1; index < axisCount; ++index) {
        const AffineAxis axis = axes[index];
        uint32_t insertion = index;
        while (insertion > 0 && axes[insertion - 1].absoluteStride > axis.absoluteStride) {
            axes[insertion] = axes[insertion - 1];
            --insertion;
        }
        axes[insertion] = axis;
    }

    __int128 contiguousBegin = 0;
    uint64_t contiguousBytes = layout.elementBytes;
    uint32_t sparseBegin = 0;
    for (; sparseBegin < axisCount; ++sparseBegin) {
        const AffineAxis& axis = axes[sparseBegin];
        if (axis.absoluteStride > contiguousBytes) {
            break;
        }

        __int128 extent = 0;
        if (!AxisExtent(axis, extent)) {
            return false;
        }
        const unsigned __int128 absoluteExtent =
            extent < 0 ? static_cast<unsigned __int128>(-(extent + 1)) + 1 : static_cast<unsigned __int128>(extent);
        const unsigned __int128 mergedBytes = static_cast<unsigned __int128>(contiguousBytes) + absoluteExtent;
        if (mergedBytes > std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        contiguousBytes = static_cast<uint64_t>(mergedBytes);
        if (extent < 0 && !AddOffset(contiguousBegin, extent, contiguousBegin)) {
            return false;
        }
    }

    uint64_t segmentCount = 1;
    __int128 minimumOffset = contiguousBegin;
    __int128 maximumOffset = contiguousBegin;
    for (uint32_t axisIndex = sparseBegin; axisIndex < axisCount; ++axisIndex) {
        const AffineAxis& axis = axes[axisIndex];
        if (axis.count > kMaxLayoutSegments / segmentCount) {
            return false;
        }
        segmentCount *= axis.count;

        __int128 extent = 0;
        if (!AxisExtent(axis, extent)) {
            return false;
        }
        if (extent < 0) {
            if (!AddOffset(minimumOffset, extent, minimumOffset)) {
                return false;
            }
        } else if (!AddOffset(maximumOffset, extent, maximumOffset)) {
            return false;
        }
    }
    if (!AddressWithOffset(layout.base, minimumOffset) || !AddressWithOffset(layout.base, maximumOffset)) {
        return false;
    }

    for (uint64_t linear = 0; linear < segmentCount; ++linear) {
        uint64_t index = linear;
        __int128 offset = contiguousBegin;
        for (uint32_t axisIndex = sparseBegin; axisIndex < axisCount; ++axisIndex) {
            const AffineAxis& axis = axes[axisIndex];
            const uint64_t coordinate = index % axis.count;
            index /= axis.count;
            __int128 term = 0;
            if (__builtin_mul_overflow(static_cast<__int128>(coordinate), static_cast<__int128>(axis.stride), &term) ||
                !AddOffset(offset, term, offset)) {
                return false;
            }
        }
        const auto address = AddressWithOffset(layout.base, offset);
        if (!address) {
            return false;
        }
        visitor(*address, contiguousBytes);
    }
    return true;
}

const char* PipelineName(uint32_t pipeline)
{
    switch (pipeline) {
        case ACLSAN_DEVICE_PIPE_SCALAR:
            return "SCALAR";
        case ACLSAN_DEVICE_PIPE_VECTOR:
            return "VECTOR";
        case ACLSAN_DEVICE_PIPE_MATRIX:
            return "MATRIX";
        case ACLSAN_DEVICE_PIPE_MTE1:
            return "MTE1";
        case ACLSAN_DEVICE_PIPE_MTE2:
            return "MTE2";
        case ACLSAN_DEVICE_PIPE_MTE3:
            return "MTE3";
        case ACLSAN_DEVICE_PIPE_ALL:
            return "ALL";
        case ACLSAN_DEVICE_PIPE_FIXPIPE:
            return "FIXPIPE";
        default:
            return "UNKNOWN";
    }
}

NpuCheckReportAllocation ToReportAllocation(const std::optional<Allocation>& allocation)
{
    NpuCheckReportAllocation result{};
    if (!allocation) {
        return result;
    }
    result.allocId = allocation->resourceId;
    result.base = allocation->base;
    result.bytes = allocation->bytes;
    result.allocSerialNo = allocation->allocSequence;
    result.freeSerialNo = allocation->freeSequence;
    result.memorySpace = NpuCheckReportMemorySpace::GM;
    result.deviceId = allocation->deviceId;
    result.state = allocation->freeSequence == 0 ? kAllocationStateLive : kAllocationStateFreed;
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
    exec.phyCoreId = data.header.phyCoreId;
    exec.blockId = data.header.blockId;
    exec.blockType = data.header.blockType;
    exec.pipeId = data.header.pipeline;
    exec.siteId = data.header.siteId;
    exec.pipeName = PipelineName(data.header.pipeline);
    return exec;
}

int64_t ClampDistance(uint64_t distance)
{
    constexpr uint64_t kMaximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return static_cast<int64_t>(distance > kMaximum ? kMaximum : distance);
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
    const auto status =
        allocations_.Register(data.resourceId, reinterpret_cast<uint64_t>(data.ptr), data.bytes, data.deviceId);
    if (status == AllocationUpdateStatus::OK) {
        ++stats_.allocations;
    }
}

void Memcheck::OnFree(const AclsanResourceData& data)
{
    if (data.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE || data.common.result != 0) {
        return;
    }
    const auto status = allocations_.Release(data.resourceId, reinterpret_cast<uint64_t>(data.ptr), data.deviceId);
    if (status == AllocationUpdateStatus::OK) {
        ++stats_.frees;
    }
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckAccess(
    const AclsanDeviceMemoryAccessData& data, NpuCheckReportAccessMode accessMode, uint64_t address, uint64_t bytes,
    uint64_t groupId)
{
    const bool preload = (data.header.flags & ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) != 0 && bytes == 0;
    const uint64_t capacity = CubeCapacity(data.memorySpace);
    RangeResult range{};
    if (capacity != 0) {
        range.status =
            address <= capacity && bytes <= capacity - address ? RangeStatus::VALID : RangeStatus::OUT_OF_BOUNDS;
        range.allocation = Allocation{0, 0, capacity, data.header.deviceId, 0, 0};
    } else {
        range = allocations_.Classify(data.header.deviceId, address, preload ? 1 : bytes);
    }
    if (range.status == RangeStatus::VALID) {
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
    const auto nearest = range.allocation ? range.allocation : allocations_.Nearest(data.header.deviceId, address);
    report.nearestAllocation = ToReportAllocation(nearest);
    if (capacity != 0) {
        report.allocation.memorySpace = report.access.memorySpace;
        report.nearestAllocation.memorySpace = report.access.memorySpace;
    }
    SetDistance(address, bytes, nearest, report);
    return {std::move(report)};
}

void Memcheck::QueueDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& data)
{
    if ((data.header.flags & ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) != 0) {
        ++stats_.inexactDeviceOperations;
        return;
    }
    if (data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM && CubeCapacity(data.memorySpace) == 0) {
        return;
    }
    ++stats_.deviceOperations;
    if (pendingDeviceAccesses_.size() >= maxPendingDeviceOperations) {
        ++stats_.droppedDeviceOperations;
        return;
    }
    pendingDeviceAccesses_.push_back(data);
    stats_.pendingDeviceOperations = pendingDeviceAccesses_.size();
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckDeviceMemoryAccess(
    const AclsanDeviceMemoryAccessData& data, uint64_t groupId)
{
    const auto markIncomplete = [this]() {
        ++stats_.droppedDeviceOperations;
        return std::vector<NpuCheckMemcheckReport>{};
    };

    std::vector<NpuCheckReportAccessMode> accessModes;
    switch (data.accessMode) {
        case ACLSAN_DEVICE_MEMORY_ACCESS_READ:
            accessModes.push_back(NpuCheckReportAccessMode::READ);
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_WRITE:
            accessModes.push_back(NpuCheckReportAccessMode::WRITE);
            break;
        case ACLSAN_DEVICE_MEMORY_ACCESS_READ_WRITE:
            accessModes.push_back(NpuCheckReportAccessMode::READ);
            accessModes.push_back(NpuCheckReportAccessMode::WRITE);
            break;
        default:
            return markIncomplete();
    }

    if ((data.header.flags & kDeviceEventFlagPredicated) != 0 && data.predicateMask0 == 0 && data.predicateMask1 == 0) {
        return {};
    }

    std::vector<NpuCheckMemcheckReport> reports;
    auto appendAccess = [this, &data, &accessModes, &reports, groupId](uint64_t address, uint64_t bytes) {
        for (const NpuCheckReportAccessMode accessMode : accessModes) {
            auto found = CheckAccess(data, accessMode, address, bytes, groupId);
            reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        }
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
            const auto& layout = data.layout.blockRepeat;
            AffineLayout affine{};
            affine.base = data.address;
            affine.rank = 2;
            affine.elementBytes = layout.blockSize;
            affine.dimensions[0] = layout.blockNum;
            affine.dimensions[1] = layout.repeatTimes;
            affine.strides[0] = layout.blockStride;
            affine.strides[1] = layout.repeatStride;
            if (!VisitAffineSegments(affine, appendAccess)) {
                return markIncomplete();
            }
            break;
        }
        case ACLSAN_MEM_LAYOUT_ND_AFFINE: {
            const auto& layout = data.layout.ndAffine;
            AffineLayout affine{};
            affine.base = data.address;
            affine.rank = layout.rank;
            affine.elementBytes = layout.elementBytes;
            for (uint32_t dimension = 0; dimension < kMaxAffineRank; ++dimension) {
                affine.dimensions[dimension] = layout.dims[dimension];
                affine.strides[dimension] = layout.strides[dimension];
            }
            if (!VisitAffineSegments(affine, appendAccess)) {
                return markIncomplete();
            }
            break;
        }
        default:
            return markIncomplete();
    }

    return reports;
}

std::vector<NpuCheckMemcheckReport> Memcheck::OnSynchronization()
{
    ++stats_.synchronizationEvents;
    std::vector<AclsanDeviceMemoryAccessData> accesses;
    accesses.swap(pendingDeviceAccesses_);
    stats_.pendingDeviceOperations = pendingDeviceAccesses_.size(); // update stats after swap

    using InstructionIdentity = std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t>;
    std::map<InstructionIdentity, uint64_t> instructionGroups;
    std::vector<NpuCheckMemcheckReport> reports;
    for (const auto& access : accesses) {
        const InstructionIdentity identity{
            access.header.deviceId, access.header.launchId, access.header.blockType, access.header.blockId,
            access.header.instrExecId};
        const auto [group, inserted] = instructionGroups.try_emplace(identity, nextGroupId_);
        if (inserted) {
            ++nextGroupId_;
        }
        auto found = CheckDeviceMemoryAccess(access, group->second);
        reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    Count(reports);
    return reports;
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
