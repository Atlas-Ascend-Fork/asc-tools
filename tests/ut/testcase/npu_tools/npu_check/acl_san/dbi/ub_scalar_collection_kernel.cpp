// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include <cstdint>

// Compile/instrument fixture only: the UB buffers do not contain executable workload data.
extern "C" __global__ __aicore__ void UbScalarCollectionKernel(__gm__ uint64_t* out, uint64_t value)
{
#if defined(__DAV_VEC__)
    auto* ub8 = reinterpret_cast<volatile __ubuf__ uint8_t*>(0x1000);
    auto* ub16 = reinterpret_cast<volatile __ubuf__ uint16_t*>(0x1020);
    auto* ub32 = reinterpret_cast<volatile __ubuf__ uint32_t*>(0x1040);
    auto* ub64 = reinterpret_cast<volatile __ubuf__ uint64_t*>(0x1060);
    *ub8 = value;
    *ub16 = value;
    *ub32 = value;
    *ub64 = value;
    out[0] = *ub8;
    out[1] = *ub16;
    out[2] = *ub32;
    out[3] = *ub64;
#endif
}
