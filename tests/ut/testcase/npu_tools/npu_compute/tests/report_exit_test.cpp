/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "report/summary_metrics.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>

namespace {
void CheckReport()
{
    aclptiProfilingDataResult result{};
    // A real task duration with no PMU data must still produce a complete schema.
    result.taskLogs[1] = {
        {0, 0, 1, 0, 100, 0, 0, ACLPTI_CORE_TYPE_AIC, 0, 0}, {0, 1, 1, 0, 10100, 0, 0, ACLPTI_CORE_TYPE_AIC, 0, 0}};
    const npucompute::ReportConfig config{};
    const auto header = npucompute::PmuMetricBuilder::Header("ArithmeticUtilization", npucompute::PmuDataLevel::Task);
    const auto values = npucompute::PmuMetricBuilder::BuildTask("ArithmeticUtilization", result.taskPmuData, config);
    if (header.empty() || header.size() != values.size()) {
        throw std::runtime_error("report schema unavailable during process exit");
    }
    const auto metrics = npucompute::BuildSummaryMetrics(result, config, {});
    if (metrics.aicFlops || metrics.aivFlops) {
        throw std::runtime_error("missing PMU data must remain unavailable");
    }
}

void ReportAtExit() noexcept
{
    try {
        CheckReport();
        std::puts("report exit check passed");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "report exit check failed: %s\n", error.what());
        std::_Exit(EXIT_FAILURE);
    }
}
} // namespace

int main()
{
    // Register before the first lookup: late reports must outlive lazy statics.
    if (std::atexit(ReportAtExit) != 0) {
        return EXIT_FAILURE;
    }
    CheckReport();
    return EXIT_SUCCESS;
}
