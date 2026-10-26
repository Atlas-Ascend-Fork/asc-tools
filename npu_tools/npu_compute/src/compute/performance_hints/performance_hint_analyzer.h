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

#include "report/summary_metrics.h"
#include <map>
#include <string>
#include <vector>

namespace npucompute {

using HintSections = std::map<std::string, SummaryFields>;
struct HintEvaluation {
    std::string id;
    aclptiCoreType coreType;
    std::vector<std::string> sections;
    // Summary inputs use Section.field keys; derived observations use rule-specific names.
    std::map<std::string, double> observations;
    // An empty skip reason means all prerequisites were available; otherwise it explains why the rule was skipped.
    std::string skipReason;
};

struct HintAnalysis {
    std::vector<std::string> hints;
    std::vector<HintEvaluation> evaluations;
};

HintAnalysis AnalyzePerformanceHints(
    const aclptiProfilingDataResult& result, const HintSections& sections, const ReportConfig& config,
    const KernelMetadata& metadata);
} // namespace npucompute
