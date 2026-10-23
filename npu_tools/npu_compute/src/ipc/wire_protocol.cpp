/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "ipc.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace npucompute::ipc {
namespace {
constexpr std::size_t kMaximumSectionCount = 9;
constexpr std::size_t kMaximumPmSamplingPathComponents = 2;
void Check(bool condition, const char* reason)
{
    if (!condition) {
        throw Exception({1, reason, "protocol", "payload"});
    }
}
class Writer {
public:
    void Integer(uint64_t n, size_t bytes)
    {
        for (size_t i = 0; i < bytes; ++i) {
            data.push_back(static_cast<uint8_t>(n >> (i * 8)));
        }
    }
    void String(const std::string& value, size_t limit = 128)
    {
        Check(value.size() <= limit && value.find('\0') == std::string::npos, "invalid IPC string");
        Integer(value.size(), 4);
        data.insert(data.end(), value.begin(), value.end());
    }
    Bytes data;
};
class Reader {
public:
    explicit Reader(const Bytes& data) : data_(data) {}
    uint64_t Integer(size_t bytes)
    {
        Check(bytes <= data_.size() - position_, "truncated IPC payload");
        uint64_t result = 0;
        for (size_t i = 0; i < bytes; ++i) {
            result |= static_cast<uint64_t>(data_[position_++]) << (i * 8);
        }
        return result;
    }
    std::string String(size_t limit = 128)
    {
        const auto count = Integer(4);
        Check(count <= limit && count <= data_.size() - position_, "invalid IPC string length");
        std::string result(data_.begin() + position_, data_.begin() + position_ + count);
        position_ += count;
        Check(result.find('\0') == std::string::npos, "NUL in IPC string");
        return result;
    }
    Bytes Blob(size_t limit)
    {
        const auto count = Integer(4);
        Check(count <= limit && count <= data_.size() - position_, "invalid IPC blob length");
        Bytes result(data_.begin() + position_, data_.begin() + position_ + count);
        position_ += count;
        return result;
    }
    void End() { Check(position_ == data_.size(), "trailing IPC payload bytes"); }

private:
    const Bytes& data_;
    size_t position_ = 0;
};
void WriteSections(Writer& writer, const std::vector<std::string>& sections)
{
    Check(!sections.empty() && sections.size() <= kMaximumSectionCount, "invalid section count");
    writer.Integer(sections.size(), 4);
    for (const auto& section : sections) {
        writer.String(section);
    }
}
std::vector<std::string> ReadSections(Reader& reader)
{
    const auto count = reader.Integer(4);
    Check(count > 0 && count <= kMaximumSectionCount, "invalid section count");
    std::vector<std::string> result;
    for (uint64_t i = 0; i < count; ++i) {
        result.push_back(reader.String());
    }
    return result;
}
void WriteConfig(Writer& writer, const Config& value)
{
    ValidateConfig(value);
    WriteSections(writer, value.sections);
    writer.String(value.replayMode);
    writer.String(value.pmuLevel);
    writer.Integer(value.featureFlags, 8);
}
Config ReadConfig(Reader& reader)
{
    Config result;
    result.sections = ReadSections(reader);
    result.replayMode = reader.String();
    result.pmuLevel = reader.String();
    result.featureFlags = reader.Integer(8);
    ValidateConfig(result);
    return result;
}
void ValidateResult(const Result& result)
{
    Config config;
    config.sections = result.sections;
    ValidateConfig(config);
    Check(result.manifest.size() <= 64, "invalid artifact count");
    std::set<std::string> names;
    std::set<uint64_t> ids;
    for (const auto& item : result.manifest) {
        KindForName(item.name);
        Check(ids.insert(item.id).second && names.insert(item.name).second, "duplicate manifest artifact");
    }
}

std::vector<std::string> ArtifactPathComponents(const std::string& name)
{
    Check(
        !name.empty() && name.front() != '/' && name.back() != '/' && name.find('\\') == std::string::npos,
        "artifact name is not allowed");
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto separator = name.find('/', start);
        const auto component =
            name.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
        Check(!component.empty() && component != "." && component != "..", "artifact name is not allowed");
        Check(
            std::all_of(
                component.begin(), component.end(),
                [](unsigned char character) {
                    return std::isalnum(character) != 0 || character == '.' || character == '_' || character == '-';
                }),
            "artifact name is not allowed");
        components.push_back(component);
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    Check(components.size() <= 3, "artifact name is not allowed");
    if (components.size() == 2) {
        Check(components[0].rfind("collection-p", 0) == 0, "artifact name is not allowed");
    } else if (components.size() == 3) {
        Check(components[0] == ".biu-staging" && components[1] == "process-uds", "artifact name is not allowed");
    }
    return components;
}
} // namespace

void ValidateConfig(const Config& config)
{
    Check(!config.sections.empty() && config.sections.size() <= kMaximumSectionCount, "invalid section count");
    std::set<std::string> seen;
    for (const auto& section : config.sections) {
        Check(
            section == "PipeUtilization" || section == "Memory" || section == "MemoryL0" || section == "MemoryUB" ||
                section == "L2Cache" || section == "Pipeline" || section == "ArithmeticUtilization" ||
                section == "ResourceConflictRatio" || section == "PmSampling",
            "unknown section");
        Check(seen.insert(section).second, "duplicate section");
    }
    Check(config.replayMode == "kernel", "unsupported replay mode");
    Check(config.pmuLevel == "block" || config.pmuLevel == "task", "unsupported PMU level");
    Check(config.featureFlags == 0, "unsupported config flags");
}
DataKind KindForName(const std::string& name)
{
    const auto components = ArtifactPathComponents(name);
    const std::string& base = components.back();
    if (base == "HardwareInfo.jsonl" && components.size() == 1) {
        return DataKind::HardwareInfo;
    }
    const bool pipelinePath =
        components.size() == 3 && (base == "manifest.json" || (base.rfind("result-", 0) == 0 && base.size() > 12 &&
                                                               base.compare(base.size() - 5, 5, ".json") == 0));
    if ((base == "PipeTrace.json" && components.size() == 1) || pipelinePath) {
        return DataKind::PipelineTrace;
    }
    if (base == "summary.db" || base == "objdump.txt" || base == "symbolizer.json") {
        Check(components.size() <= kMaximumPmSamplingPathComponents, "invalid PmSampling artifact path");
        return DataKind::PmSampling;
    }
    if (base == "summary.jsonl" || base == "PipeUtilization.csv") {
        return DataKind::PmuCsv;
    }
    Check(
        base == "Memory.csv" || base == "MemoryL0.csv" || base == "MemoryUB.csv" || base == "L2Cache.csv" ||
            base == "ArithmeticUtilization.csv" || base == "ResourceConflictRatio.csv",
        "artifact name is not allowed");
    return DataKind::PmuCsv;
}
ArtifactType ArtifactTypeForName(const std::string& name)
{
    const auto kind = KindForName(name);
    const auto components = ArtifactPathComponents(name);
    const std::string& base = components.back();
    if (base == "summary.db")
        return ArtifactType::Db;
    if (base == "objdump.txt")
        return ArtifactType::Txt;
    if (base == "symbolizer.json")
        return ArtifactType::Json;
    if (kind == DataKind::PipelineTrace) {
        return ArtifactType::Json;
    }
    if (base == "summary.jsonl" || base == "HardwareInfo.jsonl") {
        return ArtifactType::Jsonl;
    }
    return ArtifactType::Csv;
}
Type TypeForKind(DataKind kind)
{
    Check(
        kind == DataKind::PmuCsv || kind == DataKind::PipelineTrace || kind == DataKind::HardwareInfo ||
            kind == DataKind::PmSampling,
        "unknown artifact kind");
    if (kind == DataKind::PmSampling)
        return Type::PmSampling;
    if (kind == DataKind::PipelineTrace) {
        return Type::Pipeline;
    }
    return kind == DataKind::HardwareInfo ? Type::Hardware : Type::Pmu;
}
Bytes EncodeConfig(const Config& value)
{
    Writer writer;
    WriteConfig(writer, value);
    return writer.data;
}
Config DecodeConfig(const Bytes& value)
{
    Reader reader(value);
    auto result = ReadConfig(reader);
    reader.End();
    return result;
}
Bytes EncodeHello(const Hello& value)
{
    Writer writer;
    writer.Integer(value.pid, 4);
    writer.Integer(value.uid, 4);
    WriteConfig(writer, value.config);
    return writer.data;
}
Hello DecodeHello(const Bytes& value)
{
    Reader reader(value);
    Hello result;
    result.pid = reader.Integer(4);
    result.uid = reader.Integer(4);
    result.config = ReadConfig(reader);
    reader.End();
    return result;
}
Bytes EncodeReady(const Ready& value)
{
    Writer writer;
    writer.Integer(value.pid, 4);
    writer.Integer(value.uid, 4);
    writer.Integer(value.status, 4);
    return writer.data;
}
Ready DecodeReady(const Bytes& value)
{
    Reader reader(value);
    Ready result;
    result.pid = reader.Integer(4);
    result.uid = reader.Integer(4);
    result.status = reader.Integer(4);
    reader.End();
    return result;
}
Bytes EncodeArtifactBegin(const ArtifactBegin& value)
{
    Check(KindForName(value.name) == value.kind, "artifact kind does not match name");
    Check(value.artifactType == ArtifactTypeForName(value.name), "artifact type does not match kind");
    Writer writer;
    writer.Integer(value.id, 8);
    writer.Integer(static_cast<uint16_t>(value.kind), 2);
    writer.Integer(static_cast<uint16_t>(value.artifactType), 2);
    writer.String(value.name);
    return writer.data;
}
ArtifactBegin DecodeArtifactBegin(const Bytes& value)
{
    Reader reader(value);
    ArtifactBegin result;
    result.id = reader.Integer(8);
    result.kind = static_cast<DataKind>(reader.Integer(2));
    result.artifactType = static_cast<ArtifactType>(reader.Integer(2));
    result.name = reader.String();
    reader.End();
    Check(KindForName(result.name) == result.kind, "artifact kind does not match name");
    Check(result.artifactType == ArtifactTypeForName(result.name), "artifact type does not match kind");
    return result;
}
Bytes EncodeArtifactChunk(const ArtifactChunk& value)
{
    Check(
        value.data.size() <= kChunkBytes && value.data.size() <= std::numeric_limits<uint64_t>::max() - value.offset,
        "invalid artifact chunk size");
    Writer writer;
    writer.Integer(value.id, 8);
    writer.Integer(value.offset, 8);
    writer.Integer(value.data.size(), 4);
    writer.data.insert(writer.data.end(), value.data.begin(), value.data.end());
    return writer.data;
}
ArtifactChunk DecodeArtifactChunk(const Bytes& value)
{
    Reader reader(value);
    ArtifactChunk result;
    result.id = reader.Integer(8);
    result.offset = reader.Integer(8);
    result.data = reader.Blob(kChunkBytes);
    reader.End();
    Check(result.data.size() <= std::numeric_limits<uint64_t>::max() - result.offset, "artifact byte offset overflow");
    return result;
}
Bytes EncodeArtifactEnd(const ArtifactEnd& value)
{
    Writer writer;
    writer.Integer(value.id, 8);
    writer.Integer(value.bytes, 8);
    writer.Integer(value.records, 8);
    return writer.data;
}
ArtifactEnd DecodeArtifactEnd(const Bytes& value)
{
    Reader reader(value);
    ArtifactEnd result;
    result.id = reader.Integer(8);
    result.bytes = reader.Integer(8);
    result.records = reader.Integer(8);
    reader.End();
    return result;
}
Bytes EncodeResult(const Result& value)
{
    ValidateResult(value);
    Writer writer;
    writer.Integer(value.status, 4);
    writer.Integer(value.code, 4);
    writer.Integer(value.complete ? 1 : 0, 1);
    writer.Integer(value.degraded ? 1 : 0, 1);
    writer.Integer(value.replayCount, 8);
    writer.Integer(value.rowCount, 8);
    writer.Integer(value.droppedCount, 8);
    writer.Integer(value.errorCount, 8);
    WriteSections(writer, value.sections);
    writer.Integer(value.manifest.size(), 4);
    for (const auto& item : value.manifest) {
        writer.Integer(item.id, 8);
        writer.String(item.name);
        writer.Integer(item.bytes, 8);
    }
    return writer.data;
}
Result DecodeResult(const Bytes& value)
{
    Reader reader(value);
    Result result;
    result.status = reader.Integer(4);
    result.code = reader.Integer(4);
    const auto complete = reader.Integer(1);
    const auto degraded = reader.Integer(1);
    Check(complete <= 1 && degraded <= 1, "invalid result flags");
    result.complete = complete;
    result.degraded = degraded;
    result.replayCount = reader.Integer(8);
    result.rowCount = reader.Integer(8);
    result.droppedCount = reader.Integer(8);
    result.errorCount = reader.Integer(8);
    result.sections = ReadSections(reader);
    const auto count = reader.Integer(4);
    Check(count <= 64, "invalid manifest length");
    for (uint64_t i = 0; i < count; ++i) {
        ManifestEntry item;
        item.id = reader.Integer(8);
        item.name = reader.String();
        item.bytes = reader.Integer(8);
        result.manifest.push_back(item);
    }
    reader.End();
    ValidateResult(result);
    return result;
}
Bytes EncodeError(const Error& value)
{
    Writer writer;
    writer.Integer(value.code, 4);
    writer.String(value.message, 4096);
    writer.String(value.domain);
    writer.String(value.phase);
    return writer.data;
}
Error DecodeError(const Bytes& value)
{
    Reader reader(value);
    Error result;
    result.code = reader.Integer(4);
    result.message = reader.String(4096);
    result.domain = reader.String();
    result.phase = reader.String();
    reader.End();
    return result;
}
} // namespace npucompute::ipc
