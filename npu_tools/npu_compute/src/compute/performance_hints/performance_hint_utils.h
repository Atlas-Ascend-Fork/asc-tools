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

#include "performance_hint_analyzer.h"
#include <initializer_list>
#include <string>

namespace npucompute::performance_hints {

struct HintContext {
    const aclptiProfilingDataResult& result;
    const HintSections& sections;
    const ReportConfig& config;
    const KernelMetadata& metadata;
};

std::string CoreName(aclptiCoreType core);

std::string Prefix(aclptiCoreType core);

std::string Number(double value, double threshold);

HintEvaluation BeginEvaluation(
    const HintContext& context, const std::string& id, aclptiCoreType core,
    std::initializer_list<std::string> sections);

SummaryMetric ReadField(
    const HintContext& context, HintEvaluation& evaluation, const std::string& section, const std::string& field,
    bool ratio = true);

bool MaximumPipeline(
    const HintContext& context, HintEvaluation& evaluation, const std::string& section, const std::string& target);

void FinishEvaluation(
    const HintContext& context, HintAnalysis& analysis, HintEvaluation evaluation, const std::string& message);

} // namespace npucompute::performance_hints
