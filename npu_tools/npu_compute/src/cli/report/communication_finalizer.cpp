/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/data/communication_protocol.h"
#include "communication/artifact_schema.h"
#include "communication_finalizer.h"
#include "communication/record_json.h"
#include "communication/interval_set.h"
#include "common/trace_writer.h"
#include "file_io.h"
#include <boost/algorithm/hex.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ratio>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unistd.h>

namespace npucompute::cli {
namespace {
namespace fs = boost::filesystem;
namespace comm = npucompute::communication;
using aclpti::communication::ClockMapping;
using aclpti::communication::DeviceClocks;
using aclpti::communication::kUnknownStreamId;
using aclpti::communication::Record;
using aclpti::communication::RecordKind;
constexpr long double nsPerUs = std::nano::den / std::micro::den;
constexpr int kDecimalBase = 10;
constexpr int kHexBase = 16;
constexpr unsigned kHexDigitOffset = 10U;
constexpr char kDigitFirst = '0';
constexpr char kDigitLast = '9';
constexpr char kLowerHexDigitFirst = 'a';
constexpr char kLowerHexDigitLast = 'f';
constexpr char kUpperHexDigitFirst = 'A';
constexpr char kUpperHexDigitLast = 'F';

template <typename T>
bool ParseUnsigned(std::string_view text, int base, T& result)
{
    if (text.empty() || (base != kDecimalBase && base != kHexBase))
        return false;
    T value = 0;
    for (const char character : text) {
        unsigned digit;
        if (character >= kDigitFirst && character <= kDigitLast)
            digit =
                static_cast<unsigned>(static_cast<unsigned char>(character) - static_cast<unsigned char>(kDigitFirst));
        else if (base == kHexBase && character >= kLowerHexDigitFirst && character <= kLowerHexDigitLast)
            digit = static_cast<unsigned>(
                        static_cast<unsigned char>(character) - static_cast<unsigned char>(kLowerHexDigitFirst)) +
                    kHexDigitOffset;
        else if (base == kHexBase && character >= kUpperHexDigitFirst && character <= kUpperHexDigitLast)
            digit = static_cast<unsigned>(
                        static_cast<unsigned char>(character) - static_cast<unsigned char>(kUpperHexDigitFirst)) +
                    kHexDigitOffset;
        else
            return false;
        if (digit >= static_cast<unsigned>(base) || value > (std::numeric_limits<T>::max() - digit) / base)
            return false;
        value = static_cast<T>(value * base + digit);
    }
    result = value;
    return true;
}

struct Source {
    std::string name;
    std::vector<Record> records;
    std::map<int32_t, DeviceClocks> clocks;
    std::map<std::pair<int32_t, uint32_t>, std::map<uint32_t, std::optional<size_t>>> hcclTasks;
};
struct Metrics {
    comm::Intervals compute, communication;
    bool aicpu = false, ccu = false, aiv = false;
    unsigned ModeCount() const { return static_cast<unsigned>(aicpu) + ccu + aiv; }
};
struct State {
    std::vector<std::string> issues;
    std::vector<std::string> errors;
    std::vector<Source> sources;
    std::map<int32_t, Metrics> devices;
    std::set<std::tuple<std::string, int32_t, uint32_t>> reportedHcclTasks;
    void Issue(const std::string& issue) { issues.push_back(issue); }
    void Error(const std::string& error) { errors.push_back(error); }
};
bool SafePath(const fs::path& path)
{
    fs::path current;
    for (const auto& part : fs::absolute(path)) {
        if (part == "..")
            return false;
        current /= part;
        if (fs::is_symlink(fs::symlink_status(current)))
            return false;
    }
    return path.string().find('\0') == std::string::npos;
}
comm::Tree ReadJson(const fs::path& path)
{
    constexpr std::size_t maxManifestBytes = 64U * 1024U * 1024U;
    std::string content;
    if (!SafePath(path) || !asctools::security::ReadTextFile(path.string(), content, maxManifestBytes))
        throw std::runtime_error("cannot read " + path.string());
    std::istringstream stream(content);
    comm::Tree tree;
    boost::property_tree::read_json(stream, tree);
    return tree;
}
namespace wire = aclpti::communication::protocol;

struct FileCloser {
    void operator()(FILE* file) const noexcept
    {
        if (file != nullptr)
            std::fclose(file);
    }
};

// The application artifact belongs to the same process as the process-uds staging source.
class TaskTracks {
public:
    explicit TaskTracks(const fs::path& path)
    {
        if (!SafePath(path))
            return;
        const int fd = asctools::security::OpenFile(path.string(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            return;
        const std::unique_ptr<FILE, FileCloser> input(fdopen(fd, "r"));
        if (!input) {
            close(fd);
            return;
        }
        // A row carries at most 256 raw bytes, encoded as hex, and fixed callback metadata.
        constexpr std::size_t rawRowBufferBytes = 1024;
        std::array<char, rawRowBufferBytes> line{};
        while (std::fgets(line.data(), line.size(), input.get())) {
            const std::string_view row(line.data());
            if (row.empty() || row.back() != '\n') {
                int ch;
                do {
                    ch = std::fgetc(input.get());
                } while (ch != '\n' && ch != EOF);
                continue;
            }
            ReadRow(row);
        }
    }

    uint32_t Stream(const Record& record) const
    {
        const auto found = tracks_.find({record.deviceId, record.taskId});
        return found == tracks_.end() ? kUnknownStreamId : found->second.stream;
    }

    std::string_view Name(uint64_t id) const
    {
        const auto found = names_.find(id);
        return found == names_.end() ? std::string_view{} : std::string_view(found->second);
    }

    std::string KernelName(const Record& record) const
    {
        const auto track = tracks_.find({record.deviceId, record.taskId});
        if (track == tracks_.end() || track->second.nameId == 0 || track->second.stream != record.streamId)
            return {};
        return std::string(Name(track->second.nameId));
    }

private:
    struct Track {
        uint32_t stream;
        uint64_t nameId;
    };
    using ChunkKey = std::tuple<int32_t, uint32_t, uint64_t>;
    std::map<std::pair<int32_t, uint32_t>, Track> tracks_;
    std::map<uint64_t, std::string> names_;
    std::map<ChunkKey, std::vector<uint8_t>> chunks_;
    size_t bufferedBytes_ = 0;

    void ReadRow(std::string_view line)
    {
        try {
            comm::Tree row;
            std::istringstream stream{std::string(line)};
            boost::property_tree::read_json(stream, row);
            const auto type = row.get<int32_t>("rawType", 0);
            if (type != wire::kCompact && type != wire::kAdditional)
                return;
            if (row.get<int32_t>("deviceId") != wire::kHostDeviceId)
                return;
            const ChunkKey key{type, row.get<uint32_t>("chunkModule"), row.get<uint64_t>("deviceEpoch")};
            const auto offset = row.get<size_t>("offset");
            const auto hex = row.get<std::string>("payloadHex");
            const auto last = row.get<bool>("isLastChunk");
            constexpr std::size_t hexDigitsPerByte = 2;
            if (hex.empty() || hex.size() % hexDigitsPerByte != 0 ||
                hex.size() > hexDigitsPerByte * wire::kAdditionalBytes)
                return;
            std::array<uint8_t, wire::kAdditionalBytes> payload;
            const auto size = hex.size() / hexDigitsPerByte;
            for (size_t i = 0; i < size; ++i) {
                unsigned value;
                const auto begin = hex.data() + hexDigitsPerByte * i;
                const auto end = begin + hexDigitsPerByte;
                if (!ParseUnsigned(std::string_view(begin, static_cast<std::size_t>(end - begin)), kHexBase, value))
                    return;
                payload[i] = static_cast<uint8_t>(value);
            }
            BufferChunk(key, offset, payload.data(), size, last);
        } catch (const boost::property_tree::ptree_error&) {
            // Optional naming metadata. Keep reporting durations when the raw artifact is incomplete.
        }
    }

    void BufferChunk(const ChunkKey& key, size_t offset, const uint8_t* payload, size_t size, bool last)
    {
        auto found = chunks_.find(key);
        if (found != chunks_.end() && offset == 0) {
            bufferedBytes_ -= found->second.size();
            chunks_.erase(found);
            found = chunks_.end();
        }
        if (found == chunks_.end()) {
            if (offset != 0 || chunks_.size() >= wire::kMaxStreams)
                return;
            found = chunks_.try_emplace(key).first;
        }
        auto& bytes = found->second;
        if (offset != bytes.size() || size > wire::kMaxBufferedBytes - bufferedBytes_) {
            bufferedBytes_ -= bytes.size();
            chunks_.erase(found);
            return;
        }
        bytes.insert(bytes.end(), payload, payload + size);
        bufferedBytes_ += size;
        if (last) {
            ReadChunk(std::get<int32_t>(key), bytes);
            bufferedBytes_ -= bytes.size();
            chunks_.erase(found);
        }
    }

    void ReadChunk(int32_t type, const std::vector<uint8_t>& bytes)
    {
        using wire::Read;
        if (type == wire::kAdditional) {
            if (bytes.size() < sizeof(uint16_t) || Read<uint16_t>(bytes.data(), wire::kMagicOffset) != wire::kMagic) {
                ReadDictionary({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            }
            return;
        }
        constexpr auto size = wire::kCompactBytes;
        for (size_t offset = 0; offset + size <= bytes.size(); offset += size) {
            const auto* record = bytes.data() + offset;
            wire::RuntimeTaskTrack raw{};
            if (!wire::IsRuntimeTaskTrack(record, size) || !wire::DecodeRuntimeTaskTrack(record, size, raw) ||
                !wire::HasRuntimeTaskIdentity(raw)) {
                continue;
            }
            const auto name = raw.kernelNameId == UINT64_MAX ? 0 : raw.kernelNameId;
            const auto [entry, inserted] =
                tracks_.try_emplace(std::make_pair(raw.deviceId, raw.taskId), Track{raw.streamId, name});
            if (!inserted) {
                auto& track = entry->second;
                if (track.stream != raw.streamId) {
                    track.stream = kUnknownStreamId;
                }
                if (track.stream == kUnknownStreamId || track.nameId != name) {
                    track.nameId = 0;
                }
            }
        }
    }

    void ReadDictionary(std::string_view entries)
    {
        // Type registration dictionaries also use RawDataType 17. Only numeric keys name an item.
        for (size_t end; (end = entries.find('\n')) != std::string_view::npos; entries.remove_prefix(end + 1)) {
            const auto row = entries.substr(0, end);
            const auto colon = row.find(':');
            if (colon == std::string_view::npos || colon + 1 == row.size() || row.find('\0') != std::string_view::npos)
                continue;
            uint64_t id;
            if (!ParseUnsigned(row.substr(0, colon), kDecimalBase, id))
                continue;
            const auto name = row.substr(colon + 1);
            const auto [entry, inserted] = names_.try_emplace(id, name);
            if (!inserted && entry->second != name)
                entry->second.clear();
        }
    }
};
bool IsHostCcuMetadata(const Record& record)
{
    return record.deviceId == wire::kHostDeviceId &&
           (record.kind == RecordKind::CcuTask || record.kind == RecordKind::CcuGroup ||
            record.kind == RecordKind::CcuWait);
}
bool IsHostMetadata(const Record& record)
{
    return IsHostCcuMetadata(record) ||
           (record.deviceId == wire::kHostDeviceId && record.kind == RecordKind::AicoreTimestamp);
}
auto HcclFields(const Record& task)
{
    return std::tie(
        task.itemId, task.cclTag, task.groupName, task.rankId, task.remoteRank, task.rankSize, task.stage,
        task.notifyId, task.durationEstimated, task.srcAddr, task.dstAddr, task.dataLen, task.planeId, task.opType,
        task.dataType, task.linkType, task.transportType, task.rdmaType, task.role, task.workFlowMode, task.reserve,
        task.reserves, task.type);
}
void IndexRecord(Source& source, const Record& record)
{
    if (record.kind != RecordKind::HcclTask)
        return;
    auto& streams = source.hcclTasks[{record.deviceId, record.taskId}];
    if (record.streamId == kUnknownStreamId || record.streamId == UINT16_MAX)
        return;
    const auto [it, inserted] = streams.try_emplace(record.streamId, source.records.size() - 1);
    if (inserted || !it->second)
        return;

    // Repeated reports retain the first timestamp. Conflicting metadata still provides the queue ID.
    const auto& previous = source.records[*it->second];
    if (HcclFields(previous) != HcclFields(record))
        it->second.reset();
}
size_t ReadRecords(const fs::path& path, Source& source, State& state)
{
    constexpr std::size_t maxFragmentReadBytes = 64U * 1024U * 1024U;
    constexpr std::size_t maxRecordsPerSource = 1000000;
    std::string content;
    if (!SafePath(path) || !asctools::security::ReadTextFile(path.string(), content, maxFragmentReadBytes))
        throw std::runtime_error("cannot read fragment " + path.string());
    if (!content.empty() && content.back() != '\n')
        state.Error(path.string() + ": truncated final JSONL line");
    std::istringstream input(content);
    std::string line;
    size_t count = 0;
    while (std::getline(input, line)) {
        if (source.records.size() >= maxRecordsPerSource)
            throw std::runtime_error("Communication source record limit exceeded");
        ++count;
        try {
            comm::Tree tree;
            std::istringstream row(line);
            boost::property_tree::read_json(row, tree);
            auto record = comm::ReadRecord(tree);
            if (record.kind == RecordKind::LegacyTaskInfo)
                continue;
            if (record.deviceId < 0)
                throw std::runtime_error("negative device id");
            if (!IsHostMetadata(record))
                state.devices[record.deviceId];
            source.records.push_back(std::move(record));
            IndexRecord(source, source.records.back());
        } catch (const std::exception& error) {
            state.Error(path.string() + ": record " + std::to_string(count) + ": " + error.what());
        }
    }
    return count;
}
void ReadClocks(const comm::Tree& manifest, Source& source, State& state)
{
    for (const auto& entry : manifest.get_child("clocks")) {
        DeviceClocks clocks;
        clocks.deviceId = entry.second.get<int32_t>("deviceId");
        if (clocks.deviceId < 0 || source.clocks.count(clocks.deviceId) != 0)
            throw std::runtime_error("invalid or duplicate clock device");
        clocks.aicpu = comm::ReadClock(entry.second.get_child("aicpu"));
        clocks.task = comm::ReadClock(entry.second.get_child("task"));
        clocks.ccu = comm::ReadClock(entry.second.get_child("ccu"));
        source.clocks.emplace(clocks.deviceId, clocks);
        state.devices[clocks.deviceId];
    }
}
void ReadManifest(
    const fs::path& path, Source& source, State& state, std::map<std::string, uint64_t>& expected,
    uint64_t& expectedTotal)
{
    const auto manifest = ReadJson(path / "manifest.json");
    if (manifest.get<unsigned>("schema") != comm::kManifestSchemaVersion)
        throw std::runtime_error("unsupported manifest schema");
    if (!manifest.get<bool>("complete") || manifest.get<uint64_t>("dropped") != 0 ||
        manifest.get<uint64_t>("errors") != 0)
        state.Error(source.name + ": incomplete collection");
    const auto diagnostic = manifest.get<std::string>("diagnostic", "");
    if (!diagnostic.empty())
        state.Error(source.name + ": " + diagnostic);
    expectedTotal = manifest.get<uint64_t>("records");
    if (manifest.get_child("files").size() > communication::kMaxFragments)
        throw std::runtime_error("Communication fragment limit exceeded");
    for (const auto& entry : manifest.get_child("files")) {
        const auto name = entry.second.get<std::string>("file");
        if (!communication::IsRecordFileName(name) ||
            !expected.emplace(name, entry.second.get<uint64_t>("records")).second)
            throw std::runtime_error("invalid fragment name");
    }
    ReadClocks(manifest, source, state);
}
void ReadSource(const fs::path& path, State& state)
{
    Source source;
    source.name = path.filename().string();
    std::map<std::string, uint64_t> expected;
    uint64_t expectedTotal = 0;
    try {
        ReadManifest(path, source, state, expected, expectedTotal);
    } catch (const std::exception& error) {
        state.Error(source.name + ": " + error.what());
        state.sources.push_back(std::move(source));
        return;
    }
    uint64_t actualTotal = 0;
    std::set<std::string> seen;
    std::vector<fs::path> fragments;
    for (const auto& entry : fs::directory_iterator(path))
        fragments.push_back(entry.path());
    // The receiver names fragments with an unpadded sequence number.
    std::sort(fragments.begin(), fragments.end(), [](const auto& a, const auto& b) {
        const auto left = a.filename().string();
        const auto right = b.filename().string();
        return std::make_tuple(left.size(), left) < std::make_tuple(right.size(), right);
    });
    for (const auto& fragment : fragments) {
        const auto name = fragment.filename().string();
        if (name.rfind("records", 0) != 0 || fragment.extension() != ".jsonl")
            continue;
        if (expected.count(name) == 0) {
            state.Error(source.name + ": unlisted fragment " + name);
            continue;
        }
        try {
            const auto count = ReadRecords(fragment, source, state);
            actualTotal += count;
            seen.insert(name);
            if (expected.at(name) != count)
                state.Error(source.name + ": fragment count mismatch " + name);
        } catch (const std::exception& error) {
            state.Error(error.what());
        }
    }
    if (actualTotal != expectedTotal || seen.size() != expected.size())
        state.Error(source.name + ": manifest record/file count mismatch");
    if (source.records.empty())
        state.Issue(source.name + ": no valid Communication records");
    state.sources.push_back(std::move(source));
}
void ReadSources(const fs::path& directory, State& state)
{
    const auto staging = directory / communication::kStagingDirectory;
    if (!SafePath(staging) || !fs::is_directory(staging)) {
        state.Error("Communication staging directory missing or unsafe");
        return;
    }
    constexpr std::size_t maxSources = 256;
    for (const auto& entry : fs::directory_iterator(staging)) {
        if (state.sources.size() >= maxSources)
            throw std::runtime_error("Communication source limit exceeded");
        if (!SafePath(entry.path()) || !fs::is_directory(entry.path())) {
            state.Error("unsafe or invalid Communication source " + entry.path().string());
            continue;
        }
        try {
            ReadSource(entry.path(), state);
        } catch (const std::exception& error) {
            state.Error(error.what());
        }
    }
    std::sort(state.sources.begin(), state.sources.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    if (state.sources.empty())
        state.Issue("no Communication sources");
}
enum class Group { AiCpuCompute, AiCpuCommunication, CcuCompute, CcuCommunication, Fusion, Aiv, Count };
enum class FusionTrackOrder : uint64_t { Compute, Communication, Overlap };
constexpr auto kAivComputeTrack = "Compute";
constexpr uint64_t kAivComputeOrder = 0;
std::string GroupName(Group group, int32_t device)
{
    static const std::array<const char*, static_cast<size_t>(Group::Count)> names = {"AiCpu-Compute",
                                                                                     "AiCpu-Communication",
                                                                                     "Ccu-Compute",
                                                                                     "Ccu-Communication",
                                                                                     "Computation-Communication-Fusion",
                                                                                     "AIV"};
    return std::string(names[static_cast<size_t>(group)]) + "-Npu" + std::to_string(device);
}
class Trace {
public:
    explicit Trace(TraceFileOutput& output) : writer_(output, TraceLayout::Array) {}
    int64_t Process(int32_t device, Group group)
    {
        constexpr int64_t firstProcessId = 1;
        constexpr size_t processMetadataTid = 0;
        constexpr auto existingGroups = static_cast<int64_t>(Group::Aiv);
        constexpr auto aivProcessBase = wire::kHostDeviceId * existingGroups + firstProcessId;
        const auto pid = group == Group::Aiv ? aivProcessBase + device :
                                               device * existingGroups + static_cast<int64_t>(group) + firstProcessId;
        if (!processes_.insert(pid).second)
            return pid;
        constexpr std::array displayOrder = {Group::AiCpuCompute, Group::AiCpuCommunication,
                                             Group::CcuCompute,   Group::CcuCommunication,
                                             Group::Aiv,          Group::Fusion};
        const auto groupOrder = std::find(displayOrder.begin(), displayOrder.end(), group) - displayOrder.begin();
        const auto sortIndex = device * static_cast<int64_t>(displayOrder.size()) + groupOrder + firstProcessId;
        Metadata(pid, processMetadataTid, "process_name", "name", TraceQuote(GroupName(group, device)));
        Metadata(pid, processMetadataTid, "process_sort_index", "sort_index", std::to_string(sortIndex));
        return pid;
    }
    void Track(int32_t device, Group group, const std::string& name, uint64_t order)
    {
        const auto pid = Process(device, group);
        const auto key = std::make_pair(pid, name);
        if (tracks_.count(key) != 0)
            return;
        const auto tid = tracks_.size() + 1;
        tracks_.emplace(key, tid);
        Metadata(pid, tid, "thread_name", "name", TraceQuote(name));
        Metadata(pid, tid, "thread_sort_index", "sort_index", std::to_string(order));
    }
    void Duration(
        int32_t device, Group group, const std::string& track, const std::string& name, const comm::Interval& interval,
        const std::string& args, uint64_t order = 0)
    {
        Track(device, group, track, order);
        const auto pid = Process(device, group);
        const auto tid = tracks_.at({pid, track});
        writer_.Duration(
            {std::to_string(pid), std::to_string(tid), name, TraceNumber(interval.start / nsPerUs),
             TraceNumber((interval.end - interval.start) / nsPerUs), std::nullopt, args});
    }
    void Finish() { writer_.Finish(); }

private:
    void Metadata(int64_t pid, size_t tid, const std::string& name, const std::string& field, const std::string& value)
    {
        writer_.Metadata({std::to_string(pid), std::to_string(tid), name, field, value});
    }
    TraceWriter writer_;
    std::set<int64_t> processes_;
    std::map<std::pair<int64_t, std::string>, size_t> tracks_;
};
bool Convert(const ClockMapping& clock, uint64_t counter, long double& result)
{
    if (!clock.calibrated || clock.frequencyHz == 0)
        return false;
    result = static_cast<long double>(clock.hostNsAnchor) +
             (static_cast<long double>(counter) - static_cast<long double>(clock.counterAnchor)) * std::nano::den /
                 clock.frequencyHz;
    return std::isfinite(result);
}
bool Interval(const ClockMapping& clock, uint64_t start, uint64_t end, comm::Interval& interval)
{
    return end >= start && Convert(clock, start, interval.start) && Convert(clock, end, interval.end);
}
constexpr const char* unavailable = "N/A";

std::string Hex(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

template <size_t N>
std::string EnumName(uint32_t value, const std::array<const char*, N>& names)
{
    if (value == UINT8_MAX || value == UINT32_MAX) {
        return unavailable;
    }
    if (value < names.size() && names[value] != nullptr) {
        return names[value];
    }
    return "Unknown(" + std::to_string(value) + ")";
}

constexpr std::array reduceNames{"SUM", "MUL", "MAX", "MIN"};
constexpr std::array dataTypeNames{
    "INT8",  "INT16",   "INT32",   "FP16",   "FP32",  "INT64",  "UINT64",
    "UINT8", "UINT16",  "UINT32",  "FP64",   "BFP16", "INT128", static_cast<const char*>(nullptr),
    "HIF8",  "FP8E4M3", "FP8E5M2", "FP8E8M0"};
constexpr std::array linkNames{"ON_CHIP", "HCCS",          "PCIE", "ROCE", "SIO",
                               "HCCS_SW", "STANDARD_ROCE", "UB",   "UBoE", "Reserved"};
constexpr std::array transportNames{"SDMA", "RDMA", "LOCAL", "UB", "ROCE"};
constexpr std::array rdmaNames{"RDMA_SEND_NOTIFY", "RDMA_SEND_PAYLOAD"};
constexpr std::array roleNames{"DST", "SRC"};
constexpr std::array<const char*, static_cast<size_t>(aclptiDavidSqeType::End)> sqeNames{
    "AIC",         "AIV",         "FUSION", "PLACE_HOLDER", "AICPU_H", "AICPU_D", "NOTIFY_RECORD",
    "NOTIFY_WAIT", "WRITE_VALUE", "UBDMA",  "ASYNCDMA",     "SDMA",    "VPC",     "JPEGE",
    "JPEGD",       "CMO",         "CCU",    nullptr,        "NSC",     "DSS",     "COND"};

std::string AvailableId(uint64_t id, uint64_t invalid) { return id == invalid ? unavailable : std::to_string(id); }

comm::Tree BlockArgs(uint32_t stream, uint32_t task, const comm::Interval& interval)
{
    comm::Tree args;
    args.put("StreamId", AvailableId(stream, kUnknownStreamId));
    args.put("TaskId", task);
    args.put("StartTime", TraceNumber(interval.start / nsPerUs));
    args.put("EndTime", TraceNumber(interval.end / nsPerUs));
    return args;
}

bool HasHcclPlane(const Record* metadata)
{
    return metadata && metadata->planeId != UINT16_MAX && metadata->planeId != UINT32_MAX;
}

void PutHcclEnums(comm::Tree& args, const Record& m, uint32_t sqeType, std::string_view name)
{
    constexpr auto notifyRecord = static_cast<uint32_t>(aclptiDavidSqeType::NotifyRecord);
    constexpr auto notifyWait = static_cast<uint32_t>(aclptiDavidSqeType::NotifyWait);
    constexpr auto writeValue = static_cast<uint32_t>(aclptiDavidSqeType::WriteValue);
    constexpr uint32_t rdmaTransport = 1, defaultRdmaType = 0, defaultReduceType = 0;
    const bool nonReduce = sqeType == notifyRecord || sqeType == notifyWait || sqeType == writeValue ||
                           name == "Notify_Record" || name == "Notify_Wait" || name == "Memcpy";
    args.put(
        "opType", nonReduce && m.opType == defaultReduceType ? "N/A (reported 0)" : EnumName(m.opType, reduceNames));
    args.put("dataType", EnumName(m.dataType, dataTypeNames));
    args.put("linkType", EnumName(m.linkType, linkNames));
    args.put("transportType", EnumName(m.transportType, transportNames));
    const bool nonRdma = m.transportType < transportNames.size() && m.transportType != rdmaTransport;
    args.put(
        "rdmaType", nonRdma && m.rdmaType == defaultRdmaType ? "N/A (reported 0)" : EnumName(m.rdmaType, rdmaNames));
    args.put("role", EnumName(m.role, roleNames));
}

void PutHcclReserves(comm::Tree& args, const Record& metadata)
{
    if (metadata.reserve.has_value()) {
        args.put("reserve", Hex(*metadata.reserve));
    }
    if (metadata.reserves.has_value()) {
        std::string bytes = "0x";
        boost::algorithm::hex_lower(*metadata.reserves, std::back_inserter(bytes));
        args.put("reserves", bytes);
    }
}

void PutHcclMetadata(comm::Tree& args, const Record& m)
{
    const bool legacy = m.type == wire::kAicpuLegacyHcclTask;
    args.put("itemId", AvailableId(m.itemId, UINT64_MAX));
    args.put("cclTag", AvailableId(m.cclTag, UINT64_MAX));
    args.put("groupName", AvailableId(m.groupName, UINT64_MAX));
    args.put("localRank", AvailableId(m.rankId, UINT32_MAX));
    args.put("remoteRank", AvailableId(m.remoteRank, UINT32_MAX));
    args.put("rankSize", AvailableId(m.rankSize, UINT32_MAX));
    args.put("stage", AvailableId(m.stage, UINT32_MAX));
    if (m.notifyId != UINT32_MAX && m.notifyId != UINT64_MAX) {
        args.put("notifyID", m.notifyId);
    }
    if (!legacy) {
        args.put("timeStamp", m.times[Record::kHcclStartTimestamp]);
    }
    if (std::isfinite(m.durationEstimated)) {
        args.put("durationEstimated", m.durationEstimated);
    }
    if (m.srcAddr != UINT64_MAX && m.srcAddr != UINT32_MAX) {
        args.put("srcAddr", Hex(m.srcAddr));
    }
    if (m.dstAddr != UINT64_MAX && m.dstAddr != UINT32_MAX) {
        args.put("dstAddr", Hex(m.dstAddr));
    }
    args.put("dataSize", AvailableId(m.dataLen, UINT64_MAX));
    if (HasHcclPlane(&m)) {
        args.put("planeID", m.planeId);
    }
    args.put("workFlowMode", AvailableId(m.workFlowMode, legacy ? UINT32_MAX : UINT8_MAX));
}

std::string HcclArgs(
    const Record& start, const Record* metadata, const TaskTracks* tracks, const comm::Interval& interval)
{
    auto args = BlockArgs(tracks ? tracks->Stream(start) : kUnknownStreamId, start.taskId, interval);
    args.put("SqId", AvailableId(start.streamId, kUnknownStreamId));
    constexpr std::array fields{"itemId",   "cclTag",   "groupName", "localRank",     "remoteRank",
                                "rankSize", "stage",    "notifyID",  "timeStamp",     "durationEstimated",
                                "srcAddr",  "dstAddr",  "dataSize",  "planeID",       "workFlowMode",
                                "opType",   "dataType", "linkType",  "transportType", "rdmaType",
                                "role",     "reserve",  "reserves"};
    for (const auto* field : fields) {
        args.put(field, unavailable);
    }
    if (!metadata) {
        return comm::Json(args);
    }

    const auto& m = *metadata;
    PutHcclMetadata(args, m);
    PutHcclEnums(args, m, start.taskType, tracks ? tracks->Name(m.itemId) : std::string_view{});
    PutHcclReserves(args, m);
    return comm::Json(args);
}
using CcuTaskKey = std::tuple<int32_t, uint32_t, uint32_t>;
using CcuTaskStreams = std::map<CcuTaskKey, uint32_t>;
using CcuMissionDevices = std::map<std::pair<uint32_t, uint32_t>, std::set<int32_t>>;

CcuMissionDevices IndexCcuMissionDevices(const Source& source)
{
    CcuMissionDevices devices;
    for (const auto& record : source.records) {
        if (record.kind == RecordKind::CcuMission && record.deviceId != wire::kHostDeviceId) {
            devices[{record.taskId, record.dieId}].insert(record.deviceId);
        }
    }
    return devices;
}

CcuTaskStreams IndexCcuTaskStreams(const Source& source, const CcuMissionDevices& missionDevices)
{
    CcuTaskStreams streams;
    for (const auto& record : source.records) {
        if (record.kind != RecordKind::CcuTask || record.rawType != wire::kAdditional ||
            record.level != wire::kHcclLevel || record.type != wire::kCcuTaskInfo || record.streamId >= UINT16_MAX) {
            continue;
        }
        int32_t device = record.deviceId;
        if (device == wire::kHostDeviceId) {
            // The Host envelope has no physical device ID. Join only a unique mission in this source.
            const auto mission = missionDevices.find({record.taskId, record.dieId});
            if (mission == missionDevices.end() || mission->second.size() != 1) {
                continue;
            }
            device = *mission->second.begin();
        }
        const auto [entry, inserted] = streams.try_emplace({device, record.taskId, record.dieId}, record.streamId);
        if (!inserted && entry->second != record.streamId) {
            entry->second = kUnknownStreamId;
        }
    }
    return streams;
}
ClockMapping Clock(const Source& source, int32_t device, ClockMapping DeviceClocks::*domain)
{
    const auto found = source.clocks.find(device);
    return found == source.clocks.end() ? ClockMapping{} : found->second.*domain;
}
void BadInterval(const Source& source, const Record& record, State& state)
{
    state.Issue(
        source.name + ": invalid interval or unavailable calibrated clock for task " + std::to_string(record.taskId));
}
std::string Stream(uint32_t stream)
{
    return stream == kUnknownStreamId ? "Stream unresolved" : "Stream" + std::to_string(stream);
}
std::string Task(const Record& record) { return "Task" + std::to_string(record.taskId); }
bool ComputeType(const Record& record)
{
    constexpr auto aicTaskType = static_cast<uint32_t>(aclptiDavidSqeType::Aic);
    constexpr auto aivTaskType = static_cast<uint32_t>(aclptiDavidSqeType::Aiv);
    constexpr auto fusionTaskType = static_cast<uint32_t>(aclptiDavidSqeType::Fusion);
    constexpr uint32_t aiCoreFusionBit = 4;
    return record.taskType == aicTaskType || record.taskType == aivTaskType ||
           (record.taskType == fusionTaskType && (record.fusionType & aiCoreFusionBit) != 0);
}
void DetermineModes(State& state, Trace& trace)
{
    for (const auto& source : state.sources) {
        for (const auto& record : source.records) {
            if (IsHostMetadata(record)) {
                continue;
            }
            auto& metrics = state.devices[record.deviceId];
            if (record.kind == RecordKind::HcclTask) {
                metrics.aicpu = true;
            }
            if (record.kind == RecordKind::CcuTask || record.kind == RecordKind::CcuMission) {
                metrics.ccu = true;
            }
        }
    }
    // A captured compute task remains useful when this device has no communication metadata.
    for (const auto& source : state.sources) {
        for (const auto& record : source.records) {
            if (record.kind != RecordKind::TaskStart || !ComputeType(record)) {
                continue;
            }
            auto& metrics = state.devices.at(record.deviceId);
            if (!metrics.ccu && !metrics.aiv) {
                metrics.aicpu = true;
            }
        }
    }
    constexpr uint64_t ccuCommunicationTrackOrder = 0;
    for (const auto& entry : state.devices) {
        if (entry.second.aicpu) {
            trace.Process(entry.first, Group::AiCpuCompute);
            trace.Process(entry.first, Group::AiCpuCommunication);
        }
        if (entry.second.ccu) {
            trace.Process(entry.first, Group::CcuCompute);
            trace.Track(entry.first, Group::CcuCommunication, "Communication", ccuCommunicationTrackOrder);
        }
        if (entry.second.aiv) {
            trace.Track(entry.first, Group::Aiv, kAivComputeTrack, kAivComputeOrder);
        }
        if (entry.second.ModeCount() == 0) {
            continue;
        }
        trace.Process(entry.first, Group::Fusion);
        trace.Track(entry.first, Group::Fusion, "compute", static_cast<uint64_t>(FusionTrackOrder::Compute));
        trace.Track(
            entry.first, Group::Fusion, "communication", static_cast<uint64_t>(FusionTrackOrder::Communication));
        trace.Track(entry.first, Group::Fusion, "overlap", static_cast<uint64_t>(FusionTrackOrder::Overlap));
    }
}
using CcuCounterIndex = Record::CcuCounterIndex;
using CcuInstructionKey = std::tuple<int32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
using CcuHostKey = std::pair<CcuInstructionKey, RecordKind>;

auto CcuMetadataFields(const Record& record)
{
    return std::tie(
        record.dataLen, record.opType, record.dataType, record.outputDataType, record.mask, record.channelIds);
}

std::map<CcuHostKey, const Record*> IndexCcuInstructionMetadata(
    const Source& source, const CcuMissionDevices& missionDevices, const CcuTaskStreams& streams)
{
    std::map<CcuHostKey, const Record*> metadata;
    for (const auto& record : source.records) {
        if (record.kind != RecordKind::CcuGroup && record.kind != RecordKind::CcuWait) {
            continue;
        }
        const auto mission = missionDevices.find({record.taskId, record.dieId});
        if (mission == missionDevices.end() || record.streamId >= UINT16_MAX) {
            continue;
        }
        std::optional<int32_t> device;
        for (const auto candidate : mission->second) {
            if (record.deviceId != wire::kHostDeviceId && record.deviceId != candidate) {
                continue;
            }
            const auto stream = streams.find({candidate, record.taskId, record.dieId});
            if (stream == streams.end() || stream->second != record.streamId) {
                continue;
            }
            if (device) {
                device.reset();
                break;
            }
            device = candidate;
        }
        if (!device) {
            continue;
        }
        const CcuHostKey key{{*device, record.dieId, record.streamId, record.taskId, record.instrId}, record.kind};
        const auto [entry, added] = metadata.try_emplace(key, &record);
        if (!added && entry->second && CcuMetadataFields(*entry->second) != CcuMetadataFields(record)) {
            entry->second = nullptr;
        }
    }
    return metadata;
}

using CcuChannelKey = std::tuple<int32_t, uint32_t, uint16_t>;
using CcuChannelSamples = std::vector<std::pair<uint64_t, uint32_t>>;

std::map<CcuChannelKey, CcuChannelSamples> IndexCcuChannels(const Source& source)
{
    std::map<CcuChannelKey, CcuChannelSamples> channels;
    for (const auto& record : source.records) {
        if (record.kind == RecordKind::CcuChannel && record.timestamp != 0 && record.channelAverage != 0 &&
            record.channelId < wire::kCcuChannelCount) {
            channels[{record.deviceId, record.dieId, record.channelId}].emplace_back(
                record.timestamp, record.channelAverage);
        }
    }
    for (auto& channel : channels) {
        auto& samples = channel.second;
        std::sort(samples.begin(), samples.end());
        uint32_t maximum = 0;
        for (auto& sample : samples) {
            maximum = std::max(maximum, sample.second);
            sample.second = maximum;
        }
    }
    return channels;
}

struct CcuInstruction {
    Record record;
    comm::Interval interval;
    uint64_t endCounter;
    std::optional<size_t> notifySlot;
};

void PutCcuWaitMetadata(
    comm::Tree& args, const CcuInstruction& event, const Record& metadata,
    const std::map<CcuChannelKey, CcuChannelSamples>& channels)
{
    const auto& record = event.record;
    if (metadata.mask != UINT32_MAX) {
        args.put("Mask", Hex(metadata.mask));
    }
    uint32_t maxDelay = 0;
    uint16_t maxChannel = UINT16_MAX;
    for (const auto channelId : metadata.channelIds) {
        const auto found = channels.find({record.deviceId, record.dieId, channelId});
        if (found == channels.end()) {
            continue;
        }
        const auto& samples = found->second;
        const auto limit = std::lower_bound(
            samples.begin(), samples.end(), event.endCounter,
            [](const auto& sample, uint64_t end) { return sample.first < end; });
        if (limit == samples.begin()) {
            continue;
        }
        const auto delay = std::prev(limit)->second;
        if (delay > maxDelay || (delay == maxDelay && channelId < maxChannel)) {
            maxDelay = delay;
            maxChannel = channelId;
        }
    }
    if (maxDelay != 0) {
        args.put("Maximum Delay Channel", maxChannel);
        args.put("Maximum Channel Delay", maxDelay);
    }
}

std::string CcuArgs(
    const CcuInstruction& event, const Record* metadata, const std::map<CcuChannelKey, CcuChannelSamples>& channels)
{
    const auto& record = event.record;
    const bool wait = record.times[CcuCounterIndex::waitStart] != 0;
    auto args = BlockArgs(record.streamId, record.taskId, event.interval);
    args.put("LpInstrId", record.times[CcuCounterIndex::loopStart] != 0 ? std::to_string(record.instrId) : unavailable);
    args.put("setckeBitInstrId", wait ? std::to_string(record.setckeInstrId) : unavailable);
    args.put("Notify Rank Id", event.notifySlot ? std::to_string(*event.notifySlot) : unavailable);
    args.put("Die Id", record.dieId);
    for (const auto* field :
         {"Data Size", "Bandwidth (MB/s)", "Reduce Op Type", "Input Data Type", "Output Data Type", "Mask",
          "Maximum Delay Channel", "Maximum Channel Delay"}) {
        args.put(field, unavailable);
    }
    if (!metadata) {
        return comm::Json(args);
    }

    if (!wait) {
        if (metadata->dataLen != UINT64_MAX) {
            args.put("Data Size", metadata->dataLen);
            const auto durationUs = (event.interval.end - event.interval.start) / nsPerUs;
            if (durationUs > 0) {
                args.put("Bandwidth (MB/s)", TraceNumber(metadata->dataLen / durationUs));
            }
        }
        if (metadata->opType < reduceNames.size()) {
            args.put("Reduce Op Type", EnumName(metadata->opType, reduceNames));
            args.put("Input Data Type", EnumName(metadata->dataType, dataTypeNames));
            args.put("Output Data Type", EnumName(metadata->outputDataType, dataTypeNames));
        }
        return comm::Json(args);
    }

    PutCcuWaitMetadata(args, event, *metadata, channels);
    return comm::Json(args);
}
comm::Intervals Envelope(const comm::Intervals& intervals)
{
    if (intervals.empty()) {
        return {};
    }
    comm::Interval result = intervals.front();
    for (const auto& interval : intervals) {
        result.start = std::min(result.start, interval.start);
        result.end = std::max(result.end, interval.end);
    }
    return {result};
}
using CcuInstructions = std::map<CcuInstructionKey, CcuInstruction>;
struct CcuSelection {
    CcuInstructions loops;
    CcuInstructions waits;
    std::set<CcuTaskKey> unresolvedStreams;
    std::set<CcuTaskKey> invalidIntervals;
};

uint32_t ResolveCcuStream(
    const Source& source, const CcuTaskKey& task, const CcuTaskStreams& streams, CcuSelection& selection, State& state)
{
    const auto stream = streams.find(task);
    if (stream != streams.end() && stream->second != kUnknownStreamId) {
        return stream->second;
    }
    if (selection.unresolvedStreams.insert(task).second) {
        const auto [device, taskId, die] = task;
        const auto reason = stream == streams.end() ? "missing or unassignable" : "conflicting";
        state.Issue(
            source.name + ": CCU Stream association failed: " + reason +
            " RawDataType=17 level=5500 type=14 for device " + std::to_string(device) + " task " +
            std::to_string(taskId) + " die " + std::to_string(die));
    }
    return kUnknownStreamId;
}

void SelectCcuIntervals(
    const Record& record, const ClockMapping& clock, CcuSelection& selection, comm::Intervals& communication,
    bool& invalid)
{
    const auto loopStart = record.times[CcuCounterIndex::loopStart];
    const auto loopEnd = record.times[CcuCounterIndex::loopEnd];
    const auto waitStart = record.times[CcuCounterIndex::waitStart];
    // Mixed records still contribute loop time to the summary.
    if (loopStart != 0) {
        comm::Interval loop;
        if (Interval(clock, loopStart, loopEnd, loop)) {
            communication.push_back(loop);
            if (waitStart == 0) {
                selection.loops.insert_or_assign(
                    {record.deviceId, record.dieId, record.streamId, record.taskId, record.instrId},
                    CcuInstruction{record, loop, loopEnd, std::nullopt});
            }
        } else {
            invalid = true;
        }
    }
    if (waitStart == 0) {
        return;
    }
    const auto maxSlot = std::max_element(record.relative.begin(), record.relative.end());
    const auto waitTicks = static_cast<uint64_t>(wire::kRelativeScale) * *maxSlot;
    comm::Interval wait;
    if (waitTicks > UINT64_MAX - waitStart || !Interval(clock, waitStart, waitStart + waitTicks, wait)) {
        invalid = true;
        return;
    }
    communication.push_back(wait);
    const CcuInstructionKey key{record.deviceId, record.dieId, record.streamId, record.taskId, record.setckeInstrId};
    const auto selected = selection.waits.find(key);
    const auto endCounter = waitStart + waitTicks;
    if (selected != selection.waits.end() && endCounter <= selected->second.endCounter) {
        return;
    }
    std::optional<size_t> notifySlot;
    if (*maxSlot != 0) {
        notifySlot = static_cast<size_t>(std::distance(maxSlot, record.relative.end()) - 1);
    }
    selection.waits.insert_or_assign(key, CcuInstruction{record, wait, endCounter, notifySlot});
}

void CcuMissions(const Source& source, State& state, Trace& trace)
{
    const auto missionDevices = IndexCcuMissionDevices(source);
    const auto streams = IndexCcuTaskStreams(source, missionDevices);
    const auto metadata = IndexCcuInstructionMetadata(source, missionDevices, streams);
    const auto channels = IndexCcuChannels(source);
    CcuSelection selection;
    for (const auto& record : source.records) {
        if (record.kind != RecordKind::CcuMission ||
            (record.times[CcuCounterIndex::loopStart] == 0 && record.times[CcuCounterIndex::waitStart] == 0)) {
            continue;
        }
        const CcuTaskKey task{record.deviceId, record.taskId, record.dieId};
        Record resolved = record;
        resolved.streamId = ResolveCcuStream(source, task, streams, selection, state);
        bool invalid = false;
        SelectCcuIntervals(
            resolved, Clock(source, record.deviceId, &DeviceClocks::ccu), selection,
            state.devices[record.deviceId].communication, invalid);
        if (invalid && selection.invalidIntervals.insert(task).second) {
            BadInterval(source, record, state);
        }
    }
    const auto emit = [&metadata, &channels, &trace](
                          const CcuInstructions& instructions, RecordKind kind, const char* name) {
        for (const auto& [key, event] : instructions) {
            const auto found = metadata.find({key, kind});
            const auto* host = found == metadata.end() ? nullptr : found->second;
            trace.Duration(
                event.record.deviceId, Group::CcuCommunication, "Communication", name, event.interval,
                CcuArgs(event, host, channels));
        }
    };
    emit(selection.loops, RecordKind::CcuGroup, "LoopGroup");
    emit(selection.waits, RecordKind::CcuWait, "Wait");
}

void EmitComputeTask(
    const Record& record, const comm::Interval& interval, const Metrics& modes, const TaskTracks* tracks, Trace& trace)
{
    auto fields = BlockArgs(record.streamId, record.taskId, interval);
    fields.put("SqeType", EnumName(record.taskType, sqeNames));
    const auto args = comm::Json(fields);
    const auto track = Stream(record.streamId);
    const auto name = tracks ? tracks->KernelName(record) : std::string{};
    const auto deviceId = record.deviceId;
    const auto streamId = record.streamId;
    const auto emit = [&trace, &track, &name, &interval, &args, deviceId, streamId](Group group, const char* fallback) {
        trace.Duration(deviceId, group, track, name.empty() ? fallback : name, interval, args, streamId);
    };
    if (modes.aicpu) {
        emit(Group::AiCpuCompute, "AiCpu-Comp-Launch");
    }
    if (modes.ccu) {
        emit(Group::CcuCompute, "Ccu-Comp-Launch");
    }
    if (modes.aiv) {
        trace.Duration(
            deviceId, Group::Aiv, kAivComputeTrack, name.empty() ? "AIV-Comp-Launch" : name, interval, args,
            kAivComputeOrder);
    }
}
void TaskPair(
    const Source& source, const Record& start, const Record& end, State& state, Trace& trace, const TaskTracks* tracks)
{
    const auto task = std::make_pair(start.deviceId, start.taskId);
    const auto communication = source.hcclTasks.find(task);
    const bool isCompute = ComputeType(start);
    Record resolved = start;
    const Record* hcclMetadata = nullptr;
    if (isCompute) {
        resolved.streamId = tracks ? tracks->Stream(start) : kUnknownStreamId;
    } else {
        if (communication == source.hcclTasks.end()) {
            return;
        }
        const auto& streams = communication->second;
        if (streams.size() != 1 || (start.streamId != kUnknownStreamId && streams.count(start.streamId) == 0) ||
            (end.streamId != kUnknownStreamId && streams.count(end.streamId) == 0)) {
            state.Issue(
                source.name + ": missing or ambiguous TaskLog Stream for device " + std::to_string(start.deviceId) +
                " " + Task(start));
            return;
        }
        const auto& [stream, metadata] = *streams.begin();
        resolved.streamId = stream;
        if (metadata) {
            hcclMetadata = &source.records[*metadata];
        }
    }

    comm::Interval interval;
    if (!Interval(
            Clock(source, start.deviceId, &DeviceClocks::task), start.times[Record::kTaskSystemCounter],
            end.times[Record::kTaskSystemCounter], interval)) {
        state.Issue(source.name + ": invalid TaskLog interval for " + Task(start));
        return;
    }
    auto& metrics = state.devices.at(start.deviceId);
    (isCompute ? metrics.compute : metrics.communication).push_back(interval);
    if (!isCompute) {
        const auto args = HcclArgs(resolved, hcclMetadata, tracks, interval);
        state.reportedHcclTasks.insert({source.name, start.deviceId, start.taskId});
        constexpr auto group = Group::AiCpuCommunication;
        constexpr const char* label = "AiCpu-Comm-Task";
        if (HasHcclPlane(hcclMetadata)) {
            const auto plane = hcclMetadata->planeId;
            trace.Duration(start.deviceId, group, "Plane " + std::to_string(plane), label, interval, args, plane);
        } else {
            trace.Duration(start.deviceId, group, "Communication", label, interval, args, UINT32_MAX);
        }
        return;
    }
    EmitComputeTask(resolved, interval, metrics, tracks, trace);
}
void UnmatchedTask(const Source& source, const Record& record, State& state)
{
    state.Issue(source.name + ": unmatched or duplicate TaskLog for " + Task(record));
}
template <typename Emit, typename Report>
void PairBoundaries(std::vector<const Record*>& records, RecordKind startKind, Emit emit, Report report)
{
    std::sort(records.begin(), records.end(), [](const auto* a, const auto* b) {
        return std::tie(a->times[Record::kTaskSystemCounter], a->kind) <
               std::tie(b->times[Record::kTaskSystemCounter], b->kind);
    });
    const Record* start = nullptr;
    bool ambiguous = false;
    for (const auto* record : records) {
        if (record->kind == startKind) {
            if (start) {
                ambiguous = true;
                report(*record);
            } else
                start = record;
            continue;
        }
        if (!start || ambiguous)
            report(*record);
        else
            emit(*start, *record);
        start = nullptr;
        ambiguous = false;
    }
    if (start)
        report(*start);
}
void TaskLogs(const Source& source, State& state, Trace& trace, const TaskTracks* tracks)
{
    enum class TaskLogFamily { Ordinary, Fusion };
    using Key = std::tuple<int32_t, uint32_t, uint32_t, uint32_t, TaskLogFamily, uint32_t>;
    std::map<Key, std::vector<const Record*>> tasks;
    for (const auto& record : source.records) {
        if (record.kind != RecordKind::TaskStart && record.kind != RecordKind::TaskEnd)
            continue;
        if (!ComputeType(record) && source.hcclTasks.count({record.deviceId, record.taskId}) == 0)
            continue;
        const bool fusionLog = record.functionType == aclpti::data::kFusionStartFunction ||
                               record.functionType == aclpti::data::kFusionEndFunction;
        const auto family = fusionLog ? TaskLogFamily::Fusion : TaskLogFamily::Ordinary;
        tasks[{record.deviceId, record.taskId, record.taskType, record.fusionType, family, record.missionId}].push_back(
            &record);
    }
    for (auto& item : tasks) {
        PairBoundaries(
            item.second, RecordKind::TaskStart,
            [&](const Record& start, const Record& end) { TaskPair(source, start, end, state, trace, tracks); },
            [&](const Record& record) { UnmatchedTask(source, record, state); });
    }
}
void ValidateTaskLogs(const Source& source, State& state)
{
    for (const auto& entry : source.hcclTasks) {
        const auto& [device, task] = entry.first;
        if (state.reportedHcclTasks.count({source.name, device, task}) == 0)
            state.Issue(
                source.name + ": no usable communication TaskLog pair for device " + std::to_string(device) + " Task" +
                std::to_string(task));
    }
}

constexpr uint32_t kLastReservedDescId = 0xffff;
constexpr uint32_t kMaxVectorSubBlock = 1;
constexpr uint64_t kTracksPerBlock = 3;
constexpr size_t kSystemCounter = Record::kTaskSystemCounter;
constexpr uint32_t kDescIdPairMask = 1;
constexpr uint32_t kSubBlockCountOffset = 1;
constexpr uint64_t kFirstBlockTrackOffset = 1;
constexpr uint32_t kMinimumMixedModes = 2;
constexpr uint32_t kSingleModeCount = 1;
struct AivOperation {
    uint32_t startId;
    const char* name;
};
constexpr AivOperation aivOperations[] = {
    {0x1000, "INIT"},
    {0x1010, "COMMIT"},
    {0x1020, "WAIT"},
    {0x1030, "QUERY"},
    {0x1040, "FINALIZE"},
    {0x1050, "GROUP_SYNC"},
    {0x1060, "GET_WINDOW_IN"},
    {0x1062, "GET_WINDOW_OUT"},
    {0x1064, "GET_RANK_ID"},
    {0x1066, "GET_RANK_DIM"},
    {0x1068, "SET_CCTILING"},
    {0x1070, "ITERATE"},
    {0x1072, "BARRIER"},
    {0x1100, "ALL_REDUCE_PREPARE"},
    {0x1110, "ALL_GATHER_PREPARE"},
    {0x1120, "REDUCE_SCATTER_PREPARE"},
    {0x1130, "ALL_TO_ALL_PREPARE"},
    {0x1140, "ALL_TO_ALL_V_PREPARE"},
    {0x1150, "BATCH_WRITE_PREPARE"},
};

const AivOperation* FindAivOperation(uint32_t descId)
{
    const auto startId = descId & ~kDescIdPairMask;
    const auto it = std::lower_bound(
        std::begin(aivOperations), std::end(aivOperations), startId,
        [](const AivOperation& operation, uint32_t id) { return operation.startId < id; });
    return it != std::end(aivOperations) && it->startId == startId ? &*it : nullptr;
}

struct AivBlock {
    const Record* start;
    uint64_t end;
};
using AivBlockIndex = std::map<uint32_t, std::vector<size_t>>;
using AivMarkKey = std::tuple<size_t, uint32_t, uint32_t>;
using AivMarks = std::map<AivMarkKey, std::vector<const Record*>>;
struct AivTimeline {
    std::vector<AivBlock> blocks;
    AivMarks marks;
};

using AivTaskKey = std::pair<int32_t, uint32_t>;
bool IsVectorBlock(const Record& record) { return record.coreType == ACLPTI_CORE_TYPE_AIV; }

bool IsSupportedBlock(const Record& record)
{
    if (record.kind != RecordKind::BlockStart && record.kind != RecordKind::BlockEnd)
        return false;
    return IsVectorBlock(record) ? record.subBlockId <= kMaxVectorSubBlock :
                                   record.coreType == ACLPTI_CORE_TYPE_AIC && record.subBlockId == 0;
}

std::vector<AivBlock> CollectAivBlocks(const Source& source, State& state)
{
    using Key = std::tuple<int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::map<Key, std::vector<const Record*>> groups;
    for (const auto& record : source.records) {
        if (IsSupportedBlock(record)) {
            groups[{record.deviceId, record.taskId, record.blockId, record.subBlockId, record.coreType, record.coreId}]
                .push_back(&record);
        }
    }
    std::vector<AivBlock> blocks;
    for (auto& group : groups) {
        PairBoundaries(
            group.second, RecordKind::BlockStart,
            [&](const Record& start, const Record& end) { blocks.push_back({&start, end.times[kSystemCounter]}); },
            [&](const Record& row) { state.Issue(source.name + ": unmatched BlockLog for " + Task(row)); });
    }
    return blocks;
}

AivBlockIndex IndexAivBlocks(const std::vector<AivBlock>& blocks)
{
    std::map<AivTaskKey, uint32_t> vectorCounts;
    for (const auto& block : blocks) {
        const auto& row = *block.start;
        if (IsVectorBlock(row)) {
            auto& count = vectorCounts[{row.deviceId, row.taskId}];
            count = std::max(count, row.subBlockId + kSubBlockCountOffset);
        }
    }
    AivBlockIndex index;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& row = *blocks[i].start;
        if (IsVectorBlock(row)) {
            const auto count = vectorCounts.at({row.deviceId, row.taskId});
            const auto logicalId = row.blockId * count + row.subBlockId;
            index[logicalId].push_back(i);
        }
    }
    return index;
}

std::optional<size_t> MatchAivBlock(const Record& mark, const std::vector<AivBlock>& blocks, const AivBlockIndex& index)
{
    // Dump and BlockLog core IDs use different numbering on A5. A host record
    // can be assigned only when its logical ID and device counter match one block.
    const auto found = index.find(mark.blockId);
    if (found == index.end())
        return std::nullopt;
    std::optional<size_t> matched;
    const auto timestamp = mark.times[kSystemCounter];
    for (const auto i : found->second) {
        const auto& block = blocks[i];
        const auto& start = *block.start;
        if (mark.deviceId != wire::kHostDeviceId && start.deviceId != mark.deviceId)
            continue;
        if (timestamp < start.times[kSystemCounter] || timestamp > block.end)
            continue;
        if (matched)
            return std::nullopt;
        matched = i;
    }
    return matched;
}

AivTimeline CollectAivTimeline(const Source& source, State& state)
{
    AivTimeline timeline;
    if (std::none_of(source.records.begin(), source.records.end(), [](const Record& row) {
            return row.kind == RecordKind::AicoreTimestamp;
        }))
        return timeline;
    timeline.blocks = CollectAivBlocks(source, state);
    const auto index = IndexAivBlocks(timeline.blocks);
    for (const auto& row : source.records) {
        if (row.kind != RecordKind::AicoreTimestamp)
            continue;
        const auto* operation = FindAivOperation(row.descId);
        if (!operation && row.descId <= kLastReservedDescId)
            continue;
        if (row.deviceId < wire::kHostDeviceId) {
            state.devices.at(row.deviceId).aiv = true;
        }
        const auto block = MatchAivBlock(row, timeline.blocks, index);
        if (!block) {
            state.Issue(source.name + ": missing or ambiguous Vector Block for descId " + Hex(row.descId));
            continue;
        }
        const auto& start = *timeline.blocks[*block].start;
        state.devices.at(start.deviceId).aiv = true;
        timeline.marks[{*block, operation ? operation->startId : row.descId, row.coreId}].push_back(&row);
    }
    return timeline;
}

std::string AivBlockTrack(const Record& block)
{
    return "block" + std::to_string(block.blockId) + (IsVectorBlock(block) ? "-vector" : "-cube") +
           std::to_string(block.subBlockId);
}

uint64_t AivBlockOrder(const Record& block)
{
    return kAivComputeOrder + kFirstBlockTrackOffset + block.blockId * kTracksPerBlock +
           (IsVectorBlock(block) ? kSubBlockCountOffset + block.subBlockId : 0);
}

comm::Tree AivBlockArgs(const Record& block, const TaskTracks* tracks, const comm::Interval& interval)
{
    auto args = BlockArgs(tracks ? tracks->Stream(block) : kUnknownStreamId, block.taskId, interval);
    args.put("BlockId", block.blockId);
    args.put("SubBlockId", block.subBlockId);
    args.put("CoreType", IsVectorBlock(block) ? "AIV" : "AIC");
    args.put("BlockLog Core Id", block.coreId);
    return args;
}

void EmitAivMark(
    const Source& source, const Record& block, const Record& start, const Record& end, const AivOperation* operation,
    const TaskTracks* tracks, State& state, Trace& trace)
{
    comm::Interval interval;
    if (end.times[kSystemCounter] <= start.times[kSystemCounter] ||
        !Interval(
            Clock(source, block.deviceId, &DeviceClocks::task), start.times[kSystemCounter], end.times[kSystemCounter],
            interval)) {
        state.Issue(source.name + ": invalid AIV interval for descId " + Hex(start.descId));
        return;
    }
    auto args = AivBlockArgs(block, tracks, interval);
    args.put("Start DescId", Hex(start.descId));
    args.put("End DescId", Hex(end.descId));
    args.put("Start PC", Hex(start.curPc));
    args.put("End PC", Hex(end.curPc));
    args.put("Physical Core Id", start.coreId);
    if (operation)
        state.devices.at(block.deviceId).communication.push_back(interval);
    trace.Duration(
        block.deviceId, Group::Aiv, AivBlockTrack(block), operation ? operation->name : std::to_string(start.descId),
        interval, comm::Json(args), AivBlockOrder(block));
}

void EmitAivMarks(const Source& source, AivTimeline& timeline, const TaskTracks* tracks, State& state, Trace& trace)
{
    for (auto& [key, marks] : timeline.marks) {
        const auto [blockIndex, descId, coreId] = key;
        const auto& block = *timeline.blocks[blockIndex].start;
        const auto* operation = FindAivOperation(descId);
        std::stable_sort(marks.begin(), marks.end(), [](const Record* a, const Record* b) {
            return std::tie(a->times[kSystemCounter], a->descId) < std::tie(b->times[kSystemCounter], b->descId);
        });
        const Record* start = nullptr;
        bool ambiguous = false;
        const auto unmatched = [&]() {
            state.Issue(
                source.name + ": unmatched AIV descId " + Hex(descId) + " for " + Task(block) + " on physical core " +
                std::to_string(coreId));
        };
        for (const auto* mark : marks) {
            if (operation ? mark->descId == descId : start == nullptr) {
                if (start) {
                    ambiguous = true;
                    unmatched();
                } else {
                    start = mark;
                }
                continue;
            }
            if (start && !ambiguous)
                EmitAivMark(source, block, *start, *mark, operation, tracks, state, trace);
            else
                unmatched();
            start = nullptr;
            ambiguous = false;
        }
        if (start)
            unmatched();
    }
}

void EmitAivBlocks(
    const Source& source, const std::vector<AivBlock>& blocks, const TaskTracks* tracks, State& state, Trace& trace)
{
    for (const auto& block : blocks) {
        const auto& start = *block.start;
        if (!state.devices.at(start.deviceId).aiv)
            continue;
        comm::Interval interval;
        if (!Interval(
                Clock(source, start.deviceId, &DeviceClocks::task), start.times[kSystemCounter], block.end, interval)) {
            state.Issue(source.name + ": invalid BlockLog interval for " + Task(start));
            continue;
        }
        trace.Duration(
            start.deviceId, Group::Aiv, AivBlockTrack(start), IsVectorBlock(start) ? "vector time" : "cube time",
            interval, comm::Json(AivBlockArgs(start, tracks, interval)), AivBlockOrder(start));
    }
}

void ProcessSources(const fs::path& directory, State& state, Trace& trace)
{
    std::vector<AivTimeline> timelines;
    for (const auto& source : state.sources)
        timelines.push_back(CollectAivTimeline(source, state));
    DetermineModes(state, trace);
    // Keep the legacy input available when regenerating a trace from an older report.
    const auto taskTracksPath = directory / comm::kProcessDirectory / comm::kTaskTracksFileName;
    const TaskTracks tracks(fs::exists(taskTracksPath) ? taskTracksPath : directory / "application_raw.jsonl");
    const auto process = fs::path(comm::kProcessDirectory).filename().string();
    for (size_t i = 0; i < state.sources.size(); ++i) {
        const auto& source = state.sources[i];
        auto& timeline = timelines[i];
        const auto* sourceTracks = source.name == process ? &tracks : nullptr;
        CcuMissions(source, state, trace);
        TaskLogs(source, state, trace, sourceTracks);
        if (timeline.blocks.empty() &&
            std::any_of(source.records.begin(), source.records.end(), [&](const Record& row) {
                return IsSupportedBlock(row) && state.devices.at(row.deviceId).aiv;
            })) {
            timeline.blocks = CollectAivBlocks(source, state);
        }
        EmitAivBlocks(source, timeline.blocks, sourceTracks, state, trace);
        EmitAivMarks(source, timeline, sourceTracks, state, trace);
        ValidateTaskLogs(source, state);
    }
    for (const auto& item : state.devices) {
        if (item.second.ModeCount() < kMinimumMixedModes)
            continue;
        const auto warning = "Device Id " + std::to_string(item.first) +
                             ": mixed communication modes; domain envelopes combine all detected modes";
        state.Issue(warning);
    }
}
void Fusion(Trace& trace, int32_t device, const Metrics& metrics, size_t computeCount, size_t communicationCount)
{
    if (metrics.ModeCount() == 0)
        return;
    comm::Tree args;
    args.put("deviceId", device);
    args.put(
        "communicationMode",
        metrics.ModeCount() > kSingleModeCount ? "mixed" : (metrics.aiv ? "AIV" : (metrics.aicpu ? "AICPU" : "CCU")));
    args.put("aggregation", "envelope");
    if (!metrics.compute.empty()) {
        auto computeArgs = args;
        computeArgs.put("intervalCount", computeCount);
        trace.Duration(
            device, Group::Fusion, "compute", "Compute", metrics.compute.front(), comm::Json(computeArgs),
            static_cast<uint64_t>(FusionTrackOrder::Compute));
    }
    if (!metrics.communication.empty()) {
        auto communicationArgs = args;
        communicationArgs.put("intervalCount", communicationCount);
        trace.Duration(
            device, Group::Fusion, "communication", "Communication", metrics.communication.front(),
            comm::Json(communicationArgs), static_cast<uint64_t>(FusionTrackOrder::Communication));
    }
    if (metrics.compute.empty() || metrics.communication.empty())
        return;
    args.put("aggregation", "envelopeIntersection");
    args.put("computeEnvelopeUs", TraceNumber(comm::Duration(metrics.compute) / nsPerUs));
    args.put("communicationEnvelopeUs", TraceNumber(comm::Duration(metrics.communication) / nsPerUs));
    for (const auto& interval : comm::Intersection(metrics.compute, metrics.communication))
        trace.Duration(
            device, Group::Fusion, "overlap", "Overlap", interval, comm::Json(args),
            static_cast<uint64_t>(FusionTrackOrder::Overlap));
}
std::string Metric(const comm::Intervals& intervals, bool valid)
{
    return valid ? TraceNumber(comm::Duration(intervals) / nsPerUs) : "\"NA\"";
}
void Summary(TraceFileOutput& output, int32_t device, const Metrics& metrics)
{
    output.Write(
        "{\"category\":\"DeviceCommunicationSummary\",\"Scope\":\"Device\",\"Device Id\":" + std::to_string(device) +
        ",\"Compute Duration(us)\":" + Metric(metrics.compute, !metrics.compute.empty()) +
        ",\"Communication Duration(us)\":" + Metric(metrics.communication, !metrics.communication.empty()) +
        ",\"Overlap Duration(us)\":" +
        Metric(
            comm::Intersection(metrics.compute, metrics.communication),
            !metrics.compute.empty() && !metrics.communication.empty()) +
        "}\n");
}
void WriteSummaries(State& state, Trace& trace, TraceFileOutput& summary)
{
    for (auto& item : state.devices) {
        auto& metrics = item.second;
        const auto computeCount = metrics.compute.size();
        const auto communicationCount = metrics.communication.size();
        metrics.compute = Envelope(metrics.compute);
        metrics.communication = Envelope(metrics.communication);
        if (metrics.compute.empty())
            state.Issue("Device Id " + std::to_string(item.first) + ": no usable compute intervals");
        if (metrics.communication.empty())
            state.Issue("Device Id " + std::to_string(item.first) + ": no usable communication intervals");
        Fusion(trace, item.first, metrics, computeCount, communicationCount);
        Summary(summary, item.first, metrics);
    }
    summary.Flush();
}
} // namespace
bool FinalizeCommunication(const fs::path& directory, std::vector<std::string>* issues)
{
    State state;
    if (issues)
        state.errors = *issues;
    const auto upstreamErrors = state.errors.size();
    try {
        if (!SafePath(directory) || !fs::is_directory(directory))
            throw std::runtime_error("unsafe Communication output directory");
        if (fs::exists(directory / "CommunicationTrace.json") || fs::exists(directory / "app_summary.jsonl"))
            throw std::runtime_error("Communication outputs already exist");
        try {
            ReadSources(directory, state);
        } catch (const std::exception& error) {
            state.Error(error.what());
        }
        TraceFileOutput traceOutput(directory / "CommunicationTrace.json");
        Trace trace(traceOutput);
        ProcessSources(directory, state, trace);
        if (!state.devices.empty()) {
            TraceFileOutput summaryOutput(directory / "app_summary.jsonl");
            WriteSummaries(state, trace, summaryOutput);
            summaryOutput.Close();
            summaryOutput.Publish();
        }
        trace.Finish();
        traceOutput.Close();
        traceOutput.Publish();
    } catch (const std::exception& error) {
        state.Error(error.what());
    }
    for (const auto& diagnostic : state.issues)
        std::fprintf(stderr, "[WARNING] npu-compute: Communication: %s\n", diagnostic.c_str());
    if (issues)
        issues->insert(issues->end(), state.errors.begin() + upstreamErrors, state.errors.end());
    return state.errors.empty();
}
} // namespace npucompute::cli
