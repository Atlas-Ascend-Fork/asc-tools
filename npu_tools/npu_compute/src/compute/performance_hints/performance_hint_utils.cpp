/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "performance_hint_utils.h"
#include "npu_compute_log.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace npucompute::performance_hints {

namespace {

bool Applicable(aclptiCoreType core, const KernelMetadata& metadata)
{
    return !metadata.kind || *metadata.kind == KernelKind::Mix ||
           (*metadata.kind == KernelKind::Cube) == (core == ACLPTI_CORE_TYPE_AIC);
}

} // namespace

std::string CoreName(aclptiCoreType core) { return core == ACLPTI_CORE_TYPE_AIC ? "AIC" : "AIV"; }

std::string Prefix(aclptiCoreType core) { return core == ACLPTI_CORE_TYPE_AIC ? "aic_" : "aiv_"; }

// Increase display precision only when rounding would hide which side of the threshold was observed.
std::string Number(double value, double threshold)
{
    const auto format = [](double number, int precision) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(precision) << number;
        return out.str();
    };
    for (int precision = 6; precision < std::numeric_limits<double>::max_digits10; ++precision) {
        const auto text = format(value, precision);
        if (value == threshold || text != format(threshold, precision)) {
            return text;
        }
    }
    return format(value, std::numeric_limits<double>::max_digits10);
}

HintEvaluation BeginEvaluation(
    const HintContext& context, const std::string& id, aclptiCoreType core, std::initializer_list<std::string> sections)
{
    HintEvaluation evaluation;
    evaluation.id = id;
    evaluation.coreType = core;
    evaluation.sections = sections;
    if (!Applicable(core, context.metadata)) {
        evaluation.skipReason = "kernel_type_not_applicable";
        return evaluation;
    }
    for (const auto& section : evaluation.sections) {
        if (!context.sections.count(section)) {
            evaluation.skipReason = "section_not_selected: " + section;
            return evaluation;
        }
    }
    return evaluation;
}

SummaryMetric ReadField(
    const HintContext& context, HintEvaluation& evaluation, const std::string& section, const std::string& field,
    bool ratio)
{
    const auto source = context.sections.find(section);
    if (source == context.sections.end()) {
        evaluation.skipReason = "section_not_selected: " + section;
        return std::nullopt;
    }
    const auto it = source->second.find(field);
    if (it == source->second.end() || !it->second.value || !std::isfinite(*it->second.value) || *it->second.value < 0 ||
        (ratio && *it->second.value > 1)) {
        evaluation.skipReason = "invalid_or_missing_field: " + section + "." + field;
        return std::nullopt;
    }
    evaluation.observations[section + "." + field] = *it->second.value;
    return it->second.value;
}

bool MaximumPipeline(
    const HintContext& context, HintEvaluation& evaluation, const std::string& section, const std::string& target)
{
    const auto pipes = evaluation.coreType == ACLPTI_CORE_TYPE_AIC ?
                           std::vector<std::string>{"cube", "scalar", "mte1", "mte2", "mte3", "fixpipe"} :
                           std::vector<std::string>{"vec", "scalar", "mte2", "mte3"};
    double highest = 0;
    bool complete = true;
    for (const auto& pipe : pipes) {
        const auto value = ReadField(context, evaluation, section, Prefix(evaluation.coreType) + pipe + "_ratio");
        complete = complete && value.has_value();
        if (value) {
            highest = std::max(highest, *value);
        }
    }
    const auto value = ReadField(context, evaluation, section, target);
    return complete && value && *value == highest;
}

void FinishEvaluation(
    const HintContext& context, HintAnalysis& analysis, HintEvaluation evaluation, const std::string& message)
{
    if (evaluation.skipReason.empty()) {
        if (!message.empty()) {
            analysis.hints.push_back("[" + evaluation.id + "][" + CoreName(evaluation.coreType) + "] " + message);
        }
    } else {
        std::string sections;
        for (const auto& section : evaluation.sections) {
            if (!sections.empty()) {
                sections += ",";
            }
            sections += section;
        }
        ASCTOOL_DEBUG(
            "[npu-compute] component=performance_hint performance_hint_id=%s core_type=%s task_name=%s "
            "required_sections=%s skip_reason=%s",
            evaluation.id.c_str(), CoreName(evaluation.coreType).c_str(), context.metadata.name.value_or("NA").c_str(),
            sections.c_str(), evaluation.skipReason.c_str());
    }
    analysis.evaluations.push_back(std::move(evaluation));
}

} // namespace npucompute::performance_hints
