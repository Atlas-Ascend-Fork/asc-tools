/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_COMPUTE_INTERNAL_UDS_SERVER_H
#define NPU_COMPUTE_INTERNAL_UDS_SERVER_H
#include "ipc/ipc.h"
namespace npucompute {
class UdsServer {
public:
    ipc::Hello Accept(bool connectToCli = false);
    void Ready();
    void Fail(const std::string& message) noexcept;
    void Fail(const ipc::Error& error) noexcept;
    ipc::Deadline Deadline() const { return deadline_; }
    ipc::Channel TakeChannel() { return std::move(channel_); }

private:
    ipc::Channel channel_;
    ipc::Deadline deadline_;
};
} // namespace npucompute
#endif // NPU_COMPUTE_INTERNAL_UDS_SERVER_H
