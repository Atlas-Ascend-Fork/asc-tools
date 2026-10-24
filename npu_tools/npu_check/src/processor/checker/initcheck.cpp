/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/initcheck.h"
#include "checker/initcheck_report.h"
#include "checker/memory_access_layout.h"
#include "checker/racecheck.h"
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
constexpr uint64_t kMask = InitStateTable::kPageBytes - 1;
} // namespace

GmInitStateTable::GmInitStateTable() : root_(std::make_unique<Root>()) {}

bool GmInitStateTable::ValidRange(uint64_t address, uint64_t bytes)
{
    return address < kAddressLimit && bytes <= kAddressLimit - address;
}

void GmInitStateTable::ValidateRange(uint64_t address, uint64_t bytes) const
{
    if (!ValidRange(address, bytes))
        throw std::out_of_range("GM initialization range exceeds 48-bit address space");
}

const GmInitStateTable::Leaf* GmInitStateTable::FindLeaf(uint64_t address) const
{
    const auto& middle = (*root_)[address >> 32];
    return middle ? (*middle)[(address >> 16) & kMask].get() : nullptr;
}

std::unique_ptr<GmInitStateTable::Leaf>& GmInitStateTable::LeafSlot(uint64_t address)
{
    auto& middle = (*root_)[address >> 32];
    if (!middle)
        middle = std::make_unique<Middle>();
    return (*middle)[(address >> 16) & kMask];
}

InitStateQuery GmInitStateTable::Query(uint64_t address, uint64_t bytes) const
{
    ValidateRange(address, bytes);
    InitStateQuery result;
    while (bytes != 0) {
        const Leaf* leaf = FindLeaf(address);
        const uint64_t offset = address & kMask;
        const uint64_t count = std::min(bytes, kPageBytes - offset);
        if (!leaf || leaf->writtenBytes == 0) {
            if (result.uninitializedBytes == 0)
                result.firstUninitializedAddress = address;
            result.uninitializedBytes += count;
        } else if (leaf->writtenBytes == kPageBytes) {
            // A fully written page cannot contain an uninitialized byte.
        } else if (offset == 0 && count == kPageBytes) {
            // Allocation-level reports need the count, not a byte-by-byte scan
            // of every full page. Only locate the first missing byte once.
            if (result.uninitializedBytes == 0) {
                const auto first = std::find_if(leaf->states.begin(), leaf->states.end(), [](uint8_t state) {
                    return !detail::InitWritten(state);
                });
                result.firstUninitializedAddress = address + (first - leaf->states.begin());
            }
            result.uninitializedBytes += kPageBytes - leaf->writtenBytes;
        } else {
            for (uint64_t i = 0; i < count; ++i) {
                if (!detail::InitWritten(leaf->states[offset + i])) {
                    if (result.uninitializedBytes == 0)
                        result.firstUninitializedAddress = address + i;
                    ++result.uninitializedBytes;
                }
            }
        }
        address += count;
        bytes -= count;
    }
    return result;
}

void GmInitStateTable::Write(uint64_t address, uint64_t bytes, uint8_t pipe)
{
    ValidateRange(address, bytes);
    if (pipe > 15)
        throw std::invalid_argument("GM initialization pipeline exceeds four bits");
    // 先完成全部页的分配，分配失败时不留下只更新一半的 WRITE 状态。
    for (uint64_t current = address, remaining = bytes; remaining != 0;) {
        auto& leaf = LeafSlot(current);
        if (!leaf) {
            leaf = std::make_unique<Leaf>();
            ++leafCount_;
        }
        const uint64_t count = std::min(remaining, kPageBytes - (current & kMask));
        current += count;
        remaining -= count;
    }
    while (bytes != 0) {
        auto& leaf = LeafSlot(address);
        const uint64_t offset = address & kMask;
        const uint64_t count = std::min(bytes, kPageBytes - offset);
        for (uint64_t i = 0; i < count; ++i) {
            auto& state = leaf->states[offset + i];
            if (!detail::InitWritten(state))
                ++leaf->writtenBytes;
            state = detail::kInitWrite | pipe;
        }
        address += count;
        bytes -= count;
    }
}

void GmInitStateTable::Clear(uint64_t address, uint64_t bytes)
{
    ValidateRange(address, bytes);
    while (bytes != 0) {
        // 跳过不存在的 4GB 目录，释放巨大但未展开的分配不逐字节遍历。
        if (!(*root_)[address >> 32]) {
            const uint64_t count = std::min(bytes, (UINT64_C(1) << 32) - (address & UINT64_C(0xffffffff)));
            address += count;
            bytes -= count;
            continue;
        }
        auto& leaf = LeafSlot(address);
        const uint64_t offset = address & kMask;
        const uint64_t count = std::min(bytes, kPageBytes - offset);
        if (leaf) {
            for (uint64_t i = 0; i < count; ++i) {
                auto& state = leaf->states[offset + i];
                if (detail::InitWritten(state))
                    --leaf->writtenBytes;
                state = 0;
            }
            if (leaf->writtenBytes == 0) {
                leaf.reset();
                --leafCount_;
            }
        }
        address += count;
        bytes -= count;
    }
    // 根目录保留；空中间目录在作用域销毁时释放。
}

LocalInitStateTable::LocalInitStateTable(uint64_t capacity)
    : capacity_(capacity), pages_((capacity + kPageBytes - 1) / kPageBytes)
{}

void LocalInitStateTable::ValidateRange(uint64_t address, uint64_t bytes) const
{
    if (address > capacity_ || bytes > capacity_ - address)
        throw std::out_of_range("Local initialization range exceeds memory capacity");
}

const LocalInitStateTable::Leaf* LocalInitStateTable::FindLeaf(uint64_t address) const
{
    return pages_[address >> 16].get();
}

std::unique_ptr<LocalInitStateTable::Leaf>& LocalInitStateTable::LeafSlot(uint64_t address)
{
    return pages_[address >> 16];
}

InitStateQuery LocalInitStateTable::Query(uint64_t address, uint64_t bytes) const
{
    ValidateRange(address, bytes);
    InitStateQuery result;
    while (bytes != 0) {
        const Leaf* leaf = FindLeaf(address);
        const uint64_t offset = address & kMask;
        const uint64_t count = std::min(bytes, kPageBytes - offset);
        if (!leaf || leaf->writtenBytes == 0) {
            if (result.uninitializedBytes == 0)
                result.firstUninitializedAddress = address;
            result.uninitializedBytes += count;
        } else if (leaf->writtenBytes == kPageBytes) {
            // A fully written page cannot contain an uninitialized byte.
        } else if (offset == 0 && count == kPageBytes) {
            // Allocation-level reports need the count, not a byte-by-byte scan
            // of every full page. Only locate the first missing byte once.
            if (result.uninitializedBytes == 0) {
                const auto first = std::find_if(leaf->states.begin(), leaf->states.end(), [](uint8_t state) {
                    return !detail::InitWritten(state);
                });
                result.firstUninitializedAddress = address + (first - leaf->states.begin());
            }
            result.uninitializedBytes += kPageBytes - leaf->writtenBytes;
        } else {
            for (uint64_t i = 0; i < count; ++i) {
                if (!detail::InitWritten(leaf->states[offset + i])) {
                    if (result.uninitializedBytes == 0)
                        result.firstUninitializedAddress = address + i;
                    ++result.uninitializedBytes;
                }
            }
        }
        address += count;
        bytes -= count;
    }
    return result;
}

void LocalInitStateTable::Write(uint64_t address, uint64_t bytes, uint8_t pipe)
{
    ValidateRange(address, bytes);
    if (pipe > 15)
        throw std::invalid_argument("Local initialization pipeline exceeds four bits");
    // 先完成全部页的分配，分配失败时不留下只更新一半的 WRITE 状态。
    for (uint64_t current = address, remaining = bytes; remaining != 0;) {
        auto& leaf = LeafSlot(current);
        if (!leaf) {
            leaf = std::make_unique<Leaf>();
            ++leafCount_;
        }
        const uint64_t count = std::min(remaining, kPageBytes - (current & kMask));
        current += count;
        remaining -= count;
    }
    while (bytes != 0) {
        auto& leaf = LeafSlot(address);
        const uint64_t offset = address & kMask;
        const uint64_t count = std::min(bytes, kPageBytes - offset);
        for (uint64_t i = 0; i < count; ++i) {
            auto& state = leaf->states[offset + i];
            if (!detail::InitWritten(state))
                ++leaf->writtenBytes;
            state = detail::kInitWrite | pipe;
        }
        address += count;
        bytes -= count;
    }
}

void LocalInitStateTable::Clear(uint64_t address, uint64_t bytes)
{
    ValidateRange(address, bytes);
    while (bytes != 0) {
        auto& leaf = LeafSlot(address);
        const uint64_t offset = address & kMask;
        const uint64_t count = std::min(bytes, kPageBytes - offset);
        if (leaf) {
            for (uint64_t i = 0; i < count; ++i) {
                auto& state = leaf->states[offset + i];
                if (detail::InitWritten(state))
                    --leaf->writtenBytes;
                state = 0;
            }
            if (leaf->writtenBytes == 0) {
                leaf.reset();
                --leafCount_;
            }
        }
        address += count;
        bytes -= count;
    }
}

L1InitStateTable::L1InitStateTable() : LocalInitStateTable(kCapacity) {}

L0CInitStateTable::L0CInitStateTable() : LocalInitStateTable(kCapacity) {}

uint64_t InitStateTableIndex::LocalCapacity(uint32_t space)
{
    switch (space) {
        case ACLSAN_DEVICE_MEMORY_SPACE_L1:
            return L1InitStateTable::kCapacity;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0C:
            return L0CInitStateTable::kCapacity;
        default:
            return 0;
    }
}
bool InitStateTableIndex::ValidKey(const InitStateKey& key)
{
    if (key.space == ACLSAN_DEVICE_MEMORY_SPACE_GM)
        return key.launchId == 0 && key.blockType == 0 && key.blockId == 0;
    return LocalCapacity(key.space) != 0 && key.contextId == 0;
}
InitStateTable& InitStateTableIndex::GetOrCreate(const InitStateKey& key)
{
    if (!ValidKey(key))
        throw std::invalid_argument("Invalid initialization table key");
    if (auto* table = Find(key))
        return *table;
    std::unique_ptr<InitStateTable> table;
    switch (key.space) {
        case ACLSAN_DEVICE_MEMORY_SPACE_GM:
            table = std::make_unique<GmInitStateTable>();
            break;
        case ACLSAN_DEVICE_MEMORY_SPACE_L1:
            table = std::make_unique<L1InitStateTable>();
            break;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0C:
            table = std::make_unique<L0CInitStateTable>();
            break;
        default:
            throw std::invalid_argument("Unsupported initialization memory space");
    }
    // Construct first: failed allocation must not publish a null table entry.
    return *tables_.emplace(key, std::move(table)).first->second;
}
InitStateTable* InitStateTableIndex::Find(const InitStateKey& key)
{
    auto found = tables_.find(key);
    return found == tables_.end() ? nullptr : found->second.get();
}
void InitStateTableIndex::Erase(const InitStateKey& key) { tables_.erase(key); }
void InitStateTableIndex::EraseLaunch(uint32_t device, uint64_t launch)
{
    for (auto it = tables_.begin(); it != tables_.end();) {
        const auto& key = it->first;
        if (key.space != ACLSAN_DEVICE_MEMORY_SPACE_GM && key.deviceId == device && key.launchId == launch)
            it = tables_.erase(it);
        else
            ++it;
    }
}

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
    auto append = [&](uint64_t address, uint64_t bytes) {
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
        case ACLSAN_MEM_LAYOUT_BLOCK_REPEAT:
            layout.rank = 2;
            layout.elementBytes = data.layout.blockRepeat.blockSize;
            layout.dimensions[0] = data.layout.blockRepeat.blockNum;
            layout.dimensions[1] = data.layout.blockRepeat.repeatTimes;
            layout.strides[0] = data.layout.blockRepeat.blockStride;
            layout.strides[1] = data.layout.blockRepeat.repeatStride;
            if (!VisitAffineSegments(layout, append))
                return false;
            break;
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
    report.common.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
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
    report.common.severity = ReportSeverity::ERROR;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    auto& exec = report.common.exec;
    const auto& h = data.header;
    exec.launchId = h.launchId;
    exec.instrExecId = h.instrExecId;
    exec.serialNo = h.serialNo;
    exec.pc = h.pc;
    exec.deviceId = h.deviceId;
    exec.phyCoreId = h.phyCoreId;
    exec.blockId = h.blockId;
    exec.blockType = h.blockType;
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
    if (scope.space != ACLSAN_DEVICE_MEMORY_SPACE_GM || !InitStateTableIndex::ValidKey(scope) ||
        !GmInitStateTable::ValidRange(address, bytes))
        return AllocationUpdateStatus::INVALID_RANGE;
    auto& table = tables_.GetOrCreate(scope);
    auto& allocations = gmAllocations_[scope];
    const auto status = allocations.Register(id, address, bytes, scope.deviceId);
    if (status == AllocationUpdateStatus::OK)
        table.Clear(address, bytes);
    return status;
}

AllocationUpdateStatus MemoryInitcheck::ReleaseAllocation(InitStateKey scope, uint64_t id, uint64_t address)
{
    auto context = gmAllocations_.find(scope);
    if (context == gmAllocations_.end())
        return AllocationUpdateStatus::NOT_FOUND;
    const auto allocation = context->second.Classify(scope.deviceId, address, 1).allocation;
    const auto status = context->second.Release(id, address, scope.deviceId);
    if (status == AllocationUpdateStatus::OK && allocation)
        tables_.Find(scope)->Clear(allocation->base, allocation->bytes);
    return status;
}

InitCheckStatus MemoryInitcheck::MarkInitialized(InitStateKey scope, uint64_t address, uint64_t bytes, uint8_t pipe)
{
    if (!InitStateTableIndex::ValidKey(scope) || !GmInitStateTable::ValidRange(address, bytes) || pipe > 15)
        return InitCheckStatus::INVALID_RECORD;
    if (bytes == 0)
        return InitCheckStatus::OK;
    if (scope.space == ACLSAN_DEVICE_MEMORY_SPACE_GM) {
        const auto context = gmAllocations_.find(scope);
        if (context == gmAllocations_.end() ||
            context->second.Classify(scope.deviceId, address, bytes).status != RangeStatus::VALID)
            return InitCheckStatus::INVALID_ALLOCATION;
    } else {
        const auto capacity = InitStateTableIndex::LocalCapacity(scope.space);
        if (address > capacity || bytes > capacity - address)
            return InitCheckStatus::INVALID_ALLOCATION;
    }
    try {
        tables_.GetOrCreate(scope).Write(address, bytes, pipe);
        return InitCheckStatus::OK;
    } catch (const std::bad_alloc&) {
        return InitCheckStatus::RESOURCE_EXHAUSTED;
    }
}

InitCheckResult MemoryInitcheck::CheckInstruction(
    InitStateKey scope, const std::vector<AclsanDeviceMemoryAccessData>& accesses,
    const InitializedRanges& precedingWrites, bool commitWrites, bool restrictToPrecedingWrites)
{
    InitCheckResult result;
    auto fail = [&](InitCheckStatus status) { return InitCheckResult{status, {}}; };
    if (!InitStateTableIndex::ValidKey(scope))
        return fail(InitCheckStatus::INVALID_RECORD);
    if (accesses.empty())
        return result;
    try {
        const auto& first = accesses.front().header;
        if (scope.space != ACLSAN_DEVICE_MEMORY_SPACE_GM &&
            (scope.launchId != first.launchId || scope.blockType != first.blockType || scope.blockId != first.blockId))
            return fail(InitCheckStatus::INVALID_RECORD);
        std::vector<bool> seen(accesses.size(), false);
        std::vector<Operand> operands;
        // 完整性检查先于任何状态更新。调用方必须已校验配置寄存器依赖，并按同步依赖排序。
        for (const auto& access : accesses) {
            const auto& h = access.header;
            if (h.version != ACLSAN_API_VERSION || h.size < sizeof(access) || h.deviceId != scope.deviceId ||
                h.launchId != first.launchId || h.instrExecId != first.instrExecId || h.blockType != first.blockType ||
                h.blockId != first.blockId || h.pipeline > 15 || access.accessIndex >= accesses.size() ||
                seen[access.accessIndex] || access.accessMode < ACLSAN_DEVICE_MEMORY_ACCESS_READ ||
                access.accessMode > ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX) {
                return fail(InitCheckStatus::INVALID_RECORD);
            }
            if (access.accessCount != accesses.size() || !(h.flags & ACLSAN_DEVICE_EVENT_FLAG_EXACT) ||
                (h.flags & (ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED | ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED |
                            ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR))) {
                return fail(InitCheckStatus::INCOMPLETE_RECORD);
            }
            seen[access.accessIndex] = true;
            if (access.memorySpace != scope.space)
                continue;
            if ((h.flags & ACLSAN_DEVICE_EVENT_FLAG_PREDICATED) && access.predicateMask0 == 0 &&
                access.predicateMask1 == 0)
                continue;
            Operand operand{&access, {}};
            if (!ExpandRanges(access, operand.ranges))
                return fail(InitCheckStatus::INVALID_RECORD);
            operands.push_back(std::move(operand));
        }
        const auto found = gmAllocations_.find(scope);
        const auto classify = [&](uint64_t address, uint64_t bytes) -> RangeResult {
            if (scope.space != ACLSAN_DEVICE_MEMORY_SPACE_GM) {
                const auto capacity = InitStateTableIndex::LocalCapacity(scope.space);
                if (address > capacity || bytes > capacity - address)
                    return {RangeStatus::OUT_OF_BOUNDS, std::nullopt};
                Allocation allocation{};
                allocation.bytes = capacity;
                allocation.deviceId = scope.deviceId;
                return {RangeStatus::VALID, allocation};
            }
            return found == gmAllocations_.end() ? RangeResult{} :
                                                   found->second.Classify(scope.deviceId, address, bytes);
        };
        for (const auto& operand : operands) {
            uint64_t total = 0;
            for (const auto& range : operand.ranges) {
                if (classify(range.address, range.bytes).status != RangeStatus::VALID) {
                    return fail(InitCheckStatus::INVALID_ALLOCATION);
                }
                total += range.bytes;
            }
            // 报告 ABI 的 accessBytes 为 uint32_t，不静默截断读取大小。
            if (total > std::numeric_limits<uint32_t>::max())
                return fail(InitCheckStatus::RESOURCE_EXHAUSTED);
        }
        if (operands.empty())
            return result;
        auto& table = tables_.GetOrCreate(scope);
        // GM combines its launch baseline with proven preceding writes. Local
        // replay queries actual state bytes only where a preceding write proves
        // visibility; topologically earlier but unordered writes cannot leak in.
        const auto queryInitialized = [&](uint64_t address, uint64_t bytes) {
            InitStateQuery result;
            const auto append = [&](uint64_t begin, uint64_t end) {
                const auto q = table.Query(begin, end - begin);
                if (!result.uninitializedBytes && q.uninitializedBytes)
                    result.firstUninitializedAddress = q.firstUninitializedAddress;
                result.uninitializedBytes += q.uninitializedBytes;
            };
            const auto appendGap = [&](uint64_t begin, uint64_t end) {
                if (!restrictToPrecedingWrites) {
                    append(begin, end);
                } else if (begin < end) {
                    if (!result.uninitializedBytes)
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
                if (restrictToPrecedingWrites)
                    append(coveredBegin, coveredEnd);
                address = coveredEnd;
            }
            if (address < end)
                appendGap(address, end);
            return result;
        };
        const uint64_t group = nextGroupId_++;
        // 每个读操作按分配汇总；同一条指令的全部读端先检查，随后才写入。
        for (const auto& operand : operands) {
            if (!(operand.access->accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_READ))
                continue;
            std::map<uint64_t, NpuCheckInitcheckReport> reports;
            for (const auto& range : operand.ranges) {
                const auto allocation = *classify(range.address, range.bytes).allocation;
                auto [it, inserted] = reports.try_emplace(allocation.base);
                auto& report = it->second;
                if (inserted) {
                    report = MakeMemoryReport(*operand.access, allocation);
                    report.access.rangeBegin = range.address;
                }
                report.access.rangeEnd = range.address + range.bytes;
                report.access.accessBytes += static_cast<uint32_t>(range.bytes);
                const auto query = queryInitialized(range.address, range.bytes);
                if (query.uninitializedBytes && report.uninitBytes == 0) {
                    report.firstUninitAddress = query.firstUninitializedAddress;
                    report.firstUninitOffset = query.firstUninitializedAddress - allocation.base;
                }
                report.uninitBytes += query.uninitializedBytes;
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
                report.common.reportId = nextReportId_++;
                report.common.groupId = group;
                result.reports.push_back(std::move(report));
            }
        }
        for (const auto& operand : operands) {
            if (!commitWrites || !(operand.access->accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE))
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
    tables_.Erase(scope);
    gmAllocations_.erase(scope);
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
} // namespace

bool Initcheck::OnDeviceSync(AclsanCallbackId cbid, const void* data)
{
    if (!data) {
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
    if (pendingSyncEvents_.size() >= race::Limits{}.events) {
        replayReasons_[h.launchId].insert("event_budget");
        return true;
    }
    if (!barrier) {
        const auto& sync = *static_cast<const AclsanDeviceSyncData*>(data);
        if (sync.syncKind == ACLSAN_DEVICE_SYNC_KIND_UNKNOWN)
            replayReasons_[h.launchId].insert("unsupported_sync_kind");
        pendingSyncEvents_.push_back(race::NormalizeSync(sync));
    } else {
        AclsanDeviceSyncData sync{};
        sync.header = h;
        sync.scope = ACLSAN_DEVICE_SYNC_SCOPE_PIPE;
        sync.srcPipe = h.pipeline;
        sync.dstPipe = static_cast<const AclsanDeviceBarrierData*>(data)->pipe;
        pendingSyncEvents_.push_back(sync);
    }
    return true;
}

void Initcheck::FlushMemoryAccesses(void* stream, CheckerReportList& reports)
{
    struct Batch {
        std::vector<AclsanDeviceMemoryAccessData> memory;
        std::vector<AclsanDeviceSyncData> sync;
    };
    std::map<uint64_t, Batch> batches;
    const auto selected = [&](uint64_t id) {
        const auto launch = launchStreams_.find(id);
        // Standalone cbdata consumers may not provide launch callbacks. Real
        // registered launches are always drained only by their own stream.
        return launch == launchStreams_.end() || launch->second == stream;
    };
    for (const auto& launch : launchStreams_)
        if (selected(launch.first))
            batches[launch.first];
    const auto memoryEnd =
        std::remove_if(pendingMemoryAccesses_.begin(), pendingMemoryAccesses_.end(), [&](const auto& a) {
            if (!selected(a.header.launchId))
                return false;
            // Dependency-only legacy records still drive the register checker.
            if (a.accessCount || a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN)
                batches[a.header.launchId].memory.push_back(a);
            else
                batches[a.header.launchId];
            return true;
        });
    pendingMemoryAccesses_.erase(memoryEnd, pendingMemoryAccesses_.end());
    const auto syncEnd = std::remove_if(pendingSyncEvents_.begin(), pendingSyncEvents_.end(), [&](const auto& s) {
        if (!selected(s.header.launchId))
            return false;
        batches[s.header.launchId].sync.push_back(s);
        return true;
    });
    pendingSyncEvents_.erase(syncEnd, pendingSyncEvents_.end());
    for (const auto& entry : replayReasons_)
        if (selected(entry.first))
            batches[entry.first];
    for (auto& entry : batches) {
        try {
            ReplayLaunch(entry.first, entry.second.memory, entry.second.sync, reports);
        } catch (const std::bad_alloc&) {
            ++stats_.malformedEvents;
            ++replayFailures_["allocation_failure"];
            for (const auto& access : entry.second.memory)
                uncertainDevices_.insert(access.header.deviceId);
        }
        launchStreams_.erase(entry.first);
        replayReasons_.erase(entry.first);
        for (auto it = initializedRegisters_.begin(); it != initializedRegisters_.end();)
            if (std::get<1>(it->first) == entry.first)
                it = initializedRegisters_.erase(it);
            else
                ++it;
        for (auto it = reportedUsages_.begin(); it != reportedUsages_.end();)
            if (std::get<1>(*it) == entry.first)
                it = reportedUsages_.erase(it);
            else
                ++it;
        for (auto it = reportGroups_.begin(); it != reportGroups_.end();)
            if (std::get<1>(it->first) == entry.first)
                it = reportGroups_.erase(it);
            else
                ++it;
    }
}

void Initcheck::ReplayLaunch(
    uint64_t launchId, std::vector<AclsanDeviceMemoryAccessData>& memory, std::vector<AclsanDeviceSyncData>& sync,
    CheckerReportList& reports)
{
    using namespace race;
    auto& reasons = replayReasons_[launchId];
    const Limits limits;
    if (memory.size() + sync.size() > limits.events)
        reasons.insert("event_budget");
    // A pending stream's traces may not have arrived yet. Without a stream
    // dependency, neither completion order proves which kernel initialized GM.
    const bool usesGm = std::any_of(
        memory.begin(), memory.end(), [](const auto& a) { return a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM; });
    const bool hasMasterTracked = std::any_of(memory.begin(), memory.end(), [](const auto& a) {
        return a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_L0A && a.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_L0B &&
               (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM ||
                InitStateTableIndex::LocalCapacity(a.memorySpace) != 0);
    });
    const bool hasL0 = std::any_of(memory.begin(), memory.end(), [](const auto& a) {
        return a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0A || a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0B ||
               aclsan::AccessesL0Operands(a.instructionId);
    });
    if (hasL0 && !hasMasterTracked && reasons.empty())
        return;
    const auto currentLaunch = launchStreams_.find(launchId);
    if (usesGm)
        for (const auto& other : launchStreams_) {
            if (other.first != launchId &&
                (currentLaunch == launchStreams_.end() || other.second != currentLaunch->second)) {
                reasons.insert("cross_stream_initialization_unknown");
                replayReasons_[other.first].insert("cross_stream_initialization_unknown");
            }
        }
    std::optional<uint32_t> device;
    const auto checkDevice = [&](const AclsanDeviceEventHeader& h) {
        if (device && *device != h.deviceId)
            reasons.insert("device_mismatch");
        device = h.deviceId;
    };
    for (const auto& a : memory) {
        checkDevice(a.header);
        if (a.header.flags != ACLSAN_DEVICE_EVENT_FLAG_EXACT || a.vectorMask0 || a.vectorMask1 || a.predicateMask0 ||
            a.predicateMask1 || a.memoryFlags)
            reasons.insert("memory_coverage_unknown");
        // Fragment handshakes require per-fragment initialization transactions;
        // never promote their partial completion to a whole-instruction write.
        if (a.unitFlag)
            reasons.insert("unit_flag_initialization_unsupported");
        if (uncertainDevices_.count(a.header.deviceId))
            reasons.insert("prior_initialization_state_unknown");
    }
    for (const auto& s : sync)
        checkDevice(s.header);
    struct LocalTablesGuard {
        MemoryInitcheck& checker;
        uint32_t device;
        uint64_t launch;
        ~LocalTablesGuard() { checker.ResetLaunch(device, launch); }
    } localTablesGuard{memoryInitcheck_, device.value_or(0), launchId};
    DependencyGraph dependency(limits);
    if (reasons.empty())
        BuildDependencyGraph(memory, sync, {}, limits, reasons, dependency);
    auto& graph = dependency.graph;
    auto& accesses = dependency.accesses;
    // Canonical tool ordering puts memcheck before initcheck. Bounds/lifetime
    // errors belong to memcheck. Keep their nodes in the validated dependency
    // graph, but do not replay any operand of the invalid
    // instruction or use its local writes to prove initialization.
    using InvalidInstruction =
        std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t, uint32_t>;
    std::set<InvalidInstruction> invalidInstructions;
    for (const auto& report : reports) {
        const auto* bounds = std::get_if<NpuCheckMemcheckReport>(&report);
        if (!bounds || bounds->common.tool != ReportTool::MEMCHECK ||
            !(bounds->common.flags & kNpuCheckReportCommonHasExecContext) ||
            (bounds->common.pattern != NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS &&
             bounds->common.pattern != NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE))
            continue;
        const auto& exec = bounds->common.exec;
        if (exec.launchId == launchId)
            invalidInstructions.emplace(
                exec.deviceId, exec.launchId, exec.blockType, exec.blockId, exec.phyCoreId, exec.instrExecId,
                exec.pipeId, exec.pc, exec.siteId);
    }
    std::map<size_t, std::vector<size_t>> instructions;
    std::map<Storage, std::vector<size_t>> writers;
    for (size_t i = 0; i < accesses.size(); ++i) {
        const auto& a = accesses[i].data;
        const auto& h = a.header;
        if (invalidInstructions.count(
                {h.deviceId, h.launchId, h.blockType, h.blockId, h.phyCoreId, h.instrExecId, h.pipeline, h.pc,
                 h.siteId})) {
            // An invalid GM write has unknown effects; it must never establish
            // initialized bytes for this launch or a subsequent one.
            if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM && (a.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE))
                reasons.insert("invalid_gm_write");
            continue;
        }
        instructions[accesses[i].node].push_back(i);
        if (accesses[i].data.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE)
            writers[StorageOf(accesses[i].data)].push_back(i);
    }
    CheckerReportList staged;
    size_t comparisons = 0, candidates = 0;
    for (auto node : graph.Order()) {
        if (!reasons.empty())
            break;
        const auto instruction = instructions.find(node);
        if (instruction == instructions.end())
            continue;
        std::vector<AclsanDeviceMemoryAccessData> operands;
        std::map<Storage, std::vector<size_t>> byStorage;
        for (auto i : instruction->second) {
            operands.push_back(accesses[i].data);
            byStorage[StorageOf(accesses[i].data)].push_back(i);
        }
        for (const auto& storage : byStorage) {
            const auto space = std::get<0>(storage.first);
            if (space == ACLSAN_DEVICE_MEMORY_SPACE_L0A || space == ACLSAN_DEVICE_MEMORY_SPACE_L0B)
                continue;
            if (space != ACLSAN_DEVICE_MEMORY_SPACE_GM && !InitStateTableIndex::LocalCapacity(space))
                continue;
            std::vector<Interval> preceding;
            const bool hasRead = std::any_of(storage.second.begin(), storage.second.end(), [&](size_t i) {
                return accesses[i].data.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_READ;
            });
            if (hasRead)
                for (auto w : writers[storage.first]) {
                    if (++candidates > limits.comparisons) {
                        reasons.insert("candidate_budget");
                        break;
                    }
                    if (accesses[w].node == node)
                        continue; // Read-before-write within one instruction.
                    if (++comparisons > limits.comparisons) {
                        reasons.insert("comparison_budget");
                        break;
                    }
                    const auto relation = graph.Compare(accesses[w].node, node);
                    if (relation == HappensBeforeGraph::Relation::Unknown) {
                        reasons.insert("graph_query_budget");
                        break;
                    }
                    if (relation == HappensBeforeGraph::Relation::Before) {
                        preceding.insert(preceding.end(), accesses[w].ranges.begin(), accesses[w].ranges.end());
                    } else if (relation == HappensBeforeGraph::Relation::Unordered) {
                        for (auto r : storage.second)
                            if ((accesses[r].data.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_READ) &&
                                Overlap(accesses[w].ranges, accesses[r].ranges))
                                reasons.insert("unordered_initialization");
                    }
                    if (preceding.size() > limits.segments) {
                        reasons.insert("segment_budget");
                        break;
                    }
                }
            if (!reasons.empty())
                break;
            MergeIntervals(preceding);
            MemoryInitcheck::InitializedRanges initialized;
            for (auto range : preceding)
                initialized.emplace_back(range.begin, range.end);
            const auto& h = accesses[storage.second.front()].data.header;
            const auto key = space == ACLSAN_DEVICE_MEMORY_SPACE_GM ?
                                 InitStateKey{*device, 0} :
                                 InitStateKey::Local(h.deviceId, h.launchId, h.blockType, h.blockId, space);
            const auto result = memoryInitcheck_.CheckInstruction(
                key, operands, initialized, false, space != ACLSAN_DEVICE_MEMORY_SPACE_GM);
            if (result.status != InitCheckStatus::OK) {
                reasons.insert("invalid_memory_operation");
                break;
            }
            const uint64_t group = nextGroupId_++;
            for (auto report : result.reports) {
                report.common.reportId = nextReportId_++;
                report.common.groupId = group;
                staged.emplace_back(std::move(report));
            }
        }
        // All read operands of this instruction have been checked. Record each
        // local write in its byte table, even when the source read was uninitialized
        // (initialization checking does not propagate value taint).
        if (reasons.empty())
            for (auto i : instruction->second) {
                const auto& access = accesses[i];
                const auto space = access.data.memorySpace;
                if (!InitStateTableIndex::LocalCapacity(space) ||
                    !(access.data.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE))
                    continue;
                const auto& h = access.data.header;
                const auto key = InitStateKey::Local(h.deviceId, h.launchId, h.blockType, h.blockId, space);
                for (const auto& range : access.ranges)
                    if (memoryInitcheck_.MarkInitialized(
                            key, range.begin, range.end - range.begin,
                            static_cast<uint8_t>(access.data.header.pipeline)) != InitCheckStatus::OK)
                        reasons.insert("write_commit_failed");
            }
    }
    // Commit device writes only once the entire launch has been validated. The
    // next synchronized launch sees these writes, never an arbitrary replay prefix.
    if (reasons.empty()) {
        for (const auto& access : accesses) {
            if (access.data.memorySpace != ACLSAN_DEVICE_MEMORY_SPACE_GM ||
                !(access.data.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE))
                continue;
            // A stream completion does not establish an edge into kernels
            // already submitted on another stream. Invalidate their baseline.
            const auto current = launchStreams_.find(launchId);
            for (const auto& pending : launchStreams_)
                if (pending.first != launchId && (current == launchStreams_.end() || pending.second != current->second))
                    replayReasons_[pending.first].insert("cross_stream_initialization_unknown");
            for (const auto range : access.ranges)
                if (memoryInitcheck_.MarkInitialized(
                        {access.data.header.deviceId, 0}, range.begin, range.end - range.begin,
                        static_cast<uint8_t>(access.data.header.pipeline)) != InitCheckStatus::OK)
                    reasons.insert("write_commit_failed");
        }
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
            if (a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM && (a.accessMode & ACLSAN_DEVICE_MEMORY_ACCESS_WRITE))
                uncertainDevices_.insert(a.header.deviceId);
        return;
    }
    for (auto& report : staged) {
        reports.emplace_back(std::move(report));
        ++stats_.errors;
    }
}

namespace {

constexpr size_t kMemoryAccessDependencySize =
    offsetof(AclsanDeviceMemoryAccessData, regDependencyMask1) + sizeof(uint64_t);
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
    };
    static_assert(std::size(descriptions) == ACLSAN_DEVICE_REGISTER_COUNT);
    return id < std::size(descriptions) ? descriptions[id] : RegisterDescription{"unknown", "unknown"};
}

const char* ConsumerInstructionName(uint32_t id)
{
    using aclsan::InstructionId;
    switch (static_cast<InstructionId>(id)) {
        case InstructionId::LoadGmToCbuf2DV2:
            return "load_gm_to_cbuf_2dv2";
        case InstructionId::CopyGmToCbufV2:
            return "copy_gm_to_cbuf_v2";
        case InstructionId::CopyGmToCbufAlignV2B8:
        case InstructionId::CopyGmToCbufAlignV2B16:
        case InstructionId::CopyGmToCbufAlignV2B32:
            return "copy_gm_to_cbuf_align_v2";
        case InstructionId::CopyGmToCbufMultiNd2NzB8:
        case InstructionId::CopyGmToCbufMultiNd2NzB16:
        case InstructionId::CopyGmToCbufMultiNd2NzB32:
            return "copy_gm_to_cbuf_multi_nd2nz";
        case InstructionId::CopyGmToCbufMultiDn2NzB8:
        case InstructionId::CopyGmToCbufMultiDn2NzB16:
        case InstructionId::CopyGmToCbufMultiDn2NzB32:
            return "copy_gm_to_cbuf_multi_dn2nz";
        case InstructionId::CopyUbufToGmAlignV2:
            return "copy_ubuf_to_gm_align_v2";
        case InstructionId::CopyGmToUbufAlignV2B8:
        case InstructionId::CopyGmToUbufAlignV2B16:
        case InstructionId::CopyGmToUbufAlignV2B32:
            return "copy_gm_to_ubuf_align_v2";
        case InstructionId::NdDmaOutToUbufB8:
        case InstructionId::NdDmaOutToUbufB16:
        case InstructionId::NdDmaOutToUbufB32:
            return "nd_copy_gm_to_ubuf";
        case InstructionId::FixL0cToOutF32:
        case InstructionId::FixL0cToOutS32:
            return "fix_l0c_to_out";
        default:
            return "unknown";
    }
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

} // namespace

const std::vector<CallbackSpec>& Initcheck::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> callbacks{
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_ALLOC},
        {ACLSAN_CB_DOMAIN_RESOURCE, ACLSAN_CBID_RESOURCE_MEMORY_FREE},
        {ACLSAN_CB_DOMAIN_MEMORY, ACLSAN_CBID_MEMORY_WRITE_END},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_SYNC},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_PIPE_BARRIER},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
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
    if (!pendingLaunchStreams_.empty() || !pendingMemoryAccesses_.empty()) {
        ++stats_.malformedEvents;
        ASCTOOL_WARNING("initcheck: Host write overlaps pending device work; initialization order is unknown");
        return false;
    }
    const auto status =
        memoryInitcheck_.MarkInitialized({data.deviceId, 0}, reinterpret_cast<uint64_t>(data.destination), data.bytes);
    if (status != InitCheckStatus::OK) {
        ++stats_.malformedEvents;
        ASCTOOL_WARNING("initcheck: cannot record completed Host write, status=%u", static_cast<unsigned>(status));
        return false;
    }
    return true;
}

void Initcheck::OnAllocation(const AclsanResourceData& data)
{
    if (data.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE || data.common.result != 0) {
        return;
    }
    (void)memoryInitcheck_.RegisterAllocation(
        {data.deviceId, 0}, data.resourceId, reinterpret_cast<uint64_t>(data.ptr), data.bytes);
}

void Initcheck::OnFree(const AclsanResourceData& data)
{
    if (data.memorySpace != ACLSAN_MEMORY_SPACE_DEVICE || data.common.result != 0) {
        return;
    }
    (void)memoryInitcheck_.ReleaseAllocation({data.deviceId, 0}, data.resourceId, reinterpret_cast<uint64_t>(data.ptr));
}

bool Initcheck::OnRegisterState(const AclsanDeviceRegisterStateData& data)
{
    if (data.header.version != ACLSAN_API_VERSION || data.header.size < sizeof(data) ||
        data.regId >= ACLSAN_DEVICE_REGISTER_COUNT || data.regId >= 64) {
        ++stats_.malformedEvents;
        memory_.Invalidate();
        return false;
    }
    const CoreKey key{data.header.deviceId, data.header.launchId, data.header.blockType, data.header.blockId};
    initializedRegisters_[key] |= UINT64_C(1) << data.regId;
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

bool Initcheck::OnMemoryAccess(const AclsanDeviceMemoryAccessData& data, CheckerReportList& reports)
{
    if (data.header.version != ACLSAN_API_VERSION || data.header.size < kMemoryAccessDependencySize ||
        data.regDependencyMask1 != 0 || (data.regDependencyMask0 & ~kSupportedRegisterMask) != 0) {
        ++stats_.malformedEvents;
        memory_.Invalidate();
        return false;
    }
    ++stats_.deviceOperations;
    const CoreKey core{data.header.deviceId, data.header.launchId, data.header.blockType, data.header.blockId};
    const uint64_t initialized = initializedRegisters_[core];
    uint64_t missing = data.regDependencyMask0 & ~initialized;
    // These transfers read GM and write UB/L1. If a required register is
    // missing, their single address-less placeholder has no GM write to commit.
    // Report the register error without attempting address replay. Partial
    // accesses and lost traces must still fail the coverage checks below.
    using aclsan::InstructionId;
    const bool nddma = data.instructionId == static_cast<uint32_t>(InstructionId::NdDmaOutToUbufB8) ||
                       data.instructionId == static_cast<uint32_t>(InstructionId::NdDmaOutToUbufB16) ||
                       data.instructionId == static_cast<uint32_t>(InstructionId::NdDmaOutToUbufB32);
    const bool gmToL1 = data.instructionId == static_cast<uint32_t>(InstructionId::CopyGmToCbufAlignV2B8) ||
                        data.instructionId == static_cast<uint32_t>(InstructionId::CopyGmToCbufAlignV2B16) ||
                        data.instructionId == static_cast<uint32_t>(InstructionId::CopyGmToCbufAlignV2B32);
    const bool missingRegisterPlaceholder =
        (nddma || gmToL1) && missing != 0 && data.header.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED &&
        data.accessCount == 1 && data.accessIndex == 0 && data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN &&
        data.layoutKind == 0 && data.accessMode == 0 && data.address == 0 && !data.vectorMask0 && !data.vectorMask1 &&
        !data.predicateMask0 && !data.predicateMask1 && !data.memoryFlags && !data.unitFlag;
    if (missingRegisterPlaceholder) {
        // Retain a dependency-only marker so synchronization also closes this
        // launch's register state when no address or sync records were emitted.
        auto registerOnly = data;
        registerOnly.accessCount = 0;
        pendingMemoryAccesses_.push_back(registerOnly);
    } else {
        if (data.header.flags & (ACLSAN_DEVICE_EVENT_FLAG_TRUNCATED | ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR |
                                 ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED)) {
            replayReasons_[data.header.launchId].insert("memory_coverage_unknown");
        }
        // Keep all operands until synchronization so a GM instruction with multiple
        // operands can be checked atomically (reads before writes).
        pendingMemoryAccesses_.push_back(data);
    }
    auto memory = data;
    if (missing != 0)
        memory.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
    // L0 replay needs every fragment of an L0 instruction, but unrelated master
    // events must not make the independent L0 analysis incomplete.
    const bool l0Instruction = data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0A ||
                               data.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_L0B ||
                               aclsan::AccessesL0Operands(data.instructionId);
    if (l0Instruction)
        memory_.Access(memory);
    if (missing == 0) {
        return true;
    }

    if (data.accessCount != 0 && !missingRegisterPlaceholder) {
        replayReasons_[data.header.launchId].insert("uninitialized_register_dependency");
    }
    const InstructionKey instruction{
        data.header.deviceId, data.header.launchId, data.header.blockType, data.header.blockId,
        data.header.instrExecId};
    const auto [group, inserted] = reportGroups_.try_emplace(instruction, nextGroupId_);
    if (inserted) {
        ++nextGroupId_;
    }
    while (missing != 0) {
        const uint32_t registerId = static_cast<uint32_t>(__builtin_ctzll(missing));
        missing &= missing - 1;
        const UsageKey usage{data.header.deviceId, data.header.launchId,    data.header.blockType,
                             data.header.blockId,  data.header.instrExecId, registerId};
        if (!reportedUsages_.insert(usage).second) {
            continue;
        }
        reports.emplace_back(MakeReport(data, registerId, group->second));
        ++stats_.errors;
    }
    return true;
}

void Initcheck::OnSynchronization(const AclsanSynchronizeData& data, CheckerReportList& reports)
{
    ++stats_.synchronizations;
    if (data.common.result != 0) {
        ++stats_.failedSynchronizations;
        return;
    }
    const size_t reportsBefore = reports.size();
    FlushMemoryAccesses(data.stream, reports);
    pendingLaunchStreams_.erase(data.stream);
    // L0A/L0B coverage is replayed independently from the GM baseline. Both
    // checkers consume the same launch stream and contribute to one summary.
    const auto memoryBegin = reports.size();
    memory_.Collect(data.stream, reports);
    std::map<InstructionKey, uint64_t> memoryGroups;
    for (size_t i = memoryBegin; i < reports.size(); ++i) {
        auto& report = std::get<NpuCheckInitcheckReport>(reports[i]);
        report.common.reportId = nextReportId_++;
        report.common.timestampNs = TimestampNs();
        const auto& e = report.common.exec;
        const InstructionKey key{e.deviceId, e.launchId, e.blockType, e.blockId, e.instrExecId};
        const auto [group, inserted] = memoryGroups.try_emplace(key, nextGroupId_);
        if (inserted)
            ++nextGroupId_;
        report.common.groupId = group->second;
        report.common.exec.pipeName = PipelineName(e.pipeId);
        ++stats_.errors;
    }
    ASCTOOL_INFO(
        "initcheck synchronization completed memory_reports=%zu l0_memory_reports=%zu", reports.size() - reportsBefore,
        reports.size() - memoryBegin);
    // Other streams may still have live state. The two flushers remove only
    // state belonging to launches closed by this stream.
}

bool Initcheck::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    if ((domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_SYNC) ||
        (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_PIPE_BARRIER)) {
        if (!OnDeviceSync(cbid, data)) {
            memory_.Invalidate();
            return false;
        }
        if (cbid == ACLSAN_CBID_DEVICE_SYNC) {
            memory_.Sync(*static_cast<const AclsanDeviceSyncData*>(data));
        } else {
            memory_.Barrier(*static_cast<const AclsanDeviceBarrierData*>(data));
        }
        return true;
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
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_STATE) {
        return OnRegisterState(*static_cast<const AclsanDeviceRegisterStateData*>(data));
    }
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        return OnMemoryAccess(*static_cast<const AclsanDeviceMemoryAccessData*>(data), reports);
    }
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto* event = static_cast<const AclsanLaunchData*>(data);
        if (event->common.result != 0) {
            ++stats_.failedLaunches;
        } else {
            pendingLaunchStreams_.insert(event->stream);
            launchStreams_[event->launchId] = event->stream;
        }
        memory_.Launch(*event);
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        OnSynchronization(*static_cast<const AclsanSynchronizeData*>(data), reports);
    }
    return true;
}

InitcheckStats Initcheck::Stats() const { return stats_; }

void Initcheck::Finalize([[maybe_unused]] CheckerReportList& reports) { memory_.Finalize(); }

bool Initcheck::HasErrors() const { return stats_.errors != 0; }

bool Initcheck::AnalysisComplete() const
{
    return pendingLaunchStreams_.empty() && pendingMemoryAccesses_.empty() && pendingSyncEvents_.empty() &&
           replayReasons_.empty() && replayFailures_.empty() && stats_.malformedEvents == 0 &&
           stats_.failedLaunches == 0 && stats_.failedSynchronizations == 0 && stats_.failedHostWrites == 0 &&
           memory_.Complete();
}

std::string Initcheck::Summary() const
{
    std::ostringstream output;
    output << "tool=initcheck register_writes=" << stats_.registerWrites
           << " device_operations=" << stats_.deviceOperations << " synchronizations=" << stats_.synchronizations
           << " errors=" << stats_.errors << " malformed_events=" << stats_.malformedEvents
           << " failed_launches=" << stats_.failedLaunches
           << " failed_synchronizations=" << stats_.failedSynchronizations
           << " pending_memory_operations=" << pendingMemoryAccesses_.size()
           << " failed_host_writes=" << stats_.failedHostWrites
           << " pending_launch_streams=" << pendingLaunchStreams_.size();
    output << " sync_events=" << syncEvents_ << " pending_sync_events=" << pendingSyncEvents_.size();
    if (memory_.Operations() != 0 || memory_.IncompleteOperations() != 0) {
        output << " l0_memory_operations=" << memory_.Operations()
               << " incomplete_memory_operations=" << memory_.IncompleteOperations();
    }
    for (const auto& failure : replayFailures_)
        output << " " << failure.first << "=" << failure.second;
    return output.str();
}

} // namespace npucheck
