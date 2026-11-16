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
#include "aclsan_trace_runtime.h"
#include "aclsan_dispatch.h"
#include "aclsan_register_dependency.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"
#include "device_instr/common/instruction_id.h"
#include "checker/memcheck.h"
#include "mock.h"
#include "plog_test_library.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#ifdef NPUBIT_TEST_REGISTRY
#include "device_runtime/device_binary_registry.h"
#endif
#include <cassert>
#include <iostream>
#include <limits>
using namespace aclsan;
namespace {
std::vector<AclsanDeviceMemoryAccessData> accesses;
std::vector<AclsanDeviceBarrierData> barriers;
std::vector<AclsanDeviceVfData> expansions;
std::vector<AclsanDeviceRegisterStateData> registerStates;
std::vector<AclsanDeviceEventHeader> events;
std::vector<std::string> logs;
int32_t EnableLog(int32_t, int32_t) { return 1; }
void CaptureLog(int32_t, int32_t, const char* format, ...)
{
    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logs.emplace_back(buffer);
}
ParsedTraceRecord Record(InstructionId id, uint64_t pc)
{
    ParsedTraceRecord r{};
    r.physicalCoreSlotCount = 108;
    r.launchId = 42;
    r.phyCoreId = 22;
    r.blockId = 2;
    r.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    r.deviceId = 1;
    r.instrExecId = 3;
    r.record.siteId = 9;
    r.record.instrId = static_cast<uint32_t>(id);
    r.record.pc = pc;
    return r;
}
} // namespace
// Capture the real DispatchTraceRecords output at the existing callback boundary.
namespace aclsan {
void AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& a) noexcept
{
    accesses.push_back(a);
    events.push_back(a.header);
}
void AclsanCallbackDispatcher::DispatchDeviceState(const AclsanDeviceRegisterStateData& state) noexcept
{
    registerStates.push_back(state);
}
void AclsanCallbackDispatcher::DispatchDeviceVf(const AclsanDeviceVfData& v) noexcept { expansions.push_back(v); }
void AclsanCallbackDispatcher::DispatchDeviceSync(const AclsanDeviceSyncData&) noexcept {}
void AclsanCallbackDispatcher::DispatchDeviceBarrier(const AclsanDeviceBarrierData& b) noexcept
{
    barriers.push_back(b);
    events.push_back(b.header);
}
} // namespace aclsan
int main()
{
    plog_test::SetApi({EnableLog, CaptureLog});
    auto push = Record(InstructionId::PushPb, 0x1cc);
    auto vf = Record(InstructionId::Vf, 0x1e8);
    push.record.args[0] = 0x1000000003e;
    push.record.args[1] = 0x200000;
    push.record.args[2] = push.record.args[3] = 0x400000002000;
    vf.record.args[0] = 0xABC00228;
    vf.record.args[1] = 9;
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    assert(IsDefinedInstructionId(push.record.instrId) && IsDefinedInstructionId(vf.record.instrId));
    assert(std::get<PushPbParamField>(decoder.decode(push.record)->params).values[2] == push.record.args[2]);
    assert(std::get<VfParamField>(decoder.decode(vf.record)->params).entryPc == 0xABC00228);
    std::vector<uint8_t> object{0x7f, 'E', 'L', 'F', 1};
    auto handle = NpubitUbHandle::Create(object.data(), object.size());
    assert(handle && handle->Matches(object.data(), object.size()));
    object.back() = 2; // Adapter must own the original bytes rather than this caller's buffer.
    assert(!handle->Matches(object.data(), object.size()));
    VfPbState state;
    hibit_test::ranges = {
        {0x234, 0, 8192, NPUBIT_UB_ACCESS_VECTOR_READ},
        {0x238, 8192, 8192, NPUBIT_UB_ACCESS_VECTOR_READ},
        {0x244, 16384, 8192, NPUBIT_UB_ACCESS_VECTOR_WRITE}};
    for (size_t i = 0; i < hibit_test::ranges.size(); ++i) {
        hibit_test::ranges[i].instruction_execution_number = 10 + i;
    }
    DispatchTraceRecords({push}, decoder, nullptr, nullptr, nullptr, handle.get(), &state);
    assert(hibit_test::queries.empty());
    DispatchTraceRecords({vf}, decoder, nullptr, nullptr, nullptr, handle.get(), &state);
    assert(accesses.size() == 3 && hibit_test::queries.size() == 1);
    assert(expansions.size() == 1 && expansions.back().status == ACLSAN_DEVICE_VF_SUCCESS);
    assert(expansions.back().accessCount == 3 && expansions.back().eventCount == 3);
    assert(expansions.back().warningFlags == ACLSAN_DEVICE_VF_MASK_NOT_REFRESHED);
    assert(std::any_of(logs.begin(), logs.end(), [](const std::string& text) {
        return text.find("[vf completion]") != std::string::npos &&
               text.find("accessCount=3 eventCount=3 rawEventCount=3") != std::string::npos;
    }));
    for (size_t i = 0; i < accesses.size(); ++i) {
        assert(accesses[i].vfInstrValid == 1 && accesses[i].vfInstrExecSeq == 10 + i);
    }
    const auto& input = hibit_test::queries.back();
    assert(input.vf_instr_address == 0x1e8 && input.aicore_id == 22 && input.subcore_type == NPUBIT_SUBCORE_TYPE_VEC0);
    for (unsigned i = 0; i < 16; ++i) {
        assert(input.states[i] == (i < 4 ? push.record.args[i] : 0));
    }
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find("NpubitGetUbAddressRanges input pc=0x1e8 aicoreId=22 subcoreType=0") != std::string::npos;
    }));
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find("NpubitGetUbAddressRanges input state[0]=0x1000000003e") != std::string::npos;
    }));
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find("NpubitGetUbAddressRanges input state[15]=0x0") != std::string::npos;
    }));
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find("NpubitGetUbAddressRanges input mask[0]=0xffffffffffffffff") != std::string::npos;
    }));
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find("NpubitGetUbAddressRanges input mask[1]=0xffffffffffffffff") != std::string::npos;
    }));
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find("NpubitGetUbAddressRanges result status=0 count=3") != std::string::npos;
    }));
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find(
                   "NpubitGetUbAddressRanges result range[2] pc=0x244 address=0x4000 bytes=0x2000 accessType=1") !=
               std::string::npos;
    }));
    for (unsigned i = 0; i < 3; ++i) {
        const auto& a = accesses[i];
        assert(a.header.pc == hibit_test::ranges[i].pc && a.header.instrExecId == vf.instrExecId);
        assert(a.header.launchId == 42 && a.header.executor.phyCoreId == 22 && a.header.executor.blockId == 2);
        assert(a.header.sourceKind == ACLSAN_DEVICE_SOURCE_VECTOR && a.header.pipeline == ACLSAN_DEVICE_PIPE_VECTOR);
        assert(a.accessIndex == 0 && a.accessCount == 1 && a.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
    }
    assert(accesses[0].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ);
    assert(accesses[2].accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE);
    // SMEM_BAR records share the vendor result stream with memory accesses.
    // Their address/byte fields have no memory-access meaning, including zero bytes.
    {
        struct BarrierCase {
            NpubitUbAccessType type;
            uint32_t src;
            uint32_t dst;
        };
        const BarrierCase cases[] = {
            {NPUBIT_SMEM_BAR_VV_ALL, ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL, ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL},
            {NPUBIT_SMEM_BAR_VST_VLD, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD},
            {NPUBIT_SMEM_BAR_VLD_VST, ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE},
            {NPUBIT_SMEM_BAR_VST_VST, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE},
            {NPUBIT_SMEM_BAR_VS_ALL, ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_ALL},
            {NPUBIT_SMEM_BAR_VST_LD, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_LOAD},
            {NPUBIT_SMEM_BAR_VLD_ST, ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE},
            {NPUBIT_SMEM_BAR_VST_ST, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE},
            {NPUBIT_SMEM_BAR_SV_ALL, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_ALL, ACLSAN_DEVICE_LOCAL_MEM_VEC_ALL},
            {NPUBIT_SMEM_BAR_ST_VLD, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_LOAD},
            {NPUBIT_SMEM_BAR_LD_VST, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_LOAD, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE},
            {NPUBIT_SMEM_BAR_ST_VST, ACLSAN_DEVICE_LOCAL_MEM_SCALAR_STORE, ACLSAN_DEVICE_LOCAL_MEM_VEC_STORE}};
        const auto originalAccesses = accesses;
        for (size_t i = 0; i < std::size(cases); ++i) {
            const auto& c = cases[i];
            accesses.clear();
            barriers.clear();
            events.clear();
            logs.clear();
            const uint64_t pc = i == 0 ? 0 : 0x240;
            hibit_test::ranges = {
                {0x234, 0, 4, NPUBIT_UB_ACCESS_VECTOR_WRITE, 10},
                {pc, UINT64_MAX, i == 0 ? 16u : 0u, c.type, 11},
                {0x244, 0, 0, NPUBIT_UB_ACCESS_VECTOR_READ, 12},
                {0x248, 0, 4, NPUBIT_UB_ACCESS_SCALAR_READ, 13}};
            DispatchTraceRecords({push, vf}, decoder, nullptr, nullptr, nullptr, handle.get(), &state);
            assert(barriers.size() == 1 && accesses.size() == 2 && events.size() == 3);
            const auto& b = barriers.front();
            assert(b.barrierKind == ACLSAN_DEVICE_BARRIER_LOCAL_MEM && b.pipe == ACLSAN_DEVICE_PIPE_INVALID);
            assert(b.srcMemType == c.src && b.dstMemType == c.dst);
            assert(b.vfInstrExecSeq == 11);
            assert(b.header.version == ACLSAN_API_VERSION && b.header.size == sizeof(b));
            assert(b.header.pc == pc && b.header.instrExecId == vf.instrExecId);
            assert(b.header.launchId == vf.launchId && b.header.siteId == vf.record.siteId);
            assert(b.header.deviceId == vf.deviceId && b.header.executor.phyCoreId == vf.phyCoreId);
            assert(b.header.executor.blockId == vf.blockId && b.header.executor.blockType == vf.blockType);
            assert(
                b.header.pipeline == ACLSAN_DEVICE_PIPE_VECTOR && b.header.sourceKind == ACLSAN_DEVICE_SOURCE_VECTOR);
            assert(b.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
            assert(events[0].pc == 0x234 && events[1].pc == pc && events[2].pc == 0x248);
            assert(events[0].serialNo == 0 && events[1].serialNo == 1 && events[2].serialNo == 3);
            assert(accesses[0].vfInstrExecSeq == 10 && accesses[1].vfInstrExecSeq == 13);
            for (size_t j = 0; j < accesses.size(); ++j) {
                assert(accesses[j].accessIndex == 0 && accesses[j].accessCount == 1);
            }
            const auto fields =
                "barrierKind=1 srcMemType=" + std::to_string(c.src) + " dstMemType=" + std::to_string(c.dst);
            assert(std::any_of(logs.begin(), logs.end(), [&](const auto& text) {
                return text.find("[cbdata] type=AclsanDeviceBarrierData") != std::string::npos &&
                       text.find(fields) != std::string::npos;
            }));
        }
        // A barrier-only result must be delivered even when it has no byte range.
        barriers.clear();
        accesses.clear();
        events.clear();
        hibit_test::ranges = {{0x240, 0, 0, NPUBIT_SMEM_BAR_VST_VLD}};
        DispatchTraceRecords({push, vf}, decoder, nullptr, nullptr, nullptr, handle.get(), &state);
        assert(barriers.size() == 1 && accesses.empty() && events.size() == 1);
        accesses = originalAccesses;
    }
    npucheck::Memcheck checker;
    for (const auto& a : accesses) {
        checker.QueueDeviceMemoryAccess(a);
    }
    assert(checker.OnSynchronization().empty());
    const auto queryCount = hibit_test::queries.size();
    DispatchTraceRecords({vf}, decoder, nullptr, nullptr, nullptr, handle.get(), &state);
    assert(hibit_test::queries.size() == queryCount); // No stale reuse.
    assert(expansions.back().status == ACLSAN_DEVICE_VF_INPUT_MISSING);
    accesses.clear();
    constexpr uint64_t ub = 248 * 1024;
    hibit_test::ranges = {
        {0x234, ub - 4, 4, NPUBIT_UB_ACCESS_VECTOR_READ},
        {0x238, ub - 4, 8, NPUBIT_UB_ACCESS_VECTOR_READ},
        {0x244, ub, 1, NPUBIT_UB_ACCESS_VECTOR_WRITE},
        {0x244, std::numeric_limits<uint64_t>::max(), 2, NPUBIT_UB_ACCESS_VECTOR_WRITE},
        {0x234, 1, std::numeric_limits<uint64_t>::max(), NPUBIT_UB_ACCESS_VECTOR_READ}};
    for (size_t i = 0; i < hibit_test::ranges.size(); ++i) {
        hibit_test::ranges[i].instruction_execution_number = i;
    }
    DispatchTraceRecords({push, vf}, decoder, nullptr, nullptr, nullptr, handle.get(), &state);
    for (const auto& a : accesses) {
        checker.QueueDeviceMemoryAccess(a);
    }
    const auto reports = checker.OnSynchronization();
    assert(reports.empty()); // UB bounds checking is outside the current Memcheck scope.
    assert(accesses.size() == 5);
    // Four sequential pushes occupy all slots; the next batch clears slots 4..15.
    VfPbState slots;
    for (uint64_t i = 0; i < 4; ++i) {
        assert(slots.Push(push, {i * 4 + 1, i * 4 + 2, i * 4 + 3, i * 4 + 4}));
    }
    auto snapshot = slots.Consume(vf);
    assert(snapshot);
    for (unsigned i = 0; i < 16; ++i) {
        assert((*snapshot)[i] == i + 1);
    }
    assert(slots.Push(push, {20, 21, 22, 23}));
    snapshot = slots.Consume(vf);
    assert(snapshot);
    assert((*snapshot)[0] == 20);
    for (unsigned i = 4; i < 16; ++i) {
        assert((*snapshot)[i] == 0);
    }
    for (unsigned i = 0; i < 4; ++i) {
        assert(slots.Push(push, {1, 2, 3, 4}));
    }
    assert(!slots.Push(push, {5, 6, 7, 8}) && !slots.Consume(vf));
    assert(slots.Push(push, {9, 10, 11, 12}) && slots.Consume(vf));
    {
        VfPbState overflow;
        accesses.clear();
        const auto completionsBefore = expansions.size();
        DispatchTraceRecords(
            {push, push, push, push, push}, decoder, nullptr, nullptr, nullptr, handle.get(), &overflow);
        assert(accesses.size() == 1 && (accesses.front().header.flags & ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR));
        assert(expansions.size() == completionsBefore);
        accesses.clear();
    }
    // Independent core/block/device/launch batches cannot contaminate each other.
    for (unsigned dimension = 0; dimension < 5; ++dimension) {
        VfPbState isolated;
        auto other = push;
        switch (dimension) {
            case 0:
                ++other.phyCoreId;
                break;
            case 1:
                ++other.blockId;
                break;
            case 2:
                ++other.launchId;
                break;
            case 3:
                ++other.deviceId;
                break;
            case 4:
                ++other.blockType;
                break;
        }
        isolated.Push(push, {1, 2, 3, 4});
        isolated.Push(other, {5, 6, 7, 8});
        assert(isolated.Consume(push)->at(0) == 1 && isolated.Consume(other)->at(0) == 5);
    }
    // Failure with allocated output still releases it; empty ranges emit no events.
    const int released = hibit_test::frees;
    hibit_test::queryStatus = 1;
    AclsanDeviceVfData completion{};
    assert(handle->Query(vf, {}, {UINT64_MAX, UINT64_MAX}, &completion).empty() && hibit_test::frees == released + 1);
    assert(completion.status == ACLSAN_DEVICE_VF_QUERY_FAILED);
    assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
        return text.find("NpubitGetUbAddressRanges failed") != std::string::npos;
    }));
    hibit_test::queryStatus = 0;
    hibit_test::ranges = {{0x234, 0, 0, NPUBIT_UB_ACCESS_VECTOR_READ}};
    assert(handle->Query(vf, {}, {UINT64_MAX, UINT64_MAX}, &completion).empty());
    assert(
        completion.status == ACLSAN_DEVICE_VF_SUCCESS && completion.accessCount == 0 && completion.rawEventCount == 1);
    hibit_test::ranges = {{0x234, 0, 0, static_cast<NpubitUbAccessType>(99)}};
    assert(handle->Query(vf, {}, {UINT64_MAX, UINT64_MAX}, &completion).empty());
    assert(completion.status == ACLSAN_DEVICE_VF_QUERY_FAILED);
    // Keep all four VF modes distinct while skipping zero-sized ranges.
    hibit_test::ranges = {
        {0x234, 0, 4, NPUBIT_UB_ACCESS_VECTOR_READ, 0},
        {0x238, 4, 4, NPUBIT_UB_ACCESS_VECTOR_WRITE, 1},
        {0x23c, 8, 0, NPUBIT_UB_ACCESS_SCALAR_READ, 2},
        {0x240, 8, 4, NPUBIT_UB_ACCESS_SCALAR_READ, 3},
        {0x244, 12, 4, NPUBIT_UB_ACCESS_SCALAR_WRITE, 4}};
    const auto modes = handle->Query(vf, {}, {UINT64_MAX, UINT64_MAX});
    assert(modes.size() == 4);
    const uint32_t expectedModes[] = {
        ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ, ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_WRITE,
        ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_READ, ACLSAN_DEVICE_MEMORY_ACCESS_VF_SCALAR_WRITE};
    for (size_t i = 0; i < modes.size(); ++i) {
        const auto& access = std::get<AclsanDeviceMemoryAccessData>(modes[i]);
        assert(access.accessMode == expectedModes[i]);
        assert(access.accessIndex == 0 && access.accessCount == 1);
        assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
        assert(access.header.pipeline == ACLSAN_DEVICE_PIPE_VECTOR);
        assert(access.header.sourceKind == (i < 2 ? ACLSAN_DEVICE_SOURCE_VECTOR : ACLSAN_DEVICE_SOURCE_SCALAR));
        assert(access.address == i * 4 && access.layout.range.bytes == 4);
    }
    hibit_test::ranges = {
        {0x234, ub - 4, 8, NPUBIT_UB_ACCESS_SCALAR_READ}, {0x238, ub - 4, 8, NPUBIT_UB_ACCESS_SCALAR_WRITE, 1}};
    accesses.clear();
    DispatchTraceRecords({push, vf}, decoder, nullptr, nullptr, nullptr, handle.get(), &state);
    assert(accesses.size() == 2);
    for (const auto& a : accesses) {
        checker.QueueDeviceMemoryAccess(a);
    }
    const auto scalarReports = checker.OnSynchronization();
    assert(scalarReports.empty());
    // A returned PC of zero is valid and must not be replaced by the VF call PC.
    hibit_test::ranges = {{0, 0, 4, NPUBIT_UB_ACCESS_VECTOR_READ}};
    const auto zeroPc = handle->Query(vf, {}, {UINT64_MAX, UINT64_MAX});
    assert(zeroPc.size() == 1 && std::get<AclsanDeviceMemoryAccessData>(zeroPc.front()).header.pc == 0);
    // Fragment identity is scoped to one dynamic execution, including sequence zero.
    hibit_test::ranges = {
        {0x234, 0, 4, NPUBIT_UB_ACCESS_VECTOR_READ, 0},
        {0x234, 8, 4, NPUBIT_UB_ACCESS_VECTOR_READ, 0},
        {0x238, 0, 4, NPUBIT_UB_ACCESS_VECTOR_WRITE, 9}};
    auto grouped = handle->Query(vf, {}, {}, &completion);
    assert(grouped.size() == 3 && completion.accessCount == 3);
    const auto& firstFragment = std::get<AclsanDeviceMemoryAccessData>(grouped[0]);
    const auto& secondFragment = std::get<AclsanDeviceMemoryAccessData>(grouped[1]);
    assert(firstFragment.header.instrExecId == secondFragment.header.instrExecId);
    assert(firstFragment.header.instrExecId == vf.instrExecId);
    assert(firstFragment.accessCount == 2 && secondFragment.accessIndex == 1);
    assert(firstFragment.memoryOwner.phyCoreId == vf.phyCoreId);
    const auto repeated = handle->Query(vf, {}, {}, &completion);
    assert(repeated.size() == grouped.size());
    for (size_t i = 0; i < grouped.size(); ++i) {
        const auto& first = std::get<AclsanDeviceMemoryAccessData>(grouped[i]);
        const auto& again = std::get<AclsanDeviceMemoryAccessData>(repeated[i]);
        assert(again.header.instrExecId == first.header.instrExecId && again.vfInstrExecSeq == first.vfInstrExecSeq);
    }
    std::swap(hibit_test::ranges[1], hibit_test::ranges[2]);
    assert(handle->Query(vf, {}, {}, &completion).empty() && completion.status != ACLSAN_DEVICE_VF_SUCCESS);
    hibit_test::ranges.resize(2);
    hibit_test::ranges[1].instruction_execution_number = 0;
    assert(handle->Query(vf, {}, {}, &completion).empty() && completion.status != ACLSAN_DEVICE_VF_SUCCESS);
    hibit_test::ranges = {{0, 0, 4, NPUBIT_UB_ACCESS_VECTOR_READ, 0}};
    for (const auto core : {18U, 19U, 72U, 73U}) {
        auto mapped = vf;
        mapped.phyCoreId = core;
        assert(!handle->Query(mapped, {}, {}, &completion).empty());
        assert(
            hibit_test::queries.back().subcore_type ==
            ((core == 18 || core == 72) ? NPUBIT_SUBCORE_TYPE_VEC0 : NPUBIT_SUBCORE_TYPE_VEC1));
    }
    auto tooLarge = vf;
    tooLarge.phyCoreId = 256;
    assert(handle->Query(tooLarge, {}, {UINT64_MAX, UINT64_MAX}).empty());
    hibit_test::createStatus = 1;
    assert(!NpubitUbHandle::Create(object.data(), object.size()));
    hibit_test::createStatus = 0;
#ifdef NPUBIT_TEST_REGISTRY
    {
        device_runtime::DeviceBinaryRegistry registry;
        const int before = hibit_test::creates;
        registry.RecordBinaryLoadFromData(10, true, 24, object.data(), object.size());
        registry.RecordBinaryLoadFromData(11, true, 32, object.data(), object.size());
        registry.RecordBinaryFunctionLookup(10, 100, "a");
        registry.RecordBinaryFunctionLookup(11, 101, "b");
        auto first = registry.GetFunctionHibitHandle(100);
        auto second = registry.GetFunctionHibitHandle(101);
        assert(first && first == second && hibit_test::creates == before + 1);
        accesses.clear();
        DispatchTraceRecords({push, vf}, decoder, nullptr, nullptr, nullptr, first.get());
        assert(accesses.size() == 1 && accesses.front().accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_VF_VECTOR_READ);
        object.back() = 3;
        registry.RecordBinaryLoadFromData(12, true, 24, object.data(), object.size());
        registry.RecordBinaryFunctionLookup(12, 102, "c");
        assert(registry.GetFunctionHibitHandle(102) != first && hibit_test::creates == before + 2);
        assert(!registry.GetFunctionHibitHandle(999));
        registry.RecordBinaryUnload(10);
        registry.RecordBinaryUnload(11);
        assert(!registry.GetFunctionHibitHandle(100));
        const int destroyed = hibit_test::destroys;
        first->Query(vf, {}, {UINT64_MAX, UINT64_MAX}); // Launch reference remains usable after unload.
        first.reset();
        second.reset();
        assert(hibit_test::destroys == destroyed + 1);
    }
#endif
    // Mask writes persist across collection batches; each VF gets a fresh snapshot.
    {
        dav3510::Dav3510RegisterStateManager registers(vf.launchId);
        VfPbState pb;
        auto low = Record(InstructionId::SetVectorMask, 0x180);
        auto high = Record(InstructionId::SetVectorMask, 0x184);
        low.record.args[0] = 0;
        low.record.args[1] = 0x0123456789abcdefULL;
        high.record.args[0] = 1;
        high.record.args[1] = 0xfedcba9876543210ULL;
        assert(IsDefinedInstructionId(low.record.instrId) && IsDefinedInstructionId(high.record.instrId));
        const auto decodedLow = std::get<VectorMaskWriteParamField>(decoder.decode(low.record)->params);
        const auto decodedHigh = std::get<VectorMaskWriteParamField>(decoder.decode(high.record)->params);
        assert(decodedLow.index == 0 && decodedLow.value == low.record.args[1]);
        assert(decodedHigh.index == 1 && decodedHigh.value == high.record.args[1]);
        registerStates.clear();
        logs.clear();
        // The first write changes only one half, starting from the full-mask default.
        DispatchTraceRecords({high, push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(registerStates.size() == 1);
        const auto& highState = registerStates.front();
        assert(highState.regId == ACLSAN_DEVICE_REGISTER_VECTOR_MASK && highState.value == high.record.args[1]);
        assert(highState.regIndex == 1);
        assert(highState.header.size == sizeof(highState) && highState.header.version == ACLSAN_API_VERSION);
        assert(highState.header.pc == high.record.pc && highState.header.siteId == high.record.siteId);
        assert(highState.header.launchId == high.launchId && highState.header.instrExecId == high.instrExecId);
        assert(highState.header.deviceId == high.deviceId && highState.header.executor.phyCoreId == high.phyCoreId);
        assert(
            highState.header.executor.blockType == high.blockType && highState.header.executor.blockId == high.blockId);
        assert(
            highState.header.sourceKind == ACLSAN_DEVICE_SOURCE_SCALAR &&
            highState.header.pipeline == high.record.pipeline &&
            highState.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
        assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
            return text.find("[cbdata] type=AclsanDeviceRegisterStateData") != std::string::npos &&
                   text.find("regId=0 regIndex=1 value=0xfedcba9876543210") != std::string::npos;
        }));
        assert(hibit_test::queries.back().mask[0] == UINT64_MAX);
        assert(hibit_test::queries.back().mask[1] == high.record.args[1]);
        assert(expansions.back().warningFlags == 0);
        DispatchTraceRecords({push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(expansions.back().warningFlags == 0); // The prior refresh persists.
        DispatchTraceRecords({high, low, push}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(registerStates.size() == 3 && registerStates[2].value == low.record.args[1]);
        assert(registerStates[1].regIndex == 1 && registerStates[2].regIndex == 0);
        assert(std::any_of(logs.begin(), logs.end(), [](const auto& text) {
            return text.find("[cbdata] type=AclsanDeviceRegisterStateData") != std::string::npos &&
                   text.find("regId=0 regIndex=0 value=0x123456789abcdef") != std::string::npos;
        }));
        DispatchTraceRecords({vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(registerStates.size() == 3); // Querying a VF does not synthesize mask writes.
        assert(hibit_test::queries.back().mask[0] == low.record.args[1]);
        assert(hibit_test::queries.back().mask[1] == high.record.args[1]);
        // Zero is a valid update; updating low must preserve high.
        low.record.args[1] = 0;
        DispatchTraceRecords({low, push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(registerStates.size() == 4 && registerStates.back().value == 0);
        assert(registerStates.back().regIndex == 0);
        assert(hibit_test::queries.back().mask[0] == 0);
        assert(hibit_test::queries.back().mask[1] == high.record.args[1]);
        // Interleaved identities do not overwrite the first core's masks.
        for (unsigned dimension = 0; dimension < 4; ++dimension) {
            auto other = high;
            switch (dimension) {
                case 0:
                    ++other.phyCoreId;
                    break;
                case 1:
                    ++other.blockId;
                    break;
                case 2:
                    ++other.deviceId;
                    break;
                case 3:
                    ++other.blockType;
                    break;
            }
            other.record.args[1] = 0x55;
            DispatchTraceRecords({other, push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
            const auto& state = registerStates.back();
            assert(
                state.header.executor.phyCoreId == other.phyCoreId && state.header.executor.blockId == other.blockId);
            assert(state.header.deviceId == other.deviceId && state.header.executor.blockType == other.blockType);
            assert(state.value == other.record.args[1]);
            assert(state.regIndex == 1);
            assert(hibit_test::queries.back().mask[0] == 0);
            assert(hibit_test::queries.back().mask[1] == high.record.args[1]);
        }
        high.record.args[1] = 7;
        DispatchTraceRecords({high, push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(hibit_test::queries.back().mask[0] == 0 && hibit_test::queries.back().mask[1] == 7);
        dav3510::Dav3510RegisterStateManager nextLaunch(vf.launchId + 1);
        auto nextPush = push;
        auto nextVf = vf;
        ++nextPush.launchId;
        ++nextVf.launchId;
        DispatchTraceRecords({nextPush, nextVf}, decoder, nullptr, &nextLaunch, nullptr, handle.get(), &pb);
        assert(hibit_test::queries.back().mask[0] == UINT64_MAX && hibit_test::queries.back().mask[1] == UINT64_MAX);
        registers.Reset();
        const auto stateCount = registerStates.size();
        DispatchTraceRecords({push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(registerStates.size() == stateCount);
        assert(hibit_test::queries.back().mask[0] == UINT64_MAX);
        assert(hibit_test::queries.back().mask[1] == UINT64_MAX);
        // MOVEMASK 93 writes one indexed half; the next VF receives both updates.
        auto high93 = Record(InstructionId::SetVectorMask, 0x188);
        high93.record.args[0] = 1;
        high93.record.args[1] = UINT64_MAX;
        auto low93 = Record(InstructionId::SetVectorMask, 0x18c);
        low93.record.args[0] = 0;
        low93.record.args[1] = UINT64_MAX;
        registerStates.clear();
        DispatchTraceRecords({high93, low93, push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(registerStates.size() == 2);
        assert(registerStates[0].regIndex == 1 && registerStates[0].value == UINT64_MAX);
        assert(registerStates[1].regIndex == 0 && registerStates[1].value == UINT64_MAX);
        assert(hibit_test::queries.back().mask[0] == UINT64_MAX);
        assert(hibit_test::queries.back().mask[1] == UINT64_MAX);
        high93.record.args[1] = 0xffff;
        low93.record.args[1] = 0;
        DispatchTraceRecords({high93, low93, push, vf}, decoder, nullptr, &registers, nullptr, handle.get(), &pb);
        assert(hibit_test::queries.back().mask[0] == 0);
        assert(hibit_test::queries.back().mask[1] == 0xffff);
        auto invalid = *decoder.decode(low.record);
        std::get<VectorMaskWriteParamField>(invalid.params).index = 2;
        assert(!MakeRegisterStateData(low, invalid));
    }
    handle.reset();
    assert(hibit_test::creates == hibit_test::destroys);
    plog_test::ResetApi();
    std::cout << "Npubit PB/decoder/dispatch/UB checks passed" << std::endl;
}
