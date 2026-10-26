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
#include "compute_types.h"
#include "pmu/pmu_metric_builder.h"
#include <map>
#include <string>

namespace npucompute {
using SummaryMetric = std::optional<double>;
struct SummaryField {
    SummaryMetric value;
    bool integer = false;
};
using SummaryFields = std::map<std::string, SummaryField>;
struct SummaryMetrics {
    SummaryMetric parallelUtilization;
    SummaryMetric parallelBalance;
    SummaryMetric aicFlopsTheoretical;
    SummaryMetric aivFlopsTheoretical;
    SummaryMetric aicFlops;
    SummaryMetric aivFlops;
};
SummaryMetric SummaryEffectiveFrequency(const KernelMetadata& metadata, const ReportConfig& config, bool aic);
SummaryMetric SummaryRatedFrequency(const KernelMetadata& metadata, bool aic);
ReportConfig SummaryReportConfig(const KernelMetadata& metadata, const ReportConfig& config);
SummaryFields BuildTaskSummarySection(
    const std::string& section, const aclptiProfilingDataResult& result, const ReportConfig& config);
SummaryMetrics BuildSummaryMetrics(
    const aclptiProfilingDataResult& result, const ReportConfig& config, const KernelMetadata& metadata);
} // namespace npucompute
