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
constexpr size_t DEFAULT_STRING_BYTES = 128;
constexpr size_t MAX_SECTION_COUNT = 9;
constexpr size_t MAX_ARTIFACT_COUNT = 64;
constexpr size_t MAX_ERROR_MESSAGE_BYTES = 4096;
constexpr unsigned int BITS_PER_BYTE = std::numeric_limits<uint8_t>::digits;
constexpr std::size_t MAX_PM_SAMPLING_PATH_COMPONENTS = 2;
constexpr size_t UINT8_BYTES = sizeof(uint8_t);
constexpr size_t UINT16_BYTES = sizeof(uint16_t);
constexpr size_t UINT32_BYTES = sizeof(uint32_t);
constexpr size_t UINT64_BYTES = sizeof(uint64_t);
constexpr uint32_t IPC_PROTOCOL_ERROR_CODE = 1;
constexpr size_t MAX_ARTIFACT_PATH_COMPONENTS = 3;
constexpr size_t COLLECTION_PATH_COMPONENTS = 2;
constexpr size_t STAGING_PATH_COMPONENTS = 3;
constexpr size_t RESULT_NAME_MIN_BYTES = 12;
constexpr size_t JSON_SUFFIX_BYTES = 5;
constexpr uint64_t NO_FEATURE_FLAGS = 0U;
constexpr uint64_t BOOLEAN_FALSE = 0U;
constexpr uint64_t BOOLEAN_TRUE = 1U;
constexpr std::size_t PATH_START_OFFSET = 0U;
constexpr std::size_t PATH_COMPONENT_INCREMENT = 1U;

void Check(bool condition, const char* reason)
{
    if (!condition) {
        throw Exception({IPC_PROTOCOL_ERROR_CODE, reason, "protocol", "payload"});
    }
}
class Writer {
public:
    void Integer(uint64_t n, size_t bytes)
    {
        for (size_t i = 0; i < bytes; ++i) {
            data.push_back(static_cast<uint8_t>(n >> (i * BITS_PER_BYTE)));
        }
    }
    void String(const std::string& value, size_t limit = DEFAULT_STRING_BYTES)
    {
        Check(value.size() <= limit && value.find('\0') == std::string::npos, "invalid IPC string");
        Integer(value.size(), UINT32_BYTES);
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
            result |= static_cast<uint64_t>(data_[position_++]) << (i * BITS_PER_BYTE);
        }
        return result;
    }
    std::string String(size_t limit = DEFAULT_STRING_BYTES)
    {
        const auto count = Integer(UINT32_BYTES);
        Check(count <= limit && count <= data_.size() - position_, "invalid IPC string length");
        std::string result(data_.begin() + position_, data_.begin() + position_ + count);
        position_ += count;
        Check(result.find('\0') == std::string::npos, "NUL in IPC string");
        return result;
    }
    Bytes Blob(size_t limit)
    {
        const auto count = Integer(UINT32_BYTES);
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
    Check(!sections.empty() && sections.size() <= MAX_SECTION_COUNT, "invalid section count");
    writer.Integer(sections.size(), UINT32_BYTES);
    for (const auto& section : sections) {
        writer.String(section);
    }
}
std::vector<std::string> ReadSections(Reader& reader)
{
    const auto count = reader.Integer(UINT32_BYTES);
    Check(count > 0 && count <= MAX_SECTION_COUNT, "invalid section count");
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
    writer.Integer(value.featureFlags, UINT64_BYTES);
    writer.Integer(value.cacheControlMode, UINT32_BYTES);
}
Config ReadConfig(Reader& reader)
{
    Config result;
    result.sections = ReadSections(reader);
    result.replayMode = reader.String();
    result.featureFlags = reader.Integer(UINT64_BYTES);
    result.cacheControlMode = static_cast<uint32_t>(reader.Integer(UINT32_BYTES));
    ValidateConfig(result);
    return result;
}
void ValidateResult(const Result& result)
{
    Config config;
    config.sections = result.sections;
    ValidateConfig(config);
    Check(result.manifest.size() <= MAX_ARTIFACT_COUNT, "invalid artifact count");
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
    std::size_t start = PATH_START_OFFSET;
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
        start = separator + PATH_COMPONENT_INCREMENT;
    }
    Check(components.size() <= MAX_ARTIFACT_PATH_COMPONENTS, "artifact name is not allowed");
    if (components.size() == COLLECTION_PATH_COMPONENTS) {
        Check(
            components[0].rfind("collection-p", PATH_START_OFFSET) == PATH_START_OFFSET,
            "artifact name is not allowed");
    } else if (components.size() == STAGING_PATH_COMPONENTS) {
        Check(components[0] == ".biu-staging" && components[1] == "process-uds", "artifact name is not allowed");
    }
    return components;
}
} // namespace

void ValidateConfig(const Config& config)
{
    Check(!config.sections.empty() && config.sections.size() <= MAX_SECTION_COUNT, "invalid section count");
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
    Check(config.featureFlags == NO_FEATURE_FLAGS, "unsupported feature flags");
    Check(
        config.cacheControlMode == kCacheControlModeAll || config.cacheControlMode == kCacheControlModeNone,
        "unsupported cache control mode");
}
DataKind KindForName(const std::string& name)
{
    const auto components = ArtifactPathComponents(name);
    const std::string& base = components.back();
    if (base == "HardwareInfo.jsonl" && components.size() == 1) {
        return DataKind::HardwareInfo;
    }
    const bool pipelinePath =
        components.size() == STAGING_PATH_COMPONENTS &&
        (base == "manifest.json" || (base.rfind("result-", 0) == 0 && base.size() > RESULT_NAME_MIN_BYTES &&
                                     base.compare(base.size() - JSON_SUFFIX_BYTES, JSON_SUFFIX_BYTES, ".json") == 0));
    if ((base == "PipeTrace.json" && components.size() == 1) || pipelinePath) {
        return DataKind::PipelineTrace;
    }
    if (base == "summary.db" || base == "objdump.txt" || base == "symbolizer.json") {
        Check(components.size() <= MAX_PM_SAMPLING_PATH_COMPONENTS, "invalid PmSampling artifact path");
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
    if (base == "summary.db") {
        return ArtifactType::Db;
    }
    if (base == "objdump.txt") {
        return ArtifactType::Txt;
    }
    if (base == "symbolizer.json") {
        return ArtifactType::Json;
    }
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
    if (kind == DataKind::PmSampling) {
        return Type::PmSampling;
    }
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
    writer.Integer(value.pid, UINT32_BYTES);
    writer.Integer(value.uid, UINT32_BYTES);
    WriteConfig(writer, value.config);
    return writer.data;
}
Hello DecodeHello(const Bytes& value)
{
    Reader reader(value);
    Hello result;
    result.pid = reader.Integer(UINT32_BYTES);
    result.uid = reader.Integer(UINT32_BYTES);
    result.config = ReadConfig(reader);
    reader.End();
    return result;
}
Bytes EncodeReady(const Ready& value)
{
    Writer writer;
    writer.Integer(value.pid, UINT32_BYTES);
    writer.Integer(value.uid, UINT32_BYTES);
    writer.Integer(value.status, UINT32_BYTES);
    return writer.data;
}
Ready DecodeReady(const Bytes& value)
{
    Reader reader(value);
    Ready result;
    result.pid = reader.Integer(UINT32_BYTES);
    result.uid = reader.Integer(UINT32_BYTES);
    result.status = reader.Integer(UINT32_BYTES);
    reader.End();
    return result;
}
Bytes EncodeArtifactBegin(const ArtifactBegin& value)
{
    Check(KindForName(value.name) == value.kind, "artifact kind does not match name");
    Check(value.artifactType == ArtifactTypeForName(value.name), "artifact type does not match kind");
    Writer writer;
    writer.Integer(value.id, UINT64_BYTES);
    writer.Integer(static_cast<uint16_t>(value.kind), UINT16_BYTES);
    writer.Integer(static_cast<uint16_t>(value.artifactType), UINT16_BYTES);
    writer.String(value.name);
    return writer.data;
}
ArtifactBegin DecodeArtifactBegin(const Bytes& value)
{
    Reader reader(value);
    ArtifactBegin result;
    result.id = reader.Integer(UINT64_BYTES);
    result.kind = static_cast<DataKind>(reader.Integer(UINT16_BYTES));
    result.artifactType = static_cast<ArtifactType>(reader.Integer(UINT16_BYTES));
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
    writer.Integer(value.id, UINT64_BYTES);
    writer.Integer(value.offset, UINT64_BYTES);
    writer.Integer(value.data.size(), UINT32_BYTES);
    writer.data.insert(writer.data.end(), value.data.begin(), value.data.end());
    return writer.data;
}
ArtifactChunk DecodeArtifactChunk(const Bytes& value)
{
    Reader reader(value);
    ArtifactChunk result;
    result.id = reader.Integer(UINT64_BYTES);
    result.offset = reader.Integer(UINT64_BYTES);
    result.data = reader.Blob(kChunkBytes);
    reader.End();
    Check(result.data.size() <= std::numeric_limits<uint64_t>::max() - result.offset, "artifact byte offset overflow");
    return result;
}
Bytes EncodeArtifactEnd(const ArtifactEnd& value)
{
    Writer writer;
    writer.Integer(value.id, UINT64_BYTES);
    writer.Integer(value.bytes, UINT64_BYTES);
    writer.Integer(value.records, UINT64_BYTES);
    return writer.data;
}
ArtifactEnd DecodeArtifactEnd(const Bytes& value)
{
    Reader reader(value);
    ArtifactEnd result;
    result.id = reader.Integer(UINT64_BYTES);
    result.bytes = reader.Integer(UINT64_BYTES);
    result.records = reader.Integer(UINT64_BYTES);
    reader.End();
    return result;
}
Bytes EncodeResult(const Result& value)
{
    ValidateResult(value);
    Writer writer;
    writer.Integer(value.status, UINT32_BYTES);
    writer.Integer(value.code, UINT32_BYTES);
    writer.Integer(value.complete ? BOOLEAN_TRUE : BOOLEAN_FALSE, UINT8_BYTES);
    writer.Integer(value.degraded ? BOOLEAN_TRUE : BOOLEAN_FALSE, UINT8_BYTES);
    writer.Integer(value.replayCount, UINT64_BYTES);
    writer.Integer(value.rowCount, UINT64_BYTES);
    writer.Integer(value.droppedCount, UINT64_BYTES);
    writer.Integer(value.errorCount, UINT64_BYTES);
    WriteSections(writer, value.sections);
    writer.Integer(value.manifest.size(), UINT32_BYTES);
    for (const auto& item : value.manifest) {
        writer.Integer(item.id, UINT64_BYTES);
        writer.String(item.name);
        writer.Integer(item.bytes, UINT64_BYTES);
    }
    return writer.data;
}
Result DecodeResult(const Bytes& value)
{
    Reader reader(value);
    Result result;
    result.status = reader.Integer(UINT32_BYTES);
    result.code = reader.Integer(UINT32_BYTES);
    const auto complete = reader.Integer(UINT8_BYTES);
    const auto degraded = reader.Integer(UINT8_BYTES);
    Check(complete <= BOOLEAN_TRUE && degraded <= BOOLEAN_TRUE, "invalid result flags");
    result.complete = complete != BOOLEAN_FALSE;
    result.degraded = degraded != BOOLEAN_FALSE;
    result.replayCount = reader.Integer(UINT64_BYTES);
    result.rowCount = reader.Integer(UINT64_BYTES);
    result.droppedCount = reader.Integer(UINT64_BYTES);
    result.errorCount = reader.Integer(UINT64_BYTES);
    result.sections = ReadSections(reader);
    const auto count = reader.Integer(UINT32_BYTES);
    Check(count <= MAX_ARTIFACT_COUNT, "invalid manifest length");
    for (uint64_t i = 0; i < count; ++i) {
        ManifestEntry item;
        item.id = reader.Integer(UINT64_BYTES);
        item.name = reader.String();
        item.bytes = reader.Integer(UINT64_BYTES);
        result.manifest.push_back(item);
    }
    reader.End();
    ValidateResult(result);
    return result;
}
Bytes EncodeError(const Error& value)
{
    Writer writer;
    writer.Integer(value.code, UINT32_BYTES);
    writer.String(value.message, MAX_ERROR_MESSAGE_BYTES);
    writer.String(value.domain);
    writer.String(value.phase);
    return writer.data;
}
Error DecodeError(const Bytes& value)
{
    Reader reader(value);
    Error result;
    result.code = reader.Integer(UINT32_BYTES);
    result.message = reader.String(MAX_ERROR_MESSAGE_BYTES);
    result.domain = reader.String();
    result.phase = reader.String();
    reader.End();
    return result;
}
} // namespace npucompute::ipc
