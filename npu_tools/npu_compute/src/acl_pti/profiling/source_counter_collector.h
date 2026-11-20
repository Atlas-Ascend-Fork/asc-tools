/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SOURCE_COUNTER_COLLECTOR_H
#define NPU_TOOLS_NPU_COMPUTE_SOURCE_COUNTER_COLLECTOR_H

#include "aclpti/aclpti_data.h"
#include "binary_registry.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace aclpti::profiling {

std::size_t SourceCounterBufferSize(std::size_t reported, std::size_t defaultTraceMiB);

struct SourceCounterReplayResult {
    aclptiResult status;
    bool streamDrained;
};

using SourceCounterReplay = std::function<SourceCounterReplayResult(aclrtFuncHandle, std::size_t)>;

aclptiResult CollectSourceCounterData(
    const BinarySource& source, std::int32_t deviceId, const SourceCounterReplay& replay,
    aclptiSourceCounterData& data);

} // namespace aclpti::profiling
#endif
