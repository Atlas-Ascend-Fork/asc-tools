/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <npubit.h>
#include <cstdint>
#include <cstdio>
int main()
{
    std::uint32_t major = 0, minor = 0, patch = 0;
    const auto status = NpubitGetVersion(&major, &minor, &patch);
    std::printf("NpubitGetVersion status=%d version=%u.%u.%u\n", static_cast<int>(status), major, minor, patch);
    return status == NPUBIT_SUCCESS ? 0 : 1;
}
