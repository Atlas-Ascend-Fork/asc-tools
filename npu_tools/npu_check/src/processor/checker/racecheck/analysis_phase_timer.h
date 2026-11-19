/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#pragma once
#include "npu_tool_log.h"
#include <chrono>
#include <cstdint>
#include <limits>

namespace npucheck::race {
// Callers pass static stage names. The end marker is absent if the process is terminated inside the phase.
class AnalysisPhaseTimer {
public:
    AnalysisPhaseTimer(uint64_t launchId, const char* stage) noexcept
        : launchId_(launchId), stage_(stage), start_(std::chrono::steady_clock::now())
    {
        ASCTOOL_INFO(
            "analysis phase: launch=%llu stage=%s state=begin", static_cast<unsigned long long>(launchId_), stage_);
    }
    ~AnalysisPhaseTimer() noexcept
    {
        using Duration = std::chrono::microseconds;
        static_assert(std::numeric_limits<Duration::rep>::digits <= std::numeric_limits<long long>::digits);
        const auto elapsed = std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now() - start_).count();
        ASCTOOL_INFO(
            "analysis phase: launch=%llu stage=%s state=end elapsed_us=%lld",
            static_cast<unsigned long long>(launchId_), stage_, static_cast<long long>(elapsed));
    }
    AnalysisPhaseTimer(const AnalysisPhaseTimer&) = delete;
    AnalysisPhaseTimer& operator=(const AnalysisPhaseTimer&) = delete;

private:
    const uint64_t launchId_;
    const char* const stage_;
    const std::chrono::steady_clock::time_point start_;
};
} // namespace npucheck::race
