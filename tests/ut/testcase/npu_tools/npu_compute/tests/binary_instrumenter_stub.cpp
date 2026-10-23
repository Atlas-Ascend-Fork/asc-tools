/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/profiling/binary_instrumenter.h"
#include <cstdlib>

namespace aclpti::profiling {
// Stub only the binary instrumentation boundary; binary ownership, lookup and replay
// execute the production implementation against the Runtime stub.
bool InstrumentKernelEnd(
    const void* data, std::size_t size, std::vector<char>& output, std::uint32_t* traceArgumentOffset,
    PmSamplingInstructionMap* pmSamplingMap)
{
    if (pmSamplingMap != nullptr) {
        pmSamplingMap->business = {{0x100, 0}};
        pmSamplingMap->excluded = {0x104};
        pmSamplingMap->functionEntries = {{"kernel", 0x100}, {"sample", 0x100}};
        pmSamplingMap->relocatedEntries = {{0, 0x100}, {42, 42}};
        if (std::getenv("NPU_COMPUTE_TEST_PM_MAP_FAILURE") != nullptr) {
            pmSamplingMap->business.clear();
            pmSamplingMap->excluded.clear();
            pmSamplingMap->error = "ambiguous business instruction alignment in kernel";
        }
    }
    if (traceArgumentOffset != nullptr) {
        *traceArgumentOffset = 56;
    }
    output.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
    return true;
}
} // namespace aclpti::profiling
