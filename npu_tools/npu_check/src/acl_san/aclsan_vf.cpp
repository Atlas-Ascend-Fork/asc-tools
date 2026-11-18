/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_vf.h"
#include "acl_san/aclsan_api.h"
#include "npu_tool_log.h"

#include <npubit.h>

#include <algorithm>
#include <map>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <utility>
#include <type_traits>

namespace aclsan {
namespace {
// Feature detection keeps builds with older vendor headers safe. Never infer an
// instruction index from PC, range position or an assumed vendor struct offset.
template <class T, class = void>
struct DynamicIndex {
    static std::optional<uint64_t> Read(const T&) { return std::nullopt; }
};
template <class T>
struct DynamicIndex<T, std::void_t<decltype(std::declval<T>().instruction_execution_number)>> {
    static std::optional<uint64_t> Read(const T& range)
    {
        using Index = std::decay_t<decltype(range.instruction_execution_number)>;
        static_assert(std::is_integral_v<Index> && sizeof(Index) <= sizeof(uint64_t), "unsupported Npubit index type");
        if constexpr (std::is_signed_v<Index>) {
            if (range.instruction_execution_number < 0) {
                return std::nullopt;
            }
        }
        return static_cast<uint64_t>(range.instruction_execution_number);
    }
};
constexpr size_t MAX_VF_EVENTS = 100000;

std::optional<std::pair<uint32_t, uint32_t>> LocalMemBarrierTypes(NpubitUbAccessType type) noexcept
{
    switch (type) {
        case NPUBIT_SMEM_BAR_VV_ALL:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL, ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL}};
        case NPUBIT_SMEM_BAR_VST_VLD:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD}};
        case NPUBIT_SMEM_BAR_VLD_VST:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE}};
        case NPUBIT_SMEM_BAR_VST_VST:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE}};
        case NPUBIT_SMEM_BAR_VS_ALL:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_ALL}};
        case NPUBIT_SMEM_BAR_VST_LD:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_LOAD}};
        case NPUBIT_SMEM_BAR_VLD_ST:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE}};
        case NPUBIT_SMEM_BAR_VST_ST:
            return {{ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE}};
        case NPUBIT_SMEM_BAR_SV_ALL:
            return {{ACLSAN_DEVICE_LOCAL_MEM_SCALAR_ALL, ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL}};
        case NPUBIT_SMEM_BAR_ST_VLD:
            return {{ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD}};
        case NPUBIT_SMEM_BAR_LD_VST:
            return {{ACLSAN_DEVICE_LOCAL_MEM_SCALAR_LOAD, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE}};
        case NPUBIT_SMEM_BAR_ST_VST:
            return {{ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE}};
        default:
            return std::nullopt;
    }
}
} // namespace

VfPbState::Key VfPbState::Identity(const ParsedTraceRecord& record)
{
    return {record.launchId, record.deviceId, record.phyCoreId, record.blockType, record.blockId};
}

bool VfPbState::Push(const ParsedTraceRecord& record, const std::array<uint64_t, PUSH_PB_VALUE_COUNT>& values)
{
    State& state = states_[Identity(record)];
    if (state.consumed) {
        state = {};
    }
    if (state.invalid || state.count == MAX_PUSH_PB_RECORDS_PER_VF) {
        state.invalid = true;
        ASCTOOL_ERROR(
            "acl_san VF: more than %zu PUSH_PB records, pc=0x%llx", MAX_PUSH_PB_RECORDS_PER_VF,
            static_cast<unsigned long long>(record.record.pc));
        return false;
    }
    std::copy(values.begin(), values.end(), state.values.begin() + PUSH_PB_VALUE_COUNT * state.count);
    ++state.count;
    return true;
}

std::optional<std::array<uint64_t, PUSH_PB_STATE_COUNT_PER_VF>> VfPbState::Consume(const ParsedTraceRecord& record)
{
    State& state = states_[Identity(record)];
    const bool valid = !state.invalid && !state.consumed && state.count != 0;
    state.consumed = true;
    if (!valid) {
        ASCTOOL_ERROR(
            "acl_san VF: missing or invalid PB batch, pc=0x%llx", static_cast<unsigned long long>(record.record.pc));
        return std::nullopt;
    }
    return state.values;
}

struct NpubitUbHandle::Impl {
    std::vector<uint8_t> object;
    NpubitHandle handle{};
    std::mutex mutex;
};
NpubitUbHandle::NpubitUbHandle(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

NpubitUbHandle::~NpubitUbHandle()
{
    // Assumed cleanup contract; replace here when the official header is supplied.
    if (impl_->handle) {
        NpubitReleaseHandle(impl_->handle);
    }
}

std::shared_ptr<NpubitUbHandle> NpubitUbHandle::Create(const void* object, size_t bytes) noexcept
{
    try {
        if (!object || bytes == 0) {
            return {};
        }
        auto impl = std::make_unique<Impl>();
        const auto* data = static_cast<const uint8_t*>(object);
        impl->object.assign(data, data + bytes);
        auto result = std::make_shared<NpubitUbHandle>(std::move(impl));
        const auto status = NpubitCreateHandle(result->impl_->object.data(), bytes, &result->impl_->handle, V100, true);
        // Success == 0 is an integration assumption, confined to this adapter.
        if (static_cast<int>(status) != 0 || !result->impl_->handle) {
            ASCTOOL_ERROR("acl_san NpubitCreateHandle failed, status=%d", static_cast<int>(status));
            return {};
        }
        return result;
    } catch (...) {
        ASCTOOL_ERROR("acl_san NpubitCreateHandle: cannot retain original object");
        return {};
    }
}

bool NpubitUbHandle::Matches(const void* object, size_t bytes) const noexcept
{
    return object && bytes == impl_->object.size() && std::memcmp(object, impl_->object.data(), bytes) == 0;
}

std::vector<VfCallbackData> NpubitUbHandle::Query(
    const ParsedTraceRecord& vf, const std::array<uint64_t, PUSH_PB_STATE_COUNT_PER_VF>& states,
    const std::array<uint64_t, VECTOR_MASK_REGISTER_COUNT>& masks, AclsanDeviceVfData* completion) noexcept
{
    auto local = MakeVfCompletion(vf, ACLSAN_DEVICE_VF_QUERY_FAILED);
    AclsanDeviceVfData& info = completion != nullptr ? *completion : local;
    info = local;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (vf.phyCoreId > std::numeric_limits<uint8_t>::max()) {
            ASCTOOL_ERROR("acl_san Npubit: physical core id cannot fit aicore_id: %u", vf.phyCoreId);
            return {};
        }
        NpubitPushPBStateRecord input{};
        // HiBit treats these fields as labels, not hardware-state selectors.
        // Callback execution identity and UB ownership come from the parent trace.
        input.aicore_id = static_cast<uint8_t>(vf.phyCoreId);
        const auto aic = AicPhysicalCoreForAiv(vf.phyCoreId);
        if (aic == UINT32_MAX) {
            ASCTOOL_ERROR("acl_san Npubit: invalid AIV topology core=%u", vf.phyCoreId);
            return {};
        }
        const auto firstAiv = AivPhysicalCoreForAic(aic, 0);
        input.subcore_type = vf.phyCoreId == firstAiv ? NPUBIT_SUBCORE_TYPE_VEC0 : NPUBIT_SUBCORE_TYPE_VEC1;
        input.vf_instr_address = vf.record.pc; // Original instruction PC, not args[0] (VF entry).
        std::copy(states.begin(), states.end(), input.states);
        // Hardware register order: MASK[0] is low, MASK[1] is high.
        std::copy(masks.begin(), masks.end(), input.mask);
        ASCTOOL_DEBUG(
            "acl_san NpubitGetUbAddressRanges input pc=0x%llx aicoreId=%u subcoreType=%u",
            static_cast<unsigned long long>(input.vf_instr_address), static_cast<unsigned int>(input.aicore_id),
            static_cast<unsigned int>(input.subcore_type));
        for (uint32_t index = 0; index < PUSH_PB_STATE_COUNT_PER_VF; ++index) {
            ASCTOOL_DEBUG(
                "acl_san NpubitGetUbAddressRanges input state[%u]=0x%llx", index,
                static_cast<unsigned long long>(input.states[index]));
        }
        for (uint32_t index = 0; index < VECTOR_MASK_REGISTER_COUNT; ++index) {
            ASCTOOL_DEBUG(
                "acl_san NpubitGetUbAddressRanges input mask[%u]=0x%llx", index,
                static_cast<unsigned long long>(input.mask[index]));
        }
        // Free while still holding the query mutex, before another query can invalidate vendor storage.
        struct Output {
            NpubitUbAddressRange* data = nullptr;
            ~Output()
            {
                if (data != nullptr) {
                    void* ptr = static_cast<void*>(data);
                    NpubitFree(&ptr);
                    data = nullptr;
                }
            }
        } output;
        size_t count = 0;
        // HiBit guarantees that success returns every dynamic access and barrier,
        // including a valid empty expansion. Conversion and delivery are checked below.
        const auto status = NpubitGetUbAddressRanges(impl_->handle, &input, &output.data, &count);
        ASCTOOL_DEBUG(
            "acl_san NpubitGetUbAddressRanges result status=%d count=%zu data=%p", static_cast<int>(status), count,
            static_cast<void*>(output.data));
        if (output.data != nullptr && count <= MAX_VF_EVENTS) {
            for (size_t index = 0; index < count; ++index) {
                const auto& range = output.data[index];
                ASCTOOL_DEBUG(
                    "acl_san NpubitGetUbAddressRanges result range[%zu] pc=0x%llx address=0x%llx "
                    "bytes=0x%llx accessType=%u",
                    index, static_cast<unsigned long long>(range.pc),
                    static_cast<unsigned long long>(range.target_address),
                    static_cast<unsigned long long>(range.processed_bytes),
                    static_cast<unsigned int>(range.access_type));
            }
        }
        if (static_cast<int>(status) != 0 || (count != 0 && output.data == nullptr) || count > MAX_VF_EVENTS) {
            ASCTOOL_ERROR(
                "acl_san NpubitGetUbAddressRanges failed, pc=0x%llx status=%d",
                static_cast<unsigned long long>(vf.record.pc), static_cast<int>(status));
            return {};
        }
        info.rawEventCount = static_cast<uint32_t>(count);
        std::vector<VfCallbackData> result;
        result.reserve(count);
        uint32_t accessCount = 0;
        std::set<uint64_t> executions;
        std::map<uint64_t, uint32_t> fragmentCounts;
        std::optional<uint64_t> previousSequence;
        uint64_t previousPc = 0;
        NpubitUbAccessType previousType{};
        for (size_t i = 0; i < count; ++i) {
            const auto& range = output.data[i];
            const auto dynamicIndex = DynamicIndex<NpubitUbAddressRange>::Read(range);
            if (!dynamicIndex) {
                info.status = ACLSAN_DEVICE_VF_UNSUPPORTED;
                return {};
            }
            if (previousSequence && *previousSequence == *dynamicIndex) {
                if (previousPc != range.pc || previousType != range.access_type ||
                    LocalMemBarrierTypes(range.access_type)) {
                    return {};
                }
            } else if (!executions.insert(*dynamicIndex).second) {
                return {};
            }
            previousSequence = *dynamicIndex;
            previousPc = range.pc;
            previousType = range.access_type;
            // Keep original PCs and ordering for both internal LD/ST and SMEM_BAR records.
            AclsanDeviceEventHeader header{
                ACLSAN_API_VERSION,
                0,
                vf.launchId,
                range.pc,
                vf.record.siteId,
                ACLSAN_DEVICE_SOURCE_VECTOR,
                vf.instrExecId,
                i,
                vf.deviceId,
                vf.phyCoreId,
                vf.blockId,
                vf.blockType,
                ACLSAN_DEVICE_PIPE_VECTOR,
                ACLSAN_DEVICE_EVENT_FLAG_EXACT};
            if (const auto types = LocalMemBarrierTypes(range.access_type)) {
                AclsanDeviceBarrierData barrier{};
                barrier.header = header;
                barrier.header.size = sizeof(barrier);
                barrier.pipe = ACLSAN_DEVICE_PIPE_INVALID;
                barrier.barrierKind = ACLSAN_DEVICE_BARRIER_LOCAL_MEM;
                barrier.srcMemType = types->first;
                barrier.dstMemType = types->second;
                barrier.vfInstrExecSeq = *dynamicIndex;
                result.emplace_back(barrier);
                continue;
            }
            AclsanDeviceMemoryAccessMode mode;
            uint32_t sourceKind = ACLSAN_DEVICE_SOURCE_VECTOR;
            switch (range.access_type) {
                case NPUBIT_UB_ACCESS_VECTOR_READ:
                    mode = ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ;
                    break;
                case NPUBIT_UB_ACCESS_VECTOR_WRITE:
                    mode = ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE;
                    break;
                case NPUBIT_UB_ACCESS_SCALAR_READ:
                    mode = ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ;
                    sourceKind = ACLSAN_DEVICE_SOURCE_SCALAR;
                    break;
                case NPUBIT_UB_ACCESS_SCALAR_WRITE:
                    mode = ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE;
                    sourceKind = ACLSAN_DEVICE_SOURCE_SCALAR;
                    break;
                default:
                    ASCTOOL_ERROR("acl_san Npubit: invalid access mode");
                    return {};
            }
            // Validate the type before filtering zero-byte records.
            if (range.processed_bytes == 0) {
                continue;
            }
            AclsanDeviceMemoryAccessData access{};
            access.header = header;
            access.header.size = sizeof(access);
            access.header.sourceKind = sourceKind;
            access.address = range.target_address;
            access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
            access.accessMode = mode;
            access.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
            access.layout.range.bytes = range.processed_bytes;
            access.instructionId = vf.record.instrId;
            access.memoryOwner = header.executor;
            access.vfInstrExecSeq = *dynamicIndex;
            access.vfInstrValid = 1;
            access.accessIndex = fragmentCounts[*dynamicIndex]++;
            ++accessCount;
            result.emplace_back(access);
        }
        for (auto& event : result) {
            if (auto* access = std::get_if<AclsanDeviceMemoryAccessData>(&event)) {
                access->accessCount = fragmentCounts.at(access->vfInstrExecSeq);
            }
        }
        info.status = ACLSAN_DEVICE_VF_SUCCESS;
        info.accessCount = accessCount;
        info.eventCount = static_cast<uint32_t>(result.size());
        return result;
    } catch (...) {
        info.status = ACLSAN_DEVICE_VF_QUERY_FAILED;
        ASCTOOL_ERROR("acl_san Npubit: failed to convert UB ranges");
        return {};
    }
}

} // namespace aclsan
