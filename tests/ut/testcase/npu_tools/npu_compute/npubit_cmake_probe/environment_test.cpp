/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "simt_warp_environment.h"
#include <cassert>
#include <cstdlib>
#include <initializer_list>
int main()
{
    using aclpti::profiling::GetWarpEnvironment;
    NpubitPlatformType platform = UNDEFINED;
    NpubitHwContext hw{};
    setenv("NPU_COMPUTE_NPUBIT_PLATFORM", "V100", 1);
    unsetenv("NPU_COMPUTE_NPUBIT_DIE_COUNT");
    unsetenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE");
    unsetenv("NPU_COMPUTE_NPUBIT_AICORE_PER_DIE");
    assert(GetWarpEnvironment(platform, hw) && platform == V100 && hw.die_count == 2 && hw.aicore_per_die == 18);
    setenv("NPU_COMPUTE_NPUBIT_PLATFORM", "V121", 1);
    assert(!GetWarpEnvironment(platform, hw));
    setenv("NPU_COMPUTE_NPUBIT_PLATFORM", "V100", 1);
    setenv("NPU_COMPUTE_NPUBIT_DIE_COUNT", "", 1);
    assert(!GetWarpEnvironment(platform, hw));
    setenv("NPU_COMPUTE_NPUBIT_DIE_COUNT", "1", 1);
    setenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE", "32", 1);
    assert(GetWarpEnvironment(platform, hw) && platform == V100 && hw.die_count == 1 && hw.aicore_per_die == 32);
    unsetenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE");
    setenv("NPU_COMPUTE_NPUBIT_AICORE_PER_DIE", "32", 1);
    assert(GetWarpEnvironment(platform, hw) && hw.aicore_per_die == 32);
    setenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE", "032", 1);
    assert(GetWarpEnvironment(platform, hw));
    setenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE", "31", 1);
    assert(!GetWarpEnvironment(platform, hw));
    unsetenv("NPU_COMPUTE_NPUBIT_AICORE_PER_DIE");
    for (const char* bad : {"", "0", "-1", "+1", " 32", "32x", "4294967296"}) {
        setenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE", bad, 1);
        assert(!GetWarpEnvironment(platform, hw));
    }
    setenv("NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE", "32", 1);
    setenv("NPU_COMPUTE_NPUBIT_PLATFORM", "950DT", 1);
    assert(!GetWarpEnvironment(platform, hw));

    std::size_t traceBytes = 4U * 1024U * 1024U;
    unsetenv("NPU_COMPUTE_NPUBIT_TRACE_BUFFER_BYTES");
    assert(aclpti::profiling::GetWarpTraceBufferBytes(traceBytes));
    assert(traceBytes == 4U * 1024U * 1024U);
    setenv("NPU_COMPUTE_NPUBIT_TRACE_BUFFER_BYTES", "8388608", 1);
    assert(aclpti::profiling::GetWarpTraceBufferBytes(traceBytes));
    assert(traceBytes == 8388608U);
    for (const char* bad : {"", "0", "-8", "+8", " 8", "12x", "18446744073709551616"}) {
        setenv("NPU_COMPUTE_NPUBIT_TRACE_BUFFER_BYTES", bad, 1);
        assert(!aclpti::profiling::GetWarpTraceBufferBytes(traceBytes));
        assert(traceBytes == 8388608U);
    }
    unsetenv("NPU_COMPUTE_NPUBIT_TRACE_BUFFER_BYTES");
    return 0;
}
