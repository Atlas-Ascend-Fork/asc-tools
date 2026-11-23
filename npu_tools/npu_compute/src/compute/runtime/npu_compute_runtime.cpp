/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "file_io.h"
#include "ipc/ipc.h"
#include "runtime/npu_compute_runtime.h"
#include "report/source_counter_sqlite_writer.h"
#include <vector>

#include "artifact_sink.h"
#include "occupancy/occupancy_trace.h"
#include "hardware/hardware_device_api.h"
#include "report/report_writer.h"
#include "pm_sampling/pm_sampling_decoder.h"
#include "pm_sampling/pm_sampling_sqlite_writer.h"
#include "pm_sampling/pm_sampling_artifacts.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <map>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <dlfcn.h>
#include <unistd.h>

namespace npucompute {
namespace {

constexpr std::array<aclptiCallbackId, 9> HARDWARE_INFO_TRIGGER_CALLBACK_IDS = {
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernel,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs,
    ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction,
    ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry,
    ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad,
    ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol,
};
constexpr double A5_FALLBACK_FREQUENCY_MHZ = 1650.0;
constexpr int REPORT_SEQUENCE_WIDTH = 4;
constexpr double HZ_PER_MHZ = 1000000.0;

std::string TraceJsonString(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out << '\\' << static_cast<char>(c);
        } else if (c < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            constexpr unsigned int hexDigitBits = 4U;
            constexpr unsigned int hexDigitMask = (1U << hexDigitBits) - 1U;
            const auto byte = static_cast<unsigned int>(c);
            out << "\\u00" << hex[byte >> hexDigitBits] << hex[byte & hexDigitMask];
        } else {
            out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}

uint32_t TraceCoreId(uint8_t aicoreId, uint8_t subcoreId)
{
    return 1U + (static_cast<uint32_t>(aicoreId) << 8U) + subcoreId;
}

uint32_t TraceCoreId(const aclptiSimtWarpRecord& record) { return TraceCoreId(record.aicoreId, record.subcoreId); }

std::string WarpTraceCoreName(uint32_t pid)
{
    const uint32_t coreKey = pid - 1U;
    const uint32_t coreId = coreKey >> 8U;
    const uint32_t subcore = coreKey & static_cast<uint32_t>(UINT8_MAX);
    // WarpOccupancyAnalyzeData preserves the SDK's VEC0/VEC1/CUBE codes.
    constexpr uint32_t kVec1 = 1U;
    constexpr uint32_t kCube = 2U;
    constexpr uint32_t kVectorSubcoresPerCore = 2U;
    if (subcore <= kVec1) {
        return "aiv" + std::to_string(kVectorSubcoresPerCore * coreId + subcore);
    }
    if (subcore == kCube) {
        return "aic" + std::to_string(coreId);
    }
    return "core " + std::to_string(coreId) + " / subcore " + std::to_string(subcore);
}

constexpr uint8_t kWarpAicSubcore = 2U;
constexpr int kAivActivityTid = -1;
constexpr uint32_t kHalfWarpClockRange = std::numeric_limits<uint32_t>::max() / 2U;

struct WarpSubcoreTiming {
    uint32_t firstClock = 0;
    uint32_t clockMhz = 0;
    bool usesRatedClock = false;
};

struct WarpTraceTiming {
    std::map<uint32_t, WarpSubcoreTiming> subcores;
    double syscntMhz = 0;
};

const char* SetWarpClockFrequencies(int32_t deviceId, WarpTraceTiming& timing)
{
    DynamicHardwareDeviceApi api;
    double syscntHz = 0;
    if (!api.GetSyscntFrequencyHz(deviceId, &syscntHz)) {
        return "Device syscnt frequency unavailable";
    }
    timing.syscntMhz = syscntHz / HZ_PER_MHZ;
    uint32_t aicMhz = 0;
    uint32_t aivMhz = 0;
    // The driver can provide the current frequency for either core type independently.
    (void)api.GetCurrentAiCoreFrequencies(deviceId, &aicMhz, &aivMhz);
    uint32_t ratedAicMhz = 0;
    uint32_t ratedAivMhz = 0;
    if (aicMhz == 0 || aivMhz == 0) {
        (void)api.GetRatedAiCoreFrequencies(deviceId, &ratedAicMhz, &ratedAivMhz);
    }
    for (auto& [pid, subcore] : timing.subcores) {
        const auto subcoreId = (pid - 1U) & 0xffU;
        subcore.clockMhz = subcoreId == kWarpAicSubcore ? aicMhz : aivMhz;
        if (subcore.clockMhz == 0) {
            subcore.clockMhz = subcoreId == kWarpAicSubcore ? ratedAicMhz : ratedAivMhz;
            subcore.usesRatedClock = true;
        }
        if (subcoreId > kWarpAicSubcore || subcore.clockMhz == 0) {
            return "Current and rated subcore clock frequencies unavailable";
        }
    }

    return nullptr;
}

using WarpGroup = std::vector<const aclptiSimtWarpRecord*>;
using VfWindows = std::vector<const aclptiSimtVfAnchor*>;

struct Round {
    const aclptiSimtVfAnchor* window = nullptr;
    uint32_t firstClock = 0;
};

using WarpRounds = std::map<const aclptiSimtWarpRecord*, Round>;

std::vector<size_t> FindRoundEnds(const WarpGroup& warps, size_t windowCount)
{
    std::vector<std::pair<uint32_t, size_t>> gaps;
    for (size_t i = 1; i < warps.size(); ++i) {
        gaps.emplace_back(static_cast<uint32_t>(warps[i]->startTime - warps[i - 1]->startTime), i);
    }
    std::sort(gaps.rbegin(), gaps.rend());
    const auto cutsNeeded = windowCount - 1;
    if (cutsNeeded != 0 && (gaps[cutsNeeded - 1].first == 0 ||
                            (cutsNeeded < gaps.size() && gaps[cutsNeeded - 1].first == gaps[cutsNeeded].first))) {
        return {};
    }
    std::vector<size_t> ends;
    for (size_t i = 0; i < cutsNeeded; ++i) {
        ends.push_back(gaps[i].second);
    }
    std::sort(ends.begin(), ends.end());
    ends.push_back(warps.size());
    return ends;
}

bool WarpsFitWindow(const WarpGroup& warps, size_t first, size_t end, uint32_t mhz, double duration)
{
    std::set<uint32_t> ids;
    const auto firstClock = warps[first]->startTime;
    for (size_t i = first; i < end; ++i) {
        const auto start = static_cast<uint32_t>(warps[i]->startTime - firstClock);
        const auto endClock = static_cast<uint64_t>(start) + warps[i]->duration;
        if (!ids.insert(warps[i]->warpId).second || static_cast<double>(endClock) / mhz > duration) {
            return false;
        }
    }
    return true;
}

// Recover chronological rounds from one launch, keeping all ticks in the unsigned clock domain.
bool MatchRounds(WarpGroup warps, VfWindows windows, uint32_t origin, uint32_t mhz, double sysMhz, WarpRounds& rounds)
{
    if (windows.empty() || windows.size() > warps.size() || mhz == 0 || sysMhz <= 0) {
        return false;
    }
    const auto offset = [origin](const auto* warp) { return static_cast<uint32_t>(warp->startTime - origin); };
    std::stable_sort(warps.begin(), warps.end(), [&offset](auto* a, auto* b) { return offset(a) < offset(b); });
    if (offset(warps.back()) > kHalfWarpClockRange) {
        return false;
    }
    std::sort(windows.begin(), windows.end(), [](auto* a, auto* b) { return a->startSyscnt < b->startSyscnt; });
    const auto ends = FindRoundEnds(warps, windows.size());
    if (ends.empty()) {
        return false;
    }
    size_t first = 0;
    for (size_t j = 0; j < ends.size(); ++j) {
        const auto* window = windows[j];
        if (window->startSyscnt == 0U || window->endSyscnt < window->startSyscnt ||
            (j != 0U && window->startSyscnt < windows[j - 1]->endSyscnt)) {
            return false;
        }
        const double duration = static_cast<double>(window->endSyscnt - window->startSyscnt) / sysMhz;
        if (!WarpsFitWindow(warps, first, ends[j], mhz, duration)) {
            return false;
        }
        first = ends[j];
    }
    // Publish matches only after validating every round on the subcore.
    first = 0;
    for (size_t j = 0; j < ends.size(); ++j) {
        for (size_t i = first; i < ends[j]; ++i) {
            rounds.emplace(warps[i], Round{windows[j], warps[first]->startTime});
        }
        first = ends[j];
    }
    return true;
}

VfWindows SelectSimtWindows(const WarpGroup& warps, const VfWindows& windows)
{
    const auto warpAddress = warps.front()->vfInstrAddress;
    if (std::any_of(warps.begin(), warps.end(), [warpAddress](const auto* warp) {
            return warp->vfInstrAddress != warpAddress;
        })) {
        return {};
    }

    VfWindows candidates;
    for (const auto* window : windows) {
        if (window->kind == aclptiSimtVfKind::Simd) {
            continue;
        }
        if (!candidates.empty() && window->vfInstrAddress != candidates.front()->vfInstrAddress) {
            return {};
        }
        candidates.push_back(window);
    }
    // The Warp pass sees the rewritten ELF. Match a unique SIMT site without comparing PCs across passes.
    if (candidates.size() != windows.size() &&
        std::any_of(candidates.begin(), candidates.end(), [](const auto* window) {
            return window->kind != aclptiSimtVfKind::Simt;
        })) {
        return {};
    }
    return candidates;
}

void WriteTraceLane(std::ostream& out, uint32_t pid, int64_t tid, const std::string& label)
{
    out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":" << pid << ",\"tid\":" << tid
        << ",\"args\":{\"name\":" << TraceJsonString(label) << "}},";
}

struct WarpTraceLayout {
    WarpTraceTiming timing;
    std::map<uint32_t, VfWindows> windows;
    WarpRounds rounds;
    uint64_t origin = std::numeric_limits<uint64_t>::max();
};

WarpTraceLayout BuildWarpTraceLayout(const aclptiSimtWarpData& data)
{
    WarpTraceLayout layout;
    auto& [timing, windows, rounds, origin] = layout;
    std::map<uint32_t, WarpGroup> warps;
    for (const auto& w : data.records) {
        const auto pid = TraceCoreId(w);
        auto& group = warps[pid];
        auto& core = timing.subcores[pid];
        if (group.empty() || w.startTime - core.firstClock > kHalfWarpClockRange) {
            core.firstClock = w.startTime;
        }
        group.push_back(&w);
    }
    for (const auto& v : data.vfAnchors) {
        if (v.startSyscnt != 0U && v.endSyscnt >= v.startSyscnt) {
            windows[TraceCoreId(v.aicoreId, v.subcoreId)].push_back(&v);
            origin = std::min(origin, v.startSyscnt);
        }
    }
    const auto* frequencyError = SetWarpClockFrequencies(data.deviceId, timing);
    for (const auto& [pid, group] : warps) {
        if (frequencyError != nullptr || windows.count(pid) == 0U) {
            continue;
        }
        auto candidates = SelectSimtWindows(group, windows.at(pid));
        if (candidates.empty()) {
            continue;
        }
        const auto& core = timing.subcores.at(pid);
        MatchRounds(group, std::move(candidates), core.firstClock, core.clockMhz, timing.syscntMhz, rounds);
    }
    return layout;
}

void WriteTraceProcess(std::ostream& out, std::set<uint32_t>& named, uint32_t pid, const std::string& label)
{
    if (named.insert(pid).second) {
        out << "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":" << pid
            << ",\"tid\":0,\"args\":{\"name\":" << TraceJsonString(label) << "}},";
    }
}

void WriteBlockTraceEvents(std::ostream& out, WarpTraceLayout& layout, std::set<uint32_t>& named)
{
    auto& [timing, windows, rounds, origin] = layout;
    if (timing.syscntMhz <= 0) {
        return;
    }
    std::set<const aclptiSimtVfAnchor*> matchedWindows;
    for (const auto& entry : rounds) {
        matchedWindows.insert(entry.second.window);
    }
    for (auto& [pid, vs] : windows) {
        const auto unrelatedToSimt = [&matchedWindows](const auto* window) {
            return window->kind != aclptiSimtVfKind::Simt && matchedWindows.count(window) == 0;
        };
        vs.erase(std::remove_if(vs.begin(), vs.end(), unrelatedToSimt), vs.end());
        if (vs.empty()) {
            continue;
        }
        WriteTraceProcess(out, named, pid, WarpTraceCoreName(pid));
        WriteTraceLane(out, pid, kAivActivityTid, "block");
        out << "{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":" << pid
            << ",\"tid\":-1,\"args\":{\"sort_index\":-1}},";
        std::sort(vs.begin(), vs.end(), [](auto* a, auto* b) { return a->startSyscnt < b->startSyscnt; });
        for (auto* v : vs) {
            out << "{\"name\":\"block\",\"cat\":\"SIMT VF\",\"ph\":\"X\",\"pid\":" << pid
                << ",\"tid\":-1,\"ts\":" << (v->startSyscnt - origin) / timing.syscntMhz
                << ",\"dur\":" << (v->endSyscnt - v->startSyscnt) / timing.syscntMhz << ",\"args\":{\"start_syscnt\":\""
                << v->startSyscnt << "\",\"end_syscnt\":\"" << v->endSyscnt
                << "\",\"vf_instr_address\":" << v->vfInstrAddress << ",\"aicore_id\":" << unsigned(v->aicoreId)
                << ",\"subcore_id\":" << unsigned(v->subcoreId) << "}},";
        }
    }
}

void WriteWarpTraceEvents(
    std::ostream& out, const aclptiSimtWarpData& data, const WarpTraceLayout& layout, std::set<uint32_t>& named)
{
    const auto& timing = layout.timing;
    const auto& rounds = layout.rounds;
    const auto origin = layout.origin;
    std::set<std::pair<uint32_t, uint32_t>> lanes;
    for (size_t i = 0; i < data.records.size(); ++i) {
        const auto& w = data.records[i];
        const auto pid = TraceCoreId(w);
        const auto& core = timing.subcores.at(pid);
        const auto it = rounds.find(&w);
        const bool aligned = it != rounds.end();
        const auto lanePid = aligned ? pid : pid + (1U << 17U);
        WriteTraceProcess(out, named, lanePid, WarpTraceCoreName(pid) + (aligned ? "" : " (local clock)"));
        const auto name = "warp " + std::to_string(w.warpId);
        if (lanes.emplace(lanePid, w.warpId).second)
            WriteTraceLane(out, lanePid, w.warpId, name);
        const double mhz = core.clockMhz != 0U ? core.clockMhz : 1;
        const auto first = aligned ? it->second.firstClock : core.firstClock;
        const double start =
            (w.startTime - first) / mhz + (aligned ? (it->second.window->startSyscnt - origin) / timing.syscntMhz : 0);
        out << "{\"name\":" << TraceJsonString(name) << ",\"cat\":\"SIMT Warp\",\"ph\":\"X\",\"pid\":" << lanePid
            << ",\"tid\":" << w.warpId << ",\"ts\":" << start << ",\"dur\":" << w.duration / mhz
            << ",\"args\":{\"function_name\":" << TraceJsonString(w.functionName) << ",\"warp_id\":" << w.warpId
            << ",\"aicore_id\":" << unsigned(w.aicoreId) << ",\"subcore_id\":" << unsigned(w.subcoreId)
            << ",\"vf_instr_address\":" << w.vfInstrAddress << ",\"start_time_raw\":" << w.startTime
            << ",\"end_time_raw\":" << w.endTime << ",\"duration_raw\":" << w.duration
            << ",\"clock_frequency_mhz\":" << core.clockMhz << ",\"clock_frequency_source\":\""
            << (core.clockMhz != 0U ? (core.usesRatedClock ? "rated" : "current") : "unavailable_raw_ticks") << '"';
        if (aligned)
            out << ",\"vf_start_syscnt\":\"" << it->second.window->startSyscnt << '"';
        out << "}}";
        if (i + 1 < data.records.size())
            out << ',';
    }
}

std::string WarpTraceText(const aclptiSimtWarpData& data)
{
    auto layout = BuildWarpTraceLayout(data);
    const auto& [timing, windows, rounds, origin] = layout;
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::digits10);
    const char* mode = rounds.empty()                       ? "per_subcore_clock" :
                       rounds.size() == data.records.size() ? "same_replay_syscnt" :
                                                              "partial_same_replay_syscnt";
    out << "{\"alignmentMode\":" << TraceJsonString(mode)
        << ",\"windowSource\":\"NPUBIT_VECTOR_FUNCTION_UTILIZATION\",\"syscntFrequencyMHz\":" << timing.syscntMhz
        << ",\"alignedWarpCount\":" << rounds.size()
        << ",\"unalignedWarpCount\":" << data.records.size() - rounds.size();
    if (!windows.empty())
        out << ",\"syscntOrigin\":\"" << origin << '"';
    out << ",\"traceEvents\":[";
    std::set<uint32_t> named;
    WriteBlockTraceEvents(out, layout, named);
    WriteWarpTraceEvents(out, data, layout, named);
    out << "]}\n";
    return out.str();
}

aclptiResult PublishWarpTrace(const aclptiSimtWarpData& data, ArtifactSink& sink, const std::string& prefix)
{
    if (data.status != ACLPTI_SUCCESS || data.records.empty()) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    std::string content;
    try {
        content = WarpTraceText(data);
    } catch (const std::exception& error) {
        ASCTOOL_WARNING("[npu-compute] occupancy report generation failed: %s", error.what());
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    bool begun = false;
    try {
        sink.Begin(prefix + ipc::kSimtOccupancyFileName);
        begun = true;
        sink.Write(content);
        sink.Commit(data.records.size());
        return ACLPTI_SUCCESS;
    } catch (...) {
        if (begun) {
            sink.Abort();
        }
        return ACLPTI_ERROR_INTERNAL;
    }
}

bool IsHardwareInfoTriggerCallback(aclptiCallbackId cbid)
{
    return std::find(HARDWARE_INFO_TRIGGER_CALLBACK_IDS.begin(), HARDWARE_INFO_TRIGGER_CALLBACK_IDS.end(), cbid) !=
           HARDWARE_INFO_TRIGGER_CALLBACK_IDS.end();
}

bool CreatePrivatePipelineRoot(boost::filesystem::path* root, std::string* error)
{
    std::string path = "/tmp/npu-compute-pipeline-XXXXXX";
    if (::mkdtemp(path.data()) == nullptr) {
        *error = "create private Pipeline staging failed: " + std::string(std::strerror(errno));
        return false;
    }
    *root = std::move(path);
    return true;
}

std::string ReportPrefix(std::uint64_t sequence)
{
    if (sequence == 0) {
        return {};
    }
    std::ostringstream output;
    output << "collection-p" << static_cast<long long>(::getpid()) << '-' << std::setw(REPORT_SEQUENCE_WIDTH)
           << std::setfill('0') << sequence << '/';
    return output.str();
}

std::string_view Trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

bool ParsePositiveDouble(std::string_view text, double* result)
{
    const std::string_view trimmed = Trim(text);
    if (trimmed.empty() || result == nullptr) {
        return false;
    }
    std::string owned(trimmed);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(owned.c_str(), &end);
    if (errno != 0 || end == owned.c_str() || *end != '\0' || parsed <= 0.0 || !std::isfinite(parsed)) {
        return false;
    }
    *result = parsed;
    return true;
}

void LoadCsvDeviceInfo(ReportConfig* config, bool loadFrequencies, int32_t deviceId)
{
    if (config == nullptr) {
        return;
    }

    DynamicHardwareDeviceApi api;
    DiagnosticSink diagnostics = [](std::string_view message) {
        ASCTOOL_WARNING(
            "[npu-compute] "
            "CSV hardware config diagnostic: %.*s",
            static_cast<int>(message.size()), message.data());
    };

    std::string detectedSoc;
    if (api.GetSocName(&detectedSoc) && !detectedSoc.empty()) {
        config->socName = std::move(detectedSoc);
    }
    if (loadFrequencies) {
        std::uint32_t aiCubeFrequencyMhz = 0;
        std::uint32_t aiVectorFrequencyMhz = 0;
        CollectAiCoreFrequencies(api, &aiCubeFrequencyMhz, &aiVectorFrequencyMhz, &diagnostics, deviceId);
        if (aiCubeFrequencyMhz != 0) {
            config->aicFrequencyMhz = static_cast<double>(aiCubeFrequencyMhz);
        }
        if (aiVectorFrequencyMhz != 0) {
            config->aivFrequencyMhz = static_cast<double>(aiVectorFrequencyMhz);
        }
    }
    ASCTOOL_DEBUG(
        "[npu-compute] "
        "CSV hardware config: source=DeviceApi socName=%s fallbackFrequency=%f aicFrequency=%f "
        "aivFrequency=%f",
        config->socName.c_str(), config->frequencyMhz, config->aicFrequencyMhz, config->aivFrequencyMhz);
}

bool ValidPmuRows(const aclptiProfilingDataResult& result)
{
    for (const auto& [blockKey, row] : result.pmuLogs) {
        if (blockKey.blockId != row.blockId || blockKey.subBlockId != row.subBlockId ||
            blockKey.coreType != row.coreType || blockKey.coreId != row.coreId) {
            return false;
        }
    }
    return true;
}

void LogPipelineInput(std::uint64_t replayId, const aclptiPipelineData& replay)
{
    ASCTOOL_DEBUG(
        "[npu-compute] BIU replay input: replay=%llu device=%d status=%d receivedBytes=%llu acceptedBytes=%llu "
        "rejectedChunks=%llu failedRecords=%llu channels=%zu",
        static_cast<unsigned long long>(replayId), replay.deviceId, static_cast<int>(replay.status),
        static_cast<unsigned long long>(replay.stats.receivedBytes),
        static_cast<unsigned long long>(replay.stats.acceptedBytes),
        static_cast<unsigned long long>(replay.stats.rejectedChunkCount),
        static_cast<unsigned long long>(replay.stats.failedRecordCount), replay.channels.size());
}

void WarnIncompletePipeline(std::uint64_t replayId, std::int32_t deviceId, const BiuParseStats& stats)
{
    if (stats.incompleteIntervalCount != 0) {
        ASCTOOL_WARNING(
            "[libnpu-compute] BIU device=%d replay=%llu: skipped %llu incomplete intervals "
            "(missing start or end); retained %llu pipe intervals and %llu DFX trace events. "
            "Pipeline trace may be partial.\n",
            deviceId, static_cast<unsigned long long>(replayId),
            static_cast<unsigned long long>(stats.incompleteIntervalCount),
            static_cast<unsigned long long>(stats.intervalCount), static_cast<unsigned long long>(stats.dfxEventCount));
    }
}

void LogPipelineResult(std::uint64_t replayId, std::int32_t deviceId, aclptiResult status, const BiuParseStats& stats)
{
    ASCTOOL_DEBUG(
        "[npu-compute] "
        "BIU replay processed: device=%d replay=%llu status=%d payloadBytes=%llu paddingBytes=%llu "
        "intervals=%llu invalid=%llu incomplete=%llu",
        deviceId, static_cast<unsigned long long>(replayId), static_cast<int>(status),
        static_cast<unsigned long long>(stats.payloadBytes), static_cast<unsigned long long>(stats.paddingBytes),
        static_cast<unsigned long long>(stats.intervalCount), static_cast<unsigned long long>(stats.invalidRecordCount),
        static_cast<unsigned long long>(stats.incompleteIntervalCount));
}

void CopyArtifactBytes(std::istream& input, ArtifactSink& sink, const char* readError)
{
    constexpr std::size_t kArtifactTransferBufferBytes = 16 * 1024;
    std::array<char, kArtifactTransferBufferBytes> buffer{};
    while (input.good()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            sink.Write(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(readError);
    }
}

bool PublishPipelineFile(const boost::filesystem::path& path, ArtifactSink& sink)
{
    boost::system::error_code statusError;
    const auto status = boost::filesystem::symlink_status(path, statusError);
    if (statusError || !boost::filesystem::is_regular_file(status)) {
        return false;
    }
    std::ifstream input(path.string(), std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    bool artifactStarted = false;
    try {
        sink.Begin(".biu-staging/process-uds/" + path.filename().string());
        artifactStarted = true;
        CopyArtifactBytes(input, sink, "read Pipeline artifact failed");
        sink.Commit(0);
        artifactStarted = false;
    } catch (...) {
        if (artifactStarted) {
            sink.Abort();
        }
        throw;
    }
    return true;
}

struct PmSamplingContextInfo {
    bool emptySampling = false;
    std::uint64_t replayId = 0;
    std::shared_ptr<const aclptiPmSamplingContext> context;
    std::string error;
};

PmSamplingContextInfo ResolvePmSamplingContext(const aclptiProfilingDataResult& result)
{
    const bool mappingFailed = std::any_of(
        result.pmSamplingContexts.begin(), result.pmSamplingContexts.end(),
        [](const auto& entry) { return entry.second != nullptr && !entry.second->instructionMapError.empty(); });
    PmSamplingContextInfo info;
    info.emptySampling = result.pcSamplingData.empty() && result.pmSamplingContexts.empty() && !mappingFailed;
    if (!info.emptySampling) {
        if (result.pmSamplingContexts.size() != 1 || !result.pmSamplingContexts.begin()->second) {
            info.error = "missing or ambiguous PmSampling ELF context";
            return info;
        }
        info.replayId = result.pmSamplingContexts.begin()->first;
        info.context = result.pmSamplingContexts.begin()->second;
    }
    return info;
}

void MarkPmSamplingFailure(
    bool& complete, const std::string& prefix, const std::string& file, const std::string& stage,
    const std::string& reason)
{
    complete = false;
    std::fprintf(
        stderr, "[libnpu-compute] %s%s: PmSampling %s failed: %s; other valid artifacts are retained where possible\n",
        prefix.c_str(), file.c_str(), stage.c_str(), reason.c_str());
}

void DecodePmSampling(
    const aclptiProfilingDataResult& result, const PmSamplingContextInfo& info, PmSamplingResult& sampling,
    const std::string& prefix, bool& complete)
{
    if (!info.error.empty()) {
        MarkPmSamplingFailure(complete, prefix, ipc::kSummaryDbFileName, "sample decoding", info.error);
        return;
    }
    try {
        if (!info.emptySampling && !info.context->instructionMapError.empty()) {
            throw std::runtime_error("instruction mapping failed: " + info.context->instructionMapError);
        }
        if (!info.emptySampling) {
            const auto failedReplay = result.errorStats.failedRecordCountByReplay.find(info.replayId);
            if (failedReplay != result.errorStats.failedRecordCountByReplay.end() && failedReplay->second != 0) {
                throw std::runtime_error(
                    "sampling replay " + std::to_string(info.replayId) + " contains " +
                    std::to_string(failedReplay->second) + " failed records");
            }
            PmSamplingDecoder decoder(info.replayId, info.context);
            for (const auto& chunk : result.pcSamplingData) {
                if (!decoder.Consume(chunk)) {
                    throw std::runtime_error(decoder.Error());
                }
            }
            if (!decoder.Finish(sampling)) {
                throw std::runtime_error(decoder.Error());
            }
        }
    } catch (const std::exception& exception) {
        sampling = {};
        MarkPmSamplingFailure(complete, prefix, ipc::kSummaryDbFileName, "sample decoding", exception.what());
    }
}

struct InstructionOutputs {
    boost::filesystem::path database;
    std::vector<boost::filesystem::path> files;
    uint64_t rows = 0;
    aclptiResult sourceStatus = ACLPTI_SUCCESS;
    bool samplingComplete = true;
    bool artifactsReady = false;
};

InstructionOutputs PrepareInstructionOutputs(
    const boost::filesystem::path& directory, const aclptiProfilingDataResult& result, const std::string& prefix,
    bool collectSource, bool collectSampling)
{
    InstructionOutputs output;
    output.database = directory / ipc::kSummaryDbFileName;
    const auto& counters = result.sourceCounterData;
    const auto* sourceElf = collectSource && counters ? counters->originalElf.get() : nullptr;
    const auto sourceDisassembly = sourceElf ? std::string_view(counters->originalDisassembly) : std::string_view{};
    const auto contextInfo = collectSampling ? ResolvePmSamplingContext(result) : PmSamplingContextInfo{};
    PmSamplingResult sampling;
    uint64_t sourceRows = 0;
    std::string error;
    bool samplingDatabaseReady = false;
    if (collectSampling) {
        DecodePmSampling(result, contextInfo, sampling, prefix, output.samplingComplete);
        samplingDatabaseReady = WritePmSamplingSqlite(output.database.string(), sampling, error);
        if (!samplingDatabaseReady) {
            MarkPmSamplingFailure(output.samplingComplete, prefix, "summary.db", "database writing", error);
            boost::filesystem::remove(output.database);
        }
    }
    bool sourceDatabaseReady = false;
    if (collectSource) {
        const aclptiSourceCounterData empty;
        output.sourceStatus = counters ? counters->status : empty.status;
        sourceDatabaseReady =
            WriteSourceCounterSqlite(output.database.string(), counters ? *counters : empty, sourceRows, error);
        if (!sourceDatabaseReady) {
            ASCTOOL_ERROR("SourceCounter %ssummary.db: %s", prefix.c_str(), error.c_str());
            output.sourceStatus = ACLPTI_ERROR_TRACE_WRITE;
        }
    }
    PmSamplingArtifacts artifacts;
    output.artifactsReady = GenerateInstructionArtifacts(
        directory.string(), {contextInfo.context.get(), sourceElf, collectSource, collectSampling, sourceDisassembly},
        sampling, PmSamplingArtifactOptions{}, artifacts, error);
    if (!output.artifactsReady) {
        ASCTOOL_WARNING("Instruction report %s: %s", prefix.c_str(), error.c_str());
    }
    output.rows = (samplingDatabaseReady ? sampling.rows.size() : 0U) + sourceRows;
    if (samplingDatabaseReady || sourceDatabaseReady) {
        output.files.push_back(output.database);
    }
    if (!artifacts.disassemblyPath.empty()) {
        output.files.emplace_back(artifacts.disassemblyPath);
    }
    if (!artifacts.pcStackPath.empty()) {
        output.files.emplace_back(artifacts.pcStackPath);
    }
    return output;
}

void PublishInstructionFiles(
    ArtifactSink& sink, const std::string& prefix, const boost::filesystem::path& database,
    const std::vector<boost::filesystem::path>& files, std::uint64_t rowCount)
{
    // Transport errors abort collection because artifact boundaries and receiver state are then uncertain.
    for (const auto& file : files) {
        bool started = false;
        try {
            if (!asctools::security::ValidatePath(file.string(), asctools::security::PathKind::ExistingFile)) {
                throw std::runtime_error("cannot read instruction report artifact");
            }
            std::ifstream input(file.string(), std::ios::binary);
            if (!input) {
                throw std::runtime_error("cannot read instruction report artifact");
            }
            sink.Begin(prefix + file.filename().string());
            started = true;
            CopyArtifactBytes(input, sink, "cannot read complete instruction report artifact");
            sink.Commit(file == database ? rowCount : 0);
            started = false;
        } catch (...) {
            if (started) {
                sink.Abort();
            }
            throw;
        }
    }
}

struct InstructionReportStaging {
    boost::filesystem::path directory;
    ~InstructionReportStaging()
    {
        boost::system::error_code ignored;
        if (!directory.empty()) {
            boost::filesystem::remove_all(directory, ignored);
        }
    }
};

} // namespace

NpuComputeRuntime::~NpuComputeRuntime() { Rollback(); }

int NpuComputeRuntime::Initialize(const RuntimeConfig& config, ArtifactSink& sink, aclptiCallbackFunc sessionCallback)
{
    artifact_sink_ = &sink;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ConfigureReports(config)) {
        return kInitializeFailed;
    }
    if (pipeline_enabled_ && !PreparePipelineStaging()) {
        return kInitializeFailed;
    }
    const int collectionStatus = StartReportCollection();
    if (collectionStatus != ACLPTI_SUCCESS) {
        return collectionStatus;
    }
    const int callbackStatus = InitializeHardwareCallbacks(sessionCallback);
    if (callbackStatus != ACLPTI_SUCCESS) {
        DrainPmuConsumer();
        hardware_info_collector_.Stop();
        return callbackStatus;
    }

    auto* rangeConfig = section_config_.Params();
    rangeConfig->cacheControl = config.cacheControl;
    const aclptiResult result = aclptiRangeProfilerSetConfig(rangeConfig);
    if (result != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[npu-compute] [libnpu-compute] aclptiRangeProfilerSetConfig failed: %d"
            " code=%u",
            result, static_cast<unsigned>(static_cast<uint32_t>(result)));
        DisableHardwareCallbacks();
        DrainPmuConsumer();
        hardware_info_collector_.Stop();
        return result;
    }
    ASCTOOL_INFO("[libnpu-compute] configured sections=%s", section_config_.JoinedSections().c_str());
    return ACLPTI_SUCCESS;
}

bool NpuComputeRuntime::ConfigureReports(const RuntimeConfig& config)
{
    std::string error;
    if (!section_config_.Load(config.sections, &error)) {
        ASCTOOL_ERROR(
            "[npu-compute] [libnpu-compute] invalid sections: %s"
            " code=%u",
            error.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    csv_frequency_override_ = false;
    csv_device_info_loaded_ = false;
    csv_config_.frequencyMhz = A5_FALLBACK_FREQUENCY_MHZ;
    csv_config_.aicFrequencyMhz = 0.0;
    csv_config_.aivFrequencyMhz = 0.0;
    const auto* frequencyEnvironment = std::getenv("NPU_COMPUTE_FREQUENCY_MHZ");
    const std::string frequency = frequencyEnvironment == nullptr ? "" : frequencyEnvironment;
    if (!frequency.empty()) {
        double parsed = 0.0;
        if (!ParsePositiveDouble(frequency, &parsed)) {
            ASCTOOL_ERROR(
                "[npu-compute] [libnpu-compute] invalid NPU_COMPUTE_FREQUENCY_MHZ: %s"
                " code=%u",
                frequency.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return false;
        }
        csv_config_.frequencyMhz = parsed;
        csv_config_.aicFrequencyMhz = parsed;
        csv_config_.aivFrequencyMhz = parsed;
        csv_frequency_override_ = true;
    }
    pm_sampling_enabled_ = section_config_.Params()->collectPcSampling;
    pipeline_enabled_ = section_config_.PipelineEnabled();
    return true;
}

bool NpuComputeRuntime::PreparePipelineStaging()
{
    std::string error;
    if (!CreatePrivatePipelineRoot(&pipeline_temporary_root_, &error) ||
        CreatePipeTraceProcessStaging(pipeline_temporary_root_, &pipeline_process_directory_, &error) !=
            ACLPTI_SUCCESS ||
        WritePipeTraceManifest(pipeline_process_directory_, "collecting", ACLPTI_SUCCESS, pipeline_fragments_) !=
            ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[npu-compute] [libnpu-compute] initialize PipeTrace staging failed: %s"
            " code=%u",
            error.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    return true;
}

int NpuComputeRuntime::StartReportCollection()
{
    std::string error;
    auto consumer = PmuDataConsumer::Create(
        [this](std::shared_ptr<const aclptiProfilingDataResult> result) { return ProcessPmuData(std::move(result)); });
    const auto startStatus = consumer->Start();
    if (startStatus != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[npu-compute] PMU consumer start failed: status=%d"
            " code=%u",
            startStatus, static_cast<unsigned>(startStatus));
        return kInitializeFailed;
    }
    pmu_consumer_ = std::move(consumer);

    const bool hardwareInitialized = hardware_info_collector_.Initialize(
        boost::filesystem::current_path(),
        [this](const boost::filesystem::path&, std::string_view jsonl, std::string* publishError) {
            bool artifactStarted = false;
            try {
                artifact_sink_->Begin("HardwareInfo.jsonl");
                artifactStarted = true;
                artifact_sink_->Write(jsonl);
                artifact_sink_->Commit(1);
                return PublishResult::Published;
            } catch (const std::exception& exception) {
                if (artifactStarted) {
                    artifact_sink_->Abort();
                }
                *publishError = exception.what();
                return PublishResult::Failed;
            }
        },
        &error);
    if (!hardwareInitialized) {
        ASCTOOL_ERROR(
            "[npu-compute] [libnpu-compute] initialize HardwareInfo collector failed: %s"
            " code=%u",
            error.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        DrainPmuConsumer();
        return kInitializeFailed;
    }

    return ACLPTI_SUCCESS;
}

void NpuComputeRuntime::DrainPmuConsumer()
{
    pmu_consumer_->ShutdownAndDrain();
    pmu_consumer_.reset();
}

int NpuComputeRuntime::InitializeHardwareCallbacks(aclptiCallbackFunc sessionCallback)
{
    aclptiResult result = aclptiSubscribe(&subscriber_, sessionCallback, nullptr, nullptr);
    if (result != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[npu-compute] [libnpu-compute] aclptiSubscribe failed: %d"
            " code=%u",
            result, static_cast<unsigned>(static_cast<uint32_t>(result)));
        return result;
    }
    if (subscriber_ == nullptr) {
        ASCTOOL_ERROR(
            "[npu-compute] [libnpu-compute] aclptiSubscribe returned a null handle"
            " code=%u",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return kInitializeFailed;
    }
    ASCTOOL_INFO("[libnpu-compute] subscriber initialized");

    enabled_hardware_callback_count_ = 0;
    for (aclptiCallbackId cbid : HARDWARE_INFO_TRIGGER_CALLBACK_IDS) {
        result = aclptiEnableCallback(true, subscriber_, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid);
        if (result != ACLPTI_SUCCESS) {
            ASCTOOL_ERROR(
                "[npu-compute] [libnpu-compute] aclptiEnableCallback failed for cbid=%u: %d"
                " code=%u",
                cbid, result, static_cast<unsigned>(static_cast<uint32_t>(result)));
            DisableHardwareCallbacks();
            return result;
        }
        ++enabled_hardware_callback_count_;
        if (ASCTOOL_DEBUG_ENABLED()) {
            ASCTOOL_DEBUG("[libnpu-compute] enabled ACL PTI callback cbid=%u", cbid);
        }
    }

    return ACLPTI_SUCCESS;
}

void NpuComputeRuntime::Stop() noexcept { hardware_info_collector_.Stop(); }

void NpuComputeRuntime::Rollback() noexcept
{
    try {
        if (pmu_consumer_) {
            DrainPmuConsumer();
        }
        if (!pipeline_temporary_root_.empty()) {
            boost::system::error_code ignored;
            boost::filesystem::remove_all(pipeline_temporary_root_, ignored);
            pipeline_temporary_root_.clear();
        }
    } catch (...) {
        ASCTOOL_ERROR("[npu-compute] kernel rollback failed");
    }
}

aclptiResult NpuComputeRuntime::Submit(std::shared_ptr<const aclptiProfilingDataResult> snapshot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pmu_consumer_ ? pmu_consumer_->Submit(std::move(snapshot)) : ACLPTI_ERROR_INVALID_STATE;
}

void NpuComputeRuntime::DisableHardwareCallbacks() noexcept
{
    while (enabled_hardware_callback_count_ > 0) {
        const aclptiCallbackId cbid = HARDWARE_INFO_TRIGGER_CALLBACK_IDS[enabled_hardware_callback_count_ - 1];
        const aclptiResult result = aclptiEnableCallback(false, subscriber_, ACLPTI_CB_DOMAIN_RUNTIME_API, cbid);
        if (result != ACLPTI_SUCCESS) {
            ASCTOOL_ERROR(
                "[npu-compute] [libnpu-compute] disable ACL PTI callback failed for cbid=%u: %d"
                " code=%u",
                cbid, result, static_cast<unsigned>(static_cast<uint32_t>(result)));
        } else if (ASCTOOL_DEBUG_ENABLED()) {
            ASCTOOL_DEBUG("[libnpu-compute] disabled ACL PTI callback cbid=%u", cbid);
        }
        --enabled_hardware_callback_count_;
    }
}

aclptiResult NpuComputeRuntime::FinalizePipeline(aclptiResult status)
{
    if (pipeline_enabled_ && !pipeline_finalized_) {
        if (status == ACLPTI_SUCCESS && pipeline_fragments_.empty()) {
            status = ACLPTI_ERROR_TRACE_INCOMPLETE;
        }
        const std::string state = status == ACLPTI_SUCCESS ? "complete" : "failed";
        const aclptiResult manifestStatus = WritePipeTraceManifest(
            pipeline_process_directory_, state, status, pipeline_fragments_,
            status == ACLPTI_SUCCESS ? std::string() : "pipeline collection or processing failed");
        if (manifestStatus != ACLPTI_SUCCESS) {
            status = manifestStatus;
        }
        if (manifestStatus == ACLPTI_SUCCESS && artifact_sink_ != nullptr) {
            const aclptiResult publishStatus = PublishPipelineArtifacts();
            if (status == ACLPTI_SUCCESS) {
                status = publishStatus;
            }
        }
        pipeline_finalized_ = true;
    }
    if (!pipeline_temporary_root_.empty()) {
        boost::system::error_code cleanupError;
        boost::filesystem::remove_all(pipeline_temporary_root_, cleanupError);
        pipeline_temporary_root_.clear();
        if (cleanupError && status == ACLPTI_SUCCESS) {
            status = ACLPTI_ERROR_TRACE_WRITE;
        }
    }
    return status;
}

int NpuComputeRuntime::ShutdownAfterPtiDrain()
{
    std::shared_ptr<PmuDataConsumer> consumer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        consumer = std::move(pmu_consumer_);
    }
    aclptiResult status = ACLPTI_SUCCESS;
    if (consumer != nullptr) {
        status = consumer->ShutdownAndDrain();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status = FinalizePipeline(status);
    if (pm_sampling_enabled_ && (!PmSamplingComplete() || error_count_ != 0)) {
        if (error_count_ == 0) {
            ++error_count_;
        }
        status = ACLPTI_ERROR_TRACE_INCOMPLETE;
    }
    if (status != ACLPTI_SUCCESS) {
        ASCTOOL_ERROR(
            "[npu-compute] collection finalization failed: status=%d code=%u", status, static_cast<unsigned>(status));
    }
    return status == ACLPTI_SUCCESS ? 0 : kInitializeFailed;
}

void NpuComputeRuntime::UpdateCollectionStats(const aclptiProfilingDataResult& result)
{
    row_count_ += result.pmuLogs.size();
    error_count_ += result.errorStats.failedRecordCount;
    if (result.status != ACLPTI_SUCCESS && result.errorStats.failedRecordCount == 0) {
        ++error_count_;
    }
    std::set<std::uint64_t> replayIds;
    for (const auto& entry : result.taskLogs) {
        for (const auto& row : entry.second) {
            replayIds.insert(row.replayId);
        }
    }
    for (const auto& entry : result.pipelineData) {
        replayIds.insert(entry.first);
    }
    for (const auto& entry : result.pmSamplingContexts) {
        replayIds.insert(entry.first);
    }
    for (const auto& chunk : result.pcSamplingData) {
        replayIds.insert(chunk.replayId);
    }
    for (const auto& entry : result.replayData) {
        replayIds.insert(entry.first);
    }
    replay_count_ += replayIds.size();
}

aclptiResult NpuComputeRuntime::ProcessPipelineReplay(std::uint64_t replayId, const aclptiPipelineData& replay)
{
    LogPipelineInput(replayId, replay);
    const std::string fragmentName = "result-" + std::to_string(pipeline_result_sequence_) + "-device-" +
                                     std::to_string(replay.deviceId) + "-replay-" + std::to_string(replayId) + ".json";
    PipeTraceWriter writer;
    std::string writerError;
    aclptiResult parseStatus = writer.Begin(pipeline_process_directory_ / fragmentName, &writerError);
    BiuParseStats stats;
    if (parseStatus == ACLPTI_SUCCESS) {
        biu_clock_config_.syscntFrequencyHz = syscnt_frequency_hz_;
        if (biu_clock_config_.syscntFrequencyHz <= 0.0) {
            parseStatus = ACLPTI_ERROR_INVALID_PARAMETER;
        }
    }
    biu_clock_config_.source = "halGetDeviceInfo device oscillator and device AI-core frequencies";
    if (parseStatus == ACLPTI_SUCCESS) {
        parseStatus = ParsePipelineReplay(
            replayId, replay, biu_clock_config_, [&writer](const BiuTraceEvent& event) { return writer.Append(event); },
            &stats);
    }
    WarnIncompletePipeline(replayId, replay.deviceId, stats);
    if (parseStatus == ACLPTI_SUCCESS) {
        parseStatus = writer.Commit();
    }
    if (parseStatus == ACLPTI_SUCCESS) {
        pipeline_fragments_.push_back(PipeTraceFragmentInfo{
            pipeline_result_sequence_,
            replayId,
            replay.deviceId,
            fragmentName,
            writer.EventCount(),
        });
    } else {
        ASCTOOL_ERROR(
            "[npu-compute] Pipeline processing failed: device=%d replay=%llu status=%d"
            " code=%u",
            replay.deviceId, static_cast<unsigned long long>(replayId), parseStatus,
            static_cast<unsigned>(parseStatus));
        writer.Abort();
    }
    LogPipelineResult(replayId, replay.deviceId, parseStatus, stats);
    return parseStatus;
}

aclptiResult NpuComputeRuntime::ProcessPipelineData(const aclptiProfilingDataResult& result, const ReportConfig& config)
{
    biu_clock_config_.aicFrequencyHz = config.aicFrequencyMhz * HZ_PER_MHZ;
    biu_clock_config_.aivFrequencyHz = config.aivFrequencyMhz * HZ_PER_MHZ;
    aclptiResult status = result.pipelineData.empty() ? ACLPTI_ERROR_TRACE_INCOMPLETE : ACLPTI_SUCCESS;
    for (const auto& [replayId, replay] : result.pipelineData) {
        const aclptiResult parseStatus = ProcessPipelineReplay(replayId, replay);
        if (status == ACLPTI_SUCCESS && parseStatus != ACLPTI_SUCCESS) {
            status = parseStatus;
        }
    }
    ++pipeline_result_sequence_;
    return status;
}

KernelMetadata NpuComputeRuntime::CollectReportMetadata(
    const ReportConfig& csvConfig, bool frequencyOverride, int32_t deviceId)
{
    KernelMetadata metadata = kernel_metadata_collector_.Snapshot();
    DynamicHardwareDeviceApi api;
    metadata.hardware = CollectSummaryHardwareInfo(api, nullptr, deviceId);
    metadata.ratedAicFrequencyMhz = metadata.hardware.ratedAicFrequencyMhz;
    metadata.ratedAivFrequencyMhz = metadata.hardware.ratedAivFrequencyMhz;
    if (frequencyOverride) {
        // An explicit validated NPU_COMPUTE_FREQUENCY_MHZ remains an authoritative input.
        metadata.hardware.currentAicFrequencyMhz = csvConfig.aicFrequencyMhz;
        metadata.hardware.currentAivFrequencyMhz = csvConfig.aivFrequencyMhz;
    }
    return metadata;
}

aclptiResult NpuComputeRuntime::ProcessPmuData(std::shared_ptr<const aclptiProfilingDataResult> result)
{
    if (result == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    const bool validPmuRows = ValidPmuRows(*result);
    ReportConfig csvConfig;
    KernelMetadata metadata;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        csvConfig = csv_config_;
        metadata = report_metadata_;
    }
    UpdateCollectionStats(*result);
    const auto prefix = ReportPrefix(report_sequence_);
    auto pmuSections = section_config_.Sections();
    const auto warp = std::find(pmuSections.begin(), pmuSections.end(), "occupancy");
    const bool warpRequested = warp != pmuSections.end();
    if (warpRequested) {
        pmuSections.erase(warp);
    }
    // Every kernel result has a summary, including sampling-only launches with no PMU sections.
    // Merged metric rows are independent of the raw replay data used by occupancy and other sections.
    const aclptiResult csvStatus =
        validPmuRows ? WritePmuReport(*result, pmuSections, csvConfig, metadata, *artifact_sink_, prefix, &degraded_) :
                       ACLPTI_ERROR_ASSEMBLE;
    ++report_sequence_;
    const aclptiResult pmuResult = result->status == ACLPTI_SUCCESS ? csvStatus : result->status;
    aclptiResult firstStatus = ProcessAdditionalReports(*result, csvConfig, prefix, pmuResult);
    if (warpRequested) {
        const bool warpRequired = pmuSections.empty() && !pipeline_enabled_ && !pm_sampling_enabled_;
        const auto warpStatus = ProcessWarpReport(*result, prefix, warpRequired);
        if (firstStatus == ACLPTI_SUCCESS) {
            firstStatus = warpStatus;
        }
    }
    return firstStatus;
}

aclptiResult NpuComputeRuntime::ProcessAdditionalReports(
    const aclptiProfilingDataResult& result, const ReportConfig& config, const std::string& prefix,
    aclptiResult firstStatus)
{
    if (pipeline_enabled_) {
        const aclptiResult pipelineStatus = ProcessPipelineData(result, config);
        if (firstStatus == ACLPTI_SUCCESS) {
            firstStatus = pipelineStatus;
        }
    }
    try {
        ProcessOccupancy(result, prefix);
    } catch (const std::exception& error) {
        ASCTOOL_WARNING("[npu-compute] Occupancy processing failed: %s", error.what());
    }
    const auto& sections = section_config_.Sections();
    const bool collectSource = std::find(sections.begin(), sections.end(), "SourceCounter") != sections.end();
    if (pm_sampling_enabled_ || collectSource) {
        const auto status = ProcessInstructionReports(result, prefix, collectSource);
        if (status != ACLPTI_SUCCESS) {
            degraded_ = true;
            ++error_count_;
            if (firstStatus == ACLPTI_SUCCESS) {
                firstStatus = status;
            }
        }
    }
    return firstStatus;
}

void NpuComputeRuntime::ProcessOccupancy(const aclptiProfilingDataResult& result, const std::string& prefix) const
{
    DynamicHardwareDeviceApi deviceApi;
    std::map<int32_t, double> frequencies;
    for (const auto& [replayId, replay] : result.replayData) {
        static_cast<void>(replayId);
        if (replay.kind != aclptiReplayKind::Pmu || frequencies.count(replay.deviceId) != 0) {
            continue;
        }
        double frequency = 0;
        if (deviceApi.GetSyscntFrequencyHz(replay.deviceId, &frequency)) {
            frequencies.emplace(replay.deviceId, frequency);
        }
    }
    const auto trace = BuildOccupancyTrace(result, frequencies, report_sequence_ - 1);
    const auto status = PublishOccupancyTrace(trace, *artifact_sink_, prefix);
    if (status != ACLPTI_SUCCESS) {
        ASCTOOL_WARNING(
            "[npu-compute] Occupancy processing failed: %s code=%u", trace.diagnostic.c_str(),
            static_cast<unsigned>(status));
    } else if (!trace.diagnostic.empty()) {
        ASCTOOL_WARNING("[npu-compute] Occupancy: %s", trace.diagnostic.c_str());
    }
}

aclptiResult NpuComputeRuntime::ProcessWarpReport(
    const aclptiProfilingDataResult& result, const std::string& prefix, bool required)
{
    const auto status = result.simtWarpData ? PublishWarpTrace(*result.simtWarpData, *artifact_sink_, prefix) :
                                              ACLPTI_ERROR_PROFILING_FAILED;
    if (status == ACLPTI_SUCCESS) {
        row_count_ += result.simtWarpData->records.size();
    } else if (status == ACLPTI_ERROR_PROFILING_FAILED && !required) {
        degraded_ = true;
        return ACLPTI_SUCCESS;
    }
    return status;
}

aclptiResult NpuComputeRuntime::ProcessInstructionReports(
    const aclptiProfilingDataResult& result, const std::string& prefix, bool collectSource)
{
    try {
        InstructionReportStaging staging;
        std::string pattern = "/tmp/.npu-compute-instructions-XXXXXX";
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("cannot create instruction report staging: " + std::string(std::strerror(errno)));
        }
        staging.directory = pattern;
        const auto output =
            PrepareInstructionOutputs(staging.directory, result, prefix, collectSource, pm_sampling_enabled_);
        PublishInstructionFiles(*artifact_sink_, prefix, output.database, output.files, output.rows);
        row_count_ += output.rows;
        if (pm_sampling_enabled_) {
            if (output.samplingComplete && output.artifactsReady) {
                ++pm_sampling_completed_;
            } else {
                ++pm_sampling_failures_;
            }
        }
        return output.samplingComplete && output.artifactsReady ? output.sourceStatus : ACLPTI_ERROR_TRACE_INCOMPLETE;
    } catch (const std::exception& error) {
        ASCTOOL_ERROR("Instruction report %s failed: %s", prefix.c_str(), error.what());
    }
    if (pm_sampling_enabled_) {
        ++pm_sampling_failures_;
    }
    return ACLPTI_ERROR_TRACE_WRITE;
}

aclptiResult NpuComputeRuntime::PublishPipelineArtifacts()
{
    if (artifact_sink_ == nullptr || pipeline_process_directory_.empty()) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    try {
        std::vector<boost::filesystem::path> files{pipeline_process_directory_ / ipc::kManifestFileName};
        for (const auto& fragment : pipeline_fragments_) {
            files.push_back(pipeline_process_directory_ / fragment.file);
        }
        aclptiResult result = ACLPTI_SUCCESS;
        for (const auto& path : files) {
            if (!PublishPipelineFile(path, *artifact_sink_)) {
                if (path.filename() == ipc::kManifestFileName) {
                    return ACLPTI_ERROR_TRACE_WRITE;
                }
                result = ACLPTI_ERROR_TRACE_WRITE;
            }
        }
        return result;
    } catch (const std::exception& error) {
        ASCTOOL_ERROR(
            "[npu-compute] Pipeline artifact publication failed: %s"
            " code=%u",
            error.what(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return ACLPTI_ERROR_TRACE_WRITE;
    }
}

void NpuComputeRuntime::CaptureReportEnvironment()
{
    int32_t device = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (csv_device_info_loaded_) {
            return;
        }
        using GetDevice = aclError (*)(int32_t*);
        const auto getDevice = reinterpret_cast<GetDevice>(::dlsym(RTLD_DEFAULT, "aclrtGetDevice"));
        device = -1;
        if (getDevice != nullptr) {
            (void)getDevice(&device);
        }
        const auto reportDevice = device >= 0 ? device : 0;
        LoadCsvDeviceInfo(&csv_config_, !csv_frequency_override_, reportDevice);
        report_metadata_ = CollectReportMetadata(csv_config_, csv_frequency_override_, reportDevice);
        if (pipeline_enabled_ && device >= 0) {
            DynamicHardwareDeviceApi api(false);
            (void)api.GetSyscntFrequencyHz(device, &syscnt_frequency_hz_);
        }
        csv_device_info_loaded_ = true;
    }
    // A launch callback runs on the application's thread: never change its device or context.
    hardware_info_collector_.CollectOnKernelLaunch(device >= 0 ? device : 0);
}

void NpuComputeRuntime::ObserveRuntime(
    aclptiCallbackDomain domain, aclptiCallbackId cbid, const aclptiCallbackData* data) noexcept
{
    if (domain != ACLPTI_CB_DOMAIN_RUNTIME_API || data == nullptr || data->domain != domain || data->cbid != cbid ||
        !IsHardwareInfoTriggerCallback(cbid)) {
        return;
    }
    try {
        kernel_metadata_collector_.OnCallback(cbid, *data);
        const bool launch = cbid != ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction &&
                            cbid != ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry &&
                            cbid != ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad &&
                            cbid != ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol;
        if (launch && data->callbackSite == ACLPTI_API_EXIT && data->retval == ACL_SUCCESS) {
            CaptureReportEnvironment();
        }
    } catch (...) {
        ASCTOOL_ERROR("[npu-compute] kernel metadata collection failed");
    }
}

} // namespace npucompute
