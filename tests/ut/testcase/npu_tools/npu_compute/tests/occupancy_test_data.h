/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_OCCUPANCY_TEST_DATA_H
#define NPU_COMPUTE_OCCUPANCY_TEST_DATA_H
#include "aclpti/aclpti_data.h"

namespace npucompute::test {
inline void AddOccupancyReplay(aclptiProfilingDataResult& result, int32_t deviceId = 0)
{
    auto& replay = result.replayData[0];
    replay.deviceId = deviceId;
    replay.taskLogs = {
        {0, 0x00, 0, 0, 1000, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}, {0, 0x01, 0, 0, 2000, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}};
    replay.blockLogs = {
        {0, 0x24, 0, 0, 1100, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}, {0, 0x25, 0, 0, 1900, 0, 0, ACLPTI_CORE_TYPE_AIV, 0}};
    replay.pmuRecords = {{0x29, 0, 0, ACLPTI_CORE_TYPE_AIV, 0, 0, 0, 1050, 1950, 900, false}};
}
} // namespace npucompute::test
#endif
