/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_device_data.h"
#include "aclsan_device_data_log.h"
#include "npu_tool_log.h"
#include "aclsan_memory_cbdata.h"
#include "aclsan_register_dependency.h"
#include "device_instr/common/instruction_id.h"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace aclsan {

namespace {

const char* MemoryCbdataStatusName(MemoryCbdataStatus status) noexcept
{
    switch (status) {
        case MemoryCbdataStatus::SUCCESS:
            return "SUCCESS";
        case MemoryCbdataStatus::NO_ACCESS:
            return "NO_ACCESS";
        case MemoryCbdataStatus::INVALID_FIELD:
            return "INVALID_FIELD";
        case MemoryCbdataStatus::MISSING_REGISTER_STATE:
            return "MISSING_REGISTER_STATE";
        case MemoryCbdataStatus::ARITHMETIC_OVERFLOW:
            return "ARITHMETIC_OVERFLOW";
        case MemoryCbdataStatus::RESOURCE_EXHAUSTED:
            return "RESOURCE_EXHAUSTED";
        case MemoryCbdataStatus::MISSING_ADDRESS_CONTEXT:
            return "MISSING_ADDRESS_CONTEXT";
        case MemoryCbdataStatus::UNSUPPORTED_ADDRESS_SPACE:
            return "UNSUPPORTED_ADDRESS_SPACE";
        case MemoryCbdataStatus::INVALID_ADDRESS_SPACE:
            return "INVALID_ADDRESS_SPACE";
        case MemoryCbdataStatus::PARTIAL_COVERAGE:
            return "PARTIAL_COVERAGE";
    }
    return "UNKNOWN";
}

template <typename ParamField>
constexpr bool IsMemoryAccessParamField() noexcept
{
    return std::is_same_v<ParamField, aclsan::CopyGmToUbufAlignV2ParamField> ||
           std::is_same_v<ParamField, aclsan::CopyGmToCbufAlignV2ParamField> ||
           std::is_same_v<ParamField, aclsan::CopyGmToCbufMultiNd2NzParamField> ||
           std::is_same_v<ParamField, aclsan::CopyGmToCbufMultiDn2NzParamField> ||
           std::is_same_v<ParamField, aclsan::CopyUbufToGmAlignV2ParamField> ||
           std::is_same_v<ParamField, aclsan::CopyGmToCbufV2ParamField> ||
           std::is_same_v<ParamField, aclsan::NdDmaParamField> ||
           std::is_same_v<ParamField, aclsan::LoadGmToCbuf2DV2ParamField> ||
           std::is_same_v<ParamField, aclsan::FixL0cToOutParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarGmParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarDevParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarPreloadParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarAtomicParamField> ||
           std::is_same_v<ParamField, aclsan::SetL12DParamField> ||
           std::is_same_v<ParamField, aclsan::MmadParamField> ||
           std::is_same_v<ParamField, aclsan::LoadL1MxParamField> ||
           std::is_same_v<ParamField, aclsan::LocalMemoryTransferParamField> ||
           std::is_same_v<ParamField, aclsan::LoadCbufToL0ParamField> ||
           std::is_same_v<ParamField, aclsan::LoadCbufToCbTransposeParamField> ||
           std::is_same_v<ParamField, aclsan::Img2ColParamField>;
}

template <typename ParamField>
constexpr bool IsSyncParamField() noexcept
{
    return std::is_same_v<ParamField, aclsan::FlagParamField> ||
           std::is_same_v<ParamField, aclsan::HardwareFlagParamField> ||
           std::is_same_v<ParamField, aclsan::FftsSyncParamField> ||
           std::is_same_v<ParamField, aclsan::DeviceFlagParamField> ||
           std::is_same_v<ParamField, aclsan::SyncBufParamField> ||
           std::is_same_v<ParamField, aclsan::CrossCoreParamField>;
}

class Translator final {
public:
    static std::optional<DeviceCallbackData> TranslateToCallbackData(
        const ParsedTraceRecord& parsed, const aclsan::DecodedInstruction& decoded,
        const MemoryRegisterState& registerState, const std::vector<InternalMemoryRange>* internalInputs,
        const std::vector<InternalMemoryRange>* readOnlyBinaryRanges) noexcept
    {
        const auto pipeline = static_cast<AclsanDevicePipeline>(parsed.record.pipeline);
        return std::visit(
            [&](const auto& value) noexcept -> std::optional<DeviceCallbackData> {
                using ParamField = std::decay_t<decltype(value)>;
                if constexpr (IsMemoryAccessParamField<ParamField>()) {
                    return MakeDeviceMemoryAccessCallbackData(
                        parsed, pipeline, value, registerState, internalInputs, readOnlyBinaryRanges);
                } else if constexpr (std::is_same_v<ParamField, aclsan::LocalMemoryTransferParamField>) {
                    ASCTOOL_DEBUG(
                        "[cbdata] no GM access for local-only memory instruction instrId=%u kind=%u", value.instrId,
                        static_cast<unsigned int>(value.kind));
                    return DeviceCallbackData{DeviceMemoryAccessDataList{}};
                } else if constexpr (std::is_same_v<ParamField, aclsan::PipeBarrierParamField>) {
                    AclsanDeviceBarrierData barrier{};
                    barrier.header = MakeDeviceEventHeader(parsed, pipeline, 0, sizeof(barrier));
                    barrier.pipe = value.pipe;
                    return DeviceCallbackData{barrier};
                } else if constexpr (IsSyncParamField<ParamField>()) {
                    return MakeDeviceSyncData(parsed, decoded.kind, pipeline, value);
                } else {
                    return std::nullopt;
                }
            },
            decoded.params);
    }

private:
    static AclsanDeviceEventHeader MakeDeviceEventHeader(
        const ParsedTraceRecord& parsed, AclsanDevicePipeline pipeline, uint64_t serialNo, uint32_t dataSize) noexcept
    {
        return {
            ACLSAN_API_VERSION,
            dataSize,
            parsed.launchId,
            parsed.record.pc,
            parsed.record.siteId,
            0,
            parsed.instrExecId,
            serialNo,
            parsed.deviceId,
            parsed.phyCoreId,
            parsed.blockId,
            parsed.blockType,
            static_cast<uint32_t>(pipeline),
            0};
    }

    template <typename ParamField>
    static std::optional<DeviceCallbackData> MakeDeviceMemoryAccessCallbackData(
        const ParsedTraceRecord& parsed, AclsanDevicePipeline pipeline, const ParamField& params,
        const MemoryRegisterState& registerState, const std::vector<InternalMemoryRange>* internalInputs,
        const std::vector<InternalMemoryRange>* readOnlyBinaryRanges) noexcept
    {
        const MemoryCbdataContext context{
            parsed.record.pc,
            parsed.instrExecId,
            0,
            parsed.record.siteId,
            parsed.phyCoreId,
            parsed.blockId,
            static_cast<uint32_t>(pipeline),
            parsed.launchId,
            parsed.deviceId,
            parsed.blockType,
            parsed.record.parameterBase,
            parsed.parameterBytes,
            internalInputs,
            readOnlyBinaryRanges,
            parsed.record.instrId};
        MemoryCbdataResult result =
            MemoryFieldToCbdataConverter{context, registerState}.Convert(MemoryInstructionField{params});
        const RegisterDependencyMask dependencies =
            ResolveRegisterDependencies(MemoryInstructionField{params}, registerState);
        // Partial conversion can still contain proven GM or on-chip accesses.
        for (AclsanDeviceMemoryAccessData& access : result.data) {
            if constexpr (
                std::is_same_v<ParamField, aclsan::MmadParamField> ||
                std::is_same_v<ParamField, aclsan::FixL0cToOutParamField>) {
                access.unitFlag = params.unitFlag;
            } else if constexpr (std::is_same_v<ParamField, aclsan::LocalMemoryTransferParamField>) {
                switch (params.kind) {
                    case aclsan::LocalMemoryTransferKind::FixL0cToCbufF32:
                    case aclsan::LocalMemoryTransferKind::FixL0cToCbufS32:
                    case aclsan::LocalMemoryTransferKind::FixL0cToUbufF32:
                    case aclsan::LocalMemoryTransferKind::FixL0cToUbufS32:
                        access.unitFlag = static_cast<uint32_t>((params.config1 >> 32) & 3);
                        break;
                    default:
                        break;
                }
            }
            access.regDependencyMask0 = dependencies.mask0;
            access.regDependencyMask1 = dependencies.mask1;
        }
        if (result.status == MemoryCbdataStatus::SUCCESS) {
            return DeviceCallbackData{std::move(result.data)};
        }
        if (result.status == MemoryCbdataStatus::NO_ACCESS) {
            return DeviceCallbackData{DeviceMemoryAccessDataList{}};
        }
        if (result.status == MemoryCbdataStatus::UNSUPPORTED_ADDRESS_SPACE) {
            ASCTOOL_DEBUG(
                "acl_san trace: skipped memory access in known UB/PRIVATE address space instrId=%llu pc=0x%llx "
                "blockType=%u blockId=%u",
                static_cast<unsigned long long>(parsed.record.instrId),
                static_cast<unsigned long long>(parsed.record.pc), parsed.blockType, parsed.blockId);
            return DeviceCallbackData{DeviceMemoryAccessDataList{}};
        }
        const bool missingRegister = result.status == MemoryCbdataStatus::MISSING_REGISTER_STATE &&
                                     (dependencies.mask0 != 0 || dependencies.mask1 != 0);
        if (missingRegister || result.requiresIncompleteEvent) {
            if (!result.requiresIncompleteEvent && !result.data.empty()) {
                return DeviceCallbackData{std::move(result.data)};
            }
            // 保留已确认的源读，并标记采集不完整；
            // 对无法解析的目标端不臆造原子操作。
            AclsanDeviceMemoryAccessData estimated{};
            estimated.header =
                MakeDeviceEventHeader(parsed, pipeline, 0, static_cast<uint32_t>(sizeof(AclsanDeviceMemoryAccessData)));
            estimated.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
            estimated.instructionId = parsed.record.instrId;
            estimated.regDependencyMask0 = dependencies.mask0;
            estimated.regDependencyMask1 = dependencies.mask1;
            result.data.push_back(estimated);
            for (size_t i = 0; i < result.data.size(); ++i) {
                result.data[i].accessIndex = static_cast<uint32_t>(i);
                result.data[i].accessCount = static_cast<uint32_t>(result.data.size());
            }
            return DeviceCallbackData{std::move(result.data)};
        }
        ASCTOOL_ERROR(
            "acl_san trace: incomplete memory access status=%s instrId=%llu pc=0x%llx blockType=%u "
            "blockId=%u requiredSetInstrId=%llu",
            MemoryCbdataStatusName(result.status), static_cast<unsigned long long>(parsed.record.instrId),
            static_cast<unsigned long long>(parsed.record.pc), parsed.blockType, parsed.blockId,
            static_cast<unsigned long long>(result.requiredRegisterInstructionId));
        // Keep proven accesses, but do not synthesize an event for conversion failure.
        if (!result.data.empty()) {
            return DeviceCallbackData{std::move(result.data)};
        }
        return std::nullopt;
    }

    static AclsanDeviceSyncData MakeDeviceSyncDataBase(
        const ParsedTraceRecord& parsed, AclsanDevicePipeline pipeline,
        AclsanDeviceSourceKind sourceKind = ACLSAN_DEVICE_SOURCE_SET_WAIT_FLAG,
        AclsanDeviceSyncKind syncKind = ACLSAN_DEVICE_SYNC_KIND_SET_WAIT_FLAG) noexcept
    {
        AclsanDeviceSyncData callbackData{};
        callbackData.header =
            MakeDeviceEventHeader(parsed, pipeline, 0, static_cast<uint32_t>(sizeof(AclsanDeviceSyncData)));
        callbackData.header.sourceKind = sourceKind;
        callbackData.syncKind = syncKind;
        return callbackData;
    }

    static DeviceCallbackData MakeDeviceSyncData(
        const ParsedTraceRecord& parsed, aclsan::DeviceInstructionKind kind, AclsanDevicePipeline pipeline,
        const aclsan::FlagParamField& params) noexcept
    {
        const bool isSet = kind == aclsan::DeviceInstructionKind::SetFlag;
        auto callbackData = MakeDeviceSyncDataBase(parsed, pipeline);
        callbackData.action = isSet ? ACLSAN_DEVICE_SYNC_ACTION_SET : ACLSAN_DEVICE_SYNC_ACTION_WAIT;
        callbackData.scope = ACLSAN_DEVICE_SYNC_SCOPE_BLOCK;
        callbackData.srcPipe = params.srcPipe;
        callbackData.dstPipe = params.dstPipe;
        callbackData.objectId = params.eventId;
        return callbackData;
    }

    static DeviceCallbackData MakeDeviceSyncData(
        const ParsedTraceRecord& parsed, [[maybe_unused]] aclsan::DeviceInstructionKind kind,
        AclsanDevicePipeline pipeline, const aclsan::FftsSyncParamField& params) noexcept
    {
        auto callbackData = MakeDeviceSyncDataBase(parsed, pipeline);
        callbackData.action = ACLSAN_DEVICE_SYNC_ACTION_SET;
        callbackData.scope = ACLSAN_DEVICE_SYNC_SCOPE_CORE;
        callbackData.srcPipe = params.dstPipe;
        callbackData.dstPipe = params.dstPipe;
        callbackData.mode = params.mode;
        callbackData.objectId = params.flagId;
        return callbackData;
    }

    static DeviceCallbackData MakeDeviceSyncData(
        const ParsedTraceRecord& parsed, [[maybe_unused]] aclsan::DeviceInstructionKind kind,
        AclsanDevicePipeline pipeline, const aclsan::DeviceFlagParamField& params) noexcept
    {
        auto callbackData = MakeDeviceSyncDataBase(parsed, pipeline);
        callbackData.action = ACLSAN_DEVICE_SYNC_ACTION_WAIT;
        callbackData.scope = ACLSAN_DEVICE_SYNC_SCOPE_CORE;
        callbackData.srcPipe = params.pipe;
        callbackData.dstPipe = params.pipe;
        callbackData.objectId = params.flagId;
        return callbackData;
    }

    static DeviceCallbackData MakeDeviceSyncData(
        const ParsedTraceRecord& parsed, aclsan::DeviceInstructionKind kind, AclsanDevicePipeline pipeline,
        const aclsan::HardwareFlagParamField& params) noexcept
    {
        const bool isSet = kind == aclsan::DeviceInstructionKind::HardwareSetFlag;
        auto callbackData = MakeDeviceSyncDataBase(parsed, pipeline);
        callbackData.action = isSet ? ACLSAN_DEVICE_SYNC_ACTION_SET : ACLSAN_DEVICE_SYNC_ACTION_WAIT;
        callbackData.scope = ACLSAN_DEVICE_SYNC_SCOPE_CORE;
        callbackData.srcPipe = params.srcPipe;
        callbackData.dstPipe = params.dstPipe;
        callbackData.mode = static_cast<uint32_t>(params.memory);
        callbackData.objectId = params.eventId;
        callbackData.reserved[0] = params.isVirtual ? 1U : 0U;
        return callbackData;
    }

    static DeviceCallbackData MakeDeviceSyncData(
        const ParsedTraceRecord& parsed, aclsan::DeviceInstructionKind kind, AclsanDevicePipeline pipeline,
        const aclsan::SyncBufParamField& params) noexcept
    {
        const bool isGet = kind == aclsan::DeviceInstructionKind::GetBuf;
        auto callbackData = MakeDeviceSyncDataBase(
            parsed, pipeline, ACLSAN_DEVICE_SOURCE_GET_RLS_BUF, ACLSAN_DEVICE_SYNC_KIND_GET_RLS_BUF);
        callbackData.action = isGet ? ACLSAN_DEVICE_SYNC_ACTION_GET : ACLSAN_DEVICE_SYNC_ACTION_RELEASE;
        callbackData.scope = ACLSAN_DEVICE_SYNC_SCOPE_PIPE;
        callbackData.dstPipe = params.pipe;
        callbackData.mode = params.mode;
        callbackData.objectId = params.bufId;
        return callbackData;
    }

    static DeviceCallbackData MakeDeviceSyncData(
        const ParsedTraceRecord& parsed, aclsan::DeviceInstructionKind kind, AclsanDevicePipeline pipeline,
        const aclsan::CrossCoreParamField& params) noexcept
    {
        AclsanDeviceSyncData callbackData{};
        callbackData.header =
            MakeDeviceEventHeader(parsed, pipeline, 0, static_cast<uint32_t>(sizeof(AclsanDeviceSyncData)));
        callbackData.header.sourceKind = ACLSAN_DEVICE_SOURCE_CROSS_CORE_SYNC;
        const bool isSet =
            kind == aclsan::DeviceInstructionKind::CrossCoreSet || kind == aclsan::DeviceInstructionKind::IntraBlockSet;
        const bool isIntraBlock = kind == aclsan::DeviceInstructionKind::IntraBlockSet ||
                                  kind == aclsan::DeviceInstructionKind::IntraBlockWait;
        callbackData.syncKind = isIntraBlock ? ACLSAN_DEVICE_SYNC_KIND_INTRA_BLOCK : ACLSAN_DEVICE_SYNC_KIND_CROSS_CORE;
        callbackData.action = isSet ? ACLSAN_DEVICE_SYNC_ACTION_SET : ACLSAN_DEVICE_SYNC_ACTION_WAIT;
        callbackData.scope = ACLSAN_DEVICE_SYNC_SCOPE_DEVICE;
        callbackData.mode = params.mode;
        callbackData.objectId = params.flagId;
        if (callbackData.action == ACLSAN_DEVICE_SYNC_ACTION_SET) {
            callbackData.srcPipe = params.pipe;
        } else {
            callbackData.dstPipe = params.pipe;
        }
        return callbackData;
    }
};

} // namespace

std::optional<DeviceCallbackData> TranslateDecodedTraceToCallbackData(
    const ParsedTraceRecord& parsed, const aclsan::DecodedInstruction& decoded,
    const MemoryRegisterState& registerState, const std::vector<InternalMemoryRange>* internalInputs,
    const std::vector<InternalMemoryRange>* readOnlyBinaryRanges) noexcept
{
    LogRawRecord(parsed);
    // 判断是否找到对应的paramfield  raw data -> param field
    if (std::holds_alternative<std::monostate>(decoded.params)) {
        ASCTOOL_DEBUG("[param] rawData -> paramField translation failed instrId=%u", parsed.record.instrId);
        return std::nullopt;
    }
    LogParamField(decoded.params);

    std::optional<DeviceCallbackData> cbdata =
        Translator::TranslateToCallbackData(parsed, decoded, registerState, internalInputs, readOnlyBinaryRanges);
    // 判断 param field -> cbdata 的转换是否成功
    if (cbdata == std::nullopt) {
        ASCTOOL_DEBUG("[cbdata] paramField -> cbdata translation failed instrId=%u", parsed.record.instrId);
        return std::nullopt;
    }
    LogCallbackData(*cbdata);
    return cbdata;
}

} // namespace aclsan
