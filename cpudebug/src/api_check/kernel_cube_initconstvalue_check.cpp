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
 * \file kernel_cube_initconstvalue_check.cpp
 * \brief
 */

#include "kernel_check_params.h"
#include "kernel_cube_initconstvalue_check.h"

namespace AscendC {
namespace check {
bool TikcppCubeInitConstValueCheck::CheckAllLowLevel()
{
    uint16_t repeatTimes = param_.repeatTimes;
    uint16_t blockNum = param_.blockNum;
    uint16_t dstGap = param_.dstGap;

    ASCENDC_CHECK(CheckBufferSizeOverFlow(
        param_.dstSize, GlobalParams::Instance().bufferSizeMap.at(param_.dstPos),
        "Failed to check dstLocal tensor buffersize in InitConstValue"));
    // repeatTimes + block + dstGap not exceed dstLocal
#if defined(__NPU_ARCH__) && __NPU_ARCH__ == 2002
    uint32_t blockLen = 512; //  310P fix one repeat is 512B
#else
    uint32_t blockLen = 32; // one block length is 32B
    if (param_.dstPos == static_cast<uint8_t>(HardWareIndex::L0A) ||
        param_.dstPos == static_cast<uint8_t>(HardWareIndex::L0B)) {
        blockLen = 512; // when L0A / L0B, block is in unit of 512B
    }
#endif
    uint64_t bufferSize = ((blockNum + dstGap) * repeatTimes - dstGap) * blockLen;
    if (param_.dstSize < bufferSize) {
        CHECK_LOG_ERROR(
            "Failed to check dstLocal tensor calculated size in InitConstValue: calculated size %llu bytes "
            "exceeds dstSize %llu bytes; please check repeatTimes, blockNum, dstGap",
            static_cast<unsigned long long>(bufferSize), static_cast<unsigned long long>(param_.dstSize));
        return false;
    }

    return true;
}
} // namespace check
} // namespace AscendC
