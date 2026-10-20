/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_MEMORY_CBDATA_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_MEMORY_CBDATA_H

#include "acl_san/aclsan_cbdata_device.h"
#include "device_instr/common/device_instr_struct_dma.h"
#include "device_instr/common/device_instr_struct_scalar.h"
#include "device_instr/common/device_instr_struct_register.h"
#include "device_instr/common/device_instr_next_iter.h"

#include <array>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace aclsan {

enum class MemoryCbdataStatus : uint8_t {
    SUCCESS,   // 转换成功，已生成访存 cbdata；不代表访问已通过 Memcheck 边界检查。
    NO_ACCESS, // 本次转换不生成访存 cbdata，如零长度、零次数或当前实现直接跳过的模式。
    INVALID_FIELD, // 指令 ID、位宽、控制字段或字段组合非法，或当前转换器不支持该模式。
    MISSING_REGISTER_STATE, // 缺少还原访问所需的 SET 配置寄存器状态，如 DMA loop、MTE2 或 FIXPIPE 配置。
    ARITHMETIC_OVERFLOW, // 地址、长度或步长计算溢出，或访问布局不满足内部描述的可表示性约束。
    RESOURCE_EXHAUSTED, // 展开的访问记录数超过内部上限，或容器分配失败、容量超限。
    MISSING_ADDRESS_CONTEXT, // 地址映射协议标记缺失或不匹配，无法确认采集的 SYS_VA_BASE 上下文有效。
    UNSUPPORTED_ADDRESS_SPACE, // 已识别为 UB 或 PRIVATE，但当前转换路径仅支持 GM。
    INVALID_ADDRESS_SPACE,     // 地址编码未匹配到已知的存储空间，无法完成地址映射。
    PARTIAL_COVERAGE,          // 已知指令的部分模式尚未确认实际访问集合。
};

using MemoryInstructionField = std::variant<
    CopyGmToUbufAlignV2ParamField, CopyGmToCbufAlignV2ParamField, CopyGmToCbufMultiNd2NzParamField,
    CopyGmToCbufMultiDn2NzParamField, CopyGmToCbufV2ParamField, CopyUbufToGmAlignV2ParamField, FixL0cToOutParamField,
    LoadGmToCbuf2DV2ParamField, NdDmaOutToUbufParamField, SetL12DParamField, MmadParamField, LoadL1MxParamField,
    LocalMemoryTransferParamField, LoadCbufToL0ParamField, LoadCbufToCbTransposeParamField, Img2ColParamField,
    ScalarGmParamField, ScalarDevParamField, ScalarPreloadParamField, ScalarAtomicParamField>;

struct MemoryRegisterState {
    std::optional<CtrlRegisterParamField> ctrl;
    std::array<std::optional<FmatrixParamField>, 2> fmatrix{}; // normal / _B register banks
    std::array<std::optional<L3dRptParamField>, 2> l3dRpt{};
    std::optional<Mte2SourceParamField> mte2Source;
    std::optional<NdDmaPadCountParamField> ndDmaPadCount;
    std::array<std::optional<NdDmaLoopStrideParamField>, 5> ndDmaLoopStrides{};
    std::optional<Mte2NzParamField> mte2Nz;
    std::optional<Loop3ParamField> loop3;
    std::array<std::optional<DmaLoopSizeParamField>, 3> dmaLoopSizes{};
    std::array<std::array<std::optional<DmaLoopStrideParamField>, 2>, 3> dmaLoopStrides{};
};

struct InternalMemoryRange {
    void* address = nullptr;
    size_t bytes = 0;
};

struct MemoryCbdataContext {
    uint64_t pc = 0;
    uint64_t instrExecId = 0;
    uint64_t serialNo = 0;
    uint32_t siteId = 0;
    uint32_t coreId = 0;
    uint32_t blockId = 0;
    uint32_t pipeline = 0;
    uint64_t launchId = 0;
    uint32_t deviceId = 0;
    uint32_t blockType = ACLSAN_DEVICE_BLOCK_TYPE_AICORE;
    uint64_t parameterBase = 0;
    uint64_t parameterBytes = 0;
    const std::vector<InternalMemoryRange>* internalInputs = nullptr;
    const std::vector<InternalMemoryRange>* readOnlyBinaryRanges = nullptr;
    uint32_t instructionId = 0;
};

using MemoryCbdata = std::vector<AclsanDeviceMemoryAccessData>;

struct MemoryCbdataResult {
    MemoryCbdataStatus status = MemoryCbdataStatus::INVALID_FIELD;
    MemoryCbdata data;
    uint64_t requiredRegisterInstructionId = 0;
    // 即使保留了已知访问，仍须补发采集不完整事件；翻译层无需识别具体指令。
    bool requiresIncompleteEvent = false;
};

class MemoryFieldToCbdataConverter final {
public:
    explicit MemoryFieldToCbdataConverter(MemoryCbdataContext context, MemoryRegisterState registerState = {}) noexcept;

    [[nodiscard]] MemoryCbdataResult Convert(const MemoryInstructionField& field) const noexcept;

private:
    MemoryCbdataContext context_;
    MemoryRegisterState registerState_;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_ACLSAN_MEMORY_CBDATA_H
