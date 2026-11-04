/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "checker/memory_state.h"
#include <algorithm>
#include <stdexcept>
namespace npucheck {
namespace {
constexpr uint8_t MAX_WRITER_PIPELINE = 0x0f;
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
    if (pipe > MAX_WRITER_PIPELINE)
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
    if (pipe > MAX_WRITER_PIPELINE)
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
        // Ascend950 platform_config: ub_size=253952 bytes (248 KiB).
        case ACLSAN_DEVICE_MEMORY_SPACE_UB:
            return 248U * 1024U;
        case ACLSAN_DEVICE_MEMORY_SPACE_L0A:
        case ACLSAN_DEVICE_MEMORY_SPACE_L0B:
            return 64U * 1024U;
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
        case ACLSAN_DEVICE_MEMORY_SPACE_UB:
        case ACLSAN_DEVICE_MEMORY_SPACE_L0A:
        case ACLSAN_DEVICE_MEMORY_SPACE_L0B:
            table = std::make_unique<LocalInitStateTable>(LocalCapacity(key.space));
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

AllocationUpdateStatus MemoryState::Register(
    InitStateKey scope, uint64_t id, uint64_t address, uint64_t bytes, size_t node)
{
    if (eventActive_ && resourceApplied_)
        return resourceStatus_;
    if (scope.space != ACLSAN_DEVICE_MEMORY_SPACE_GM || !InitStateTableIndex::ValidKey(scope) ||
        !GmInitStateTable::ValidRange(address, bytes))
        return AllocationUpdateStatus::INVALID_RANGE;
    auto& registry = allocations[scope];
    const auto status = registry.Register(id, address, bytes, scope.deviceId, node);
    if (status == AllocationUpdateStatus::OK) {
        if (auto* table = initialized.Find(scope))
            table->Clear(address, bytes);
    }
    resourceStatus_ = status;
    resourceApplied_ = eventActive_;
    return status;
}

AllocationUpdateStatus MemoryState::Release(InitStateKey scope, uint64_t id, uint64_t address, size_t node)
{
    if (eventActive_ && resourceApplied_)
        return resourceStatus_;
    auto context = allocations.find(scope);
    if (context == allocations.end())
        return AllocationUpdateStatus::NOT_FOUND;
    const auto allocation = context->second.Classify(scope.deviceId, address, 1).allocation;
    const auto status = context->second.Release(id, address, scope.deviceId, node);
    if (status == AllocationUpdateStatus::OK && allocation)
        if (auto* table = initialized.Find(scope))
            table->Clear(allocation->base, allocation->bytes);
    resourceStatus_ = status;
    resourceApplied_ = eventActive_;
    return status;
}

} // namespace npucheck
