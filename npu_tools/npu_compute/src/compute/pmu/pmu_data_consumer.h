/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PMU_PMU_DATA_CONSUMER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PMU_PMU_DATA_CONSUMER_H

#include "aclpti/aclpti_data.h"

#include <functional>
#include <memory>

namespace npucompute {

class PmuDataConsumer final {
public:
    using Processor = std::function<aclptiResult(std::shared_ptr<const aclptiProfilingDataResult>)>;

    static std::shared_ptr<PmuDataConsumer> Create(Processor processor);
    ~PmuDataConsumer();

    PmuDataConsumer(const PmuDataConsumer&) = delete;
    PmuDataConsumer& operator=(const PmuDataConsumer&) = delete;

    aclptiResult Start();
    aclptiResult Submit(std::shared_ptr<const aclptiProfilingDataResult> result);
    aclptiResult ShutdownAndDrain();

    explicit PmuDataConsumer(Processor processor);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PMU_PMU_DATA_CONSUMER_H
