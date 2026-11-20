/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "aclsan_dispatch.h"
#include "aclsan_device_data.h"
#include "aclsan_device_data_log.h"
#include "aclsan_register_dependency.h"
#include "aclsan_trace_runtime.h"
#include "aclsan_vf.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"
#include "device_instr/common/instruction_id.h"
#include "plog_test_library.h"
#include "npu_tool_log.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using namespace aclsan;
namespace {
std::vector<AclsanDeviceRegisterStateData> registerStates;
std::vector<AclsanDeviceMemoryAccessData> memoryAccesses;
std::vector<AclsanDeviceVfData> vfCompletions;
std::string logs;
unsigned errorCount = 0;
unsigned instructionCallbacks = 0;

int32_t EnableLog(int32_t, int32_t) { return 1; }
void CaptureLog(int32_t, int32_t level, const char* format, ...)
{
    char buffer[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logs += buffer;
    if (level == DLOG_ERROR) {
        ++errorCount;
    }
}

ParsedTraceRecord Record(InstructionId id, uint64_t pc)
{
    ParsedTraceRecord record{};
    record.launchId = 42;
    record.deviceId = 1;
    record.phyCoreId = 7;
    record.blockId = 2;
    record.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    record.instrExecId = 3;
    record.record.siteId = 9;
    record.record.instrId = static_cast<uint32_t>(id);
    record.record.pc = pc;
    record.record.pipeline = ACLSAN_DEVICE_PIPE_SCALAR;
    return record;
}

void CheckState(
    const AclsanDeviceRegisterStateData& state, const ParsedTraceRecord& record, uint32_t index, uint64_t value)
{
    assert(state.regId == ACLSAN_DEVICE_REGISTER_VECTOR_MASK && state.regIndex == index && state.value == value);
    const auto& header = state.header;
    assert(header.version == ACLSAN_API_VERSION && header.size == sizeof(state));
    assert(header.pc == record.record.pc && header.siteId == record.record.siteId);
    assert(header.launchId == record.launchId && header.instrExecId == record.instrExecId);
    assert(header.deviceId == record.deviceId && header.executor.phyCoreId == record.phyCoreId);
    assert(header.executor.blockId == record.blockId && header.executor.blockType == record.blockType);
    assert(header.sourceKind == ACLSAN_DEVICE_SOURCE_SCALAR && header.pipeline == record.record.pipeline);
    assert(header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
}

void TestPushPbAndVfCollection()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    auto push = Record(InstructionId::PushPb, 0x1cc);
    push.record.args[0] = 0x1000000003e;
    push.record.args[1] = 0x200000;
    push.record.args[2] = 0;
    push.record.args[3] = UINT64_MAX;
    auto vf = Record(InstructionId::Vf, 0x1e8);
    vf.record.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    vf.record.args[0] = 0xABC00228;
    vf.record.args[1] = 9;
    assert(IsDefinedInstructionId(push.record.instrId) && IsDefinedInstructionId(vf.record.instrId));
    const auto decodedPush = decoder.decode(push.record);
    const auto decodedVf = decoder.decode(vf.record);
    assert(decodedPush && decodedPush->kind == DeviceInstructionKind::PushPb);
    assert(decodedVf && decodedVf->kind == DeviceInstructionKind::Vf);
    const auto& values = std::get<PushPbParamField>(decodedPush->params).values;
    for (unsigned index = 0; index < values.size(); ++index) {
        assert(values[index] == push.record.args[index]);
    }
    const auto& params = std::get<VfParamField>(decodedVf->params);
    assert(params.entryPc == vf.record.args[0] && params.instructionCount == 9);
    DispatchTraceRecords({push, vf}, decoder);
    assert(
        logs.find("[param] type=PushPbParamField values=[0x1000000003e,0x200000,0x0,0xffffffffffffffff]") !=
        std::string::npos);
    assert(logs.find("[param] type=VfParamField entryPc=0xabc00228 instructionCount=9") != std::string::npos);
    assert(errorCount == 1 && instructionCallbacks == 0 && registerStates.empty());
    assert(vfCompletions.size() == 1 && vfCompletions.back().status == ACLSAN_DEVICE_VF_INPUT_MISSING);

    // Missing state and oversized PB batches are reported through the callback boundary.
    DispatchTraceRecords({vf}, decoder);
    DispatchTraceRecords({push, push, push, push, push}, decoder);
    vf.record.args[1] = 0;
    DispatchTraceRecords({vf}, decoder);
    assert(vfCompletions.size() == 3 && vfCompletions.back().status == ACLSAN_DEVICE_VF_INPUT_MISSING);
    assert(instructionCallbacks == 1 && memoryAccesses.size() == 1);
    assert((memoryAccesses.back().header.flags & ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR) != 0);
    assert(registerStates.empty());
    errorCount = 0;
    instructionCallbacks = 0;
    memoryAccesses.clear();
    vfCompletions.clear();
}

void TestVectorMaskCollection()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    dav3510::Dav3510RegisterStateManager registers(42);
    auto high = Record(InstructionId::SetVectorMask, 0x184);
    high.record.args[0] = 1;
    high.record.args[1] = 0xfedcba9876543210;
    auto low = Record(InstructionId::SetVectorMask, 0x180);
    low.record.args[0] = 0;
    low.record.args[1] = 0x0123456789abcdef;
    const dav3510::Dav3510CoreKey key{high.blockType, high.blockId, high.phyCoreId, high.deviceId};
    DispatchTraceRecords({high}, decoder, nullptr, &registers);
    assert(registerStates.size() == 1);
    CheckState(registerStates.back(), high, 1, high.record.args[1]);
    assert(registers.Get(key)->vectorMask->vectorMask0 == UINT64_MAX);
    DispatchTraceRecords({low}, decoder, nullptr, &registers);
    assert(registerStates.size() == 2);
    CheckState(registerStates.back(), low, 0, low.record.args[1]);
    assert(registers.Get(key)->vectorMask->vectorMask1 == high.record.args[1]);

    low.record.args[1] = 0;
    DispatchTraceRecords({low}, decoder, nullptr, &registers);
    CheckState(registerStates.back(), low, 0, 0);
    assert(registers.Get(key)->vectorMask->vectorMask0 == 0);
    assert(registers.Get(key)->vectorMask->vectorMask1 == high.record.args[1]);
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
        DispatchTraceRecords({other}, decoder, nullptr, &registers);
        CheckState(registerStates.back(), other, 1, 0x55);
        assert(registers.Get(key)->vectorMask->vectorMask0 == 0);
        assert(registers.Get(key)->vectorMask->vectorMask1 == high.record.args[1]);
    }

    // The operand selects the mask half.
    {
        auto operand = Record(InstructionId::SetVectorMask, 0x180);
        for (uint64_t index = 0; index < 2; ++index) {
            operand.record.args[0] = index;
            operand.record.args[1] = UINT64_MAX;
            const auto decoded = decoder.decode(operand.record);
            assert(decoded);
            const auto& mask = std::get<VectorMaskWriteParamField>(decoded->params);
            assert(mask.index == index && mask.value == UINT64_MAX);
        }
        for (const uint64_t index : {uint64_t{2}, uint64_t{1} << 32, UINT64_MAX}) {
            operand.record.args[0] = index;
            assert(!decoder.decode(operand.record));
        }
        operand.record.args[0] = 0;
        for (const uint16_t legacyId : {uint16_t{165}, uint16_t{166}}) {
            operand.record.instrId = legacyId;
            assert(!IsDefinedInstructionId(legacyId));
            assert(!decoder.decode(operand.record));
        }
    }

    auto high93 = Record(InstructionId::SetVectorMask, 0x188);
    high93.record.args[0] = 1;
    high93.record.args[1] = UINT64_MAX;
    auto low93 = Record(InstructionId::SetVectorMask, 0x18c);
    low93.record.args[0] = 0;
    low93.record.args[1] = UINT64_MAX;
    registerStates.clear();
    DispatchTraceRecords({high93, low93}, decoder, nullptr, &registers);
    assert(registerStates.size() == 2);
    CheckState(registerStates[0], high93, 1, UINT64_MAX);
    CheckState(registerStates[1], low93, 0, UINT64_MAX);
    assert(registers.Get(key)->vectorMask->vectorMask0 == UINT64_MAX);
    assert(registers.Get(key)->vectorMask->vectorMask1 == UINT64_MAX);
    high93.record.args[1] = 0xffff;
    low93.record.args[1] = 0;
    DispatchTraceRecords({high93, low93}, decoder, nullptr, &registers);
    assert(registers.Get(key)->vectorMask->vectorMask0 == 0);
    assert(registers.Get(key)->vectorMask->vectorMask1 == 0xffff);
    auto invalid = *decoder.decode(low.record);
    std::get<VectorMaskWriteParamField>(invalid.params).index = 2;
    assert(!MakeRegisterStateData(low, invalid));
    registers.Reset();
    assert(!registers.Get(key));
    assert(errorCount == 0 && instructionCallbacks == 0);
}

void TestScalarUbCollection()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    registerStates.clear();
    // IDs 24..55 are the complete scalar LD/ST/STI/pair families; 56..61 are atomic stores.
    for (uint32_t id = 24; id <= 61; ++id) {
        const bool pair = (id >= 32 && id <= 35) || (id >= 52 && id <= 55);
        for (int64_t offset : {-8, 0, 8}) {
            for (uint64_t post : {0U, 1U}) {
                if (pair && post != 0) {
                    continue;
                }
                auto record = Record(static_cast<InstructionId>(id), 0x400 + id * 4);
                record.record.args[0] = 0x100080040; // UB window relative to SYS_VA_BASE.
                record.record.args[1] = static_cast<uint64_t>(offset);
                record.record.args[2] = post;
                record.record.args[3] = 0x100000000;
                record.record.args[4] = ASCSAN_SCALAR_ADDRESS_CONTEXT_V1;
                const auto decoded = decoder.decode(record.record);
                assert(decoded);
                if (id < 56) {
                    const auto& params = std::get<ScalarGmParamField>(decoded->params);
                    assert(params.addr == record.record.args[0] && params.offset == offset && params.post == post);
                    assert(params.sysVaBase == record.record.args[3]);
                    assert(params.addressContext == ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
                } else {
                    const auto& params = std::get<ScalarAtomicParamField>(decoded->params);
                    assert(params.addr == record.record.args[0] && params.offset == offset && params.post == post);
                    assert(params.sysVaBase == record.record.args[3]);
                    assert(params.addressContext == ASCSAN_SCALAR_ADDRESS_CONTEXT_V1);
                }
                logs.clear();
                memoryAccesses.clear();
                const unsigned callbacksBefore = instructionCallbacks;
                DispatchTraceRecords({record}, decoder);
                assert(logs.find("[raw]") != std::string::npos);
                assert(logs.find("addr=0x100080040 offset=" + std::to_string(offset)) != std::string::npos);
                assert(logs.find("post=" + std::to_string(post) + " sysVaBase=0x100000000") != std::string::npos);
                assert(logs.find("[cbdata]") != std::string::npos);
                assert(memoryAccesses.size() == 1 && instructionCallbacks == callbacksBefore + 1);
                const auto& access = memoryAccesses.front();
                const uint32_t bits = id < 56 ? 64U >> ((id - 24) % 4) : 32U >> ((id - 56) % 3);
                const bool registerOffset = (id >= 28 && id <= 31) || (id >= 40 && id <= 43) ||
                                            (id >= 48 && id <= 51) || (id >= 56 && id <= 58);
                const int64_t byteOffset = post == 1 ? 0 : offset * (registerOffset ? bits / 8 : 1);
                const bool store = id <= 43 || id >= 56;
                assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
                assert(access.address == static_cast<uint64_t>(0x40 + byteOffset));
                assert(access.dataBits == bits && access.instructionId == id);
                assert(
                    access.accessMode ==
                    (store ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ));
                assert(access.layoutKind == ACLSAN_MEM_LAYOUT_RANGE);
                assert(access.layout.range.bytes == (bits / 8) * (pair ? 2 : 1));
                assert(access.accessIndex == 0 && access.accessCount == 1);
                assert(access.header.launchId == record.launchId && access.header.pc == record.record.pc);
                assert(access.header.instrExecId == record.instrExecId && access.header.siteId == record.record.siteId);
                assert(
                    access.header.deviceId == record.deviceId && access.header.executor.phyCoreId == record.phyCoreId);
                assert(
                    access.header.executor.blockId == record.blockId &&
                    access.header.executor.blockType == record.blockType);
                assert(access.header.pipeline == ACLSAN_DEVICE_PIPE_SCALAR);
                assert(access.header.sourceKind == (store ? ACLSAN_DEVICE_SOURCE_ST : ACLSAN_DEVICE_SOURCE_LD));
                const uint32_t flags =
                    ACLSAN_DEVICE_EVENT_FLAG_EXACT | (id >= 56 ? ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN : 0);
                assert(access.header.flags == flags);
                assert(errorCount == 0 && registerStates.empty());
            }
        }
    }
}
void TestVectorInstructionFamilyFields()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    auto parsed = Record(InstructionId::VtransposeB16, 0x300);
    parsed.record.args[0] = 0x30000;
    parsed.record.args[1] = 0x20000;
    auto decoded = decoder.decode(parsed.record);
    assert(decoded.has_value() && decoded->kind == DeviceInstructionKind::Vtranspose);
    const auto& transpose = std::get<VtransposeParamField>(decoded->params);
    assert(transpose.dstAddr == 0x30000 && transpose.srcAddr == 0x20000);

    for (const auto id : {InstructionId::Vbs32F16, InstructionId::Vbs32F32}) {
        parsed.record.instrId = static_cast<uint32_t>(id);
        parsed.record.args[2] = 0x18000;
        parsed.record.args[3] = 3ULL << 56;
        decoded = decoder.decode(parsed.record);
        assert(decoded.has_value() && decoded->kind == DeviceInstructionKind::Vbs32);
        const auto& bitsort = std::get<Vbs32ParamField>(decoded->params);
        assert(bitsort.instrId == static_cast<uint32_t>(id));
        assert(bitsort.dstAddr == 0x30000 && bitsort.scoreAddr == 0x20000 && bitsort.indexAddr == 0x18000);
        assert(bitsort.dataBits == (id == InstructionId::Vbs32F16 ? 16U : 32U));
        assert(bitsort.config == 3ULL << 56);
    }
    for (const auto id : {InstructionId::VmrgsortF16, InstructionId::VmrgsortF32}) {
        parsed.record.instrId = static_cast<uint32_t>(id);
        parsed.record.args[1] = 0xffff000200010000;
        parsed.record.args[2] = 0x0004000300020001;
        parsed.record.args[3] = 0xf01;
        decoded = decoder.decode(parsed.record);
        assert(decoded.has_value() && decoded->kind == DeviceInstructionKind::Vmrgsort);
        const auto& merge = std::get<VmrgsortParamField>(decoded->params);
        assert(merge.instrId == static_cast<uint32_t>(id) && merge.dstAddr == 0x30000);
        assert(merge.srcListAddresses == parsed.record.args[1] && merge.srcListLengths == parsed.record.args[2]);
        assert(merge.dataBits == (id == InstructionId::VmrgsortF16 ? 16U : 32U) && merge.config == 0xf01);
    }
}

void TestVnchwconvParamFields()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    for (const auto id :
         {InstructionId::ScatterVnchwconvB8, InstructionId::ScatterVnchwconvB16, InstructionId::ScatterVnchwconvB32}) {
        auto parsed = Record(id, 0x300);
        parsed.record.args[0] = 6;
        parsed.record.args[1] = 2;
        parsed.record.args[2] = 3ULL << 56;
        parsed.record.args[3] = id == InstructionId::ScatterVnchwconvB8 ? 1 : 0;
        const auto decoded = decoder.decode(parsed.record);
        assert(decoded.has_value() && decoded->kind == DeviceInstructionKind::Vnchwconv);
        const auto& scatter = std::get<VnchwconvParamField>(decoded->params);
        assert(scatter.instrId == static_cast<uint32_t>(id));
        assert(scatter.dstVaIndex == 6 && scatter.srcVaIndex == 2 && scatter.config == 3ULL << 56);
        const uint32_t bits = id == InstructionId::ScatterVnchwconvB8  ? 8U :
                              id == InstructionId::ScatterVnchwconvB16 ? 16U :
                                                                         32U;
        assert(scatter.dataBits == bits);
        assert(scatter.dstHighHalf == (bits == 8) && !scatter.srcHighHalf);
        for (const unsigned operand : {0U, 1U}) {
            const uint64_t validIndex = parsed.record.args[operand];
            for (const uint64_t invalidIndex : {uint64_t{1}, uint64_t{8}, uint64_t{1} << 32, UINT64_MAX}) {
                parsed.record.args[operand] = invalidIndex;
                assert(!decoder.decode(parsed.record).has_value());
            }
            parsed.record.args[operand] = validIndex;
        }
    }
}

void TestVectorInstructionFamilyLogs()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    const auto check = [&decoder](InstructionId id, const std::array<uint64_t, 5>& args, const char* expected) {
        auto parsed = Record(id, 0x300);
        std::copy(args.begin(), args.end(), parsed.record.args);
        const auto decoded = decoder.decode(parsed.record);
        assert(decoded.has_value());
        logs.clear();
        LogParamField(decoded->params);
        assert(logs.find(expected) != std::string::npos);
    };
    check(
        InstructionId::VtransposeB16, {0x600, 0x200, 0, 0, 0},
        "type=VtransposeParamField instrId=178 dstAddr=0x600 srcAddr=0x200");
    check(
        InstructionId::Vbs32F32, {0x1000, 0x200, 0x600, 3ULL << 56, 0},
        "type=Vbs32ParamField instrId=419 dataBits=32 dstAddr=0x1000 scoreAddr=0x200 indexAddr=0x600");
    check(
        InstructionId::VmrgsortF32, {0x1000, 0x1000080, 0x200010, 0x301, 0},
        "type=VmrgsortParamField instrId=421 dataBits=32 dstAddr=0x1000 srcListAddresses=0x1000080 "
        "srcListLengths=0x200010");
    check(
        InstructionId::ScatterVnchwconvB8, {0, 2, 1ULL << 56, 1, 0},
        "type=VnchwconvParamField instrId=177 dataBits=8 dstVaIndex=0 srcVaIndex=2");
}

void TestTransposeMemoryCollection()
{
    auto transpose = Record(static_cast<InstructionId>(178), 0x300);
    transpose.record.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    transpose.record.args[0] = 0x600;
    transpose.record.args[1] = 0x200;
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    assert(decoder.decode(transpose.record));
    memoryAccesses.clear();
    logs.clear();
    DispatchTraceRecords({transpose}, decoder);
    assert(memoryAccesses.size() == 2);
    assert(memoryAccesses[0].address == 0x200 && memoryAccesses[1].address == 0x600);
    for (unsigned i = 0; i < 2; ++i) {
        const auto& access = memoryAccesses[i];
        assert(access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_UB);
        assert(access.accessMode == i + 1 && access.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
        assert(access.accessIndex == i && access.accessCount == 2);
        assert(access.header.pc == 0x300 && access.header.executor.phyCoreId == 7);
        assert(access.memoryOwner.phyCoreId == 7 && access.memoryOwner.blockType == transpose.blockType);
        assert(access.layoutKind == ACLSAN_MEM_LAYOUT_RANGE && access.layout.range.bytes == 512);
    }
}
MemoryCbdataResult ConvertVectorRaw(uint32_t id, std::array<uint64_t, 5> args, MemoryRegisterState state = {})
{
    auto parsed = Record(static_cast<InstructionId>(id), 0x400);
    std::copy(args.begin(), args.end(), parsed.record.args);
    const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(parsed.record);
    assert(decoded);
    MemoryCbdataContext context{};
    context.instructionId = id;
    context.executor = {parsed.phyCoreId, parsed.blockId, parsed.blockType};
    context.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    return std::visit(
        [&context, &state](const auto& f) -> MemoryCbdataResult {
            if constexpr (std::is_constructible_v<MemoryInstructionField, std::decay_t<decltype(f)>>) {
                return MemoryFieldToCbdataConverter{context, state}.Convert(MemoryInstructionField{f});
            }
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        },
        decoded->params);
}

void TestBitsortMemory()
{
    for (uint32_t id : {418U, 419U}) {
        const auto result = ConvertVectorRaw(id, {0x1000, 0x200, 0x600, 3ULL << 56, 0});
        assert(result.status == MemoryCbdataStatus::SUCCESS && !result.requiresIncompleteEvent);
        assert(result.data.size() == 3);
        assert(result.data[0].address == 0x200 && result.data[1].address == 0x600);
        assert(result.data[0].layout.range.bytes == 3 * 32 * (id == 418 ? 2 : 4));
        assert(result.data[1].layout.range.bytes == 3 * 128);
        assert(result.data[2].address == 0x1000 && result.data[2].layout.range.bytes == 3 * 256);
        for (const auto& access : result.data) {
            assert(access.header.flags == ACLSAN_DEVICE_EVENT_FLAG_EXACT);
        }
        assert(result.data[0].accessMode == 1 && result.data[1].accessMode == 1);
        const auto zeroRepeat = ConvertVectorRaw(id, {0x1000, 0x200, 0x600, 0, 0});
        assert(zeroRepeat.status == MemoryCbdataStatus::NO_ACCESS);
        assert(zeroRepeat.data.empty() && !zeroRepeat.requiresIncompleteEvent);
        auto parsed = Record(static_cast<InstructionId>(id), 0x400);
        parsed.record.args[0] = 0x1000;
        parsed.record.args[1] = 0x200;
        parsed.record.args[2] = 0x600;
        memoryAccesses.clear();
        DispatchTraceRecords({parsed}, dav3510::GetDeviceInstructionDecoder());
        assert(memoryAccesses.empty());
    }
    assert(
        ConvertVectorRaw(418, {UINT64_MAX - 64, 0, 0, 1ULL << 56, 0}).status ==
        MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
}

void TestMergeSortMemory()
{
    constexpr uint64_t addresses = 0x80ULL | (0xa0ULL << 16) | (0xc0ULL << 32) | (0xe0ULL << 48);
    constexpr uint64_t lengths = 32ULL | (32ULL << 16) | (32ULL << 32) | (32ULL << 48);
    for (uint32_t id : {420U, 421U}) {
        auto result = ConvertVectorRaw(id, {0x1000, addresses, lengths, 1 | (3 << 8), 0});
        assert(result.status == (id == 420 ? MemoryCbdataStatus::PARTIAL_COVERAGE : MemoryCbdataStatus::SUCCESS));
        assert(result.data.size() == 3);
        assert(result.data[0].address == 0x400 && result.data[1].address == 0x500);
        assert(result.data[0].layout.range.bytes == 256 && result.data[2].layout.range.bytes == 512);
        result = ConvertVectorRaw(id, {0x1000, addresses, lengths, 3 | (15 << 8), 0});
        assert(result.status == (id == 420 ? MemoryCbdataStatus::PARTIAL_COVERAGE : MemoryCbdataStatus::SUCCESS));
        assert(result.data.size() == 2);
        assert(result.data[0].layout.range.bytes == 3072 && result.data[1].layout.range.bytes == 3072);
        result = ConvertVectorRaw(id, {0x1000, addresses, lengths, 1 | (15 << 8) | (1 << 12), 0});
        assert(result.status == MemoryCbdataStatus::PARTIAL_COVERAGE && result.requiresIncompleteEvent);
        for (const auto& access : result.data) {
            assert(access.header.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED);
        }
        assert(ConvertVectorRaw(id, {0x1000, addresses, lengths, 0, 0}).status == MemoryCbdataStatus::PARTIAL_COVERAGE);
        assert(
            ConvertVectorRaw(id, {0x1000, addresses, lengths, 3 | (5 << 8), 0}).status ==
            MemoryCbdataStatus::INVALID_FIELD);
        assert(
            ConvertVectorRaw(id, {0x1000, addresses, 0, 1 | (15 << 8), 0}).status == MemoryCbdataStatus::INVALID_FIELD);
    }
}

void TestVaStateAndScatter()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    dav3510::Dav3510RegisterStateManager registers(42);
    const dav3510::Dav3510CoreKey key{1, 2, 7, 1};
    std::vector<ParsedTraceRecord> records;
    // Two source VA registers and two destination VA registers.
    for (uint32_t index = 0; index < 4; ++index) {
        for (uint32_t entry = 0; entry < 8; entry += 2) {
            auto move = Record(InstructionId::MoveVa, 0x500);
            move.record.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
            move.record.args[0] = index;
            move.record.args[1] = entry;
            move.record.args[2] = 0x400 + index * 0x200 + entry * 32;
            move.record.args[3] = move.record.args[2] + 32;
            records.push_back(move);
        }
    }
    DispatchTraceRecords(records, decoder, nullptr, &registers);
    MemoryRegisterState state{};
    state.va = registers.Get(key)->va;
    for (uint32_t id : {175U, 176U, 177U}) {
        const auto result = ConvertVectorRaw(id, {0, 2, (3ULL << 56) | (4ULL << 16) | 6, id == 177, 0}, state);
        assert(result.status == MemoryCbdataStatus::SUCCESS && result.data.size() == 32);
        for (unsigned lane = 0; lane < 16; ++lane) {
            const auto& read = result.data[lane];
            const auto& write = result.data[16 + lane];
            assert(read.address == 0x800 + (lane / 8) * 0x200 + (lane % 8) * 32);
            assert(write.address == 0x400 + (lane / 8) * 0x200 + (lane % 8) * 32 + (id == 177 ? 16 : 0));
            assert(read.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT);
            assert(read.layout.blockRepeat.blockSize == (id == 177 ? 16 : 32));
            assert(read.layout.blockRepeat.repeatTimes == 3 && read.layout.blockRepeat.repeatStride == 128);
            assert(write.layout.blockRepeat.repeatStride == 192);
            assert(read.accessIndex == lane && write.accessIndex == 16 + lane && write.accessCount == 32);
        }
        assert(ConvertVectorRaw(id, {0, 2, 0, 0, 0}).status == MemoryCbdataStatus::NO_ACCESS);
        const auto missing = ConvertVectorRaw(id, {0, 2, 1ULL << 56, 0, 0});
        assert(missing.status == MemoryCbdataStatus::PARTIAL_COVERAGE && missing.requiresIncompleteEvent);
        const auto single = ConvertVectorRaw(id, {0, 2, (1ULL << 56) | (4ULL << 16) | 6, 0, id == 177}, state);
        assert(single.status == MemoryCbdataStatus::SUCCESS);
        assert(single.data[0].address == 0x800 + 128 + (id == 177 ? 16 : 0));
        assert(single.data[16].address == 0x400 + 192);
    }
    // Missing entries stay unknown instead of being treated as zero addresses.
    state.va[2].validEntries &= ~1U;
    const auto partial = ConvertVectorRaw(175, {0, 2, 1ULL << 56, 0, 0}, state);
    assert(partial.status == MemoryCbdataStatus::PARTIAL_COVERAGE);
    for (const auto& access : partial.data) {
        assert(access.header.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED);
    }
    for (unsigned dimension = 0; dimension < 4; ++dimension) {
        auto other = records.front();
        if (dimension == 0) {
            ++other.deviceId;
        }
        if (dimension == 1) {
            ++other.blockId;
        }
        if (dimension == 2) {
            ++other.phyCoreId;
        }
        if (dimension == 3) {
            ++other.blockType;
        }
        other.record.args[2] = 0;
        DispatchTraceRecords({other}, decoder, nullptr, &registers);
        assert(registers.Get(key)->va[0].entries[0] == (0x400 >> 5));
    }
    registers.Reset();
    assert(!registers.Get(key));
}

void TestLdvaInvalidatesVaStateAndKeepsRead()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    dav3510::Dav3510RegisterStateManager registers(42);
    auto ld = Record(InstructionId::Ldva, 0x700);
    ld.record.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
    ld.record.args[0] = 2;
    ld.record.args[1] = 0x200;
    ld.record.args[2] = 1;
    auto moveVa = Record(InstructionId::MoveVa, 0x6fc);
    moveVa.record.args[0] = 2;
    moveVa.record.args[2] = 0x400;
    moveVa.record.args[3] = 0x600;
    DispatchTraceRecords({moveVa}, decoder, nullptr, &registers);
    const dav3510::Dav3510CoreKey key{1, 2, 7, 1};
    assert(registers.Get(key)->va[2].validEntries == 3);
    memoryAccesses.clear();
    DispatchTraceRecords({ld}, decoder, nullptr, &registers);
    assert(memoryAccesses.size() == 1 && memoryAccesses[0].address == 0x200);
    assert(memoryAccesses[0].layout.range.bytes == 32 && memoryAccesses[0].accessMode == 1);
    assert(registers.Get(key)->va[2].validEntries == 0);

    auto scatter = Record(InstructionId::ScatterVnchwconvB16, 0x704);
    scatter.record.args[1] = 2;
    scatter.record.args[2] = 1ULL << 56;
    memoryAccesses.clear();
    DispatchTraceRecords({scatter}, decoder, nullptr, &registers);
    assert(memoryAccesses.size() == 1 && memoryAccesses[0].header.flags == ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED);
    auto invalid = ld.record;
    invalid.args[0] = 8;
    assert(!decoder.decode(invalid));
    invalid = ld.record;
    invalid.args[2] = 2;
    assert(!decoder.decode(invalid));
    auto move = Record(InstructionId::MoveVa, 0x708).record;
    move.args[1] = 7;
    assert(!decoder.decode(move));
    invalid = scatter.record;
    invalid.args[0] = 7;
    assert(!decoder.decode(invalid));
}

void TestVectorSyncCbdata()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    for (uint32_t id = 456; id <= 470; ++id) {
        auto sync = Record(static_cast<InstructionId>(id), 0x800);
        sync.record.pipeline = ACLSAN_DEVICE_PIPE_VECTOR;
        sync.record.args[0] = ACLSAN_DEVICE_PIPE_VECTOR;
        sync.record.args[1] = 3;
        sync.record.args[2] = 2;
        if (id == 468) {
            sync.record.args[1] = (2 << 4) | (3 << 8);
        }
        if (id >= 469) {
            sync.record.args[0] = 3;
        }
        assert(IsDefinedInstructionId(id));
        const auto decoded = decoder.decode(sync.record);
        assert(decoded);
        const auto cb = TranslateDecodedTraceToCallbackData(sync, *decoded);
        assert(cb && std::holds_alternative<AclsanDeviceSyncData>(*cb));
        const auto& data = std::get<AclsanDeviceSyncData>(*cb);
        assert(data.header.pc == 0x800 && data.header.instrExecId == sync.instrExecId);
        assert(data.header.pipeline == ACLSAN_DEVICE_PIPE_VECTOR);
        if (id <= 459) {
            assert(data.objectId == 2);
        } else {
            assert(data.objectId == 3);
        }
    }
}
} // namespace

// Capture the real dispatcher's output at the callback boundary.
namespace aclsan {
void AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(const AclsanDeviceMemoryAccessData& access) noexcept
{
    memoryAccesses.push_back(access);
    ++instructionCallbacks;
}
void AclsanCallbackDispatcher::DispatchDeviceState(const AclsanDeviceRegisterStateData& state) noexcept
{
    registerStates.push_back(state);
}
void AclsanCallbackDispatcher::DispatchDeviceVf(const AclsanDeviceVfData& data) noexcept
{
    vfCompletions.push_back(data);
}
void AclsanCallbackDispatcher::DispatchDeviceSync(const AclsanDeviceSyncData&) noexcept { ++instructionCallbacks; }
void AclsanCallbackDispatcher::DispatchDeviceBarrier(const AclsanDeviceBarrierData&) noexcept
{
    ++instructionCallbacks;
}
} // namespace aclsan

void TestFixStateCollection()
{
    registerStates.clear();
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    dav3510::Dav3510RegisterStateManager registers(42);
    auto channel = Record(InstructionId::ChannelPara, 0x300);
    channel.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    channel.phyCoreId = 0;
    channel.record.args[0] = 3ULL << 48;
    auto fpc = channel;
    fpc.record.instrId = static_cast<uint32_t>(InstructionId::SetFpc);
    fpc.instrExecId = 4;
    fpc.record.args[0] = 1ULL << 63;
    DispatchTraceRecords({channel}, decoder, nullptr, &registers);
    DispatchTraceRecords({fpc}, decoder, nullptr, &registers);
    assert(registerStates.size() == 2);
    assert(registerStates[0].regId == ACLSAN_DEVICE_REGISTER_CHANNEL_PARA);
    assert(registerStates[0].value == channel.record.args[0]);
    assert(registerStates[1].regId == ACLSAN_DEVICE_REGISTER_FPC);
    assert(registerStates[1].value == fpc.record.args[0]);
    const dav3510::Dav3510CoreKey key{channel.blockType, channel.blockId, channel.phyCoreId, channel.deviceId};
    assert(registers.Get(key) && registers.Get(key)->channelPara);
    assert(registers.Get(key)->channelPara->value == channel.record.args[0]);
}

int main()
{
    plog_test::SetApi({EnableLog, CaptureLog});
    TestPushPbAndVfCollection();
    TestVectorMaskCollection();
    TestFixStateCollection();
    TestScalarUbCollection();
    TestVectorInstructionFamilyFields();
    TestVnchwconvParamFields();
    TestVectorInstructionFamilyLogs();
    TestTransposeMemoryCollection();
    TestBitsortMemory();
    TestMergeSortMemory();
    TestVaStateAndScatter();
    TestLdvaInvalidatesVaStateAndKeepsRead();
    TestVectorSyncCbdata();
    plog_test::ResetApi();
    std::cout << "Vector and scalar UB instruction cbdata collection checks passed\n";
}
