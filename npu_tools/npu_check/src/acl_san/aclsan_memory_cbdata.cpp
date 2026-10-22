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
#include <utility>
#include <variant>

namespace aclsan {
namespace {

constexpr uint32_t kBlockTypeAic = 0;
constexpr uint32_t kBlockTypeAiv = 1;
constexpr std::size_t kMemoryNdMaxRank = 5;
constexpr std::size_t kMaxExpandedMemoryAccessesPerInstruction = 4096;

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
    std::array<uint64_t, kMemoryNdMaxRank> counts;
    std::array<uint64_t, kMemoryNdMaxRank> strideBytes;
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
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, kBlockTypeAiv};
    }

    static std::optional<MemoryInstructionProfile> Create(const CopyGmToCbufAlignV2ParamField& field) noexcept
    {
        const uint32_t dataBits = field.dataBits;
        if (!IsSupportedReadDataBits(dataBits)) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, kBlockTypeAic};
    }

    template <NdNzConversionMode ConversionMode>
    static std::optional<MemoryInstructionProfile> Create(
        const CopyGmToCbufMultiParamField<ConversionMode>& field) noexcept
    {
        const uint32_t dataBits = field.dataBits;
        if (!IsSupportedReadDataBits(dataBits)) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, kBlockTypeAic};
    }

    static std::optional<MemoryInstructionProfile> Create(const CopyGmToCbufV2ParamField& field) noexcept
    {
        if (static_cast<InstructionId>(field.instrId) != InstructionId::CopyGmToCbufV2) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{0, ACLSAN_DEVICE_SOURCE_MTE2, kBlockTypeAic};
    }

    static std::optional<MemoryInstructionProfile> Create(const CopyUbufToGmAlignV2ParamField& field) noexcept
    {
        if (static_cast<InstructionId>(field.instrId) != InstructionId::CopyUbufToGmAlignV2) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{0, ACLSAN_DEVICE_SOURCE_MTE3, kBlockTypeAiv};
    }

    static std::optional<MemoryInstructionProfile> Create(const FixL0cToOutParamField& field) noexcept
    {
        const auto id = static_cast<InstructionId>(field.instrId);
        const uint32_t dataBits = FixpipeDestinationDataBits(id, field.quantPre);
        if (dataBits == 0) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_FIXPIPE, kBlockTypeAic};
    }

    static std::optional<MemoryInstructionProfile> Create(const LoadGmToCbuf2DV2ParamField& field) noexcept
    {
        if (static_cast<InstructionId>(field.instrId) != InstructionId::LoadGmToCbuf2DV2) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{0, ACLSAN_DEVICE_SOURCE_MTE2, kBlockTypeAic};
    }

    static std::optional<MemoryInstructionProfile> Create(const NdDmaOutToUbufParamField& field) noexcept
    {
        const uint32_t dataBits = field.dataBits;
        if (!IsSupportedReadDataBits(dataBits)) {
            return std::nullopt;
        }
        return MemoryInstructionProfile{dataBits, ACLSAN_DEVICE_SOURCE_MTE2, kBlockTypeAiv};
    }

private:
    static uint32_t FixpipeDestinationDataBits(InstructionId id, uint8_t quantPre) noexcept
    {
        if (id == InstructionId::FixL0cToOutF32) {
            switch (quantPre) {
                case 0:
                case 14:
                case 15:
                    return 32;
                case 1:
                case 16:
                case 31:
                case 32:
                case 33:
                case 34:
                    return 16;
                case 2:
                case 3:
                case 4:
                case 5:
                case 12:
                case 13:
                case 23:
                case 24:
                    return 8;
                case 25:
                case 26:
                    return 4;
                default:
                    return 0;
            }
        }
        if (id != InstructionId::FixL0cToOutS32) {
            return 0;
        }
        switch (quantPre) {
            case 0:
                return 32;
            case 10:
            case 11:
            case 35:
            case 36:
                return 16;
            case 8:
            case 9:
                return 8;
            case 21:
            case 22:
                return 4;
            default:
                return 0;
        }
    }

    static bool IsSupportedReadDataBits(uint32_t dataBits) noexcept
    {
        return dataBits == 8 || dataBits == 16 || dataBits == 32;
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
    result.blockId /= 2U;
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
            (strideBytes == 0 || count - 1 <= (std::numeric_limits<uint64_t>::max() - bytes) / strideBytes)) {
            rangeBytes += (count - 1) * strideBytes;
            isRange = true;
        }
        if (isRange) {
            return {address, accessMode, dataBits, RangeDescriptor{rangeBytes}};
        }
        return {address, accessMode, dataBits, StridedDescriptor{bytes, count, strideBytes}};
    }

    static MemoryAccessDescriptor NdRead(
        uint64_t address, uint32_t dataBits, uint64_t bytes, std::array<uint64_t, 2> counts,
        std::array<uint64_t, 2> strides) noexcept
    {
        struct Axis {
            uint64_t count;
            uint64_t stride;
        };
        std::array<Axis, 2> axes{};
        std::size_t axisCount = 0;
        for (std::size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] > 1 && strides[index] != 0) {
                axes[axisCount++] = {counts[index], strides[index]};
            }
        }
        std::sort(axes.begin(), axes.begin() + axisCount, [](const Axis& left, const Axis& right) {
            return left.stride < right.stride;
        });

        std::array<Axis, 2> sparseAxes{};
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
        if (sparseCount == 0) {
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
        layout.rank = 2;
        layout.bytes = segmentBytes;
        for (std::size_t index = 0; index < sparseCount; ++index) {
            layout.counts[index] = sparseAxes[index].count;
            layout.strideBytes[index] = sparseAxes[index].stride;
        }
        return {address, ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits, layout};
    }

    static MemoryAccessDescriptor NdRead(
        uint64_t address, uint32_t dataBits, uint64_t elementBytes, const std::array<uint64_t, 5>& counts,
        const std::array<uint64_t, 5>& strides) noexcept
    {
        return {
            address, ACLSAN_DEVICE_MEMORY_ACCESS_READ, dataBits, NdAffineDescriptor{5, elementBytes, counts, strides}};
    }

    static MemoryAccessDescriptor NdWrite(
        uint64_t address, uint32_t dataBits, uint64_t bytes, std::array<uint64_t, 2> counts,
        std::array<uint64_t, 2> strides) noexcept
    {
        MemoryAccessDescriptor descriptor = NdRead(address, dataBits, bytes, counts, strides);
        descriptor.accessMode = ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
        return descriptor;
    }

    static MemoryAccessDescriptor DmaAffine(
        uint64_t address, uint32_t accessMode, uint32_t dataBits, uint64_t bytes, const std::array<uint64_t, 3>& counts,
        const std::array<uint64_t, 3>& strides) noexcept
    {
        NdAffineDescriptor layout{};
        layout.rank = 3;
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
        const auto contains = [&](uint64_t base, uint64_t bytes) {
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
        if (descriptors_.size() >= kMaxExpandedMemoryAccessesPerInstruction) {
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
            const uint32_t accessCount = static_cast<uint32_t>(descriptors_.size());
            for (uint32_t index = 0; index < accessCount; ++index) {
                data.push_back(MakeCbdata(header, descriptors_[index], index, accessCount, context_.instructionId));
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
                if (layout.bytes == 0) {
                    return false;
                }
                lastOffset = layout.bytes - 1;
                if constexpr (std::is_same_v<Layout, RangeDescriptor>) {
                    return true;
                } else if constexpr (std::is_same_v<Layout, StridedDescriptor>) {
                    if (layout.bytes > std::numeric_limits<uint32_t>::max() || layout.count == 0 ||
                        layout.count > std::numeric_limits<uint32_t>::max() ||
                        layout.strideBytes > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                        return false;
                    }
                    lastOffset += static_cast<unsigned __int128>(layout.count - 1) * layout.strideBytes;
                    return true;
                } else {
                    if (layout.bytes > std::numeric_limits<uint32_t>::max() || layout.rank == 0 ||
                        layout.rank > kMemoryNdMaxRank) {
                        return false;
                    }
                    for (std::size_t index = 0; index < layout.rank; ++index) {
                        if (layout.counts[index] == 0 ||
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
        const AclsanDeviceEventHeader& header, const MemoryAccessDescriptor& descriptor, uint32_t accessIndex,
        uint32_t accessCount, uint32_t instructionId) noexcept
    {
        AclsanDeviceMemoryAccessData data{};
        data.header = header;
        if (descriptor.preloadTarget) {
            data.header.flags = ACLSAN_DEVICE_EVENT_FLAG_ESTIMATED;
        }
        data.address = descriptor.address;
        data.memorySpace = descriptor.memorySpace;
        data.accessMode = descriptor.accessMode;
        data.accessIndex = accessIndex;
        data.accessCount = accessCount;
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
    constexpr uint64_t windowMask = (UINT64_C(1) << 24) - 1;
    const bool systemWindow = ((effective >> 25) & windowMask) == ((field.sysVaBase >> 25) & windowMask);
    if (systemWindow && ((effective >> 24) & 1U) == 0) {
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
        constexpr uint32_t kWidthsPerFamily = 4;
        const uint32_t dataBits = 64U >> ((field.instrId - firstStore) % kWidthsPerFamily);
        if (field.dataBits != dataBits) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const bool isPair = (field.instrId >= static_cast<uint32_t>(InstructionId::StpB64) &&
                             field.instrId <= static_cast<uint32_t>(InstructionId::StpB8)) ||
                            (field.instrId >= static_cast<uint32_t>(InstructionId::LdpB64) &&
                             field.instrId <= static_cast<uint32_t>(InstructionId::LdpB8));
        if (isPair && field.post != 0U) {
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
            access.header.flags |= ACLSAN_DEVICE_EVENT_FLAG_ATOMIC_CONFIG_UNKNOWN;
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
        constexpr uint64_t kC0Bytes = 32;
        constexpr std::array<uint64_t, 5> kInsertedPaddingSourceBytes{1, 2, 4, 8, 16};
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.burstNum == 0 || field.burstLen == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (field.padFunctionMode > 8 ||
            (field.padFunctionMode >= 1 && field.padFunctionMode <= 5 && field.burstLen != 1)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const bool insertsPadding = field.padFunctionMode >= 1 && field.padFunctionMode <= 5;
        if (field.burstLen > std::numeric_limits<uint64_t>::max() / kC0Bytes ||
            (!insertsPadding && field.srcStride > std::numeric_limits<uint64_t>::max() / kC0Bytes)) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        const uint64_t burstBytes = insertsPadding ? kInsertedPaddingSourceBytes[field.padFunctionMode - 1] :
                                                     static_cast<uint64_t>(field.burstLen) * kC0Bytes;
        const uint64_t burstStride = insertsPadding ? burstBytes : field.srcStride * kC0Bytes;
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
        if (field.burstNum == 0 || field.burstLen == 0) {
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
        if (field.nSize == 0 || field.mSize == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if ((field.nz2ndEnable && field.nz2dnEnable) ||
            (field.splitEnable && (field.nz2ndEnable || field.nz2dnEnable))) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.splitEnable &&
            (field.instrId != static_cast<uint32_t>(InstructionId::FixL0cToOutF32) || profile->dataBits != 32)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (!field.nz2ndEnable && !field.nz2dnEnable &&
            ((field.splitEnable && field.nSize % 8 != 0) || (!field.splitEnable && field.nSize % 16 != 0))) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.c0PadEnable &&
            (field.instrId != static_cast<uint32_t>(InstructionId::FixL0cToOutF32) || field.sid != 0 ||
             field.nSize % 16 != 0 || field.mSize != 16 || field.loopDstStride != 256 || field.loopSrtStride != 16 ||
             field.l2CacheControl != 0 || field.clipReluPre != 0 || field.unitFlag != 0 || field.quantPre != 0 ||
             field.reluPre != 0 || field.splitEnable || field.nz2ndEnable || field.nz2dnEnable)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }

        MemoryCbdataBuilder builder(context_, *profile, fixOutputSpace_);
        if (profile->dataBits == 4) {
            if (!field.nz2ndEnable && !field.nz2dnEnable && field.nSize % 64 != 0) {
                return {MemoryCbdataStatus::INVALID_FIELD, {}};
            }
            return ConvertFixpipePacked4(field, std::move(builder));
        }

        const uint64_t dstElementBytes = profile->dataBits / 8U;
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
            if (loop3.loopCount == 0) {
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
                                        std::array<uint64_t, 2>{innerCount, loop3.loopCount},
                                        std::array<uint64_t, 2>{dstStrideBytes, matrixStrideBytes}));
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
        if (field.mStep == 0 || field.kStep == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (!registerState_.mte2Source.has_value()) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::Mte2SrcPara)};
        }
        constexpr uint64_t kFractalBytes = 512;
        const int64_t signedSrcStride = registerState_.mte2Source->srcStride;
        const __int128 strideMagnitude =
            signedSrcStride < 0 ? -static_cast<__int128>(signedSrcStride) : static_cast<__int128>(signedSrcStride);
        const __int128 repeatStrideBytes = strideMagnitude * kFractalBytes;
        if (repeatStrideBytes > std::numeric_limits<int64_t>::max()) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }

        const __int128 firstFractal =
            static_cast<__int128>(field.kStartPosition) * strideMagnitude + field.mStartPosition;
        const __int128 firstAddress = static_cast<__int128>(field.srcAddr) + firstFractal * kFractalBytes;
        const __int128 lowestAddress = signedSrcStride < 0 ?
                                           firstAddress - static_cast<__int128>(field.kStep - 1) * repeatStrideBytes :
                                           firstAddress;
        if (lowestAddress < 0 || lowestAddress > std::numeric_limits<uint64_t>::max()) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        MemoryCbdataBuilder builder(context_, *profile);
        return BuildSingleAccess(
            std::move(builder), MemoryAccessDescriptorFactory::NdRead(
                                    static_cast<uint64_t>(lowestAddress), profile->dataBits, kFractalBytes,
                                    std::array<uint64_t, 2>{field.mStep, field.kStep},
                                    std::array<uint64_t, 2>{kFractalBytes, static_cast<uint64_t>(repeatStrideBytes)}));
    }

    MemoryCbdataResult operator()(const NdDmaOutToUbufParamField& field) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const std::array<uint64_t, 5> counts{
            field.loop0Size, field.loop1Size, field.loop2Size, field.loop3Size, field.loop4Size};
        if (std::any_of(counts.begin(), counts.end(), [](uint64_t count) { return count == 0; })) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (field.paddingMode && !registerState_.ndDmaPadCount.has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::NdDmaPadCount)};
        }
        for (std::size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] > 1 && !registerState_.ndDmaLoopStrides[index].has_value()) {
                return {
                    MemoryCbdataStatus::MISSING_REGISTER_STATE,
                    {},
                    static_cast<uint64_t>(InstructionId::NdDmaLoop0Stride) + index};
            }
        }

        const uint64_t elementBytes = profile->dataBits / 8U;
        std::array<uint64_t, 5> strideBytes{};
        for (std::size_t index = 0; index < strideBytes.size(); ++index) {
            if (counts[index] <= 1) {
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
        return field.quantPost != 0 || field.reluPost != 0 || field.clipReluPost || field.loopEnhanceEnable ||
               field.eltwiseOp != 0 || field.eltwiseAntqEnable || field.loopEnhanceMergeEnable ||
               field.winoPostEnable || field.brcbEnable;
    }

    template <typename Field>
    MemoryCbdataResult ConvertGmRead(const Field& field, DmaLoopDirection direction) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (field.burstNum == 0 || field.burstLen == 0) {
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
                static_cast<uint64_t>(InstructionId::LoopSizeUbufToGm) + directionIndex * 3};
        }
        const DmaLoopSizeParamField& size = *registerState_.dmaLoopSizes[directionIndex];
        const std::array<uint64_t, 2> loopCounts{size.loop1Size, size.loop2Size};
        if (loopCounts[0] == 0 || loopCounts[1] == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }

        std::array<uint64_t, 2> loopStrides{};
        for (std::size_t index = 0; index < loopCounts.size(); ++index) {
            if (loopCounts[index] <= 1) {
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
        if (loopCounts[0] == 1 && loopCounts[1] == 1) {
            return BuildSingleAccess(
                std::move(builder), MemoryAccessDescriptorFactory::Linear(
                                        address, accessMode, profile.dataBits, burstNum, burstBytes, burstStride));
        }
        return BuildSingleAccess(
            std::move(builder), MemoryAccessDescriptorFactory::DmaAffine(
                                    address, accessMode, profile.dataBits, burstBytes,
                                    std::array<uint64_t, 3>{burstNum, loopCounts[0], loopCounts[1]},
                                    std::array<uint64_t, 3>{burstStride, loopStrides[0], loopStrides[1]}));
    }

    template <NdNzConversionMode ConversionMode>
    MemoryCbdataResult ConvertMultiGmRead(const CopyGmToCbufMultiParamField<ConversionMode>& field) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(field);
        if (!profile.has_value()) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const uint64_t elementBytes = profile->dataBits / 8U;
        const uint64_t rowCount = ConversionMode == NdNzConversionMode::ND2NZ ? field.nValue : field.dValue;
        const uint64_t rowElements = ConversionMode == NdNzConversionMode::ND2NZ ? field.dValue : field.nValue;
        if (rowCount == 0 || rowElements == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (!registerState_.mte2Nz.has_value()) {
            return {
                MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, static_cast<uint64_t>(InstructionId::SetMte2NzPara)};
        }
        const uint16_t matrixNum = registerState_.mte2Nz->matrixNum;
        if (matrixNum == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (rowElements > std::numeric_limits<uint64_t>::max() / elementBytes) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }

        MemoryCbdataBuilder builder(context_, *profile);
        return BuildSingleAccess(
            std::move(builder), MemoryAccessDescriptorFactory::NdRead(
                                    field.srcAddr, profile->dataBits, rowElements * elementBytes,
                                    std::array<uint64_t, 2>{rowCount, matrixNum},
                                    std::array<uint64_t, 2>{field.loop1SrcStride, field.loop4SrcStride}));
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
        if (elementCount == 0 || byteOffset > std::numeric_limits<uint64_t>::max() ||
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
            if (loop3.loopCount == 0) {
                return {MemoryCbdataStatus::NO_ACCESS, {}};
            }
            const uint64_t innerCount = field.nz2ndEnable ? field.mSize : field.nSize;
            const unsigned __int128 segmentCount = static_cast<unsigned __int128>(innerCount) * loop3.loopCount;
            if (segmentCount > kMaxExpandedMemoryAccessesPerInstruction) {
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
            uint64_t(f.blockNum) * 32, uint64_t(f.repeatGap) * 32, f.dataBits);
    }

    MemoryCbdataResult operator()(const LoadL1MxParamField& f) const noexcept
    {
        const uint64_t offset = (uint64_t(f.xStart) * f.srcStride + f.yStart) * 32;
        if (f.srcAddr > UINT64_MAX - offset) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        return Linear(
            f.srcAddr + offset, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.xStep,
            uint64_t(f.yStep) * 32, uint64_t(f.srcStride) * 32);
    }

    MemoryCbdataResult operator()(const MmadParamField& f) const noexcept
    {
        if (f.instrId < 400 || f.instrId > 415) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.m == 0 || f.n == 0 || f.k == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const uint32_t bits = f.instrId == 403                       ? 32 :
                              (f.instrId == 401 || f.instrId == 402) ? 16 :
                              (f.instrId >= 408 && f.instrId <= 411) ? 4 :
                                                                       8;
        const uint64_t m = CeilDiv(f.m, 16), n = CeilDiv(f.n, 16), k = CeilDiv(f.k, 256 / bits);
        uint64_t aFractals = m * k;
        if (f.m == 1 && !f.disableGemv) {
            // The reference packs GEMV's A into 16-row fractals. Do not silently
            // truncate a partial fractal where the instruction constraints are unknown.
            if (aFractals % 16 != 0) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            aFractals /= 16;
        }
        MemoryCbdataBuilder builder(context_, {bits, ACLSAN_DEVICE_SOURCE_CUBE, kBlockTypeAic});
        if (!Add(
                builder, f.src0Addr, ACLSAN_DEVICE_MEMORY_SPACE_L0A, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1,
                aFractals * 512, 0, bits) ||
            !Add(
                builder, f.src1Addr, ACLSAN_DEVICE_MEMORY_SPACE_L0B, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1, n * k * 512,
                0, bits) ||
            !Add(
                builder, f.dstAddr, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 1, m * n * 1024,
                0, 32)) {
            return std::move(builder).Build();
        }
        if (!f.cmatrixSource && !f.cmatrixInitVal &&
            !Add(
                builder, f.dstAddr, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1, m * n * 1024,
                0, 32)) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const LoadCbufToL0ParamField& f) const noexcept
    {
        if (f.instrId < 141 || f.instrId > 148) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.mStep == 0 || f.kStep == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const uint32_t bits = (f.instrId == 141 || f.instrId == 146) ? 4 :
                              (f.instrId == 142 || f.instrId == 145) ? 16 :
                              (f.instrId == 143 || f.instrId == 147) ? 8 :
                                                                       32;
        uint64_t dstBlocks = f.mStep, dstRepeats = f.kStep;
        if (f.transpose) {
            if ((bits == 4 && f.mStep % 4) || (bits == 8 && f.mStep % 2) || (bits == 32 && f.kStep % 2)) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            dstBlocks = uint64_t(f.kStep) * 16 / bits;
            dstRepeats = uint64_t(f.mStep) * bits / 16;
        }
        const uint64_t offset = (uint64_t(f.kStartPosition) * f.srcStride + f.mStartPosition) * 512;
        if (f.srcAddr > UINT64_MAX - offset) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        MemoryCbdataBuilder builder(context_, {bits, ACLSAN_DEVICE_SOURCE_CUBE, kBlockTypeAic});
        if (!Add(
                builder, f.srcAddr + offset, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.kStep,
                uint64_t(f.mStep) * 512, uint64_t(f.srcStride) * 512, bits) ||
            !Add(
                builder, f.dstAddr, f.instrId <= 144 ? ACLSAN_DEVICE_MEMORY_SPACE_L0A : ACLSAN_DEVICE_MEMORY_SPACE_L0B,
                ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, dstRepeats, dstBlocks * 512, uint64_t(f.dstStride) * 512, bits)) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const LoadCbufToCbTransposeParamField& f) const noexcept
    {
        if (f.instrId < 137 || f.instrId > 140) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.repeat == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        // The dav-3510 reference does not restore index/decrement addressing.
        // Do not pretend those modes use the same source base as index zero.
        if (f.indexId != 0 || f.decrement) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const uint32_t bits = f.instrId == 137 ? 8 : f.instrId == 138 ? 16 : f.instrId == 139 ? 32 : 4;
        const uint64_t fractals = bits == 4 ? 4 : bits == 16 ? 1 : 2;
        const uint64_t srcFracStride = bits == 16 ? 1 : uint64_t(f.srcFracGap) + 1;
        const uint64_t dstFracStride = bits == 16 ? 1 : uint64_t(f.dstFracGap) + 1;
        auto read = MemoryAccessDescriptorFactory::NdRead(
            f.srcAddr, bits, 512, std::array<uint64_t, 2>{fractals, f.repeat},
            std::array<uint64_t, 2>{srcFracStride * 512, uint64_t(f.srcStride) * 512});
        auto write = MemoryAccessDescriptorFactory::NdWrite(
            f.dstAddr, bits, 512, {fractals, f.repeat}, {dstFracStride * 512, (uint64_t(f.dstGap) + 1) * 512});
        return Merge(
            Build(std::move(read), ACLSAN_DEVICE_MEMORY_SPACE_L1),
            Build(std::move(write), ACLSAN_DEVICE_MEMORY_SPACE_L0B));
    }

    MemoryCbdataResult operator()(const Img2ColParamField& f) const noexcept
    {
        if (!((f.instrId >= 153 && f.instrId <= 157) || f.instrId == 422)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.mExtension == 0 || f.kExtension == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const size_t bank = f.fMatrixControl ? 1 : 0;
        if (!state_.fmatrix[bank]) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, 386 + bank};
        }
        if (!state_.l3dRpt[bank]) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, 390 + bank};
        }
        const uint32_t bits = (f.instrId == 153 || f.instrId == 155) ? 16 :
                              (f.instrId == 154 || f.instrId == 156) ? 8 :
                                                                       32;
        const uint64_t c0 = 256 / bits;
        const auto& rpt = *state_.l3dRpt[bank];
        // For one full C0, NC1HWC0 and NHWC share the same byte layout. Other
        // channel packing and repeated-window modes need separate validation.
        if (f.channelSize != c0 || rpt.repeatTimes != 1 || f.mExtension % 16 || f.kExtension % c0 ||
            f.kStartPoint % c0 || (f.transpose && (f.mExtension % c0 || f.kExtension % 16))) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const auto& fm = *state_.fmatrix[bank];
        const uint64_t width = fm.width, height = fm.height;
        const int64_t left = fm.paddingLeft, right = fm.paddingRight;
        const int64_t top = fm.paddingTop, bottom = fm.paddingBottom;
        const uint64_t fw = uint64_t(f.filterWidth) + (f.filterSizeWidth ? 256 : 0);
        const uint64_t fh = uint64_t(f.filterHeight) + (f.filterSizeHeight ? 256 : 0);
        if (width == 0 || height == 0 || fw == 0 || fh == 0 || f.strideWidth == 0 || f.strideHeight == 0 ||
            f.dilationFilterWidth == 0 || f.dilationFilterHeight == 0) {
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
        if (uint64_t(f.mExtension) * (f.kExtension / c0) >= kMaxExpandedMemoryAccessesPerInstruction) {
            return {MemoryCbdataStatus::RESOURCE_EXHAUSTED, {}};
        }
        MemoryCbdataBuilder builder(context_, {bits, ACLSAN_DEVICE_SOURCE_CUBE, kBlockTypeAic});
        for (uint64_t m = f.mStartPoint; m < uint64_t(f.mStartPoint) + f.mExtension; ++m) {
            for (uint64_t k = f.kStartPoint / c0; k < (uint64_t(f.kStartPoint) + f.kExtension) / c0; ++k) {
                const int64_t y = int64_t((m / outputW) * f.strideHeight + (k / fw) * f.dilationFilterHeight) - top;
                const int64_t x = int64_t((m % outputW) * f.strideWidth + (k % fw) * f.dilationFilterWidth) - left;
                // Padding synthesizes values: it does not read outside the feature map.
                if (y < 0 || x < 0 || uint64_t(y) >= height || uint64_t(x) >= width) {
                    continue;
                }
                const uint64_t offset = (uint64_t(y) * width + uint64_t(x)) * 32;
                if (f.srcAddr > UINT64_MAX - offset) {
                    return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
                }
                if (!Add(
                        builder, f.srcAddr + offset, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 1,
                        32, 0, bits)) {
                    return std::move(builder).Build();
                }
            }
        }
        const uint64_t dstOffset = uint64_t(rpt.dstOffset) * 512;
        if (f.dstAddr > UINT64_MAX - dstOffset) {
            return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
        }
        const uint64_t rows = f.transpose ? f.kExtension / 16 : f.mExtension / 16;
        const uint64_t columns = f.transpose ? f.mExtension / c0 : f.kExtension / c0;
        const uint32_t space = (f.instrId == 153 || f.instrId == 154 || f.instrId == 422) ?
                                   ACLSAN_DEVICE_MEMORY_SPACE_L0A :
                                   ACLSAN_DEVICE_MEMORY_SPACE_L0B;
        if (!Add(
                builder, f.dstAddr + dstOffset, space, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, columns, rows * 512,
                uint64_t(rpt.dstStride) * 512, bits)) {
            return std::move(builder).Build();
        }
        return std::move(builder).Build();
    }

    MemoryCbdataResult operator()(const CopyGmToCbufV2ParamField& f) const noexcept
    {
        if (f.burstNum == 0 || f.burstLen == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (f.padFunctionMode != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        return DmaWrite(f.dstAddr, f.burstNum, uint64_t(f.burstLen) * 32, (uint64_t(f.burstLen) + f.dstStride) * 32, 0);
    }

    MemoryCbdataResult operator()(const CopyGmToCbufAlignV2ParamField& f) const noexcept
    {
        if (f.burstLen == 0 || f.burstNum == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const uint64_t padding = uint64_t(f.leftPaddingCount + f.rightPaddingCount) * (f.dataBits / 8);
        if (f.dataSelectBit || f.leftPaddingCount * (f.dataBits / 8) > 32 ||
            f.rightPaddingCount * (f.dataBits / 8) > 32) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const auto& loops = state_.dmaLoopSizes[static_cast<size_t>(DmaLoopDirection::GM_TO_CBUF)];
        if (padding && loops && (loops->loop1Size > 1 || loops->loop2Size > 1)) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const bool compact = f.burstDstStride == f.burstLen;
        if (!compact && f.burstDstStride % 32 != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const uint64_t bytes = CeilDiv((uint64_t(f.burstLen) + padding) * (compact ? f.burstNum : 1), 32) * 32;
        return DmaWrite(f.dstAddr, compact ? 1 : f.burstNum, bytes, f.burstDstStride, f.dataBits);
    }

    MemoryCbdataResult operator()(const LoadGmToCbuf2DV2ParamField& f) const noexcept
    {
        if (f.decompMode != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        return Linear(
            f.dstAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.kStep,
            uint64_t(f.mStep) * 512, uint64_t(f.dstStride) * 512);
    }

    template <NdNzConversionMode Mode>
    MemoryCbdataResult operator()(const CopyGmToCbufMultiParamField<Mode>& f) const noexcept
    {
        if (!state_.mte2Nz) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, uint64_t(InstructionId::SetMte2NzPara)};
        }
        if (f.smallC0Enable) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        const auto& s = *state_.mte2Nz;
        if (f.nValue == 0 || f.dValue == 0 || s.matrixNum == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        auto d = MemoryAccessDescriptorFactory::DmaAffine(
            f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, f.dataBits, 32,
            {f.nValue, CeilDiv(uint64_t(f.dValue) * (f.dataBits / 8), 32), s.matrixNum},
            {uint64_t(s.loop2DstStride) * 32, uint64_t(s.loop3DstStride) * 32, uint64_t(s.loop4DstStride) * 32});
        return Build(std::move(d), ACLSAN_DEVICE_MEMORY_SPACE_L1);
    }

    MemoryCbdataResult operator()(const FixL0cToOutParamField& f) const noexcept { return FixRead(f, 64); }

    MemoryCbdataResult operator()(const LocalMemoryTransferParamField& f) const noexcept
    {
        if (f.kind == LocalMemoryTransferKind::CopyCbufToFbuf) {
            if (f.dstMemBlock >= 2 || (f.dstMemBlock == 0 && (f.lenBurst % 2 != 0 || f.dstStride % 2 != 0))) {
                return {MemoryCbdataStatus::INVALID_FIELD, {}};
            }
            // MOV_L1_TO_FB uses 64-byte lengths and absolute 32-byte source strides.
            return Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.nBurst,
                uint64_t(f.lenBurst) * 64, uint64_t(f.srcStride) * 32);
        }
        if (f.kind == LocalMemoryTransferKind::CopyCbufToBt) {
            const bool source32 = f.instrId == uint32_t(InstructionId::CopyCbufToBtF32) ||
                                  f.instrId == uint32_t(InstructionId::CopyCbufToBtS32);
            if ((source32 && (f.lenBurst % 2 != 0 || f.cvtEnable)) || f.dstGap % 2 != 0) {
                return {MemoryCbdataStatus::INVALID_FIELD, {}};
            }
            // BT conversion changes the destination footprint, not the L1 read footprint.
            return Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.nBurst,
                uint64_t(f.lenBurst) * 32, (uint64_t(f.lenBurst) + f.srcGap) * 32);
        }
        if (f.kind == LocalMemoryTransferKind::CopyUbufToCbuf || f.kind == LocalMemoryTransferKind::CopyCbufToUbuf) {
            const bool write = f.kind == LocalMemoryTransferKind::CopyUbufToCbuf;
            const uint64_t count = (f.config0 >> 4) & 0xfff;
            const uint64_t length = (f.config0 >> 16) & 0xffff;
            const uint64_t gap = (f.config0 >> (write ? 48 : 32)) & 0xffff;
            if (count == 0 || length == 0) {
                return {MemoryCbdataStatus::NO_ACCESS, {}};
            }
            const auto l1Context = ContextForL1(context_);
            if (!l1Context) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            return LinearInContext(
                *l1Context, write ? f.dstAddr : f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L1,
                write ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ, count, length * 32,
                (length + gap) * 32);
        }
        // FIX's common fields have the same encoding. Decode through the existing
        // architecture decoder; bits 16..19 describe local destinations, not L2.
        AclsanRawTraceRecord raw{};
        raw.instrId = (f.instrId == 168 || f.instrId == 170) ? 91 : 92;
        raw.args[0] = f.dstAddr;
        raw.args[1] = f.srcAddr;
        raw.args[2] = f.config0;
        raw.args[3] = f.config1 & ~(uint64_t(0xf) << 16);
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
        if (!decoded) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const auto& fix = std::get<FixL0cToOutParamField>(decoded->params);
        // L0C stores 32-bit accumulators: an N=16 row occupies 64 bytes,
        // independent of the destination type or destination memory space.
        auto read = FixRead(fix, 64);
        if (read.status != MemoryCbdataStatus::SUCCESS || f.instrId == 170 || f.instrId == 171) {
            return read;
        }
        if ((f.config1 & (uint64_t(0xf) << 16)) != 0 || fix.splitEnable) {
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
        if (first.data.size() + second.data.size() > kMaxExpandedMemoryAccessesPerInstruction) {
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
            if (second.requiredRegisterInstructionId) {
                first.requiredRegisterInstructionId = second.requiredRegisterInstructionId;
            }
        } else {
            first.status = first.data.empty() ? MemoryCbdataStatus::NO_ACCESS : MemoryCbdataStatus::SUCCESS;
        }
        // A local transfer may publish endpoints owned by different physical
        // cores.  accessIndex/accessCount are per callback identity, so keep
        // a separate fragment sequence for each owner header while preserving
        // the ordinary single-header sequence for all other instructions.
        for (size_t i = 0; i < first.data.size(); ++i) {
            uint32_t index = 0;
            uint32_t count = 0;
            for (size_t j = 0; j < first.data.size(); ++j) {
                if (first.data[j].header.phyCoreId == first.data[i].header.phyCoreId &&
                    first.data[j].header.blockId == first.data[i].header.blockId &&
                    first.data[j].header.blockType == first.data[i].header.blockType) {
                    if (j < i) {
                        ++index;
                    }
                    ++count;
                }
            }
            first.data[i].accessIndex = index;
            first.data[i].accessCount = count;
        }
        return first;
    }

private:
    static uint64_t CeilDiv(uint64_t n, uint64_t d) noexcept { return n / d + (n % d != 0); }

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
        MemoryCbdataBuilder builder(context, {d.dataBits, source, kBlockTypeAic});
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
        if (count == 0 || bytes == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        return Build(MemoryAccessDescriptorFactory::Linear(address, mode, bits, count, bytes, stride), space);
    }

    MemoryCbdataResult LinearInContext(
        MemoryCbdataContext context, uint64_t address, uint32_t space, uint32_t mode, uint64_t count, uint64_t bytes,
        uint64_t stride, uint32_t bits = 0) const noexcept
    {
        if (count == 0 || bytes == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        return BuildInContext(
            context, MemoryAccessDescriptorFactory::Linear(address, mode, bits, count, bytes, stride), space);
    }

    MemoryCbdataResult DmaWrite(
        uint64_t address, uint64_t count, uint64_t bytes, uint64_t stride, uint32_t bits) const noexcept
    {
        if (count == 0 || bytes == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const size_t direction = static_cast<size_t>(DmaLoopDirection::GM_TO_CBUF);
        const auto& size = state_.dmaLoopSizes[direction];
        const std::array<uint64_t, 3> counts{count, size ? size->loop1Size : 1, size ? size->loop2Size : 1};
        if (counts[1] == 0 || counts[2] == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        std::array<uint64_t, 3> strides{stride, 0, 0};
        for (size_t i = 0; i < 2; ++i) {
            if (counts[i + 1] > 1) {
                if (!state_.dmaLoopStrides[direction][i]) {
                    return {
                        MemoryCbdataStatus::MISSING_REGISTER_STATE,
                        {},
                        uint64_t(InstructionId::Loop1StrideGmToCbuf) + i};
                }
                strides[i + 1] = state_.dmaLoopStrides[direction][i]->dstStride;
            }
        }
        if (counts[1] == 1 && counts[2] == 1) {
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
        if (f.nSize == 0 || f.mSize == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if ((f.nz2ndEnable || f.nz2dnEnable) && state_.loop3 && state_.loop3->loopCount == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        // NZ2DN additionally needs CHANNEL_PARA, which is not captured yet.
        if (f.nz2dnEnable || f.quantPost || f.reluPost || f.clipReluPost || f.loopEnhanceEnable ||
            f.loopEnhanceMergeEnable || f.eltwiseOp || f.eltwiseAntqEnable || f.winoPostEnable || f.brcbEnable ||
            f.c0PadEnable) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        if (!f.nz2ndEnable) {
            return Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, CeilDiv(f.nSize, 16),
                uint64_t(f.mSize) * rowBytes, uint64_t(f.loopSrtStride) * rowBytes, 32);
        }
        if (!state_.loop3) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, uint64_t(InstructionId::Loop3Param)};
        }
        if (state_.loop3->loopCount == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        MemoryCbdataBuilder builder(context_, {32, ACLSAN_DEVICE_SOURCE_FIXPIPE, kBlockTypeAic});
        for (uint64_t matrix = 0; matrix < state_.loop3->loopCount; ++matrix) {
            const uint64_t offset = matrix * state_.loop3->srcStride * rowBytes;
            if (f.srcAddr > UINT64_MAX - offset) {
                return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
            }
            const uint64_t base = f.srcAddr + offset;
            if (f.nSize / 16 &&
                !Add(
                    builder, base, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ, f.nSize / 16,
                    uint64_t(f.mSize) * rowBytes, uint64_t(f.loopSrtStride) * rowBytes, 32)) {
                return std::move(builder).Build();
            }
            if (f.nSize % 16) {
                const uint64_t tailOffset = uint64_t(f.nSize / 16) * f.loopSrtStride * rowBytes;
                if (base > UINT64_MAX - tailOffset) {
                    return {MemoryCbdataStatus::ARITHMETIC_OVERFLOW, {}};
                }
                if (!Add(
                        builder, base + tailOffset, ACLSAN_DEVICE_MEMORY_SPACE_L0C, ACLSAN_DEVICE_MEMORY_ACCESS_READ,
                        f.mSize, uint64_t(f.nSize % 16) * 4, 64, 32)) {
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
        if (f.burstNum == 0 || f.burstLen == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const auto profile = MemoryInstructionProfileFactory::Create(f);
        if (!profile) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const uint64_t padding = uint64_t(f.leftPaddingCount + f.rightPaddingCount) * f.dataBits / 8;
        const bool compact = f.burstDstStride == f.burstLen && padding == 0;
        if (!compact && f.burstNum > 1 && f.burstDstStride % 32 != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        // Compact copies pad the complete burst group once; normal copies pad
        // every burst, including explicitly requested left/right padding.
        const uint64_t bytes = compact ? uint64_t(f.burstLen) * f.burstNum : uint64_t(f.burstLen) + padding;
        return Dma(
            *profile, DmaLoopDirection::GM_TO_UBUF, f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE,
            compact ? 1 : f.burstNum, (bytes + 31) / 32 * 32, f.burstDstStride);
    }

    MemoryCbdataResult operator()(const CopyUbufToGmAlignV2ParamField& f) const noexcept
    {
        if (f.burstNum == 0 || f.burstLen == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const auto profile = MemoryInstructionProfileFactory::Create(f);
        if (!profile) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const bool compact = f.srcStride == f.burstLen;
        if (!compact && f.burstNum > 1 && f.srcStride % 32 != 0) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }
        // The hardware reads the UB alignment tail, then discards it before
        // writing GM. Compact mode rounds the complete burst group once.
        const uint64_t bytes = compact ? uint64_t(f.burstLen) * f.burstNum : f.burstLen;
        return Dma(
            *profile, DmaLoopDirection::UBUF_TO_GM, f.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ,
            compact ? 1 : f.burstNum, (bytes + 31) / 32 * 32, f.srcStride);
    }

    MemoryCbdataResult operator()(const NdDmaOutToUbufParamField& f) const noexcept
    {
        const auto profile = MemoryInstructionProfileFactory::Create(f);
        if (!profile) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        NdAffineDescriptor layout{
            5, f.dataBits / 8, {f.loop0Size, f.loop1Size, f.loop2Size, f.loop3Size, f.loop4Size}, {}};
        if (std::any_of(layout.counts.begin(), layout.counts.end(), [](uint64_t n) { return n == 0; })) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (!state_.ndDmaPadCount) {
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
            if (layout.counts[i] <= 1) {
                continue;
            }
            if (!state_.ndDmaLoopStrides[i]) {
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
        if (f.burstNum == 0 || f.burstLen == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        const MemoryInstructionProfile profile{0, ACLSAN_DEVICE_SOURCE_VECTOR, kBlockTypeAiv};
        auto read = Build(
            MemoryAccessDescriptorFactory::Linear(
                f.srcAddr, ACLSAN_DEVICE_MEMORY_ACCESS_READ, 0, f.burstNum, uint64_t(f.burstLen) * 32,
                (uint64_t(f.burstLen) + f.srcGap) * 32),
            profile);
        auto write = Build(
            MemoryAccessDescriptorFactory::Linear(
                f.dstAddr, ACLSAN_DEVICE_MEMORY_ACCESS_WRITE, 0, f.burstNum, uint64_t(f.burstLen) * 32,
                (uint64_t(f.burstLen) + f.dstGap) * 32),
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
            if (length == 0 || count == 0) {
                return {MemoryCbdataStatus::NO_ACCESS, {}};
            }
            const auto ubContext = ContextForUbuf(context_, write && f.subBlockId ? 1U : 0U);
            if (!ubContext) {
                return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            return LinearInContext(
                *ubContext, write ? f.dstAddr : f.srcAddr, ACLSAN_DEVICE_MEMORY_SPACE_UB,
                write ? ACLSAN_DEVICE_MEMORY_ACCESS_WRITE : ACLSAN_DEVICE_MEMORY_ACCESS_READ, count, length * 32U,
                (length + gap) * 32U);
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
        raw.args[3] = f.config1 & ~(uint64_t(0xf) << 16U);
        const auto decoded = dav3510::GetDeviceInstructionDecoder().decode(raw);
        if (!decoded || !std::holds_alternative<FixL0cToOutParamField>(decoded->params)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        const auto& fix = std::get<FixL0cToOutParamField>(decoded->params);
        if (fix.nSize == 0 || fix.mSize == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        if (f.dualDstCtl == 3U) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.dualDstCtl == 1U && (fix.mSize == 0 || fix.mSize % 2U != 0)) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.dualDstCtl == 2U && fix.nSize % (fix.nz2ndEnable ? 2U : 32U) != 0) {
            return {MemoryCbdataStatus::INVALID_FIELD, {}};
        }
        if (f.dualDstCtl != 0 &&
            (fix.quantPre || fix.reluPre || fix.clipReluPre || fix.splitEnable || fix.nz2dnEnable)) {
            return {MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
        }

        const auto emit = [&](uint32_t subBlockId, FixL0cToOutParamField target) noexcept {
            const auto ubContext = ContextForUbuf(context_, subBlockId);
            if (!ubContext) {
                return MemoryCbdataResult{MemoryCbdataStatus::PARTIAL_COVERAGE, {}};
            }
            return MemoryFieldVisitor(*ubContext, state_, ACLSAN_DEVICE_MEMORY_SPACE_UB)(target);
        };

        if (f.dualDstCtl == 0U) {
            return emit(f.subBlockId ? 1U : 0U, fix);
        }
        FixL0cToOutParamField first = fix;
        FixL0cToOutParamField second = fix;
        if (f.dualDstCtl == 1U) {
            first.mSize = static_cast<uint16_t>(fix.mSize / 2U);
            second.mSize = first.mSize;
        } else {
            first.nSize = static_cast<uint16_t>(fix.nSize / 2U);
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
        if (count == 0 || bytes == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        auto descriptor = MemoryAccessDescriptorFactory::Linear(address, mode, bits, count, bytes, stride);
        descriptor.memorySpace = space;
        const auto source =
            context.pipeline == ACLSAN_DEVICE_PIPE_MTE3 ? ACLSAN_DEVICE_SOURCE_MTE3 : ACLSAN_DEVICE_SOURCE_CUBE;
        MemoryCbdataBuilder builder(context, {bits, source, kBlockTypeAiv});
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
        if (!state_.dmaLoopSizes[d]) {
            return {MemoryCbdataStatus::MISSING_REGISTER_STATE, {}, uint64_t(InstructionId::LoopSizeUbufToGm) + d * 3};
        }
        const auto& size = *state_.dmaLoopSizes[d];
        const std::array<uint64_t, 3> counts{bursts, size.loop1Size, size.loop2Size};
        if (counts[1] == 0 || counts[2] == 0) {
            return {MemoryCbdataStatus::NO_ACCESS, {}};
        }
        std::array<uint64_t, 3> strides{stride, 0, 0};
        for (size_t i = 0; i < 2; ++i) {
            if (counts[i + 1] <= 1) {
                continue;
            }
            if (!state_.dmaLoopStrides[d][i]) {
                return {
                    MemoryCbdataStatus::MISSING_REGISTER_STATE,
                    {},
                    uint64_t(InstructionId::Loop1StrideUbufToGm) + d * 3 + i};
            }
            const auto& s = *state_.dmaLoopStrides[d][i];
            strides[i + 1] = direction == DmaLoopDirection::UBUF_TO_GM ? s.srcStride : s.dstStride;
        }
        return Build(
            counts[1] == 1 && counts[2] == 1 ?
                MemoryAccessDescriptorFactory::Linear(address, mode, profile.dataBits, bursts, bytes, stride) :
                MemoryAccessDescriptorFactory::DmaAffine(address, mode, profile.dataBits, bytes, counts, strides),
            profile);
    }

    MemoryCbdataContext context_;
    const MemoryRegisterState& state_;
};

void ApplyAtomicConfiguration(MemoryCbdataResult& output, const MemoryRegisterState& state) noexcept
{
    const bool hasGmWrite = std::any_of(output.data.begin(), output.data.end(), [](const auto& access) {
        return access.memorySpace == ACLSAN_DEVICE_MEMORY_SPACE_GM &&
               access.accessMode == ACLSAN_DEVICE_MEMORY_ACCESS_WRITE;
    });
    if (!hasGmWrite) {
        return;
    }
    if (!state.ctrl) {
        // 未配置 CTRL 时按普通写检测，保留已还原的精确访存范围。
        // 寄存器依赖仍由翻译层提供给 Initcheck。
        return;
    }
    // CTRL[8:6] 选择原子类型；类型为 0 时禁用原子操作，忽略保留的操作位。
    const uint64_t type = (state.ctrl->value >> 6U) & 7U;
    if (type == 0) {
        return;
    }
    const uint64_t operation = (state.ctrl->value >> 9U) & 3U;
    constexpr uint32_t typeBits[] = {0, 32, 16, 16, 32, 8, 16, 0};
    constexpr AclsanDeviceMemoryAccessMode modes[] = {
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD, ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX,
        ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MIN};
    const bool widthMismatch = std::any_of(
        output.data.begin(), output.data.end(),
        [type, &typeBits](const auto& access) { return access.dataBits != 0 && access.dataBits != typeBits[type]; });
    if (operation >= std::size(modes) || typeBits[type] == 0 || widthMismatch) {
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
    return gm;
}

} // namespace aclsan
