/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
// 参考实现：将真实 npubit.h 的 SIMT Warp Occupancy pass 接入 npu-compute。
// 目标仓库中使用时，建议把本文件放到：
// npu_tools/npu_compute/src/acl_pti/profiling/
#pragma once

#include "range_profiler.h"            // ReplayLaunchFunction
#include "replay_memory.h"             // ReplayMemory
#include "simt_warp_source_registry.h" // SimtWarpBinarySource

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <npubit.h>

namespace aclpti::profiling {

using SimtWarpRecord = aclptiSimtWarpRecord;
using SimtWarpData = aclptiSimtWarpData;

struct SimtWarpCollectOptions {
    // 必须由调用方按真实设备填写；不要默认把所有设备当成 V100/1 die/32 core。
    NpubitPlatformType platform = UNDEFINED;
    NpubitHwContext hardware{};
    bool needsVerify = true;

    // NpubitGetBufferSize 对 trace-like pass 可能返回 SIZE_MAX。
    std::size_t traceFallbackBytes = 4U * 1024U * 1024U;
};

// launch(function, false) 必须使用原始 Kernel 的业务参数启动 function。
// Collector 只替换函数句柄，不保存或异步使用 launch 回调。
aclptiResult CollectNpubitSimtWarp(
    const SimtWarpBinarySource& source, const ReplayMemory& replayMemory, const ReplayLaunchFunction& launch,
    aclrtSynchronizeStreamFunc synchronize, aclrtStream stream, const SimtWarpCollectOptions& options,
    SimtWarpData& output);

} // namespace aclpti::profiling
