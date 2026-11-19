/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_ANALYSIS_BUDGET_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_ANALYSIS_BUDGET_H

#include <cstddef>
#include <initializer_list>
#include <tuple>

namespace npucheck::race {
// Defaults cover multi-core traces. Equal default values do not imply shared counters.
inline constexpr size_t DEFAULT_EVENT_BUDGET = 4'194'304;
inline constexpr size_t DEFAULT_SEGMENT_BUDGET = 8'388'608;
inline constexpr size_t DEFAULT_EDGE_BUDGET = 16'777'216;
inline constexpr size_t DEFAULT_COMPARISON_BUDGET = 10'000'000;
inline constexpr size_t DEFAULT_QUERY_WORK_BUDGET = 10'000'000;
inline constexpr size_t DEFAULT_QUERY_CACHE_BUDGET = 65'536;
inline constexpr size_t DEFAULT_ANALYSIS_WORK_BUDGET = 100'000'000;
inline constexpr size_t DEFAULT_STATE_SEGMENT_BUDGET = 1'048'576;
inline constexpr size_t DEFAULT_STATE_BYTE_BUDGET = 536'870'912;       // 512 MiB of live replay state.
inline constexpr size_t DEFAULT_RACE_REPORT_BYTE_BUDGET = 268'435'456; // 256 MiB of report objects.
inline constexpr size_t DEFAULT_RANGE_STORAGE_INTERVAL_BUDGET = 8'388'608;
inline constexpr size_t DEFAULT_RANGE_GEOMETRY_KEY_BUDGET = 65'536;

// Compatibility inputs: multiple scopes can use the same configured value,
// but each scope accounts for its own usage. Consumers use the named accessors.
struct Limits {
    size_t events = DEFAULT_EVENT_BUDGET;     // Records per launch or per independent history.
    size_t segments = DEFAULT_SEGMENT_BUDGET; // Materialized intervals, fragments or report entries per operation.
    size_t edges = DEFAULT_EDGE_BUDGET;       // Graph edges, synthetic nodes or routed SETs per graph.
    size_t comparisons = DEFAULT_COMPARISON_BUDGET;      // Candidate visits or comparisons per pass.
    size_t queryWork = DEFAULT_QUERY_WORK_BUDGET;        // Graph search work units per query.
    size_t queryCache = DEFAULT_QUERY_CACHE_BUDGET;      // Entries per reachability or replay ordering cache.
    size_t analysisWork = DEFAULT_ANALYSIS_WORK_BUDGET;  // Aggregate graph search work units per phase.
    size_t stateSegments = DEFAULT_STATE_SEGMENT_BUDGET; // Live tracker nodes per launch replay.
    size_t stateBytes = DEFAULT_STATE_BYTE_BUDGET;       // Accounted bytes per launch replay.
    size_t rangeStorageIntervals = DEFAULT_RANGE_STORAGE_INTERVAL_BUDGET; // Live shared interval capacity in slots.
    size_t rangeGeometryKeys = DEFAULT_RANGE_GEOMETRY_KEY_BUDGET;         // Entries per footprint geometry cache.
    size_t reportBytes = DEFAULT_RACE_REPORT_BYTE_BUDGET; // Accounted bytes per host/global report collection.

    // Launch input records; launch identities and session histories are counted separately.
    size_t LaunchEventBudget() const { return events; }
    size_t TrackedLaunchBudget() const { return events; }
    size_t HostAccessRecordBudget() const { return events; }
    size_t InitializationRecordBudget() const { return events; }
    size_t GlobalAccessHistoryBudget() const { return events; }

    // Interval counts: per footprint, unit-flag expansion, or initialization query, respectively.
    size_t FootprintIntervalBudget() const { return segments; }
    size_t UnitFlagFragmentBudget() const { return segments; }
    size_t InitializationIntervalBudget() const { return segments; }
    // Allocated interval slots in live shared footprints, not bytes or replay segments.
    size_t LiveRangeIntervalBudget() const { return rangeStorageIntervals; }

    // Graph edges, synthetic join/boundary nodes and routed SETs have separate checks.
    size_t GraphEdgeBudget() const { return edges; }
    size_t SyntheticGraphNodeBudget() const { return edges; }
    size_t RoutedNotificationBudget() const { return edges; }

    // Each comparison or candidate pass tracks its own count. Search work is charged
    // separately, with caps per search and per analysis phase.
    size_t CandidateComparisonBudget() const { return comparisons; }
    size_t InitializationCandidateBudget() const { return comparisons; }
    size_t ReachabilityQueryWorkBudget() const { return queryWork; }
    size_t ReachabilityPhaseWorkBudget() const { return analysisWork; }

    // Live replay state: interval nodes and accounted bytes are independent limits.
    size_t ReplayStateSegmentBudget() const { return stateSegments; }
    size_t ReplayStateByteBudget() const { return stateBytes; }

    // Host/global report objects: the lower of the entry and accounted-byte limits.
    // This does not control the size of the final serialized report.
    template <class Report>
    size_t RaceReportBudget() const
    {
        const auto byteDerivedCount = reportBytes / sizeof(Report);
        return segments < byteDerivedCount ? segments : byteDerivedCount;
    }

    // Cache capacity (soft): counts entries, not work; saturation never stops analysis.
    size_t ReachabilityCacheEntryBudget() const { return queryCache; }
    size_t ReplayOrderCacheEntryBudget() const { return queryCache; }
    size_t RangeGeometryCacheEntryBudget() const { return rangeGeometryKeys; }

    bool operator==(const Limits& other) const { return Values() == other.Values(); }

private:
    using BudgetValues =
        std::tuple<size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t, size_t>;
    BudgetValues Values() const
    {
        return {events,       segments,      edges,      comparisons,           queryWork,         queryCache,
                analysisWork, stateSegments, stateBytes, rangeStorageIntervals, rangeGeometryKeys, reportBytes};
    }
};

// Subtract remaining capacity instead of overflowing a sum of counts.
inline bool ExceedsBudget(size_t budget, std::initializer_list<size_t> counts)
{
    for (const auto count : counts) {
        if (count > budget) {
            return true;
        }
        budget -= count;
    }
    return false;
}

inline bool TryConsumeBudget(size_t budget, size_t& used, size_t amount = 1)
{
    if (ExceedsBudget(budget, {used, amount})) {
        return false;
    }
    used += amount;
    return true;
}
} // namespace npucheck::race
#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_CHECKER_RACECHECK_ANALYSIS_BUDGET_H
