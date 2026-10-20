/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report/report_summary.h"

#include <array>
#include <cctype>
#include <map>

namespace npucheck::detail {
namespace {

struct ToolSummary {
    std::uint64_t errors = 0;
    std::uint64_t warnings = 0;
    std::uint64_t infos = 0;
    std::uint64_t leaks = 0;
    std::uint64_t unused = 0;
    std::uint64_t hazards = 0;
    std::uint64_t deadlocks = 0;
};

using Summaries = std::map<ReportTool, ToolSummary>;
using SummaryCounter = std::uint64_t ToolSummary::*;

const std::map<ReportTemplateKey, SummaryCounter>& SummaryCounters()
{
    static const std::map<ReportTemplateKey, SummaryCounter> counters = {
        {{ReportTool::MEMCHECK, "leak"}, &ToolSummary::leaks},
        {{ReportTool::INITCHECK, "unused_memory"}, &ToolSummary::unused},
        {{ReportTool::RACECHECK, "hazard_raw"}, &ToolSummary::hazards},
        {{ReportTool::RACECHECK, "hazard_war"}, &ToolSummary::hazards},
        {{ReportTool::RACECHECK, "hazard_waw"}, &ToolSummary::hazards},
        {{ReportTool::RACECHECK, "atomic_semantic"}, &ToolSummary::hazards},
        {{ReportTool::SYNCCHECK, "deadlock"}, &ToolSummary::deadlocks},
    };
    return counters;
}

constexpr std::array<ReportTool, 5> kReportTools = {
    ReportTool::MEMCHECK, ReportTool::INITCHECK, ReportTool::RACECHECK, ReportTool::SYNCCHECK, ReportTool::SOCCHECK,
};

void IncrementPatternMetric(const ReportTemplateKey& key, ToolSummary* summary)
{
    const auto found = SummaryCounters().find(key);
    if (found != SummaryCounters().end()) {
        ++(summary->*(found->second));
    }
}

void AccumulateSummary(const ReportRecord& record, Summaries* summaries)
{
    ToolSummary& summary = (*summaries)[record.key.tool];
    if (record.severity == ReportSeverity::ERROR) {
        ++summary.errors;
    } else if (record.severity == ReportSeverity::WARNING) {
        ++summary.warnings;
    } else if (record.severity == ReportSeverity::INFO) {
        ++summary.infos;
    }
    IncrementPatternMetric(record.key, &summary);
}

void AppendSeverityCounts(const ToolSummary& summary, std::string* out)
{
    out->append(std::to_string(summary.errors));
    out->append(" errors, ");
    out->append(std::to_string(summary.warnings));
    out->append(" warnings, ");
    out->append(std::to_string(summary.infos));
    out->append(" infos");
}

void AppendToolSummary(ReportTool tool, const ToolSummary& summary, std::string* out)
{
    switch (tool) {
        case ReportTool::MEMCHECK:
            out->append("========= MEMCHECK SUMMARY: ");
            AppendSeverityCounts(summary, out);
            out->append(", ");
            out->append(std::to_string(summary.leaks));
            out->append(" leaks\n");
            return;
        case ReportTool::INITCHECK:
            out->append("========= INITCHECK SUMMARY: ");
            AppendSeverityCounts(summary, out);
            if (summary.unused != 0) {
                out->append(", ");
                out->append(std::to_string(summary.unused));
                out->append(" unused memory reports");
            }
            out->push_back('\n');
            return;
        case ReportTool::RACECHECK:
            out->append("========= RACECHECK SUMMARY: ");
            out->append(std::to_string(summary.hazards));
            out->append(" hazard displayed (");
            AppendSeverityCounts(summary, out);
            out->append(")\n");
            return;
        case ReportTool::SYNCCHECK:
            out->append("========= SYNCCHECK SUMMARY: ");
            AppendSeverityCounts(summary, out);
            out->append(", ");
            out->append(std::to_string(summary.deadlocks));
            out->append(" deadlocks\n");
            return;
        case ReportTool::SOCCHECK:
            out->append("========= SOCCHECK SUMMARY: ");
            AppendSeverityCounts(summary, out);
            out->push_back('\n');
            return;
    }
}

void AppendToolSummaries(const Summaries& summaries, std::string* out)
{
    for (const ReportTool tool : kReportTools) {
        const auto found = summaries.find(tool);
        if (found != summaries.end()) {
            AppendToolSummary(tool, found->second, out);
        }
    }
}

void AppendGlobalSummary(const Summaries& summaries, std::string* out)
{
    std::uint64_t totalErrorCount = 0;
    for (const auto& entry : summaries) {
        totalErrorCount += entry.second.errors;
    }
    out->append("========= ERROR SUMMARY: ");
    out->append(std::to_string(totalErrorCount));
    out->append(" errors\n");
    for (const ReportTool tool : kReportTools) {
        const auto found = summaries.find(tool);
        if (found == summaries.end()) {
            continue;
        }
        std::string toolName = ReportToolName(tool);
        for (char& ch : toolName) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        out->append("=========     ");
        out->append(toolName);
        out->append(": ");
        out->append(std::to_string(found->second.errors));
        out->append(" errors\n");
    }
}

} // namespace

void AppendReportSummaries(const std::vector<ReportRecord>& records, std::string* out)
{
    Summaries summaries;
    for (const ReportRecord& record : records) {
        AccumulateSummary(record, &summaries);
    }
    AppendToolSummaries(summaries, out);
    AppendGlobalSummary(summaries, out);
}

} // namespace npucheck::detail
