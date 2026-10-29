/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_register_dependency.h"

#include "device_instr/common/instruction_id.h"

#include <array>
#include <type_traits>
#include <variant>

namespace aclsan {
namespace {
constexpr uint64_t NO_TRANSFER_COUNT = 0U;
constexpr uint64_t STRIDE_DEPENDENCY_THRESHOLD = 1U;
constexpr uint32_t PAD_FUNCTION_MODE_MIN = 1U;
constexpr uint32_t PAD_FUNCTION_MODE_MAX = 5U;
constexpr uint32_t NO_DECOMPOSITION_MODE = 0U;

AclsanDeviceEventHeader MakeHeader(const ParsedTraceRecord& parsed) noexcept
{
    return {ACLSAN_API_VERSION,     static_cast<uint32_t>(sizeof(AclsanDeviceRegisterStateData)),
            parsed.launchId,        parsed.record.pc,
            parsed.record.siteId,   ACLSAN_DEVICE_SOURCE_SCALAR,
            parsed.instrExecId,     0,
            parsed.deviceId,        parsed.phyCoreId,
            parsed.blockId,         parsed.blockType,
            parsed.record.pipeline, ACLSAN_DEVICE_EVENT_FLAG_EXACT};
}

std::optional<AclsanDeviceRegisterId> RegisterId(const DecodedInstruction& decoded) noexcept
{
    return std::visit(
        [](const auto& value) noexcept -> std::optional<AclsanDeviceRegisterId> {
            using Field = std::decay_t<decltype(value)>;
            if constexpr (
                std::is_same_v<Field, VectorMaskParamField> || std::is_same_v<Field, VectorMaskWriteParamField>) {
                return ACLSAN_DEVICE_REGISTER_VECTOR_MASK;
            } else if constexpr (std::is_same_v<Field, CtrlRegisterParamField>) {
                return ACLSAN_DEVICE_REGISTER_CTRL;
            } else if constexpr (std::is_same_v<Field, Mte2SourceParamField>) {
                return ACLSAN_DEVICE_REGISTER_MTE2_SOURCE;
            } else if constexpr (std::is_same_v<Field, NdDmaPadCountParamField>) {
                return ACLSAN_DEVICE_REGISTER_NDDMA_PAD_COUNT;
            } else if constexpr (std::is_same_v<Field, NdDmaLoopStrideParamField>) {
                if (value.loopIndex >= NDDMA_LOOP_COUNT) {
                    return std::nullopt;
                }
                return static_cast<AclsanDeviceRegisterId>(
                    static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_NDDMA_LOOP0_STRIDE) + value.loopIndex);
            } else if constexpr (std::is_same_v<Field, Mte2NzParamField>) {
                return ACLSAN_DEVICE_REGISTER_MTE2_NZ;
            } else if constexpr (std::is_same_v<Field, Loop3ParamField>) {
                return ACLSAN_DEVICE_REGISTER_LOOP3;
            } else if constexpr (std::is_same_v<Field, DmaLoopSizeParamField>) {
                const auto direction = static_cast<uint32_t>(value.direction);
                if (direction >= DMA_DIRECTION_COUNT) {
                    return std::nullopt;
                }
                return static_cast<AclsanDeviceRegisterId>(
                    static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_UBUF_TO_GM) + direction);
            } else if constexpr (std::is_same_v<Field, DmaLoopStrideParamField>) {
                const auto direction = static_cast<uint32_t>(value.direction);
                if (direction >= DMA_DIRECTION_COUNT || value.loopIndex >= DMA_STRIDE_LOOP_COUNT) {
                    return std::nullopt;
                }
                return static_cast<AclsanDeviceRegisterId>(
                    static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_DMA_LOOP1_STRIDE_UBUF_TO_GM) +
                    direction * DMA_STRIDE_LOOP_COUNT + value.loopIndex);
            } else if constexpr (std::is_same_v<Field, SetPaddingParamField>) {
                return ACLSAN_DEVICE_REGISTER_SET_PADDING;
            }
            return std::nullopt;
        },
        decoded.params);
}

template <typename Field>
RegisterDependencyMask DmaDependencies(
    const Field& field, DmaLoopDirection direction, const MemoryRegisterState& state) noexcept
{
    if (field.burstNum == NO_TRANSFER_COUNT || field.burstLen == NO_TRANSFER_COUNT) {
        return {};
    }
    const auto directionIndex = static_cast<size_t>(direction);
    RegisterDependencyMask result{
        RegisterBit(static_cast<AclsanDeviceRegisterId>(
            static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_DMA_LOOP_SIZE_UBUF_TO_GM) + directionIndex)),
        0};
    if (!state.dmaLoopSizes[directionIndex].has_value()) {
        return result;
    }
    const auto& size = *state.dmaLoopSizes[directionIndex];
    if (size.loop1Size > STRIDE_DEPENDENCY_THRESHOLD) {
        result.mask0 |= RegisterBit(static_cast<AclsanDeviceRegisterId>(
            static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_DMA_LOOP1_STRIDE_UBUF_TO_GM) +
            directionIndex * DMA_STRIDE_LOOP_COUNT));
    }
    if (size.loop2Size > STRIDE_DEPENDENCY_THRESHOLD) {
        result.mask0 |= RegisterBit(static_cast<AclsanDeviceRegisterId>(
            static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_DMA_LOOP2_STRIDE_UBUF_TO_GM) +
            directionIndex * DMA_STRIDE_LOOP_COUNT));
    }
    return result;
}

} // namespace

RegisterDependencyMask ResolveRegisterDependencies(
    const MemoryInstructionField& field, const MemoryRegisterState& state) noexcept
{
    return std::visit(
        [&state](const auto& value) noexcept -> RegisterDependencyMask {
            using Field = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Field, LoadGmToCbuf2DV2ParamField>) {
                return value.decompMode == NO_DECOMPOSITION_MODE && value.mStep != NO_TRANSFER_COUNT &&
                               value.kStep != NO_TRANSFER_COUNT ?
                           RegisterDependencyMask{RegisterBit(ACLSAN_DEVICE_REGISTER_MTE2_SOURCE), 0} :
                           RegisterDependencyMask{};
            } else if constexpr (
                std::is_same_v<Field, CopyGmToCbufMultiNd2NzParamField> ||
                std::is_same_v<Field, CopyGmToCbufMultiDn2NzParamField>) {
                return value.nValue != NO_TRANSFER_COUNT && value.dValue != NO_TRANSFER_COUNT ?
                           RegisterDependencyMask{RegisterBit(ACLSAN_DEVICE_REGISTER_MTE2_NZ), 0} :
                           RegisterDependencyMask{};
            } else if constexpr (std::is_same_v<Field, NdDmaOutToUbufParamField>) {
                const std::array<uint64_t, NDDMA_LOOP_COUNT> counts{
                    value.loop0Size, value.loop1Size, value.loop2Size, value.loop3Size, value.loop4Size};
                for (uint64_t count : counts) {
                    if (count == NO_TRANSFER_COUNT) {
                        return {};
                    }
                }
                RegisterDependencyMask result{RegisterBit(ACLSAN_DEVICE_REGISTER_NDDMA_PAD_COUNT), 0};
                for (size_t index = 0; index < counts.size(); ++index) {
                    const uint64_t padding =
                        index == 0          ? uint64_t(value.loop0LeftPaddingCount) + value.loop0RightPaddingCount :
                        state.ndDmaPadCount ? uint64_t(state.ndDmaPadCount->leftPaddingCounts[index - 1]) +
                                                  state.ndDmaPadCount->rightPaddingCounts[index - 1] :
                                              0;
                    if (counts[index] + padding > STRIDE_DEPENDENCY_THRESHOLD) {
                        result.mask0 |= RegisterBit(static_cast<AclsanDeviceRegisterId>(
                            static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_NDDMA_LOOP0_STRIDE) + index));
                    }
                }
                return result;
            } else if constexpr (std::is_same_v<Field, CopyGmToUbufAlignV2ParamField>) {
                return DmaDependencies(value, DmaLoopDirection::GM_TO_UBUF, state);
            } else if constexpr (std::is_same_v<Field, CopyGmToCbufAlignV2ParamField>) {
                return DmaDependencies(value, DmaLoopDirection::GM_TO_CBUF, state);
            } else if constexpr (std::is_same_v<Field, CopyUbufToGmAlignV2ParamField>) {
                auto dependencies = DmaDependencies(value, DmaLoopDirection::UBUF_TO_GM, state);
                if (value.burstNum != NO_TRANSFER_COUNT && value.burstLen != NO_TRANSFER_COUNT) {
                    dependencies.mask0 |= RegisterBit(ACLSAN_DEVICE_REGISTER_CTRL);
                }
                return dependencies;
            } else if constexpr (std::is_same_v<Field, CopyGmToCbufV2ParamField>) {
                if (value.burstNum == NO_TRANSFER_COUNT || value.burstLen == NO_TRANSFER_COUNT) {
                    return {};
                }
                RegisterDependencyMask result = DmaDependencies(value, DmaLoopDirection::GM_TO_CBUF, state);
                if (value.padFunctionMode >= PAD_FUNCTION_MODE_MIN && value.padFunctionMode <= PAD_FUNCTION_MODE_MAX) {
                    result.mask0 |= RegisterBit(ACLSAN_DEVICE_REGISTER_SET_PADDING);
                }
                return result;
            } else if constexpr (std::is_same_v<Field, FixL0cToOutParamField>) {
                RegisterDependencyMask dependencies{};
                if (value.nSize != NO_TRANSFER_COUNT && value.mSize != NO_TRANSFER_COUNT) {
                    dependencies.mask0 = RegisterBit(ACLSAN_DEVICE_REGISTER_CTRL);
                    if (value.nz2ndEnable || value.nz2dnEnable) {
                        dependencies.mask0 |= RegisterBit(ACLSAN_DEVICE_REGISTER_LOOP3);
                    }
                }
                return dependencies;
            }
            return {};
        },
        field);
}

std::optional<AclsanDeviceRegisterStateData> MakeRegisterStateData(
    const ParsedTraceRecord& parsed, const DecodedInstruction& decoded) noexcept
{
    const auto regId = RegisterId(decoded);
    if (!regId.has_value()) {
        return std::nullopt;
    }
    AclsanDeviceRegisterStateData data{};
    data.header = MakeHeader(parsed);
    data.regId = static_cast<uint32_t>(*regId);
    data.value = parsed.record.args[0];
    if (const auto* mask = std::get_if<VectorMaskWriteParamField>(&decoded.params)) {
        data.regIndex = mask->index;
        data.value = mask->value;
    }
    return data;
}

std::array<AclsanDeviceRegisterStateData, VECTOR_MASK_REGISTER_COUNT> MakeRegisterStateDataForVecMask(
    const ParsedTraceRecord& parsed, const VectorMaskParamField& mask) noexcept
{
    std::array<AclsanDeviceRegisterStateData, VECTOR_MASK_REGISTER_COUNT> data{};
    for (uint32_t index = 0; index < data.size(); ++index) {
        data[index].header = MakeHeader(parsed);
        data[index].regId = ACLSAN_DEVICE_REGISTER_VECTOR_MASK;
        data[index].regIndex = index;
        data[index].value = index == 0 ? mask.vectorMask0 : mask.vectorMask1;
    }
    return data;
}

} // namespace aclsan
