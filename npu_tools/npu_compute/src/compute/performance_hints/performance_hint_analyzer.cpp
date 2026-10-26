/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "performance_hint_analyzer.h"
#include "performance_hint_utils.h"
#include "npu_compute_log.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace npucompute {

namespace {

using performance_hints::BeginEvaluation;
using performance_hints::CoreName;
using performance_hints::FinishEvaluation;
using performance_hints::HintContext;
using performance_hints::MaximumPipeline;
using performance_hints::Number;
using performance_hints::Prefix;
using performance_hints::ReadField;

void EvaluatePh01(const HintContext& context, HintAnalysis& analysis)
{
    constexpr double kThreshold = .50;
    auto evaluation = BeginEvaluation(context, "PH01", ACLPTI_CORE_TYPE_AIV, {"PipeUtilization"});
    std::string message;
    if (evaluation.skipReason.empty()) {
        const auto observed = ReadField(context, evaluation, "PipeUtilization", "aiv_vec_ratio");
        const bool highest = MaximumPipeline(context, evaluation, "PipeUtilization", "aiv_vec_ratio");
        if (observed && *observed > kThreshold && highest) {
            message = "aiv_vec_ratio（Vector 流水占比）为 " + Number(*observed * 100, kThreshold * 100) +
                      "%，超过 50% 且占比最高；建议检查 Vector 指令序列、冗余计算、转换和归约。";
        }
    }
    FinishEvaluation(context, analysis, std::move(evaluation), message);
}

void EvaluatePh02(const HintContext& context, HintAnalysis& analysis)
{
    constexpr double kThreshold = .30;
    for (const auto core : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
        auto evaluation = BeginEvaluation(context, "PH02", core, {"PipeUtilization"});
        std::string message;
        if (evaluation.skipReason.empty()) {
            const auto field = Prefix(core) + "scalar_ratio";
            const auto observed = ReadField(context, evaluation, "PipeUtilization", field);
            if (observed && *observed > kThreshold) {
                message = field + "（Scalar 流水占比）为 " + Number(*observed * 100, kThreshold * 100) +
                          "%，超过 30%；建议检查循环内地址计算、参数重建、分支和可外提不变量。";
            }
        }
        FinishEvaluation(context, analysis, std::move(evaluation), message);
    }
}

void EvaluatePh03(const HintContext& context, HintAnalysis& analysis)
{
    constexpr double kThreshold = .50;
    for (const auto core : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
        auto evaluation = BeginEvaluation(context, "PH03", core, {"PipeUtilization"});
        std::string message;
        if (evaluation.skipReason.empty()) {
            const auto field = Prefix(core) + "mte2_ratio";
            const auto observed = ReadField(context, evaluation, "PipeUtilization", field);
            const bool highest = MaximumPipeline(context, evaluation, "PipeUtilization", field);
            if (observed && *observed > kThreshold && highest) {
                message = field + "（MTE2 流水占比）为 " + Number(*observed * 100, kThreshold * 100) +
                          "%，超过 50% 且占比最高；建议检查搬入链路、搬运粒度和数据复用。";
            }
        }
        FinishEvaluation(context, analysis, std::move(evaluation), message);
    }
}

void EvaluatePh04(const HintContext& context, HintAnalysis& analysis)
{
    constexpr double kThreshold = 16;
    auto evaluation = BeginEvaluation(context, "PH04", ACLPTI_CORE_TYPE_AIV, {"Memory"});
    if (!evaluation.skipReason.empty()) {
        FinishEvaluation(context, analysis, std::move(evaluation), {});
        return;
    }
    const auto data = ReadField(context, evaluation, "Memory", "GM_to_UB_datas(KB)", false);
    const auto count = ReadField(context, evaluation, "Memory", "aiv_mte2_instructions", false);
    if (!data || !count) {
        FinishEvaluation(context, analysis, std::move(evaluation), {});
        return;
    }
    if (*count == 0) {
        evaluation.skipReason = "zero_denominator: Memory.aiv_mte2_instructions";
        FinishEvaluation(context, analysis, std::move(evaluation), {});
        return;
    }
    const double observed = *data / *count;
    if (!std::isfinite(observed)) {
        evaluation.skipReason = "invalid_average_transfer";
        FinishEvaluation(context, analysis, std::move(evaluation), {});
        return;
    }
    evaluation.observations["avg_transfer_kb"] = observed;
    std::string message;
    if (observed < kThreshold) {
        message = "GM_to_UB_datas(KB) / aiv_mte2_instructions（平均对应搬运量）为 " + Number(observed, kThreshold) +
                  " KB/指令，小于 16 KB/指令；建议检查相邻搬运合并和连续访问。";
    }
    FinishEvaluation(context, analysis, std::move(evaluation), message);
}

void EvaluatePh05(const HintContext& context, HintAnalysis& analysis)
{
    constexpr double kThreshold = .15;
    for (const auto core : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
        auto evaluation = BeginEvaluation(context, "PH05", core, {"PipeUtilization"});
        std::string message;
        if (evaluation.skipReason.empty()) {
            const auto field = Prefix(core) + "icache_miss_rate";
            const auto observed = ReadField(context, evaluation, "PipeUtilization", field);
            if (observed && *observed > kThreshold) {
                message = field + "（ICache 缺失率）为 " + Number(*observed * 100, kThreshold * 100) +
                          "%，超过 15%；建议检查代码体积、循环展开和模板分支。";
            }
        }
        FinishEvaluation(context, analysis, std::move(evaluation), message);
    }
}

using Identity = std::tuple<uint16_t, uint16_t, aclptiCoreType>;
using Group = std::pair<aclptiCoreType, uint16_t>;

struct IdentityHash {
    size_t operator()(const Identity& identity) const
    {
        return (static_cast<size_t>(std::get<0>(identity)) << 17) | (static_cast<size_t>(std::get<1>(identity)) << 1) |
               static_cast<size_t>(std::get<2>(identity));
    }
};

std::map<aclptiCoreType, double> Ph06Imbalance(const aclptiProfilingDataResult& result, const ReportConfig& config)
{
    // The logical identity excludes the physical core: replay may have moved an instance.
    std::unordered_map<Identity, std::optional<double>, IdentityHash> instances;
    for (const auto& [key, row] : result.pmuLogs) {
        if (key.coreType != ACLPTI_CORE_TYPE_AIC && key.coreType != ACLPTI_CORE_TYPE_AIV) {
            continue;
        }
        std::optional<double> cycles;
        bool invalid = false;
        const auto add = [&](const auto& data) {
            if (data.overflow || !std::isfinite(data.totalCycles) || data.totalCycles <= 0 ||
                (cycles && *cycles != data.totalCycles)) {
                invalid = true;
            } else {
                cycles = data.totalCycles;
            }
        };
        if (row.coreData.empty()) {
            if (row.coreType == key.coreType) {
                add(row);
            }
        } else {
            for (const auto& core : row.coreData) {
                if (core.coreType == key.coreType) {
                    add(core);
                }
            }
        }
        const double frequency = key.coreType == ACLPTI_CORE_TYPE_AIC ? config.aicFrequencyMhz : config.aivFrequencyMhz;
        std::optional<double> validCycles;
        if (!invalid && cycles && std::isfinite(frequency) && frequency > 0) {
            validCycles = cycles;
        }
        if (!validCycles) {
            ASCTOOL_DEBUG(
                "[npu-compute] PH06 %s block=%u role=%u invalid_duration_or_frequency", CoreName(key.coreType).c_str(),
                key.blockId, key.subBlockId);
        }
        const auto inserted = instances.emplace(Identity{key.blockId, key.subBlockId, key.coreType}, validCycles);
        if (!inserted.second && inserted.first->second != validCycles) {
            inserted.first->second.reset();
            ASCTOOL_DEBUG(
                "[npu-compute] PH06 %s block=%u role=%u conflicting_logical_instance", CoreName(key.coreType).c_str(),
                key.blockId, key.subBlockId);
        }
    }
    struct Range {
        double min = std::numeric_limits<double>::max();
        double max = 0;
        size_t count = 0;
    };
    std::map<Group, Range> groups;
    for (const auto& [identity, cycles] : instances) {
        auto& range = groups[{std::get<2>(identity), std::get<1>(identity)}];
        if (cycles) {
            range.min = std::min(range.min, *cycles);
            range.max = std::max(range.max, *cycles);
            ++range.count;
        }
    }
    std::map<aclptiCoreType, double> values;
    for (const auto& [group, range] : groups) {
        if (range.count >= 2) {
            // Frequency is constant within a role group and cancels from the duration ratio.
            values[group.first] = std::max(values[group.first], (range.max - range.min) / range.max);
        } else {
            ASCTOOL_DEBUG(
                "[npu-compute] PH06 %s role=%u insufficient_samples count=%zu", CoreName(group.first).c_str(),
                group.second, range.count);
        }
    }
    return values;
}

void EvaluatePh06(const HintContext& context, HintAnalysis& analysis)
{
    constexpr double kThreshold = .10;
    const auto config = SummaryReportConfig(context.metadata, context.config);
    const auto imbalance = context.sections.count("PipeUtilization") ? Ph06Imbalance(context.result, config) :
                                                                       std::map<aclptiCoreType, double>{};
    for (const auto core : {ACLPTI_CORE_TYPE_AIC, ACLPTI_CORE_TYPE_AIV}) {
        auto evaluation = BeginEvaluation(context, "PH06", core, {"PipeUtilization"});
        if (!evaluation.skipReason.empty()) {
            FinishEvaluation(context, analysis, std::move(evaluation), {});
            continue;
        }
        const auto it = imbalance.find(core);
        if (it == imbalance.end()) {
            evaluation.skipReason = "invalid_duration_or_insufficient_samples";
            FinishEvaluation(context, analysis, std::move(evaluation), {});
            continue;
        }
        const double observed = it->second;
        evaluation.observations["imbalance_ratio"] = observed;
        std::string message;
        if (observed >= kThreshold) {
            message = "核间耗时分布不均匀，最长与最短耗时之差占最长耗时的 " + Number(observed * 100, kThreshold * 100) +
                      "%（提示阈值：≥10%）；建议检查任务切分、尾块分配及各核工作量。";
        }
        FinishEvaluation(context, analysis, std::move(evaluation), message);
    }
}

using HintEvaluator = void (*)(const HintContext&, HintAnalysis&);

constexpr HintEvaluator kEvaluators[] = {
    EvaluatePh01, EvaluatePh02, EvaluatePh03, EvaluatePh04, EvaluatePh05, EvaluatePh06,
};

} // namespace

HintAnalysis AnalyzePerformanceHints(
    const aclptiProfilingDataResult& result, const HintSections& sections, const ReportConfig& config,
    const KernelMetadata& metadata)
{
    const HintContext context{result, sections, config, metadata};
    HintAnalysis analysis;
    for (const auto evaluate : kEvaluators) {
        evaluate(context, analysis);
    }
    return analysis;
}

} // namespace npucompute
