/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_OCCUPANCY_TRACE_H
#define NPU_COMPUTE_OCCUPANCY_TRACE_H
#include "aclpti/aclpti_data.h"
#include <map>
#include <string>

namespace npucompute {
class ArtifactSink;
struct OccupancyTrace {
    aclptiResult status = ACLPTI_ERROR_TRACE_INCOMPLETE;
    std::string json;
    std::string diagnostic;
    uint64_t intervalCount = 0;
};
OccupancyTrace BuildOccupancyTrace(
    const aclptiProfilingDataResult& result, const std::map<int32_t, double>& frequenciesHz, uint64_t launchSequence);
aclptiResult PublishOccupancyTrace(const OccupancyTrace& trace, ArtifactSink& sink, const std::string& prefix);
} // namespace npucompute
#endif
