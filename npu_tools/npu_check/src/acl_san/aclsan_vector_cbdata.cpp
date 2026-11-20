/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_vector_cbdata.h"
#include "acl_san/aclsan_api.h"

#include <array>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <utility>

namespace aclsan {
namespace {
constexpr uint32_t UB_BLOCK_BYTES = 32;
constexpr uint32_t SORT_ELEMENT_BYTES = 8;
constexpr uint32_t SORT_INPUT_ELEMENTS = 32;
constexpr uint32_t TRANSPOSE_BYTES = 512;
constexpr uint32_t VA_ADDRESS_SHIFT = 5;
constexpr uint32_t LIST_ADDRESS_SHIFT = 3;
constexpr uint32_t CONFIG_REPEAT_SHIFT = 56;
constexpr uint32_t CONFIG_STRIDE_SHIFT = 16;
constexpr uint32_t CONFIG_FIELD_MASK = 0xffff;
constexpr uint32_t MERGE_VALID_SHIFT = 8;
constexpr uint32_t MERGE_SUSPEND_SHIFT = 12;
constexpr uint32_t MERGE_REPEAT_MASK = 0xff;
constexpr uint32_t MERGE_LIST_COUNT = 4;
constexpr uint32_t TWO_MERGE_LISTS = 0x3;
constexpr uint32_t THREE_MERGE_LISTS = 0x7;
constexpr uint32_t ALL_MERGE_LISTS = 0xf;

class VectorAccessBuilder {
public:
    explicit VectorAccessBuilder(const MemoryCbdataContext& context) : context_(context) {}

    bool Add(
        uint64_t address, uint32_t mode, uint32_t bits, uint64_t bytes, uint32_t repeats = 1, uint64_t stride = 0,
        bool firstStride = false)
    {
        if (bytes == 0 || repeats == 0) {
            return true;
        }
        if (firstStride && address > UINT64_MAX - stride) {
            status_ = MemoryCbdataStatus::ARITHMETIC_OVERFLOW;
            return false;
        }
        address += firstStride ? stride : 0;
        if (stride != 0 && static_cast<uint64_t>(repeats - 1) > UINT64_MAX / stride) {
            status_ = MemoryCbdataStatus::ARITHMETIC_OVERFLOW;
            return false;
        }
        const uint64_t lastOffset = static_cast<uint64_t>(repeats - 1) * stride;
        if (address > UINT64_MAX - bytes || lastOffset > UINT64_MAX - address - bytes) {
            status_ = MemoryCbdataStatus::ARITHMETIC_OVERFLOW;
            return false;
        }
        AclsanDeviceMemoryAccessData data{};
        data.header = {ACLSAN_API_VERSION,   sizeof(data),      context_.launchId,
                       context_.pc,          context_.siteId,   ACLSAN_DEVICE_SOURCE_VECTOR,
                       context_.instrExecId, context_.serialNo, context_.deviceId,
                       context_.executor,    context_.pipeline, ACLSAN_DEVICE_EVENT_FLAG_EXACT};
        data.memoryOwner = context_.executor;
        data.address = address;
        data.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
        data.accessMode = mode;
        data.dataBits = bits;
        data.alignSize = UB_BLOCK_BYTES;
        data.instructionId = context_.instructionId;
        if (repeats == 1 || stride <= bytes) {
            data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
            data.layout.range.bytes = bytes + lastOffset;
        } else {
            data.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
            data.layout.blockRepeat = {1, static_cast<uint32_t>(bytes), 0, repeats, 0, static_cast<int64_t>(stride)};
        }
        accesses_.push_back(data);
        return true;
    }

    MemoryCbdataResult Finish(bool partial = false, bool estimated = false)
    {
        if (status_ != MemoryCbdataStatus::SUCCESS) {
            return {status_, {}, 0, true};
        }
        if (partial) {
            // Data-dependent merge termination has no proven exact output range.
            if (estimated) {
                for (auto& access : accesses_) {
                    access.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
                }
            }
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, std::move(accesses_), 0, true};
        }
        const auto status = accesses_.empty() ? MemoryCbdataStatus::NO_ACCESS : MemoryCbdataStatus::SUCCESS;
        return {status, std::move(accesses_)};
    }

private:
    const MemoryCbdataContext& context_;
    MemoryCbdata accesses_;
    MemoryCbdataStatus status_ = MemoryCbdataStatus::SUCCESS;
};

MemoryCbdataResult BuildVtransposeAccesses(const VtransposeParamField& f, const MemoryCbdataContext& context)
{
    VectorAccessBuilder out(context);
    out.Add(f.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 16, TRANSPOSE_BYTES);
    out.Add(f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 16, TRANSPOSE_BYTES);
    return out.Finish();
}

MemoryCbdataResult BuildVbs32Accesses(const Vbs32ParamField& f, const MemoryCbdataContext& context)
{
    const uint32_t repeat = static_cast<uint32_t>(f.config >> CONFIG_REPEAT_SHIFT);
    // A zero-repeat VBS32 has no memory accesses; hardware overhead is not checked.
    if (repeat == 0) {
        return {MemoryCbdataStatus::NO_ACCESS, {}};
    }
    const uint64_t scores = SORT_INPUT_ELEMENTS * (f.dataBits / 8);
    const uint64_t indices = SORT_INPUT_ELEMENTS * sizeof(uint32_t);
    const uint64_t result = SORT_INPUT_ELEMENTS * SORT_ELEMENT_BYTES;
    VectorAccessBuilder out(context);
    out.Add(f.scoreAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.dataBits, scores, repeat, scores);
    out.Add(f.indexAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 32, indices, repeat, indices);
    out.Add(f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.dataBits, result, repeat, result);
    // F16 records also write the two padding bytes between the score and index.
    return out.Finish();
}

bool AddVaPairAccesses(
    VectorAccessBuilder& out, uint32_t index, bool high, uint32_t mode, uint32_t bits, uint32_t repeats,
    uint64_t stride, bool firstStride, const MemoryRegisterState& state)
{
    bool complete = true;
    const uint32_t bytes = bits == 8 ? UB_BLOCK_BYTES / 2 : UB_BLOCK_BYTES;
    for (uint32_t lane = 0; lane < 2 * VA_ENTRY_COUNT; ++lane) {
        const auto& va = state.va[index + lane / VA_ENTRY_COUNT];
        const uint32_t entry = lane % VA_ENTRY_COUNT;
        if ((va.validEntries & (1U << entry)) == 0) {
            complete = false;
            continue;
        }
        const uint64_t address = (static_cast<uint64_t>(va.entries[entry]) << VA_ADDRESS_SHIFT) + (high ? bytes : 0);
        out.Add(address, mode, bits, bytes, repeats, stride, firstStride);
    }
    return complete;
}

MemoryCbdataResult BuildVnchwconvAccesses(
    const VnchwconvParamField& f, const MemoryCbdataContext& context, const MemoryRegisterState& state)
{
    const uint32_t repeats = static_cast<uint32_t>(f.config >> CONFIG_REPEAT_SHIFT);
    if (repeats == 0) {
        return {MemoryCbdataStatus::NO_ACCESS, {}};
    }
    VectorAccessBuilder out(context);
    const uint64_t srcStride = ((f.config >> CONFIG_STRIDE_SHIFT) & CONFIG_FIELD_MASK) * UB_BLOCK_BYTES;
    const uint64_t dstStride = (f.config & CONFIG_FIELD_MASK) * UB_BLOCK_BYTES;
    // VNCHWCONV's single-repeat form starts at base + stride; repeat > 1 starts at base.
    const bool firstStride = repeats == 1;
    const bool read = AddVaPairAccesses(
        out, f.srcVaIndex, f.dataBits == 8 && f.srcHighHalf, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.dataBits, repeats,
        srcStride, firstStride, state);
    const bool write = AddVaPairAccesses(
        out, f.dstVaIndex, f.dataBits == 8 && f.dstHighHalf, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.dataBits, repeats,
        dstStride, firstStride, state);
    return out.Finish(!read || !write, !read || !write);
}

MemoryCbdataResult BuildVmrgsortAccesses(const VmrgsortParamField& f, const MemoryCbdataContext& context)
{
    const uint32_t repeat = static_cast<uint32_t>(f.config & MERGE_REPEAT_MASK);
    const uint32_t valid = static_cast<uint32_t>((f.config >> MERGE_VALID_SHIFT) & ALL_MERGE_LISTS);
    const bool suspend = ((f.config >> MERGE_SUSPEND_SHIFT) & 1U) != 0;
    // devkit's Ascend 950 contract does not guarantee repeat=0 is a NOP.
    if (repeat == 0) {
        return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}, 0, true};
    }
    if ((valid != TWO_MERGE_LISTS && valid != THREE_MERGE_LISTS && valid != ALL_MERGE_LISTS) ||
        (repeat > 1 && suspend)) {
        return {MemoryCbdataStatus::INVALID_FIELD, {}, 0, true};
    }
    std::array<uint64_t, MERGE_LIST_COUNT> addresses{};
    std::array<uint32_t, MERGE_LIST_COUNT> lengths{};
    uint64_t total = 0;
    for (uint32_t i = 0; i < MERGE_LIST_COUNT; ++i) {
        addresses[i] = ((f.srcListAddresses >> (i * CONFIG_STRIDE_SHIFT)) & CONFIG_FIELD_MASK) << LIST_ADDRESS_SHIFT;
        lengths[i] = static_cast<uint32_t>((f.srcListLengths >> (i * CONFIG_STRIDE_SHIFT)) & CONFIG_FIELD_MASK);
        if ((valid & (1U << i)) != 0) {
            constexpr uint32_t maxListElements = 4095;
            if (lengths[i] == 0 || lengths[i] > maxListElements) {
                return {MemoryCbdataStatus::INVALID_FIELD, {}, 0, true};
            }
            total += static_cast<uint64_t>(lengths[i]) * SORT_ELEMENT_BYTES;
        }
    }
    VectorAccessBuilder out(context);
    if (!suspend && repeat > 1) {
        if (valid != ALL_MERGE_LISTS) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}, 0, true};
        }
        for (uint32_t i = 1; i < MERGE_LIST_COUNT; ++i) {
            if (lengths[i] != lengths[0] ||
                addresses[i] != addresses[0] + static_cast<uint64_t>(i) * lengths[0] * SORT_ELEMENT_BYTES) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}, 0, true};
            }
        }
        out.Add(addresses[0], ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.dataBits, total, repeat, total);
        out.Add(f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.dataBits, total, repeat, total);
    } else {
        for (uint32_t i = 0; i < MERGE_LIST_COUNT; ++i) {
            if ((valid & (1U << i)) != 0) {
                out.Add(
                    addresses[i], ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.dataBits,
                    static_cast<uint64_t>(lengths[i]) * SORT_ELEMENT_BYTES);
            }
        }
        out.Add(f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.dataBits, total);
    }
    const bool incomplete = suspend || f.dataBits == 16;
    return out.Finish(incomplete, incomplete);
}
} // namespace

MemoryCbdataResult ConvertVtranspose(const VtransposeParamField& field, const MemoryCbdataContext& context) noexcept
{
    try {
        return BuildVtransposeAccesses(field, context);
    } catch (const std::bad_alloc&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    } catch (const std::length_error&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    }
}

MemoryCbdataResult ConvertVbs32(const Vbs32ParamField& field, const MemoryCbdataContext& context) noexcept
{
    try {
        return BuildVbs32Accesses(field, context);
    } catch (const std::bad_alloc&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    } catch (const std::length_error&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    }
}

MemoryCbdataResult ConvertVmrgsort(const VmrgsortParamField& field, const MemoryCbdataContext& context) noexcept
{
    try {
        return BuildVmrgsortAccesses(field, context);
    } catch (const std::bad_alloc&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    } catch (const std::length_error&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    }
}

MemoryCbdataResult ConvertVnchwconv(
    const VnchwconvParamField& field, const MemoryCbdataContext& context, const MemoryRegisterState& state) noexcept
{
    try {
        return BuildVnchwconvAccesses(field, context, state);
    } catch (const std::bad_alloc&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    } catch (const std::length_error&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    }
}

MemoryCbdataResult ConvertLdva(const LdvaParamField& field, const MemoryCbdataContext& context) noexcept
{
    try {
        VectorAccessBuilder out(context);
        // LDVA always accesses a 32-byte UB block; h selects its loaded 128-bit half.
        out.Add(field.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 8, UB_BLOCK_BYTES);
        return out.Finish();
    } catch (const std::bad_alloc&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    } catch (const std::length_error&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}, 0, true};
    }
}
} // namespace aclsan
