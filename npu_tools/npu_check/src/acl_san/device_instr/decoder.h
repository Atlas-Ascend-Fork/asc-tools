/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_DECODER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_DECODER_H

#include "device_instr/common/device_instr_struct_dma.h"
#include "device_instr/common/device_instr_struct_scalar.h"
#include "device_instr/common/device_instr_struct_register.h"
#include "device_instr/common/device_instr_struct_sync.h"
#include "device_instr/common/device_instr_next_iter.h"
#include "dbi/trace_buffer_abi.h"

#include <optional>
#include <variant>

namespace aclsan {

// 将不同的指令合并，例如CopyGmToUbufAlignB16/B32/B8合并为一个
enum class DeviceInstructionKind : uint32_t {
    InvalidInstruction, // 默认值，错误场景
    Mmad,
    LoadL1Mx,
    LoadL1ToL0,
    LoadL1Transpose,
    Img2Col,
    Img2ColRegister,

    CopyGmToUbufAlignV2, // InstructionId::CopyGmToUbufAlignV2B8/B16/B32 -> CopyGmToUbufAlignV2ParamField
    CopyGmToCbufAlignV2, // InstructionId::CopyGmToCbufAlignV2B8/B16/B32 -> CopyGmToCbufAlignV2ParamField
    CopyUbufToGmAlignV2, // InstructionId::CopyUbufToGmAlignV2           -> CopyUbufToGmAlignV2ParamField
    CopyGmToCbufMulti,   // InstructionId::CopyGmToCbufMulti*            -> CopyGmToCbufMultiParamField<DN2NZ/ND2NZ>
    CopyGmToCbufV2,      // InstructionId::CopyGmToCbufV2                -> CopyGmToCbufV2ParamField
    LoadGmToCbuf2DV2,    // InstructionId::LoadGmToCbuf2DV2              -> LoadGmToCbuf2DV2ParamField
    NdDmaOutToUbuf,      // InstructionId::NdDmaOutToUbufB8/B16/B32      -> NdDmaOutToUbufParamField
    Mte2SourceParam,     // InstructionId::Mte2SrcPara                 -> Mte2SourceParamField
    NdDmaPadCount,       // InstructionId::NdDmaPadCount               -> NdDmaPadCountParamField
    NdDmaLoopStride,     // InstructionId::NdDmaLoop*Stride            -> NdDmaLoopStrideParamField
    Mte2NzParam,         // InstructionId::SetMte2NzPara               -> Mte2NzParamField
    Loop3Param,          // InstructionId::Loop3Param                   -> Loop3ParamField
    DmaLoopSize,         // InstructionId::LoopSize*                    -> DmaLoopSizeParamField
    DmaLoopStride,       // InstructionId::Loop*Stride*                 -> DmaLoopStrideParamField
    SetL12D,             // InstructionId::SetL12DB16/B32                -> SetL12DParamField
    FixL0cToOut,         // InstructionId::FixL0cToOutF32/S32            -> FixL0cToOutParamField
    LocalMemoryTransfer, // InstructionId::CopyCbufToFbuf/FixL0cTo*/CopyUbufToCbuf -> LocalMemoryTransferParamField
    SetPadding,          // InstructionId::SetPadding                    -> SetPaddingParamField

    ScalarGm, // InstructionId::St*/Sti*/Stp*/Ld*/Ldp* -> ScalarGmParamField
    StDev,    // InstructionId::StDevB64/B32/B16/B8 -> ScalarDevParamField
    LdDev,    // InstructionId::LdDevB64/B32/B16/B8 -> ScalarDevParamField
    ScalarPreload,
    ScalarAtomic, // InstructionId::StAtomicB*/StiAtomicB* -> ScalarAtomicParamField

    SetFlag,           // InstructionId::SetFlag/SetFlagI/SetFlagV/SetFlagIV     -> FlagParamField
    WaitFlag,          // InstructionId::WaitFlag/WaitFlagI/WaitFlagV/WaitFlagIV -> FlagParamField
    HardwareSetFlag,   // InstructionId::HSetFlag/HSetFlagI -> HardwareFlagParamField
    HardwareWaitFlag,  // InstructionId::HWaitFlag/HWaitFlagI -> HardwareFlagParamField
    CrossCoreSetFlag,  // InstructionId::SetCrossCore/SetCrossCoreV -> FftsSyncParamField
    CrossCoreWaitFlag, // WAIT_FLAG_DEV(_V) -> DeviceFlagParamField
    GetBuf,            // InstructionId::GetBuf/GetBufI/GetBufV/GetBufIV         -> SyncBufParamField
    RlsBuf,            // InstructionId::RlsBuf/RlsBufI/RlsBufV/RlsBufIV         -> SyncBufParamField
    PipeBarrier,
};

using DeviceInstructionParamField = std::variant<
    std::monostate, // 默认构造为空
    CopyGmToUbufAlignV2ParamField, CopyGmToCbufAlignV2ParamField, CopyUbufToGmAlignV2ParamField,
    CopyGmToCbufMultiDn2NzParamField, CopyGmToCbufMultiNd2NzParamField, CopyGmToCbufV2ParamField, Mte2SourceParamField,
    NdDmaPadCountParamField, NdDmaLoopStrideParamField, Mte2NzParamField, Loop3ParamField, DmaLoopSizeParamField,
    DmaLoopStrideParamField, LoadGmToCbuf2DV2ParamField, NdDmaOutToUbufParamField, SetL12DParamField,
    FixL0cToOutParamField, LocalMemoryTransferParamField, SetPaddingParamField, FlagParamField, HardwareFlagParamField,
    FftsSyncParamField, DeviceFlagParamField, SyncBufParamField, ScalarDevParamField, ScalarPreloadParamField,
    ScalarAtomicParamField, MmadParamField, LoadL1MxParamField, LoadCbufToL0ParamField, LoadCbufToCbTransposeParamField,
    Img2ColParamField, FmatrixParamField, L3dRptParamField, ScalarGmParamField, PipeBarrierParamField>;

struct DecodedInstruction {
    DeviceInstructionKind kind = DeviceInstructionKind::InvalidInstruction;
    DeviceInstructionParamField params{}; // 该条指令对应的ParamField
};

// 输入：一条raw data 输出：解码成功时为DecodedInstruction，失败时为std::nullopt
// DecodeDeviceInstructionFunc 为函数指针
using DecodeDeviceInstructionFunc =
    std::optional<DecodedInstruction> (*)(const aclsan::AclsanRawTraceRecord& record) noexcept;

// 示例：DeviceInstructionDecoder decoder{"dav_3510", Decode}; Decode(const aclsan::AclsanRawTraceRecord& record)
struct DeviceInstructionDecoder {
    const char* architecture = nullptr;
    DecodeDeviceInstructionFunc decode = nullptr;
};

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_DECODER_H
