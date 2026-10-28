/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_memory_cbdata.h"

#include "acl_san/aclsan_api.h"
#include "device_instr/common/instruction_id.h"
#include "device_instr/arch/dav_3510/decoder.h"
#include "dbi/trace_buffer_abi.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace aclsan {
namespace {

constexpr uint32_t BLOCK_TYPE_AIC = 0;
constexpr uint32_t BLOCK_TYPE_AIV = 1;
constexpr std::size_t MEMORY_ND_MAX_RANK = 5;
constexpr std::size_t MAX_EXPANDED_MEMORY_ACCESSES_PER_INSTRUCTION = 4096;
constexpr uint32_t BITS_PER_BYTE = 8;
constexpr uint32_t CUBE_M_DIMENSION = 16;
constexpr uint32_t CUBE_K_BYTES = 256;
constexpr uint32_t CUBE_L1_BLOCK_BYTES = 32;
constexpr uint32_t CUBE_FRACTAL_BYTES = 512;
constexpr uint32_t CUBE_OUTPUT_FRACTAL_BYTES = 1024;
constexpr uint32_t AIV_BLOCKS_PER_AIC = 2;
constexpr std::size_t TWO_DIMENSIONAL_RANK = 2;
constexpr std::size_t THREE_DIMENSIONAL_RANK = 3;
constexpr uint32_t NO_ACCESS_COUNT = 0U;
constexpr uint32_t SINGLE_ACCESS_COUNT = 1U;
constexpr uint32_t TWO_ACCESS_COUNT = 2U;
constexpr uint32_t FOUR_BIT_DATA = 4U;
constexpr uint32_t SIXTEEN_BIT_DATA = 2U * BITS_PER_BYTE;
constexpr uint32_t THIRTY_TWO_BIT_DATA = 4U * BITS_PER_BYTE;
constexpr uint32_t PAD_FUNCTION_MODE_MIN = 1U;
constexpr uint32_t PAD_FUNCTION_MODE_MAX = 5U;
constexpr uint32_t PAD_FUNCTION_ENCODING_MAX = 8U;
constexpr uint32_t CUBE_C0_BYTES = 32U;
constexpr uint32_t CUBE_N_ALIGNMENT = 16U;
constexpr uint32_t CUBE_PACKED4_ALIGNMENT = 64U;
constexpr uint32_t SUPPORTED_DMA_DIRECTION_COUNT = 3U;
constexpr uint32_t LOOP_DIMENSION_COUNT = 2U;
constexpr uint32_t SUPPORTED_NDDMA_DIMENSION_COUNT = 5U;
constexpr uint32_t CUBE_ROW_BYTES = 64U;
constexpr uint32_t FIXPIPE_CONFIG_LENGTH_SHIFT = 16U;
constexpr uint32_t FIXPIPE_CONFIG_GAP_READ_SHIFT = 32U;
constexpr uint32_t FIXPIPE_CONFIG_GAP_WRITE_SHIFT = 48U;
constexpr uint32_t FIXPIPE_CONFIG_FIELD_MASK = 0xFFFFU;
constexpr uint32_t FIXPIPE_CONFIG_COUNT_SHIFT = 4U;
constexpr uint32_t FIXPIPE_CONFIG_COUNT_MASK = 0xFFFU;
constexpr uint32_t QUANT_PRE_FIELD_MASK = 0xFU;
constexpr uint32_t ATOMIC_TYPE_SHIFT = 6U;
constexpr uint32_t ATOMIC_TYPE_MASK = 7U;
constexpr uint32_t ATOMIC_OPERATION_SHIFT = 9U;
constexpr uint32_t ATOMIC_OPERATION_MASK = 3U;
constexpr uint32_t DUAL_DST_MODE_NONE = 0U;
constexpr uint32_t DUAL_DST_MODE_FIRST = 1U;
constexpr uint32_t DUAL_DST_MODE_SECOND = 2U;
constexpr uint32_t DUAL_DST_MODE_BOTH = 3U;
constexpr uint32_t FOUR_BIT_FRACTAL_COUNT = 4U;
constexpr uint32_t TWO_FRACTAL_COUNT = 2U;
constexpr uint32_t NO_INDEX_ID = 0U;
constexpr uint32_t REGISTER_BANK_A = 0U;
constexpr uint32_t REGISTER_BANK_B = 1U;
constexpr uint32_t DMA_DESCRIPTOR_DIMENSION_COUNT = 3U;
constexpr uint32_t UNSPECIFIED_DATA_BITS = 0U;
constexpr uint32_t SCALAR_SYSTEM_WINDOW_MASK_BITS = 24U;
constexpr uint32_t TRANSPOSE_B4_M_STEP_ALIGNMENT = 4U;
constexpr uint32_t TRANSPOSE_B8_M_STEP_ALIGNMENT = 2U;
constexpr uint32_t TRANSPOSE_B32_K_STEP_ALIGNMENT = 2U;

constexpr std::array<uint8_t, 3> FIXPIPE_F32_32BIT_QUANT_PRE{0, 14, 15};
constexpr std::array<uint8_t, 6> FIXPIPE_F32_16BIT_QUANT_PRE{1, 16, 31, 32, 33, 34};
constexpr std::array<uint8_t, 8> FIXPIPE_F32_8BIT_QUANT_PRE{2, 3, 4, 5, 12, 13, 23, 24};
constexpr std::array<uint8_t, 2> FIXPIPE_F32_4BIT_QUANT_PRE{25, 26};
constexpr std::array<uint8_t, 4> FIXPIPE_S32_16BIT_QUANT_PRE{10, 11, 35, 36};
constexpr std::array<uint8_t, 2> FIXPIPE_S32_8BIT_QUANT_PRE{8, 9};
constexpr std::array<uint8_t, 2> FIXPIPE_S32_4BIT_QUANT_PRE{21, 22};

template <std::size_t N>
bool Contains(const std::array<uint8_t, N>& values, uint8_t value) noexcept
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

uint32_t MmadDataBits(InstructionId id) noexcept
{
    switch (id) {
        case InstructionId::MadF32F32:
            return THIRTY_TWO_BIT_DATA;
        case InstructionId::MadF16F32:
        case InstructionId::MadBf16F32:
            return SIXTEEN_BIT_DATA;
        case InstructionId::MadMxE1m2E1m2:
        case InstructionId::MadMxE1m2E2m1:
        case InstructionId::MadMxE2m1E1m2:
        case InstructionId::MadMxE2m1E2m1:
            return FOUR_BIT_DATA;
        case InstructionId::MadS8:
        case InstructionId::MadE4m3E4m3:
        case InstructionId::MadE4m3E5m2:
        case InstructionId::MadE5m2E4m3:
        case InstructionId::MadE5m2E5m2:
        case InstructionId::MadMxE4m3E4m3:
        case InstructionId::MadMxE4m3E5m2:
        case InstructionId::MadMxE5m2E4m3:
        case InstructionId::MadMxE5m2E5m2:
            return BITS_PER_BYTE;
        default:
            return NO_ACCESS_COUNT;
    }
}

bool IsMmadInstruction(InstructionId id) noexcept { return MmadDataBits(id) != 0; }

uint32_t LoadCbufToL0DataBits(InstructionId id) noexcept
{
    switch (id) {
        case InstructionId::LoadCbufToCaB4:
        case InstructionId::LoadCbufToCbB4:
            return FOUR_BIT_DATA;
        case InstructionId::LoadCbufToCaB16:
        case InstructionId::LoadCbufToCbB16:
            return SIXTEEN_BIT_DATA;
        case InstructionId::LoadCbufToCaB8:
        case InstructionId::LoadCbufToCbB8:
            return BITS_PER_BYTE;
        case InstructionId::LoadCbufToCaB32:
        case InstructionId::LoadCbufToCbB32:
            return THIRTY_TWO_BIT_DATA;
        default:
            return NO_ACCESS_COUNT;
    }
}

uint32_t LoadCbufToCbTransposeDataBits(InstructionId id) noexcept
{
    switch (id) {
        case InstructionId::LoadCbufToCbTransposeB8:
            return BITS_PER_BYTE;
        case InstructionId::LoadCbufToCbTransposeB16:
            return SIXTEEN_BIT_DATA;
        case InstructionId::LoadCbufToCbTransposeB32:
            return THIRTY_TWO_BIT_DATA;
        case InstructionId::LoadCbufToCbTransposeB4:
            return FOUR_BIT_DATA;
        default:
            return NO_ACCESS_COUNT;
    }
}

bool IsImg2ColInstruction(InstructionId id) noexcept
{
    switch (id) {
        case InstructionId::Img2ColCbufToCaB16:
        case InstructionId::Img2ColCbufToCaB8:
        case InstructionId::Img2ColCbufToCbB16:
        case InstructionId::Img2ColCbufToCbB8:
        case InstructionId::Img2ColCbufToCbB32:
        case InstructionId::Img2ColCbufToCaB32:
            return true;
        default:
            return false;
    }
}

uint32_t Img2ColDataBits(InstructionId id) noexcept
{
    switch (id) {
        case InstructionId::Img2ColCbufToCaB16:
        case InstructionId::Img2ColCbufToCbB16:
            return SIXTEEN_BIT_DATA;
        case InstructionId::Img2ColCbufToCaB8:
        case InstructionId::Img2ColCbufToCbB8:
            return BITS_PER_BYTE;
        case InstructionId::Img2ColCbufToCbB32:
        case InstructionId::Img2ColCbufToCaB32:
            return THIRTY_TWO_BIT_DATA;
        default:
            return NO_ACCESS_COUNT;
    }
}

struct RangeDescriptor {
    uint64_t bytes;
};

struct StridedDescriptor {
    uint64_t bytes;
    uint64_t count;
    uint64_t strideBytes;
};

struct NdAffineDescriptor {
    uint32_t rank;
    uint64_t bytes;
    std::array<uint64_t, MEMORY_ND_MAX_RANK> counts;
    std::array<uint64_t, MEMORY_ND_MAX_RANK> strideBytes;
};

using MemoryLayoutDescriptor = std::variant<RangeDescriptor, StridedDescriptor, NdAffineDescriptor>;

struct MemoryAccessDescriptor {
    uint64_t address;
    uint32_t accessMode;
    uint32_t dataBits;
    MemoryLayoutDescriptor layout;
    bool preloadTarget = false;
    uint32_t memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_GM;
};

struct MemoryInstructionProfile {
    uint32_t dataBits;
    AclsanDeviceSourceKind sourceKind;
    uint32_t blockType;
};

class MemoryInstructionProfileFactory final {
public:
    static std::optional<MemoryInstructionProfile> Create(const CopyGmToUbufAlignV2ParamField& field) noexcept
    {
        const uint32_t dataBits = field.dataBits;
        if (!IsSupportedReadDataBits(dataBits)) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, BLOCK_TYPE_AIV};
    }

    static std::optional<MemoryInstructionProfile> Create(const CopyGmToCbufAlignV2ParamField& field) noexcept
    {
        const uint32_t dataBits = field.dataBits;
        if (!IsSupportedReadDataBits(dataBits)) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, BLOCK_TYPE_AIC};
    }

    template <NdNzConversionMode ConversionMode>
    static std::optional<MemoryInstructionProfile> Create(
        const CopyGmToCbufMultiParamField<ConversionMode>& field) noexcept
    {
        const uint32_t dataBits = field.dataBits;
        if (!IsSupportedReadDataBits(dataBits)) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, BLOCK_TYPE_AIC};
    }

    static std::optional<MemoryInstructionProfile> Create(const CopyGmToCbufV2ParamField& field) noexcept
    {
        if (static_cast<InstructionId>(field.instrId) != InstructionId::CopyGmToCbufV2) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{0, ACLSAN_DEVICE_SOURCE_MTE2, BLOCK_TYPE_AIC};
    }

    static std::optional<MemoryInstructionProfile> Create(const CopyUbufToGmAlignV2ParamField& field) noexcept
    {
        if (static_cast<InstructionId>(field.instrId) != InstructionId::CopyUbufToGmAlignV2) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{0, ACLSAN_DEVICE_SOURCE_MTE3, BLOCK_TYPE_AIV};
    }

    static std::optional<MemoryInstructionProfile> Create(const FixL0cToOutParamField& field) noexcept
    {
        const auto id = static_cast<InstructionId>(field.instrId);
        const uint32_t dataBits = FixpipeDestinationDataBits(id, field.quantPre);
        if (dataBits == NO_ACCESS_COUNT) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_FIXPIPE, BLOCK_TYPE_AIC};
    }

    static std::optional<MemoryInstructionProfile> Create(const LoadGmToCbuf2DV2ParamField& field) noexcept
    {
        if (static_cast<InstructionId>(field.instrId) != InstructionId::LoadGmToCbuf2DV2) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{0, ACLSAN_DEVICE_SOURCE_MTE2, BLOCK_TYPE_AIC};
    }

    static std::optional<MemoryInstructionProfile> Create(const NdDmaOutToUbufParamField& field) noexcept
    {
        const uint32_t dataBits = field.dataBits;
        if (!IsSupportedReadDataBits(dataBits)) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, BLOCK_TYPE_AIV};
    }

private:
    static uint32_t FixpipeDestinationDataBits(InstructionId id, uint8_t quantPre) noexcept
    {
        if (id == InstructionId::FixL0cToOutF32) {
            if (Contains(FIXPIPE_F32_32BIT_QUANT_PRE, quantPre)) {
                return THIRTY_TWO_BIT_DATA;
            }
            if (Contains(FIXPIPE_F32_16BIT_QUANT_PRE, quantPre)) {
                return SIXTEEN_BIT_DATA;
            }
            if (Contains(FIXPIPE_F32_8BIT_QUANT_PRE, quantPre)) {
                return BITS_PER_BYTE;
            }
            return Contains(FIXPIPE_F32_4BIT_QUANT_PRE, quantPre) ? FOUR_BIT_DATA : NO_ACCESS_COUNT;
        }
        if (id != InstructionId::FixL0cToOutS32) {
            return NO_ACCESS_COUNT;
        }
        if (quantPre == NO_ACCESS_COUNT) {
            return THIRTY_TWO_BIT_DATA;
        }
        if (Contains(FIXPIPE_S32_16BIT_QUANT_PRE, quantPre)) {
            return SIXTEEN_BIT_DATA;
        }
        if (Contains(FIXPIPE_S32_8BIT_QUANT_PRE, quantPre)) {
            return BITS_PER_BYTE;
        }
        return Contains(FIXPIPE_S32_4BIT_QUANT_PRE, quantPre) ? FOUR_BIT_DATA : NO_ACCESS_COUNT;
    }

    static bool IsSupportedReadDataBits(uint32_t dataBits) noexcept
    {
        return dataBits == BITS_PER_BYTE || dataBits == SIXTEEN_BIT_DATA || dataBits == THIRTY_TWO_BIT_DATA;
    }
};

// 仅用于 L1 <-> UB、L0C -> UB 的 UB 端访问，确定该 UB 所属的 AIV。
// AIC 发起的访问按 subBlockId 映射到目标 AIV；输入已经是 AIV 时直接沿用。
// UB -> UB、GM <-> UB（含 NDDMA）直接使用原 context，不调用此函数。
// 返回 context 的副本，仅调整 coreId/blockId/blockType，保留指令执行信息；映射失败返回 nullopt。
std::optional<MemoryCbdataContext> ContextForUbuf(const MemoryCbdataContext& context, uint32_t subBlockId) noexcept
{
    if (context.blockType == ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR) {
        return context;
    }
    const uint32_t core = AivPhysicalCoreForAic(context.coreId, subBlockId, context.physicalCoreCount);
    const uint32_t block = AivBlockIdForAic(context.blockId, subBlockId);
    if (core == UINT32_MAX || block == UINT32_MAX) {
        return std::nullopt;
    }
    auto result = context;
    result.coreId = core;
    result.blockId = block;
    result.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR;
    return result;
}

// 仅用于 L1 <-> UB 的 L1 端访问，确定该 L1 所属的 AIC。
// UB -> L1 的 AIV context 映射回对应 AIC；L1 -> UB 的 AIC context 直接沿用。
// UB -> UB、GM <-> UB 不调用此函数，其他 L1 访问也直接使用其原 context。
// 返回 context 的副本，仅调整 coreId/blockId/blockType，保留指令执行信息；映射失败返回 nullopt。
std::optional<MemoryCbdataContext> ContextForL1(const MemoryCbdataContext& context) noexcept
{
    if (context.blockType != ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR) {
        return context;
    }
    const uint32_t core = AicPhysicalCoreForAiv(context.coreId, context.physicalCoreCount);
    if (core == UINT32_MAX) {
        return std::nullopt;
    }
    auto result = context;
    result.coreId = core;
    result.blockId /= AIV_BLOCKS_PER_AIC;
    result.blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE;
    return result;
}

class MemoryAccessDescriptorFactory final {
public:
    static MemoryAccessDescriptor Linear(
        uint64_t address, uint32_t accessMode, uint32_t dataBits, uint64_t count, uint64_t bytes,
        uint64_t strideBytes) noexcept
    {
        uint64_t rangeBytes = bytes;
        bool isRange = count == 1;
        if (count > 1 && strideBytes <= bytes &&
            (strideBytes == NO_ACCESS_COUNT ||
             count - 1 <= (std::numeric_limits<uint64_t>::max() - bytes) / strideBytes)) {
            rangeBytes += (count - 1) * strideBytes;
            isRange = true;
        }
        if (isRange) {
            return {address, accessMode, dataBits, RangeDescriptor{rangeBytes}};
        }
        return {address, accessMode, dataBits, StridedDescriptor{bytes, count, strideBytes}};
    }

    static MemoryAccessDescriptor NdRead(
        uint64_t address, uint32_t dataBits, uint64_t bytes, std::array<uint64_t, TWO_DIMENSIONAL_RANK> counts,
        std::array<uint64_t, TWO_DIMENSIONAL_RANK> strides) noexcept
    {
        struct Axis {
            uint64_t count;
            uint64_t stride;
        };
        std::array<Axis, TWO_DIMENSIONAL_RANK> axes{};
        std::size_t axisCount = 0;
        for (std::size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] > SINGLE_ACCESS_COUNT && strides[index] != 0) {
                axes[axisCount++] = {counts[index], strides[index]};
            }
        }
        std::sort(axes.begin(), axes.begin() + axisCount, [](const Axis& left, const Axis& right) {
            return left.stride < right.stride;
        });

        std::array<Axis, TWO_DIMENSIONAL_RANK> sparseAxes{};
        std::size_t sparseCount = 0;
        uint64_t segmentBytes = bytes;
        for (std::size_t index = 0; index < axisCount; ++index) {
            const Axis axis = axes[index];
            const unsigned __int128 span = static_cast<unsigned __int128>(axis.count - 1) * axis.stride + segmentBytes;
            if (axis.stride <= segmentBytes && span <= std::numeric_limits<uint64_t>::max()) {
                segmentBytes = static_cast<uint64_t>(span);
            } else {
                sparseAxes[sparseCount++] = axis;
            }
        }
        if (sparseCount == NO_ACCESS_COUNT) {
            return Linear(address, ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits, 1, segmentBytes, 0);
        }
        if (sparseCount == 1) {
            return Linear(
                address, ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits, sparseAxes[0].count, segmentBytes,
                sparseAxes[0].stride);
        }
        const unsigned __int128 flattenedStride =
            static_cast<unsigned __int128>(sparseAxes[0].stride) * sparseAxes[0].count;
        if (flattenedStride == sparseAxes[1].stride &&
            sparseAxes[0].count <= std::numeric_limits<uint32_t>::max() / sparseAxes[1].count) {
            return Linear(
                address, ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits, sparseAxes[0].count * sparseAxes[1].count,
                segmentBytes, sparseAxes[0].stride);
        }

        NdAffineDescriptor layout{};
        layout.rank = TWO_DIMENSIONAL_RANK;
        layout.bytes = segmentBytes;
        for (std::size_t index = 0; index < sparseCount; ++index) {
            layout.counts[index] = sparseAxes[index].count;
            layout.strideBytes[index] = sparseAxes[index].stride;
        }
        return {address, ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits, layout};
    }

    static MemoryAccessDescriptor NdRead(
        uint64_t address, uint32_t dataBits, uint64_t elementBytes,
        const std::array<uint64_t, SUPPORTED_NDDMA_DIMENSION_COUNT>& counts,
        const std::array<uint64_t, SUPPORTED_NDDMA_DIMENSION_COUNT>& strides) noexcept
    {
        return {
            address, ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits,
            NdAffineDescriptor{MEMORY_ND_MAX_RANK, elementBytes, counts, strides}};
    }

    static MemoryAccessDescriptor NdWrite(
        uint64_t address, uint32_t dataBits, uint64_t bytes, std::array<uint64_t, TWO_DIMENSIONAL_RANK> counts,
        std::array<uint64_t, TWO_DIMENSIONAL_RANK> strides) noexcept
    {
        MemoryAccessDescriptor descriptor = NdRead(address, dataBits, bytes, counts, strides);
        descriptor.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        return descriptor;
    }

    static MemoryAccessDescriptor DmaAffine(
        uint64_t address, uint32_t accessMode, uint32_t dataBits, uint64_t bytes,
        const std::array<uint64_t, THREE_DIMENSIONAL_RANK>& counts,
        const std::array<uint64_t, THREE_DIMENSIONAL_RANK>& strides) noexcept
    {
        NdAffineDescriptor layout{};
        layout.rank = THREE_DIMENSIONAL_RANK;
        layout.bytes = bytes;
        std::copy(counts.begin(), counts.end(), layout.counts.begin());
        std::copy(strides.begin(), strides.end(), layout.strideBytes.begin());
        return {address, accessMode, dataBits, layout};
    }
};

class MemoryCbdataBuilder final {
public:
    MemoryCbdataBuilder(
        MemoryCbdataContext context, MemoryInstructionProfile profile,
        uint32_t memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN) noexcept
        : context_(context), profile_(profile), memorySpace_(memorySpace)
    {}

    MemoryCbdataBuilder(const MemoryCbdataBuilder&) = delete;
    MemoryCbdataBuilder& operator=(const MemoryCbdataBuilder&) = delete;
    MemoryCbdataBuilder(MemoryCbdataBuilder&&) noexcept = default;
    MemoryCbdataBuilder& operator=(MemoryCbdataBuilder&&) noexcept = default;

    [[nodiscard]] bool Add(MemoryAccessDescriptor descriptor) noexcept
    {
        if (memorySpace_ != ACLSAN_DEVICE_MEMORY_SPACE_UNKNOWN) {
            descriptor.memorySpace = memorySpace_;
        }
        if (status_ != MemoryCbdataStatus::SUCCESS) {
            return false;
        }
        unsigned __int128 lastOffset = 0;
        if (!IsRepresentable(descriptor, lastOffset)) {
            Fail(MemoryCbdataStatus::ARITHMETIC_OVERFLOW);
            return false;
        }
        // Suppress only whole accesses contained in this launch's exact internal ranges.
        const auto contains = [&descriptor, &lastOffset](uint64_t base, uint64_t bytes) {
            return descriptor.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM && base != 0 && bytes != 0 &&
                   descriptor.address >= base &&
                   static_cast<unsigned __int128>(base) + bytes <= (static_cast<unsigned __int128>(1) << 64U) &&
                   static_cast<unsigned __int128>(descriptor.address - base) + lastOffset < bytes;
        };
        if (contains(context_.parameterBase, context_.parameterBytes)) {
            return true;
        }
        if (context_.internalInputs != nullptr) {
            for (const auto& input : *context_.internalInputs) {
                if (contains(reinterpret_cast<uintptr_t>(input.address), input.bytes)) {
                    return true;
                }
            }
        }
        if (descriptor.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_READ && context_.readOnlyBinaryRanges != nullptr) {
            for (const auto& range : *context_.readOnlyBinaryRanges) {
                if (contains(reinterpret_cast<uintptr_t>(range.address), range.bytes)) {
                    return true;
                }
            }
        }
        if (descriptors_.size() >= MAX_EXPANDED_MEMORY_ACCESSES_PER_INSTRUCTION) {
            Fail(MemoryCbdataStatus::RESOURCE_EXHAUSTED);
            return false;
        }
        try {
            descriptors_.push_back(std::move(descriptor));
        } catch (const std::bad_alloc&) {
            Fail(MemoryCbdataStatus::RESOURCE_EXHAUSTED);
            return false;
        } catch (const std::length_error&) {
            Fail(MemoryCbdataStatus::RESOURCE_EXHAUSTED);
            return false;
        }
        return true;
    }

    [[nodiscard]] MemoryCbdataResult Build() && noexcept
    {
        if (status_ != MemoryCbdataStatus::SUCCESS) {
            return {status_, {}};
        }
        if (descriptors_.empty()) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }

        MemoryCbdata data;
        try {
            data.reserve(descriptors_.size());
            const AclsanDeviceEventHeader header = MakeHeader();
            for (const auto& descriptor : descriptors_) {
                data.push_back(MakeCbdata(header, descriptor, context_.instructionId));
            }
        } catch (const std::bad_alloc&) {
            return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
        } catch (const std::length_error&) {
            return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
        }
        return {MemoryCbdataStatus::SUCCESS, std::move(data)};
    }

private:
    static bool IsRepresentable(const MemoryAccessDescriptor& descriptor, unsigned __int128& lastOffset) noexcept
    {
        const bool valid = std::visit(
            [&lastOffset](const auto& layout) noexcept {
                using Layout = std::decay_t<decltype(layout)>;
                if (layout.bytes == NO_ACCESS_COUNT) {
                    return false;
                }
                lastOffset = layout.bytes - 1;
                if constexpr (std::is_same_v<Layout, RangeDescriptor>) {
                    return true;
                } else if constexpr (std::is_same_v<Layout, StridedDescriptor>) {
                    if (layout.bytes > std::numeric_limits<uint32_t>::max() || layout.count == NO_ACCESS_COUNT ||
                        layout.count > std::numeric_limits<uint32_t>::max() ||
                        layout.strideBytes > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                        return false;
                    }
                    lastOffset += static_cast<unsigned __int128>(layout.count - 1) * layout.strideBytes;
                    return true;
                } else {
                    if (layout.bytes > std::numeric_limits<uint32_t>::max() || layout.rank == NO_ACCESS_COUNT ||
                        layout.rank > MEMORY_ND_MAX_RANK) {
                        return false;
                    }
                    for (std::size_t index = 0; index < layout.rank; ++index) {
                        if (layout.counts[index] == NO_ACCESS_COUNT ||
                            layout.strideBytes[index] > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                            return false;
                        }
                        lastOffset +=
                            static_cast<unsigned __int128>(layout.counts[index] - 1) * layout.strideBytes[index];
                    }
                    return true;
                }
            },
            descriptor.layout);
        return valid &&
               static_cast<unsigned __int128>(descriptor.address) + lastOffset <= std::numeric_limits<uint64_t>::max();
    }

    void Fail(MemoryCbdataStatus status) noexcept
    {
        status_ = status;
        descriptors_.clear();
    }

    AclsanDeviceEventHeader MakeHeader() const noexcept
    {
        return {ACLSAN_API_VERSION,   static_cast<uint32_t>(sizeof(AclsanDeviceMemoryAccessData)),
                context_.launchId,    context_.pc,
                context_.siteId,      static_cast<uint32_t>(profile_.sourceKind),
                context_.instrExecId, context_.serialNo,
                context_.deviceId,    context_.coreId,
                context_.blockId,     context_.blockType,
                context_.pipeline,    ACLSAN_DEVICE_EVENT_FLAG_EXACT};
    }

    static AclsanDeviceMemoryAccessData MakeCbdata(
        const AclsanDeviceEventHeader& header, const MemoryAccessDescriptor& descriptor,
        uint32_t instructionId) noexcept
    {
        AclsanDeviceMemoryAccessData data{};
        data.header = header;
        if (descriptor.preloadTarget) {
            data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
        }
        data.address = descriptor.address;
        data.memorySpace = descriptor.memorySpace;
        data.accessMode = descriptor.accessMode;
        data.dataBits = descriptor.dataBits;
        data.instructionId = instructionId;
        std::visit(
            [&data](const auto& layout) noexcept {
                using Layout = std::decay_t<decltype(layout)>;
                if constexpr (std::is_same_v<Layout, RangeDescriptor>) {
                    data.layoutKind = ACLSAN_MEM_LAYOUT_RANGE;
                    data.layout.range.bytes = layout.bytes;
                } else if constexpr (std::is_same_v<Layout, StridedDescriptor>) {
                    data.layoutKind = ACLSAN_MEM_LAYOUT_BLOCK_REPEAT;
                    data.layout.blockRepeat = {
                        1, static_cast<uint32_t>(layout.bytes),      0, static_cast<uint32_t>(layout.count),
                        0, static_cast<int64_t>(layout.strideBytes),
                    };
                } else {
                    data.layoutKind = ACLSAN_MEM_LAYOUT_ND_AFFINE;
                    data.layout.ndAffine.rank = layout.rank;
                    data.layout.ndAffine.elementBytes = static_cast<uint32_t>(layout.bytes);
                    std::copy(layout.counts.begin(), layout.counts.end(), data.layout.ndAffine.dims);
                    std::transform(
                        layout.strideBytes.begin(), layout.strideBytes.end(), data.layout.ndAffine.strides,
                        [](uint64_t stride) { return static_cast<int64_t>(stride); });
                }
            },
            descriptor.layout);
        return data;
    }

    MemoryCbdataContext context_;
    MemoryInstructionProfile profile_;
    uint32_t memorySpace_;
    std::vector<MemoryAccessDescriptor> descriptors_;
    MemoryCbdataStatus status_ = MemoryCbdataStatus::SUCCESS;
};

struct ScalarGmAddressResult {
    MemoryCbdataStatus status = MemoryCbdataStatus::SUCCESS;
    uint64_t address = 0;
};

template <typename ParamField>
ScalarGmAddressResult ResolveScalarGmAddress(
    const ParamField& field, int64_t offsetScale, uint64_t accessBytes) noexcept
{
    if (field.post > 1U) {
        return {MemoryCbdataStatus::INVALID_FIELD, 0};
    }
    if (field.addressContext != ASCSAN_SCALAR_ADDRESS_CONTEXT_V1) {
        return {MemoryCbdataStatus::MISSING_ADDRESS_CONTEXT, 0};
    }
    const __int128 offset =
        field.post == 1U ? 0 : static_cast<__int128>(field.offset) * static_cast<__int128>(offsetScale);
    const __int128 address = static_cast<__int128>(field.addr) + offset;
    if (address < 0 || address > std::numeric_limits<uint64_t>::max()) {
        return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, 0};
    }

    // Match dav-3510 RemapAddress after effective-address calculation.
    // Stack backing is irrelevant here: local spaces are outside this GM-only scope.
    const uint64_t effective = static_cast<uint64_t>(address);
    constexpr uint64_t windowMask = (UINT64_C(1) << SCALAR_SYSTEM_WINDOW_MASK_BITS) - 1;
    const bool systemWindow = ((effective >> 25) & windowMask) == ((field.sysVaBase >> 25) & windowMask);
    if (systemWindow && ((effective >> 24) & 1U) == NO_ACCESS_COUNT) {
        const bool ub = ((effective >> 20) & 0x1fU) == 0 && ((effective >> 19) & 1U) != 0;
        const bool stack = ((effective >> 20) & 0xfU) != 0;
        return {
            ub || stack ? MemoryCbdataStatus::UNSUPPORTED_ADDRESS_SPACE : MemoryCbdataStatus::INVALID_ADDRESS_SPACE, 0};
    }
    constexpr uint64_t gmLimit = UINT64_C(1) << 48;
    const uint64_t gmAddress = effective & (gmLimit - 1);
    if (accessBytes > gmLimit - gmAddress) {
        return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, 0};
    }
    return {MemoryCbdataStatus::SUCCESS, gmAddress};
}

class MemoryFieldVisitor final {
public:
    template <typename Field>
    MemoryCbdataResult operator()(const Field&) const noexcept
    {
        return {MemoryCbdataStatus::NO_ACCESS, {}};
    }
    MemoryFieldVisitor(
        MemoryCbdataContext context, const MemoryRegisterState& registerState,
        uint32_t fixOutputSpace = ACLSAN_DEVICE_MEMORY_SPACE_GM) noexcept
        : context_(context), registerState_(registerState), fixOutputSpace_(fixOutputSpace)
    {}

    MemoryCbdataResult operator()(const ScalarGmParamField& field) const noexcept
    {
        const auto firstStore = static_cast<uint32_t>(InstructionId::StB64Imm);
        const auto lastStore = static_cast<uint32_t>(InstructionId::StiB8Reg);
        const auto firstLoad = static_cast<uint32_t>(InstructionId::LdB64Imm);
        const auto lastLoad = static_cast<uint32_t>(InstructionId::LdpB8);
        const bool isStore = field.instrId >= firstStore && field.instrId <= lastStore;
        const bool isLoad = field.instrId >= firstLoad && field.instrId <= lastLoad;
        if (!isStore && !isLoad) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        constexpr uint32_t WIDTHS_PER_FAMILY = 4;
        const uint32_t dataBits = 64U >> ((field.instrId - firstStore) % WIDTHS_PER_FAMILY);
        if (field.dataBits != dataBits) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const bool isPair = (field.instrId >= static_cast<uint32_t>(InstructionId::StpB64) &&
                             field.instrId <= static_cast<uint32_t>(InstructionId::StpB8)) ||
                            (field.instrId >= static_cast<uint32_t>(InstructionId::LdpB64) &&
                             field.instrId <= static_cast<uint32_t>(InstructionId::LdpB8));
        if (isPair && field.post != NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const bool hasRegisterOffset = (field.instrId >= static_cast<uint32_t>(InstructionId::StB64Reg) &&
                                        field.instrId <= static_cast<uint32_t>(InstructionId::StB8Reg)) ||
                                       (field.instrId >= static_cast<uint32_t>(InstructionId::StiB64Reg) &&
                                        field.instrId <= static_cast<uint32_t>(InstructionId::StiB8Reg)) ||
                                       (field.instrId >= static_cast<uint32_t>(InstructionId::LdB64Reg) &&
                                        field.instrId <= static_cast<uint32_t>(InstructionId::LdB8Reg));
        const uint64_t accessBytes = static_cast<uint64_t>(dataBits / 8U) * (isPair ? 2U : 1U);
        const int64_t offsetScale = hasRegisterOffset ? static_cast<int64_t>(dataBits / 8U) : 1;
        const ScalarGmAddressResult resolved = ResolveScalarGmAddress(field, offsetScale, accessBytes);
        if (resolved.status != MemoryCbdataStatus::SUCCESS) {
            return {resolved.status, {}};
        }
        const MemoryInstructionProfile profile{
            dataBits, isStore ? ACLSAN_DEVICE_SOURCE_ST : ACLSAN_DEVICE_SOURCE_LD, context_.blockType};
        MemoryCbdataBuilder builder{context_, profile};
        (void)builder.Add(
            {resolved.address, isStore ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits,
             RangeDescriptor{accessBytes}});
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const ScalarDevParamField& field) const noexcept
    {
        const auto firstStore = static_cast<uint32_t>(InstructionId::StDevB64);
        const auto lastStore = static_cast<uint32_t>(InstructionId::StDevB8);
        const auto firstLoad = static_cast<uint32_t>(InstructionId::LdDevB64);
        const auto lastLoad = static_cast<uint32_t>(InstructionId::LdDevB8);
        const bool isStore = field.instrId >= firstStore && field.instrId <= lastStore;
        const bool isLoad = field.instrId >= firstLoad && field.instrId <= lastLoad;
        if (!isStore && !isLoad) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const uint32_t dataBits = 64U >> (field.instrId - (isStore ? firstStore : firstLoad));
        if (field.dataBits != dataBits) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        // Widen before adding so negative offsets (including INT64_MIN) cannot wrap.
        const __int128 address = static_cast<__int128>(field.addr) + field.offset;
        if (address < 0 || address > std::numeric_limits<uint64_t>::max()) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        const MemoryInstructionProfile profile{
            dataBits, isStore ? ACLSAN_DEVICE_SOURCE_ST : ACLSAN_DEVICE_SOURCE_LD, context_.blockType};
        MemoryCbdataBuilder builder{context_, profile};
        (void)builder.Add(
            {static_cast<uint64_t>(address),
             isStore ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits,
             RangeDescriptor{dataBits / 8U}});
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const ScalarAtomicParamField& field) const noexcept
    {
        const auto firstStAtomic = static_cast<uint32_t>(InstructionId::StAtomicB32);
        const auto lastStAtomic = static_cast<uint32_t>(InstructionId::StAtomicB8);
        const auto firstStiAtomic = static_cast<uint32_t>(InstructionId::StiAtomicB32);
        const auto lastStiAtomic = static_cast<uint32_t>(InstructionId::StiAtomicB8);
        const bool isStAtomic = field.instrId >= firstStAtomic && field.instrId <= lastStAtomic;
        const bool isStiAtomic = field.instrId >= firstStiAtomic && field.instrId <= lastStiAtomic;
        if (!isStAtomic && !isStiAtomic) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const uint32_t dataBits = 32U >> (field.instrId - (isStAtomic ? firstStAtomic : firstStiAtomic));
        if (field.dataBits != dataBits) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        // ST_ATOMIC keeps an element offset; STI_ATOMIC keeps a byte offset.
        const int64_t elementBytes = static_cast<int64_t>(dataBits / 8U);
        const ScalarGmAddressResult resolved =
            ResolveScalarGmAddress(field, isStAtomic ? elementBytes : 1, dataBits / 8U);
        if (resolved.status != MemoryCbdataStatus::SUCCESS) {
            return {resolved.status, {}};
        }
        // ST_ATOMIC_CFG 独立于 CTRL，且 ctrlbin 不会提供它。
        // 保留已知地址和位宽，供 Memcheck 按普通写检查；Racecheck 通过标记识别语义缺失。
        const MemoryInstructionProfile profile{dataBits, ACLSAN_DEVICE_SOURCE_ST, context_.blockType};
        MemoryCbdataBuilder builder{context_, profile};
        (void)builder.Add(
            {resolved.address, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, dataBits, RangeDescriptor{dataBits / 8U}});
        auto result = std::move(builder).Build();
        for (auto& access : result.data) {
            access.header.flags |= static_cast<uint32_t>(ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN);
        }
        return result;
    }

    MemoryCbdataResult operator()(const ScalarPreloadParamField& field) const noexcept
    {
        if (field.instrId != static_cast<uint32_t>(InstructionId::DcPreload) &&
            field.instrId != static_cast<uint32_t>(InstructionId::DcPreloadI)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        // DC_PRELOAD is a cache hint, not a byte-precise GM read. Keep the decoded trace in ACLSan,
        // but do not approximate it with DEVICE_MEMORY_ACCESS cbdata.
        return {MemoryCbdataStatus::NO_ACCESS, {}};
    }

    MemoryCbdataResult operator()(const CopyGmToUbufAlignV2ParamField& field) const noexcept
    {
        return ConvertGmRead(field, DmaLoopDirection::GM_TO_UBUF);
    }

    MemoryCbdataResult operator()(const CopyGmToCbufAlignV2ParamField& field) const noexcept
    {
        return ConvertGmRead(field, DmaLoopDirection::GM_TO_CBUF);
    }

    MemoryCbdataResult operator()(const CopyGmToCbufMultiNd2NzParamField& field) const noexcept
    {
        return ConvertMultiGmRead(field);
    }

    MemoryCbdataResult operator()(const CopyGmToCbufMultiDn2NzParamField& field) const noexcept
    {
        return ConvertMultiGmRead(field);
    }

    MemoryCbdataResult operator()(const CopyGmToCbufV2ParamField& field) const noexcept
    {
        constexpr uint64_t C0_BYTES = CUBE_C0_BYTES;
        constexpr std::array<uint64_t, 5> INSERTED_PADDING_SOURCE_BYTES{1, 2, 4, 8, 16};
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.burstNum == NO_ACCESS_COUNT || field.burstLen == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (field.padFunctionMode > PAD_FUNCTION_ENCODING_MAX ||
            (field.padFunctionMode >= PAD_FUNCTION_MODE_MIN && field.padFunctionMode <= PAD_FUNCTION_MODE_MAX &&
             field.burstLen != SINGLE_ACCESS_COUNT)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const bool insertsPadding =
            field.padFunctionMode >= PAD_FUNCTION_MODE_MIN && field.padFunctionMode <= PAD_FUNCTION_MODE_MAX;
        if (field.burstLen > std::numeric_limits<uint64_t>::max() / C0_BYTES ||
            (!insertsPadding && field.srcStride > std::numeric_limits<uint64_t>::max() / C0_BYTES)) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        const uint64_t burstBytes = insertsPadding ?
                                        INSERTED_PADDING_SOURCE_BYTES[field.padFunctionMode - PAD_FUNCTION_MODE_MIN] :
                                        static_cast<uint64_t>(field.burstLen) * C0_BYTES;
        const uint64_t burstStride = insertsPadding ? burstBytes : field.srcStride * C0_BYTES;
        if (burstBytes > std::numeric_limits<uint32_t>::max() ||
            burstStride > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        return ConvertDmaAccess(
            *profile, DmaLoopDirection::GM_TO_CBUF, field.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, field.burstNum,
            burstBytes, burstStride);
    }

    MemoryCbdataResult operator()(const CopyUbufToGmAlignV2ParamField& field) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.burstNum == NO_ACCESS_COUNT || field.burstLen == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        return ConvertDmaAccess(
            *profile, DmaLoopDirection::UBUF_TO_GM, field.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, field.burstNum,
            field.burstLen, field.dstStride);
    }

    MemoryCbdataResult operator()(const FixL0cToOutParamField& field) const noexcept
    {
        if (HasUnsupportedFixpipeMode(field)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.nSize == NO_ACCESS_COUNT || field.mSize == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if ((field.nz2ndEnable && field.nz2dnEnable) ||
            (field.splitEnable && (field.nz2ndEnable || field.nz2dnEnable))) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.splitEnable && (field.instrId != static_cast<uint32_t>(InstructionId::FixL0cToOutF32) ||
                                  profile->dataBits != THIRTY_TWO_BIT_DATA)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (!field.nz2ndEnable && !field.nz2dnEnable &&
            ((field.splitEnable && field.nSize % (CUBE_N_ALIGNMENT / TWO_ACCESS_COUNT) != 0) ||
             (!field.splitEnable && field.nSize % CUBE_N_ALIGNMENT != 0))) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.c0PadEnable &&
            (field.instrId != static_cast<uint32_t>(InstructionId::FixL0cToOutF32) || field.sid != NO_ACCESS_COUNT ||
             field.nSize % CUBE_N_ALIGNMENT != NO_ACCESS_COUNT || field.mSize != CUBE_N_ALIGNMENT ||
             field.loopDstStride != CUBE_K_BYTES || field.loopSrtStride != CUBE_N_ALIGNMENT ||
             field.l2CacheControl != NO_ACCESS_COUNT || field.clipReluPre != NO_ACCESS_COUNT ||
             field.unitFlag != NO_ACCESS_COUNT || field.quantPre != NO_ACCESS_COUNT ||
             field.reluPre != NO_ACCESS_COUNT || field.splitEnable || field.nz2ndEnable || field.nz2dnEnable)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }

        MemoryCbdataBuilder builder(context_, *profile, fixOutputSpace_);
        if (profile->dataBits == FOUR_BIT_DATA) {
            if (!field.nz2ndEnable && !field.nz2dnEnable && field.nSize % CUBE_PACKED4_ALIGNMENT != 0) {
                return {MemoryCbdataStatus::INVALID_FIELD, {}};
            }
            return ConvertFixpipePacked4(field, std::move(builder));
        }

        const uint64_t dstElementBytes = profile->dataBits / BITS_PER_BYTE;
        if (field.loopDstStride > std::numeric_limits<uint64_t>::max() / dstElementBytes) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        const uint64_t dstStrideBytes = static_cast<uint64_t>(field.loopDstStride) * dstElementBytes;
        if (field.nz2ndEnable || field.nz2dnEnable) {
            if (!registerState_.loop3.has_value()) {
                return {
                    MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::Loop3Param)};
            }
            const Loop3ParamField& loop3 = *registerState_.loop3;
            if (loop3.loopCount == NO_ACCESS_COUNT) {
                return {MemoryCbdataStatus::NO_ACCESS, {}};
            }
            if (loop3.dstStride > std::numeric_limits<uint64_t>::max() / dstElementBytes) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            const uint64_t matrixStrideBytes = static_cast<uint64_t>(loop3.dstStride) * dstElementBytes;
            const uint64_t segmentElements = field.nz2ndEnable ? field.nSize : field.mSize;
            if (segmentElements > std::numeric_limits<uint64_t>::max() / dstElementBytes) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            const uint64_t innerCount = field.nz2ndEnable ? field.mSize : field.nSize;
            return BuildSingleAccess(
                std::move(builder), MemoryAccessDescriptorFactory::NdWrite(
                                        field.dstAddr, profile->dataBits, segmentElements * dstElementBytes,
                                        std::array<uint64_t, TWO_DIMENSIONAL_RANK>{innerCount, loop3.loopCount},
                                        std::array<uint64_t, TWO_DIMENSIONAL_RANK>{dstStrideBytes, matrixStrideBytes}));
        }
        return ConvertFixpipeNz(field, profile->dataBits, dstElementBytes, dstStrideBytes, std::move(builder));
    }

    MemoryCbdataResult operator()(const LoadGmToCbuf2DV2ParamField& field) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.decompMode != 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (field.mStep == NO_ACCESS_COUNT || field.kStep == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (!registerState_.mte2Source.has_value()) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::Mte2SrcPara)};
        }
        const int64_t signedSrcStride = registerState_.mte2Source->srcStride;
        const __int128 strideMagnitude =
            signedSrcStride < 0 ? -static_cast<__int128>(signedSrcStride) : static_cast<__int128>(signedSrcStride);
        const __int128 repeatStrideBytes = strideMagnitude * CUBE_FRACTAL_BYTES;
        if (repeatStrideBytes > std::numeric_limits<int64_t>::max()) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }

        const __int128 firstFractal =
            static_cast<__int128>(field.kStartPosition) * strideMagnitude + field.mStartPosition;
        const __int128 firstAddress = static_cast<__int128>(field.srcAddr) + firstFractal * CUBE_FRACTAL_BYTES;
        const __int128 lowestAddress = signedSrcStride < 0 ?
                                           firstAddress - static_cast<__int128>(field.kStep - 1) * repeatStrideBytes :
                                           firstAddress;
        if (lowestAddress < 0 || lowestAddress > std::numeric_limits<uint64_t>::max()) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        MemoryCbdataBuilder builder(context_, *profile);
        return BuildSingleAccess(
            std::move(builder), MemoryAccessDescriptorFactory::NdRead(
                                    static_cast<uint64_t>(lowestAddress), profile->dataBits, CUBE_FRACTAL_BYTES,
                                    std::array<uint64_t, TWO_DIMENSIONAL_RANK>{field.mStep, field.kStep},
                                    std::array<uint64_t, TWO_DIMENSIONAL_RANK>{
                                        CUBE_FRACTAL_BYTES, static_cast<uint64_t>(repeatStrideBytes)}));
    }

    MemoryCbdataResult operator()(const NdDmaOutToUbufParamField& field) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const std::array<uint64_t, SUPPORTED_NDDMA_DIMENSION_COUNT> counts{
            field.loop0Size, field.loop1Size, field.loop2Size, field.loop3Size, field.loop4Size};
        if (std::any_of(counts.begin(), counts.end(), [](uint64_t count) { return count == NO_ACCESS_COUNT; })) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (field.paddingMode && !registerState_.ndDmaPadCount.has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::NdDmaPadCount)};
        }
        for (std::size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] > SINGLE_ACCESS_COUNT && !registerState_.ndDmaLoopStrides[index].has_value()) {
                return {
                    MemoryCbdataStatus::MISSING_REGISTER_STATE,
                    {},
                    static_cast<uint64_t>(InstructionId::NdDmaLoop0Stride) + index};
            }
        }

        const uint64_t elementBytes = profile->dataBits / BITS_PER_BYTE;
        std::array<uint64_t, SUPPORTED_NDDMA_DIMENSION_COUNT> strideBytes{};
        for (std::size_t index = 0; index < strideBytes.size(); ++index) {
            if (counts[index] <= SINGLE_ACCESS_COUNT) {
                continue;
            }
            const uint64_t srcStride = registerState_.ndDmaLoopStrides[index]->srcStride;
            if (srcStride > std::numeric_limits<uint64_t>::max() / elementBytes) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            strideBytes[index] = srcStride * elementBytes;
        }

        MemoryCbdataBuilder builder(context_, *profile);
        return BuildSingleAccess(
            std::move(builder),
            MemoryAccessDescriptorFactory::NdRead(field.srcAddr, profile->dataBits, elementBytes, counts, strideBytes));
    }

private:
    static bool HasUnsupportedFixpipeMode(const FixL0cToOutParamField& field) noexcept
    {
        return field.quantPost != NO_ACCESS_COUNT || field.reluPost != NO_ACCESS_COUNT || field.clipReluPost ||
               field.loopEnhanceEnable || field.eltwiseOp != NO_ACCESS_COUNT || field.eltwiseAntqEnable ||
               field.loopEnhanceMergeEnable || field.winoPostEnable || field.brcbEnable;
    }

    template <typename Field>
    MemoryCbdataResult ConvertGmRead(const Field& field, DmaLoopDirection direction) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.burstNum == NO_ACCESS_COUNT || field.burstLen == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        return ConvertDmaAccess(
            *profile, direction, field.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, field.burstNum, field.burstLen,
            field.burstSrcStride);
    }

    MemoryCbdataResult ConvertDmaAccess(
        const MemoryInstructionProfile& profile, DmaLoopDirection direction, uint64_t address, uint32_t accessMode,
        uint64_t burstNum, uint64_t burstBytes, uint64_t burstStride) const noexcept
    {
        if (burstBytes > std::numeric_limits<uint32_t>::max() ||
            burstStride > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        const auto directionIndex = static_cast<std::size_t>(direction);
        if (directionIndex >= registerState_.dmaLoopSizes.size()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }

        if (!registerState_.dmaLoopSizes[directionIndex].has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE,
                {},
                static_cast<uint64_t>(InstructionId::LoopSizeUbufToGm) +
                    directionIndex * SUPPORTED_DMA_DIRECTION_COUNT};
        }
        const DmaLoopSizeParamField& size = *registerState_.dmaLoopSizes[directionIndex];
        const std::array<uint64_t, LOOP_DIMENSION_COUNT> loopCounts{size.loop1Size, size.loop2Size};
        if (loopCounts[0] == NO_ACCESS_COUNT || loopCounts[1] == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }

        std::array<uint64_t, LOOP_DIMENSION_COUNT> loopStrides{};
        for (std::size_t index = 0; index < loopCounts.size(); ++index) {
            if (loopCounts[index] <= SINGLE_ACCESS_COUNT) {
                continue;
            }
            const auto& strideState = registerState_.dmaLoopStrides[directionIndex][index];
            if (!strideState.has_value()) {
                return {
                    MemoryCbdataStatus::MISSING_REGISTER_STATE,
                    {},
                    MissingDmaLoopStrideInstructionId(direction, index)};
            }
            loopStrides[index] =
                direction == DmaLoopDirection::UBUF_TO_GM ? strideState->dstStride : strideState->srcStride;
            if (loopStrides[index] > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
        }

        MemoryCbdataBuilder builder(context_, profile);
        if (loopCounts[0] == SINGLE_ACCESS_COUNT && loopCounts[1] == SINGLE_ACCESS_COUNT) {
            return BuildSingleAccess(
                std::move(builder), MemoryAccessDescriptorFactory::Linear(
                                        address, accessMode, profile.dataBits, burstNum, burstBytes, burstStride));
        }
        return BuildSingleAccess(
            std::move(builder),
            MemoryAccessDescriptorFactory::DmaAffine(
                address, accessMode, profile.dataBits, burstBytes,
                std::array<uint64_t, DMA_DESCRIPTOR_DIMENSION_COUNT>{burstNum, loopCounts[0], loopCounts[1]},
                std::array<uint64_t, DMA_DESCRIPTOR_DIMENSION_COUNT>{burstStride, loopStrides[0], loopStrides[1]}));
    }

    template <NdNzConversionMode ConversionMode>
    MemoryCbdataResult ConvertMultiGmRead(const CopyGmToCbufMultiParamField<ConversionMode>& field) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const uint64_t elementBytes = profile->dataBits / BITS_PER_BYTE;
        const uint64_t rowCount = ConversionMode == NdNzConversionMode::ND2NZ ? field.nValue : field.dValue;
        const uint64_t rowElements = ConversionMode == NdNzConversionMode::ND2NZ ? field.dValue : field.nValue;
        if (rowCount == NO_ACCESS_COUNT || rowElements == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (!registerState_.mte2Nz.has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::SetMte2NzPara)};
        }
        const uint16_t matrixNum = registerState_.mte2Nz->matrixNum;
        if (matrixNum == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (rowElements > std::numeric_limits<uint64_t>::max() / elementBytes) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }

        MemoryCbdataBuilder builder(context_, *profile);
        return BuildSingleAccess(
            std::move(builder),
            MemoryAccessDescriptorFactory::NdRead(
                field.srcAddr, profile->dataBits, rowElements * elementBytes,
                std::array<uint64_t, TWO_DIMENSIONAL_RANK>{rowCount, matrixNum},
                std::array<uint64_t, TWO_DIMENSIONAL_RANK>{field.loop1SrcStride, field.loop4SrcStride}));
    }

    static MemoryCbdataResult BuildSingleAccess(
        MemoryCbdataBuilder&& builder, MemoryAccessDescriptor descriptor) noexcept
    {
        if (!builder.Add(std::move(descriptor))) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    static uint64_t MissingDmaLoopStrideInstructionId(DmaLoopDirection direction, std::size_t loopIndex) noexcept
    {
        uint64_t loop1InstructionId = 0;
        switch (direction) {
            case DmaLoopDirection::UBUF_TO_GM:
                loop1InstructionId = static_cast<uint64_t>(InstructionId::Loop1StrideUbufToGm);
                break;
            case DmaLoopDirection::GM_TO_UBUF:
                loop1InstructionId = static_cast<uint64_t>(InstructionId::Loop1StrideGmToUbuf);
                break;
            case DmaLoopDirection::GM_TO_CBUF:
                loop1InstructionId = static_cast<uint64_t>(InstructionId::Loop1StrideGmToCbuf);
                break;
        }
        return loop1InstructionId + loopIndex;
    }

    static std::optional<MemoryAccessDescriptor> Packed4Write(
        uint64_t baseAddress, unsigned __int128 elementOffset, unsigned __int128 elementCount) noexcept
    {
        const unsigned __int128 bitOffset = elementOffset * 4U;
        const unsigned __int128 byteOffset = bitOffset / 8U;
        const unsigned __int128 leadingBits = bitOffset % 8U;
        const unsigned __int128 byteCount = (leadingBits + elementCount * 4U + 7U) / 8U;
        if (elementCount == NO_ACCESS_COUNT || byteOffset > std::numeric_limits<uint64_t>::max() ||
            byteCount > std::numeric_limits<uint64_t>::max() ||
            static_cast<unsigned __int128>(baseAddress) + byteOffset > std::numeric_limits<uint64_t>::max()) {
            return std::nullopt;
        }
        return MemoryAccessDescriptorFactory::Linear(
            baseAddress + static_cast<uint64_t>(byteOffset), ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 4, 1,
            static_cast<uint64_t>(byteCount), 0);
    }

    MemoryCbdataResult ConvertFixpipePacked4(
        const FixL0cToOutParamField& field, MemoryCbdataBuilder&& builder) const noexcept
    {
        MemoryCbdataStatus segmentStatus = MemoryCbdataStatus::SUCCESS;
        auto addSegment = [&builder, &field, &segmentStatus](
                              unsigned __int128 offset, unsigned __int128 elements) noexcept {
            const auto descriptor = Packed4Write(field.dstAddr, offset, elements);
            if (!descriptor.has_value()) {
                segmentStatus = MemoryCbdataStatus::ARITHMETIC_OVERFLOW;
                return false;
            }
            return builder.Add(*descriptor);
        };

        if (field.nz2ndEnable || field.nz2dnEnable) {
            if (!registerState_.loop3.has_value()) {
                return {
                    MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::Loop3Param)};
            }
            const Loop3ParamField& loop3 = *registerState_.loop3;
            if (loop3.loopCount == NO_ACCESS_COUNT) {
                return {MemoryCbdataStatus::NO_ACCESS, {}};
            }
            const uint64_t innerCount = field.nz2ndEnable ? field.mSize : field.nSize;
            const unsigned __int128 segmentCount = static_cast<unsigned __int128>(innerCount) * loop3.loopCount;
            if (segmentCount > MAX_EXPANDED_MEMORY_ACCESSES_PER_INSTRUCTION) {
                return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
            }
            const uint64_t segmentElements = field.nz2ndEnable ? field.nSize : field.mSize;
            for (uint64_t inner = 0; inner < innerCount; ++inner) {
                for (uint64_t matrix = 0; matrix < loop3.loopCount; ++matrix) {
                    const unsigned __int128 offset = static_cast<unsigned __int128>(inner) * field.loopDstStride +
                                                     static_cast<unsigned __int128>(matrix) * loop3.dstStride;
                    if (!addSegment(offset, segmentElements)) {
                        return segmentStatus == MemoryCbdataStatus::SUCCESS ? std::move(builder).Build() :
                                                                              MemoryCbdataResult{segmentStatus, {}};
                    }
                }
            }
            return std::move(builder).Build();
        }

        constexpr uint32_t kFractalSize = 64;
        const uint32_t fullGroups = field.nSize / kFractalSize;
        const uint32_t tailRows = field.nSize % kFractalSize;
        for (uint32_t group = 0; group < fullGroups; ++group) {
            if (!addSegment(
                    static_cast<unsigned __int128>(group) * field.loopDstStride,
                    static_cast<unsigned __int128>(field.mSize) * kFractalSize)) {
                return segmentStatus == MemoryCbdataStatus::SUCCESS ? std::move(builder).Build() :
                                                                      MemoryCbdataResult{segmentStatus, {}};
            }
        }
        if (tailRows != 0 && !addSegment(
                                 static_cast<unsigned __int128>(fullGroups) * field.loopDstStride,
                                 static_cast<unsigned __int128>(field.mSize) * tailRows)) {
            return segmentStatus == MemoryCbdataStatus::SUCCESS ? std::move(builder).Build() :
                                                                  MemoryCbdataResult{segmentStatus, {}};
        }
        return std::move(builder).Build();
    }

    static MemoryCbdataResult ConvertFixpipeNz(
        const FixL0cToOutParamField& field, uint32_t dataBits, uint64_t dstElementBytes, uint64_t groupStrideBytes,
        MemoryCbdataBuilder&& builder) noexcept
    {
        constexpr uint32_t kDefaultFractalSize = 16;
        constexpr uint32_t kChannelSplitFractalSize = 8;
        constexpr uint32_t kChannelMergeB8FractalSize = 32;
        const uint32_t fractalSize = field.splitEnable ? kChannelSplitFractalSize :
                                     dataBits == 8     ? kChannelMergeB8FractalSize :
                                                         kDefaultFractalSize;
        const uint32_t fullGroups = field.nSize / fractalSize;
        const uint32_t tailRows = field.nSize % fractalSize;
        if (fullGroups != 0) {
            const uint64_t groupBytes = static_cast<uint64_t>(field.mSize) * fractalSize * dstElementBytes;
            if (groupBytes > std::numeric_limits<uint32_t>::max()) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            if (!builder.Add(MemoryAccessDescriptorFactory::Linear(
                    field.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, dataBits, fullGroups, groupBytes,
                    groupStrideBytes))) {
                return std::move(builder).Build();
            }
        }
        if (tailRows != 0) {
            if (groupStrideBytes != 0 &&
                fullGroups > (std::numeric_limits<uint64_t>::max() - field.dstAddr) / groupStrideBytes) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            const uint64_t tailAddress = field.dstAddr + static_cast<uint64_t>(fullGroups) * groupStrideBytes;
            const uint64_t tailBytes = static_cast<uint64_t>(field.mSize) * tailRows * dstElementBytes;
            if (tailBytes > std::numeric_limits<uint32_t>::max()) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            if (!builder.Add(MemoryAccessDescriptorFactory::Linear(
                    tailAddress, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, dataBits, 1, tailBytes, groupStrideBytes))) {
                return std::move(builder).Build();
            }
        }
        return std::move(builder).Build();
    }

    MemoryCbdataContext context_;
    const MemoryRegisterState& registerState_;
    uint32_t fixOutputSpace_;
};

// Local accesses are kept separate from the existing GM conversion so that enabling
// Cube checks does not change the GM address sets or quantization rules.
class CubeMemoryFieldVisitor final {
public:
    CubeMemoryFieldVisitor(MemoryCbdataContext context, const MemoryRegisterState& state)
        : context_(context), state_(state)
    {}

    template <typename Field>
    MemoryCbdataResult operator()(const Field&) const noexcept
    {
        return {MemoryCbdataStatus::NO_ACCESS, {}};
    }

    MemoryCbdataResult operator()(const SetL12DParamField& f) const noexcept
    {
        return Linear(
            f.dstAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.repeatTimes,
            uint64_t(f.blockNum) * CUBE_L1_BLOCK_BYTES, uint64_t(f.repeatGap) * CUBE_L1_BLOCK_BYTES, f.dataBits);
    }

    MemoryCbdataResult operator()(const LoadL1MxParamField& f) const noexcept
    {
        const uint64_t offset = (uint64_t(f.xStart) * f.srcStride + f.yStart) * CUBE_L1_BLOCK_BYTES;
        if (f.srcAddr > UINT64_MAX - offset) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        return Linear(
            f.srcAddr + offset, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.xStep,
            uint64_t(f.yStep) * CUBE_L1_BLOCK_BYTES, uint64_t(f.srcStride) * CUBE_L1_BLOCK_BYTES);
    }

    MemoryCbdataResult operator()(const MmadParamField& f) const noexcept
    {
        const auto instructionId = static_cast<InstructionId>(f.instrId);
        if (!IsMmadInstruction(instructionId)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.m == NO_ACCESS_COUNT || f.n == NO_ACCESS_COUNT || f.k == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const uint32_t bits = MmadDataBits(instructionId);
        const uint64_t m = CeilDiv(f.m, CUBE_M_DIMENSION), n = CeilDiv(f.n, CUBE_M_DIMENSION),
                       k = CeilDiv(f.k, CUBE_K_BYTES / bits);
        uint64_t aFractals = m * k;
        if (f.m == 1 && !f.disableGemv) {
            // The reference packs GEMV's A into 16-row fractals. Do not silently
            // truncate a partial fractal where the instruction constraints are unknown.
            if (aFractals % CUBE_M_DIMENSION != 0) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            aFractals /= CUBE_M_DIMENSION;
        }
        MemoryCbdataBuilder builder(context_, {bits, ACLSAN_DEVICE_SOURCE_CUBE, BLOCK_TYPE_AIC});
        if (!Add(
                builder, f.src0Addr, ACLSAN_DEVICE_MEMORY_SPACE_L0A, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1,
                aFractals * CUBE_FRACTAL_BYTES, 0, bits) ||
            !Add(
                builder, f.src1Addr, ACLSAN_DEVICE_MEMORY_SPACE_L0B, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1,
                n * k * CUBE_FRACTAL_BYTES, 0, bits) ||
            !Add(
                builder, f.dstAddr, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 1,
                m * n * CUBE_OUTPUT_FRACTAL_BYTES, 0, THIRTY_TWO_BIT_DATA)) {
            return std::move(builder).Build();
        }
        if (!f.cmatrixSource && !f.cmatrixInitVal &&
            !Add(
                builder, f.dstAddr, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1,
                m * n * CUBE_OUTPUT_FRACTAL_BYTES, 0, THIRTY_TWO_BIT_DATA)) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const LoadCbufToL0ParamField& f) const noexcept
    {
        const auto instructionId = static_cast<InstructionId>(f.instrId);
        const uint32_t bits = LoadCbufToL0DataBits(instructionId);
        if (bits == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.mStep == NO_ACCESS_COUNT || f.kStep == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        uint64_t dstBlocks = f.mStep, dstRepeats = f.kStep;
        if (f.transpose) {
            if ((bits == FOUR_BIT_DATA && f.mStep % TRANSPOSE_B4_M_STEP_ALIGNMENT != 0U) ||
                (bits == BITS_PER_BYTE && f.mStep % TRANSPOSE_B8_M_STEP_ALIGNMENT != 0U) ||
                (bits == THIRTY_TWO_BIT_DATA && f.kStep % TRANSPOSE_B32_K_STEP_ALIGNMENT != 0U)) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            dstBlocks = uint64_t(f.kStep) * CUBE_M_DIMENSION / bits;
            dstRepeats = uint64_t(f.mStep) * bits / CUBE_M_DIMENSION;
        }
        const uint64_t offset = (uint64_t(f.kStartPosition) * f.srcStride + f.mStartPosition) * CUBE_FRACTAL_BYTES;
        if (f.srcAddr > UINT64_MAX - offset) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        MemoryCbdataBuilder builder(context_, {bits, ACLSAN_DEVICE_SOURCE_CUBE, BLOCK_TYPE_AIC});
        if (!Add(
                builder, f.srcAddr + offset, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.kStep,
                uint64_t(f.mStep) * CUBE_FRACTAL_BYTES, uint64_t(f.srcStride) * CUBE_FRACTAL_BYTES, bits) ||
            !Add(
                builder, f.dstAddr,
                instructionId == InstructionId::LoadCbufToCaB4 || instructionId == InstructionId::LoadCbufToCaB16 ||
                        instructionId == InstructionId::LoadCbufToCaB8 ||
                        instructionId == InstructionId::LoadCbufToCaB32 ?
                    ACLSAN_DEVICE_MEMORY_SPACE_L0A :
                    ACLSAN_DEVICE_MEMORY_SPACE_L0B,
                ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, dstRepeats, dstBlocks * CUBE_FRACTAL_BYTES,
                uint64_t(f.dstStride) * CUBE_FRACTAL_BYTES, bits)) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const LoadCbufToCbTransposeParamField& f) const noexcept
    {
        const auto instructionId = static_cast<InstructionId>(f.instrId);
        const uint32_t bits = LoadCbufToCbTransposeDataBits(instructionId);
        if (bits == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.repeat == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        // The dav-3510 reference does not restore index/decrement addressing.
        // Do not pretend those modes use the same source base as index zero.
        if (f.indexId != NO_INDEX_ID || f.decrement) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const uint64_t fractals = bits == FOUR_BIT_DATA    ? FOUR_BIT_FRACTAL_COUNT :
                                  bits == SIXTEEN_BIT_DATA ? SINGLE_ACCESS_COUNT :
                                                             TWO_FRACTAL_COUNT;
        const uint64_t srcFracStride = bits == SIXTEEN_BIT_DATA ? SINGLE_ACCESS_COUNT : uint64_t(f.srcFracGap) + 1;
        const uint64_t dstFracStride = bits == SIXTEEN_BIT_DATA ? SINGLE_ACCESS_COUNT : uint64_t(f.dstFracGap) + 1;
        auto read = MemoryAccessDescriptorFactory::NdRead(
            f.srcAddr, bits, CUBE_FRACTAL_BYTES, std::array<uint64_t, TWO_DIMENSIONAL_RANK>{fractals, f.repeat},
            std::array<uint64_t, TWO_DIMENSIONAL_RANK>{
                srcFracStride * CUBE_FRACTAL_BYTES, uint64_t(f.srcStride) * CUBE_FRACTAL_BYTES});
        auto write = MemoryAccessDescriptorFactory::NdWrite(
            f.dstAddr, bits, CUBE_FRACTAL_BYTES, {fractals, f.repeat},
            {dstFracStride * CUBE_FRACTAL_BYTES, (uint64_t(f.dstGap) + 1) * CUBE_FRACTAL_BYTES});
        return Merge(
            Build(std::move(read), ACLSAN_DEVICE_MEMORY_SPACE_L1),
            Build(std::move(write), ACLSAN_DEVICE_MEMORY_SPACE_L0B));
    }

    MemoryCbdataResult operator()(const Img2ColParamField& f) const noexcept
    {
        const auto instructionId = static_cast<InstructionId>(f.instrId);
        if (!IsImg2ColInstruction(instructionId)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.mExtension == NO_ACCESS_COUNT || f.kExtension == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const size_t bank = f.fMatrixControl ? REGISTER_BANK_B : REGISTER_BANK_A;
        if (!state_.fmatrix[bank].has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE,
                {},
                static_cast<uint64_t>(InstructionId::SetFmatrix) + bank};
        }
        if (!state_.l3dRpt[bank].has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::SetL3dRpt) + bank};
        }
        const uint32_t bits = Img2ColDataBits(instructionId);
        const uint64_t c0 = CUBE_K_BYTES / bits;
        const auto& rpt = *state_.l3dRpt[bank];
        // For one full C0, NC1HWC0 and NHWC share the same byte layout. Other
        // channel packing and repeated-window modes need separate validation.
        if (f.channelSize != c0 || rpt.repeatTimes != SINGLE_ACCESS_COUNT || f.mExtension % CUBE_M_DIMENSION != 0U ||
            f.kExtension % c0 != 0U || f.kStartPoint % c0 != 0U ||
            (f.transpose && (f.mExtension % c0 != 0U || f.kExtension % CUBE_M_DIMENSION != 0U))) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const auto& fm = *state_.fmatrix[bank];
        const uint64_t width = fm.width, height = fm.height;
        const int64_t left = fm.paddingLeft, right = fm.paddingRight;
        const int64_t top = fm.paddingTop, bottom = fm.paddingBottom;
        const uint64_t fw = uint64_t(f.filterWidth) + (f.filterSizeWidth ? CUBE_K_BYTES : 0);
        const uint64_t fh = uint64_t(f.filterHeight) + (f.filterSizeHeight ? CUBE_K_BYTES : 0);
        if (width == NO_ACCESS_COUNT || height == NO_ACCESS_COUNT || fw == NO_ACCESS_COUNT || fh == NO_ACCESS_COUNT ||
            f.strideWidth == NO_ACCESS_COUNT || f.strideHeight == NO_ACCESS_COUNT ||
            f.dilationFilterWidth == NO_ACCESS_COUNT || f.dilationFilterHeight == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const int64_t availableW = int64_t(width) + left + right - int64_t((fw - 1) * f.dilationFilterWidth + 1);
        const int64_t availableH = int64_t(height) + top + bottom - int64_t((fh - 1) * f.dilationFilterHeight + 1);
        if (availableW < 0 || availableH < 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const uint64_t outputW = uint64_t(availableW) / f.strideWidth + 1;
        const uint64_t outputH = uint64_t(availableH) / f.strideHeight + 1;
        if (uint64_t(f.mStartPoint) + f.mExtension > outputW * outputH ||
            uint64_t(f.kStartPoint) + f.kExtension > fw * fh * c0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        if (uint64_t(f.mExtension) * (f.kExtension / c0) >= MAX_EXPANDED_MEMORY_ACCESSES_PER_INSTRUCTION) {
            return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
        }
        MemoryCbdataBuilder builder(context_, {bits, ACLSAN_DEVICE_SOURCE_CUBE, BLOCK_TYPE_AIC});
        for (uint64_t m = f.mStartPoint; m < uint64_t(f.mStartPoint) + f.mExtension; ++m) {
            for (uint64_t k = f.kStartPoint / c0; k < (uint64_t(f.kStartPoint) + f.kExtension) / c0; ++k) {
                const int64_t y = int64_t((m / outputW) * f.strideHeight + (k / fw) * f.dilationFilterHeight) - top;
                const int64_t x = int64_t((m % outputW) * f.strideWidth + (k % fw) * f.dilationFilterWidth) - left;
                // Padding synthesizes values: it does not read outside the feature map.
                if (y < 0 || x < 0 || uint64_t(y) >= height || uint64_t(x) >= width) {
                    continue;
                }
                const uint64_t offset = (uint64_t(y) * width + uint64_t(x)) * CUBE_L1_BLOCK_BYTES;
                if (f.srcAddr > UINT64_MAX - offset) {
                    return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
                }
                if (!Add(
                        builder, f.srcAddr + offset, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1,
                        CUBE_L1_BLOCK_BYTES, 0, bits)) {
                    return std::move(builder).Build();
                }
            }
        }
        const uint64_t dstOffset = uint64_t(rpt.dstOffset) * CUBE_FRACTAL_BYTES;
        if (f.dstAddr > UINT64_MAX - dstOffset) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        const uint64_t rows = f.transpose ? f.kExtension / CUBE_M_DIMENSION : f.mExtension / CUBE_M_DIMENSION;
        const uint64_t columns = f.transpose ? f.mExtension / c0 : f.kExtension / c0;
        const uint32_t space =
            (instructionId == InstructionId::Img2ColCbufToCaB16 || instructionId == InstructionId::Img2ColCbufToCaB8 ||
             instructionId == InstructionId::Img2ColCbufToCaB32) ?
                ACLSAN_DEVICE_MEMORY_SPACE_L0A :
                ACLSAN_DEVICE_MEMORY_SPACE_L0B;
        if (!Add(
                builder, f.dstAddr + dstOffset, space, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, columns,
                rows * CUBE_FRACTAL_BYTES, uint64_t(rpt.dstStride) * CUBE_FRACTAL_BYTES, bits)) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const CopyGmToCbufV2ParamField& f) const noexcept
    {
        if (f.burstNum == NO_ACCESS_COUNT || f.burstLen == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (f.padFunctionMode != NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        return DmaWrite(
            f.dstAddr, f.burstNum, uint64_t(f.burstLen) * CUBE_L1_BLOCK_BYTES,
            (uint64_t(f.burstLen) + f.dstStride) * CUBE_L1_BLOCK_BYTES, 0);
    }

    MemoryCbdataResult operator()(const CopyGmToCbufAlignV2ParamField& f) const noexcept
    {
        if (f.burstLen == NO_ACCESS_COUNT || f.burstNum == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const uint64_t padding = uint64_t(f.leftPaddingCount + f.rightPaddingCount) * (f.dataBits / BITS_PER_BYTE);
        // With no padding, the padding-value selector cannot change the written bytes.
        if ((f.dataSelectBit && padding != NO_ACCESS_COUNT) ||
            f.leftPaddingCount * (f.dataBits / BITS_PER_BYTE) > CUBE_L1_BLOCK_BYTES ||
            f.rightPaddingCount * (f.dataBits / BITS_PER_BYTE) > CUBE_L1_BLOCK_BYTES) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const auto& loops = state_.dmaLoopSizes[static_cast<size_t>(DmaLoopDirection::GM_TO_CBUF)];
        if (padding != 0U && loops.has_value() &&
            (loops->loop1Size > SINGLE_ACCESS_COUNT || loops->loop2Size > SINGLE_ACCESS_COUNT)) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const bool compact = f.burstDstStride == f.burstLen;
        if (!compact && f.burstDstStride % CUBE_L1_BLOCK_BYTES != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const uint64_t bytes =
            CeilDiv((uint64_t(f.burstLen) + padding) * (compact ? f.burstNum : 1), CUBE_L1_BLOCK_BYTES) *
            CUBE_L1_BLOCK_BYTES;
        return DmaWrite(f.dstAddr, compact ? SINGLE_ACCESS_COUNT : f.burstNum, bytes, f.burstDstStride, f.dataBits);
    }

    MemoryCbdataResult operator()(const LoadGmToCbuf2DV2ParamField& f) const noexcept
    {
        if (f.decompMode != NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        return Linear(
            f.dstAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.kStep,
            uint64_t(f.mStep) * CUBE_FRACTAL_BYTES, uint64_t(f.dstStride) * CUBE_FRACTAL_BYTES);
    }

    template <NdNzConversionMode Mode>
    MemoryCbdataResult operator()(const CopyGmToCbufMultiParamField<Mode>& f) const noexcept
    {
        if (!state_.mte2Nz.has_value()) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, uint64_t(InstructionId::SetMte2NzPara)};
        }
        if (f.smallC0Enable) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const auto& s = *state_.mte2Nz;
        if (f.nValue == NO_ACCESS_COUNT || f.dValue == NO_ACCESS_COUNT || s.matrixNum == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        auto d = MemoryAccessDescriptorFactory::DmaAffine(
            f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.dataBits, CUBE_L1_BLOCK_BYTES,
            {f.nValue, CeilDiv(uint64_t(f.dValue) * (f.dataBits / BITS_PER_BYTE), CUBE_L1_BLOCK_BYTES), s.matrixNum},
            {uint64_t(s.loop2DstStride) * CUBE_L1_BLOCK_BYTES, uint64_t(s.loop3DstStride) * CUBE_L1_BLOCK_BYTES,
             uint64_t(s.loop4DstStride) * CUBE_L1_BLOCK_BYTES});
        return Build(std::move(d), ACLSAN_DEVICE_MEMORY_SPACE_L1);
    }

    MemoryCbdataResult operator()(const FixL0cToOutParamField& f) const noexcept
    {
        return FixRead(f, TWO_ACCESS_COUNT * CUBE_L1_BLOCK_BYTES);
    }

    MemoryCbdataResult operator()(const LocalMemoryTransferParamField& f) const noexcept
    {
        if (f.kind == LocalMemoryTransferKind::CopyCbufToFbuf) {
            if (f.dstMemBlock >= AIV_BLOCKS_PER_AIC ||
                (f.dstMemBlock == NO_ACCESS_COUNT &&
                 (f.lenBurst % AIV_BLOCKS_PER_AIC != NO_ACCESS_COUNT || f.dstStride % AIV_BLOCKS_PER_AIC != 0))) {
                return {MemoryCbdataStatus::INVALID_FIELD, {}};
            }
            // MOV_L1_TO_FB uses 64-byte lengths and absolute 32-byte source strides.
            return Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.nBurst,
                uint64_t(f.lenBurst) * (TWO_ACCESS_COUNT * CUBE_L1_BLOCK_BYTES),
                uint64_t(f.srcStride) * CUBE_L1_BLOCK_BYTES);
        }
        if (f.kind == LocalMemoryTransferKind::CopyCbufToBt) {
            const bool source32 = f.instrId == uint32_t(InstructionId::CopyCbufToBtF32) ||
                                  f.instrId == uint32_t(InstructionId::CopyCbufToBtS32);
            if ((source32 && (f.lenBurst % AIV_BLOCKS_PER_AIC != NO_ACCESS_COUNT || f.cvtEnable)) ||
                f.dstGap % AIV_BLOCKS_PER_AIC != 0) {
                return {MemoryCbdataStatus::INVALID_FIELD, {}};
            }
            // BT conversion changes the destination footprint, not the L1 read footprint.
            return Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.nBurst,
                uint64_t(f.lenBurst) * CUBE_L1_BLOCK_BYTES, (uint64_t(f.lenBurst) + f.srcGap) * CUBE_L1_BLOCK_BYTES);
        }
        if (f.kind == LocalMemoryTransferKind::CopyUbufToCbuf || f.kind == LocalMemoryTransferKind::CopyCbufToUbuf) {
            const bool write = f.kind == LocalMemoryTransferKind::CopyUbufToCbuf;
            const uint64_t count = (f.config0 >> FIXPIPE_CONFIG_COUNT_SHIFT) & FIXPIPE_CONFIG_COUNT_MASK;
            const uint64_t length = (f.config0 >> FIXPIPE_CONFIG_LENGTH_SHIFT) & FIXPIPE_CONFIG_FIELD_MASK;
            const uint64_t gap =
                (f.config0 >> (write ? FIXPIPE_CONFIG_GAP_WRITE_SHIFT : FIXPIPE_CONFIG_GAP_READ_SHIFT)) &
                FIXPIPE_CONFIG_FIELD_MASK;
            if (count == NO_ACCESS_COUNT || length == NO_ACCESS_COUNT) {
                return {MemoryCbdataStatus::NO_ACCESS, {}};
            }
            const auto l1Context = ContextForL1(context_);
            if (!l1Context.has_value()) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            return LinearInContext(
                *l1Context, write ? f.dstAddr : f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1,
                write ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ, count,
                length * CUBE_L1_BLOCK_BYTES, (length + gap) * CUBE_L1_BLOCK_BYTES);
        }
        // FIX's common fields have the same encoding. Decode through the existing
        // architecture decoder; bits 16..19 describe local destinations, not L2.
        AclsanRawTraceRecord raw{};
        raw.instrId = (f.instrId == static_cast<uint32_t>(InstructionId::FixL0cToCbufF32) ||
                       f.instrId == static_cast<uint32_t>(InstructionId::FixL0cToUbufF32)) ?
                          static_cast<uint32_t>(InstructionId::FixL0cToOutF32) :
                          static_cast<uint32_t>(InstructionId::FixL0cToOutS32);
        raw.args[0] = f.dstAddr;
        raw.args[1] = f.srcAddr;
        raw.args[2] = f.config0;
        raw.args[3] = f.config1 & ~(uint64_t(QUANT_PRE_FIELD_MASK) << FIXPIPE_CONFIG_LENGTH_SHIFT);
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
        if (!decoded.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const auto& fix = std::get<FixL0cToOutParamField>(decoded->params);
        // L0C stores 32-bit accumulators: an N=16 row occupies 64 bytes,
        // independent of the destination type or destination memory space.
        auto read = FixRead(fix, TWO_ACCESS_COUNT * CUBE_L1_BLOCK_BYTES);
        if (read.status != MemoryCbdataStatus::SUCCESS ||
            f.instrId == static_cast<uint32_t>(InstructionId::FixL0cToUbufF32) ||
            f.instrId == static_cast<uint32_t>(InstructionId::FixL0cToUbufS32)) {
            return read;
        }
        if ((f.config1 & (uint64_t(QUANT_PRE_FIELD_MASK) << FIXPIPE_CONFIG_LENGTH_SHIFT)) != NO_ACCESS_COUNT ||
            fix.splitEnable) {
            read.status = MemoryCbdataStatus::PARTIAL_COVERAGE;
            return read;
        }
        auto write = MemoryFieldVisitor(context_, state_)(fix);
        if (write.status != MemoryCbdataStatus::SUCCESS) {
            read.status = MemoryCbdataStatus::PARTIAL_COVERAGE;
            return read;
        }
        for (auto& access : write.data) {
            access.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_L1;
        }
        return Merge(std::move(read), std::move(write));
    }

    static MemoryCbdataResult Merge(MemoryCbdataResult first, MemoryCbdataResult second) noexcept
    {
        if (first.data.size() + second.data.size() > MAX_EXPANDED_MEMORY_ACCESSES_PER_INSTRUCTION) {
            return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
        }
        try {
            first.data.insert(first.data.end(), second.data.begin(), second.data.end());
        } catch (const std::bad_alloc&) {
            return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
        } catch (const std::length_error&) {
            return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
        }
        const auto ok = [](MemoryCbdataStatus s) {
            return s == MemoryCbdataStatus::SUCCESS || s == MemoryCbdataStatus::NO_ACCESS;
        };
        // 缺失寄存器配置保留具体原因；其他未完整转换的组合统一为部分覆盖。
        if (first.status == MemoryCbdataStatus::MISSING_REGISTER_STATE ||
            second.status == MemoryCbdataStatus::MISSING_REGISTER_STATE) {
            if (first.status != MemoryCbdataStatus::MISSING_REGISTER_STATE) {
                first.requiredRegisterInstructionId = second.requiredRegisterInstructionId;
            }
            first.status = MemoryCbdataStatus::MISSING_REGISTER_STATE;
        } else if (!ok(first.status) || !ok(second.status)) {
            first.status = MemoryCbdataStatus::PARTIAL_COVERAGE;
            if (second.requiredRegisterInstructionId != 0) {
                first.requiredRegisterInstructionId = second.requiredRegisterInstructionId;
            }
        } else {
            first.status = first.data.empty() ? MemoryCbdataStatus::NO_ACCESS : MemoryCbdataStatus::SUCCESS;
        }
        return first;
    }

private:
    static uint64_t CeilDiv(uint64_t n, uint64_t d) noexcept
    {
        return n / d + (n % d != 0U ? uint64_t{1} : uint64_t{0});
    }

    static bool Add(
        MemoryCbdataBuilder& builder, uint64_t address, uint32_t space, uint32_t mode, uint64_t count, uint64_t bytes,
        uint64_t stride, uint32_t bits = 0) noexcept
    {
        auto d = MemoryAccessDescriptorFactory::Linear(address, mode, bits, count, bytes, stride);
        d.memorySpace = space;
        return builder.Add(std::move(d));
    }

    MemoryCbdataResult BuildInContext(
        MemoryCbdataContext context, MemoryAccessDescriptor d, uint32_t space) const noexcept
    {
        d.memorySpace = space;
        const auto source = context.pipeline == ACLSAN_DEVICE_PIPE_FIXPIPE ? ACLSAN_DEVICE_SOURCE_FIXPIPE :
                            context.pipeline == ACLSAN_DEVICE_PIPE_MTE3    ? ACLSAN_DEVICE_SOURCE_MTE3 :
                            context.pipeline == ACLSAN_DEVICE_PIPE_MTE2    ? ACLSAN_DEVICE_SOURCE_MTE2 :
                                                                             ACLSAN_DEVICE_SOURCE_CUBE;
        MemoryCbdataBuilder builder(context, {d.dataBits, source, BLOCK_TYPE_AIC});
        if (!builder.Add(std::move(d))) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult Build(MemoryAccessDescriptor d, uint32_t space) const noexcept
    {
        return BuildInContext(context_, std::move(d), space);
    }

    MemoryCbdataResult Linear(
        uint64_t address, uint32_t space, uint32_t mode, uint64_t count, uint64_t bytes, uint64_t stride,
        uint32_t bits = 0) const noexcept
    {
        if (count == NO_ACCESS_COUNT || bytes == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        return Build(MemoryAccessDescriptorFactory::Linear(address, mode, bits, count, bytes, stride), space);
    }

    MemoryCbdataResult LinearInContext(
        MemoryCbdataContext context, uint64_t address, uint32_t space, uint32_t mode, uint64_t count, uint64_t bytes,
        uint64_t stride, uint32_t bits = 0) const noexcept
    {
        if (count == NO_ACCESS_COUNT || bytes == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        return BuildInContext(
            context, MemoryAccessDescriptorFactory::Linear(address, mode, bits, count, bytes, stride), space);
    }

    MemoryCbdataResult DmaWrite(
        uint64_t address, uint64_t count, uint64_t bytes, uint64_t stride, uint32_t bits) const noexcept
    {
        if (count == NO_ACCESS_COUNT || bytes == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const size_t direction = static_cast<size_t>(DmaLoopDirection::GM_TO_CBUF);
        const auto& size = state_.dmaLoopSizes[direction];
        const std::array<uint64_t, DMA_DESCRIPTOR_DIMENSION_COUNT> counts{
            count, size.has_value() ? size->loop1Size : 1U, size.has_value() ? size->loop2Size : 1U};
        if (counts[1] == NO_ACCESS_COUNT || counts[2] == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        std::array<uint64_t, DMA_DESCRIPTOR_DIMENSION_COUNT> strides{stride, 0, 0};
        for (size_t i = 0; i < LOOP_DIMENSION_COUNT; ++i) {
            if (counts[i + 1] > SINGLE_ACCESS_COUNT) {
                if (!state_.dmaLoopStrides[direction][i].has_value()) {
                    return {
                        MemoryCbdataStatus::MISSING_REGISTER_STATE,
                        {},
                        uint64_t(InstructionId::Loop1StrideGmToCbuf) + i};
                }
                strides[i + 1] = state_.dmaLoopStrides[direction][i]->dstStride;
            }
        }
        if (counts[1] == SINGLE_ACCESS_COUNT && counts[2] == SINGLE_ACCESS_COUNT) {
            return Linear(
                address, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, count, bytes, stride, bits);
        }
        return Build(
            MemoryAccessDescriptorFactory::DmaAffine(
                address, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, bits, bytes, counts, strides),
            ACLSAN_DEVICE_MEMORY_SPACE_L1);
    }

    MemoryCbdataResult FixRead(const FixL0cToOutParamField& f, uint64_t rowBytes) const noexcept
    {
        if (f.nSize == NO_ACCESS_COUNT || f.mSize == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if ((f.nz2ndEnable || f.nz2dnEnable) && state_.loop3.has_value() &&
            state_.loop3->loopCount == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        // NZ2DN additionally needs CHANNEL_PARA, which is not captured yet.
        if (f.nz2dnEnable || f.quantPost != 0U || f.reluPost != 0U || f.clipReluPost || f.loopEnhanceEnable ||
            f.loopEnhanceMergeEnable || f.eltwiseOp != 0U || f.eltwiseAntqEnable || f.winoPostEnable || f.brcbEnable ||
            f.c0PadEnable) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        if (!f.nz2ndEnable) {
            return Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ,
                CeilDiv(f.nSize, CUBE_N_ALIGNMENT), uint64_t(f.mSize) * rowBytes, uint64_t(f.loopSrtStride) * rowBytes,
                CUBE_L1_BLOCK_BYTES);
        }
        if (!state_.loop3.has_value()) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, uint64_t(InstructionId::Loop3Param)};
        }
        if (state_.loop3->loopCount == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const uint64_t fullRows = static_cast<uint64_t>(f.nSize) / CUBE_N_ALIGNMENT;
        const uint64_t tailRows = static_cast<uint64_t>(f.nSize) % CUBE_N_ALIGNMENT;
        MemoryCbdataBuilder builder(context_, {CUBE_L1_BLOCK_BYTES, ACLSAN_DEVICE_SOURCE_FIXPIPE, BLOCK_TYPE_AIC});
        for (uint64_t matrix = 0; matrix < state_.loop3->loopCount; ++matrix) {
            const uint64_t offset = matrix * state_.loop3->srcStride * rowBytes;
            if (f.srcAddr > UINT64_MAX - offset) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            const uint64_t base = f.srcAddr + offset;
            if (fullRows != 0U &&
                !Add(
                    builder, base, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, fullRows,
                    uint64_t(f.mSize) * rowBytes, uint64_t(f.loopSrtStride) * rowBytes, CUBE_L1_BLOCK_BYTES)) {
                return std::move(builder).Build();
            }
            if (tailRows != 0U) {
                const uint64_t tailOffset = fullRows * f.loopSrtStride * rowBytes;
                if (base > UINT64_MAX - tailOffset) {
                    return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
                }
                if (!Add(
                        builder, base + tailOffset, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ,
                        f.mSize, tailRows * FOUR_BIT_DATA, CUBE_ROW_BYTES, CUBE_L1_BLOCK_BYTES)) {
                    return std::move(builder).Build();
                }
            }
        }
        return std::move(builder).Build();
    }

    MemoryCbdataContext context_;
    const MemoryRegisterState& state_;
};

// UB endpoints of local-memory transfers. Cross-core L1/L0C transfers use the
// physical AIC/AIV topology recorded with the trace launch.
class UbMemoryFieldVisitor final {
public:
    UbMemoryFieldVisitor(MemoryCbdataContext context, const MemoryRegisterState& state) noexcept
        : context_(context), state_(state)
    {}

    template <typename Field>
    MemoryCbdataResult operator()(const Field&) const noexcept
    {
        return {MemoryCbdataStatus::NO_ACCESS, {}};
    }

    MemoryCbdataResult operator()(const CopyGmToUbufAlignV2ParamField& f) const noexcept
    {
        if (f.burstNum == NO_ACCESS_COUNT || f.burstLen == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const auto profile = MemoryInstructionProfileFactory::Create(f);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const uint64_t padding = uint64_t(f.leftPaddingCount + f.rightPaddingCount) * f.dataBits / BITS_PER_BYTE;
        const bool compact = f.burstDstStride == f.burstLen && padding == 0;
        if (!compact && f.burstNum > SINGLE_ACCESS_COUNT && f.burstDstStride % CUBE_L1_BLOCK_BYTES != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        // Compact copies pad the complete burst group once; normal copies pad
        // every burst, including explicitly requested left/right padding.
        const uint64_t bytes = compact ? uint64_t(f.burstLen) * f.burstNum : uint64_t(f.burstLen) + padding;
        return Dma(
            *profile, DmaLoopDirection::GM_TO_UBUF, f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
            compact ? SINGLE_ACCESS_COUNT : f.burstNum,
            (bytes + CUBE_L1_BLOCK_BYTES - SINGLE_ACCESS_COUNT) / CUBE_L1_BLOCK_BYTES * CUBE_L1_BLOCK_BYTES,
            f.burstDstStride);
    }

    MemoryCbdataResult operator()(const CopyUbufToGmAlignV2ParamField& f) const noexcept
    {
        if (f.burstNum == NO_ACCESS_COUNT || f.burstLen == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const auto profile = MemoryInstructionProfileFactory::Create(f);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const bool compact = f.srcStride == f.burstLen;
        if (!compact && f.burstNum > SINGLE_ACCESS_COUNT && f.srcStride % CUBE_L1_BLOCK_BYTES != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        // The hardware reads the UB alignment tail, then discards it before
        // writing GM. Compact mode rounds the complete burst group once.
        const uint64_t bytes = compact ? uint64_t(f.burstLen) * f.burstNum : f.burstLen;
        return Dma(
            *profile, DmaLoopDirection::UBUF_TO_GM, f.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ,
            compact ? SINGLE_ACCESS_COUNT : f.burstNum,
            (bytes + CUBE_L1_BLOCK_BYTES - SINGLE_ACCESS_COUNT) / CUBE_L1_BLOCK_BYTES * CUBE_L1_BLOCK_BYTES,
            f.srcStride);
    }

    MemoryCbdataResult operator()(const NdDmaOutToUbufParamField& f) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(f);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        NdAffineDescriptor layout{
            SUPPORTED_NDDMA_DIMENSION_COUNT,
            f.dataBits / BITS_PER_BYTE,
            {f.loop0Size, f.loop1Size, f.loop2Size, f.loop3Size, f.loop4Size},
            {}};
        if (std::any_of(layout.counts.begin(), layout.counts.end(), [](uint64_t n) { return n == NO_ACCESS_COUNT; })) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (!state_.ndDmaPadCount.has_value()) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, uint64_t(InstructionId::NdDmaPadCount)};
        }
        // paddingMode selects constant/nearest padding values, not whether
        // padding is enabled. Both modes write the same padded destination set.
        layout.counts[0] += uint64_t(f.loop0LeftPaddingCount) + f.loop0RightPaddingCount;
        for (size_t i = 1; i < layout.counts.size(); ++i) {
            layout.counts[i] += uint64_t(state_.ndDmaPadCount->leftPaddingCounts[i - 1]) +
                                state_.ndDmaPadCount->rightPaddingCounts[i - 1];
        }
        for (size_t i = 0; i < layout.counts.size(); ++i) {
            if (layout.counts[i] <= SINGLE_ACCESS_COUNT) {
                continue;
            }
            if (!state_.ndDmaLoopStrides[i].has_value()) {
                return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, uint64_t(InstructionId::NdDmaLoop0Stride) + i};
            }
            layout.strideBytes[i] = uint64_t(state_.ndDmaLoopStrides[i]->dstStride) * layout.bytes;
        }
        return Build({f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.dataBits, layout}, *profile);
    }

    MemoryCbdataResult operator()(const CopyUbufToUbufParamField& f) const noexcept
    {
        if (f.instrId != uint32_t(InstructionId::CopyUbufToUbuf)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.burstNum == NO_ACCESS_COUNT || f.burstLen == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const MemoryInstructionProfile profile{UNSPECIFIED_DATA_BITS, ACLSAN_DEVICE_SOURCE_VECTOR, BLOCK_TYPE_AIV};
        auto read = Build(
            MemoryAccessDescriptorFactory::Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, f.burstNum, uint64_t(f.burstLen) * CUBE_L1_BLOCK_BYTES,
                (uint64_t(f.burstLen) + f.srcGap) * CUBE_L1_BLOCK_BYTES),
            profile);
        auto write = Build(
            MemoryAccessDescriptorFactory::Linear(
                f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, f.burstNum, uint64_t(f.burstLen) * CUBE_L1_BLOCK_BYTES,
                (uint64_t(f.burstLen) + f.dstGap) * CUBE_L1_BLOCK_BYTES),
            profile);
        return CubeMemoryFieldVisitor::Merge(std::move(read), std::move(write));
    }

    MemoryCbdataResult operator()(const LocalMemoryTransferParamField& f) const noexcept
    {
        if (f.kind == LocalMemoryTransferKind::CopyCbufToUbuf || f.kind == LocalMemoryTransferKind::CopyUbufToCbuf) {
            const bool write = f.kind == LocalMemoryTransferKind::CopyCbufToUbuf;
            const uint64_t count = (f.config0 >> 4U) & 0xfffU;
            const uint64_t length = (f.config0 >> 16U) & 0xffffU;
            const uint64_t gap = (f.config0 >> (write ? 48U : 32U)) & 0xffffU;
            if (length == NO_ACCESS_COUNT || count == NO_ACCESS_COUNT) {
                return {MemoryCbdataStatus::NO_ACCESS, {}};
            }
            const auto ubContext = ContextForUbuf(context_, write && f.subBlockId ? 1U : 0U);
            if (!ubContext.has_value()) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            return LinearInContext(
                *ubContext, write ? f.dstAddr : f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_UB,
                write ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ, count,
                length * CUBE_L1_BLOCK_BYTES, (length + gap) * CUBE_L1_BLOCK_BYTES);
        }
        if (f.kind != LocalMemoryTransferKind::FixL0cToUbufF32 && f.kind != LocalMemoryTransferKind::FixL0cToUbufS32) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }

        AclsanRawTraceRecord raw{};
        raw.instrId = f.kind == LocalMemoryTransferKind::FixL0cToUbufF32 ?
                          static_cast<uint32_t>(InstructionId::FixL0cToOutF32) :
                          static_cast<uint32_t>(InstructionId::FixL0cToOutS32);
        raw.args[0] = f.dstAddr;
        raw.args[1] = f.srcAddr;
        raw.args[2] = f.config0;
        raw.args[3] = f.config1 & ~(uint64_t(QUANT_PRE_FIELD_MASK) << FIXPIPE_CONFIG_LENGTH_SHIFT);
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
        if (!decoded.has_value() || !std::holds_alternative<FixL0cToOutParamField>(decoded->params)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const auto& fix = std::get<FixL0cToOutParamField>(decoded->params);
        if (fix.nSize == NO_ACCESS_COUNT || fix.mSize == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (f.dualDstCtl == DUAL_DST_MODE_BOTH) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.dualDstCtl == DUAL_DST_MODE_FIRST && (fix.mSize == NO_ACCESS_COUNT || fix.mSize % 2U != 0)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.dualDstCtl == DUAL_DST_MODE_SECOND &&
            fix.nSize % (fix.nz2ndEnable ? TWO_ACCESS_COUNT : CUBE_L1_BLOCK_BYTES) != 0) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.dualDstCtl != DUAL_DST_MODE_NONE &&
            (fix.quantPre != 0U || fix.reluPre != 0U || fix.clipReluPre != 0U || fix.splitEnable || fix.nz2dnEnable)) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }

        const auto emit = [this](uint32_t subBlockId, FixL0cToOutParamField target) noexcept {
            const auto ubContext = ContextForUbuf(context_, subBlockId);
            if (!ubContext.has_value()) {
                return MemoryCbdataResult{MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            return MemoryFieldVisitor(*ubContext, state_, ACLSAN_DEVICE_MEMORY_SPACE_UB)(target);
        };

        if (f.dualDstCtl == DUAL_DST_MODE_NONE) {
            return emit(f.subBlockId ? 1U : 0U, fix);
        }
        FixL0cToOutParamField first = fix;
        FixL0cToOutParamField second = fix;
        if (f.dualDstCtl == DUAL_DST_MODE_FIRST) {
            first.mSize = static_cast<uint16_t>(fix.mSize / TWO_ACCESS_COUNT);
            second.mSize = first.mSize;
        } else {
            first.nSize = static_cast<uint16_t>(fix.nSize / TWO_ACCESS_COUNT);
            second.nSize = first.nSize;
        }
        return CubeMemoryFieldVisitor::Merge(emit(0, first), emit(1, second));
    }

private:
    MemoryCbdataResult Build(MemoryAccessDescriptor descriptor, MemoryInstructionProfile profile) const noexcept
    {
        descriptor.memorySpace = ACLSAN_DEVICE_MEMORY_SPACE_UB;
        MemoryCbdataBuilder builder(context_, profile);
        (void)builder.Add(std::move(descriptor));
        return std::move(builder).Build();
    }

    MemoryCbdataResult LinearInContext(
        MemoryCbdataContext context, uint64_t address, uint32_t space, uint32_t mode, uint64_t count, uint64_t bytes,
        uint64_t stride, uint32_t bits = 0) const noexcept
    {
        if (count == NO_ACCESS_COUNT || bytes == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        auto descriptor = MemoryAccessDescriptorFactory::Linear(address, mode, bits, count, bytes, stride);
        descriptor.memorySpace = space;
        const auto source =
            context.pipeline == ACLSAN_DEVICE_PIPE_MTE3 ? ACLSAN_DEVICE_SOURCE_MTE3 : ACLSAN_DEVICE_SOURCE_CUBE;
        MemoryCbdataBuilder builder(context, {bits, source, BLOCK_TYPE_AIV});
        if (!builder.Add(std::move(descriptor))) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult Dma(
        MemoryInstructionProfile profile, DmaLoopDirection direction, uint64_t address, uint32_t mode, uint64_t bursts,
        uint64_t bytes, uint64_t stride) const noexcept
    {
        const size_t d = static_cast<size_t>(direction);
        if (!state_.dmaLoopSizes[d].has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE,
                {},
                uint64_t(InstructionId::LoopSizeUbufToGm) + d * SUPPORTED_DMA_DIRECTION_COUNT};
        }
        const auto& size = *state_.dmaLoopSizes[d];
        const std::array<uint64_t, DMA_DESCRIPTOR_DIMENSION_COUNT> counts{bursts, size.loop1Size, size.loop2Size};
        if (counts[1] == NO_ACCESS_COUNT || counts[2] == NO_ACCESS_COUNT) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        std::array<uint64_t, DMA_DESCRIPTOR_DIMENSION_COUNT> strides{stride, 0, 0};
        for (size_t i = 0; i < LOOP_DIMENSION_COUNT; ++i) {
            if (counts[i + 1] <= SINGLE_ACCESS_COUNT) {
                continue;
            }
            if (!state_.dmaLoopStrides[d][i].has_value()) {
                return {
                    MemoryCbdataStatus::MISSING_REGISTER_STATE,
                    {},
                    uint64_t(InstructionId::Loop1StrideUbufToGm) + d * SUPPORTED_DMA_DIRECTION_COUNT + i};
            }
            const auto& s = *state_.dmaLoopStrides[d][i];
            strides[i + 1] = direction == DmaLoopDirection::UBUF_TO_GM ? s.srcStride : s.dstStride;
        }
        return Build(
            counts[1] == SINGLE_ACCESS_COUNT && counts[2] == SINGLE_ACCESS_COUNT ?
                MemoryAccessDescriptorFactory::Linear(address, mode, profile.dataBits, bursts, bytes, stride) :
                MemoryAccessDescriptorFactory::DmaAffine(address, mode, profile.dataBits, bytes, counts, strides),
            profile);
    }

    MemoryCbdataContext context_;
    const MemoryRegisterState& state_;
};

MemoryCbdataResult FinalizeMemoryAccesses(MemoryCbdataResult result) noexcept
{
    struct Sequence {
        uint32_t count = 0;
        uint32_t nextIndex = 0;
    };

    // A local transfer may publish endpoints owned by different physical cores.
    // This result covers one dynamic instruction, so phyCoreId identifies each
    // owner. Finalize once after all merges, keeping an ordered sequence per core.
    try {
        std::unordered_map<uint32_t, Sequence> sequences;
        for (const auto& data : result.data) {
            ++sequences[data.header.phyCoreId].count;
        }
        for (auto& data : result.data) {
            auto& sequence = sequences.at(data.header.phyCoreId);
            data.accessIndex = sequence.nextIndex++;
            data.accessCount = sequence.count;
        }
    } catch (const std::bad_alloc&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
    } catch (const std::length_error&) {
        return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
    }
    return result;
}

void ApplyAtomicConfiguration(MemoryCbdataResult& output, const MemoryRegisterState& state) noexcept
{
    const bool hasGmWrite = std::any_of(output.data.begin(), output.data.end(), [](const auto& access) {
        return access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM &&
               access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    });
    if (!hasGmWrite) {
        return;
    }
    if (!state.ctrl.has_value()) {
        // 未配置 CTRL 时按普通写检测，保留已还原的精确访存范围。
        // 寄存器依赖仍由翻译层提供给 Initcheck。
        return;
    }
    // CTRL[8:6] 选择原子类型；类型为 0 时禁用原子操作，忽略保留的操作位。
    const uint64_t type = (state.ctrl->value >> ATOMIC_TYPE_SHIFT) & ATOMIC_TYPE_MASK;
    if (type == NO_ACCESS_COUNT) {
        return;
    }
    const uint64_t operation = (state.ctrl->value >> ATOMIC_OPERATION_SHIFT) & ATOMIC_OPERATION_MASK;
    constexpr uint32_t typeBits[] = {0, 32, 16, 16, 32, 8, 16, 0};
    constexpr AclsanDeviceMemoryAccessMode modes[] = {
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX,
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN};
    const bool widthMismatch =
        std::any_of(output.data.begin(), output.data.end(), [type, &typeBits](const auto& access) {
            return access.dataBits != NO_ACCESS_COUNT && access.dataBits != typeBits[type];
        });
    if (operation >= std::size(modes) || typeBits[type] == NO_ACCESS_COUNT || widthMismatch) {
        output.data.clear();
        output.status = MemoryCbdataStatus::PARTIAL_COVERAGE;
        return;
    }
    for (auto& access : output.data) {
        access.accessMode = modes[operation];
        access.dataBits = typeBits[type];
    }
}

} // namespace

MemoryFieldToCbdataConverter::MemoryFieldToCbdataConverter(
    MemoryCbdataContext context, MemoryRegisterState registerState) noexcept
    : context_(context), registerState_(std::move(registerState))
{}

MemoryCbdataResult MemoryFieldToCbdataConverter::Convert(const MemoryInstructionField& field) const noexcept
{
    auto gm = std::visit(MemoryFieldVisitor{context_, registerState_}, field);
    const bool gmOutput = std::holds_alternative<CopyUbufToGmAlignV2ParamField>(field) ||
                          std::holds_alternative<FixL0cToOutParamField>(field);
    const bool localTransfer = std::holds_alternative<LocalMemoryTransferParamField>(field);
    if (gm.status == MemoryCbdataStatus::SUCCESS || gm.status == MemoryCbdataStatus::NO_ACCESS) {
        if (gmOutput) {
            ApplyAtomicConfiguration(gm, registerState_);
        }
        auto local = std::visit(CubeMemoryFieldVisitor{context_, registerState_}, field);
        if (local.status != MemoryCbdataStatus::NO_ACCESS) {
            gm = CubeMemoryFieldVisitor::Merge(std::move(gm), std::move(local));
        }
    }
    // A GM address overflow does not invalidate an independently known UB
    // endpoint. Invalid instruction fields still block both projections.
    if (gm.status == MemoryCbdataStatus::SUCCESS || gm.status == MemoryCbdataStatus::NO_ACCESS ||
        gm.status == MemoryCbdataStatus::ARITHMETIC_OVERFLOW ||
        (gmOutput && gm.status == MemoryCbdataStatus::PARTIAL_COVERAGE) ||
        (localTransfer && gm.status == MemoryCbdataStatus::PARTIAL_COVERAGE)) {
        auto ub = std::visit(UbMemoryFieldVisitor{context_, registerState_}, field);
        if (ub.status != MemoryCbdataStatus::NO_ACCESS) {
            const bool incomplete = gm.status == MemoryCbdataStatus::ARITHMETIC_OVERFLOW ||
                                    gm.status == MemoryCbdataStatus::PARTIAL_COVERAGE ||
                                    ub.status != MemoryCbdataStatus::SUCCESS;
            gm = CubeMemoryFieldVisitor::Merge(std::move(gm), std::move(ub));
            gm.requiresIncompleteEvent = incomplete;
        }
    }
    const bool atomicConfiguration = gmOutput || std::holds_alternative<ScalarAtomicParamField>(field);
    gm.requiresIncompleteEvent = gm.requiresIncompleteEvent ||
                                 (atomicConfiguration && (gm.status == MemoryCbdataStatus::MISSING_REGISTER_STATE ||
                                                          gm.status == MemoryCbdataStatus::PARTIAL_COVERAGE));
    return FinalizeMemoryAccesses(std::move(gm));
}

} // namespace aclsan
