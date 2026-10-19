/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_device_data_log.h"
#include "npu_tool_log.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

namespace aclsan {

namespace {

template <typename ParamField>
constexpr bool HasParamFieldLogger() noexcept
{
    return std::is_same_v<ParamField, aclsan::CopyGmToUbufAlignV2ParamField> ||
           std::is_same_v<ParamField, aclsan::CopyGmToCbufAlignV2ParamField> ||
           std::is_same_v<ParamField, aclsan::CopyUbufToGmAlignV2ParamField> ||
           std::is_same_v<ParamField, aclsan::CopyGmToCbufV2ParamField> ||
           std::is_same_v<ParamField, aclsan::LoadGmToCbuf2DV2ParamField> ||
           std::is_same_v<ParamField, aclsan::NdDmaParamField> ||
           std::is_same_v<ParamField, aclsan::FixL0cToOutParamField> ||
           std::is_same_v<ParamField, aclsan::LocalMemoryTransferParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarGmParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarDevParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarPreloadParamField> ||
           std::is_same_v<ParamField, aclsan::ScalarAtomicParamField> ||
           std::is_same_v<ParamField, aclsan::MmadParamField> ||
           std::is_same_v<ParamField, aclsan::LoadL1MxParamField> ||
           std::is_same_v<ParamField, aclsan::SetL12DParamField> ||
           std::is_same_v<ParamField, aclsan::LoadCbufToL0ParamField> ||
           std::is_same_v<ParamField, aclsan::LoadCbufToCbTransposeParamField> ||
           std::is_same_v<ParamField, aclsan::Img2ColParamField> ||
           std::is_same_v<ParamField, aclsan::FmatrixParamField> ||
           std::is_same_v<ParamField, aclsan::L3dRptParamField> ||
           std::is_same_v<ParamField, aclsan::NdDmaPadCountParamField> ||
           std::is_same_v<ParamField, aclsan::Mte2SourceParamField> ||
           std::is_same_v<ParamField, aclsan::NdDmaLoopStrideParamField> ||
           std::is_same_v<ParamField, aclsan::Mte2NzParamField> ||
           std::is_same_v<ParamField, aclsan::Loop3ParamField> ||
           std::is_same_v<ParamField, aclsan::DmaLoopSizeParamField> ||
           std::is_same_v<ParamField, aclsan::DmaLoopStrideParamField> ||
           std::is_same_v<ParamField, aclsan::SetPaddingParamField> ||
           std::is_same_v<ParamField, aclsan::FlagParamField> ||
           std::is_same_v<ParamField, aclsan::SyncBufParamField> ||
           std::is_same_v<ParamField, aclsan::PipeBarrierParamField>;
}

const char* BlockTypeName(uint32_t blockType) noexcept
{
    switch (blockType) {
        case ACLSAN_DEVICE_BLOCK_TYPE_AICORE:
            return "AICORE";
        case ACLSAN_DEVICE_BLOCK_TYPE_AICORE_VECTOR:
            return "AIV";
        case ACLSAN_DEVICE_BLOCK_TYPE_AICORE_CUBE:
            return "AIC";
        default:
            return "UNKNOWN";
    }
}

void LogParamField(const aclsan::SetPaddingParamField& value) noexcept
{
    ASCTOOL_DEBUG("[param] type=SetPaddingParamField value=0x%llx", static_cast<unsigned long long>(value.value));
}

void LogParamField(const aclsan::LoadCbufToL0ParamField& f) noexcept
{
    ASCTOOL_DEBUG(
        "[param] load2d id=%u dst=0x%llx src=0x%llx mStart=%u kStart=%u mStep=%u kStep=%u "
        "srcStride=%u dstStride=%u transpose=%u",
        f.instrId, static_cast<unsigned long long>(f.dstAddr), static_cast<unsigned long long>(f.srcAddr),
        f.mStartPosition, f.kStartPosition, f.mStep, f.kStep, f.srcStride, f.dstStride, f.transpose);
}

void LogParamField(const aclsan::LoadCbufToCbTransposeParamField& f) noexcept
{
    ASCTOOL_DEBUG(
        "[param] load_transpose id=%u dst=0x%llx src=0x%llx index=%u repeat=%u "
        "srcStride=%u dstGap=%u srcFracGap=%u dstFracGap=%u decrement=%u",
        f.instrId, static_cast<unsigned long long>(f.dstAddr), static_cast<unsigned long long>(f.srcAddr), f.indexId,
        f.repeat, f.srcStride, f.dstGap, f.srcFracGap, f.dstFracGap, f.decrement);
}

void LogParamField(const aclsan::Img2ColParamField& f) noexcept
{
    ASCTOOL_DEBUG(
        "[param] img2col id=%u dst=0x%llx src=0x%llx m=%u k=%u mStart=%u kStart=%u "
        "channels=%u bank=%u transpose=%u",
        f.instrId, static_cast<unsigned long long>(f.dstAddr), static_cast<unsigned long long>(f.srcAddr), f.mExtension,
        f.kExtension, f.mStartPoint, f.kStartPoint, f.channelSize, f.fMatrixControl, f.transpose);
}

void LogParamField(const aclsan::FmatrixParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=FmatrixParamField instrId=%u width=%u height=%u "
        "paddingLeft=%u paddingRight=%u paddingTop=%u paddingBottom=%u",
        value.instrId, value.width, value.height, value.paddingLeft, value.paddingRight, value.paddingTop,
        value.paddingBottom);
}

void LogParamField(const aclsan::L3dRptParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=L3dRptParamField instrId=%u repeatStride=%u repeatTimes=%u "
        "repeatAlongK=%u dstStride=%u dstOffset=%u",
        value.instrId, value.repeatStride, value.repeatTimes, value.repeatAlongK ? 1U : 0U, value.dstStride,
        value.dstOffset);
}

void LogParamField(const aclsan::MmadParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=MmadParamField instrId=%u c=0x%llx a=0x%llx b=0x%llx "
        "m=%u k=%u n=%u disableGemv=%u cmatrixSource=%u cmatrixInitVal=%u",
        value.instrId, static_cast<unsigned long long>(value.dstAddr), static_cast<unsigned long long>(value.src0Addr),
        static_cast<unsigned long long>(value.src1Addr), value.m, value.k, value.n, value.disableGemv,
        value.cmatrixSource, value.cmatrixInitVal);
}

void LogParamField(const aclsan::LoadL1MxParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=LoadL1MxParamField instrId=%u src=0x%llx xStart=%u yStart=%u "
        "xStep=%u yStep=%u srcStride=%u",
        value.instrId, static_cast<unsigned long long>(value.srcAddr), value.xStart, value.yStart, value.xStep,
        value.yStep, value.srcStride);
}

void LogParamField(const aclsan::SetL12DParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=SetL12DParamField instrId=%u dst=0x%llx repeat=%u blocks=%u stride=%u", value.instrId,
        static_cast<unsigned long long>(value.dstAddr), value.repeatTimes, value.blockNum, value.repeatGap);
}

void LogParamField(const aclsan::NdDmaPadCountParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=NdDmaPadCountParamField left=[%u,%u,%u,%u] right=[%u,%u,%u,%u]", value.leftPaddingCounts[0],
        value.leftPaddingCounts[1], value.leftPaddingCounts[2], value.leftPaddingCounts[3], value.rightPaddingCounts[0],
        value.rightPaddingCounts[1], value.rightPaddingCounts[2], value.rightPaddingCounts[3]);
}

void LogParamField(const aclsan::Mte2SourceParamField& value) noexcept
{
    ASCTOOL_DEBUG("[param] type=Mte2SourceParamField srcStride=%lld", static_cast<long long>(value.srcStride));
}

void LogParamField(const aclsan::NdDmaLoopStrideParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=NdDmaLoopStrideParamField loopIndex=%u srcStride=%llu", value.loopIndex,
        static_cast<unsigned long long>(value.srcStride));
}

void LogParamField(const aclsan::Mte2NzParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=Mte2NzParamField matrixNum=%u loop2DstStride=%u loop3DstStride=%u loop4DstStride=%u",
        value.matrixNum, value.loop2DstStride, value.loop3DstStride, value.loop4DstStride);
}

void LogParamField(const aclsan::Loop3ParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=Loop3ParamField loopCount=%u srcStride=%u dstStride=%u", value.loopCount, value.srcStride,
        value.dstStride);
}

void LogParamField(const aclsan::DmaLoopSizeParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=DmaLoopSizeParamField direction=%u loop1Size=%u loop2Size=%llu",
        static_cast<unsigned int>(value.direction), value.loop1Size, static_cast<unsigned long long>(value.loop2Size));
}

void LogParamField(const aclsan::DmaLoopStrideParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=DmaLoopStrideParamField direction=%u loopIndex=%u srcStride=%llu dstStride=%llu",
        static_cast<unsigned int>(value.direction), value.loopIndex, static_cast<unsigned long long>(value.srcStride),
        static_cast<unsigned long long>(value.dstStride));
}

void LogParamField(const aclsan::CopyGmToUbufAlignV2ParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=CopyGmToUbufAlignV2ParamField instrId=%u dstAddr=0x%llx srcAddr=0x%llx "
        "sid=%u burstNum=%u burstLen=%u leftPaddingCount=%u rightPaddingCount=%u dataSelectBit=%u l2CacheControl=%u "
        "burstSrcStride=%llu burstDstStride=%u",
        value.instrId, static_cast<unsigned long long>(value.dstAddr), static_cast<unsigned long long>(value.srcAddr),
        value.sid, value.burstNum, value.burstLen, value.leftPaddingCount, value.rightPaddingCount,
        value.dataSelectBit ? 1U : 0U, value.l2CacheControl, static_cast<unsigned long long>(value.burstSrcStride),
        value.burstDstStride);
}

void LogParamField(const aclsan::CopyGmToCbufAlignV2ParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=CopyGmToCbufAlignV2ParamField instrId=%u dstAddr=0x%llx srcAddr=0x%llx "
        "sid=%u burstNum=%u burstLen=%u leftPaddingCount=%u rightPaddingCount=%u dataSelectBit=%u l2CacheControl=%u "
        "burstSrcStride=%llu burstDstStride=%u",
        value.instrId, static_cast<unsigned long long>(value.dstAddr), static_cast<unsigned long long>(value.srcAddr),
        value.sid, value.burstNum, value.burstLen, value.leftPaddingCount, value.rightPaddingCount,
        value.dataSelectBit ? 1U : 0U, value.l2CacheControl, static_cast<unsigned long long>(value.burstSrcStride),
        value.burstDstStride);
}

void LogParamField(const aclsan::CopyUbufToGmAlignV2ParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=CopyUbufToGmAlignV2ParamField instrId=%u dstAddr=0x%llx srcAddr=0x%llx "
        "sid=%u burstNum=%u burstLen=%u l2CacheControl=%u dstStride=%llu srcStride=%u",
        value.instrId, static_cast<unsigned long long>(value.dstAddr), static_cast<unsigned long long>(value.srcAddr),
        value.sid, value.burstNum, value.burstLen, value.l2CacheControl,
        static_cast<unsigned long long>(value.dstStride), value.srcStride);
}

void LogParamField(const aclsan::CopyGmToCbufV2ParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=CopyGmToCbufV2ParamField instrId=%u dstAddr=0x%llx srcAddr=0x%llx sid=%u "
        "burstNum=%u burstLen=%u padFunctionMode=%u l2CacheControl=%u srcStride=%llu dstStride=%u",
        value.instrId, static_cast<unsigned long long>(value.dstAddr), static_cast<unsigned long long>(value.srcAddr),
        value.sid, value.burstNum, value.burstLen, value.padFunctionMode, value.l2CacheControl,
        static_cast<unsigned long long>(value.srcStride), value.dstStride);
}

void LogParamField(const aclsan::LoadGmToCbuf2DV2ParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=LoadGmToCbuf2DV2ParamField instrId=%u dstAddr=0x%llx srcAddr=0x%llx "
        "mStartPosition=%u kStartPosition=%u dstStride=%u mStep=%u kStep=%u sid=%u decompMode=%u "
        "l2CacheControl=%u",
        value.instrId, static_cast<unsigned long long>(value.dstAddr), static_cast<unsigned long long>(value.srcAddr),
        value.mStartPosition, value.kStartPosition, value.dstStride, value.mStep, value.kStep, value.sid,
        value.decompMode, value.l2CacheControl);
}

void LogParamField(const aclsan::NdDmaParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=NdDmaParamField instrId=%u dataBits=%u dstAddr=0x%llx srcAddr=0x%llx sid=%u "
        "loopSizes=[%u,%u,%u,%u,%u] loop0LeftPaddingCount=%u loop0RightPaddingCount=%u "
        "paddingMode=%u l2CacheControl=%u",
        value.instrId, value.dataBits, static_cast<unsigned long long>(value.dstAddr),
        static_cast<unsigned long long>(value.srcAddr), value.sid, value.loop0Size, value.loop1Size, value.loop2Size,
        value.loop3Size, value.loop4Size, value.loop0LeftPaddingCount, value.loop0RightPaddingCount,
        value.paddingMode ? 1U : 0U, value.l2CacheControl);
}

void LogParamField(const aclsan::FixL0cToOutParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=FixL0cToOutParamField instrId=%u dataBits=%u dstAddr=0x%llx srcAddr=0x%llx sid=%u "
        "nSize=%u mSize=%u loopDstStride=%u loopSrtStride=%u l2CacheControl=%u clipReluPre=%u unitFlag=%u "
        "quantPre=%u reluPre=%u splitEnable=%u nz2ndEnable=%u quantPost=%u reluPost=%u clipReluPost=%u "
        "loopEnhanceEnable=%u eltwiseOp=%u eltwiseAntqEnable=%u loopEnhanceMergeEnable=%u c0PadEnable=%u "
        "winoPostEnable=%u brcbEnable=%u nz2dnEnable=%u",
        value.instrId, value.dataBits, static_cast<unsigned long long>(value.dstAddr),
        static_cast<unsigned long long>(value.srcAddr), value.sid, value.nSize, value.mSize, value.loopDstStride,
        value.loopSrtStride, value.l2CacheControl, value.clipReluPre, value.unitFlag, value.quantPre, value.reluPre,
        value.splitEnable, value.nz2ndEnable, value.quantPost, value.reluPost, value.clipReluPost,
        value.loopEnhanceEnable, value.eltwiseOp, value.eltwiseAntqEnable, value.loopEnhanceMergeEnable,
        value.c0PadEnable, value.winoPostEnable, value.brcbEnable, value.nz2dnEnable);
}

void LogParamField(const aclsan::LocalMemoryTransferParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=LocalMemoryTransferParamField instrId=%u dstAddr=0x%llx srcAddr=0x%llx "
        "config0=0x%llx config1=0x%llx kind=%u localOnly=1",
        value.instrId, static_cast<unsigned long long>(value.dstAddr), static_cast<unsigned long long>(value.srcAddr),
        static_cast<unsigned long long>(value.config0), static_cast<unsigned long long>(value.config1),
        static_cast<unsigned int>(value.kind));
}

void LogParamField(const aclsan::ScalarDevParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=ScalarDevParamField instrId=%u dataBits=%u addr=0x%llx offset=%lld", value.instrId,
        value.dataBits, static_cast<unsigned long long>(value.addr), static_cast<long long>(value.offset));
}

void LogParamField(const aclsan::ScalarGmParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=ScalarGmParamField instrId=%u dataBits=%u addr=0x%llx offset=%lld post=%llu "
        "sysVaBase=0x%llx addressContext=0x%llx",
        value.instrId, value.dataBits, static_cast<unsigned long long>(value.addr),
        static_cast<long long>(value.offset), static_cast<unsigned long long>(value.post),
        static_cast<unsigned long long>(value.sysVaBase), static_cast<unsigned long long>(value.addressContext));
}

void LogParamField(const aclsan::ScalarPreloadParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=ScalarPreloadParamField instrId=%u addr=0x%llx offset=%lld", value.instrId,
        static_cast<unsigned long long>(value.addr), static_cast<long long>(value.offset));
}

void LogParamField(const aclsan::ScalarAtomicParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=ScalarAtomicParamField instrId=%u dataBits=%u addr=0x%llx offset=%lld post=%llu "
        "sysVaBase=0x%llx addressContext=0x%llx",
        value.instrId, value.dataBits, static_cast<unsigned long long>(value.addr),
        static_cast<long long>(value.offset), static_cast<unsigned long long>(value.post),
        static_cast<unsigned long long>(value.sysVaBase), static_cast<unsigned long long>(value.addressContext));
}

void LogParamField(const aclsan::FlagParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=FlagParamField instrId=%u srcPipe=%u dstPipe=%u eventId=%llu", value.instrId, value.srcPipe,
        value.dstPipe, static_cast<unsigned long long>(value.eventId));
}

void LogParamField(const aclsan::PipeBarrierParamField& value) noexcept
{
    ASCTOOL_DEBUG("[param] type=PipeBarrierParamField instrId=%u pipe=%u", value.instrId, value.pipe);
}

void LogParamField(const aclsan::SyncBufParamField& value) noexcept
{
    ASCTOOL_DEBUG(
        "[param] type=SyncBufParamField instrId=%u pipe=%u bufId=0x%llx mode=%u", value.instrId, value.pipe,
        static_cast<unsigned long long>(value.bufId), static_cast<uint32_t>(value.mode));
}

void LogMemoryAccessData(const AclsanDeviceMemoryAccessData& value, uint32_t index) noexcept
{
    const uint64_t rangeBytes = value.layoutKind == ACLSAN_MEM_LAYOUT_RANGE ? value.layout.range.bytes : 0;
    ASCTOOL_DEBUG(
        "[cbdata] type=AclsanDeviceMemoryAccessData index=%u address=0x%llx memorySpace=%u accessMode=%u "
        "accessIndex=%u accessCount=%u dataBits=%u bytes=%llu layoutKind=%u pc=0x%llx siteId=%u "
        "deviceId=%u phyCoreId=%u blockId=%u blockType=%s instrExecId=%llu launchId=%llu serialNo=%llu pipeline=%u",
        index, static_cast<unsigned long long>(value.address), value.memorySpace, value.accessMode, value.accessIndex,
        value.accessCount, value.dataBits, static_cast<unsigned long long>(rangeBytes), value.layoutKind,
        static_cast<unsigned long long>(value.header.pc), value.header.siteId, value.header.deviceId,
        value.header.phyCoreId, value.header.blockId, BlockTypeName(value.header.blockType),
        static_cast<unsigned long long>(value.header.instrExecId),
        static_cast<unsigned long long>(value.header.launchId), static_cast<unsigned long long>(value.header.serialNo),
        value.header.pipeline);

    if (value.layoutKind == ACLSAN_MEM_LAYOUT_RANGE) {
        ASCTOOL_DEBUG("[cbdata] layout=range bytes=%llu", static_cast<unsigned long long>(value.layout.range.bytes));
    } else if (value.layoutKind == ACLSAN_MEM_LAYOUT_BLOCK_REPEAT) {
        ASCTOOL_DEBUG(
            "[cbdata] layout=block_repeat blockNum=%u blockSize=%u blockStride=%lld repeatTimes=%u "
            "repeatStride=%lld",
            value.layout.blockRepeat.blockNum, value.layout.blockRepeat.blockSize,
            static_cast<long long>(value.layout.blockRepeat.blockStride), value.layout.blockRepeat.repeatTimes,
            static_cast<long long>(value.layout.blockRepeat.repeatStride));
    }
}

void LogCallbackData(const DeviceMemoryAccessDataList& memory) noexcept
{
    for (std::size_t index = 0; index < memory.size(); ++index) {
        LogMemoryAccessData(memory[index], static_cast<uint32_t>(index));
    }
}

void LogCallbackData(const AclsanDeviceSyncData& sync) noexcept
{
    ASCTOOL_DEBUG(
        "[cbdata] deviceId=%u phyCoreId=%u blockId=%u blockType=%s  instrExecId=%llu launchId=%llu  "
        "type=AclsanDeviceSyncData pc=0x%llx serialNo=%llu syncKind=%u action=%u scope=%u srcPipe=%u "
        "dstPipe=%u mode=%u objectId=%llu",
        sync.header.deviceId, sync.header.phyCoreId, sync.header.blockId, BlockTypeName(sync.header.blockType),
        static_cast<unsigned long long>(sync.header.instrExecId), static_cast<unsigned long long>(sync.header.launchId),
        static_cast<unsigned long long>(sync.header.pc), static_cast<unsigned long long>(sync.header.serialNo),
        sync.syncKind, sync.action, sync.scope, sync.srcPipe, sync.dstPipe, sync.mode,
        static_cast<unsigned long long>(sync.objectId));
}

} // namespace

void LogRawRecord(const ParsedTraceRecord& parsed) noexcept
{
    const AclsanRawTraceRecord& record = parsed.record;
    ASCTOOL_DEBUG(
        "[raw] deviceId=%u phyCoreId=%u blockId=%u blockType=%s  instrExecId=%llu launchId=%llu  "
        "type=AclsanRawTraceRecord pc=0x%llx instrId=%u siteId=%u category=%u pipeline=%u "
        "args=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx] parameterBase=0x%llx parameterBytes=%llu",
        parsed.deviceId, parsed.phyCoreId, parsed.blockId, BlockTypeName(parsed.blockType),
        static_cast<unsigned long long>(parsed.instrExecId), static_cast<unsigned long long>(parsed.launchId),
        static_cast<unsigned long long>(record.pc), record.instrId, record.siteId,
        static_cast<uint32_t>(record.category), record.pipeline, static_cast<unsigned long long>(record.args[0]),
        static_cast<unsigned long long>(record.args[1]), static_cast<unsigned long long>(record.args[2]),
        static_cast<unsigned long long>(record.args[3]), static_cast<unsigned long long>(record.args[4]),
        static_cast<unsigned long long>(record.parameterBase), static_cast<unsigned long long>(parsed.parameterBytes));
}

void LogParamField(const aclsan::DeviceInstructionParamField& params) noexcept
{
    std::visit(
        [](const auto& value) noexcept {
            using ParamField = std::decay_t<decltype(value)>;
            if constexpr (HasParamFieldLogger<ParamField>()) {
                LogParamField(value);
            }
        },
        params);
}

void LogCallbackData(const AclsanDeviceBarrierData& barrier) noexcept
{
    const auto& h = barrier.header;
    ASCTOOL_DEBUG(
        "[cbdata] type=AclsanDeviceBarrierData deviceId=%u phyCoreId=%u blockId=%u blockType=%s "
        "instrExecId=%llu launchId=%llu pc=0x%llx pipeline=%u pipe=%u",
        h.deviceId, h.phyCoreId, h.blockId, BlockTypeName(h.blockType), static_cast<unsigned long long>(h.instrExecId),
        static_cast<unsigned long long>(h.launchId), static_cast<unsigned long long>(h.pc), h.pipeline, barrier.pipe);
}

void LogCallbackData(const DeviceCallbackData& callbackData) noexcept
{
    std::visit([](const auto& value) noexcept { LogCallbackData(value); }, callbackData);
}

} // namespace aclsan
