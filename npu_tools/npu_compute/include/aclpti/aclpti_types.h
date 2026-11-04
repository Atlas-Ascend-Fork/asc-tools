/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_TYPES_H
#define NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_TYPES_H

#include "aclpti/aclpti_export.h"

#include <stdint.h>

typedef enum aclptiResult {
    ACLPTI_SUCCESS = 0,
    ACLPTI_ERROR_INVALID_PARAMETER = 1,
    ACLPTI_ERROR_INVALID_SUBSCRIBER = 2,
    ACLPTI_ERROR_NOT_SUPPORTED = 3,
    ACLPTI_ERROR_INVALID_STATE = 4,
    ACLPTI_ERROR_OUT_OF_MEMORY = 5,
    ACLPTI_ERROR_INITIALIZATION_FAILED = 6,
    ACLPTI_ERROR_PROFILING_FAILED = 7,
    ACLPTI_ERROR_INTERNAL = 8,
    ACLPTI_ERROR_NOT_INITIALIZED = 9,
    ACLPTI_ERROR_REPLAY_ACTIVE = 10,
    ACLPTI_ERROR_NO_ACTIVE_REPLAY = 11,
    ACLPTI_ERROR_REPLAY_NOT_FOUND = 12,
    ACLPTI_ERROR_QUEUE_FULL = 13,
    ACLPTI_ERROR_INVALID_RAW_DATA = 14,
    ACLPTI_ERROR_DECODE = 15,
    ACLPTI_ERROR_ASSEMBLE = 16,
    ACLPTI_ERROR_CALLBACK = 17,
    ACLPTI_ERROR_CSV_INCOMPLETE = 18,
    ACLPTI_ERROR_CSV_WRITE = 19,
    ACLPTI_ERROR_RESULT_UNRELIABLE = 20,
    ACLPTI_ERROR_TRACE_INCOMPLETE = 21,
    ACLPTI_ERROR_TRACE_WRITE = 22,
} aclptiResult;

typedef struct aclptiSubscriber_st* aclptiSubscribeHandle;

typedef struct aclptiSubscribeParams {
    uint64_t reserved;
} aclptiSubscribeParams;

#endif // NPU_TOOLS_NPU_COMPUTE_INCLUDE_ACLPTI_ACLPTI_TYPES_H
