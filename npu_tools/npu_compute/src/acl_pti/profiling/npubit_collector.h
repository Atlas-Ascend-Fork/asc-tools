/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_NPUBIT_COLLECTOR_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_NPUBIT_COLLECTOR_H

#include "aclpti/aclpti_data.h"
#include "binary_registry.h"
#include "range_profiler.h"
#include <cstdint>
#include <optional>

namespace aclpti::profiling {

struct AiCoreTopology {
    std::uint32_t dieCount;
    std::uint32_t coresPerDie;
};

bool ReadNpubitTopology(std::int32_t deviceId, AiCoreTopology& topology);

// Work per dynamic instruction. These coefficients already include x2.
std::optional<double> RooflineVectorOperations(std::uint32_t bitWidth, std::uint64_t instructionCount);

// Called after original-kernel PMU rounds, with the target device/context current.
// Optional instrumentation failures stay in data; restore/launch/sync failures are returned.
aclptiResult CollectNpubitRoofline(
    const BinarySource& source, const ReplayMemory& memory, const ReplayLaunchFunction& launch,
    aclrtSynchronizeStreamFunc synchronize, aclrtStream stream, std::int32_t deviceId,
    const std::function<void()>& beforeReplay, aclptiRooflineData& data);

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_NPUBIT_COLLECTOR_H
