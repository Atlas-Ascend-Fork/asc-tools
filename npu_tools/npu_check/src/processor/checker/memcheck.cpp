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
#include "aclsan_ub_layout_builder.h"
#include "dbi/trace_buffer_abi.h"
#include "checker/memory_access_layout.h"
#include "checker/memory_access_mode.h"
#include "diagnostic/pipeline_name.h"
#include "simt_memory_record.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
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

bool IsSimtUbWrite(const AclsanDeviceMemoryAccessData& data)
{
    const bool write = data.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE || IsAtomicWrite(data.accessMode);
    return (data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_SIMT_UB)) != 0U &&
           data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB && write;
}

bool ValidUbLayout(const AclsanUbLayout& layout)
{
    if (layout.valid != ACLSAN_UB_LAYOUT_VALID || layout.physicalBytes != aclsan::DAV3510_UB_CAPACITY_BYTES ||
        layout.regions[ACLSAN_UB_REGION_STATIC].begin != 0U) {
        return false;
    }
    uint64_t previousEnd = 0U;
    for (const auto& region : layout.regions) {
        if (region.begin < previousEnd || region.end < region.begin || region.end > layout.physicalBytes) {
            return false;
        }
        previousEnd = region.end;
    }
    const auto& staticRegion = layout.regions[ACLSAN_UB_REGION_STATIC];
    const auto& dynamicRegion = layout.regions[ACLSAN_UB_REGION_DYNAMIC];
    const auto& reservedRegion = layout.regions[ACLSAN_UB_REGION_RESERVED];
    const auto& cacheRegion = layout.regions[ACLSAN_UB_REGION_DATA_CACHE];
    return staticRegion.end == dynamicRegion.begin && dynamicRegion.end == reservedRegion.begin &&
           reservedRegion.end - reservedRegion.begin == aclsan::DAV3510_UB_DEFAULT_RESERVED_BYTES &&
           cacheRegion.end - cacheRegion.begin >= aclsan::SIMT_UB_MIN_CACHE_BYTES &&
           cacheRegion.end - cacheRegion.begin <= aclsan::SIMT_UB_MAX_CACHE_BYTES;
}

bool HasUsableSimtUbWrite(const AclsanSimtUbMemoryAccessData& data)
{
    constexpr uint32_t CONTEXT = static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_THREAD) |
                                 static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_BLOCK_DIM) |
                                 static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_ADDRESS);
    if (data.header.size < sizeof(data) || !ValidUbLayout(data.ubLayout) ||
        (data.simt.validFlags & CONTEXT) != CONTEXT || data.simt.threadX >= data.simt.blockDimX ||
        data.simt.threadY >= data.simt.blockDimY || data.simt.threadZ >= data.simt.blockDimZ ||
        (data.header.flags & (static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) |
                              static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED))) != 0U) {
        return false;
    }
    if ((data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_PREDICATED)) != 0U &&
        data.predicateMask0 == 0U && data.predicateMask1 == 0U) {
        return false;
    }
    return (data.layoutKind == ACLSAN_MEM_LAYOUT_RANGE && data.layout.range.bytes != 0U) ||
           (data.layoutKind == ACLSAN_MEM_LAYOUT_SCALAR && data.layout.scalar.bytes != 0U);
}

bool IsValidUbWrite(const AclsanUbLayout& layout, uint64_t address, uint64_t bytes)
{
    if (address >= layout.physicalBytes || bytes > layout.physicalBytes - address) {
        return false;
    }
    for (const auto& region : layout.regions) {
        if (region.begin <= address && address < region.end) {
            return bytes <= region.end - address;
        }
    }
    return false;
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
    if ((data.simt.validFlags & static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_INVOCATION)) != 0U) {
        exec.invocationId = data.simt.invocationId;
    }
    if ((data.simt.validFlags & static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_THREAD)) != 0U) {
        exec.threadX = data.simt.threadX;
        exec.threadY = data.simt.threadY;
        exec.threadZ = data.simt.threadZ;
    }
    if ((data.simt.validFlags & static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_BLOCK_DIM)) != 0U) {
        exec.blockDimX = data.simt.blockDimX;
        exec.blockDimY = data.simt.blockDimY;
        exec.blockDimZ = data.simt.blockDimZ;
    }
    exec.simtContextFlags = data.simt.validFlags;
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

const char* UbRegionName(UbRegionKind kind)
{
    switch (kind) {
        case UbRegionKind::USER:
            return "USER";
        case UbRegionKind::STATIC:
            return "STATIC";
        case UbRegionKind::DYNAMIC:
            return "DYNAMIC";
        case UbRegionKind::RESERVED:
            return "RESERVED";
        case UbRegionKind::SIMD_FORBIDDEN:
            return "SIMD_FORBIDDEN";
    }
    return "UNKNOWN";
}

std::string UbAddressRegion(const UbLayout* layout, uint64_t address)
{
    if (address >= UB_CAPACITY) {
        return "OUTSIDE_UB";
    }
    const auto region = layout != nullptr ? layout->Find(address) : std::nullopt;
    return region.has_value() ? UbRegionName(region->kind) : "UNKNOWN";
}

NpuCheckReportUbContext DescribeUbViolation(
    const UbLayout* layout, uint64_t operandAddress, uint64_t address, uint64_t bytes)
{
    NpuCheckReportUbContext context;
    context.capacityBytes = UB_CAPACITY;
    context.operandAddress = operandAddress;
    context.fragmentBytes = bytes;
    context.originRegion = UbAddressRegion(layout, operandAddress);
    if (layout != nullptr) {
        context.kernelVfType = layout->kernelVfType;
        for (const auto& region : layout->regions) {
            context.regions.push_back({UbRegionName(region.kind), region.begin, region.end});
        }
    }
    // Match the classifier's physical-capacity precedence, including unknown layouts.
    if (operandAddress >= UB_CAPACITY || address >= UB_CAPACITY || bytes > UB_CAPACITY - address) {
        const bool fragmentOutside = address >= UB_CAPACITY || bytes > UB_CAPACITY - address;
        // Negative strides may move the reported fragment below the operand base.
        // If this fragment is inside UB, only the invalid original base is known.
        context.firstInvalidAddress = fragmentOutside ? std::max(address, UB_CAPACITY) : operandAddress;
        context.boundaryAddress = UB_CAPACITY;
        context.invalidRegion = "OUTSIDE_UB";
        return context;
    }
    const auto origin = layout != nullptr ? layout->Find(operandAddress) : std::nullopt;
    if (origin.has_value() && !origin->AllowsSimdAccess()) {
        context.violation = NpuCheckReportUbViolation::FORBIDDEN_REGION;
        context.firstInvalidAddress = operandAddress;
    } else if (origin.has_value()) {
        context.violation = NpuCheckReportUbViolation::OUTSIDE_REGION;
        context.boundaryAddress = address < origin->begin ? origin->begin : origin->end;
        // A strided access reports its actual fragment, never an unaccessed hole.
        context.firstInvalidAddress = address < origin->begin ? address : std::max(address, origin->end);
    }
    context.invalidRegion = UbAddressRegion(layout, context.firstInvalidAddress);
    return context;
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
        for (auto& [launchId, snapshot] : launchAllocations_) {
            (void)launchId;
            const auto allocation = snapshot.allocations.find({data.deviceId, 0});
            if (allocation != snapshot.allocations.end() &&
                allocation->second.Classify(data.deviceId, reinterpret_cast<uint64_t>(data.ptr), 1).status ==
                    RangeStatus::VALID) {
                snapshot.lifetimeUncertain = true;
            }
        }
    }
}

void Memcheck::QueueSimtUbMemoryAccess(const AclsanSimtUbMemoryAccessData& data)
{
    if (data.header.version != ACLSAN_API_VERSION || data.header.size < sizeof(data)) {
        ++stats_.droppedDeviceOperations;
        return;
    }
    if ((data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR)) != 0U) {
        ++stats_.droppedDeviceOperations;
    }
    if ((data.header.flags & (static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) |
                              static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED))) != 0U) {
        ++stats_.droppedDeviceOperations;
        return;
    }
    // Reads and inactive or zero-width writes need no write bounds check. Keep any loss flags above.
    const bool simtUb = (data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_SIMT_UB)) != 0U &&
                        data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB;
    if (simtUb && data.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ) {
        // The SIMT UB producer currently emits writes only. Reads are outside this
        // write bounds check and its operation counters; add read checking and
        // accounting together if the producer gains UB read support.
        return;
    }
    if (IsSimtUbWrite(data) &&
        (((data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_PREDICATED)) != 0U &&
          data.predicateMask0 == 0U && data.predicateMask1 == 0U) ||
         (data.layoutKind == ACLSAN_MEM_LAYOUT_RANGE && data.layout.range.bytes == 0U) ||
         (data.layoutKind == ACLSAN_MEM_LAYOUT_SCALAR && data.layout.scalar.bytes == 0U))) {
        return;
    }
    if (!IsSimtUbWrite(data) || data.header.launchId == 0U || data.header.instrExecId == 0U ||
        !HasUsableSimtUbWrite(data) || pendingSimtUbAccesses_.size() >= MAX_PENDING_DEVICE_OPERATIONS) {
        ++stats_.droppedDeviceOperations;
        return;
    }
    // Private UB callbacks bypass MemoryAnalysis ingress. Unknown or already completed launches
    // cannot establish completeness, but reliable records can still prove an out-of-bounds write.
    if (analysis_->Managed() && observedLaunches_.count(data.header.launchId) == 0U) {
        unfinishedLaunches_ = true;
    }
    pendingSimtUbAccesses_.push_back(data);
    ++stats_.deviceOperations;
    ++stats_.pendingDeviceOperations;
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckSimtUbMemoryAccess(const AclsanSimtUbMemoryAccessData& data)
{
    const uint64_t bytes =
        data.layoutKind == ACLSAN_MEM_LAYOUT_RANGE ? data.layout.range.bytes : data.layout.scalar.bytes;
    if (IsValidUbWrite(data.ubLayout, data.address, bytes)) {
        return {};
    }
    NpuCheckMemcheckReport report{};
    report.common.reportId = nextReportId_++;
    report.common.groupId = nextGroupId_++;
    report.common.timestampNs = TimestampNs();
    report.common.tool = ReportTool::MEMCHECK;
    report.common.severity = ReportSeverity::ERROR;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    report.common.exec = ToExecContext(data);
    report.common.pattern = NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS;
    const auto& lane = data.simt;
    const uint64_t threadId = lane.threadX + static_cast<uint64_t>(lane.blockDimX) *
                                                 (lane.threadY + static_cast<uint64_t>(lane.blockDimY) * lane.threadZ);
    report.simtUb = NpuCheckSimtUbDiagnostic{threadId, data.ubLayout};
    report.access.memorySpace = NpuCheckReportMemorySpace::UB;
    report.access.accessMode = NpuCheckReportAccessMode::WRITE;
    report.access.accessBytes = static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX));
    report.access.requiredAlign = data.alignSize;
    report.access.address = report.access.rangeBegin = data.address;
    report.access.rangeEnd = RangeEnd(data.address, bytes).value_or(UINT64_MAX);
    report.allocation = ToReportAllocation(Allocation{0, 0, data.ubLayout.physicalBytes, data.header.deviceId, 0, 0});
    report.allocation.memorySpace = NpuCheckReportMemorySpace::UB;
    report.nearestAllocation = report.allocation;
    return {std::move(report)};
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckAccess(
    const AclsanDeviceMemoryAccessData& data, NpuCheckReportAccessMode accessMode, uint64_t address, uint64_t bytes,
    uint64_t groupId)
{
    const bool preload =
        (data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED)) != 0U && bytes == 0;
    const uint64_t capacity = CubeCapacity(data.memorySpace);
    const AllocationRegistry* allocations = &memoryState_->Allocations({data.header.deviceId, 0});
    const bool standaloneSimt = !analysis_->Managed() && data.simt.validFlags != 0;
    if (data.header.launchId != 0 && standaloneSimt) {
        const auto snapshot = launchAllocations_.find(data.header.launchId);
        if (snapshot == launchAllocations_.end()) {
            ++stats_.droppedDeviceOperations;
            return {};
        }
        static const AllocationRegistry empty;
        const auto allocation = snapshot->second.allocations.find({data.header.deviceId, 0});
        allocations = allocation == snapshot->second.allocations.end() ? &empty : &allocation->second;
    }
    RangeResult range{};
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB) {
        range = analysis_->ClassifyUbRange(data, address, bytes);
    } else if (capacity != 0) {
        range.status =
            address <= capacity && bytes <= capacity - address ? RangeStatus::VALID : RangeStatus::OUT_OF_BOUNDS;
        range.allocation = Allocation{0, 0, capacity, data.header.deviceId, 0, 0};
    } else {
        range = standaloneSimt ? allocations->Classify(data.header.deviceId, address, preload ? 1 : bytes) :
                                 analysis_->ClassifyAccessRange(data, address, preload ? 1 : bytes);
    }
    if (range.status == RangeStatus::VALID) {
        return {};
    }
    if (range.status == RangeStatus::AMBIGUOUS) {
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
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB) {
        report.ubContext =
            DescribeUbViolation(analysis_->FindUbLayout(data.header.launchId), data.address, address, bytes);
    }

    report.allocation = ToReportAllocation(range.allocation);
    const auto nearest = range.allocation ? range.allocation : allocations->Nearest(data.header.deviceId, address);
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
    if (data.header.size < sizeof(data)) {
        ++stats_.droppedDeviceOperations;
        return {};
    }
    // SIMT memory belongs to the SIMT checker, not the SIMD adapter.
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB && IsSimtMemoryInstruction(data.instructionId)) {
        return {};
    }
    // 原子配置未知但范围已还原的 WRITE 仍按普通写检查边界。
    // ATOMIC_CONFIG_UNKNOWN 不影响 Memcheck；ESTIMATED 表示访问范围也不精确。
    if ((data.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED)) != 0U) {
        ++stats_.inexactDeviceOperations;
        return {};
    }
    if (data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM && CubeCapacity(data.memorySpace) == 0) {
        return {};
    }
    ++stats_.deviceOperations;
    if (!analysis_->Managed()) {
        pendingDeviceAccesses_.push_back(data);
        ++stats_.pendingDeviceOperations;
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

void Memcheck::QueueSimtMemoryAccess(const SimtMemoryAccessRecord& record)
{
    if (record.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM ||
        (record.accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_READ &&
         record.accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_WRITE)) {
        ++stats_.droppedDeviceOperations;
        return;
    }
    const auto range = ResolveSimtAccessRange(record);
    if (!range.has_value()) {
        ++stats_.droppedDeviceOperations;
        return;
    }
    AclsanDeviceMemoryAccessData data{};
    data.header.version = ACLSAN_API_VERSION;
    data.header.size = sizeof(data);
    data.header.launchId = record.launchId;
    data.header.instrExecId = record.invocationId;
    data.header.pc = record.pc;
    data.header.deviceId = record.deviceId;
    data.header.executor.phyCoreId = record.phyCoreId;
    data.header.executor.blockId = record.blockId;
    data.header.executor.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    data.header.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    data.header.sourceKind = ACLSAN_DEVICE_SOURCE_VECTOR;
    data.address = range->first;
    data.memorySpace = record.memorySpace;
    data.accessMode = record.accessMode;
    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
    data.layout.range.bytes = record.accessBytes;
    data.instructionId = record.instructionId;
    data.simt.invocationId = record.invocationId;
    data.simt.threadX = record.threadX;
    data.simt.threadY = record.threadY;
    data.simt.threadZ = record.threadZ;
    data.simt.blockDimX = record.blockDimX;
    data.simt.blockDimY = record.blockDimY;
    data.simt.blockDimZ = record.blockDimZ;
    data.simt.base = record.base;
    data.simt.offset = record.offset;
    data.simt.elementBytes = record.elementBytes;
    data.simt.validFlags =
        static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_INVOCATION) | static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_THREAD) |
        static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_BLOCK_DIM) | static_cast<uint32_t>(ACLSAN_SIMT_CONTEXT_ADDRESS);
    if (analysis_->Managed()) {
        if (!analysis_->BeginCallback(
                ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SIMT_MEMORY_ACCESS, &data)) {
            ++stats_.droppedDeviceOperations;
            return;
        }
        analysis_->EndCallback();
    } else if (record.launchId != 0) {
        launchAllocations_.try_emplace(record.launchId, LaunchAllocationSnapshot{memoryState_->allocations, nullptr});
    }
    QueueDeviceMemoryAccess(data);
}

std::vector<NpuCheckMemcheckReport> Memcheck::CheckDeviceMemoryAccess(
    const AclsanDeviceMemoryAccessData& data, uint64_t groupId)
{
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB && data.layoutKind == ACLSAN_MEM_LAYOUT_RANGE &&
        data.layout.range.bytes == 0) {
        return {};
    }
    const auto markIncomplete = [this]() {
        ++stats_.droppedDeviceOperations;
        return std::vector<NpuCheckMemcheckReport>{};
    };

    const auto accessMode = ReportMemoryAccessMode(data.accessMode);
    if (!accessMode) {
        return markIncomplete();
    }

    if ((data.header.flags & kDeviceEventFlagPredicated) != 0U && data.predicateMask0 == 0 &&
        data.predicateMask1 == 0) {
        return {};
    }

    std::vector<NpuCheckMemcheckReport> reports;
    auto appendAccess = [this, &data, accessMode = *accessMode, &reports, groupId](uint64_t address, uint64_t bytes) {
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
    const auto check = [this, &reports](const auto& accesses) {
        for (const auto& access : accesses) {
            if (access.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM ||
                (access.header.flags & static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED)) != 0U) {
                continue;
            }
            auto found = CheckObservedAccess(access);
            reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
            if (stats_.pendingDeviceOperations != 0U) {
                --stats_.pendingDeviceOperations;
            }
        }
    };
    check(shared.memory);
    check(shared.simtMemory);
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
        const auto snapshot = launchAllocations_.find(launchId);
        if (snapshot != launchAllocations_.end()) {
            if (snapshot->second.lifetimeUncertain) {
                ++stats_.droppedDeviceOperations;
            }
            launchAllocations_.erase(snapshot);
        }
    }
    const auto ubEnd = std::remove_if(
        pendingSimtUbAccesses_.begin(), pendingSimtUbAccesses_.end(), [this, launchId, &reports](const auto& access) {
            if (access.header.launchId != launchId) {
                return false;
            }
            auto found = CheckSimtUbMemoryAccess(access);
            reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
            if (stats_.pendingDeviceOperations != 0U) {
                --stats_.pendingDeviceOperations;
            }
            return true;
        });
    pendingSimtUbAccesses_.erase(ubEnd, pendingSimtUbAccesses_.end());
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
        launchAllocations_.clear();
        instructionGroups_.clear();
        stats_.pendingDeviceOperations = pendingSimtUbAccesses_.size();
    }
    std::vector<AclsanSimtUbMemoryAccessData> ubAccesses;
    ubAccesses.swap(pendingSimtUbAccesses_);
    for (auto& access : ubAccesses) {
        if (analysis_->Managed() && analysis_->Closing().count(access.header.launchId) == 0) {
            pendingSimtUbAccesses_.push_back(std::move(access));
            continue;
        }
        auto found = CheckSimtUbMemoryAccess(access);
        reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        if (stats_.pendingDeviceOperations != 0U) {
            --stats_.pendingDeviceOperations;
        }
    }
    Count(reports);
    return reports;
}

std::vector<NpuCheckMemcheckReport> Memcheck::FlushSimtUbMemoryAccesses()
{
    std::vector<NpuCheckMemcheckReport> reports;
    for (const auto& access : pendingSimtUbAccesses_) {
        auto found = CheckSimtUbMemoryAccess(access);
        reports.insert(reports.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        if (stats_.pendingDeviceOperations != 0U) {
            --stats_.pendingDeviceOperations;
        }
    }
    pendingSimtUbAccesses_.clear();
    return reports;
}

void Memcheck::Finalize(CheckerReportList& reports)
{
    unfinishedLaunches_ = unfinishedLaunches_ || !observedLaunches_.empty() || !pendingDeviceAccesses_.empty() ||
                          !pendingSimtUbAccesses_.empty();
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
    stats_.pendingDeviceOperations = pendingSimtUbAccesses_.size();
    Count(completed);
    AppendCheckerReports(std::move(completed), reports);
    auto ubReports = FlushSimtUbMemoryAccesses();
    Count(ubReports);
    AppendCheckerReports(std::move(ubReports), reports);
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
