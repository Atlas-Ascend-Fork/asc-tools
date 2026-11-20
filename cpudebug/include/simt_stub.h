/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

/*!
 * \file simt_stub.h
 * \brief
 */
#ifndef ASCENDC_SIMT_STUB_H
#define ASCENDC_SIMT_STUB_H

#if defined(ASCENDC_CPU_DEBUG)
#if defined(__NPU_ARCH__) && ((__NPU_ARCH__ == 3510) || (__NPU_ARCH__ == 5102) || (__NPU_ARCH__ == 9201))
#include "simt_internal_functions.h"
#include "simt_asc_fp16.h"
#include "simt_asc_bf16.h"
#include "simt_asc_fp8.h"
#include "simt_device_functions.h"

namespace cce {
template <auto funcPtr, typename... Args>
void async_invoke(const dim3& dim, Args&&... args)
{
    g_threadDimX = dim.x;
    g_threadDimY = dim.y;
    g_threadDimZ = dim.z;

    blockDim.x = g_threadDimX;
    blockDim.y = g_threadDimY;
    blockDim.z = g_threadDimZ;

    blockIdx.x = block_idx;

    gridDim.x = block_num;

    AscendC::Simt::ThreadBlock& threadBlock = AscendC::Simt::ThreadBlock::GetBlockInstance();
    const uint32_t threadNum = g_threadDimX * g_threadDimY * g_threadDimZ;
    threadBlock.Init(threadNum);
    auto func = [&args...]() { funcPtr(args...); };
    for (uint32_t i = 0; i < threadNum; i++) {
        threadBlock.Schedule(func, i);
    }
    threadBlock.FinishJobs();
}
} // namespace cce

#endif
#endif
#endif
