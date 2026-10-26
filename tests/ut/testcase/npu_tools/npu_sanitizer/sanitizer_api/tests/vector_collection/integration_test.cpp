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
#include "aclsan_register_dependency.h"
#include "aclsan_trace_runtime.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"
#include "device_instr/common/instruction_id.h"
#include "plog_test_library.h"
#include "npu_tool_log.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace aclsan;
namespace {
std::vector<AclsanDeviceRegisterStateData> registerStates;
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
    assert(header.deviceId == record.deviceId && header.phyCoreId == record.phyCoreId);
    assert(header.blockId == record.blockId && header.blockType == record.blockType);
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
    assert(errorCount == 0 && instructionCallbacks == 0 && registerStates.empty());

    // Collection accepts individual records and does not impose PB query batch rules.
    DispatchTraceRecords({vf}, decoder);
    DispatchTraceRecords({push, push, push, push, push}, decoder);
    vf.record.args[1] = 0;
    DispatchTraceRecords({vf}, decoder);
    assert(errorCount == 0 && instructionCallbacks == 0 && registerStates.empty());
}

void TestVectorMaskCollection()
{
    const auto& decoder = dav3510::GetDeviceInstructionDecoder();
    dav3510::Dav3510RegisterStateManager registers(42);
    auto high = Record(InstructionId::MoveMask1, 0x184);
    high.record.args[0] = 1;
    high.record.args[1] = 0xfedcba9876543210;
    auto low = Record(InstructionId::MoveMask0, 0x180);
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

    // The selected half comes from the operand, independently of the instruction ID.
    for (const auto id : {InstructionId::MoveMask0, InstructionId::MoveMask1}) {
        auto operand = Record(id, 0x180);
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
    }

    auto complete = Record(InstructionId::SetVectorMask, 0x188);
    complete.record.args[0] = 0x7777;
    complete.record.args[1] = 0x8888;
    registerStates.clear();
    DispatchTraceRecords({complete}, decoder, nullptr, &registers);
    assert(registerStates.size() == 2);
    CheckState(registerStates[0], complete, 0, 0x8888);
    CheckState(registerStates[1], complete, 1, 0x7777);
    assert(registers.Get(key)->vectorMask->vectorMask0 == 0x8888);
    assert(registers.Get(key)->vectorMask->vectorMask1 == 0x7777);
    assert(logs.find("[cbdata] type=AclsanDeviceRegisterStateData") != std::string::npos);
    assert(logs.find("regId=0 regIndex=1 value=0x7777") != std::string::npos);
    auto invalid = *decoder.decode(low.record);
    std::get<VectorMaskWriteParamField>(invalid.params).index = 2;
    const auto invalidState = MakeRegisterStateData(low, invalid);
    assert(invalidState.has_value());
    assert(invalidState->regIndex == 2);
    assert(invalidState->value == low.record.args[1]);
    registers.Reset();
    assert(!registers.Get(key));
    assert(errorCount == 0 && instructionCallbacks == 0);
}
} // namespace

// Capture the real dispatcher's output at the callback boundary.
namespace aclsan {
void AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(const AclsanDeviceMemoryAccessData&) noexcept
{
    ++instructionCallbacks;
}
void AclsanCallbackDispatcher::DispatchDeviceState(const AclsanDeviceRegisterStateData& state) noexcept
{
    registerStates.push_back(state);
}
void AclsanCallbackDispatcher::DispatchDeviceSync(const AclsanDeviceSyncData&) noexcept { ++instructionCallbacks; }
void AclsanCallbackDispatcher::DispatchDeviceBarrier(const AclsanDeviceBarrierData&) noexcept
{
    ++instructionCallbacks;
}
} // namespace aclsan

int main()
{
    plog_test::SetApi({EnableLog, CaptureLog});
    TestPushPbAndVfCollection();
    TestVectorMaskCollection();
    plog_test::ResetApi();
    std::cout << "PUSH_PB/VF/vector_mask collection checks passed\n";
}
