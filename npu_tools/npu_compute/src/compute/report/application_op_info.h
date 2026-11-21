/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_APPLICATION_OP_INFO_H
#define NPU_COMPUTE_APPLICATION_OP_INFO_H

#include "compute_types.h"
#include <vector>

namespace npucompute {
struct ApplicationOpInfo {
    KernelMetadata metadata;
    uint64_t deviceEpoch = 0;
    uint64_t launchIndex = 0;
    uint64_t instanceIndex = 0;
    std::optional<uint32_t> taskId;
    std::optional<uint32_t> streamId;
};

struct ApplicationOpSnapshot {
    std::vector<ApplicationOpInfo> ops;
    std::string hardwareJsonl;
    uint64_t unidentifiedLaunches = 0;
};
} // namespace npucompute
#endif
