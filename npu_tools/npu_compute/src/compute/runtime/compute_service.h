/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_SERVICE_H
#define NPU_COMPUTE_SERVICE_H
#include "ipc/artifact_publisher.h"
#include "ipc/npu_compute_uds_server.h"
#include "runtime/runtime_config.h"
#include <memory>
#include <mutex>
namespace npucompute {
class ComputeService {
public:
    static ComputeService& Instance();
    int Initialize() noexcept;
    int Shutdown() noexcept;
    void Stop() const noexcept;

private:
    int FinalizeOnce(int status);
    std::mutex mutex_;
    bool initialized_ = false;
    bool attempted_ = false;
    bool finalized_ = false;
    int result_ = -1;
    ipc::Config config_;
    RuntimeConfig runtime_config_;
    std::unique_ptr<ArtifactPublisher> publisher_;
};
} // namespace npucompute
#endif
