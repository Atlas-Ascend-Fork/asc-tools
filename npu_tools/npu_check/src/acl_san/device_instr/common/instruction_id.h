/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_INSTRUCTION_ID_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_INSTRUCTION_ID_H

#include <cstdint>

namespace aclsan {

// Instruction IDs are stable across chips. Each architecture decoder defines the subset it handles.
// “已完成”表示搬运指令已完成 RawData -> ParamField -> CBData 转换，或 SET 指令已作为独立状态被后续
// 搬运指令的 CBData 转换消费。
enum class InstructionId : uint32_t {
    // A5 scalar GM load/store. REG offsets are element counts; IMM and pair offsets are signed bytes.
    StB64Imm = 24,
    StB32Imm = 25,
    StB16Imm = 26,
    StB8Imm = 27,
    StB64Reg = 28,
    StB32Reg = 29,
    StB16Reg = 30,
    StB8Reg = 31,
    StpB64 = 32,
    StpB32 = 33,
    StpB16 = 34,
    StpB8 = 35,
    StiB64Imm = 36,
    StiB32Imm = 37,
    StiB16Imm = 38,
    StiB8Imm = 39,
    StiB64Reg = 40,
    StiB32Reg = 41,
    StiB16Reg = 42,
    StiB8Reg = 43,
    LdB64Imm = 44,
    LdB32Imm = 45,
    LdB16Imm = 46,
    LdB8Imm = 47,
    LdB64Reg = 48,
    LdB32Reg = 49,
    LdB16Reg = 50,
    LdB8Reg = 51,
    LdpB64 = 52,
    LdpB32 = 53,
    LdpB16 = 54,
    LdpB8 = 55,

    // Scalar atomic stores. ST_ATOMIC offsets are element counts; STI_ATOMIC offsets are bytes.
    StAtomicB32 = 56,
    StAtomicB16 = 57,
    StAtomicB8 = 58,
    StiAtomicB32 = 59,
    StiAtomicB16 = 60,
    StiAtomicB8 = 61,
    DcPreload = 62,
    DcPreloadI = 63,
    // Scalar GM load/store; offsets are signed bytes.
    StDevB64 = 64,
    StDevB32 = 65,
    StDevB16 = 66,
    StDevB8 = 67,
    LdDevB64 = 68,
    LdDevB32 = 69,
    LdDevB16 = 70,
    LdDevB8 = 71,

    LoadCbufToCbTransposeB8 = 137,
    LoadCbufToCbTransposeB16 = 138,
    LoadCbufToCbTransposeB32 = 139,
    LoadCbufToCbTransposeB4 = 140,
    LoadCbufToCaB4 = 141,
    LoadCbufToCaB16 = 142,
    LoadCbufToCaB8 = 143,
    LoadCbufToCaB32 = 144,
    LoadCbufToCbB16 = 145,
    LoadCbufToCbB4 = 146,
    LoadCbufToCbB8 = 147,
    LoadCbufToCbB32 = 148,
    Img2ColCbufToCaB16 = 153,
    Img2ColCbufToCaB8 = 154,
    Img2ColCbufToCbB16 = 155,
    Img2ColCbufToCbB8 = 156,
    Img2ColCbufToCbB32 = 157,
    Img2ColCbufToCaB32 = 422,
    SetFmatrix = 386,
    SetFmatrixB = 387,
    SetL3dRpt = 390,
    SetL3dRptB = 391,
    // Cube matrix and MX source accesses.
    LoadCbufToCaMx = 151,
    LoadCbufToCbMx = 152,
    CopyCbufToUbuf = 158,
    CopyCbufToBtF32 = 423, // LocalMemoryTransferParamField: L1 READ
    CopyCbufToBtS32 = 424,
    CopyCbufToBtF16 = 425,
    CopyCbufToBtBf16 = 426,
    MadS8 = 400,
    MadF16F32 = 401,
    MadBf16F32 = 402,
    MadF32F32 = 403,
    MadE4m3E4m3 = 404,
    MadE4m3E5m2 = 405,
    MadE5m2E4m3 = 406,
    MadE5m2E5m2 = 407,
    MadMxE1m2E1m2 = 408,
    MadMxE1m2E2m1 = 409,
    MadMxE2m1E1m2 = 410,
    MadMxE2m1E2m1 = 411,
    MadMxE4m3E4m3 = 412,
    MadMxE4m3E5m2 = 413,
    MadMxE5m2E4m3 = 414,
    MadMxE5m2E5m2 = 415,
    // MTE2
    LoadGmToCbuf2DV2 = 72,          // decompMode 0；非零跳过
    CopyGmToCbufV2 = 73,            // 已完成：CopyGmToCbufV2ParamField
    CopyGmToCbufAlignV2B8 = 74,     // 已完成：CopyGmToCbufAlignV2ParamField
    CopyGmToCbufAlignV2B16 = 75,    // 已完成：CopyGmToCbufAlignV2ParamField
    CopyGmToCbufAlignV2B32 = 76,    // 已完成：CopyGmToCbufAlignV2ParamField
    CopyGmToCbufMultiNd2NzB8 = 77,  // 已完成：CopyGmToCbufMultiNd2NzParamField
    CopyGmToCbufMultiNd2NzB16 = 78, // 已完成：CopyGmToCbufMultiNd2NzParamField
    CopyGmToCbufMultiNd2NzB32 = 79, // 已完成：CopyGmToCbufMultiNd2NzParamField
    CopyGmToCbufMultiDn2NzB8 = 80,  // 已完成：CopyGmToCbufMultiDn2NzParamField
    CopyGmToCbufMultiDn2NzB16 = 81, // 已完成：CopyGmToCbufMultiDn2NzParamField
    CopyGmToCbufMultiDn2NzB32 = 82, // 已完成：CopyGmToCbufMultiDn2NzParamField
    CopyGmToUbufAlignV2B8 = 84,     // 已完成：CopyGmToUbufAlignV2ParamField
    CopyGmToUbufAlignV2B16 = 85,    // 已完成：CopyGmToUbufAlignV2ParamField
    CopyGmToUbufAlignV2B32 = 86,    // 已完成：CopyGmToUbufAlignV2ParamField
    Mte2SrcPara = 124,              // 已完成：Mte2SourceParamField（状态；支持正、零、负 stride）
    LoopSizeUbufToGm = 125,         // 已完成：DmaLoopSizeParamField（状态）
    Loop1StrideUbufToGm = 126,      // 已完成：DmaLoopStrideParamField（状态）
    Loop2StrideUbufToGm = 127,      // 已完成：DmaLoopStrideParamField（状态）
    LoopSizeGmToUbuf = 128,         // 已完成：DmaLoopSizeParamField（状态）
    Loop1StrideGmToUbuf = 129,      // 已完成：DmaLoopStrideParamField（状态）
    Loop2StrideGmToUbuf = 130,      // 已完成：DmaLoopStrideParamField（状态）
    NdDmaPadCount = 131,            // 已完成：NdDmaPadCountParamField（状态）
    NdDmaLoop0Stride = 132,         // 已完成：NdDmaLoopStrideParamField（状态）
    NdDmaLoop1Stride = 133,         // 已完成：NdDmaLoopStrideParamField（状态）
    NdDmaLoop2Stride = 134,         // 已完成：NdDmaLoopStrideParamField（状态）
    NdDmaLoop3Stride = 135,         // 已完成：NdDmaLoopStrideParamField（状态）
    NdDmaLoop4Stride = 136,         // 已完成：NdDmaLoopStrideParamField（状态）
    NdDmaOutToUbufB8 = 87,          // 已完成：NdDmaOutToUbufParamField
    NdDmaOutToUbufB16 = 88,         // 已完成：NdDmaOutToUbufParamField
    NdDmaOutToUbufB32 = 89,         // 已完成：NdDmaOutToUbufParamField
    Loop3Param = 90,                // 已完成：Loop3ParamField（状态）
    SetL12DB16 = 149,               // SET_L1_2D.b16
    SetL12DB32 = 150,               // SET_L1_2D.b32
    SetMte2NzPara = 399,            // 已完成：Mte2NzParamField（状态）

    // MTE3
    CopyUbufToGmAlignV2 = 83, // 已完成：CopyUbufToGmAlignV2ParamField
    CopyUbufToCbuf = 173,     // LocalMemoryTransferParamField：L1 WRITE
    CopyUbufToUbuf = 174,     // CopyUbufToUbufParamField: UB READ / WRITE

    // FIX
    FixL0cToOutF32 = 91,   // 已完成：FixL0cToOutParamField
    FixL0cToOutS32 = 92,   // 已完成：FixL0cToOutParamField
    CopyCbufToFbuf = 167,  // LocalMemoryTransferParamField: L1 READ
    FixL0cToCbufF32 = 168, // LocalMemoryTransferParamField：L0C READ、L1 WRITE
    FixL0cToCbufS32 = 169, // LocalMemoryTransferParamField：L0C READ、L1 WRITE
    FixL0cToUbufF32 = 170, // LocalMemoryTransferParamField：L0C READ
    FixL0cToUbufS32 = 171, // LocalMemoryTransferParamField：L0C READ

    // REGISTER
    SetCtrl = 159,             // SET_CTRL
    SetPadding = 392,          // SET_PADDING
    LoopSizeGmToCbuf = 394,    // 已完成：DmaLoopSizeParamField（状态）
    Loop1StrideGmToCbuf = 395, // 已完成：DmaLoopStrideParamField（状态）
    Loop2StrideGmToCbuf = 396, // 已完成：DmaLoopStrideParamField（状态）

    // SYNCCHECK
    PipeBarrier = 439,
    SetFlag = 440,   // 已完成：FlagParamField
    SetFlagI = 441,  // 已完成：FlagParamField
    WaitFlag = 442,  // 已完成：FlagParamField
    WaitFlagI = 443, // 已完成：FlagParamField
    WaitFlagDev = 445,
    WaitFlagDevI = 446,
    FftsCrossCoreSync = 447,
    GetBuf = 448,  // 已完成：SyncBufParamField
    GetBufI = 449, // 已完成：SyncBufParamField
    RlsBuf = 450,  // 已完成：SyncBufParamField
    RlsBufI = 451, // 已完成：SyncBufParamField
    SetIntraBlock = 452,
    SetIntraBlockI = 453,
    WaitIntraBlock = 454,
    WaitIntraBlockI = 455,
    SetFlagV = 456,   // 已完成：FlagParamField
    SetFlagIV = 457,  // 已完成：FlagParamField
    WaitFlagV = 458,  // 已完成：FlagParamField
    WaitFlagIV = 459, // 已完成：FlagParamField
    GetBufV = 460,    // 已完成：SyncBufParamField
    GetBufIV = 461,   // 已完成：SyncBufParamField
    RlsBufV = 462,    // 已完成：SyncBufParamField
    RlsBufIV = 463,   // 已完成：SyncBufParamField
    SetIntraBlockV = 464,
    SetIntraBlockIV = 465,
    WaitIntraBlockV = 466,
    WaitIntraBlockIV = 467,
    FftsCrossCoreSyncV = 468,
    WaitFlagDevV = 469,
    WaitFlagDevIV = 470,
    // Cross-core hardware flag synchronization. These IDs are emitted by the
    // DBI probes for HSET_FLAG/HWAIT_FLAG and their immediate forms.
    HSetFlag = 471,
    HSetFlagI = 472,
    HWaitFlag = 473,
    HWaitFlagI = 474,
};

constexpr bool IsDefinedInstructionId(uint32_t instructionId) noexcept
{
    if ((instructionId >= static_cast<uint32_t>(InstructionId::StB64Imm) &&
         instructionId <= static_cast<uint32_t>(InstructionId::LdpB8)) ||
        (instructionId >= 137 && instructionId <= 148) || (instructionId >= 153 && instructionId <= 157) ||
        instructionId == 422 || instructionId == 386 || instructionId == 387 || instructionId == 390 ||
        instructionId == 391 || instructionId == 151 || instructionId == 152 || instructionId == 158 ||
        (instructionId >= 400 && instructionId <= 415)) {
        return true;
    }
    switch (static_cast<InstructionId>(instructionId)) {
        case InstructionId::PipeBarrier:
        case InstructionId::StAtomicB32:
        case InstructionId::StAtomicB16:
        case InstructionId::StAtomicB8:
        case InstructionId::StiAtomicB32:
        case InstructionId::StiAtomicB16:
        case InstructionId::StiAtomicB8:
        case InstructionId::DcPreload:
        case InstructionId::DcPreloadI:
        case InstructionId::StDevB64:
        case InstructionId::StDevB32:
        case InstructionId::StDevB16:
        case InstructionId::StDevB8:
        case InstructionId::LdDevB64:
        case InstructionId::LdDevB32:
        case InstructionId::LdDevB16:
        case InstructionId::LdDevB8:
        case InstructionId::LoadGmToCbuf2DV2:
        case InstructionId::CopyGmToCbufV2:
        case InstructionId::CopyGmToCbufAlignV2B8:
        case InstructionId::CopyGmToCbufAlignV2B16:
        case InstructionId::CopyGmToCbufAlignV2B32:
        case InstructionId::CopyGmToCbufMultiNd2NzB8:
        case InstructionId::CopyGmToCbufMultiNd2NzB16:
        case InstructionId::CopyGmToCbufMultiNd2NzB32:
        case InstructionId::CopyGmToCbufMultiDn2NzB8:
        case InstructionId::CopyGmToCbufMultiDn2NzB16:
        case InstructionId::CopyGmToCbufMultiDn2NzB32:
        case InstructionId::CopyGmToUbufAlignV2B8:
        case InstructionId::CopyGmToUbufAlignV2B16:
        case InstructionId::CopyGmToUbufAlignV2B32:
        case InstructionId::Mte2SrcPara:
        case InstructionId::LoopSizeUbufToGm:
        case InstructionId::Loop1StrideUbufToGm:
        case InstructionId::Loop2StrideUbufToGm:
        case InstructionId::LoopSizeGmToUbuf:
        case InstructionId::Loop1StrideGmToUbuf:
        case InstructionId::Loop2StrideGmToUbuf:
        case InstructionId::NdDmaPadCount:
        case InstructionId::NdDmaLoop0Stride:
        case InstructionId::NdDmaLoop1Stride:
        case InstructionId::NdDmaLoop2Stride:
        case InstructionId::NdDmaLoop3Stride:
        case InstructionId::NdDmaLoop4Stride:
        case InstructionId::NdDmaOutToUbufB8:
        case InstructionId::NdDmaOutToUbufB16:
        case InstructionId::NdDmaOutToUbufB32:
        case InstructionId::Loop3Param:
        case InstructionId::SetL12DB16:
        case InstructionId::SetL12DB32:
        case InstructionId::SetMte2NzPara:
        case InstructionId::CopyUbufToGmAlignV2:
        case InstructionId::CopyUbufToCbuf:
        case InstructionId::CopyUbufToUbuf:
        case InstructionId::FixL0cToOutF32:
        case InstructionId::FixL0cToOutS32:
        case InstructionId::CopyCbufToFbuf:
        case InstructionId::CopyCbufToBtF32:
        case InstructionId::CopyCbufToBtS32:
        case InstructionId::CopyCbufToBtF16:
        case InstructionId::CopyCbufToBtBf16:
        case InstructionId::FixL0cToCbufF32:
        case InstructionId::FixL0cToCbufS32:
        case InstructionId::FixL0cToUbufF32:
        case InstructionId::FixL0cToUbufS32:
        case InstructionId::SetCtrl:
        case InstructionId::SetPadding:
        case InstructionId::LoopSizeGmToCbuf:
        case InstructionId::Loop1StrideGmToCbuf:
        case InstructionId::Loop2StrideGmToCbuf:
        case InstructionId::SetFlag:
        case InstructionId::SetFlagI:
        case InstructionId::WaitFlag:
        case InstructionId::WaitFlagI:
        case InstructionId::WaitFlagDev:
        case InstructionId::WaitFlagDevI:
        case InstructionId::FftsCrossCoreSync:
        case InstructionId::GetBuf:
        case InstructionId::GetBufI:
        case InstructionId::RlsBuf:
        case InstructionId::RlsBufI:
        case InstructionId::SetIntraBlock:
        case InstructionId::SetIntraBlockI:
        case InstructionId::WaitIntraBlock:
        case InstructionId::WaitIntraBlockI:
        case InstructionId::SetFlagV:
        case InstructionId::SetFlagIV:
        case InstructionId::WaitFlagV:
        case InstructionId::WaitFlagIV:
        case InstructionId::GetBufV:
        case InstructionId::GetBufIV:
        case InstructionId::RlsBufV:
        case InstructionId::RlsBufIV:
        case InstructionId::SetIntraBlockV:
        case InstructionId::SetIntraBlockIV:
        case InstructionId::WaitIntraBlockV:
        case InstructionId::WaitIntraBlockIV:
        case InstructionId::FftsCrossCoreSyncV:
        case InstructionId::WaitFlagDevV:
        case InstructionId::WaitFlagDevIV:
        case InstructionId::HSetFlag:
        case InstructionId::HSetFlagI:
        case InstructionId::HWaitFlag:
        case InstructionId::HWaitFlagI:
            return true;
        default:
            return false;
    }
}

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DEVICE_INSTR_COMMON_INSTRUCTION_ID_H
