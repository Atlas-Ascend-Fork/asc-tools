/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "biu/biu_pipeline.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

#include <unistd.h>

namespace npucompute {
namespace {
constexpr double MICROSECONDS_PER_SECOND = 1000000.0;

constexpr uint32_t END_WORD = 0xefffffffU;
constexpr uint32_t PADDING_WORD = 0xddddddddU;
constexpr uint32_t CTRL_SHIFT = 28U;
constexpr uint32_t EVENTS_SHIFT = 16U;
constexpr uint32_t CTRL_MASK = 0xfU;
constexpr uint32_t EVENTS_MASK = 0xfffU;
constexpr uint32_t TIME_MASK = 0xffffU;
constexpr std::size_t STAMP_WORD_COUNT = 4;
constexpr std::size_t PIPE_COUNT = 7;
constexpr uint64_t MAX_REPLAY_BYTES = 64U * 1024U * 1024U;
constexpr std::array<uint16_t, 8> END_MARK_SEQUENCE = {0xd88, 0xd99, 0xdaa, 0xdbb, 0xdcc, 0xddd, 0xdee, 0xdff};
constexpr std::array<const char*, 12> REGION_COLORS = {
    "grass_green",
    "green",
    "pink",
    "yellow",
    "purple",
    "blue",
    "good",
    "cyan",
    "red",
    "thread_state_uninterruptible",
    "head_dump_object_type",
    "cq_build_attempt_passed",
};

struct PipeDescriptor {
    const char* name;
    const char* color;
};

constexpr std::array<PipeDescriptor, PIPE_COUNT> PIPE_DESCRIPTORS = {{
    {"SCALAR", "startup"},
    {"VECTOR", "rail_idle"},
    {"CUBE", "rail_response"},
    {"MTE1", "thread_state_iowait"},
    {"MTE2", "yellow"},
    {"MTE3", "rail_animation"},
    {"FIXP", "thread_state_unknown"},
}};

enum class PipeState : uint8_t { Unknown, Idle, BusyUnknownStart, Busy };

struct RegionStart {
    double startUs = 0.0;
};

struct EndMarkState {
    std::size_t eventStartIndex = 0;
    std::size_t nextExpectedIndex = 0;
};

struct ParsedChannelEvents {
    std::vector<BiuTraceEvent> events;
    std::optional<EndMarkState> endMark;
};

struct StampAssembler {
    std::array<uint32_t, STAMP_WORD_COUNT> words{};
    std::size_t size = 0;

    void Reset() { size = 0; }
};

uint8_t Ctrl(uint32_t word) { return static_cast<uint8_t>((word >> CTRL_SHIFT) & CTRL_MASK); }
uint16_t Events(uint32_t word) { return static_cast<uint16_t>((word >> EVENTS_SHIFT) & EVENTS_MASK); }
uint16_t TimeData(uint32_t word) { return static_cast<uint16_t>(word & TIME_MASK); }

uint64_t Timestamp(const StampAssembler& stamp)
{
    return static_cast<uint64_t>(TimeData(stamp.words[0])) | (static_cast<uint64_t>(TimeData(stamp.words[1])) << 16U) |
           (static_cast<uint64_t>(TimeData(stamp.words[2])) << 32U) |
           (static_cast<uint64_t>(TimeData(stamp.words[3])) << 48U);
}

uint16_t BlockId(const StampAssembler& stamp)
{
    return static_cast<uint16_t>(((stamp.words[0] >> 16U) & 0xffU) | ((stamp.words[1] >> 8U) & 0xff00U));
}

bool ValidFrequency(double frequency) { return frequency > 0.0 && std::isfinite(frequency); }

aclptiResult FindOrigin(const aclptiPipelineData& replay, uint64_t* origin, BiuParseStats* stats)
{
    bool found = false;
    for (const auto& [channel, words] : replay.channels) {
        static_cast<void>(channel);
        if (words.size() > MAX_REPLAY_BYTES / sizeof(uint32_t) ||
            stats->payloadBytes > MAX_REPLAY_BYTES - words.size() * sizeof(uint32_t)) {
            return ACLPTI_ERROR_OUT_OF_MEMORY;
        }
        stats->payloadBytes += words.size() * sizeof(uint32_t);
        StampAssembler stamp;
        bool afterEnd = false;
        for (uint32_t word : words) {
            if (word == END_WORD) {
                if (stamp.size != 0) {
                    ++stats->invalidRecordCount;
                    return ACLPTI_ERROR_TRACE_INCOMPLETE;
                }
                afterEnd = true;
                continue;
            }
            if (afterEnd && word == PADDING_WORD) {
                stats->paddingBytes += sizeof(uint32_t);
                continue;
            }
            afterEnd = false;
            const uint8_t ctrl = Ctrl(word);
            if (ctrl == 14U) {
                stamp.words[stamp.size++] = word;
                if (stamp.size == STAMP_WORD_COUNT) {
                    const uint64_t timestamp = Timestamp(stamp);
                    if (!found || timestamp < *origin) {
                        *origin = timestamp;
                    }
                    found = true;
                    stamp.Reset();
                }
                continue;
            }
            if (stamp.size != 0) {
                ++stats->invalidRecordCount;
                return ACLPTI_ERROR_TRACE_INCOMPLETE;
            }
            if (ctrl >= 7U && ctrl <= 9U) {
                ++stats->invalidRecordCount;
                return ACLPTI_ERROR_DECODE;
            }
            if (ctrl >= 11U && ctrl <= 13U) {
                ++stats->invalidRecordCount;
                return ACLPTI_ERROR_DECODE;
            }
        }
        if (stamp.size != 0) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_TRACE_INCOMPLETE;
        }
    }
    return found ? ACLPTI_SUCCESS : ACLPTI_ERROR_TRACE_INCOMPLETE;
}

double ChannelFrequency(const BiuClockConfig& clocks, aclptiBiuCoreKind kind)
{
    return kind == aclptiBiuCoreKind::Aic ? clocks.aicFrequencyHz : clocks.aivFrequencyHz;
}

double CurrentTimeUs(uint64_t anchor, uint64_t origin, uint64_t cycles, double syscntFrequency, double coreFrequency)
{
    return static_cast<double>(anchor - origin) * MICROSECONDS_PER_SECOND / syscntFrequency +
           static_cast<double>(cycles) * MICROSECONDS_PER_SECOND / coreFrequency;
}

aclptiResult ParseChannel(
    uint64_t replayId, int32_t deviceId, const aclptiPipelineKey& pipelineKey, const std::vector<uint32_t>& words,
    uint64_t origin, const BiuClockConfig& clocks, ParsedChannelEvents* parsed, BiuParseStats* stats)
{
    if (parsed == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    StampAssembler stamp;
    std::array<PipeState, PIPE_COUNT> states{};
    std::array<double, PIPE_COUNT> starts{};
    bool hasAnchor = false;
    bool afterEnd = false;
    uint64_t anchor = 0;
    uint64_t cycles = 0;
    uint16_t blockId = 0;
    const BiuChannelKey channel{replayId, deviceId, pipelineKey.groupId, pipelineKey.coreKind};
    std::vector<BiuTraceEvent>& events = parsed->events;
    std::map<std::pair<BiuPipe, uint16_t>, RegionStart> regionStarts;
    std::optional<EndMarkState>& endMark = parsed->endMark;
    const double coreFrequency = ChannelFrequency(clocks, pipelineKey.coreKind);

    const auto appendEvent = [&events](const BiuTraceEvent& event) { events.push_back(event); };

    const auto processDfx = [&](BiuPipe pipe, uint16_t regionId, double now) {
        const uint64_t markEndCycles = cycles + 1U;
        const double markEndUs = CurrentTimeUs(anchor, origin, markEndCycles, clocks.syscntFrequencyHz, coreFrequency);
        appendEvent(BiuTraceEvent{
            channel, pipe, blockId, "MarkStamp" + std::to_string(regionId), "", now, markEndUs - now, true});
        if ((regionId & 0x400U) == 0) {
            return;
        }
        if ((regionId & 0x800U) != 0) {
            return;
        }
        const uint16_t baseRegionId = regionId & 0x3ffU;
        const auto key = std::make_pair(pipe, baseRegionId);
        regionStarts[key] = RegionStart{now};
    };

    const auto processDfxStop = [&](BiuPipe pipe, uint16_t regionId, double now) {
        if ((regionId & 0xc00U) != 0xc00U) {
            return;
        }
        const uint16_t baseRegionId = regionId & 0x3ffU;
        const auto key = std::make_pair(pipe, baseRegionId);
        const auto start = regionStarts.find(key);
        if (start == regionStarts.end()) {
            return;
        }
        const char* regionColor = REGION_COLORS[baseRegionId % REGION_COLORS.size()];
        appendEvent(BiuTraceEvent{
            channel, pipe, blockId, "Region" + std::to_string(baseRegionId), regionColor, start->second.startUs,
            now - start->second.startUs, true});
        regionStarts.erase(start);
    };

    const auto updateEndMarks = [&](uint16_t regionId) {
        const auto marker = std::find(END_MARK_SEQUENCE.begin(), END_MARK_SEQUENCE.end(), regionId);
        if (marker == END_MARK_SEQUENCE.end()) {
            endMark.reset();
            return;
        }
        const std::size_t markerIndex = static_cast<std::size_t>(std::distance(END_MARK_SEQUENCE.begin(), marker));
        if (markerIndex == 0U) {
            endMark = EndMarkState{events.size(), 1};
            return;
        }
        if (!endMark) {
            return;
        }
        if (endMark->nextExpectedIndex == END_MARK_SEQUENCE.size()) {
            return;
        }
        if (endMark->nextExpectedIndex != markerIndex) {
            endMark.reset();
            return;
        }
        ++endMark->nextExpectedIndex;
    };

    const auto dfxPipeForCtrl = [](uint8_t ctrl, BiuPipe* pipe) {
        if (pipe == nullptr) {
            return false;
        }
        if (ctrl <= 5U) {
            *pipe = static_cast<BiuPipe>(ctrl);
            return true;
        }
        if (ctrl == 10U) {
            *pipe = BiuPipe::Fixp;
            return true;
        }
        return false;
    };

    const auto resetSegment = [&]() {
        for (PipeState& state : states) {
            if (state == PipeState::Busy) {
                ++stats->incompleteIntervalCount;
            }
            state = PipeState::Unknown;
        }
        hasAnchor = false;
        cycles = 0;
        stamp.Reset();
    };

    for (uint32_t word : words) {
        if (word == END_WORD) {
            if (stamp.size != 0) {
                ++stats->invalidRecordCount;
                return ACLPTI_ERROR_TRACE_INCOMPLETE;
            }
            resetSegment();
            afterEnd = true;
            continue;
        }
        if (afterEnd && word == PADDING_WORD) {
            continue;
        }
        afterEnd = false;
        const uint8_t ctrl = Ctrl(word);
        if (ctrl == 14U) {
            stamp.words[stamp.size++] = word;
            if (stamp.size == STAMP_WORD_COUNT) {
                for (PipeState state : states) {
                    if (state == PipeState::Busy) {
                        ++stats->incompleteIntervalCount;
                    }
                }
                anchor = Timestamp(stamp);
                blockId = BlockId(stamp);
                cycles = 0;
                hasAnchor = true;
                states.fill(PipeState::Unknown);
                stamp.Reset();
            }
            continue;
        }
        if (stamp.size != 0) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_TRACE_INCOMPLETE;
        }
        if (ctrl >= 7U && ctrl <= 9U) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_DECODE;
        }
        if (ctrl >= 11U && ctrl <= 13U) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_DECODE;
        }
        if (!hasAnchor) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_ASSEMBLE;
        }
        if (cycles > std::numeric_limits<uint64_t>::max() - TimeData(word)) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_ASSEMBLE;
        }
        cycles += TimeData(word);
        if (ctrl != 15U) {
            BiuPipe pipe = BiuPipe::Scalar;
            if (dfxPipeForCtrl(ctrl, &pipe)) {
                const double now = CurrentTimeUs(anchor, origin, cycles, clocks.syscntFrequencyHz, coreFrequency);
                if (!std::isfinite(now) || now < 0.0) {
                    return ACLPTI_ERROR_ASSEMBLE;
                }
                const uint16_t regionId = Events(word);
                updateEndMarks(regionId);
                processDfx(pipe, regionId, now);
                processDfxStop(pipe, regionId, now);
            }
            continue;
        }

        const double now = CurrentTimeUs(anchor, origin, cycles, clocks.syscntFrequencyHz, coreFrequency);
        if (!std::isfinite(now) || now < 0.0) {
            return ACLPTI_ERROR_ASSEMBLE;
        }
        const uint16_t pipeMask = Events(word);
        for (std::size_t index = 0; index < states.size(); ++index) {
            const bool busy = (pipeMask & (1U << index)) != 0;
            PipeState& state = states[index];
            if (state == PipeState::Unknown) {
                state = busy ? PipeState::BusyUnknownStart : PipeState::Idle;
                if (busy) {
                    ++stats->incompleteIntervalCount;
                }
            } else if (state == PipeState::Idle && busy) {
                starts[index] = now;
                state = PipeState::Busy;
            } else if ((state == PipeState::Busy || state == PipeState::BusyUnknownStart) && !busy) {
                if (state == PipeState::Busy) {
                    BiuTraceEvent event{channel,
                                        static_cast<BiuPipe>(index),
                                        blockId,
                                        PIPE_DESCRIPTORS[index].name,
                                        PIPE_DESCRIPTORS[index].color,
                                        starts[index],
                                        now - starts[index],
                                        false};
                    if (!std::isfinite(event.durationUs) || event.durationUs < 0.0) {
                        return ACLPTI_ERROR_ASSEMBLE;
                    }
                    events.push_back(event);
                    ++stats->intervalCount;
                }
                state = PipeState::Idle;
            }
        }
    }
    if (stamp.size != 0) {
        ++stats->invalidRecordCount;
        return ACLPTI_ERROR_TRACE_INCOMPLETE;
    }
    for (PipeState state : states) {
        if (state == PipeState::Busy) {
            ++stats->incompleteIntervalCount;
        }
    }
    return ACLPTI_SUCCESS;
}

const char* CoreSuffix(aclptiBiuCoreKind kind)
{
    switch (kind) {
        case aclptiBiuCoreKind::Aic:
            return "cubecore";
        case aclptiBiuCoreKind::Aiv0:
            return "veccore0";
        case aclptiBiuCoreKind::Aiv1:
            return "veccore1";
    }
    return "unknown";
}

std::string EscapeJson(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        switch (character) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

aclptiResult AtomicWrite(const boost::filesystem::path& path, const std::string& content)
{
    boost::filesystem::path temporary = path;
    temporary += ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    std::ofstream output(temporary.string(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    output << content;
    output.flush();
    output.close();
    if (output.fail()) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    boost::system::error_code renameError;
    boost::filesystem::rename(temporary, path, renameError);
    if (renameError) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    return ACLPTI_SUCCESS;
}

} // namespace

aclptiResult ParsePipelineReplay(
    uint64_t replayId, const aclptiPipelineData& replay, const BiuClockConfig& clocks, const BiuTraceEventSink& sink,
    BiuParseStats* stats)
{
    if (stats == nullptr || !sink) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    *stats = {};
    if (replay.status != ACLPTI_SUCCESS) {
        return replay.status;
    }
    if (replay.channels.empty()) {
        return ACLPTI_ERROR_TRACE_INCOMPLETE;
    }
    if (replay.deviceId < 0) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    if (replay.format != aclptiBiuFormat::Chip6) {
        return ACLPTI_ERROR_NOT_SUPPORTED;
    }
    if (!ValidFrequency(clocks.syscntFrequencyHz)) {
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    uint64_t origin = 0;
    const aclptiResult originStatus = FindOrigin(replay, &origin, stats);
    if (originStatus != ACLPTI_SUCCESS) {
        return originStatus;
    }
    std::vector<ParsedChannelEvents> parsedChannels;
    parsedChannels.reserve(replay.channels.size());
    for (const auto& [key, words] : replay.channels) {
        if (key.groupId > 5U || (key.coreKind != aclptiBiuCoreKind::Aic && key.coreKind != aclptiBiuCoreKind::Aiv0 &&
                                 key.coreKind != aclptiBiuCoreKind::Aiv1)) {
            return ACLPTI_ERROR_INVALID_PARAMETER;
        }
        if (!ValidFrequency(ChannelFrequency(clocks, key.coreKind))) {
            return ACLPTI_ERROR_RESULT_UNRELIABLE;
        }
        ParsedChannelEvents parsed;
        const aclptiResult status = ParseChannel(replayId, replay.deviceId, key, words, origin, clocks, &parsed, stats);
        if (status != ACLPTI_SUCCESS) {
            return status;
        }
        parsedChannels.push_back(std::move(parsed));
    }
    for (ParsedChannelEvents& parsed : parsedChannels) {
        if (parsed.endMark && parsed.endMark->eventStartIndex < parsed.events.size()) {
            parsed.events.erase(
                parsed.events.begin() + static_cast<std::ptrdiff_t>(parsed.endMark->eventStartIndex),
                parsed.events.end());
        }
        stats->dfxEventCount += static_cast<uint64_t>(
            std::count_if(parsed.events.begin(), parsed.events.end(), [](const auto& event) { return event.isDfx; }));
        for (const BiuTraceEvent& event : parsed.events) {
            aclptiResult status = ACLPTI_ERROR_INTERNAL;
            try {
                status = sink(event);
            } catch (...) {
                return ACLPTI_ERROR_INTERNAL;
            }
            if (status != ACLPTI_SUCCESS) {
                return status;
            }
        }
    }
    return (stats->intervalCount == 0 && stats->dfxEventCount == 0 && stats->incompleteIntervalCount == 0) ?
               ACLPTI_ERROR_TRACE_INCOMPLETE :
               ACLPTI_SUCCESS;
}

PipeTraceWriter::~PipeTraceWriter() { Abort(); }

aclptiResult PipeTraceWriter::Begin(const boost::filesystem::path& outputPath, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (state_ != State::Created || outputPath.empty()) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    boost::system::error_code statusError;
    const bool outputExists = boost::filesystem::exists(outputPath, statusError);
    if (outputExists) {
        if (error != nullptr) {
            *error = "PipeTrace.json already exists";
        }
        state_ = State::Failed;
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    statusError.clear();
    outputPath_ = outputPath;
    temporaryPath_ = outputPath;
    temporaryPath_ += ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    output_ = std::make_unique<std::ofstream>(temporaryPath_.string(), std::ios::binary | std::ios::trunc);
    if (!output_->is_open()) {
        if (error != nullptr) {
            *error = "open PipeTrace temporary file failed";
        }
        state_ = State::Failed;
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    *output_ << "{\"displayTimeUnit\":\"ns\",\"profilingType\":\"op\",\"schemaVersion\":1,\"traceEvents\":[";
    if (!output_->good()) {
        state_ = State::Failed;
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    state_ = State::Writing;
    return ACLPTI_SUCCESS;
}

aclptiResult PipeTraceWriter::Append(const BiuTraceEvent& event)
{
    const std::size_t pipe = static_cast<std::size_t>(event.pipe);
    if (state_ != State::Writing || pipe >= PIPE_DESCRIPTORS.size() || event.channel.groupId > 5U ||
        event.name.empty() || !std::isfinite(event.startUs) || !std::isfinite(event.durationUs) ||
        event.startUs < 0.0 || event.durationUs < 0.0) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    const PipeDescriptor& descriptor = PIPE_DESCRIPTORS[pipe];
    std::ostringstream pid;
    pid << "group" << static_cast<unsigned int>(event.channel.groupId) << "." << CoreSuffix(event.channel.coreKind);
    const std::string eventName = event.isDfx ? event.name : descriptor.name;
    *output_ << (firstEvent_ ? "" : ",") << "{\"cname\":\""
             << EscapeJson(event.color.empty() ? descriptor.color : event.color)
             << "\",\"dur\":" << std::setprecision(std::numeric_limits<double>::max_digits10) << event.durationUs
             << ",\"name\":\"" << EscapeJson(eventName) << "\",\"ph\":\"X\",\"pid\":\"" << pid.str() << "\",\"tid\":\""
             << descriptor.name << "\",\"ts\":" << std::setprecision(std::numeric_limits<double>::max_digits10)
             << event.startUs << "}";
    if (!output_->good()) {
        state_ = State::Failed;
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    firstEvent_ = false;
    ++eventCount_;
    return ACLPTI_SUCCESS;
}

aclptiResult PipeTraceWriter::Commit()
{
    if (state_ != State::Writing) {
        return ACLPTI_ERROR_INVALID_STATE;
    }
    *output_ << "]}\n";
    output_->flush();
    output_->close();
    if (output_->fail()) {
        state_ = State::Failed;
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    boost::system::error_code renameError;
    boost::filesystem::rename(temporaryPath_, outputPath_, renameError);
    if (renameError) {
        state_ = State::Failed;
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    state_ = State::Committed;
    output_.reset();
    temporaryPath_.clear();
    return ACLPTI_SUCCESS;
}

void PipeTraceWriter::Abort() noexcept
{
    if (state_ == State::Committed) {
        return;
    }
    if (output_ != nullptr && output_->is_open()) {
        output_->close();
    }
    output_.reset();
    if (!temporaryPath_.empty()) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporaryPath_, ignored);
    }
    if (state_ == State::Writing) {
        state_ = State::Failed;
    }
}

uint64_t PipeTraceWriter::EventCount() const { return eventCount_; }

aclptiResult CreatePipeTraceProcessStaging(
    const boost::filesystem::path& collectionDirectory, boost::filesystem::path* processDirectory, std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (collectionDirectory.empty() || processDirectory == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    const boost::filesystem::path stagingRoot = collectionDirectory / ".biu-staging";
    boost::system::error_code directoryError;
    boost::filesystem::create_directory(stagingRoot, directoryError);
    if (directoryError && !boost::filesystem::is_directory(stagingRoot)) {
        if (error != nullptr) {
            *error = directoryError.message();
        }
        return ACLPTI_ERROR_TRACE_WRITE;
    }
    for (std::size_t attempt = 0; attempt < 128U; ++attempt) {
        const boost::filesystem::path candidate = stagingRoot / boost::filesystem::unique_path("process-%%%%-%%%%");
        directoryError.clear();
        if (boost::filesystem::create_directory(candidate, directoryError)) {
            *processDirectory = candidate;
            return ACLPTI_SUCCESS;
        }
        if (directoryError) {
            if (error != nullptr) {
                *error = directoryError.message();
            }
            return ACLPTI_ERROR_TRACE_WRITE;
        }
    }
    if (error != nullptr) {
        *error = "cannot allocate a unique BIU process staging directory";
    }
    return ACLPTI_ERROR_TRACE_WRITE;
}

aclptiResult WritePipeTraceManifest(
    const boost::filesystem::path& processDirectory, const std::string& state, aclptiResult status,
    const std::vector<PipeTraceFragmentInfo>& fragments, const std::string& failureMessage)
{
    if (processDirectory.empty() || (state != "collecting" && state != "complete" && state != "failed") ||
        (state == "complete" && status != ACLPTI_SUCCESS) || (state == "failed" && status == ACLPTI_SUCCESS)) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::ostringstream json;
    json << "{\"version\":1,\"state\":\"" << state << "\",\"status\":" << static_cast<int>(status)
         << ",\"fragments\":[";
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        const PipeTraceFragmentInfo& fragment = fragments[index];
        json << (index == 0 ? "" : ",") << "{\"resultSequence\":\"" << fragment.resultSequence << "\",\"replayId\":\""
             << fragment.replayId << "\",\"deviceId\":" << fragment.deviceId << ",\"file\":\""
             << EscapeJson(fragment.file) << "\",\"eventCount\":\"" << fragment.eventCount << "\"}";
    }
    json << "]";
    if (!failureMessage.empty()) {
        json << ",\"error\":\"" << EscapeJson(failureMessage) << "\"";
    }
    json << "}\n";

    return AtomicWrite(processDirectory / "manifest.json", json.str());
}

} // namespace npucompute
