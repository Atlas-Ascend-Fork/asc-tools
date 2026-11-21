/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_NPUBIT_DEVICE_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_NPUBIT_DEVICE_H

#include <cstdint>
#include <npubit.h>
#include <string>

namespace aclpti::profiling {

struct AiCoreTopology {
    std::uint32_t dieCount;
    std::uint32_t coresPerDie;
};

struct NpubitDevice {
    NpubitPlatformType platform = UNDEFINED;
    AiCoreTopology topology{};
};

bool ReadNpubitDevice(std::int32_t deviceId, NpubitDevice& device, std::string& error);

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_NPUBIT_DEVICE_H
