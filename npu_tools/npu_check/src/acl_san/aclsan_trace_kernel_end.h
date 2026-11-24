/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_KERNEL_END_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_KERNEL_END_H

#include "dbi/trace_buffer_abi.h"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace aclsan {

// dav-3510 的物理核槽位按 AIC:AIV = 1:2 划分，AIC 共 36 个、AIV 共 72 个。
constexpr uint32_t TRACE_AIC_CORE_COUNT = ASCSAN_PHYSICAL_CORE_PART_COUNT * ASCSAN_AIC_CORES_PER_DIE;
constexpr uint32_t TRACE_AIV_CORE_COUNT = ASCSAN_PHYSICAL_CORE_SLOT_COUNT - TRACE_AIC_CORE_COUNT;
// Keep boundary tracking bounded independently of launch size.
constexpr uint32_t TRACE_MAX_LOGICAL_MARKERS = 1U << 20U;

// 运行时可查询到的 kernel 核类型（RT_FUNCTION_TYPE_KERNEL_TYPE 的取值）。
enum class TraceKernelCoreType : uint32_t {
    Unknown = 0,
    Aicore = 1,
    Aic = 2,
    Aiv = 3,
    MixCubeMain = 4,
    MixVectorMain = 5,
    AicRollback = 6,
    AivRollback = 7,
};

// 一次 launch 期望出现的 boundary marker 数量；known=false 时不做提前结束判定。
struct TraceKernelCoreExpectation {
    bool known = false;
    uint32_t aicCores = 0;
    uint32_t aivCores = 0;
};

// 397/398 两个合成插入点。398 在 kernel 栈帧前、397 在 kernel 结束前。
enum class TraceKernelMarker : uint8_t {
    BeforeKernelStackFrame = 0,
    BeforeKernelEnd = 1,
};

constexpr bool IsSupportedTraceMixRatio(uint32_t aicRatio, uint32_t aivRatio) noexcept
{
    if (aicRatio == 1U) {
        return aivRatio == 0U || aivRatio == 1U || aivRatio == 2U;
    }
    return aicRatio == 0U && aivRatio == 1U;
}

// 根据 kernel 类型、blockCount 与可选 mix ratio 计算期望的 AIC/AIV marker 数量。
// dav-3510 上 K_TYPE_AICORE 等价于 AIC=blockCount、AIV=2*blockCount（混合 kernel 的物理核配比）。
constexpr TraceKernelCoreExpectation MakeTraceKernelCoreExpectation(
    TraceKernelCoreType type, uint32_t blockCount, uint32_t aicRatio = 0U, uint32_t aivRatio = 0U) noexcept
{
    TraceKernelCoreExpectation expectation{};
    if (blockCount == 0U) {
        return expectation;
    }
    uint32_t aicPerBlock = 0U;
    uint32_t aivPerBlock = 0U;
    switch (type) {
        case TraceKernelCoreType::Aicore:
            aicPerBlock = 1U;
            aivPerBlock = 2U;
            break;
        case TraceKernelCoreType::Aic:
            aicPerBlock = 1U;
            break;
        case TraceKernelCoreType::Aiv:
            aivPerBlock = 1U;
            break;
        case TraceKernelCoreType::MixCubeMain:
        case TraceKernelCoreType::MixVectorMain:
            if (!IsSupportedTraceMixRatio(aicRatio, aivRatio)) {
                return expectation;
            }
            aicPerBlock = aicRatio;
            aivPerBlock = aivRatio;
            break;
        default:
            return expectation;
    }
    if ((aicPerBlock != 0U && blockCount > std::numeric_limits<uint32_t>::max() / aicPerBlock) ||
        (aivPerBlock != 0U && blockCount > std::numeric_limits<uint32_t>::max() / aivPerBlock)) {
        return expectation;
    }
    expectation.aicCores = blockCount * aicPerBlock;
    expectation.aivCores = blockCount * aivPerBlock;
    if (static_cast<uint64_t>(expectation.aicCores) + expectation.aivCores > TRACE_MAX_LOGICAL_MARKERS) {
        return expectation;
    }
    expectation.known = true;
    return expectation;
}

// Match both boundary markers for each logical block, including reused slots.
class TraceKernelEndTracker final {
public:
    void Reset(const TraceKernelCoreExpectation& expectation) noexcept
    {
        expectation_ = expectation;
        const uint64_t total = static_cast<uint64_t>(expectation.aicCores) + expectation.aivCores;
        valid_ = expectation.known && total != 0U && total <= TRACE_MAX_LOGICAL_MARKERS;
        aicStarts_ = aivStarts_ = aicEnds_ = aivEnds_ = 0U;
        activeBlocks_.fill(NO_ACTIVE_BLOCK);
        aicMarkers_.clear();
        aivMarkers_.clear();
        if (!valid_)
            return;
        try {
            aicMarkers_.assign(expectation.aicCores, 0U);
            aivMarkers_.assign(expectation.aivCores, 0U);
        } catch (...) {
            valid_ = false;
            aicMarkers_.clear();
            aivMarkers_.clear();
        }
    }

    const TraceKernelCoreExpectation& Expectation() const noexcept { return expectation_; }
    bool Known() const noexcept { return expectation_.known && valid_; }

    void Observe(TraceKernelMarker marker, uint32_t physicalCoreSlot, uint32_t blockId) noexcept
    {
        if (!Known())
            return;
        if (physicalCoreSlot >= ASCSAN_PHYSICAL_CORE_SLOT_COUNT) {
            valid_ = false;
            return;
        }
        const bool aic = IsAicPhysicalCore(physicalCoreSlot);
        auto& markers = aic ? aicMarkers_ : aivMarkers_;
        if (blockId >= markers.size()) {
            valid_ = false;
            return;
        }
        constexpr uint8_t MARKER_COMPLETE = 1U << 7U;
        static_assert(ASCSAN_PHYSICAL_CORE_SLOT_COUNT < MARKER_COMPLETE);
        const auto slot = static_cast<uint8_t>(physicalCoreSlot + 1U);
        auto& value = markers[blockId];
        if (marker == TraceKernelMarker::BeforeKernelStackFrame) {
            if (value != 0U || activeBlocks_[physicalCoreSlot] != NO_ACTIVE_BLOCK) {
                valid_ = false;
                return;
            }
            activeBlocks_[physicalCoreSlot] = blockId;
            value = slot;
            ++(aic ? aicStarts_ : aivStarts_);
        } else if (marker == TraceKernelMarker::BeforeKernelEnd) {
            if (value != slot || activeBlocks_[physicalCoreSlot] != blockId) {
                valid_ = false;
                return;
            }
            activeBlocks_[physicalCoreSlot] = NO_ACTIVE_BLOCK;
            value |= MARKER_COMPLETE;
            ++(aic ? aicEnds_ : aivEnds_);
        } else {
            valid_ = false;
        }
    }

    bool Complete() const noexcept
    {
        return Known() && aicStarts_ == expectation_.aicCores && aivStarts_ == expectation_.aivCores &&
               aicEnds_ == expectation_.aicCores && aivEnds_ == expectation_.aivCores;
    }

    uint32_t AicStarts() const noexcept { return aicStarts_; }
    uint32_t AivStarts() const noexcept { return aivStarts_; }
    uint32_t AicEnds() const noexcept { return aicEnds_; }
    uint32_t AivEnds() const noexcept { return aivEnds_; }

private:
    static constexpr uint32_t NO_ACTIVE_BLOCK = std::numeric_limits<uint32_t>::max();
    TraceKernelCoreExpectation expectation_{};
    std::array<uint32_t, ASCSAN_PHYSICAL_CORE_SLOT_COUNT> activeBlocks_{};
    std::vector<uint8_t> aicMarkers_;
    std::vector<uint8_t> aivMarkers_;
    uint32_t aicStarts_ = 0U;
    uint32_t aivStarts_ = 0U;
    uint32_t aicEnds_ = 0U;
    uint32_t aivEnds_ = 0U;
    bool valid_ = false;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_TRACE_KERNEL_END_H
