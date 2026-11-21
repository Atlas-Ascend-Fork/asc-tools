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

#include "acl_pti/profiling/application_profiler.h"
#include <condition_variable>
#include <memory>
#include <mutex>

namespace npucompute {
class ComputeService {
public:
    static ComputeService& Instance();
    ~ComputeService();
    static void InitializeFromAcl() noexcept;
    static void FinalizeFromAcl() noexcept;
    int Initialize(bool fromAcl = false) noexcept;
    int Shutdown() noexcept;

private:
    struct Session;
    enum class State { Idle, Starting, Running, Closing, Stopped };
    int ShutdownAndWait(aclpti::profiling::ApplicationEndReason reason) noexcept;
    int CloseOwnedSession(aclpti::profiling::ApplicationEndReason reason) noexcept;
    void FinalizeApplication() noexcept;

    std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::Idle;
    int result_ = -1;
    bool finalizeDeferred_ = false;
    std::unique_ptr<Session> session_;
};
} // namespace npucompute
#endif
