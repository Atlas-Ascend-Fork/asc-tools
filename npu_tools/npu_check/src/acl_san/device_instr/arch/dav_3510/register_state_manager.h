/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_ARCH_DAV_3510_REGISTER_STATE_MANAGER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_ARCH_DAV_3510_REGISTER_STATE_MANAGER_H

#include "device_instr/common/device_instr_struct_dma.h"
#include "device_instr/common/device_instr_struct_register.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace aclsan::dav3510 {

struct Dav3510CoreKey {
    uint32_t blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE;
    uint32_t blockId = 0;
    uint32_t phyCoreId = 0;
    uint32_t deviceId = 0;

    bool operator==(const Dav3510CoreKey& other) const noexcept;
};

struct Dav3510CoreRegisterState {
    std::optional<CtrlRegisterParamField> ctrl;
    std::array<std::optional<FmatrixParamField>, MATRIX_REGISTER_BANK_COUNT> fmatrix{}; // normal / _B register banks
    std::array<std::optional<L3dRptParamField>, MATRIX_REGISTER_BANK_COUNT> l3dRpt{};
    std::optional<VectorMaskParamField> vectorMask;
    std::optional<Mte2SourceParamField> mte2Source;
    std::optional<NdDmaPadCountParamField> ndDmaPadCount;
    std::array<std::optional<NdDmaLoopStrideParamField>, NDDMA_LOOP_COUNT> ndDmaLoopStrides{};
    std::optional<Mte2NzParamField> mte2Nz;
    std::optional<Loop3ParamField> loop3;
    std::array<std::optional<DmaLoopSizeParamField>, DMA_DIRECTION_COUNT> dmaLoopSizes{};
    std::array<std::array<std::optional<DmaLoopStrideParamField>, DMA_STRIDE_LOOP_COUNT>, DMA_DIRECTION_COUNT>
        dmaLoopStrides{};
    std::optional<SetPaddingParamField> setPadding;
};

class Dav3510RegisterStateManager final {
public:
    explicit Dav3510RegisterStateManager(uint64_t launchId) noexcept;

    void Update(const Dav3510CoreKey& key, const CtrlRegisterParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const VectorMaskParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const FmatrixParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const L3dRptParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const Mte2SourceParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const NdDmaPadCountParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const NdDmaLoopStrideParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const Mte2NzParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const Loop3ParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const DmaLoopSizeParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const DmaLoopStrideParamField& params) noexcept;
    void Update(const Dav3510CoreKey& key, const SetPaddingParamField& params) noexcept;
    std::optional<Dav3510CoreRegisterState> Get(const Dav3510CoreKey& key) const noexcept;
    uint64_t GetLaunchId() const noexcept;
    void Reset() noexcept;

private:
    struct CoreKeyHash {
        std::size_t operator()(const Dav3510CoreKey& key) const noexcept;
    };

    uint64_t launchId_ = 0;
    std::unordered_map<Dav3510CoreKey, Dav3510CoreRegisterState, CoreKeyHash> states_;
};

} // namespace aclsan::dav3510

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_ARCH_DAV_3510_REGISTER_STATE_MANAGER_H
