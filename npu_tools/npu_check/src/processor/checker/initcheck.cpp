/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/initcheck_padding.h"
#include "checker/initcheck.h"
#include "checker/initcheck_report.h"
#include "checker/memory_access_layout.h"
#include "device_instr/common/instruction_id.h"
#include "npu_tool_log.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace npucheck {

namespace {
constexpr uint8_t MAX_WRITER_PIPELINE = 0x0f; // State-byte bits [3:0].
} // namespace

namespace {
struct Range {
    uint64_t address;
    uint64_t bytes;
};
struct Operand {
    const AclsanDeviceMemoryAccessData* access;
    std::vector<Range> ranges;
};

bool ExpandRanges(const AclsanDeviceMemoryAccessData& data, std::vector<Range>& ranges)
{
    bool valid = true;
    auto append = [&valid, &ranges](uint64_t address, uint64_t bytes) {
        if (!GmInitStateTable::ValidRange(address, bytes))
            valid = false;
        if (bytes != 0)
            ranges.push_back({address, bytes});
    };
    using namespace memory_layout;
    AffineLayout layout{};
    layout.base = data.address;
    switch (data.layoutKind) {
        case ACLSAN_MEM_LAYOUT_SCALAR:
            append(data.address, data.layout.scalar.bytes);
            break;
        case ACLSAN_MEM_LAYOUT_RANGE:
            append(data.address, data.layout.range.bytes);
            break;
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT: {
            constexpr uint32_t BLOCK_REPEAT_RANK = 2;
            layout.rank = BLOCK_REPEAT_RANK;
            layout.elementBytes = data.layout.blockRepeat.blockSize;
            layout.dimensions[0] = data.layout.blockRepeat.blockNum;
            layout.dimensions[1] = data.layout.blockRepeat.repeatTimes;
            layout.strides[0] = data.layout.blockRepeat.blockStride;
            layout.strides[1] = data.layout.blockRepeat.repeatStride;
            if (!VisitAffineSegments(layout, append))
                return false;
            break;
        }
        case ACLSAN_MEM_LAYOUT_ND_AFFINE:
            layout.rank = data.layout.ndAffine.rank;
            layout.elementBytes = data.layout.ndAffine.elementBytes;
            for (uint32_t i = 0; i < kMaxAffineRank; ++i) {
                layout.dimensions[i] = data.layout.ndAffine.dims[i];
                layout.strides[i] = data.layout.ndAffine.strides[i];
            }
            if (!VisitAffineSegments(layout, append))
                return false;
            break;
        default:
            return false;
    }
    if (!valid)
        return false;
    // 按有效字节集合统计，重叠访问不重复计数，步长空洞不算已写入。
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.address < b.address; });
    size_t count = 0;
    for (const auto range : ranges) {
        if (count != 0 && range.address <= ranges[count - 1].address + ranges[count - 1].bytes) {
            auto& last = ranges[count - 1];
            last.bytes = std::max(last.address + last.bytes, range.address + range.bytes) - last.address;
        } else {
            ranges[count++] = range;
        }
    }
    ranges.resize(count);
    return true;
}

const char* PipelineName(uint32_t pipeline);

NpuCheckReportMemorySpace ReportMemorySpace(uint32_t space)
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
            return NpuCheckReportMemorySpace::GM;
    }
}

NpuCheckInitcheckReport MakeMemoryReport(const AclsanDeviceMemoryAccessData& data, const Allocation& allocation)
{
    auto report = BuildInitcheckMemoryReportBase(data, allocation.bytes, allocation.deviceId);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    report.common.timestampNs =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    report.allocation.state = 1;
    report.allocation.allocId = allocation.resourceId;
    report.allocation.base = allocation.base;
    report.allocation.allocSerialNo = allocation.allocSequence;
    return report;
}
} // namespace

NpuCheckInitcheckReport BuildInitcheckMemoryReportBase(
    const AclsanDeviceMemoryAccessData& data, uint64_t allocationBytes, uint32_t allocationDeviceId)
{
    NpuCheckInitcheckReport report{};
    report.common.tool = ReportTool::INITCHECK;
    report.common.severity = (data.memoryFlags & ACLSAN_DEVICE_MEMORY_FLAG_HARDWARE_PADDING) != 0 ?
                                 ReportSeverity::WARNING :
                                 ReportSeverity::ERROR;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    auto& exec = report.common.exec;
    const auto& h = data.header;
    exec.launchId = h.launchId;
    exec.instrExecId = h.instrExecId;
    exec.serialNo = h.serialNo;
    exec.pc = h.pc;
    exec.deviceId = h.deviceId;
    exec.phyCoreId = h.executor.phyCoreId;
    exec.blockId = h.executor.blockId;
    exec.blockType = h.executor.blockType;
    exec.pipeId = h.pipeline;
    exec.pipeName = PipelineName(h.pipeline);
    exec.siteId = h.siteId;
    report.access.memorySpace = ReportMemorySpace(data.memorySpace);
    report.access.accessMode = NpuCheckReportAccessMode::READ;
    report.access.address = data.address;
    report.access.requiredAlign = data.alignSize;
    report.allocation.bytes = allocationBytes;
    report.allocation.deviceId = allocationDeviceId;
    report.allocation.memorySpace = report.access.memorySpace;
    return report;
}

AllocationUpdateStatus MemoryInitcheck::RegisterAllocation(
    InitStateKey scope, uint64_t id, uint64_t address, uint64_t bytes)
{
    return state_->Register(scope, id, address, bytes);
}
AllocationUpdateStatus MemoryInitcheck::ReleaseAllocation(InitStateKey scope, uint64_t id, uint64_t address)
{
    return state_->Release(scope, id, address);
}

InitCheckStatus MemoryInitcheck::MarkInitialized(InitStateKey scope, uint64_t address, uint64_t bytes, uint8_t pipe)
{
    if (!InitStateTableIndex::ValidKey(scope) || !GmInitStateTable::ValidRange(address, bytes) ||
        pipe > MAX_WRITER_PIPELINE)
        return InitCheckStatus::INVALID_RECORD;
    if (bytes == 0)
        return InitCheckStatus::OK;
    if (scope.space == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
        const auto context = state_->allocations.find(scope);
        if (context == state_->allocations.end() ||
            context->second.Classify(scope.deviceId, address, bytes).status != RangeStatus::VALID)
            return InitCheckStatus::INVALID_ALLOCATION;
    } else {
        const auto capacity = InitStateTableIndex::LocalCapacity(scope.space);
        if (address > capacity || bytes > capacity - address)
            return InitCheckStatus::INVALID_ALLOCATION;
    }
    try {
        state_->initialized.GetOrCreate(scope).Write(address, bytes, pipe);
        return InitCheckStatus::OK;
    } catch (const std::bad_alloc&) {
        return InitCheckStatus::RESOURCE_EXHAUSTED;
    }
}

namespace {
constexpr uint32_t INCOMPLETE_FLAGS = static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED) |
                                      static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED) |
                                      static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR);

bool IsCompleteInstructionAccess(const AclsanDeviceMemoryAccessData& access, size_t accessCount)
{
    return access.accessCount == accessCount && (access.header.flags & ACLSAN_DEVICE_EVENT_FLAG_EXACT) != 0U &&
           (access.header.flags & INCOMPLETE_FLAGS) == 0U;
}

bool MatchesInitializationOwner(const InitStateKey& scope, const AclsanDeviceMemoryAccessData& access)
{
    if (scope.space == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
        return true;
    }
    const auto& owner = access.memoryOwner;
    return scope.blockType == owner.blockType && scope.blockId == (scope.physical ? owner.phyCoreId : owner.blockId);
}

InitCheckStatus CollectInstructionOperands(
    InitStateKey scope, const std::vector<AclsanDeviceMemoryAccessData>& accesses, std::vector<Operand>& operands)
{
    std::vector<bool> seen(accesses.size(), false);
    // 完整性检查先于任何状态更新。调用方必须已校验配置寄存器依赖，并按同步依赖排序。
    for (const auto& access : accesses) {
        const auto& h = access.header;
        if (h.version != ACLSAN_API_VERSION || h.size < sizeof(access) || h.deviceId != scope.deviceId ||
            race::Key(access) != race::Key(accesses.front()) || h.pipeline > MAX_WRITER_PIPELINE ||
            access.accessIndex >= accesses.size() || seen[access.accessIndex] ||
            access.accessMode < ACLSAN_DEVICE_MEMORY_ACCESS_READ ||
            access.accessMode > ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX) {
            return InitCheckStatus::INVALID_RECORD;
        }
        if (!IsCompleteInstructionAccess(access, accesses.size())) {
            return InitCheckStatus::INCOMPLETE_RECORD;
        }
        seen[access.accessIndex] = true;
        if (access.memorySpace != scope.space)
            continue;
        if (!MatchesInitializationOwner(scope, access)) {
            return InitCheckStatus::INVALID_RECORD;
        }
        if ((h.flags & ACLSAN_DEVICE_EVENT_FLAG_PREDICATED) != 0U && access.predicateMask0 == 0 &&
            access.predicateMask1 == 0)
            continue;
        Operand operand{&access, {}};
        if (!ExpandRanges(access, operand.ranges))
            return InitCheckStatus::INVALID_RECORD;
        operands.push_back(std::move(operand));
    }
    return InitCheckStatus::OK;
}

template <typename Classify>
InitCheckStatus ValidateOperandAllocations(const std::vector<Operand>& operands, const Classify& classify)
{
    for (const auto& operand : operands) {
        uint64_t total = 0;
        for (const auto& range : operand.ranges) {
            const auto status = classify(range.address, range.bytes).status;
            if (status == RangeStatus::UNKNOWN || status == RangeStatus::AMBIGUOUS) {
                return InitCheckStatus::UNRESOLVED_ALLOCATION;
            }
            if (status != RangeStatus::VALID) {
                return InitCheckStatus::INVALID_ALLOCATION;
            }
            total += range.bytes;
        }
        // 报告 ABI 的 accessBytes 为 uint32_t，不静默截断读取大小。
        if (total > std::numeric_limits<uint32_t>::max())
            return InitCheckStatus::RESOURCE_EXHAUSTED;
    }
    return InitCheckStatus::OK;
}

// GM combines its launch baseline with proven preceding writes. Local
// replay queries actual state bytes only where a preceding write proves
// visibility; topologically earlier but unordered writes cannot leak in.
InitStateQuery QueryInstructionInitialization(
    const InitStateTable& table, const MemoryInitcheck::InitializedRanges& precedingWrites,
    bool restrictToPrecedingWrites, InitStateKey scope, uint64_t address, uint64_t bytes)
{
    InitStateQuery result;
    const auto append = [&table, &result](uint64_t begin, uint64_t end) {
        const auto q = table.Query(begin, end - begin);
        if (result.uninitializedBytes == 0 && q.uninitializedBytes != 0)
            result.firstUninitializedAddress = q.firstUninitializedAddress;
        result.uninitializedBytes += q.uninitializedBytes;
    };
    const auto appendGap = [&append, &result, restrictToPrecedingWrites](uint64_t begin, uint64_t end) {
        if (!restrictToPrecedingWrites) {
            append(begin, end);
        } else if (begin < end) {
            if (result.uninitializedBytes == 0)
                result.firstUninitializedAddress = begin;
            result.uninitializedBytes += end - begin;
        }
    };
    const uint64_t end = address + bytes;
    for (const auto& range : precedingWrites) {
        if (range.second <= address)
            continue;
        if (range.first >= end)
            break;
        if (range.first > address)
            appendGap(address, range.first);
        const auto coveredBegin = std::max(address, range.first);
        const auto coveredEnd = std::min(end, range.second);
        if (restrictToPrecedingWrites && scope.space != ACLSAN_DEVICE_MEMORY_SPACE_GM)
            append(coveredBegin, coveredEnd);
        address = coveredEnd;
    }
    if (address < end)
        appendGap(address, end);
    return result;
}

template <typename Classify, typename Query>
void ReportInstructionReads(
    const std::vector<Operand>& operands, const Classify& classify, const Query& queryInitialized, uint64_t group,
    uint64_t& nextReportId, std::vector<NpuCheckInitcheckReport>& output)
{
    // Report all reads before writing any state for this instruction.
    for (const auto& operand : operands) {
        if (!race::ReadsMemory(operand.access->accessMode))
            continue;
        std::map<uint64_t, NpuCheckInitcheckReport> reports;
        for (const auto& range : operand.ranges) {
            const auto allocation = *classify(range.address, range.bytes).allocation;
            auto [it, inserted] = reports.try_emplace(allocation.base);
            auto& report = it->second;
            if (inserted) {
                report = MakeMemoryReport(*operand.access, allocation);
                // rowBytes 非零表示读取携带 NZ 有效数据布局，先暂定为 WARNING。
                // 若物理读范围内只有对齐填充字节未初始化，则最终报告 WARNING；
                // 若有效数据未初始化，或布局无效且存在未初始化字节，后续检查会升级为 ERROR；
                // 全部初始化则不报告。
                if (operand.access->validNz.rowBytes != 0) {
                    report.common.severity = ReportSeverity::WARNING;
                }
                report.access.rangeBegin = range.address;
            }
            report.access.rangeEnd = range.address + range.bytes;
            report.access.accessBytes += static_cast<uint32_t>(range.bytes);
            const auto query = queryInitialized(range.address, range.bytes);
            if (query.uninitializedBytes != 0 && report.uninitBytes == 0) {
                report.firstUninitAddress = query.firstUninitializedAddress;
                report.firstUninitOffset = query.firstUninitializedAddress - allocation.base;
            }
            report.uninitBytes += query.uninitializedBytes;
            if (operand.access->validNz.rowBytes != 0 && query.uninitializedBytes != 0 &&
                !RequiredReadInitialized(
                    *operand.access, range.address, range.address + range.bytes,
                    [&queryInitialized](uint64_t begin, uint64_t end) {
                        return queryInitialized(begin, end - begin).uninitializedBytes == 0;
                    })) {
                report.common.severity = ReportSeverity::ERROR;
            }
        }
        for (auto& entry : reports) {
            auto& report = entry.second;
            if (report.uninitBytes == 0)
                continue;
            report.initializedBytes = report.access.accessBytes - report.uninitBytes;
            report.allocationInitializedBytes =
                report.allocation.bytes -
                queryInitialized(report.allocation.base, report.allocation.bytes).uninitializedBytes;
            report.common.pattern = report.initializedBytes == 0 ?
                                        NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ :
                                        NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ;
            report.common.reportId = nextReportId++;
            report.common.groupId = group;
            output.push_back(std::move(report));
        }
    }
}
} // namespace

InitCheckResult MemoryInitcheck::CheckInstruction(
    InitStateKey scope, const std::vector<AclsanDeviceMemoryAccessData>& accesses,
    const InitializedRanges& precedingWrites, bool commitWrites, bool restrictToPrecedingWrites,
    const RangeClassifier& classifier)
{
    InitCheckResult result;
    auto fail = [](InitCheckStatus status) { return InitCheckResult{status, {}}; };
    if (!InitStateTableIndex::ValidKey(scope))
        return fail(InitCheckStatus::INVALID_RECORD);
    if (accesses.empty())
        return result;
    try {
        const auto& first = accesses.front().header;
        if (scope.space != ACLSAN_DEVICE_MEMORY_SPACE_GM && scope.launchId != first.launchId)
            return fail(InitCheckStatus::INVALID_RECORD);
        std::vector<Operand> operands;
        const auto collectStatus = CollectInstructionOperands(scope, accesses, operands);
        if (collectStatus != InitCheckStatus::OK) {
            return fail(collectStatus);
        }
        const auto found = state_->allocations.find(scope);
        const auto classify = [this, scope, found, &classifier](uint64_t address, uint64_t bytes) -> RangeResult {
            if (scope.space == ACLSAN_DEVICE_MEMORY_SPACE_GM && classifier)
                return classifier(address, bytes);
            if (scope.space != ACLSAN_DEVICE_MEMORY_SPACE_GM) {
                const auto capacity = InitStateTableIndex::LocalCapacity(scope.space);
                if (address > capacity || bytes > capacity - address)
                    return {RangeStatus::OUT_OF_BOUNDS, std::nullopt};
                Allocation allocation{};
                allocation.bytes = capacity;
                allocation.deviceId = scope.deviceId;
                return {RangeStatus::VALID, allocation};
            }
            return found == state_->allocations.end() ? RangeResult{} :
                                                        found->second.Classify(scope.deviceId, address, bytes);
        };
        const auto allocationStatus = ValidateOperandAllocations(operands, classify);
        if (allocationStatus != InitCheckStatus::OK) {
            return fail(allocationStatus);
        }
        if (operands.empty())
            return result;
        auto& table = state_->initialized.GetOrCreate(scope);
        const auto queryInitialized = [&table, &precedingWrites, restrictToPrecedingWrites, scope](
                                          uint64_t address, uint64_t bytes) {
            return QueryInstructionInitialization(
                table, precedingWrites, restrictToPrecedingWrites, scope, address, bytes);
        };
        const uint64_t group = nextGroupId_++;
        ReportInstructionReads(operands, classify, queryInitialized, group, nextReportId_, result.reports);
        for (const auto& operand : operands) {
            if (!commitWrites || !race::WritesMemory(operand.access->accessMode))
                continue;
            for (const auto& range : operand.ranges) {
                table.Write(range.address, range.bytes, static_cast<uint8_t>(operand.access->header.pipeline));
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return fail(InitCheckStatus::RESOURCE_EXHAUSTED);
    } catch (const std::length_error&) {
        return fail(InitCheckStatus::RESOURCE_EXHAUSTED);
    }
}

void MemoryInitcheck::ResetContext(InitStateKey scope)
{
    state_->initialized.Erase(scope);
    state_->allocations.erase(scope);
}

namespace {
bool Overlap(const std::vector<race::Interval>& a, const std::vector<race::Interval>& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].begin < b[j].end && b[j].begin < a[i].end)
            return true;
        if (a[i].end <= b[j].end)
            ++i;
        else
            ++j;
    }
    return false;
}

// Axis order of the ND/DN-to-Nz producer's affine layout.
struct L1PaddingLayout {
    static constexpr uint32_t kRank = 3;
    static constexpr size_t kRowAxis = 0;
    static constexpr size_t kColumnBlockAxis = 1;
    static constexpr size_t kMatrixAxis = 2;
    static constexpr uint64_t kBiasAlignmentBytes = 64;
};

uint64_t L1PaddingReadAlignment(const AclsanDeviceMemoryAccessData& read)
{
    using aclsan::InstructionId;
    constexpr uint64_t kFractalRowAlignment = 16;
    constexpr uint64_t kTransposeB8RowAlignment = 32;
    if (read.accessMode != ACLSAN_DEVICE_MEMORY_ACCESS_READ || read.header.pipeline != ACLSAN_DEVICE_PIPE_MTE1) {
        return 0;
    }
    const auto id = static_cast<InstructionId>(read.instructionId);
    switch (id) {
        case InstructionId::CopyCbufToBtF32:
        case InstructionId::CopyCbufToBtS32:
            return L1PaddingLayout::kBiasAlignmentBytes;
        case InstructionId::LoadCbufToCbTransposeB8:
            return kTransposeB8RowAlignment; // Two adjacent 16-row fractals.
        case InstructionId::LoadCbufToCbTransposeB4:
            return 0; // ND/DN-to-Nz does not expose a packed 4-bit producer here.
        default:
            return id >= InstructionId::LoadCbufToCbTransposeB16 && id <= InstructionId::LoadCbufToCbB32 ?
                       kFractalRowAlignment :
                       0;
    }
}

constexpr uint64_t kL1PaddingBlockBytes = 32;

bool IsSingleL1PaddingLayout(const AclsanMemNdAffineLayout& nz)
{
    return nz.rank == L1PaddingLayout::kRank && nz.elementBytes == kL1PaddingBlockBytes &&
           nz.dims[L1PaddingLayout::kMatrixAxis] == 1 &&
           nz.strides[L1PaddingLayout::kRowAxis] == kL1PaddingBlockBytes && nz.dims[L1PaddingLayout::kRowAxis] != 0 &&
           nz.dims[L1PaddingLayout::kRowAxis] <= L1InitStateTable::kCapacity / kL1PaddingBlockBytes &&
           nz.dims[L1PaddingLayout::kColumnBlockAxis] != 0;
}

void AppendL1BiasPadding(
    uint64_t readAddress, const AclsanDeviceMemoryAccessData& write, uint64_t available,
    std::vector<race::Interval>& padding)
{
    // 32-bit bias is one dense Nz row. BT rounds the trailing length to 64B.
    constexpr uint32_t kBiasDataBits = 32;
    constexpr uint64_t kBlocksPerBiasAlignment = L1PaddingLayout::kBiasAlignmentBytes / kL1PaddingBlockBytes;
    const auto& nz = write.layout.ndAffine;
    const uint64_t blocks = nz.dims[L1PaddingLayout::kColumnBlockAxis];
    if (write.dataBits != kBiasDataBits || nz.dims[L1PaddingLayout::kRowAxis] != 1 ||
        nz.strides[L1PaddingLayout::kColumnBlockAxis] != kL1PaddingBlockBytes ||
        blocks % kBlocksPerBiasAlignment == 0 || blocks >= available / kL1PaddingBlockBytes ||
        readAddress != write.address) {
        return;
    }
    const uint64_t end = write.address + blocks * kL1PaddingBlockBytes;
    padding.push_back({end, end + kL1PaddingBlockBytes});
}

void AppendSingleL1Padding(
    const race::DependencyGraph::Access& read, uint64_t alignment, const AclsanDeviceMemoryAccessData& write,
    std::vector<race::Interval>& padding, size_t limit)
{
    using aclsan::InstructionId;
    constexpr uint64_t capacity = L1InitStateTable::kCapacity;
    const auto id = static_cast<InstructionId>(write.instructionId);
    if (id < InstructionId::CopyGmToCbufMultiNd2NzB8 || id > InstructionId::CopyGmToCbufMultiDn2NzB32 ||
        write.layoutKind != ACLSAN_MEM_LAYOUT_ND_AFFINE || write.address >= capacity || read.ranges.empty()) {
        return;
    }
    const auto& nz = write.layout.ndAffine;
    if (!IsSingleL1PaddingLayout(nz)) {
        return;
    }

    const uint64_t available = capacity - write.address;
    if (alignment == L1PaddingLayout::kBiasAlignmentBytes) {
        AppendL1BiasPadding(read.data.address, write, available, padding);
        return;
    }

    // Only the rounded row tail is padding. A larger column stride may leave
    // additional gaps, which are never included in the inferred padding.
    const uint64_t rows = nz.dims[L1PaddingLayout::kRowAxis];
    const uint64_t tileBytes = ((rows + alignment - 1) / alignment) * alignment * kL1PaddingBlockBytes;
    const uint64_t writtenBytes = rows * kL1PaddingBlockBytes;
    if (write.dataBits != read.data.dataBits || tileBytes == writtenBytes ||
        nz.strides[L1PaddingLayout::kColumnBlockAxis] < static_cast<int64_t>(tileBytes) ||
        nz.dims[L1PaddingLayout::kColumnBlockAxis] >
            available / static_cast<uint64_t>(nz.strides[L1PaddingLayout::kColumnBlockAxis])) {
        return;
    }
    const uint64_t columnStride = static_cast<uint64_t>(nz.strides[L1PaddingLayout::kColumnBlockAxis]);
    const uint64_t readBegin = read.ranges.front().begin, readEnd = read.ranges.back().end;
    if (readEnd <= write.address ||
        readBegin >= write.address + nz.dims[L1PaddingLayout::kColumnBlockAxis] * columnStride) {
        return;
    }
    const uint64_t first = readBegin <= write.address ? 0 : (readBegin - write.address) / columnStride;
    const uint64_t last =
        std::min(nz.dims[L1PaddingLayout::kColumnBlockAxis] - 1, (readEnd - 1 - write.address) / columnStride);
    for (uint64_t i = first; i <= last && padding.size() < limit; ++i) {
        const uint64_t base = write.address + i * columnStride;
        padding.push_back({base + writtenBytes, base + tileBytes});
    }
}

// Each matrix has an independent logical tail; gaps between matrices are not padding.
void AppendL1Padding(
    const race::DependencyGraph::Access& read, uint64_t alignment, const AclsanDeviceMemoryAccessData& write,
    std::vector<race::Interval>& padding, size_t limit)
{
    if (write.layoutKind != ACLSAN_MEM_LAYOUT_ND_AFFINE || write.layout.ndAffine.rank != L1PaddingLayout::kRank) {
        return;
    }
    const auto& nz = write.layout.ndAffine;
    if (nz.dims[L1PaddingLayout::kMatrixAxis] == 1) {
        AppendSingleL1Padding(read, alignment, write, padding, limit);
        return;
    }
    constexpr uint64_t capacity = L1InitStateTable::kCapacity;
    if (nz.dims[L1PaddingLayout::kMatrixAxis] == 0 || nz.strides[L1PaddingLayout::kMatrixAxis] <= 0 ||
        nz.strides[L1PaddingLayout::kColumnBlockAxis] <= 0 || nz.dims[L1PaddingLayout::kColumnBlockAxis] == 0 ||
        write.address >= capacity) {
        return;
    }
    const uint64_t matrixStride = static_cast<uint64_t>(nz.strides[L1PaddingLayout::kMatrixAxis]);
    const __int128 occupied = static_cast<__int128>(nz.dims[L1PaddingLayout::kColumnBlockAxis]) *
                              nz.strides[L1PaddingLayout::kColumnBlockAxis];
    if (occupied > matrixStride || nz.dims[L1PaddingLayout::kMatrixAxis] > (capacity - write.address) / matrixStride) {
        return;
    }
    auto matrix = write;
    matrix.layout.ndAffine.dims[L1PaddingLayout::kMatrixAxis] = 1;
    for (uint64_t i = 0; i < nz.dims[L1PaddingLayout::kMatrixAxis] && padding.size() < limit; ++i) {
        matrix.address = write.address + i * matrixStride;
        AppendSingleL1Padding(read, alignment, matrix, padding, limit);
    }
}

bool CoveredBy(const std::vector<race::Interval>& ranges, const std::vector<race::Interval>& coverage)
{
    return std::all_of(ranges.begin(), ranges.end(), [&coverage](const auto& range) {
        const auto after = std::upper_bound(
            coverage.begin(), coverage.end(), range.begin,
            [](uint64_t address, const auto& item) { return address < item.begin; });
        return after != coverage.begin() && std::prev(after)->end >= range.end;
    });
}
} // namespace

bool Initcheck::OnDeviceSync(AclsanCallbackId cbid, const void* data)
{
    if (data == nullptr) {
        ++stats_.malformedEvents;
        return false;
    }
    const auto& h = *static_cast<const AclsanDeviceEventHeader*>(data);
    const bool barrier = cbid == ACLSAN_CBID_DEVICE_PIPE_BARRIER;
    if (h.version != ACLSAN_API_VERSION ||
        h.size < (barrier ? sizeof(AclsanDeviceBarrierData) : sizeof(AclsanDeviceSyncData))) {
        ++stats_.malformedEvents;
        replayReasons_[h.launchId].insert("invalid_sync_record");
        return false;
    }
    ++syncEvents_;
    if (h.flags != ACLSAN_DEVICE_EVENT_FLAG_NONE && h.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT) {
        replayReasons_[h.launchId].insert("sync_coverage_unknown");
        return true;
    }
    if (!analysis_->Managed() && pendingSyncEvents_.size() >= analysis_->AnalysisLimits().events) {
        replayReasons_[h.launchId].insert("event_budget");
        return true;
    }
    if (!barrier) {
        const auto& sync = *static_cast<const AclsanDeviceSyncData*>(data);
        if (sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_UNKNOWN)
            replayReasons_[h.launchId].insert("unsupported_sync_kind");
        if (!analysis_->Managed())
            pendingSyncEvents_.push_back(race::NormalizeSync(sync));
    } else {
        AclsanDeviceSyncData sync{};
        sync.header = h;
        sync.scope = ACLSAN_DEVICE_SYNC_SCOPE_PIPE;
        sync.srcPipe = h.pipeline;
        sync.dstPipe = static_cast<const AclsanDeviceBarrierData*>(data)->pipe;
        if (!analysis_->Managed())
            pendingSyncEvents_.push_back(sync);
    }
    return true;
}

void Initcheck::FlushMemoryAccesses(uint64_t launchId, CheckerReportList& reports)
{
    struct Batch {
        std::vector<AclsanDeviceMemoryAccessData> memory;
        std::vector<AclsanDeviceSyncData> sync;
    };
    std::map<uint64_t, Batch> batches;
    const auto selected = [launchId](uint64_t id) { return id == launchId; };
    for (const auto& launch : launchStreams_)
        if (selected(launch.first))
            batches[launch.first];
    const auto memoryEnd = std::remove_if(
        pendingMemoryAccesses_.begin(), pendingMemoryAccesses_.end(), [&selected, &batches](const auto& a) {
            if (!selected(a.header.launchId))
                return false;
            // Dependency-only legacy records still drive the register checker.
            if (a.accessCount != 0 || a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN)
                batches[a.header.launchId].memory.push_back(a);
            else
                batches[a.header.launchId];
            return true;
        });
    pendingMemoryAccesses_.erase(memoryEnd, pendingMemoryAccesses_.end());
    const auto syncEnd =
        std::remove_if(pendingSyncEvents_.begin(), pendingSyncEvents_.end(), [&selected, &batches](const auto& s) {
            if (!selected(s.header.launchId))
                return false;
            batches[s.header.launchId].sync.push_back(s);
            return true;
        });
    pendingSyncEvents_.erase(syncEnd, pendingSyncEvents_.end());
    for (const auto& entry : unitFlagRegisters_)
        if (selected(entry.first))
            batches[entry.first];
    for (const auto& entry : replayReasons_)
        if (selected(entry.first))
            batches[entry.first];
    for (auto& entry : batches) {
        try {
            if (analysis_->Managed()) {
                auto& shared = analysis_->Get(entry.first);
                ReplayLaunch(entry.first, shared.memory, shared.sync, reports);
            } else {
                ReplayLaunch(entry.first, entry.second.memory, entry.second.sync, reports);
            }
        } catch (const std::bad_alloc&) {
            ++stats_.malformedEvents;
            ++replayFailures_["allocation_failure"];
            for (const auto& access : entry.second.memory)
                uncertainDevices_.insert(access.header.deviceId);
        }
        ClearLaunchReplayState(entry.first);
    }
}

void Initcheck::ClearLaunchReplayState(uint64_t launchId)
{
    launchStreams_.erase(launchId);
    replayReasons_.erase(launchId);
    unitFlagRegisters_.erase(launchId);
    for (auto it = initializedRegisters_.begin(); it != initializedRegisters_.end();) {
        if (std::get<1>(it->first) == launchId) {
            it = initializedRegisters_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = reportedUsages_.begin(); it != reportedUsages_.end();) {
        if (std::get<3>(std::get<0>(*it)) == launchId) {
            it = reportedUsages_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = reportGroups_.begin(); it != reportGroups_.end();) {
        if (std::get<3>(it->first) == launchId) {
            it = reportGroups_.erase(it);
        } else {
            ++it;
        }
    }
}

void Initcheck::CheckReplayEventBudget(
    uint64_t launchId, size_t memoryCount, size_t syncCount, std::set<std::string>& reasons)
{
    const auto& limits = analysis_->AnalysisLimits();
    if (memoryInvalid_)
        reasons.insert("malformed_memory_event");
    if (race::ExceedsBudget(
            limits.LaunchEventBudget(), {memoryCount, syncCount, unitFlagRegisters_[launchId].size()})) {
        if (reasons.insert("event_budget").second)
            ASCTOOL_WARNING(
                "analysis budget: launch=%llu stage=events memory=%zu sync=%zu registers=%zu limit=%zu",
                static_cast<unsigned long long>(launchId), memoryCount, syncCount, unitFlagRegisters_[launchId].size(),
                limits.LaunchEventBudget());
    }
}

void Initcheck::CheckReplayStreamOrder(
    uint64_t launchId, const std::vector<AclsanDeviceMemoryAccessData>& memory, std::set<std::string>& reasons)
{
    // A pending stream's traces may not have arrived yet. Without a stream
    // dependency, neither completion order proves which kernel initialized GM.
    const bool usesGm = std::any_of(
        memory.begin(), memory.end(), [](const auto& a) { return a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM; });
    const auto currentLaunch = launchStreams_.find(launchId);
    if (usesGm)
        for (const auto& other : launchStreams_) {
            if (other.first != launchId &&
                (currentLaunch == launchStreams_.end() || other.second != currentLaunch->second) &&
                (!analysis_->Managed() ||
                 analysis_->CompareLaunches(other.first, launchId) == race::HappensBeforeGraph::Relation::Unordered ||
                 analysis_->CompareLaunches(other.first, launchId) == race::HappensBeforeGraph::Relation::Unknown)) {
                reasons.insert("cross_stream_initialization_unknown");
                replayReasons_[other.first].insert("cross_stream_initialization_unknown");
            }
        }
}

std::optional<uint32_t> Initcheck::CheckReplayDevice(
    const std::vector<AclsanDeviceMemoryAccessData>& memory, const std::vector<AclsanDeviceSyncData>& sync,
    std::set<std::string>& reasons)
{
    std::optional<uint32_t> device;
    const auto checkDevice = [&device, &reasons](const AclsanDeviceEventHeader& h) {
        if (device && *device != h.deviceId)
            reasons.insert("device_mismatch");
        device = h.deviceId;
    };
    for (const auto& a : memory) {
        checkDevice(a.header);
        if (a.header.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT || a.vectorMask0 != 0 || a.vectorMask1 != 0 ||
            a.predicateMask0 != 0 || a.predicateMask1 != 0 ||
            (a.memoryFlags & ~static_cast<uint32_t>(ACLSAN_DEVICE_MEMORY_FLAG_HARDWARE_PADDING)) != 0U) {
            reasons.insert("memory_coverage_unknown");
        }
        // Fragment handshakes require per-fragment initialization transactions;
        // never promote their partial completion to a whole-instruction write.

        if (uncertainDevices_.count(a.header.deviceId) != 0)
            reasons.insert("prior_initialization_state_unknown");
    }
    for (const auto& s : sync)
        checkDevice(s.header);
    return device;
}

void Initcheck::CommitReplayGmWrites(
    uint64_t launchId, const std::vector<race::DependencyGraph::Access>& accesses, std::set<std::string>& reasons)
{
    // Commit device writes only once the entire launch has been validated. The
    // next synchronized launch sees these writes, never an arbitrary replay prefix.
    if (reasons.empty() && !analysis_->Managed()) {
        for (const auto& access : accesses) {
            if (access.data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM || !race::WritesMemory(access.data.accessMode))
                continue;
            // A stream completion does not establish an edge into kernels
            // already submitted on another stream. Invalidate their baseline.
            const auto current = launchStreams_.find(launchId);
            for (const auto& pending : launchStreams_)
                if (!analysis_->Managed() && pending.first != launchId &&
                    (current == launchStreams_.end() || pending.second != current->second))
                    replayReasons_[pending.first].insert("cross_stream_initialization_unknown");
            for (const auto range : access.ranges) {
                if (memoryInitcheck_.MarkInitialized(
                        {access.data.header.deviceId, 0}, range.begin, range.end - range.begin,
                        static_cast<uint8_t>(access.data.header.pipeline)) != InitCheckStatus::OK)
                    reasons.insert("write_commit_failed");
                else
                    analysis_->RecordInitialization(
                        launchId, access.data.header.deviceId, range.begin, range.end - range.begin);
            }
        }
    }
}

void Initcheck::FinishReplayLaunch(
    uint64_t launchId, const std::vector<AclsanDeviceMemoryAccessData>& memory, const std::set<std::string>& reasons,
    size_t degradedInstructions, std::vector<NpuCheckInitcheckReport>& staged, CheckerReportList& reports)
{
    if (analysis_->Managed()) {
        const auto& evidence = analysis_->Get(launchId);
        ASCTOOL_INFO(
            "initialization evidence: launch=%llu hb_queries=%zu visited_sources=%zu deferred_storage_sources=%zu",
            static_cast<unsigned long long>(launchId), evidence.initializationQueryWork,
            evidence.initializationSourcesVisited, evidence.initializationSourcesPruned);
    }
    if (!reasons.empty()) {
        ++stats_.malformedEvents;
        for (const auto& reason : reasons) {
            ++replayFailures_[reason];
            ASCTOOL_WARNING(
                "initcheck: launch=%llu analysis incomplete: %s", static_cast<unsigned long long>(launchId),
                reason.c_str());
        }
        for (const auto& a : memory)
            if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM && race::WritesMemory(a.accessMode))
                uncertainDevices_.insert(a.header.deviceId);
        return;
    }
    for (auto& report : staged) {
        if (report.common.severity == ReportSeverity::WARNING) {
            ++stats_.warnings;
        } else {
            ++stats_.errors;
        }
        reports.emplace_back(std::move(report));
    }
    if (degradedInstructions != 0) {
        degradedInstructions_ += degradedInstructions;
        replayFailures_["invalid_memory_operation"] += degradedInstructions;
    }
}

namespace {

using InvalidInstruction =
    std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t, uint32_t>;

using ReplayAccesses = std::vector<race::DependencyGraph::Access>;
using ReplayWriters = std::map<race::Storage, std::vector<size_t>>;

std::set<InvalidInstruction> CollectInvalidReplayInstructions(
    uint64_t launchId, const MemoryAnalysis& analysis, const CheckerReportList& reports)
{
    // Managed sessions query shared lifetime facts and never depend on another
    // checker's diagnostics. Retain the legacy standalone report adapter only
    // for callers that do not provide the common callback ingress.
    std::set<InvalidInstruction> invalidInstructions;
    for (const auto& report : reports) {
        if (analysis.Managed())
            break;
        const auto* bounds = std::get_if<NpuCheckMemcheckReport>(&report);
        if (bounds == nullptr || bounds->common.tool != ReportTool::MEMCHECK ||
            (bounds->common.flags & kNpuCheckReportCommonHasExecContext) == 0U ||
            (bounds->common.pattern != NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS &&
             bounds->common.pattern != NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE))
            continue;
        const auto& exec = bounds->common.exec;
        if (exec.launchId == launchId)
            invalidInstructions.emplace(
                exec.deviceId, exec.launchId, exec.blockType, exec.blockId, exec.phyCoreId, exec.instrExecId,
                exec.pipeId, exec.pc, exec.siteId);
    }
    return invalidInstructions;
}

struct ReplayAccessIndex {
    std::map<size_t, std::vector<size_t>> instructions;
    ReplayWriters writers;
};

ReplayAccessIndex IndexReplayAccesses(
    const ReplayAccesses& accesses, const std::set<InvalidInstruction>& invalidInstructions, MemoryAnalysis& analysis,
    std::set<std::string>& reasons)
{
    std::map<size_t, std::vector<size_t>> instructions;
    std::map<race::Storage, std::vector<size_t>> writers;
    for (size_t i = 0; i < accesses.size(); ++i) {
        const auto& a = accesses[i].data;
        // UB operands remain in the dependency graph, but initcheck does not check UB bytes.
        if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB)
            continue;
        const auto& h = a.header;
        if (invalidInstructions.count(
                {h.deviceId, h.launchId, h.executor.blockType, h.executor.blockId, h.executor.phyCoreId, h.instrExecId,
                 h.pipeline, h.pc, h.siteId}) != 0) {
            // An invalid GM write has unknown effects; it must never establish
            // initialized bytes for this launch or a subsequent one.
            if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM && race::WritesMemory(a.accessMode))
                reasons.insert("invalid_gm_write");
            continue;
        }
        if (analysis.Managed() && a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
            bool valid = true;
            for (auto range : accesses[i].ranges) {
                const auto status = analysis.ClassifyAccessRange(a, range.begin, range.end - range.begin).status;
                if (status == RangeStatus::AMBIGUOUS || status == RangeStatus::UNKNOWN)
                    reasons.insert("ambiguous_gm_lifetime");
                valid = valid && status == RangeStatus::VALID;
            }
            if (!valid) {
                if (race::WritesMemory(a.accessMode))
                    reasons.insert("invalid_gm_write");
                continue;
            }
        }
        instructions[accesses[i].node].push_back(i);
        if (race::WritesMemory(accesses[i].data.accessMode))
            writers[race::StorageOf(accesses[i].data)].push_back(i);
    }
    return {std::move(instructions), std::move(writers)};
}

struct ProjectedReplayOperands {
    std::vector<AclsanDeviceMemoryAccessData> operands;
    std::vector<size_t> operandSources;
    std::map<race::Storage, std::vector<size_t>> byStorage;
};

ProjectedReplayOperands ProjectReplayOperands(const ReplayAccesses& accesses, const std::vector<size_t>& indices)
{
    std::vector<AclsanDeviceMemoryAccessData> operands;
    std::vector<size_t> operandSources;
    std::map<race::Storage, std::vector<size_t>> byStorage;
    for (auto i : indices) {
        // The graph has already validated complete raw instructions. Replay
        // only the byte ranges represented by this fragment's HB node.
        for (const auto range : accesses[i].ranges) {
            auto projected = accesses[i].data;
            projected.address = range.begin;
            projected.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
            projected.layout = {};
            projected.layout.range.bytes = range.end - range.begin;
            operands.push_back(projected);
            operandSources.push_back(i);
        }
        byStorage[race::StorageOf(accesses[i].data)].push_back(i);
    }
    for (size_t i = 0; i < operands.size(); ++i) {
        operands[i].accessIndex = static_cast<uint32_t>(i);
        operands[i].accessCount = static_cast<uint32_t>(operands.size());
    }
    return {std::move(operands), std::move(operandSources), std::move(byStorage)};
}

} // namespace

void Initcheck::CommitReplayLocalWrites(
    const std::vector<race::DependencyGraph::Access>& accesses, const std::vector<size_t>& indices,
    std::set<std::string>& reasons)
{
    // Exact valid writes remain initialization facts even when the source
    // read was uninitialized or out of bounds; this is not value-taint analysis.
    // Missing operands and exhausted resources never reach this commit.
    if (reasons.empty() && !analysis_->Managed())
        for (auto i : indices) {
            const auto& access = accesses[i];
            const auto space = access.data.memorySpace;
            if (InitStateTableIndex::LocalCapacity(space) == 0 || !race::WritesMemory(access.data.accessMode))
                continue;
            const auto& h = access.data.header;
            const auto owner = access.data.memoryOwner;
            const auto key = InitStateKey::Local(h.deviceId, h.launchId, owner.blockType, owner.blockId, space);
            for (const auto& range : access.ranges) {
                const auto capacity = InitStateTableIndex::LocalCapacity(space);
                if (range.end > capacity) {
                    reasons.insert("invalid_initialization_source");
                    continue;
                }
                if (memoryInitcheck_.MarkInitialized(
                        key, range.begin, range.end - range.begin, static_cast<uint8_t>(access.data.header.pipeline)) !=
                    InitCheckStatus::OK)
                    reasons.insert("write_commit_failed");
            }
        }
}

namespace {
using race::HappensBeforeGraph;
using race::Interval;
using PaddingByRead = std::map<size_t, std::vector<Interval>>;

struct ReplayCandidateSearch {
    const ReplayAccesses& accesses;
    HappensBeforeGraph& graph;
    const race::Limits& limits;
    std::set<std::string>& reasons;
    size_t& comparisons;
    size_t& candidates;
    size_t node;
};

bool HasReplayRead(const ReplayAccesses& accesses, const std::vector<size_t>& indices)
{
    return std::any_of(indices.begin(), indices.end(), [&accesses](size_t i) {
        return race::ReadsMemory(accesses[i].data.accessMode);
    });
}

PaddingByRead FindPaddingReads(
    const ReplayAccesses& accesses, const std::vector<size_t>& readIndices, uint32_t space,
    const std::vector<Interval>& preceding, bool managed, const std::set<std::string>& reasons)
{
    std::map<size_t, std::vector<Interval>> paddingByRead;
    if (space == ACLSAN_DEVICE_MEMORY_SPACE_L1) {
        for (auto i : readIndices) {
            if (L1PaddingReadAlignment(accesses[i].data) == 0U)
                continue;
            // Padding inference is unnecessary when actual HB-before writes
            // already initialize the read's entire exact footprint.
            const bool initialized = managed && reasons.empty() && CoveredBy(accesses[i].ranges, preceding);
            if (!initialized)
                paddingByRead.emplace(i, std::vector<Interval>{});
        }
    }
    return paddingByRead;
}

bool PaddingIsSuperseded(
    const ReplayCandidateSearch& search, size_t w, const std::vector<size_t>& sourceWrites, uint64_t begin,
    uint64_t end)
{
    for (auto newer : sourceWrites) {
        if (++search.candidates > search.limits.InitializationCandidateBudget()) {
            search.reasons.insert("candidate_budget");
            break;
        }
        if (newer == w || search.accesses[newer].node == search.node ||
            !std::any_of(
                search.accesses[newer].ranges.begin(), search.accesses[newer].ranges.end(),
                [begin, end](const auto& r) { return r.begin < end && begin < r.end; })) {
            continue;
        }
        // Compare the newer write against both the producer and the read.
        constexpr size_t kComparisonsPerOverwriteCheck = 2;
        if ((search.comparisons += kComparisonsPerOverwriteCheck) > search.limits.CandidateComparisonBudget()) {
            search.reasons.insert("comparison_budget");
            break;
        }
        const auto afterProducer = search.graph.Compare(search.accesses[w].node, search.accesses[newer].node);
        const auto beforeRead = search.graph.Compare(search.accesses[newer].node, search.node);
        if (afterProducer == HappensBeforeGraph::Relation::Unknown ||
            beforeRead == HappensBeforeGraph::Relation::Unknown) {
            search.reasons.insert("graph_query_budget");
            break;
        }
        if (beforeRead == HappensBeforeGraph::Relation::Before &&
            afterProducer != HappensBeforeGraph::Relation::After) {
            return true;
        }
    }
    return false;
}

void InferReplayPadding(
    const ReplayCandidateSearch& search, size_t w, const std::vector<size_t>& sourceWrites,
    PaddingByRead& paddingByRead)
{
    for (auto& evidence : paddingByRead) {
        auto& padding = evidence.second;
        const auto& l1Read = search.accesses[evidence.first];
        if (padding.size() >= search.limits.InitializationIntervalBudget()) {
            search.reasons.insert("segment_budget");
            break;
        }
        std::vector<Interval> inferred;
        AppendL1Padding(
            l1Read, L1PaddingReadAlignment(l1Read.data), search.accesses[w].data, inferred,
            search.limits.InitializationIntervalBudget() - padding.size());
        bool superseded = false;
        if (!inferred.empty() && !search.accesses[w].ranges.empty()) {
            const uint64_t begin = search.accesses[w].ranges.front().begin;
            const uint64_t end = std::max(search.accesses[w].ranges.back().end, inferred.back().end);
            superseded = PaddingIsSuperseded(search, w, sourceWrites, begin, end);
        }
        if (!superseded) {
            padding.insert(padding.end(), inferred.begin(), inferred.end());
        }
    }
}

void ScanReplayWriters(
    const ReplayCandidateSearch& search, const std::vector<size_t>& sourceWrites,
    const std::vector<size_t>& readIndices, bool managed, std::vector<Interval>& preceding,
    PaddingByRead& paddingByRead)
{
    for (auto w : sourceWrites) {
        if (++search.candidates > search.limits.InitializationCandidateBudget()) {
            search.reasons.insert("candidate_budget");
            break;
        }
        if (search.accesses[w].node == search.node)
            continue; // Read-before-write within one instruction.
        if (++search.comparisons > search.limits.CandidateComparisonBudget()) {
            search.reasons.insert("comparison_budget");
            break;
        }
        const auto relation = search.graph.Compare(search.accesses[w].node, search.node);
        if (relation == HappensBeforeGraph::Relation::Unknown) {
            search.reasons.insert("graph_query_budget");
            break;
        }
        if (relation == HappensBeforeGraph::Relation::Before) {
            if (!managed) {
                preceding.insert(preceding.end(), search.accesses[w].ranges.begin(), search.accesses[w].ranges.end());
            }
            InferReplayPadding(search, w, sourceWrites, paddingByRead);
        } else if (relation == HappensBeforeGraph::Relation::Unordered) {
            for (auto r : readIndices)
                if (race::ReadsMemory(search.accesses[r].data.accessMode) &&
                    Overlap(search.accesses[w].ranges, search.accesses[r].ranges))
                    search.reasons.insert("unordered_initialization");
        }
        if (preceding.size() > search.limits.InitializationIntervalBudget()) {
            search.reasons.insert("segment_budget");
            break;
        }
    }
}

InitStateKey ReplayInitializationKey(const AclsanDeviceMemoryAccessData& access, bool managed)
{
    const auto& h = access.header;
    if (access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
        return {h.deviceId, 0};
    }
    const auto& owner = access.memoryOwner;
    if (managed) {
        return InitStateKey::PhysicalLocal(
            h.deviceId, h.launchId, owner.blockType, owner.phyCoreId, access.memorySpace);
    }
    return InitStateKey::Local(h.deviceId, h.launchId, owner.blockType, owner.blockId, access.memorySpace);
}

const std::vector<AclsanDeviceMemoryAccessData>& SelectReplayStorageOperands(
    const std::vector<AclsanDeviceMemoryAccessData>& operands, const race::Storage& storage, bool managed,
    std::vector<AclsanDeviceMemoryAccessData>& selected)
{
    if (!managed) {
        return operands;
    }
    for (const auto& operand : operands) {
        if (race::StorageOf(operand) == storage) {
            selected.push_back(operand);
        }
    }
    for (size_t i = 0; i < selected.size(); ++i) {
        selected[i].accessIndex = static_cast<uint32_t>(i);
        selected[i].accessCount = static_cast<uint32_t>(selected.size());
    }
    return selected;
}

void ApplyReplayPadding(
    const ReplayAccesses& accesses, const std::vector<Interval>& preceding, const std::vector<size_t>& operandSources,
    PaddingByRead& paddingByRead, std::vector<AclsanDeviceMemoryAccessData>& operands)
{
    for (auto& evidence : paddingByRead) {
        auto& padding = evidence.second;
        if (padding.empty()) {
            continue;
        }
        padding.insert(padding.end(), preceding.begin(), preceding.end());
        MergeIntervals(padding);
        // Padding changes severity only; it never initializes the bytes.
        if (CoveredBy(accesses[evidence.first].ranges, padding)) {
            for (size_t i = 0; i < operands.size(); ++i)
                if (operandSources[i] == evidence.first) {
                    operands[i].memoryFlags |= ACLSAN_DEVICE_MEMORY_FLAG_HARDWARE_PADDING;
                }
        }
    }
}

} // namespace

bool Initcheck::StageReplayReports(
    const InitCheckResult& result, size_t reportBegin, std::vector<NpuCheckInitcheckReport>& staged,
    std::set<std::string>& reasons, bool& instructionDegraded)
{
    if (result.status != InitCheckStatus::OK) {
        if (result.status == InitCheckStatus::INVALID_ALLOCATION) {
            // A known invalid range blocks read verdicts, not the write facts used by later instructions.
            instructionDegraded = true;
        } else {
            reasons.insert(
                result.status == InitCheckStatus::RESOURCE_EXHAUSTED ? "initialization_resource_exhausted" :
                                                                       "invalid_memory_record");
        }
        staged.resize(reportBegin);
        return false;
    }
    const uint64_t group = nextGroupId_++;
    for (auto report : result.reports) {
        report.common.reportId = nextReportId_++;
        report.common.groupId = group;
        staged.emplace_back(std::move(report));
    }
    return true;
}

void Initcheck::ReplayLaunch(
    uint64_t launchId, std::vector<AclsanDeviceMemoryAccessData>& memory, std::vector<AclsanDeviceSyncData>& sync,
    CheckerReportList& reports)
{
    using namespace race;
    auto& reasons = replayReasons_[launchId];
    size_t degradedInstructions = 0;
    const auto& limits = analysis_->AnalysisLimits();
    CheckReplayEventBudget(launchId, memory.size(), sync.size(), reasons);
    CheckReplayStreamOrder(launchId, memory, reasons);
    const auto device = CheckReplayDevice(memory, sync, reasons);
    struct LocalTablesGuard {
        MemoryInitcheck& checker;
        uint32_t device;
        uint64_t launch;
        ~LocalTablesGuard() { checker.ResetLaunch(device, launch); }
    } localTablesGuard{memoryInitcheck_, device.value_or(0), launchId};
    std::unique_ptr<DependencyGraph> ownedDependency;
    DependencyGraph* dependency = nullptr;
    if (analysis_->Managed()) {
        dependency = &analysis_->Graph(launchId, limits, reasons);
    } else {
        ownedDependency = std::make_unique<DependencyGraph>(limits);
        dependency = ownedDependency.get();
        if (reasons.empty()) {
            std::vector<AclsanDeviceVfData> vfs;
            BuildDependencyGraph(memory, sync, {}, limits, reasons, *dependency, unitFlagRegisters_[launchId], vfs, {});
        }
    }
    auto& graph = dependency->graph;
    graph.BeginAnalysis(limits.ReachabilityPhaseWorkBudget());
    auto& accesses = dependency->accesses;
    const auto invalidInstructions = CollectInvalidReplayInstructions(launchId, *analysis_, reports);
    auto [instructions, writers] = IndexReplayAccesses(accesses, invalidInstructions, *analysis_, reasons);
    std::vector<NpuCheckInitcheckReport> staged;
    size_t comparisons = 0, candidates = 0;
    for (auto node : graph.Order()) {
        if (!reasons.empty())
            break;
        const auto instruction = instructions.find(node);
        if (instruction == instructions.end())
            continue;
        auto [operands, operandSources, byStorage] = ProjectReplayOperands(accesses, instruction->second);
        const auto instructionReportsBegin = staged.size();
        bool instructionDegraded = false;
        for (const auto& storage : byStorage) {
            const auto space = std::get<0>(storage.first);
            if (space != ACLSAN_DEVICE_MEMORY_SPACE_GM && InitStateTableIndex::LocalCapacity(space) == 0)
                continue;
            std::vector<Interval> preceding;
            if (analysis_->Managed())
                preceding = analysis_->PrecedingWrites(launchId, node, storage.first, reasons);
            auto paddingByRead =
                FindPaddingReads(accesses, storage.second, space, preceding, analysis_->Managed(), reasons);
            ReplayCandidateSearch search{accesses, graph, limits, reasons, comparisons, candidates, node};
            if (HasReplayRead(accesses, storage.second) && (!analysis_->Managed() || !paddingByRead.empty())) {
                ScanReplayWriters(
                    search, writers[storage.first], storage.second, analysis_->Managed(), preceding, paddingByRead);
            }
            if (!reasons.empty())
                break;
            MergeIntervals(preceding);
            ApplyReplayPadding(accesses, preceding, operandSources, paddingByRead, operands);
            MemoryInitcheck::InitializedRanges initialized;
            for (auto range : preceding)
                initialized.emplace_back(range.begin, range.end);
            const auto key = ReplayInitializationKey(accesses[storage.second.front()].data, analysis_->Managed());
            MemoryInitcheck::RangeClassifier classifier;
            if (analysis_->Managed())
                classifier = [this, launchId, device](uint64_t address, uint64_t bytes) {
                    return analysis_->Classify(launchId, {*device, address, bytes});
                };
            // The graph validates the full instruction before replay groups its operands by storage.
            std::vector<AclsanDeviceMemoryAccessData> selectedOperands;
            const auto& storageOperands =
                SelectReplayStorageOperands(operands, storage.first, analysis_->Managed(), selectedOperands);
            const auto result = memoryInitcheck_.CheckInstruction(
                key, storageOperands, initialized, false,
                space != ACLSAN_DEVICE_MEMORY_SPACE_GM || analysis_->Managed(), classifier);
            if (!StageReplayReports(result, instructionReportsBegin, staged, reasons, instructionDegraded)) {
                break;
            }
        }
        if (instructionDegraded) {
            ++degradedInstructions;
        }
        CommitReplayLocalWrites(accesses, instruction->second, reasons);
    }
    CommitReplayGmWrites(launchId, accesses, reasons);
    FinishReplayLaunch(launchId, memory, reasons, degradedInstructions, staged, reports);
}

namespace {

// Owner metadata is part of the required callback schema. Reject older,
// shorter records before copying or consulting the appended owner extension.
constexpr size_t kMemoryAccessDependencySize = sizeof(AclsanDeviceMemoryAccessData);
static_assert(ACLSAN_DEVICE_REGISTER_COUNT < 64);
constexpr uint64_t kSupportedRegisterMask = (UINT64_C(1) << static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_COUNT)) - 1;

uint64_t TimestampNs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
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
        case ACLSAN_DEVICE_PIPE_FIXPIPE:
            return "FIXPIPE";
        default:
            return "UNKNOWN";
    }
}

struct RegisterDescription {
    const char* name;
    const char* setter;
};

RegisterDescription DescribeRegister(uint32_t id)
{
    static constexpr RegisterDescription descriptions[] = {
        {"vector_mask", "set_vector_mask"},
        {"mte2_source", "set_mte2_src_para"},
        {"nddma_pad_count", "set_pad_cnt_nddma"},
        {"nddma_loop0_stride", "set_loop0_stride_nddma"},
        {"nddma_loop1_stride", "set_loop1_stride_nddma"},
        {"nddma_loop2_stride", "set_loop2_stride_nddma"},
        {"nddma_loop3_stride", "set_loop3_stride_nddma"},
        {"nddma_loop4_stride", "set_loop4_stride_nddma"},
        {"mte2_nz", "set_mte2_nz_para"},
        {"loop3", "set_loop3_para"},
        {"dma_loop_size_ubuf_to_gm", "set_loop_size_ubtoout"},
        {"dma_loop_size_gm_to_ubuf", "set_loop_size_outtoub"},
        {"dma_loop_size_gm_to_cbuf", "set_loop_size_outtol1"},
        {"dma_loop1_stride_ubuf_to_gm", "set_loop1_stride_ubtoout"},
        {"dma_loop2_stride_ubuf_to_gm", "set_loop2_stride_ubtoout"},
        {"dma_loop1_stride_gm_to_ubuf", "set_loop1_stride_outtoub"},
        {"dma_loop2_stride_gm_to_ubuf", "set_loop2_stride_outtoub"},
        {"dma_loop1_stride_gm_to_cbuf", "set_loop1_stride_outtol1"},
        {"dma_loop2_stride_gm_to_cbuf", "set_loop2_stride_outtol1"},
        {"set_padding", "set_padding"},
        {"ctrl", "set_ctrl"},
        {"channel_para", "set_channel_para"},
        {"fpc", "set_fpc"},
    };
    static_assert(std::size(descriptions) == ACLSAN_DEVICE_REGISTER_COUNT);
    return id < std::size(descriptions) ? descriptions[id] : RegisterDescription{"unknown", "unknown"};
}

const char* ConsumerInstructionName(uint32_t id)
{
    using aclsan::InstructionId;
    struct InstructionName {
        InstructionId id;
        const char* name;
    };
    static constexpr InstructionName names[] = {
        {InstructionId::LoadGmToCbuf2DV2, "load_gm_to_cbuf_2dv2"},
        {InstructionId::CopyGmToCbufV2, "copy_gm_to_cbuf_v2"},
        {InstructionId::CopyGmToCbufAlignV2B8, "copy_gm_to_cbuf_align_v2"},
        {InstructionId::CopyGmToCbufAlignV2B16, "copy_gm_to_cbuf_align_v2"},
        {InstructionId::CopyGmToCbufAlignV2B32, "copy_gm_to_cbuf_align_v2"},
        {InstructionId::CopyGmToCbufMultiNd2NzB8, "copy_gm_to_cbuf_multi_nd2nz"},
        {InstructionId::CopyGmToCbufMultiNd2NzB16, "copy_gm_to_cbuf_multi_nd2nz"},
        {InstructionId::CopyGmToCbufMultiNd2NzB32, "copy_gm_to_cbuf_multi_nd2nz"},
        {InstructionId::CopyGmToCbufMultiDn2NzB8, "copy_gm_to_cbuf_multi_dn2nz"},
        {InstructionId::CopyGmToCbufMultiDn2NzB16, "copy_gm_to_cbuf_multi_dn2nz"},
        {InstructionId::CopyGmToCbufMultiDn2NzB32, "copy_gm_to_cbuf_multi_dn2nz"},
        {InstructionId::CopyUbufToGmAlignV2, "copy_ubuf_to_gm_align_v2"},
        {InstructionId::CopyGmToUbufAlignV2B8, "copy_gm_to_ubuf_align_v2"},
        {InstructionId::CopyGmToUbufAlignV2B16, "copy_gm_to_ubuf_align_v2"},
        {InstructionId::CopyGmToUbufAlignV2B32, "copy_gm_to_ubuf_align_v2"},
        {InstructionId::NdDmaOutToUbufB8, "nd_copy_gm_to_ubuf"},
        {InstructionId::NdDmaOutToUbufB16, "nd_copy_gm_to_ubuf"},
        {InstructionId::NdDmaOutToUbufB32, "nd_copy_gm_to_ubuf"},
        {InstructionId::FixL0cToOutF32, "fix_l0c_to_out"},
        {InstructionId::FixL0cToOutS32, "fix_l0c_to_out"},
    };
    for (const auto& entry : names) {
        if (static_cast<uint32_t>(entry.id) == id) {
            return entry.name;
        }
    }
    return "unknown";
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
    exec.pipeName = PipelineName(data.header.pipeline);
    return exec;
}

} // namespace

const std::vector<CallbackSpec>& Initcheck::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> callbacks{
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC},
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_TRANSFER_END},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_VF},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION, ACLSAN_CBID_OPERATOR_EXECUTION_COMPLETE},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END}};
    return callbacks;
}

bool Initcheck::OnHostWrite(const AclsanMemoryWriteData& data)
{
    if (data.common.version != ACLSAN_API_VERSION || data.common.size < sizeof(data)) {
        ++stats_.malformedEvents;
        return false;
    }
    if (data.common.result != 0) {
        ++stats_.failedHostWrites;
        return true;
    }
    if (data.bytes == 0) {
        return true;
    }
    // Device traces arrive at synchronization. A later Host write must not
    // initialize an earlier read retroactively. Without a dependency proof,
    // mark the analysis incomplete instead of silently accepting this order.
    if (!analysis_->Managed() && (!pendingLaunchStreams_.empty() || !pendingMemoryAccesses_.empty())) {
        ++stats_.malformedEvents;
        ASCTOOL_WARNING("initcheck: Host write overlaps pending device work; initialization order is unknown");
        return false;
    }
    if (analysis_->Managed()) {
        if (!analysis_->HostWriteRecorded())
            ++stats_.malformedEvents;
        return analysis_->HostWriteRecorded();
    }
    const auto status =
        memoryInitcheck_.MarkInitialized({data.deviceId, 0}, reinterpret_cast<uint64_t>(data.destination), data.bytes);
    if (status != InitCheckStatus::OK) {
        ++stats_.malformedEvents;
        ASCTOOL_WARNING("initcheck: cannot record completed Host write, status=%u", static_cast<unsigned>(status));
        return false;
    }
    analysis_->RecordInitialization(0, data.deviceId, reinterpret_cast<uint64_t>(data.destination), data.bytes);
    return true;
}

void Initcheck::OnAllocation(const AclsanResourceData& data)
{
    if (data.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE || data.common.result != 0) {
        return;
    }
    if (analysis_->Managed())
        return;
    (void)memoryInitcheck_.RegisterAllocation(
        {data.deviceId, 0}, data.resourceId, reinterpret_cast<uint64_t>(data.ptr), data.bytes);
}

void Initcheck::OnFree(const AclsanResourceData& data)
{
    if (data.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE || data.common.result != 0) {
        return;
    }
    if (analysis_->Managed())
        return;
    (void)memoryInitcheck_.ReleaseAllocation({data.deviceId, 0}, data.resourceId, reinterpret_cast<uint64_t>(data.ptr));
}

bool Initcheck::OnRegisterState(const AclsanDeviceRegisterStateData& data)
{
    if (data.header.version != ACLSAN_API_VERSION || data.header.size < sizeof(data) ||
        data.regId >= ACLSAN_DEVICE_REGISTER_COUNT || data.regId >= std::numeric_limits<uint64_t>::digits) {
        ++stats_.malformedEvents;
        memoryInvalid_ = true;
        return false;
    }
    const CoreKey key{
        data.header.deviceId, data.header.launchId, data.header.executor.blockType, data.header.executor.blockId};
    initializedRegisters_[key] |= UINT64_C(1) << data.regId;
    if (data.regId == ACLSAN_DEVICE_REGISTER_FPC) {
        constexpr size_t MAX_UNIT_FLAG_REGISTERS = 100000;
        auto& registers = unitFlagRegisters_[data.header.launchId];
        if (registers.size() >= MAX_UNIT_FLAG_REGISTERS)
            replayReasons_[data.header.launchId].insert("event_budget");
        else if (!analysis_->Managed())
            registers.push_back(data);
    }
    ++stats_.registerWrites;
    return true;
}

NpuCheckInitcheckReport Initcheck::MakeReport(
    const AclsanDeviceMemoryAccessData& data, uint32_t registerId, uint64_t groupId)
{
    const RegisterDescription description = DescribeRegister(registerId);
    NpuCheckInitcheckReport report{};
    report.common.reportId = nextReportId_++;
    report.common.groupId = groupId;
    report.common.timestampNs = TimestampNs();
    report.common.tool = ReportTool::INITCHECK;
    report.common.severity = ReportSeverity::ERROR;
    report.common.pattern = NpuCheckReportPattern::INITCHECK_UNINITIALIZED_REGISTER_USE;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    report.common.exec = ToExecContext(data);
    report.registerId = registerId;
    report.registerName = description.name;
    report.setterInstruction = description.setter;
    report.consumerInstruction = ConsumerInstructionName(data.instructionId);
    return report;
}

static bool IsMissingRegisterPlaceholder(const AclsanDeviceMemoryAccessData& data, uint64_t missing)
{
    // These GM-to-UB/L1 transfers can emit a single address-less record when a required register is missing.
    using aclsan::InstructionId;
    const bool nddma = data.instructionId == static_cast<uint32_t>(InstructionId::NdDmaOutToUbufB8) ||
                       data.instructionId == static_cast<uint32_t>(InstructionId::NdDmaOutToUbufB16) ||
                       data.instructionId == static_cast<uint32_t>(InstructionId::NdDmaOutToUbufB32);
    const bool gmToL1 = data.instructionId == static_cast<uint32_t>(InstructionId::CopyGmToCbufAlignV2B8) ||
                        data.instructionId == static_cast<uint32_t>(InstructionId::CopyGmToCbufAlignV2B16) ||
                        data.instructionId == static_cast<uint32_t>(InstructionId::CopyGmToCbufAlignV2B32);
    return (nddma || gmToL1) && missing != 0 && data.header.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED &&
           data.accessCount == 1 && data.accessIndex == 0 && data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN &&
           data.layoutKind == 0 && data.accessMode == 0 && data.address == 0 && data.vectorMask0 == 0 &&
           data.vectorMask1 == 0 && data.predicateMask0 == 0 && data.predicateMask1 == 0 && data.memoryFlags == 0 &&
           data.unitFlag == 0;
}

bool Initcheck::OnMemoryAccess(const AclsanDeviceMemoryAccessData& data, CheckerReportList& reports)
{
    if (data.header.version != ACLSAN_API_VERSION || data.header.size < kMemoryAccessDependencySize ||
        data.regDependencyMask1 != 0 || (data.regDependencyMask0 & ~kSupportedRegisterMask) != 0) {
        ++stats_.malformedEvents;
        memoryInvalid_ = true;
        replayReasons_[data.header.launchId].insert("malformed_memory_event");
        return false;
    }
    ++stats_.deviceOperations;
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0A || data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0B)
        ++l0Operations_;
    if (data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB) {
        // UB and L1 operands now share the issuing core's instruction identity.
        // Preserve multi-operand instructions for dependency-graph validation.
        if (!analysis_->Managed() && data.accessCount > 1)
            pendingMemoryAccesses_.push_back(data);
        return true;
    }
    const CoreKey core{
        data.header.deviceId, data.header.launchId, data.header.executor.blockType, data.header.executor.blockId};
    const uint64_t initialized = initializedRegisters_[core];
    const uint64_t missing = data.regDependencyMask0 & ~initialized;
    const bool missingRegisterPlaceholder = IsMissingRegisterPlaceholder(data, missing);
    if (missingRegisterPlaceholder) {
        // Retain a dependency-only marker so synchronization also closes this
        // launch's register state when no address or sync records were emitted.
        auto registerOnly = data;
        registerOnly.accessCount = 0;
        if (!analysis_->Managed()) {
            pendingMemoryAccesses_.push_back(registerOnly);
        }
    } else {
        if ((data.header.flags & INCOMPLETE_FLAGS) != 0U) {
            replayReasons_[data.header.launchId].insert("memory_coverage_unknown");
        }
        // Keep all operands until synchronization so a GM instruction with multiple
        // operands can be checked atomically (reads before writes).
        if (!analysis_->Managed())
            pendingMemoryAccesses_.push_back(data);
    }
    if (missing == 0) {
        return true;
    }

    if (data.accessCount != 0 && !missingRegisterPlaceholder) {
        replayReasons_[data.header.launchId].insert("uninitialized_register_dependency");
    }
    ReportMissingRegisters(data, missing, reports);
    return true;
}

void Initcheck::ReportMissingRegisters(
    const AclsanDeviceMemoryAccessData& data, uint64_t missing, CheckerReportList& reports)
{
    const InstructionKey instruction = race::Key(data);
    const auto [group, inserted] = reportGroups_.try_emplace(instruction, nextGroupId_);
    if (inserted) {
        ++nextGroupId_;
    }
    while (missing != 0) {
        const uint32_t registerId = static_cast<uint32_t>(__builtin_ctzll(missing));
        missing &= missing - 1;
        const UsageKey usage{instruction, registerId};
        if (!reportedUsages_.insert(usage).second) {
            continue;
        }
        reports.emplace_back(MakeReport(data, registerId, group->second));
        ++stats_.errors;
    }
}

void Initcheck::OnSynchronization(const AclsanSynchronizeData& data)
{
    ++stats_.synchronizations;
    if (data.common.result != 0)
        ++stats_.failedSynchronizations;
}

bool Initcheck::OnDeviceCallback(AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    switch (cbid) {
        case ACLSAN_CBID_DEVICE_SYNC:
        case ACLSAN_CBID_DEVICE_PIPE_BARRIER:
            if (!OnDeviceSync(cbid, data)) {
                memoryInvalid_ = true;
                return false;
            }
            return true;
        case ACLSAN_CBID_DEVICE_STATE:
            return OnRegisterState(*static_cast<const AclsanDeviceRegisterStateData*>(data));
        case ACLSAN_CBID_DEVICE_MEMORY_ACCESS:
            return OnMemoryAccess(*static_cast<const AclsanDeviceMemoryAccessData*>(data), reports);
        default:
            return true;
    }
}

bool Initcheck::OnOperatorExecution(const AclsanOperatorExecutionData& event, CheckerReportList& reports)
{
    const auto launch = launchStreams_.find(event.launchId);
    if (event.common.version != ACLSAN_API_VERSION || event.common.size < sizeof(event) ||
        launch == launchStreams_.end() || launch->second != event.stream) {
        ++stats_.malformedEvents;
        memoryInvalid_ = true;
        return false;
    }
    if (event.common.result != 0 || event.flags != ACLSAN_OPERATOR_EXECUTION_FLAG_NONE)
        replayReasons_[event.launchId].insert("operator_execution_incomplete");
    FlushMemoryAccesses(event.launchId, reports);
    const auto sameStream = [&event](const auto& entry) { return entry.second == event.stream; };
    if (std::none_of(launchStreams_.begin(), launchStreams_.end(), sameStream))
        pendingLaunchStreams_.erase(event.stream);
    return true;
}

bool Initcheck::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION) {
        return OnDeviceCallback(cbid, data, reports);
    }
    if (domain == ACLSAN_CB_DOMAIN_MEMORY && analysis_->Managed()) {
        const auto& common = *static_cast<const AclsanCallbackCommonData*>(data);
        if (!analysis_->IsLegacyMemcpy(cbid, common))
            OnHostMemory(common, reports);
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_MEMORY && cbid == ACLSAN_CBID_MEMORY_TRANSFER_END) {
        ++stats_.malformedEvents;
        return false;
    }
    if (domain == ACLSAN_CB_DOMAIN_MEMORY && cbid == ACLSAN_CBID_MEMORY_WRITE_END) {
        return OnHostWrite(*static_cast<const AclsanMemoryWriteData*>(data));
    }
    if (domain == ACLSAN_CB_DOMAIN_RESOURCE) {
        const auto* event = static_cast<const AclsanResourceData*>(data);
        if (cbid == ACLSAN_CBID_RESOURCE_MEMORY_ALLOC) {
            OnAllocation(*event);
        } else if (cbid == ACLSAN_CBID_RESOURCE_MEMORY_FREE) {
            OnFree(*event);
        }
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto* event = static_cast<const AclsanLaunchData*>(data);
        if (event->common.result != 0) {
            ++stats_.failedLaunches;
        } else {
            pendingLaunchStreams_.insert(event->stream);
            launchStreams_[event->launchId] = event->stream;
        }
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_OPERATOR_EXECUTION) {
        return OnOperatorExecution(*static_cast<const AclsanOperatorExecutionData*>(data), reports);
    }
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE)
        OnSynchronization(*static_cast<const AclsanSynchronizeData*>(data));
    return true;
}

InitcheckStats Initcheck::Stats() const { return stats_; }

void Initcheck::Finalize([[maybe_unused]] CheckerReportList& reports)
{
    if (!pendingLaunchStreams_.empty() || !pendingMemoryAccesses_.empty())
        memoryInvalid_ = true;
}

bool Initcheck::HasErrors() const { return stats_.errors != 0; }

bool Initcheck::AnalysisComplete() const
{
    return pendingLaunchStreams_.empty() && pendingMemoryAccesses_.empty() && pendingSyncEvents_.empty() &&
           unitFlagRegisters_.empty() && replayReasons_.empty() && replayFailures_.empty() &&
           stats_.malformedEvents == 0 && stats_.failedLaunches == 0 && stats_.failedSynchronizations == 0 &&
           stats_.failedHostWrites == 0 && !memoryInvalid_;
}

std::string Initcheck::Summary() const
{
    std::ostringstream output;
    output << "tool=initcheck register_writes=" << stats_.registerWrites
           << " device_operations=" << stats_.deviceOperations << " synchronizations=" << stats_.synchronizations
           << " errors=" << stats_.errors << " warnings=" << stats_.warnings
           << " malformed_events=" << stats_.malformedEvents << " failed_launches=" << stats_.failedLaunches
           << " failed_synchronizations=" << stats_.failedSynchronizations
           << " pending_memory_operations=" << pendingMemoryAccesses_.size()
           << " failed_host_writes=" << stats_.failedHostWrites
           << " pending_launch_streams=" << pendingLaunchStreams_.size();
    output << " sync_events=" << syncEvents_ << " pending_sync_events=" << pendingSyncEvents_.size();
    output << " l0_memory_operations=" << l0Operations_
           << " incomplete_memory_operations=" << (replayFailures_.size() + (memoryInvalid_ ? 1 : 0))
           << " degraded_instructions=" << degradedInstructions_;
    for (const auto& failure : replayFailures_)
        output << " " << failure.first << "=" << failure.second;
    return output.str();
}

} // namespace npucheck
