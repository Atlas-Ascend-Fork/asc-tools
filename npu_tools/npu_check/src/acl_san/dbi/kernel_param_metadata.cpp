/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "dbi/kernel_param_metadata.h"
#include "dbi/trace_buffer_abi.h"

#include <algorithm>
#include <cstddef>
#include <elf.h>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace aclsan {
namespace {
constexpr uint16_t PARAM_SUMMARY = 16;
constexpr uint16_t PARAM_INFO = 17;
constexpr size_t SUMMARY_SIZE = 12;
constexpr size_t PARAM_INFO_SIZE = 36;
constexpr uint32_t POINTER_SIZE = 8;
constexpr size_t BITS_PER_BYTE = std::numeric_limits<uint8_t>::digits;
constexpr size_t TLV_LENGTH_OFFSET = sizeof(uint16_t);
constexpr size_t TLV_HEADER_BYTES = sizeof(uint16_t) * 2;
constexpr size_t PARAM_ORDINAL_OFFSET = sizeof(uint32_t);
constexpr size_t PARAM_OFFSET_OFFSET = PARAM_ORDINAL_OFFSET + sizeof(uint32_t);
constexpr size_t PARAM_SIZE_OFFSET = PARAM_OFFSET_OFFSET + sizeof(uint32_t);
constexpr size_t PARAM_ALIGNMENT_OFFSET = PARAM_SIZE_OFFSET + sizeof(uint32_t);
constexpr size_t PARAM_TYPE_OFFSET = PARAM_ALIGNMENT_OFFSET + sizeof(uint16_t);
constexpr size_t SUMMARY_ARGUMENT_BYTES_OFFSET = sizeof(uint32_t);
constexpr uint16_t POINTER_PARAMETER_TYPE = 2;

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool InRange(size_t size, uint64_t offset, uint64_t length) { return offset <= size && length <= size - offset; }

uint64_t Read(const std::string& data, size_t offset, size_t size)
{
    Require(InRange(data.size(), offset, size), "truncated ELF or parameter metadata");
    uint64_t result = 0;
    for (size_t i = 0; i < size; ++i) {
        result |= static_cast<uint64_t>(static_cast<unsigned char>(data[offset + i])) << (i * BITS_PER_BYTE);
    }
    return result;
}

void Write(std::string& data, size_t offset, uint64_t value, size_t size)
{
    Require(InRange(data.size(), offset, size), "invalid metadata write range");
    for (size_t i = 0; i < size; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * BITS_PER_BYTE)) & 0xffU);
    }
}

struct Section {
    size_t header;
    uint64_t offset;
    uint64_t size;
    uint64_t alignment;
    uint64_t flags;
};

void RequireElf64Header(const std::string& data)
{
    Require(
        data.size() >= sizeof(Elf64_Ehdr) && data.compare(0, SELFMAG, ELFMAG) == 0 &&
            Read(data, EI_CLASS, 1) == ELFCLASS64 && Read(data, EI_DATA, 1) == ELFDATA2LSB &&
            Read(data, EI_VERSION, 1) == EV_CURRENT &&
            Read(data, offsetof(Elf64_Ehdr, e_version), sizeof(Elf64_Ehdr::e_version)) == EV_CURRENT &&
            Read(data, offsetof(Elf64_Ehdr, e_ehsize), sizeof(Elf64_Ehdr::e_ehsize)) == sizeof(Elf64_Ehdr),
        "expected ELF64 little-endian version 1");
}

void SelectSection(
    std::map<std::string, Section>& sections, const std::string& name, const Section& section, uint64_t type,
    bool readOnly, size_t fileSize)
{
    if (readOnly) {
        if (type == static_cast<uint32_t>(SHT_PROGBITS) && (section.flags & static_cast<uint64_t>(SHF_ALLOC)) != 0U &&
            (section.flags & (static_cast<uint64_t>(SHF_WRITE) | static_cast<uint64_t>(SHF_TLS))) == 0 &&
            section.size != 0) {
            Require(sections.emplace(name, section).second, "duplicate read-only section");
        }
        return;
    }
    constexpr char kMetadataPrefix[] = ".ascend.meta.";
    if (name.compare(0, sizeof(kMetadataPrefix) - 1, kMetadataPrefix) != 0) {
        return;
    }
    Require(
        type == static_cast<uint32_t>(SHT_NOTE) && (section.flags & static_cast<uint64_t>(SHF_ALLOC)) == 0U,
        "kernel metadata must be a non-allocated NOTE section");
    Require(
        section.alignment == 0 || ((section.alignment & (section.alignment - 1)) == 0 && section.alignment <= fileSize),
        "invalid metadata alignment");
    Require(sections.emplace(name, section).second, "duplicate kernel metadata section");
}

std::map<std::string, Section> ReadSections(const std::string& data, bool readOnly = false)
{
    RequireElf64Header(data);
    const uint64_t table = Read(data, offsetof(Elf64_Ehdr, e_shoff), sizeof(Elf64_Ehdr::e_shoff));
    const uint64_t count = Read(data, offsetof(Elf64_Ehdr, e_shnum), sizeof(Elf64_Ehdr::e_shnum));
    const uint64_t namesIndex = Read(data, offsetof(Elf64_Ehdr, e_shstrndx), sizeof(Elf64_Ehdr::e_shstrndx));
    Require(
        Read(data, offsetof(Elf64_Ehdr, e_shentsize), sizeof(Elf64_Ehdr::e_shentsize)) == sizeof(Elf64_Shdr) &&
            count > 0 && namesIndex > 0 && namesIndex < count && table >= sizeof(Elf64_Ehdr) &&
            InRange(data.size(), table, count * sizeof(Elf64_Shdr)),
        "invalid or unsupported ELF section table");
    const size_t namesHeader = table + namesIndex * sizeof(Elf64_Shdr);
    const uint64_t namesOffset =
        Read(data, namesHeader + offsetof(Elf64_Shdr, sh_offset), sizeof(Elf64_Shdr::sh_offset));
    const uint64_t namesSize = Read(data, namesHeader + offsetof(Elf64_Shdr, sh_size), sizeof(Elf64_Shdr::sh_size));
    Require(
        Read(data, namesHeader + offsetof(Elf64_Shdr, sh_type), sizeof(Elf64_Shdr::sh_type)) == SHT_STRTAB &&
            InRange(data.size(), namesOffset, namesSize),
        "invalid ELF section names");
    std::map<std::string, Section> sections;
    for (size_t i = 1; i < count; ++i) {
        const size_t header = table + i * sizeof(Elf64_Shdr);
        const uint64_t nameOffset = Read(data, header + offsetof(Elf64_Shdr, sh_name), sizeof(Elf64_Shdr::sh_name));
        const uint64_t type = Read(data, header + offsetof(Elf64_Shdr, sh_type), sizeof(Elf64_Shdr::sh_type));
        Section section{
            header, Read(data, header + offsetof(Elf64_Shdr, sh_offset), sizeof(Elf64_Shdr::sh_offset)),
            Read(data, header + offsetof(Elf64_Shdr, sh_size), sizeof(Elf64_Shdr::sh_size)),
            Read(data, header + offsetof(Elf64_Shdr, sh_addralign), sizeof(Elf64_Shdr::sh_addralign)),
            Read(data, header + offsetof(Elf64_Shdr, sh_flags), sizeof(Elf64_Shdr::sh_flags))};
        Require(
            type == static_cast<uint32_t>(SHT_NOBITS) || InRange(data.size(), section.offset, section.size),
            "ELF section exceeds file bounds");
        Require(nameOffset < namesSize, "invalid ELF section name offset");
        const size_t end = data.find('\0', namesOffset + nameOffset);
        Require(end != std::string::npos && end < namesOffset + namesSize, "unterminated ELF section name");
        if ((section.flags & static_cast<uint64_t>(SHF_ALLOC)) != 0U && type != static_cast<uint32_t>(SHT_NOBITS) &&
            section.size != 0) {
            Require(
                section.offset >= table + count * sizeof(Elf64_Shdr) || section.offset + section.size <= table,
                "allocated ELF section overlaps section table");
        }
        const std::string name = data.substr(namesOffset + nameOffset, end - namesOffset - nameOffset);
        SelectSection(sections, name, section, type, readOnly, data.size());
    }
    return sections;
}

struct Tlv {
    uint16_t tag;
    std::string value;
};

struct Metadata {
    std::vector<Tlv> values;
    std::vector<std::string> parameters;
    bool hasParameterLayout;
};

Metadata ReadMetadata(const std::string& data, const Section& section)
{
    const std::string bytes = data.substr(section.offset, section.size);
    std::vector<Tlv> values;
    size_t summaryCount = 0;
    size_t infoCount = 0;
    uint32_t count = 0;
    for (size_t offset = 0; offset < bytes.size();) {
        const auto tag = static_cast<uint16_t>(Read(bytes, offset, sizeof(uint16_t)));
        const size_t length = Read(bytes, offset + TLV_LENGTH_OFFSET, sizeof(uint16_t));
        offset += TLV_HEADER_BYTES;
        Require(InRange(bytes.size(), offset, length), "truncated parameter TLV");
        if (tag == PARAM_SUMMARY) {
            Require(length == SUMMARY_SIZE, "unsupported ParamSummary layout");
            ++summaryCount;
            count = static_cast<uint32_t>(Read(bytes, offset, sizeof(uint32_t)));
        } else if (tag == PARAM_INFO) {
            Require(length == PARAM_INFO_SIZE, "unsupported ParamInfo layout");
            ++infoCount;
        }
        values.push_back({tag, bytes.substr(offset, length)});
        offset += length;
    }
    if (summaryCount == 0) {
        Require(infoCount == 0, "parameter descriptions are missing ParamSummary");
        return {std::move(values), {}, false};
    }
    Require(summaryCount == 1 && infoCount == count, "inconsistent parameter summary and descriptions");
    std::vector<std::string> parameters(count);
    std::vector<bool> seen(count, false);
    for (const auto& item : values) {
        if (item.tag != PARAM_INFO) {
            continue;
        }
        const uint64_t ordinal = Read(item.value, PARAM_ORDINAL_OFFSET, sizeof(uint32_t));
        Require(ordinal < count, "parameter ordinal is out of range");
        Require(!seen[ordinal], "duplicate parameter ordinal");
        seen[ordinal] = true;
        parameters[ordinal] = item.value;
    }
    return {std::move(values), std::move(parameters), true};
}

void AppendTlv(std::string& output, uint16_t tag, const std::string& value)
{
    const size_t header = output.size();
    output.resize(header + TLV_HEADER_BYTES);
    Write(output, header, tag, sizeof(uint16_t));
    Write(output, header + TLV_LENGTH_OFFSET, value.size(), sizeof(uint16_t));
    output += value;
}

std::string NormalizeMetadata(const Metadata& original, const Metadata& patched, uint32_t traceOffset)
{
    if (!original.hasParameterLayout) {
        std::string output;
        for (const auto& item : patched.values) {
            // Legacy HostArgs metadata has no Runtime parameter layout. Keep the
            // tuned non-parameter TLVs, but do not expose a synthetic ArgsArray ABI.
            if (item.tag != PARAM_SUMMARY && item.tag != PARAM_INFO) {
                AppendTlv(output, item.tag, item.value);
            }
        }
        return output;
    }
    Require(patched.hasParameterLayout, "instrumented parameter summary is missing");
    for (const auto& item : original.values) {
        if (item.tag == PARAM_SUMMARY) {
            Require(
                Read(item.value, SUMMARY_ARGUMENT_BYTES_OFFSET, sizeof(uint32_t)) <= traceOffset,
                "original argument area exceeds trace offset");
        }
    }
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& parameter : original.parameters) {
        const uint64_t offset = Read(parameter, PARAM_OFFSET_OFFSET, sizeof(uint32_t));
        const uint64_t size = Read(parameter, PARAM_SIZE_OFFSET, sizeof(uint32_t));
        Require(
            size > 0 && offset <= traceOffset && size <= traceOffset - offset,
            "original parameter overlaps trace pointer");
        ranges.emplace_back(offset, offset + size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) {
        Require(ranges[i - 1].second <= ranges[i].first, "original parameters overlap");
    }
    // 与 dav-3510 bisheng-tune 的 synthetic pointer ParamInfo 一致：8 字节、8 字节对齐、类型 2。
    std::string pointer(PARAM_INFO_SIZE, '\0');
    Write(pointer, PARAM_ORDINAL_OFFSET, original.parameters.size(), sizeof(uint32_t));
    Write(pointer, PARAM_OFFSET_OFFSET, traceOffset, sizeof(uint32_t));
    Write(pointer, PARAM_SIZE_OFFSET, POINTER_SIZE, sizeof(uint32_t));
    Write(pointer, PARAM_ALIGNMENT_OFFSET, POINTER_SIZE, sizeof(uint16_t));
    Write(pointer, PARAM_TYPE_OFFSET, POINTER_PARAMETER_TYPE, sizeof(uint16_t));
    std::string output;
    for (const auto& item : patched.values) {
        if (item.tag == PARAM_INFO) {
            continue;
        }
        if (item.tag != PARAM_SUMMARY) {
            AppendTlv(output, item.tag, item.value);
            continue;
        }
        const uint64_t count = Read(item.value, 0, sizeof(uint32_t));
        Require(
            count == original.parameters.size() || count == original.parameters.size() + 1,
            "unexpected instrumented parameter count");
        std::string summary = item.value;
        Write(summary, 0, original.parameters.size() + 1, sizeof(uint32_t));
        Write(summary, SUMMARY_ARGUMENT_BYTES_OFFSET, traceOffset + POINTER_SIZE, sizeof(uint32_t));
        AppendTlv(output, PARAM_SUMMARY, summary);
        for (const auto& parameter : original.parameters) {
            AppendTlv(output, PARAM_INFO, parameter);
        }
        AppendTlv(output, PARAM_INFO, pointer);
    }
    return output;
}
} // namespace

bool GetMaximumKernelArgumentArea(
    const std::string& kernelElf, uint32_t& maximumArea, bool& found, std::string& diagnostic)
{
    maximumArea = 0;
    found = false;
    try {
        for (const auto& entry : ReadSections(kernelElf)) {
            const auto metadata = ReadMetadata(kernelElf, entry.second);
            if (!metadata.hasParameterLayout) {
                continue;
            }
            uint32_t summaryArea = 0;
            uint64_t parameterEnd = 0;
            for (const auto& item : metadata.values) {
                if (item.tag == PARAM_SUMMARY) {
                    summaryArea =
                        static_cast<uint32_t>(Read(item.value, SUMMARY_ARGUMENT_BYTES_OFFSET, sizeof(uint32_t)));
                }
            }
            for (const auto& parameter : metadata.parameters) {
                const uint64_t parameterOffset = Read(parameter, PARAM_OFFSET_OFFSET, sizeof(uint32_t));
                const uint64_t parameterSize = Read(parameter, PARAM_SIZE_OFFSET, sizeof(uint32_t));
                parameterEnd = std::max(parameterEnd, parameterOffset + parameterSize);
            }
            const uint64_t area = std::max<uint64_t>(summaryArea, parameterEnd);
            Require(area <= std::numeric_limits<uint32_t>::max(), "kernel argument area exceeds 32-bit range");
            maximumArea = std::max(maximumArea, static_cast<uint32_t>(area));
            found = true;
        }
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}

bool ModifyKernelParamMetadata(
    const std::string& original, std::string& patched, uint32_t traceOffset, std::string& diagnostic,
    const uint64_t* tilingKey)
{
    try {
        Require(
            traceOffset >= POINTER_SIZE && traceOffset % POINTER_SIZE == 0 &&
                traceOffset <= std::numeric_limits<uint32_t>::max() - POINTER_SIZE,
            "invalid trace pointer offset");
        const auto sources = ReadSections(original);
        const auto destinations = ReadSections(patched);
        Require(!sources.empty(), "no kernel parameter metadata found");
        std::string result = patched;
        size_t selected = 0;
        for (const auto& entry : sources) {
            if (tilingKey != nullptr) {
                const auto endsWith = [&entry](const std::string& suffix) {
                    return entry.first.size() >= suffix.size() &&
                           entry.first.compare(entry.first.size() - suffix.size(), suffix.size(), suffix) == 0;
                };
                const std::string suffix = "_" + std::to_string(*tilingKey);
                if (!endsWith(suffix) && !endsWith(suffix + "_mix_aic") && !endsWith(suffix + "_mix_aiv")) {
                    continue;
                }
            }
            ++selected;
            const auto destination = destinations.find(entry.first);
            Require(destination != destinations.end(), "instrumented kernel metadata section is missing");
            const Section& section = destination->second;
            const auto sourceMetadata = ReadMetadata(original, entry.second);
            const auto destinationMetadata = ReadMetadata(patched, section);
            const auto metadata = NormalizeMetadata(sourceMetadata, destinationMetadata, traceOffset);
            if (metadata == patched.substr(section.offset, section.size)) {
                continue;
            }
            // 追加非加载元数据，只更新对应节头；不移动任何原始代码、数据或 program header。
            const size_t alignment = std::max<uint64_t>(4, section.alignment);
            const size_t padding = (alignment - result.size() % alignment) % alignment;
            Require(
                padding <= result.max_size() - result.size() &&
                    metadata.size() <= result.max_size() - result.size() - padding,
                "metadata output is too large");
            result.append(padding, '\0');
            const size_t offset = result.size();
            result += metadata;
            Write(result, section.header + offsetof(Elf64_Shdr, sh_offset), offset, sizeof(Elf64_Shdr::sh_offset));
            Write(result, section.header + offsetof(Elf64_Shdr, sh_size), metadata.size(), sizeof(Elf64_Shdr::sh_size));
        }
        Require(selected != 0, "selected entry has no matching kernel metadata");
        patched.swap(result);
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}
bool GetReadOnlyBinarySections(const std::string& elf, std::vector<BinarySectionRange>& ranges, std::string& diagnostic)
{
    ranges.clear();
    try {
        std::vector<BinarySectionRange> result;
        for (const auto& entry : ReadSections(elf, true)) {
            const auto& section = entry.second;
            const uint64_t address =
                Read(elf, section.header + offsetof(Elf64_Shdr, sh_addr), sizeof(Elf64_Shdr::sh_addr));
            Require(section.size <= UINT64_MAX - address, "read-only section address overflow");
            result.push_back({address, section.size});
        }
        ranges.swap(result);
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}
namespace {
bool EndsWith(const std::string& name, const std::string& suffix)
{
    return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<uint64_t> KernelFunctionEntry(const std::string& elf, const Section& section, const std::string& name)
{
    constexpr uint16_t entryTag = 14;
    constexpr size_t entryOffset = 4;
    constexpr uint8_t entryDisabled = 0x80;
    bool found = false;
    std::optional<uint64_t> explicitEntry;
    for (const auto& tlv : ReadMetadata(elf, section).values) {
        if (tlv.tag != entryTag) {
            continue;
        }
        Require(!found && tlv.value.size() == entryOffset + sizeof(uint64_t), "invalid function entry TLV");
        found = true;
        const auto flag = Read(tlv.value, 0, sizeof(uint8_t));
        Require(flag == 0 || flag == entryDisabled, "invalid function entry flag");
        if (flag == 0) {
            explicitEntry = Read(tlv.value, entryOffset, sizeof(uint64_t));
        }
    }
    if (found) {
        return explicitEntry;
    }

    // Runtime uses the numeric suffix only for legacy metadata. A name with
    // no numeric suffix is registered with the default key 0.
    const auto end = EndsWith(name, "_mix_aiv") ? name.size() - sizeof("_mix_aiv") + 1 : name.size();
    const auto separator = end != 0 ? name.rfind('_', end - 1) : std::string::npos;
    const auto begin = separator == std::string::npos ? 0 : separator + 1;
    constexpr uint64_t decimalBase = 10;
    uint64_t entry = 0;
    for (size_t i = begin; i < end; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
        const auto digit = static_cast<uint64_t>(static_cast<int>(name[i]) - static_cast<int>('0'));
        if (entry > (std::numeric_limits<uint64_t>::max() - digit) / decimalBase) {
            return 0;
        }
        entry = entry * decimalBase + digit;
    }
    return entry;
}

KernelUbMetadata ReadUbMetadata(const std::string& elf, const Section& section, bool requireVfType = true)
{
    constexpr uint16_t kernelTypeTag = 12;
    constexpr uint16_t compilerUbAllocationTag = 7;
    KernelUbMetadata result;
    for (const auto& tlv : ReadMetadata(elf, section).values) {
        if (tlv.tag != kernelTypeTag && tlv.tag != compilerUbAllocationTag) {
            continue;
        }
        Require(tlv.value.size() == sizeof(uint32_t), "invalid UB metadata value length");
        const uint32_t value = static_cast<uint32_t>(Read(tlv.value, 0, sizeof(uint32_t)));
        if (tlv.tag == kernelTypeTag) {
            Require(
                (!requireVfType && value == ACLSAN_KERNEL_VF_TYPE_UNKNOWN) ||
                    (value >= ACLSAN_KERNEL_VF_TYPE_NO_VF && value <= ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF),
                "unknown AIV kernel type");
            Require(
                result.kernelVfType == ACLSAN_KERNEL_VF_TYPE_UNKNOWN || result.kernelVfType == value,
                "conflicting AIV kernel types");
            result.kernelVfType = value;
        } else {
            Require(
                !result.compilerAllocUbValid || result.compilerAllocUbBytes == value,
                "conflicting compiler UB allocation sizes");
            result.compilerAllocUbBytes = value;
            result.compilerAllocUbValid = true;
        }
    }
    Require(!requireVfType || result.kernelVfType != ACLSAN_KERNEL_VF_TYPE_UNKNOWN, "missing AIV kernel type");
    return result;
}
} // namespace

bool GetKernelUbMetadata(
    const std::string& elf, const std::string& functionName, KernelUbMetadata& metadata, std::string& diagnostic,
    const uint64_t* tilingKey)
{
    metadata = {};
    try {
        Require(!functionName.empty() || tilingKey != nullptr, "kernel selector is missing");
        const auto sections = ReadSections(elf);
        const Section* selected = nullptr;
        for (const auto& entry : sections) {
            const std::string name = entry.first.substr(sizeof(".ascend.meta.") - 1);
            // A mixed AIC/AIV kernel's UB belongs to its AIV half.
            if (EndsWith(name, "_mix_aic")) {
                continue;
            }
            if (!functionName.empty() && name != functionName && name != functionName + "_mix_aiv") {
                continue;
            }
            if (tilingKey != nullptr) {
                const auto kernelEntry = KernelFunctionEntry(elf, entry.second, name);
                if (!kernelEntry.has_value() || *kernelEntry != *tilingKey) {
                    continue;
                }
            }
            Require(selected == nullptr, "ambiguous kernel UB metadata selector");
            selected = &entry.second;
        }
        Require(selected != nullptr, "selected kernel UB metadata is missing");
        metadata = ReadUbMetadata(elf, *selected);
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}
bool GetKernelUbMetadata(const std::string& elf, KernelUbMetadataMap& kernels, std::string& diagnostic)
{
    kernels.clear();
    diagnostic.clear();
    try {
        const auto sections = ReadSections(elf);
        const bool linked = Read(elf, offsetof(Elf64_Ehdr, e_type), sizeof(Elf64_Ehdr::e_type)) == ET_EXEC;
        constexpr std::string_view metadataPrefix = ".ascend.meta.";
        KernelUbMetadataMap parsed;
        for (const auto& section : sections) {
            const auto name = section.first.substr(metadataPrefix.size());
            // UB belongs to the AIV half, as in the single-function selector.
            if (EndsWith(name, "_mix_aic")) {
                continue;
            }
            const auto metadata = ReadUbMetadata(elf, section.second, false);
            KernelUbMetadataEntry entry{};
            entry.functionEntry = KernelFunctionEntry(elf, section.second, name);
            auto& info = entry.launch;
            info.compilerAllocatedBytes = metadata.compilerAllocUbBytes;
            info.kernelVfType = metadata.kernelVfType;
            // Runtime defaults an absent VF tag to a non-SIMT kernel.
            if (metadata.compilerAllocUbValid && metadata.kernelVfType != ACLSAN_KERNEL_VF_TYPE_UNKNOWN) {
                info.flags |= static_cast<uint32_t>(ACLSAN_UB_METADATA_COMPILER);
                const bool simt = info.kernelVfType == ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY ||
                                  info.kernelVfType == ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF;
                if (linked && simt && info.compilerAllocatedBytes >= DAV3510_UB_DEFAULT_RESERVED_BYTES) {
                    info.userStaticBytes = info.compilerAllocatedBytes - DAV3510_UB_DEFAULT_RESERVED_BYTES;
                    info.flags |= static_cast<uint32_t>(ACLSAN_UB_METADATA_STATIC);
                }
            }
            parsed.emplace(name, std::move(entry));
        }
        kernels = std::move(parsed);
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}
} // namespace aclsan
