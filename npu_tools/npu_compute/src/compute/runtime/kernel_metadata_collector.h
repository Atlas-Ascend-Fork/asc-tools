/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_KERNEL_METADATA_COLLECTOR_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_KERNEL_METADATA_COLLECTOR_H

#include "aclpti/aclpti_runtime_api.h"
#include "compute_types.h"
#include <map>
#include <mutex>

namespace npucompute {

class KernelMetadataCollector final {
public:
    void OnCallback(aclptiCallbackId cbid, const aclptiCallbackData& data);
    KernelMetadata Snapshot();

private:
    std::mutex mutex_;
    std::map<aclrtFuncHandle, std::pair<aclrtBinHandle, std::string>> names_;
    std::map<aclrtFuncHandle*, std::pair<aclrtBinHandle, std::string>> pendingNames_;
    KernelMetadata metadata_;
    aclptiCallbackId pendingLaunchCbid_ = 0;
    bool launchPending_ = false;
    bool frozen_ = false;
};

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_KERNEL_METADATA_COLLECTOR_H
