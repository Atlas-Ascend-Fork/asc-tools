/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_APPLICATION_OP_COLLECTOR_H
#define NPU_COMPUTE_APPLICATION_OP_COLLECTOR_H

#include "runtime/kernel_metadata_collector.h"
#include "report/application_op_info.h"
#include <functional>

namespace npucompute {
class ApplicationOpCollector final {
public:
    using DeviceEpochQuery = std::function<uint64_t(int32_t)>;
    explicit ApplicationOpCollector(DeviceEpochQuery query) : deviceEpoch_(std::move(query)) {}
    bool Initialize(aclptiCallbackFunc callback);
    void OnCallback(aclptiCallbackId cbid, const aclptiCallbackData& data);
    ApplicationOpSnapshot Snapshot();

private:
    const SummaryHardwareInfo& CollectHardware(int32_t device);
    KernelMetadataCollector metadata_;
    DeviceEpochQuery deviceEpoch_;
    aclptiSubscribeHandle subscriber_ = nullptr;
    std::mutex mutex_;
    std::map<int32_t, SummaryHardwareInfo> hardware_;
    std::map<std::pair<int32_t, std::string>, uint64_t> launchIndices_;
    ApplicationOpSnapshot snapshot_;
};
} // namespace npucompute
#endif
