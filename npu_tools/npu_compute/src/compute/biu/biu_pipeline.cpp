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

aclptiResult ReadOriginChannel(const std::vector<uint32_t>& words, uint64_t* origin, bool* found, BiuParseStats* stats)
{
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
                if (!*found || timestamp < *origin) {
                    *origin = timestamp;
                }
                *found = true;
                stamp.Reset();
            }
            continue;
        }
        if (stamp.size != 0) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_TRACE_INCOMPLETE;
        }
        if ((ctrl >= 7U && ctrl <= 9U) || (ctrl >= 11U && ctrl <= 13U)) {
            ++stats->invalidRecordCount;
            return ACLPTI_ERROR_DECODE;
        }
    }
    if (stamp.size != 0) {
        ++stats->invalidRecordCount;
        return ACLPTI_ERROR_TRACE_INCOMPLETE;
    }
    return ACLPTI_SUCCESS;
}

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
        const aclptiResult status = ReadOriginChannel(words, origin, &found, stats);
        if (status != ACLPTI_SUCCESS) {
            return status;
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

class ChannelParser {
public:
    ChannelParser(
        uint64_t replayId, int32_t deviceId, const aclptiPipelineKey& pipelineKey, const std::vector<uint32_t>& words,
        uint64_t origin, const BiuClockConfig& clocks, ParsedChannelEvents& parsed, BiuParseStats& stats)
        : words_(words),
          origin_(origin),
          clocks_(clocks),
          stats_(stats),
          channel_{replayId, deviceId, pipelineKey.groupId, pipelineKey.coreKind},
          events_(parsed.events),
          endMark_(parsed.endMark),
          coreFrequency_(ChannelFrequency(clocks, pipelineKey.coreKind))
    {}

    aclptiResult Run()
    {
        for (uint32_t word : words_) {
            const aclptiResult status = ProcessWord(word);
            if (status != ACLPTI_SUCCESS) {
                return status;
            }
        }
        return Finish();
    }

private:
    aclptiResult ProcessWord(uint32_t word)
    {
        if (word == END_WORD) {
            return EndSegment();
        }
        if (afterEnd_ && word == PADDING_WORD) {
            return ACLPTI_SUCCESS;
        }
        afterEnd_ = false;
        const uint8_t ctrl = Ctrl(word);
        if (ctrl == 14U) {
            return AddStampWord(word);
        }
        if (stamp_.size != 0) {
            return InvalidRecord(ACLPTI_ERROR_TRACE_INCOMPLETE);
        }
        if ((ctrl >= 7U && ctrl <= 9U) || (ctrl >= 11U && ctrl <= 13U)) {
            return InvalidRecord(ACLPTI_ERROR_DECODE);
        }
        if (!hasAnchor_ || cycles_ > std::numeric_limits<uint64_t>::max() - TimeData(word)) {
            return InvalidRecord(ACLPTI_ERROR_ASSEMBLE);
        }
        cycles_ += TimeData(word);
        return ctrl == 15U ? ProcessPipeWord(word) : ProcessDfxWord(word, ctrl);
    }

    aclptiResult EndSegment()
    {
        if (stamp_.size != 0) {
            return InvalidRecord(ACLPTI_ERROR_TRACE_INCOMPLETE);
        }
        ResetSegment();
        afterEnd_ = true;
        return ACLPTI_SUCCESS;
    }

    aclptiResult AddStampWord(uint32_t word)
    {
        stamp_.words[stamp_.size++] = word;
        if (stamp_.size == STAMP_WORD_COUNT) {
            CountBusyIntervals();
            anchor_ = Timestamp(stamp_);
            blockId_ = BlockId(stamp_);
            cycles_ = 0;
            hasAnchor_ = true;
            states_.fill(PipeState::Unknown);
            stamp_.Reset();
        }
        return ACLPTI_SUCCESS;
    }

    aclptiResult ProcessDfxWord(uint32_t word, uint8_t ctrl)
    {
        BiuPipe pipe = BiuPipe::Scalar;
        if (!DfxPipeForControl(ctrl, &pipe)) {
            return ACLPTI_SUCCESS;
        }
        const double now = CurrentTimeUs(anchor_, origin_, cycles_, clocks_.syscntFrequencyHz, coreFrequency_);
        if (!std::isfinite(now) || now < 0.0) {
            return ACLPTI_ERROR_ASSEMBLE;
        }
        const uint16_t regionId = Events(word);
        UpdateEndMarks(regionId);
        ProcessDfxMark(pipe, regionId, now);
        ProcessDfxStop(pipe, regionId, now);
        return ACLPTI_SUCCESS;
    }

    aclptiResult ProcessPipeWord(uint32_t word)
    {
        const double now = CurrentTimeUs(anchor_, origin_, cycles_, clocks_.syscntFrequencyHz, coreFrequency_);
        if (!std::isfinite(now) || now < 0.0) {
            return ACLPTI_ERROR_ASSEMBLE;
        }
        const uint16_t pipeMask = Events(word);
        for (std::size_t index = 0; index < states_.size(); ++index) {
            const aclptiResult status = UpdatePipe(index, (pipeMask & (1U << index)) != 0, now);
            if (status != ACLPTI_SUCCESS) {
                return status;
            }
        }
        return ACLPTI_SUCCESS;
    }

    aclptiResult UpdatePipe(std::size_t index, bool busy, double now)
    {
        PipeState& state = states_[index];
        if (state == PipeState::Unknown) {
            state = busy ? PipeState::BusyUnknownStart : PipeState::Idle;
            if (busy) {
                ++stats_.incompleteIntervalCount;
            }
        } else if (state == PipeState::Idle && busy) {
            starts_[index] = now;
            state = PipeState::Busy;
        } else if ((state == PipeState::Busy || state == PipeState::BusyUnknownStart) && !busy) {
            if (state == PipeState::Busy && !AppendPipeEvent(index, now)) {
                return ACLPTI_ERROR_ASSEMBLE;
            }
            state = PipeState::Idle;
        }
        return ACLPTI_SUCCESS;
    }

    bool AppendPipeEvent(std::size_t index, double now)
    {
        const double duration = now - starts_[index];
        if (!std::isfinite(duration) || duration < 0.0) {
            return false;
        }
        events_.push_back(BiuTraceEvent{
            channel_, static_cast<BiuPipe>(index), blockId_, PIPE_DESCRIPTORS[index].name,
            PIPE_DESCRIPTORS[index].color, starts_[index], duration, false});
        ++stats_.intervalCount;
        return true;
    }

    void ProcessDfxMark(BiuPipe pipe, uint16_t regionId, double now)
    {
        const uint64_t markEndCycles = cycles_ + 1U;
        const double markEndUs =
            CurrentTimeUs(anchor_, origin_, markEndCycles, clocks_.syscntFrequencyHz, coreFrequency_);
        events_.push_back(BiuTraceEvent{
            channel_, pipe, blockId_, "MarkStamp" + std::to_string(regionId), "", now, markEndUs - now, true});
        if ((regionId & 0x400U) != 0 && (regionId & 0x800U) == 0) {
            regionStarts_[{pipe, static_cast<uint16_t>(regionId & 0x3ffU)}] = RegionStart{now};
        }
    }

    void ProcessDfxStop(BiuPipe pipe, uint16_t regionId, double now)
    {
        if ((regionId & 0xc00U) != 0xc00U) {
            return;
        }
        const uint16_t baseRegionId = regionId & 0x3ffU;
        const auto key = std::make_pair(pipe, baseRegionId);
        const auto start = regionStarts_.find(key);
        if (start == regionStarts_.end()) {
            return;
        }
        const char* regionColor = REGION_COLORS[baseRegionId % REGION_COLORS.size()];
        events_.push_back(BiuTraceEvent{
            channel_, pipe, blockId_, "Region" + std::to_string(baseRegionId), regionColor, start->second.startUs,
            now - start->second.startUs, true});
        regionStarts_.erase(start);
    }

    void UpdateEndMarks(uint16_t regionId)
    {
        const auto marker = std::find(END_MARK_SEQUENCE.begin(), END_MARK_SEQUENCE.end(), regionId);
        if (marker == END_MARK_SEQUENCE.end()) {
            endMark_.reset();
            return;
        }
        const std::size_t markerIndex = static_cast<std::size_t>(std::distance(END_MARK_SEQUENCE.begin(), marker));
        if (markerIndex == 0U) {
            endMark_ = EndMarkState{events_.size(), 1};
        } else if (endMark_ && endMark_->nextExpectedIndex != END_MARK_SEQUENCE.size()) {
            if (endMark_->nextExpectedIndex == markerIndex) {
                ++endMark_->nextExpectedIndex;
            } else {
                endMark_.reset();
            }
        }
    }

    void ResetSegment()
    {
        for (PipeState& state : states_) {
            if (state == PipeState::Busy) {
                ++stats_.incompleteIntervalCount;
            }
            state = PipeState::Unknown;
        }
        hasAnchor_ = false;
        cycles_ = 0;
        stamp_.Reset();
    }

    void CountBusyIntervals()
    {
        for (PipeState state : states_) {
            if (state == PipeState::Busy) {
                ++stats_.incompleteIntervalCount;
            }
        }
    }

    aclptiResult InvalidRecord(aclptiResult status)
    {
        ++stats_.invalidRecordCount;
        return status;
    }

    aclptiResult Finish()
    {
        if (stamp_.size != 0) {
            return InvalidRecord(ACLPTI_ERROR_TRACE_INCOMPLETE);
        }
        CountBusyIntervals();
        return ACLPTI_SUCCESS;
    }

    static bool DfxPipeForControl(uint8_t ctrl, BiuPipe* pipe)
    {
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
    }

    const std::vector<uint32_t>& words_;
    uint64_t origin_;
    const BiuClockConfig& clocks_;
    BiuParseStats& stats_;
    BiuChannelKey channel_;
    std::vector<BiuTraceEvent>& events_;
    std::optional<EndMarkState>& endMark_;
    double coreFrequency_;
    StampAssembler stamp_;
    std::array<PipeState, PIPE_COUNT> states_{};
    std::array<double, PIPE_COUNT> starts_{};
    bool hasAnchor_ = false;
    bool afterEnd_ = false;
    uint64_t anchor_ = 0;
    uint64_t cycles_ = 0;
    uint16_t blockId_ = 0;
    std::map<std::pair<BiuPipe, uint16_t>, RegionStart> regionStarts_;
};

aclptiResult ParseChannel(
    uint64_t replayId, int32_t deviceId, const aclptiPipelineKey& pipelineKey, const std::vector<uint32_t>& words,
    uint64_t origin, const BiuClockConfig& clocks, ParsedChannelEvents* parsed, BiuParseStats* stats)
{
    if (parsed == nullptr) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    return ChannelParser(replayId, deviceId, pipelineKey, words, origin, clocks, *parsed, *stats).Run();
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
