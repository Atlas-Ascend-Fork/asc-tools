/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_HOST_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_HOST_H

#include "hardware/hardware_info_types.h"

#include <boost/filesystem.hpp>

namespace npucompute {

struct HostInfoCollectionOptions {
    boost::filesystem::path cpuTopologyRoot = "/sys/devices/system/cpu";
};

bool CollectHostInfo(
    const boost::filesystem::path& outputDirectory, HostInfo* result, DiagnosticSink* diagnostics,
    const HostInfoCollectionOptions& options = HostInfoCollectionOptions{});

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_HARDWARE_HARDWARE_INFO_HOST_H
