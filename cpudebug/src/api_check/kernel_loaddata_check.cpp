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
 * \file kernel_loaddata_check.cpp
 * \brief
 */

#include "kernel_loaddata_check.h"
#include "kernel_utils.h"
#include "kernel_check_params.h"

namespace AscendC {
namespace check {
bool TikcppLoaddata2dCheck::CheckAllHighLevel()
{
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::L1)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)))) {
        ASCENDC_CHECK_TPOSITION(
            (false), "dst", "L0A Buffer(A2)/L0B Buffer(B2)", "LoadData",
            GetPositionDisplay(static_cast<TPosition>(param_.dstLogicPos)));
        return false;
    }
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::GM)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L1)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)))) {
        ASCENDC_CHECK_TPOSITION(
            (false), "dst", "L1 Buffer(A1/B1)/L0A Buffer(A2)/L0B Buffer(B2)", "LoadData",
            GetPositionDisplay(static_cast<TPosition>(param_.dstLogicPos)));
        return false;
    }
    if ((param_.srcPos != static_cast<uint8_t>(HardWareIndex::L1)) &&
        (param_.srcPos != static_cast<uint8_t>(HardWareIndex::GM))) {
        ASCENDC_CHECK_TPOSITION(
            (false), "src", "L1 Buffer(A1/B1)/GM", "LoadData",
            GetPositionDisplay(static_cast<TPosition>(param_.srcLogicPos)));
        return false;
    }
    if (param_.srcPos != static_cast<uint8_t>(HardWareIndex::GM)) {
        ASCENDC_CHECK(CheckBufferSizeOverFlow(
            param_.srcSize, GlobalParams::Instance().bufferSizeMap.at(param_.srcPos),
            "check src tensor buffersize failed"));
    }
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    // unit element
    if (param_.srcDtypeBytes == 0 || param_.dstDtypeBytes == 0) {
        CHECK_LOG_ERROR("src/dst dtype bytes is zeros");
        return false;
    }
    int32_t dataLen = (param_.repeatTimes - 1) * param_.srcStride + 1;
    int32_t srcLenElement = (param_.startIndex + dataLen) * BYTE_PER_FRACTAL / param_.srcDtypeBytes;
    int32_t dstLenElement =
        (param_.repeatTimes * BYTE_PER_FRACTAL + (param_.repeatTimes - 1) * param_.dstGap * BYTE_PER_FRACTAL) /
        param_.dstDtypeBytes;
    ASCENDC_CHECK(CheckTensorOverflowHigh(param_.dstDtypeBytes, param_.dstSize, dstLenElement, "dstLocal"));
    ASCENDC_CHECK(CheckTensorOverflowHigh(param_.srcDtypeBytes, param_.srcSize, srcLenElement, "srcLocal"));
    return true;
};

bool TikcppLoaddata2dv2Check::CheckAllHighLevel() const
{
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3102 || (__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::L1)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)))) {
        CHECK_LOG_ERROR("check dst tensor position failed,"
                        "the src hardware pos is L1, the dst hardware pos support L0A or L0B.");
        return false;
    }
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::GM)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L1)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)))) {
        CHECK_LOG_ERROR("check dst tensor position failed,"
                        "the src hardware pos is GM, the dst hardware pos support L1 or L0A or L0B.");
        return false;
    }
    if ((param_.srcPos != static_cast<uint8_t>(HardWareIndex::L1)) &&
        (param_.srcPos != static_cast<uint8_t>(HardWareIndex::GM))) {
        CHECK_LOG_ERROR("check src tensor position failed,"
                        "the src hardware pos support L1 or GM.");
        return false;
    }
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    if (param_.srcPos != static_cast<uint8_t>(HardWareIndex::GM)) {
        ASCENDC_CHECK(CheckBufferSizeOverFlow(
            param_.srcSize, GlobalParams::Instance().bufferSizeMap.at(param_.srcPos),
            "check src tensor buffersize failed"));
    }
    // unit element
    if (param_.srcDtypeBytes == 0 || param_.dstDtypeBytes == 0) {
        CHECK_LOG_ERROR("src/dst dtype bytes is zeros");
        return false;
    }
    return true;
#else
    CHECK_LOG_ERROR("Current version don't support LoadData2dv2");
    return false;
#endif
}

bool TikcppLoaddata3dv1Check::CheckAllHighLevel() const
{
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 2201) || (__NPU_ARCH__ == 3002) || (__NPU_ARCH__ == 3102) || \
                              (__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
    CHECK_LOG_ERROR("unsupport Loaddata3dv1");
    return false;
#else
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::L1)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::UB)))) {
        CHECK_LOG_ERROR("check dst tensor position failed,"
                        "the src hardware pos is L1, the dst hardware pos support L0A or L0B or UB.");
        return false;
    }

    ASCENDC_CHECK(CheckTensorScope(param_.srcLogicPos, static_cast<uint8_t>(HardWareIndex::L1), "srcLocal", "A1/B1"));

    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.srcSize, GlobalParams::Instance().bufferSizeMap.at(param_.srcPos),
        "check src tensor buffersize failed"));
    // unit element
    int32_t dstLenElement;
    if (param_.dstDtypeBytes == 0) {
        CHECK_LOG_ERROR("dst dtype bytes is zeros");
        return false;
    }
    if (param_.repeatMode == 0) {
        dstLenElement = param_.repeatTime * BYTE_PER_FRACTAL / param_.dstDtypeBytes;
    } else {
        dstLenElement =
            ((param_.repeatTime - 1) * param_.jumpStride * BYTE_PER_FRACTAL + BYTE_PER_FRACTAL) / param_.dstDtypeBytes;
    }
    ASCENDC_CHECK(CheckTensorOverflowHigh(param_.dstDtypeBytes, param_.dstSize, dstLenElement, "dstLocal"));
    return true;
#endif
};

bool TikcppLoaddata3dv2Check::CheckAllHighLevel()
{
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 2201) || (__NPU_ARCH__ == 3002) || (__NPU_ARCH__ == 3102) || \
                              (__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::L1)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)))) {
        CHECK_LOG_ERROR("check dst tensor position failed,"
                        "the src hardware pos is L1, the dst hardware pos support L0A or L0B.");
        return false;
    }
#elif defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 2002) || (__NPU_ARCH__ == 2201) || (__NPU_ARCH__ == 3002) || \
                                (__NPU_ARCH__ == 3102) || (__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::L1)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::UB)))) {
        CHECK_LOG_ERROR("check dst tensor position failed,"
                        "the src hardware pos is L1, the dst hardware pos support L0A or L0B or UB.");
        return false;
    }
#endif
    ASCENDC_CHECK(CheckTensorScope(param_.srcLogicPos, static_cast<uint8_t>(HardWareIndex::L1), "srcLocal", "A1/B1"));

    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.srcSize, GlobalParams::Instance().bufferSizeMap.at(param_.srcPos),
        "check src tensor buffersize failed"));
    // unit element
    int32_t dstLenElement = param_.mExtension * param_.kExtension;
    ASCENDC_CHECK(CheckTensorOverflowHigh(param_.dstDtypeBytes, param_.dstSize, dstLenElement, "dstLocal"));
    return true;
};

bool TikcppLoaddata3dv2ProCheck::CheckAllHighLevel()
{
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 2201) || (__NPU_ARCH__ == 3002) || (__NPU_ARCH__ == 3102) || \
                              (__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102))
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::L1)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)))) {
        CHECK_LOG_ERROR("check dst tensor position failed,"
                        "the src hardware pos is L1, the dst hardware pos support L0A or L0B.");
        return false;
    }
#elif defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 2002) || (__NPU_ARCH__ == 2201) || (__NPU_ARCH__ == 3002) || \
                                (__NPU_ARCH__ == 3102) || (__NPU_ARCH__ == 3510))
    if ((param_.srcPos == static_cast<uint8_t>(HardWareIndex::L1)) &&
        ((param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0A)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::L0B)) &&
         (param_.dstPos != static_cast<uint8_t>(HardWareIndex::UB)))) {
        CHECK_LOG_ERROR("check dst tensor position failed,"
                        "the src hardware pos is L1, the dst hardware pos support L0A or L0B or UB.");
        return false;
    }
#endif
    ASCENDC_CHECK(CheckTensorScope(param_.srcLogicPos, static_cast<uint8_t>(HardWareIndex::L1), "srcLocal", "A1/B1"));

    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.srcSize, GlobalParams::Instance().bufferSizeMap.at(param_.srcPos),
        "check src tensor buffersize failed"));
    // unit element
    int32_t dstLenElement = param_.mExtension * param_.kExtension;
    ASCENDC_CHECK(CheckTensorOverflowHigh(param_.dstDtypeBytes, param_.dstSize, dstLenElement, "dstLocal"));
    return true;
};

bool TikcppLoadImageToLocalCheck::CheckAllHighLevel()
{
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ != 3510) || (__NPU_ARCH__ == 5102))
    ASCENDC_CHECK(CheckTensorScope(param_.dstLogicPos, static_cast<uint8_t>(HardWareIndex::L1), "dstLocal", "A1/B1"));
#endif
    ASCENDC_CHECK(CheckTensorAddrAlign(param_.dstAddr, param_.dstPos, ONE_BLK_SIZE, "dst"));

    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "check dst tensor buffersize failed"));
    return true;
};
} // namespace check
} // namespace AscendC
