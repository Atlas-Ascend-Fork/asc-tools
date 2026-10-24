/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_RANGE_PROFILER_H
#define NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_RANGE_PROFILER_H

#include "aclpti/aclpti_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACLPTI_MAX_NUM_SECTIONS ((size_t)64)
#define ACLPTI_MAX_SECTION_NAME_LENGTH ((size_t)64)

typedef enum aclptiCacheControl {
    ACLPTI_CACHE_CONTROL_NONE = 0,
    ACLPTI_CACHE_CONTROL_ALL = 1,
} aclptiCacheControl;

typedef enum aclptiBlockResultMode {
    ACLPTI_BLOCK_RESULT_DISABLED = 0,
    ACLPTI_BLOCK_RESULT_ALL = 1,
    ACLPTI_BLOCK_RESULT_SHRINK = 2,
} aclptiBlockResultMode;

typedef struct aclptiRangeProfilerSetConfigParams {
    const char* const* sections;
    size_t numSections;
    aclptiBlockResultMode blockResult; // Applies to PMU/PC sampling; ignored for pipeline.
    bool collectPipeline;              // Enable before aclrtBinaryLoadFromData; requires the runtime Bisheng toolchain.
    bool collectPcSampling;
    aclptiCacheControl cacheControl; // Configure before the first replay; zero means NONE.
} aclptiRangeProfilerSetConfigParams;

ACLPTI_EXPORT aclptiResult aclptiRangeProfilerSetConfig(aclptiRangeProfilerSetConfigParams* pParams);

#ifdef __cplusplus
}
#endif

#endif // NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_RANGE_PROFILER_H
