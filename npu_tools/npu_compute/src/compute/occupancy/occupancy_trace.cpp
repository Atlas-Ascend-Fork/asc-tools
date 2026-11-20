/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "occupancy/occupancy_trace.h"
#include "artifact_sink.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace npucompute {
namespace {
constexpr uint8_t kTaskStart = 0;
constexpr uint8_t kTaskEnd = 1;
constexpr uint8_t kBlockStart = 0x24;
constexpr uint8_t kBlockEnd = 0x25;
constexpr uint8_t kBlockPmu = 0x29;
constexpr uint8_t kTaskPmu = 0x2a;
constexpr double kMicrosecondsPerSecond = 1000000.0;
constexpr std::size_t kCoreTypeIndex = 0;
constexpr std::size_t kBlockIdIndex = 1;
constexpr std::size_t kSubBlockIdIndex = 2;
constexpr int64_t kTaskTrackId = 0;
constexpr int64_t kFirstBlockTrackId = 1;
constexpr int64_t kFirstProcessId = 1;
using TaskKey = std::pair<uint16_t, uint16_t>;
using BlockKey = std::tuple<aclptiCoreType, uint16_t, uint16_t>;
struct Boundaries {
    const aclptiTaskLogRow* start = nullptr;
    const aclptiTaskLogRow* end = nullptr;
};
struct CounterInterval {
    uint64_t start;
    uint64_t end;
};
struct Task {
    Boundaries window;
    std::map<BlockKey, Boundaries> blocks;
    std::map<BlockKey, const aclptiPmuIdentity*> identities;
    bool logical = false;
};
struct TraceOutput {
    std::ostringstream events;
    std::vector<std::string> diagnostics;
    uint64_t intervals = 0;
    int64_t nextPid = kFirstProcessId;
    bool first = true;
};

void Require(bool condition, const char* reason)
{
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

bool SupportedCore(aclptiCoreType core) { return core == ACLPTI_CORE_TYPE_AIC || core == ACLPTI_CORE_TYPE_AIV; }

std::string Quote(const std::string& text)
{
    std::ostringstream output;
    output << '"';
    for (const auto character : text) {
        if (character == '"' || character == '\\') {
            output << '\\';
        }
        output << character;
    }
    output << '"';
    return output.str();
}

void AddBoundary(Boundaries& pair, const aclptiTaskLogRow& row, uint8_t start, uint8_t end)
{
    Require(row.funcType == start || row.funcType == end, "unsupported interval boundary function");
    auto& slot = row.funcType == start ? pair.start : pair.end;
    Require(slot == nullptr, "duplicate interval start or end");
    slot = &row;
}

void ValidateBoundaries(const Boundaries& pair)
{
    Require(pair.start != nullptr && pair.end != nullptr, "missing interval start or end");
    Require(pair.end->systemCounter >= pair.start->systemCounter, "reversed system-counter interval");
    Require(pair.start->coreTypeId == pair.end->coreTypeId, "ambiguous start/end core identity");
}

std::map<TaskKey, Task> GroupReplay(uint64_t replayId, const aclptiReplayData& replay)
{
    std::map<TaskKey, Task> tasks;
    for (const auto& row : replay.taskLogs) {
        Require(row.replayId == replayId, "task boundary belongs to a different replay");
        AddBoundary(tasks[{row.taskId, row.rtStreamId}].window, row, kTaskStart, kTaskEnd);
    }
    Require(!tasks.empty(), "PMU replay contains no task boundaries");
    for (const auto& row : replay.blockLogs) {
        Require(row.replayId == replayId, "block boundary belongs to a different replay");
        Require(SupportedCore(row.coreType), "unsupported block core type");
        auto task = tasks.find({row.taskId, row.rtStreamId});
        Require(task != tasks.end(), "block has no matching task and stream");
        AddBoundary(task->second.blocks[{row.coreType, row.blockId, row.subBlockId}], row, kBlockStart, kBlockEnd);
    }
    return tasks;
}

void AssociatePmu(std::map<TaskKey, Task>& tasks, const aclptiReplayData& replay)
{
    for (const auto& record : replay.pmuRecords) {
        Require(record.funcType == kBlockPmu || record.funcType == kTaskPmu, "unsupported PMU identity function");
        Require(SupportedCore(record.coreType), "unsupported PMU core type");
        auto task = tasks.find({record.taskId, record.rtStreamId});
        Require(task != tasks.end(), "PMU identity has no matching task and stream");
        if (record.funcType == kTaskPmu) {
            continue;
        }
        const BlockKey key{record.coreType, record.blockId, record.subBlockId};
        Require(task->second.blocks.count(key) != 0, "PMU identity has no matching block interval");
        Require(task->second.identities.emplace(key, &record).second, "ambiguous duplicate PMU block identity");
    }
}

std::string Context(uint64_t replayId, const TaskKey& key)
{
    return "replay " + std::to_string(replayId) + " task " + std::to_string(key.first) + " stream " +
           std::to_string(key.second);
}

void ValidateTask(Task& task)
{
    ValidateBoundaries(task.window);
    Require(!task.blocks.empty(), "task contains no block intervals");
    for (const auto& [key, block] : task.blocks) {
        ValidateBoundaries(block);
        Require(
            block.start->systemCounter >= task.window.start->systemCounter &&
                block.end->systemCounter <= task.window.end->systemCounter,
            "block interval falls outside its task window");
        const auto identity = task.identities.find(key);
        Require(identity != task.identities.end(), "block has no matching PMU 0x29 counters");
        const auto& pmu = *identity->second;
        Require(pmu.taskEndSystemCounter >= pmu.taskStartSystemCounter, "reversed PMU block interval");
        Require(
            pmu.taskStartSystemCounter >= task.window.start->systemCounter &&
                pmu.taskEndSystemCounter <= task.window.end->systemCounter,
            "PMU block interval falls outside its task window");
        if (pmu.coreId != block.start->coreTypeId) {
            task.logical = true;
        }
    }
}

void ValidateCoreOverlap(const Task& task)
{
    if (task.logical) {
        return;
    }
    using CoreKey = std::pair<aclptiCoreType, uint8_t>;
    std::map<CoreKey, std::vector<std::pair<uint64_t, uint64_t>>> windows;
    for (const auto& [key, block] : task.blocks) {
        windows[{std::get<kCoreTypeIndex>(key), block.start->coreTypeId}].emplace_back(
            block.start->systemCounter, block.end->systemCounter);
    }
    for (auto& [core, intervals] : windows) {
        static_cast<void>(core);
        std::sort(intervals.begin(), intervals.end());
        for (size_t index = 1; index < intervals.size(); ++index) {
            Require(intervals[index].first >= intervals[index - 1].second, "overlapping intervals on an observed core");
        }
    }
}

void BeginEvent(TraceOutput& output)
{
    if (!output.first) {
        output.events << ',';
    }
    output.first = false;
}

void Metadata(TraceOutput& output, int64_t pid, int64_t tid, const std::string& kind, const std::string& label)
{
    BeginEvent(output);
    output.events << "{\"ph\":\"M\",\"name\":" << Quote(kind) << ",\"pid\":" << pid << ",\"tid\":" << tid
                  << ",\"args\":{\"name\":" << Quote(label) << "}}";
}

void ThreadSortIndex(TraceOutput& output, int64_t pid, int64_t tid, int64_t index)
{
    BeginEvent(output);
    output.events << "{\"ph\":\"M\",\"name\":\"thread_sort_index\",\"pid\":" << pid << ",\"tid\":" << tid
                  << ",\"args\":{\"sort_index\":" << index << "}}";
}

void Interval(
    TraceOutput& output, int64_t pid, int64_t tid, const std::string& name, const char* category,
    CounterInterval window, uint64_t origin, double frequencyHz, const std::string& arguments)
{
    // Subtract integers first so large absolute counters cannot erase short intervals.
    const double offset = static_cast<double>(window.start - origin);
    const double duration = static_cast<double>(window.end - window.start);
    BeginEvent(output);
    output.events << "{\"ph\":\"X\",\"cat\":" << Quote(category) << ",\"name\":" << Quote(name) << ",\"pid\":" << pid
                  << ",\"tid\":" << tid << ",\"ts\":" << offset * kMicrosecondsPerSecond / frequencyHz
                  << ",\"dur\":" << duration * kMicrosecondsPerSecond / frequencyHz << ",\"args\":{" << arguments
                  << "}}";
    ++output.intervals;
}

std::string TaskArguments(
    uint64_t launch, uint64_t replayId, const aclptiReplayData& replay, const TaskKey& key, const Task& task,
    double frequencyHz)
{
    std::ostringstream args;
    args.imbue(std::locale::classic());
    args << std::setprecision(std::numeric_limits<double>::max_digits10)
         << "\"launchSequence\":" << Quote(std::to_string(launch)) << ",\"deviceId\":" << replay.deviceId
         << ",\"replayId\":" << Quote(std::to_string(replayId))
         << ",\"replayKind\":\"Pmu\",\"replayStatus\":" << static_cast<int>(replay.status)
         << ",\"taskId\":" << key.first << ",\"streamId\":" << key.second
         << ",\"frequencySource\":\"halGetDeviceInfo INFO_TYPE_DEV_OSC_FREQUE\""
         << ",\"systemCounterFrequencyHz\":" << frequencyHz
         << ",\"taskStartCounter\":" << Quote(std::to_string(task.window.start->systemCounter))
         << ",\"taskEndCounter\":" << Quote(std::to_string(task.window.end->systemCounter))
         << ",\"receivedBytes\":" << Quote(std::to_string(replay.stats.receivedBytes))
         << ",\"acceptedBytes\":" << Quote(std::to_string(replay.stats.acceptedBytes))
         << ",\"rejectedChunks\":" << replay.stats.rejectedChunkCount
         << ",\"failedRecords\":" << replay.stats.failedRecordCount;
    return args.str();
}

std::string TaskPmuArguments(const aclptiReplayData& replay, const TaskKey& key)
{
    std::ostringstream args;
    args.imbue(std::locale::classic());
    uint64_t pmuTaskStart = 0;
    uint64_t pmuTaskEnd = 0;
    size_t pmuTaskRecordCount = 0;
    std::map<aclptiCoreType, std::pair<uint64_t, uint64_t>> pmuTaskCoreWindows;
    for (const auto& record : replay.pmuRecords) {
        if (record.funcType != kTaskPmu || record.taskId != key.first || record.rtStreamId != key.second) {
            continue;
        }
        auto [coreWindow, inserted] = pmuTaskCoreWindows.emplace(
            record.coreType, std::make_pair(record.taskStartSystemCounter, record.taskEndSystemCounter));
        if (!inserted) {
            coreWindow->second.first = std::min(coreWindow->second.first, record.taskStartSystemCounter);
            coreWindow->second.second = std::max(coreWindow->second.second, record.taskEndSystemCounter);
        }
        if (pmuTaskRecordCount == 0) {
            pmuTaskStart = record.taskStartSystemCounter;
            pmuTaskEnd = record.taskEndSystemCounter;
        } else {
            pmuTaskStart = std::min(pmuTaskStart, record.taskStartSystemCounter);
            pmuTaskEnd = std::max(pmuTaskEnd, record.taskEndSystemCounter);
        }
        ++pmuTaskRecordCount;
    }
    args << ",\"pmuTaskRecordCount\":" << pmuTaskRecordCount;
    if (pmuTaskRecordCount != 0) {
        args << ",\"pmuTaskStartCounter\":" << Quote(std::to_string(pmuTaskStart))
             << ",\"pmuTaskEndCounter\":" << Quote(std::to_string(pmuTaskEnd));
        for (const auto& [coreType, window] : pmuTaskCoreWindows) {
            const char* coreName = coreType == ACLPTI_CORE_TYPE_AIC ? "AIC" : "AIV";
            args << ",\"pmu" << coreName << "TaskStartCounter\":" << Quote(std::to_string(window.first)) << ",\"pmu"
                 << coreName << "TaskEndCounter\":" << Quote(std::to_string(window.second));
        }
    }
    return args.str();
}

std::string BlockName(const BlockKey& key, size_t index)
{
    const char* prefix = std::get<kCoreTypeIndex>(key) == ACLPTI_CORE_TYPE_AIC ? "AIC" : "AIV";
    return prefix + std::to_string(index);
}

std::string BlockArguments(const Task& task, const BlockKey& key, const Boundaries& block)
{
    const auto& record = *task.identities.at(key);
    std::ostringstream args;
    args.imbue(std::locale::classic());
    args << ",\"blockId\":" << std::get<kBlockIdIndex>(key) << ",\"subBlockId\":" << std::get<kSubBlockIdIndex>(key)
         << ",\"coreType\":" << Quote(std::get<kCoreTypeIndex>(key) == ACLPTI_CORE_TYPE_AIC ? "AIC" : "AIV")
         << ",\"coreId32\":" << static_cast<unsigned>(block.start->coreTypeId)
         << ",\"physicalCoreVerified\":false,\"identityAssociation\":"
         << Quote(task.logical ? "logical block; physical core unverified" : "consistent observed IDs")
         << ",\"blockCounterSource\":\"PMU 0x29\""
         << ",\"blockStartCounter\":" << Quote(std::to_string(record.taskStartSystemCounter))
         << ",\"blockEndCounter\":" << Quote(std::to_string(record.taskEndSystemCounter))
         << ",\"blockLogStartCounter\":" << Quote(std::to_string(block.start->systemCounter))
         << ",\"blockLogEndCounter\":" << Quote(std::to_string(block.end->systemCounter));
    args << ",\"coreId128\":" << static_cast<unsigned>(record.coreId)
         << ",\"pmuTaskStartCounter\":" << Quote(std::to_string(record.taskStartSystemCounter))
         << ",\"pmuTaskEndCounter\":" << Quote(std::to_string(record.taskEndSystemCounter))
         << ",\"pmuTotalCycles\":" << Quote(std::to_string(record.totalCycles))
         << ",\"pmuOverflow\":" << (record.overflow ? "true" : "false");
    return args.str();
}

void EmitTask(
    TraceOutput& output, uint64_t launch, uint64_t replayId, const aclptiReplayData& replay, const TaskKey& key,
    const Task& task, double frequencyHz)
{
    const int64_t pid = output.nextPid++;
    const auto context = Context(replayId, key);
    Metadata(
        output, pid, kTaskTrackId, "process_name",
        "launch " + std::to_string(launch) + " device " + std::to_string(replay.deviceId) + ' ' + context);
    Metadata(output, pid, kTaskTrackId, "thread_name", "Task window");
    ThreadSortIndex(output, pid, kTaskTrackId, kTaskTrackId);
    const auto args = TaskArguments(launch, replayId, replay, key, task, frequencyHz);
    const uint64_t origin = task.window.start->systemCounter;
    Interval(
        output, pid, kTaskTrackId, "Task", "task", {task.window.start->systemCounter, task.window.end->systemCounter},
        origin, frequencyHz, args + TaskPmuArguments(replay, key));
    int64_t lane = kFirstBlockTrackId;
    size_t aicIndex = 0;
    size_t aivIndex = 0;
    std::vector<BlockKey> orderedBlocks;
    orderedBlocks.reserve(task.blocks.size());
    for (const auto& [blockKey, block] : task.blocks) {
        static_cast<void>(block);
        orderedBlocks.push_back(blockKey);
    }
    std::sort(orderedBlocks.begin(), orderedBlocks.end());
    for (const auto& blockKey : orderedBlocks) {
        const auto& block = task.blocks.at(blockKey);
        const auto& pmu = *task.identities.at(blockKey);
        auto& index = std::get<kCoreTypeIndex>(blockKey) == ACLPTI_CORE_TYPE_AIC ? aicIndex : aivIndex;
        const auto label = BlockName(blockKey, index++);
        Metadata(output, pid, lane, "thread_name", label);
        ThreadSortIndex(output, pid, lane, lane);
        Interval(
            output, pid, lane,
            (std::get<kCoreTypeIndex>(blockKey) == ACLPTI_CORE_TYPE_AIC ? "AIC BLOCK" : "AIV BLOCK") +
                std::to_string(std::get<kBlockIdIndex>(blockKey)) +
                (std::get<kCoreTypeIndex>(blockKey) == ACLPTI_CORE_TYPE_AIV ?
                     " SUBBLOCK" + std::to_string(std::get<kSubBlockIdIndex>(blockKey)) :
                     ""),
            "block", {pmu.taskStartSystemCounter, pmu.taskEndSystemCounter}, origin, frequencyHz,
            args + BlockArguments(task, blockKey, block));
        ++lane;
    }
    if (task.logical) {
        output.diagnostics.push_back(
            context + ": conflicting 32/128-byte core IDs or overlapping PMU intervals; all block tracks are logical");
    }
}

void DowngradePmuOverlap(std::map<TaskKey, Task>& tasks, TraceOutput& output, uint64_t replayId)
{
    struct CoreWindow {
        uint64_t start;
        uint64_t end;
        TaskKey task;
    };
    std::map<std::pair<aclptiCoreType, uint8_t>, std::vector<CoreWindow>> cores;
    for (auto& [taskKey, task] : tasks) {
        if (task.logical) {
            continue;
        }
        for (const auto& [blockKey, block] : task.blocks) {
            const auto& pmu = *task.identities.at(blockKey);
            cores[{std::get<kCoreTypeIndex>(blockKey), block.start->coreTypeId}].push_back(
                {pmu.taskStartSystemCounter, pmu.taskEndSystemCounter, taskKey});
        }
    }
    bool overlap = false;
    for (auto& [core, windows] : cores) {
        static_cast<void>(core);
        std::sort(windows.begin(), windows.end(), [](const auto& left, const auto& right) {
            return std::tie(left.start, left.end) < std::tie(right.start, right.end);
        });
        for (size_t index = 1; index < windows.size(); ++index) {
            if (windows[index].start < windows[index - 1].end) {
                overlap = true;
            }
        }
    }
    if (overlap) {
        for (auto& [key, task] : tasks) {
            static_cast<void>(key);
            task.logical = true;
        }
        output.diagnostics.push_back(
            "replay " + std::to_string(replayId) +
            ": overlapping PMU counter intervals on an observed core; all tracks are logical");
    }
}

void BuildReplay(
    TraceOutput& output, uint64_t launch, uint64_t replayId, const aclptiReplayData& replay, double frequencyHz)
{
    Require(replay.status == ACLPTI_SUCCESS, "PMU replay collection failed");
    Require(
        replay.stats.failedRecordCount == 0 && replay.stats.rejectedChunkCount == 0,
        "PMU replay contains rejected or malformed data");
    Require(replay.deviceId >= 0, "PMU replay has no device identity");
    Require(std::isfinite(frequencyHz) && frequencyHz > 0, "unknown or invalid device system-counter frequency");
    auto tasks = GroupReplay(replayId, replay);
    AssociatePmu(tasks, replay);
    for (auto& entry : tasks) {
        auto& task = entry.second;
        ValidateTask(task);
        ValidateCoreOverlap(task);
    }
    DowngradePmuOverlap(tasks, output, replayId);
    for (const auto& [key, task] : tasks) {
        EmitTask(output, launch, replayId, replay, key, task, frequencyHz);
    }
}
} // namespace

//  总入口，遍历各次 PMU replay
OccupancyTrace BuildOccupancyTrace(
    const aclptiProfilingDataResult& result, const std::map<int32_t, double>& frequenciesHz, uint64_t launchSequence)
{
    OccupancyTrace trace;
    TraceOutput output;
    output.events.imbue(std::locale::classic());
    output.events << std::setprecision(std::numeric_limits<double>::max_digits10);
    try {
        std::set<int32_t> selectedDevices;
        for (const auto& [replayId, replay] : result.replayData) {
            // replayData is ordered by replayId. Occupancy uses the first PMU round per device,
            // including its failure status; later metric rounds must not replace or invalidate it.
            if (replay.kind != aclptiReplayKind::Pmu || !selectedDevices.insert(replay.deviceId).second) {
                continue;
            }
            const auto clock = frequenciesHz.find(replay.deviceId);
            Require(clock != frequenciesHz.end(), "device system-counter frequency unavailable");
            try {
                BuildReplay(output, launchSequence, replayId, replay, clock->second);
            } catch (const std::exception& error) {
                throw std::runtime_error("replay " + std::to_string(replayId) + ": " + error.what());
            }
        }
        Require(output.intervals != 0, "no PMU replay block intervals collected");
        std::ostringstream diagnostics;
        for (size_t index = 0; index < output.diagnostics.size(); ++index) {
            diagnostics << (index == 0 ? "" : ",") << Quote(output.diagnostics[index]);
            if (!trace.diagnostic.empty()) {
                trace.diagnostic += "; ";
            }
            trace.diagnostic += output.diagnostics[index];
        }
        trace.json = "{\"timeOrigin\":\"task-local\",\"status\":\"complete\",\"diagnostics\":[" + diagnostics.str() +
                     "],\"traceEvents\":[" + output.events.str() + "]}\n";
        trace.intervalCount = output.intervals;
        trace.status = ACLPTI_SUCCESS;
    } catch (const std::exception& error) {
        trace.diagnostic = error.what();
    }
    return trace;
}

aclptiResult PublishOccupancyTrace(const OccupancyTrace& trace, ArtifactSink& sink, const std::string& prefix)
{
    if (trace.status != ACLPTI_SUCCESS || trace.intervalCount == 0 || trace.json.empty()) {
        return trace.status == ACLPTI_SUCCESS ? ACLPTI_ERROR_TRACE_INCOMPLETE : trace.status;
    }
    try {
        sink.PublishOptional(prefix + "Occupancy.json", trace.json, trace.intervalCount);
        return ACLPTI_SUCCESS;
    } catch (...) {
        return ACLPTI_ERROR_TRACE_WRITE;
    }
}
} // namespace npucompute
