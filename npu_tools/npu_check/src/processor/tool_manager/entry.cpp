/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "npu_check.h"

#include "tool_manager/tool_manager.h"

#include <memory>
#include <mutex>
#include <utility>

namespace npucheck {
namespace {

std::mutex g_serviceMutex;
// The function-local ServiceOwner owns the service; access is protected by g_serviceMutex.
npucheck::ToolManager* g_service = nullptr;

class ServiceOwner {
public:
    explicit ServiceOwner(std::unique_ptr<npucheck::ToolManager> service) noexcept : service_(std::move(service)) {}
    ~ServiceOwner() noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(g_serviceMutex);
            g_service = nullptr;
            service_.reset();
        } catch (...) {
            // Process-exit cleanup must not propagate exceptions.
            return;
        }
    }
    ServiceOwner(const ServiceOwner&) = delete;
    ServiceOwner& operator=(const ServiceOwner&) = delete;

    npucheck::ToolManager* Get() const noexcept { return service_.get(); }

private:
    std::unique_ptr<npucheck::ToolManager> service_;
};

} // namespace

extern "C" NPU_CHECK_API int acltoolInitialize(void)
{
    try {
        std::lock_guard<std::mutex> lock(g_serviceMutex);
        if (g_service) {
            return g_service->IsInitialized() ? 0 : 1;
        }
        auto service = std::make_unique<npucheck::ToolManager>();
        const int result = service->Initialize();
        if (result != 0) {
            return result;
        }
        // Establish exit cleanup only after initialization, preserving dependency teardown order.
        static ServiceOwner serviceOwner(std::move(service));
        g_service = serviceOwner.Get();
        return 0;
    } catch (...) {
        return 1;
    }
}
} // namespace npucheck
