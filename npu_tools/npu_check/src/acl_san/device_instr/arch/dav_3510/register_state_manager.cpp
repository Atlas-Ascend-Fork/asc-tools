/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "device_instr/arch/dav_3510/register_state_manager.h"
#include "device_instr/common/instruction_id.h"
#include "npu_tool_log.h"

#include <functional>

namespace aclsan::dav3510 {

bool Dav3510CoreKey::operator==(const Dav3510CoreKey& other) const noexcept
{
    return blockType == other.blockType && blockId == other.blockId && phyCoreId == other.phyCoreId &&
           deviceId == other.deviceId;
}

Dav3510RegisterStateManager::Dav3510RegisterStateManager(uint64_t launchId) noexcept : launchId_(launchId) {}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const CtrlRegisterParamField& params) noexcept
{
    states_[key].ctrl = params;
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const FmatrixParamField& params) noexcept
{
    if (params.instrId != static_cast<uint32_t>(InstructionId::SetFmatrix) &&
        params.instrId != static_cast<uint32_t>(InstructionId::SetFmatrixB)) {
        return;
    }
    states_[key].fmatrix[params.instrId - 386] = params;
    ASCTOOL_DEBUG(
        "[register] fmatrix instrId=%u blockType=%u blockId=%u width=%u height=%u", params.instrId, key.blockType,
        key.blockId, params.width, params.height);
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const L3dRptParamField& params) noexcept
{
    if (params.instrId != static_cast<uint32_t>(InstructionId::SetL3dRpt) &&
        params.instrId != static_cast<uint32_t>(InstructionId::SetL3dRptB)) {
        return;
    }
    states_[key].l3dRpt[params.instrId - 390] = params;
    ASCTOOL_DEBUG(
        "[register] l3dRpt instrId=%u blockType=%u blockId=%u repeatTimes=%u dstStride=%u dstOffset=%u", params.instrId,
        key.blockType, key.blockId, params.repeatTimes, params.dstStride, params.dstOffset);
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const VectorMaskParamField& params) noexcept
{
    states_[key].vectorMask = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=vector_mask launchId=%llu blockType=%u blockId=%u "
        "vectorMask0=0x%llx vectorMask1=0x%llx",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId,
        static_cast<unsigned long long>(params.vectorMask0), static_cast<unsigned long long>(params.vectorMask1));
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const VectorMaskWriteParamField& params) noexcept
{
    if (params.index >= VECTOR_MASK_REGISTER_COUNT) {
        return;
    }
    // AscendC starts with a full vector mask; subsequent writes replace only the selected half.
    auto masks = states_[key].vectorMask.value_or(VectorMaskParamField{UINT64_MAX, UINT64_MAX});
    if (params.index == 0) {
        masks.vectorMask0 = params.value;
    } else {
        masks.vectorMask1 = params.value;
    }
    Update(key, masks);
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const SetPaddingParamField& params) noexcept
{
    states_[key].setPadding = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=set_padding launchId=%llu blockType=%u blockId=%u value=0x%llx",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId,
        static_cast<unsigned long long>(params.value));
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const Mte2SourceParamField& params) noexcept
{
    states_[key].mte2Source = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=mte2_source launchId=%llu blockType=%u blockId=%u srcStride=%lld",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId,
        static_cast<long long>(params.srcStride));
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const NdDmaPadCountParamField& params) noexcept
{
    states_[key].ndDmaPadCount = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=nddma_pad_count launchId=%llu blockType=%u blockId=%u "
        "left=[%u,%u,%u,%u] right=[%u,%u,%u,%u]",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId, params.leftPaddingCounts[0],
        params.leftPaddingCounts[1], params.leftPaddingCounts[2], params.leftPaddingCounts[3],
        params.rightPaddingCounts[0], params.rightPaddingCounts[1], params.rightPaddingCounts[2],
        params.rightPaddingCounts[3]);
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const NdDmaLoopStrideParamField& params) noexcept
{
    if (params.loopIndex >= states_[key].ndDmaLoopStrides.size()) {
        return;
    }
    states_[key].ndDmaLoopStrides[params.loopIndex] = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=nddma_loop_stride launchId=%llu blockType=%u blockId=%u "
        "loopIndex=%u srcStride=%llu",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId, params.loopIndex,
        static_cast<unsigned long long>(params.srcStride));
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const Mte2NzParamField& params) noexcept
{
    states_[key].mte2Nz = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=mte2_nz launchId=%llu blockType=%u blockId=%u matrixNum=%u",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId,
        static_cast<unsigned int>(params.matrixNum));
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const ChannelParaParamField& params) noexcept
{
    states_[key].channelPara = params;
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const Loop3ParamField& params) noexcept
{
    states_[key].loop3 = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=loop3 launchId=%llu blockType=%u blockId=%u "
        "loopCount=%u srcStride=%u dstStride=%u",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId,
        static_cast<unsigned int>(params.loopCount), static_cast<unsigned int>(params.srcStride), params.dstStride);
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const DmaLoopSizeParamField& params) noexcept
{
    const auto direction = static_cast<std::size_t>(params.direction);
    if (direction >= states_[key].dmaLoopSizes.size()) {
        return;
    }
    states_[key].dmaLoopSizes[direction] = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=dma_loop_size launchId=%llu blockType=%u blockId=%u "
        "direction=%u loop1Size=%u loop2Size=%llu",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId,
        static_cast<unsigned int>(params.direction), params.loop1Size,
        static_cast<unsigned long long>(params.loop2Size));
}

void Dav3510RegisterStateManager::Update(const Dav3510CoreKey& key, const DmaLoopStrideParamField& params) noexcept
{
    const auto direction = static_cast<std::size_t>(params.direction);
    if (direction >= states_[key].dmaLoopStrides.size() || params.loopIndex >= DMA_STRIDE_LOOP_COUNT) {
        return;
    }
    states_[key].dmaLoopStrides[direction][params.loopIndex] = params;
    ASCTOOL_DEBUG(
        "[register] action=update register=dma_loop_stride launchId=%llu blockType=%u blockId=%u "
        "direction=%u loopIndex=%u srcStride=%llu dstStride=%llu",
        static_cast<unsigned long long>(launchId_), key.blockType, key.blockId,
        static_cast<unsigned int>(params.direction), params.loopIndex,
        static_cast<unsigned long long>(params.srcStride), static_cast<unsigned long long>(params.dstStride));
}

std::optional<Dav3510CoreRegisterState> Dav3510RegisterStateManager::Get(const Dav3510CoreKey& key) const noexcept
{
    const auto state = states_.find(key);
    if (state == states_.end()) {
        return std::nullopt;
    }
    return state->second;
}

uint64_t Dav3510RegisterStateManager::GetLaunchId() const noexcept { return launchId_; }

void Dav3510RegisterStateManager::Reset() noexcept { states_.clear(); }

std::size_t Dav3510RegisterStateManager::CoreKeyHash::operator()(const Dav3510CoreKey& key) const noexcept
{
    std::size_t result = std::hash<uint32_t>{}(key.blockType);
    result ^= std::hash<uint32_t>{}(key.blockId) + (result << 6U) + (result >> 2U);
    result ^= std::hash<uint32_t>{}(key.phyCoreId) + (result << 6U) + (result >> 2U);
    result ^= std::hash<uint32_t>{}(key.deviceId) + (result << 6U) + (result >> 2U);
    return result;
}

} // namespace aclsan::dav3510
