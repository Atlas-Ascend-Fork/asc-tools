/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file kernel_vec_reduce_check.cpp
 * \brief
 */

#include "kernel_check_params.h"
#include "kernel_vec_reduce_check.h"

namespace AscendC {
namespace check {
uint32_t TikcppVecReduceCheck::AlignStartPos(const uint32_t startPos, const uint32_t byteLen) const
{
    if (byteLen == 0) {
        CHECK_LOG_ERROR("byteLen is %u, it should be greater than 0", byteLen);
        return 0;
    }
    uint32_t resDiv = DivCeil(startPos * byteLen, static_cast<uint32_t>(PlatFormParams::ONE_BLK_SIZE));

    return resDiv * static_cast<uint32_t>(PlatFormParams::ONE_BLK_SIZE) / byteLen;
}

bool TikcppVecReduceCheck::CheckAllDtypeBytes(const std::string& errMsg)
{
    uint32_t dstDtypeBytes = param_.dstDtypeBytes;
    uint32_t srcDtypeBytes = param_.src0DtypeBytes;
    uint32_t workDtypeBytes = param_.src1DtypeBytes;
    if ((dstDtypeBytes != srcDtypeBytes) || (srcDtypeBytes != workDtypeBytes) || (workDtypeBytes != dstDtypeBytes)) {
        CHECK_LOG_ERROR(
            "%s, "
            "Reduce need dst data type (%u),dst src type (%u), dst work type (%u) should be same",
            errMsg.c_str(), dstDtypeBytes, srcDtypeBytes, workDtypeBytes);
        return false;
    }
    return true;
}

void TikcppVecReduceCheck::ReduceBodyCal(
    const std::vector<uint32_t>& paramsArray, uint32_t& outputCount, uint32_t& nextStartPos) const
{
    enum class ReduceBodyCalIndex {
        PRE_DATA_COUNT = 0,
        CUR_START_POS,
        ELEMENT_NUM_PER_REP,
        TYPE_SIZE,
        PER_REP_OUTPUT,
    };

    uint32_t preDataCount = paramsArray[static_cast<uint32_t>(ReduceBodyCalIndex::PRE_DATA_COUNT)];
    uint32_t curStartPos = paramsArray[static_cast<uint32_t>(ReduceBodyCalIndex::CUR_START_POS)];
    uint32_t elementNumPerRep = paramsArray[static_cast<uint32_t>(ReduceBodyCalIndex::ELEMENT_NUM_PER_REP)];
    uint32_t typeSize = paramsArray[static_cast<uint32_t>(ReduceBodyCalIndex::TYPE_SIZE)];
    uint32_t perRepOutput = paramsArray[static_cast<uint32_t>(ReduceBodyCalIndex::PER_REP_OUTPUT)];

    uint32_t tailOutputCount;
    uint32_t bodyRepTimes = preDataCount / elementNumPerRep;
    uint32_t bodyOutputCount = perRepOutput * bodyRepTimes;
    bool hasTail = (preDataCount % elementNumPerRep) != 0;
    if (hasTail) {
        tailOutputCount = perRepOutput;
    } else {
        tailOutputCount = 0;
    }
    outputCount = bodyOutputCount + tailOutputCount;
    nextStartPos = AlignStartPos(curStartPos + outputCount, typeSize);
    return;
}

bool TikcppVecReduceCheck::CheckCheckWorkSize(
    const std::string& errMsg, const uint64_t needElements, const uint32_t byteLen)
{
    uint64_t needSize = static_cast<uint64_t>(needElements * byteLen);
    if (needSize > param_.src1Size) {
        CHECK_LOG_ERROR(
            "%s, "
            "Worktensor's size should be more than %lu, but get %lu",
            errMsg.c_str(), needSize, param_.src1Size);
        return false;
    }
    return true;
}

bool TikcppVecReduceCheck::CheckWorkTensorOffset(const std::string& errMsg)
{
    uint32_t resIndex;
    uint32_t it2OutputCount;
    uint32_t it3StartPos;
    uint32_t typeSize = param_.src1DtypeBytes;
    uint32_t perRepOutput = static_cast<uint32_t>(ReduceCheckExtParams::VREDUCE_PER_REP_OUTPUT);
    uint32_t it1AlignStart = 0;
    uint32_t it1OutputCount = perRepOutput * param_.repeatTimes; // 2
    uint64_t needElement = static_cast<uint32_t>(perRepOutput * param_.repeatTimes);

    if (!param_.calIndex) {
        return CheckCheckWorkSize(errMsg, needElement, typeSize);
    }

    // iteration1
    if (it1OutputCount == perRepOutput) {
        resIndex = it1AlignStart + it1OutputCount;
        needElement = resIndex;
        return CheckCheckWorkSize(errMsg, needElement, typeSize);
    }

    if (typeSize == 0) {
        CHECK_LOG_ERROR("dtype byte size is zero");
        return false;
    }

    // iteration2
    uint32_t it2AlignStart = AlignStartPos(it1OutputCount, typeSize);
    uint32_t elementNumPerRep = static_cast<uint32_t>(PlatFormParams::ONE_REP_BYTE_SIZE) / typeSize;
    if (elementNumPerRep == 0) {
        CHECK_LOG_ERROR(
            "%s, "
            "elementNumPerRep can not be 0.",
            errMsg.c_str());
        return false;
    }
    ReduceBodyCal(
        {it1OutputCount, it2AlignStart, elementNumPerRep, typeSize, perRepOutput}, it2OutputCount, it3StartPos);

    if (it2OutputCount == perRepOutput) {
        it3StartPos = it2AlignStart;
        resIndex = it3StartPos + 1;
    } else {
        // iteration3
        resIndex = it3StartPos + 1;
        if (it2OutputCount > elementNumPerRep) {
            uint32_t tmpVal;
            uint32_t it4StartPos;
            ReduceBodyCal({it2OutputCount, it3StartPos, elementNumPerRep, typeSize, perRepOutput}, tmpVal, it4StartPos);
            resIndex = it4StartPos + 1;
        }
    }
    needElement = resIndex + 1;
    return CheckCheckWorkSize(errMsg, needElement, typeSize);
}

bool TikcppVecReduceCheck::CheckWorkTensorSizeEqual(const std::string& errMsg)
{
    uint64_t needSize = static_cast<uint64_t>(param_.repeatTimes * param_.src1DtypeBytes);
    if (needSize > param_.src1Size) {
        CHECK_LOG_ERROR(
            "%s, "
            "Need size: %lu, while tensor size is %lu",
            errMsg.c_str(), needSize, param_.src1Size);
        return false;
    }

    return true;
}

bool TikcppVecReduceCheck::CheckDstTensorSizeRange(const std::string& errMsg)
{
    uint32_t needCount = 1;
    uint64_t needSize = 0;
    if (param_.calIndex) {
        needCount = static_cast<uint32_t>(ReduceCheckExtParams::VREDUCE_CALL_INDEX_COUNT);
    }
    needSize = static_cast<uint64_t>(needCount * param_.dstDtypeBytes);

    if (needSize > param_.dstSize) {
        CHECK_LOG_ERROR(
            "%s, "
            "Need least output size: %lu, while tensor size is %lu",
            errMsg.c_str(), needSize, param_.dstSize);
        return false;
    }
    return true;
}

bool TikcppVecReduceCheck::CheckAddrAlign()
{
    bool srcRes = CheckTensorAddrAlign(param_.src0Addr, param_.src0Pos, ONE_BLK_SIZE, "src");
    bool dstRes = CheckTensorAddrAlign(param_.dstAddr, param_.dstPos, param_.dstDtypeBytes, "dst");
    bool src1Res = CheckTensorAddrAlign(param_.src1Addr, param_.src1Pos, param_.dstDtypeBytes, "work");
    return srcRes && dstRes && src1Res;
}

bool TikcppVecReduceCheck::CommonCheck()
{
    ASCENDC_CHECK(CheckAllDtypeBytes("Check Reduce data type"));
    ASCENDC_CHECK(CheckDstTensorSizeRange("Check Reduce dst data size"));

    const std::string supportPos = "VECIN/VECOUT/VECCALC";
    ASCENDC_CHECK(CheckTensorScope(param_.dstLogicPos, static_cast<uint8_t>(HardWareIndex::UB), "dst", supportPos));
    ASCENDC_CHECK(CheckTensorScope(param_.src0LogicPos, static_cast<uint8_t>(HardWareIndex::UB), "src", supportPos));
    ASCENDC_CHECK(CheckTensorScope(param_.src1LogicPos, static_cast<uint8_t>(HardWareIndex::UB), "work", supportPos));

    ASCENDC_CHECK(CheckAddrAlign());

    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.src0Size, GlobalParams::Instance().bufferSizeMap.at(param_.src0Pos),
        "check src tensor buffersize failed"));
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.src1Size, GlobalParams::Instance().bufferSizeMap.at(param_.src1Pos),
        "check work tensor buffersize failed"));
    return true;
}

bool TikcppVecReduceCheck::CheckAllHighLevel()
{
    // Only for reduce interface level 2
    ASCENDC_CHECK(CommonCheck());

    ASCENDC_CHECK(CheckTensorOverflowHigh(param_.src0DtypeBytes, param_.src0Size, param_.calCount, "src0Local"));
    if (apiName == "ReduceSum") {
        ASCENDC_CHECK(CheckWorkTensorSizeEqual("Check Reduce Sum workLocal tensor size"));
    } else if (apiName == "ReduceMax") {
        ASCENDC_CHECK(CheckWorkTensorOffset("Check Reduce max workLocal tensor size"));
    } else {
        ASCENDC_CHECK(CheckWorkTensorOffset("Check Reduce min workLocal tensor size"));
    }
    return true;
}

bool TikcppVecReduceCheck::CheckAllHighLevelMode2()
{
    uint32_t dstDtypeBytes = param_.dstDtypeBytes;
    uint32_t srcDtypeBytes = param_.src0DtypeBytes;
    if (dstDtypeBytes != srcDtypeBytes) {
        CHECK_LOG_ERROR(
            "Check Reduce data type, Reduce need dst data type (%u), src data type (%u) should be same", dstDtypeBytes,
            srcDtypeBytes);
        return false;
    }

    ASCENDC_CHECK(CheckDstTensorSizeRange("Check Reduce dst data size"));

    const std::string supportPos = "VECIN/VECOUT/VECCALC";
    ASCENDC_CHECK(CheckTensorScope(param_.dstLogicPos, static_cast<uint8_t>(HardWareIndex::UB), "dst", supportPos));
    ASCENDC_CHECK(CheckTensorScope(param_.src0LogicPos, static_cast<uint8_t>(HardWareIndex::UB), "src", supportPos));

    bool srcRes = CheckTensorAddrAlign(param_.src0Addr, param_.src0Pos, ONE_BLK_SIZE, "src");
    bool dstRes = CheckTensorAddrAlign(param_.dstAddr, param_.dstPos, param_.dstDtypeBytes, "dst");
    ASCENDC_CHECK(srcRes && dstRes);

    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.src0Size, GlobalParams::Instance().bufferSizeMap.at(param_.src0Pos),
        "check src tensor buffersize failed"));
    ASCENDC_CHECK(CheckTensorOverflowHigh(param_.src0DtypeBytes, param_.src0Size, param_.calCount, "src0Local"));

    return true;
}

bool TikcppVecReduceCheck::CheckAllLowLevel(std::vector<uint64_t> maskArray)
{
    uint32_t maxByteLen = param_.dstDtypeBytes;
    ASCENDC_CHECK(UpdateMaskArrayAndCheck(maskArray, maxByteLen));
    ASCENDC_CHECK(CommonCheck());

    TensorOverflowParams params = {
        param_.src0Size,
        param_.src0DtypeBytes,
        static_cast<uint64_t>(param_.repeatTimes),
        static_cast<uint64_t>(param_.src0BlockStride),
        static_cast<uint64_t>(param_.src0RepeatStride),
        false};
    ASCENDC_CHECK(CheckTensorOverflowLow(maskArray, params, "src0Local"));
    if (apiName == "ReduceSum") {
        ASCENDC_CHECK(CheckWorkTensorSizeEqual("Check Reduce sum workLocal tensor size"));
    } else if (apiName == "ReduceMax") {
        ASCENDC_CHECK(CheckWorkTensorOffset("Check Reduce max workLocal tensor size"));
    } else {
        ASCENDC_CHECK(CheckWorkTensorOffset("Check Reduce min workLocal tensor size"));
    }
    return true;
}
} // namespace check
} // namespace AscendC
