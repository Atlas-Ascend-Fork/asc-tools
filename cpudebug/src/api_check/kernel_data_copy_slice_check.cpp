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
 * \file kernel_data_copy_slice_check.cpp
 * \brief
 */

#include "kernel_check_params.h"
#include "kernel_data_copy_slice_check.h"

namespace AscendC {
namespace check {
bool TikcppDataCopySliceCheck::CheckSliceInfoParamters(const std::string& errMsg)
{
    for (uint32_t i = 0; i < param_.dimValue; i++) {
        if (param_.dstSliceInfo[i].burstLen != param_.srcSliceInfo[i].burstLen) {
            CHECK_LOG_ERROR(
                "%s, "
                "dstSliceInfo[%u]'s burst len (%u) should be equal to SliceInfo[%u]'s burst len (%u)",
                errMsg.c_str(), i, param_.dstSliceInfo[i].burstLen, i, param_.srcSliceInfo[i].burstLen);
            return false;
        }

        if (i > 0 && param_.dstSliceInfo[i].burstLen != 1) {
            CHECK_LOG_ERROR(
                "%s, "
                "dim[%u]'s dstSliceInfo's burst len should be equal to 1, but get (%u)",
                errMsg.c_str(), i, param_.dstSliceInfo[i].burstLen);
            return false;
        }

        if (param_.dstSliceInfo[i].startIndex >= param_.dstSliceInfo[i].endIndex) {
            CHECK_LOG_ERROR(
                "%s, "
                "dstSliceInfo[%u]'s start index (%u) should be less than dstSliceInfo[%u]'s end index (%u)",
                errMsg.c_str(), i, param_.dstSliceInfo[i].startIndex, i, param_.dstSliceInfo[i].endIndex);
            return false;
        }

        if (param_.dstSliceInfo[i].endIndex >= param_.dstShape[i]) {
            CHECK_LOG_ERROR(
                "%s, "
                "dstSliceInfo[%u]'s end index (%u) should be less than dstSliceInfo[%u]'s shape (%u)",
                errMsg.c_str(), i, param_.dstSliceInfo[i].endIndex, i, param_.dstShape[i]);
            return false;
        }

        if (param_.srcSliceInfo[i].startIndex >= param_.srcSliceInfo[i].endIndex) {
            CHECK_LOG_ERROR(
                "%s, "
                "srcSliceInfo[%u]'s start index (%u) should be less than srcSliceInfo[%u]'s end index (%u)",
                errMsg.c_str(), i, param_.srcSliceInfo[i].startIndex, i, param_.srcSliceInfo[i].endIndex);
            return false;
        }

        if (param_.srcSliceInfo[i].endIndex >= param_.srcShape[i]) {
            CHECK_LOG_ERROR(
                "%s, "
                "srcSliceInfo[%u]'s end index (%u) should be less than dst tensor dim[%u]'s shape (%u)",
                errMsg.c_str(), i, param_.srcSliceInfo[i].endIndex, i, param_.srcShape[i]);
            return false;
        }
    }
    return true;
}

uint32_t TikcppDataCopySliceCheck::DataCopyGetTotalInstrsNum(const SliceInfo sliceInfo[], const uint32_t shapeIn[])
{
    uint32_t sliceSize = 1;
    uint32_t currentCount = 0;
    uint32_t totalInstrsNum = 0;
    for (uint32_t i = 0; i < param_.dimValue; i++) {
        if (i == 0) {
            totalInstrsNum = 1;
        } else {
            sliceSize = sliceSize * shapeIn[i - 1];
            currentCount =
                (sliceInfo[i].endIndex - sliceInfo[i].startIndex + 1 + sliceInfo[i].stride) / (1 + sliceInfo[i].stride);
            totalInstrsNum = totalInstrsNum * currentCount;
        }
    }
    return totalInstrsNum;
}

bool TikcppDataCopySliceCheck::CheckDataCopyIntrsParamters(const std::string& errMsg)
{
    uint64_t oneStrideLen = 0;

    uint32_t oneSliceLen =
        param_.srcSliceInfo[0].burstLen * static_cast<uint32_t>(PlatFormParams::UB_BLOCK_SIZE) / param_.srcDtypeBytes +
        param_.srcSliceInfo[0].stride;
    uint32_t totalLen =
        param_.srcSliceInfo[0].endIndex - param_.srcSliceInfo[0].startIndex + 1 + param_.srcSliceInfo[0].stride;
    if (totalLen % oneSliceLen != 0) {
        CHECK_LOG_ERROR(
            "%s, "
            "construct datacopy intrs parameters block count failed, please check srcSliceInfo[0].burstLen(%u), "
            "srcSliceInfo[0].startIndex(%u), srcSliceInfo[0].endIndex(%u), srcSliceInfo[0].stride(%u)",
            errMsg.c_str(), param_.srcSliceInfo[0].burstLen, param_.srcSliceInfo[0].startIndex,
            param_.srcSliceInfo[0].endIndex, param_.srcSliceInfo[0].stride);
        return false;
    }

    if (param_.isGM2UB) {
        oneStrideLen = static_cast<uint64_t>(param_.dstSliceInfo[0].stride * param_.dstDtypeBytes);
        if (oneStrideLen % static_cast<uint64_t>(PlatFormParams::UB_BLOCK_SIZE) != 0) {
            CHECK_LOG_ERROR(
                "%s, "
                "one dst stride len (%lu) should be multiple of block size (%lu), "
                "dstSliceInfo[0].stride(%u), dstDtypeBytes(%u)",
                errMsg.c_str(), oneStrideLen, static_cast<uint64_t>(PlatFormParams::UB_BLOCK_SIZE),
                param_.dstSliceInfo[0].stride, param_.dstDtypeBytes);
            return false;
        }
    } else {
        oneStrideLen = static_cast<uint64_t>(param_.srcSliceInfo[0].stride * param_.srcDtypeBytes);
        if (oneStrideLen % static_cast<uint64_t>(PlatFormParams::UB_BLOCK_SIZE) != 0) {
            CHECK_LOG_ERROR(
                "%s, "
                "one src stride len (%lu) should be multiple of block size (%lu), "
                "srcSliceInfo[0].stride(%u), srcDtypeBytes(%u)",
                errMsg.c_str(), oneStrideLen, static_cast<uint64_t>(PlatFormParams::UB_BLOCK_SIZE),
                param_.srcSliceInfo[0].stride, param_.srcDtypeBytes);
            return false;
        }
    }
    return true;
}

bool TikcppDataCopySliceCheck::CheckDataCopyInstrsNum(const std::string& errMsg)
{
    uint32_t srcIntrsNum = DataCopyGetTotalInstrsNum(param_.srcSliceInfo, param_.srcShape);
    uint32_t dstIntrsNum = DataCopyGetTotalInstrsNum(param_.dstSliceInfo, param_.dstShape);
    if (srcIntrsNum != dstIntrsNum) {
        CHECK_LOG_ERROR(
            "%s, "
            "srcIntrsNum(%u) != dstIntrsNum(%u)",
            errMsg.c_str(), srcIntrsNum, dstIntrsNum);
        return false;
    }
    if (srcIntrsNum > MAX_SLICE_SIZE) {
        CHECK_LOG_ERROR(
            "%s, "
            "srcIntrsNum(%u) is bigger than MAX_SLICE_SIZE(%u), please reset the sliceInfo parameter",
            errMsg.c_str(), srcIntrsNum, MAX_SLICE_SIZE);
        return false;
    }
    return true;
}

bool TikcppDataCopySliceCheck::CheckAllHighLevel()
{
    const std::string supportPos = "VECIN/VECOUT/VECCALC";
    if (param_.isGM2UB) {
        ASCENDC_CHECK(CheckTensorScope(param_.logicPos, static_cast<uint8_t>(HardWareIndex::UB), "dst", supportPos));
    } else {
        ASCENDC_CHECK(CheckTensorScope(param_.logicPos, static_cast<uint8_t>(HardWareIndex::UB), "src", supportPos));
    }
    ASCENDC_CHECK(CheckDataCopyInstrsNum("check total number of instructions mov or mov_align failed"));
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.sizeNum, GlobalParams::Instance().bufferSizeMap.at(param_.pos), "check ub tensor buffersize failed"));
    ASCENDC_CHECK(CheckSliceInfoParamters("check sliceInfo paramters"));
    ASCENDC_CHECK(CheckDataCopyIntrsParamters("check data copy instr paramters"));
    ASCENDC_CHECK(CheckAddrAlign());
    return true;
}

bool TikcppDataCopySliceCheck::CheckAddrAlign()
{
    // gm is 1 Byte aligned, so do not need to check aligned
    uint64_t ubBaseAddr =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ConstDefiner::Instance().GetHardwareBaseAddr(Hardware::UB)));

    if (param_.isGM2UB) {
        uint64_t dstAbsPos = param_.dstAddr - ubBaseAddr;
        ASCENDC_CHECK_AND_LOG(((dstAbsPos % alignBytes_) == 0), {
            CHECK_LOG_ERROR(
                "instr %s dst addr is %lu, which should be %u B Aligned", apiName.c_str(), dstAbsPos, alignBytes_);
        });
    } else {
        uint64_t srcAbsPos = param_.srcAddr - ubBaseAddr;
        ASCENDC_CHECK_AND_LOG(((srcAbsPos % alignBytes_) == 0), {
            CHECK_LOG_ERROR(
                "instr %s src addr is %lu, which should be %u B Aligned", apiName.c_str(), srcAbsPos, alignBytes_);
        });
    }
    return true;
}
} // namespace check
} // namespace AscendC
