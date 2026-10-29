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
#include "binary_instrumenter.h"
#include <sys/stat.h>

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <elf.h>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <fstream>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace aclpti::profiling {
namespace {
namespace fs = boost::filesystem;

// Kernel-end probe source. It is embedded in the library instead of being packaged as a separate
// resource, written to the probe cache directory and compiled by Bisheng on first instrumentation.
// Keep this text in sync with the probe contract: bar.all, a fixed NOP/DFX_REGION sequence and the
// three implicit Bisheng probe ABI parameters (which the probe does not dereference).
constexpr char KERNEL_END_PROBE_SOURCE[] =
    R"PROBE(extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_compute_before_kernel_end(
    __gm__ unsigned char*, unsigned long long, unsigned int)
{
    asm volatile("bar.all" ::: "memory");
    asm volatile(".rept 32\n\tnop\n\t.endr");
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xd88));
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xd99));
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xdaa));
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xdbb));
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xdcc));
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xddd));
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xdee));
    asm volatile("DFX_REGION.S %0" ::"l"(0xdff));
    asm volatile(".rept 3500\n\tnop\n\t.endr");
    asm volatile("DFX_REGION.S %0\n\tnop" ::"l"(0xdff));
}
)PROBE";

// Keep this name in sync with the symbol in KERNEL_END_PROBE_SOURCE.
constexpr char KERNEL_END_PROBE_SYMBOL[] = "__npu_compute_before_kernel_end";

// Semantic fields for npu_compute's fixed probe record; its instruction ID is not shared with npu_check.
constexpr std::uint16_t CONTROL_RECORD_VERSION = 0;
constexpr std::uint16_t KERNEL_END_INSTRUCTION_ID = 397; // before kernel end
constexpr std::uint16_t KERNEL_END_PROBE_FUNCTION_INDEX = 0;
constexpr std::uint16_t KERNEL_END_PROBE_PARAMETER_COUNT = 0;
constexpr std::uint16_t CONTROL_RECORD_RESERVED_VALUE = 0;
constexpr std::size_t KERNEL_END_PROBE_SYMBOL_SIZE = sizeof(KERNEL_END_PROBE_SYMBOL) - 1;
constexpr std::size_t FUNCTION_NAME_LIST_PADDING_SIZE = sizeof(std::uint32_t);
constexpr char FUNCTION_NAME_LIST_PADDING[FUNCTION_NAME_LIST_PADDING_SIZE] = {};

constexpr std::size_t CONTROL_RECORD_HEADER_SIZE = sizeof(std::uint32_t) + 4 * sizeof(std::uint16_t);
constexpr std::size_t PROBE_BINDING_SIZE = 3 * sizeof(std::uint16_t);
constexpr std::uint32_t KERNEL_END_FUNCTION_NAME_LIST_SIZE =
    static_cast<std::uint32_t>(sizeof(std::uint32_t) + KERNEL_END_PROBE_SYMBOL_SIZE);
constexpr std::uint32_t KERNEL_END_CONTROL_SIZE = static_cast<std::uint32_t>(
    CONTROL_RECORD_HEADER_SIZE + PROBE_BINDING_SIZE + KERNEL_END_FUNCTION_NAME_LIST_SIZE +
    FUNCTION_NAME_LIST_PADDING_SIZE);

constexpr const char* PROBE_SOURCE_NAME = "probe.cpp";
constexpr const char* PROBE_OBJECT_NAME = "kernel_end.o";
constexpr const char* CONTROL_RECORD_NAME = "ctrl.bin";

struct KernelEndTools {
    fs::path linker;   // ld.lld
    fs::path tuner;    // bisheng-tune
    fs::path compiler; // bisheng
};

constexpr std::uint16_t PARAM_SUMMARY = 16;
constexpr std::uint16_t PARAM_INFO = 17;
constexpr std::size_t SUMMARY_SIZE = 12;
constexpr std::size_t PARAM_INFO_SIZE = 36;
constexpr std::uint32_t POINTER_SIZE = 8;
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

constexpr char KERNEL_METADATA_PREFIX[] = ".ascend.meta.";

void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool InRange(std::size_t size, std::uint64_t offset, std::uint64_t length)
{
    return offset <= size && length <= static_cast<std::uint64_t>(size) - offset;
}

std::uint64_t ReadValue(const std::string& data, std::uint64_t offset, std::size_t size)
{
    Require(size <= sizeof(std::uint64_t), "ELF value is too large");
    Require(InRange(data.size(), offset, size), "truncated ELF or parameter metadata");
    std::uint64_t value = 0;
    const auto start = static_cast<std::size_t>(offset);
    for (std::size_t index = 0; index < size; ++index) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[start + index])) << (index * BITS_PER_BYTE);
    }
    return value;
}

void WriteValue(std::string& data, std::uint64_t offset, std::uint64_t value, std::size_t size)
{
    Require(size <= sizeof(std::uint64_t), "metadata value is too large");
    Require(InRange(data.size(), offset, size), "invalid metadata write range");
    const auto start = static_cast<std::size_t>(offset);
    for (std::size_t index = 0; index < size; ++index) {
        data[start + index] = static_cast<char>((value >> (index * BITS_PER_BYTE)) & 0xffU);
    }
}

struct ElfSection {
    std::size_t header;
    std::uint32_t type;
    std::uint64_t offset;
    std::uint64_t size;
    std::uint64_t alignment;
    std::string name;
};

struct ParsedElf {
    std::vector<ElfSection> sections;
    std::map<std::string, std::size_t> metadataSections;
};

ParsedElf ReadElfSections(const std::string& data)
{
    Require(data.size() >= sizeof(Elf64_Ehdr), "Device ELF header is missing");
    Require(
        data.compare(0, SELFMAG, ELFMAG) == 0 && ReadValue(data, EI_CLASS, 1) == ELFCLASS64 &&
            ReadValue(data, EI_DATA, 1) == ELFDATA2LSB && ReadValue(data, EI_VERSION, 1) == EV_CURRENT &&
            ReadValue(data, offsetof(Elf64_Ehdr, e_version), sizeof(Elf64_Ehdr::e_version)) == EV_CURRENT &&
            ReadValue(data, offsetof(Elf64_Ehdr, e_ehsize), sizeof(Elf64_Ehdr::e_ehsize)) == sizeof(Elf64_Ehdr),
        "expected ELF64 little-endian version 1");

    const std::uint64_t sectionTable = ReadValue(data, offsetof(Elf64_Ehdr, e_shoff), sizeof(Elf64_Ehdr::e_shoff));
    const std::uint64_t sectionCount = ReadValue(data, offsetof(Elf64_Ehdr, e_shnum), sizeof(Elf64_Ehdr::e_shnum));
    const std::uint64_t sectionNamesIndex =
        ReadValue(data, offsetof(Elf64_Ehdr, e_shstrndx), sizeof(Elf64_Ehdr::e_shstrndx));
    Require(
        ReadValue(data, offsetof(Elf64_Ehdr, e_shentsize), sizeof(Elf64_Ehdr::e_shentsize)) == sizeof(Elf64_Shdr) &&
            sectionCount > 0 && sectionNamesIndex > 0 && sectionNamesIndex < sectionCount &&
            sectionTable >= sizeof(Elf64_Ehdr) && InRange(data.size(), sectionTable, sectionCount * sizeof(Elf64_Shdr)),
        "invalid or unsupported ELF section table");

    const std::uint64_t sectionNamesHeader = sectionTable + sectionNamesIndex * sizeof(Elf64_Shdr);
    const std::uint64_t sectionNamesOffset =
        ReadValue(data, sectionNamesHeader + offsetof(Elf64_Shdr, sh_offset), sizeof(Elf64_Shdr::sh_offset));
    const std::uint64_t sectionNamesSize =
        ReadValue(data, sectionNamesHeader + offsetof(Elf64_Shdr, sh_size), sizeof(Elf64_Shdr::sh_size));
    Require(
        ReadValue(data, sectionNamesHeader + offsetof(Elf64_Shdr, sh_type), sizeof(Elf64_Shdr::sh_type)) ==
                SHT_STRTAB &&
            InRange(data.size(), sectionNamesOffset, sectionNamesSize),
        "invalid ELF section names");

    ParsedElf elf;
    const std::uint64_t sectionNamesEnd = sectionNamesOffset + sectionNamesSize;
    for (std::uint64_t index = 0; index < sectionCount; ++index) {
        const std::uint64_t headerOffset = sectionTable + index * sizeof(Elf64_Shdr);
        const std::uint64_t nameOffset =
            ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_name), sizeof(Elf64_Shdr::sh_name));
        const std::uint32_t type = static_cast<std::uint32_t>(
            ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_type), sizeof(Elf64_Shdr::sh_type)));
        const auto flags = ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_flags), sizeof(Elf64_Shdr::sh_flags));
        ElfSection section{
            static_cast<std::size_t>(headerOffset),
            type,
            ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_offset), sizeof(Elf64_Shdr::sh_offset)),
            ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_size), sizeof(Elf64_Shdr::sh_size)),
            ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_addralign), sizeof(Elf64_Shdr::sh_addralign)),
            {},
        };
        Require(
            type == SHT_NOBITS || InRange(data.size(), section.offset, section.size),
            "ELF section exceeds file bounds");
        Require(nameOffset < sectionNamesSize, "invalid ELF section name offset");
        const std::uint64_t nameStart = sectionNamesOffset + nameOffset;
        const std::size_t nameEnd = data.find('\0', static_cast<std::size_t>(nameStart));
        Require(
            nameEnd != std::string::npos && static_cast<std::uint64_t>(nameEnd) < sectionNamesEnd,
            "unterminated ELF section name");

        section.name = data.substr(static_cast<std::size_t>(nameStart), nameEnd - static_cast<std::size_t>(nameStart));
        if (section.name.compare(0, sizeof(KERNEL_METADATA_PREFIX) - 1, KERNEL_METADATA_PREFIX) == 0) {
            Require(
                type == SHT_NOTE && (flags & SHF_ALLOC) == 0, "kernel metadata must be a non-allocated NOTE section");
            Require(
                section.alignment == 0 ||
                    ((section.alignment & (section.alignment - 1)) == 0 && section.alignment <= data.size()),
                "invalid metadata alignment");
            Require(
                elf.metadataSections.emplace(section.name, elf.sections.size()).second,
                "duplicate kernel metadata section");
        }
        elf.sections.push_back(std::move(section));
    }
    return elf;
}

struct Tlv {
    std::uint16_t tag;
    std::string value;
};

struct Metadata {
    std::vector<Tlv> values;
    std::vector<std::string> parameters;
    bool hasParameterLayout;
};

Metadata ReadMetadata(const std::string& data, const ElfSection& section)
{
    Require(InRange(data.size(), section.offset, section.size), "kernel metadata section exceeds file bounds");
    const std::string bytes =
        data.substr(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size));
    std::vector<Tlv> values;
    std::size_t summaryCount = 0;
    std::size_t infoCount = 0;
    std::uint32_t parameterCount = 0;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto tag = static_cast<std::uint16_t>(ReadValue(bytes, offset, sizeof(uint16_t)));
        const std::size_t length =
            static_cast<std::size_t>(ReadValue(bytes, offset + TLV_LENGTH_OFFSET, sizeof(uint16_t)));
        offset += TLV_HEADER_BYTES;
        Require(InRange(bytes.size(), offset, length), "truncated parameter TLV");
        if (tag == PARAM_SUMMARY) {
            Require(length == SUMMARY_SIZE, "unsupported ParamSummary layout");
            ++summaryCount;
            parameterCount = static_cast<std::uint32_t>(ReadValue(bytes, offset, sizeof(uint32_t)));
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
    Require(summaryCount == 1 && infoCount == parameterCount, "inconsistent parameter summary and descriptions");
    std::vector<std::string> parameters(parameterCount);
    std::vector<bool> seen(parameterCount, false);
    for (const auto& item : values) {
        if (item.tag != PARAM_INFO) {
            continue;
        }
        const std::uint64_t ordinal = ReadValue(item.value, PARAM_ORDINAL_OFFSET, sizeof(uint32_t));
        Require(ordinal < parameterCount, "parameter ordinal is out of range");
        Require(!seen[ordinal], "duplicate parameter ordinal");
        seen[ordinal] = true;
        parameters[ordinal] = item.value;
    }
    return {std::move(values), std::move(parameters), true};
}

using KernelMetadata = std::map<std::string, Metadata>;

KernelMetadata ReadKernelMetadata(const std::string& data, const ParsedElf& elf)
{
    KernelMetadata metadata;
    for (const auto& entry : elf.metadataSections) {
        metadata.emplace(entry.first, ReadMetadata(data, elf.sections[entry.second]));
    }
    return metadata;
}

void GetMaximumKernelArgumentArea(const KernelMetadata& kernels, std::uint32_t& maximumArea, bool& found)
{
    maximumArea = 0;
    found = false;
    for (const auto& entry : kernels) {
        const auto& metadata = entry.second;
        if (!metadata.hasParameterLayout) {
            continue;
        }
        std::uint32_t summaryArea = 0;
        std::uint64_t parameterEnd = 0;
        for (const auto& item : metadata.values) {
            if (item.tag == PARAM_SUMMARY) {
                summaryArea =
                    static_cast<std::uint32_t>(ReadValue(item.value, SUMMARY_ARGUMENT_BYTES_OFFSET, sizeof(uint32_t)));
            }
        }
        for (const auto& parameter : metadata.parameters) {
            const std::uint64_t parameterOffset = ReadValue(parameter, PARAM_OFFSET_OFFSET, sizeof(uint32_t));
            const std::uint64_t parameterSize = ReadValue(parameter, PARAM_SIZE_OFFSET, sizeof(uint32_t));
            parameterEnd = std::max(parameterEnd, parameterOffset + parameterSize);
        }
        const std::uint64_t area = std::max<std::uint64_t>(summaryArea, parameterEnd);
        Require(area <= std::numeric_limits<std::uint32_t>::max(), "kernel argument area exceeds 32-bit range");
        maximumArea = std::max(maximumArea, static_cast<std::uint32_t>(area));
        found = true;
    }
}

void AppendTlv(std::string& output, std::uint16_t tag, const std::string& value)
{
    Require(value.size() <= std::numeric_limits<std::uint16_t>::max(), "parameter TLV is too large");
    const std::size_t header = output.size();
    output.resize(header + TLV_HEADER_BYTES);
    WriteValue(output, header, tag, sizeof(uint16_t));
    WriteValue(output, header + TLV_LENGTH_OFFSET, value.size(), sizeof(uint16_t));
    output += value;
}

std::string NormalizeMetadata(const Metadata& original, const Metadata& patched, std::uint32_t traceArgumentOffset)
{
    if (!original.hasParameterLayout) {
        // Legacy HostArgs kernels have no ArgsArray ABI. Preserve non-parameter TLVs only.
        std::string output;
        for (const auto& item : patched.values) {
            if (item.tag != PARAM_SUMMARY && item.tag != PARAM_INFO) {
                AppendTlv(output, item.tag, item.value);
            }
        }
        return output;
    }
    Require(patched.hasParameterLayout, "instrumented parameter summary is missing");
    Require(original.parameters.size() < std::numeric_limits<std::uint32_t>::max(), "too many kernel parameters");
    for (const auto& item : original.values) {
        if (item.tag == PARAM_SUMMARY) {
            Require(
                ReadValue(item.value, SUMMARY_ARGUMENT_BYTES_OFFSET, sizeof(uint32_t)) <= traceArgumentOffset,
                "original argument area exceeds trace offset");
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto& parameter : original.parameters) {
        const std::uint64_t offset = ReadValue(parameter, PARAM_OFFSET_OFFSET, sizeof(uint32_t));
        const std::uint64_t size = ReadValue(parameter, PARAM_SIZE_OFFSET, sizeof(uint32_t));
        Require(
            size > 0 && offset <= traceArgumentOffset && size <= traceArgumentOffset - offset,
            "original parameter overlaps trace pointer");
        ranges.emplace_back(offset, offset + size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        Require(ranges[index - 1].second <= ranges[index].first, "original parameters overlap");
    }

    std::string pointer(PARAM_INFO_SIZE, '\0');
    WriteValue(pointer, PARAM_ORDINAL_OFFSET, original.parameters.size(), sizeof(uint32_t));
    WriteValue(pointer, PARAM_OFFSET_OFFSET, traceArgumentOffset, sizeof(uint32_t));
    WriteValue(pointer, PARAM_SIZE_OFFSET, POINTER_SIZE, sizeof(uint32_t));
    WriteValue(pointer, PARAM_ALIGNMENT_OFFSET, POINTER_SIZE, sizeof(uint16_t));
    WriteValue(pointer, PARAM_TYPE_OFFSET, POINTER_PARAMETER_TYPE, sizeof(uint16_t));

    std::string output;
    for (const auto& item : patched.values) {
        if (item.tag == PARAM_INFO) {
            continue;
        }
        if (item.tag != PARAM_SUMMARY) {
            AppendTlv(output, item.tag, item.value);
            continue;
        }
        const std::uint64_t count = ReadValue(item.value, 0, sizeof(uint32_t));
        Require(
            count == original.parameters.size() || count == original.parameters.size() + 1,
            "unexpected instrumented parameter count");
        std::string summary = item.value;
        WriteValue(summary, 0, original.parameters.size() + 1, sizeof(uint32_t));
        WriteValue(summary, SUMMARY_ARGUMENT_BYTES_OFFSET, traceArgumentOffset + POINTER_SIZE, sizeof(uint32_t));
        AppendTlv(output, PARAM_SUMMARY, summary);
        for (const auto& parameter : original.parameters) {
            AppendTlv(output, PARAM_INFO, parameter);
        }
        AppendTlv(output, PARAM_INFO, pointer);
    }
    return output;
}

bool ResolveTraceArgumentOffset(
    const void* data, std::size_t length, KernelMetadata& metadata, std::uint32_t& traceArgumentOffset,
    std::string& diagnostic)
{
    if (data == nullptr || length < sizeof(Elf64_Ehdr)) {
        diagnostic = "Device ELF header is missing";
        return false;
    }
    try {
        const std::string kernelElf(static_cast<const char*>(data), length);
        const ParsedElf elf = ReadElfSections(kernelElf);
        metadata = ReadKernelMetadata(kernelElf, elf);
        bool hasKernelArgumentSize = false;
        std::uint32_t maximumArgumentSize = 0;
        for (const auto& section : elf.sections) {
            if (section.name != "__CCE_KernelArgSize") {
                continue;
            }
            Require(
                section.size != 0 && section.size % sizeof(std::uint32_t) == 0 && section.type != SHT_NOBITS &&
                    InRange(kernelElf.size(), section.offset, section.size),
                "__CCE_KernelArgSize is malformed");
            for (std::uint64_t offset = 0; offset < section.size; offset += sizeof(std::uint32_t)) {
                const auto argumentSize = static_cast<std::uint32_t>(ReadValue(kernelElf, section.offset + offset, 4));
                maximumArgumentSize = std::max(maximumArgumentSize, argumentSize);
            }
            hasKernelArgumentSize = true;
        }

        std::uint32_t metadataArgumentArea = 0;
        bool hasParameterMetadata = false;
        GetMaximumKernelArgumentArea(metadata, metadataArgumentArea, hasParameterMetadata);
        if (!hasKernelArgumentSize && !hasParameterMetadata) {
            diagnostic = "__CCE_KernelArgSize and kernel parameter metadata are missing";
            return false;
        }
        maximumArgumentSize = std::max(maximumArgumentSize, metadataArgumentArea);
        constexpr std::uint32_t MINIMUM_TRACE_ARGUMENT_OFFSET = sizeof(std::uint64_t);
        maximumArgumentSize = std::max(maximumArgumentSize, MINIMUM_TRACE_ARGUMENT_OFFSET);
        Require(
            maximumArgumentSize <= std::numeric_limits<std::uint32_t>::max() - 7U,
            "kernel argument area cannot be aligned");
        traceArgumentOffset = (maximumArgumentSize + 7U) & ~7U;
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}

template <typename UInt>
bool WriteLittleEndian(std::ostream& output, UInt value)
{
    static_assert(std::is_unsigned<UInt>::value, "control record fields must be unsigned");
    for (std::size_t byteIndex = 0; byteIndex < sizeof(UInt); ++byteIndex) {
        const auto byte = static_cast<std::uint8_t>(value);
        if (!output.write(reinterpret_cast<const char*>(&byte), static_cast<std::streamsize>(sizeof(byte)))) {
            return false;
        }
        value >>= CHAR_BIT;
    }
    return true;
}

bool WriteKernelEndControl(const fs::path& directory)
{
    std::ofstream file((directory / CONTROL_RECORD_NAME).string(), std::ios::binary);
    if (!file) {
        return false;
    }

    if (!WriteLittleEndian(file, KERNEL_END_CONTROL_SIZE) || !WriteLittleEndian(file, CONTROL_RECORD_VERSION) ||
        !WriteLittleEndian(file, static_cast<std::uint16_t>(PROBE_BINDING_SIZE)) ||
        !WriteLittleEndian(file, CONTROL_RECORD_RESERVED_VALUE) ||
        !WriteLittleEndian(file, CONTROL_RECORD_RESERVED_VALUE) ||
        !WriteLittleEndian(file, KERNEL_END_INSTRUCTION_ID) ||
        !WriteLittleEndian(file, KERNEL_END_PROBE_FUNCTION_INDEX) ||
        !WriteLittleEndian(file, KERNEL_END_PROBE_PARAMETER_COUNT) ||
        !WriteLittleEndian(file, KERNEL_END_FUNCTION_NAME_LIST_SIZE)) {
        return false;
    }

    file.write(KERNEL_END_PROBE_SYMBOL, static_cast<std::streamsize>(KERNEL_END_PROBE_SYMBOL_SIZE));
    file.write(FUNCTION_NAME_LIST_PADDING, sizeof(FUNCTION_NAME_LIST_PADDING));
    file.close();
    return static_cast<bool>(file);
}

bool WriteProbeFiles(const fs::path& directory)
{
    const auto write = [&](const char* name, const void* bytes, std::size_t size) {
        std::ofstream file((directory / name).string(), std::ios::binary);
        file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        file.close();
        return static_cast<bool>(file);
    };
    return write(PROBE_SOURCE_NAME, KERNEL_END_PROBE_SOURCE, sizeof(KERNEL_END_PROBE_SOURCE) - 1) &&
           WriteKernelEndControl(directory);
}

bool Run(std::initializer_list<std::string> arguments, const fs::path& stdoutFile = {})
{
    std::vector<std::string> argumentStorage(arguments);
    std::vector<char*> argv;
    for (auto& argument : argumentStorage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t child = -1;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        return false;
    }
    if (!stdoutFile.empty() &&
        posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    const int error = posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    pid_t waited = -1;
    if (error == 0) {
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    if (error != 0 || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        ASCTOOL_ERROR(
            "[aclpti] kernel-end tool failed: %s spawn=%d status=%d"
            " code=%u",
            argv[0], error, status,
            static_cast<unsigned>(static_cast<uint32_t>(error != 0 ? error : (waited != child ? errno : status))));
        return false;
    }
    return true;
}

bool CompileProbe(const fs::path& directory, const fs::path& compiler)
{
    return Run(
        {compiler.string(), "-x", "cce", "--cce-aicore-only", "--npu-arch=dav-3510", "-O2", "-g", "-c",
         (directory / PROBE_SOURCE_NAME).string(), "-o", (directory / PROBE_OBJECT_NAME).string()});
}

bool FindTool(const fs::path& directory, const char* name, fs::path& tool)
{
    const fs::path candidate = directory / name;
    if (fs::is_regular_file(candidate)) {
        tool = candidate;
        return true;
    }
    return false;
}

bool ResolveTools(KernelEndTools& tools)
{
    const char* cann = std::getenv("ASCEND_HOME_PATH");
    if (cann == nullptr || cann[0] == '\0') {
        ASCTOOL_ERROR(
            "[aclpti] kernel-end requires ASCEND_HOME_PATH"
            " code=%u",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    const fs::path directory = fs::path(cann) / "tools/bisheng_compiler/bin";
    const char* override = std::getenv("NPU_COMPUTE_BISHENG");
    if (override != nullptr && override[0] != '\0') {
        tools.compiler = override;
    }
    if (!FindTool(directory, "ld.lld", tools.linker) || !FindTool(directory, "bisheng-tune", tools.tuner) ||
        (tools.compiler.empty() && !FindTool(directory, "bisheng", tools.compiler))) {
        ASCTOOL_ERROR(
            "[aclpti] kernel-end toolchain is incomplete under %s"
            " code=%u",
            directory.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    return true;
}

struct TemporaryDirectory {
    fs::path path;
    ~TemporaryDirectory()
    {
        boost::system::error_code error;
        fs::remove_all(path, error);
    }
};

bool PrepareProbe(const KernelEndTools& tools, fs::path& assets)
{
    static std::mutex mutex;
    static TemporaryDirectory cache;
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.path.empty()) {
        char pattern[] = "/tmp/npu-compute-kernel-end-probe-XXXXXX";
        if (mkdtemp(pattern) == nullptr) {
            return false;
        }
        TemporaryDirectory temporary{pattern};
        if (!WriteProbeFiles(temporary.path) || !CompileProbe(temporary.path, tools.compiler)) {
            ASCTOOL_ERROR(
                "[aclpti] probe_compile failed for the kernel-end probe"
                " code=%u",
                NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return false;
        }
        // Publish only complete assets. Failed attempts are cleaned up and may be retried.
        // Keep the cache alive for all binary loads in this process; clean it up on exit.
        cache.path.swap(temporary.path);
    }
    assets = cache.path;
    return true;
}

bool PatchKernelParamMetadata(
    const KernelMetadata& sources, std::string& patched, std::uint32_t traceArgumentOffset, std::string& diagnostic)
{
    try {
        Require(
            traceArgumentOffset >= POINTER_SIZE && traceArgumentOffset % POINTER_SIZE == 0 &&
                traceArgumentOffset <= std::numeric_limits<std::uint32_t>::max() - POINTER_SIZE,
            "invalid trace pointer offset");
        const ParsedElf destinations = ReadElfSections(patched);
        Require(!sources.empty(), "no kernel parameter metadata found");

        std::string result = patched;
        for (const auto& entry : sources) {
            const auto destination = destinations.metadataSections.find(entry.first);
            Require(
                destination != destinations.metadataSections.end(), "instrumented kernel metadata section is missing");
            const ElfSection& destinationSection = destinations.sections[destination->second];
            const auto& sourceMetadata = entry.second;
            const auto destinationMetadata = ReadMetadata(patched, destinationSection);
            const auto metadata = NormalizeMetadata(sourceMetadata, destinationMetadata, traceArgumentOffset);
            if (metadata == patched.substr(
                                static_cast<std::size_t>(destinationSection.offset),
                                static_cast<std::size_t>(destinationSection.size))) {
                continue;
            }
            const std::size_t alignment =
                static_cast<std::size_t>(std::max<std::uint64_t>(4, destinationSection.alignment));
            const std::size_t padding = (alignment - result.size() % alignment) % alignment;
            Require(
                padding <= result.max_size() - result.size() &&
                    metadata.size() <= result.max_size() - result.size() - padding,
                "metadata output is too large");
            result.append(padding, '\0');
            const std::size_t offset = result.size();
            result += metadata;
            WriteValue(
                result, destinationSection.header + offsetof(Elf64_Shdr, sh_offset), offset,
                sizeof(Elf64_Shdr::sh_offset));
            WriteValue(
                result, destinationSection.header + offsetof(Elf64_Shdr, sh_size), metadata.size(),
                sizeof(Elf64_Shdr::sh_size));
        }
        patched.swap(result);
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}

} // namespace

bool InstrumentKernelEnd(
    const void* data, std::size_t size, std::vector<char>& output, std::uint32_t* outputTraceArgumentOffset,
    PmSamplingInstructionMap* pmSamplingMap)
{
    output.clear();
    if (pmSamplingMap != nullptr) {
        *pmSamplingMap = {};
    }
    if (outputTraceArgumentOffset != nullptr) {
        *outputTraceArgumentOffset = 0;
    }
    KernelEndTools tools;
    if (!ResolveTools(tools)) {
        return false;
    }
    std::uint32_t traceArgumentOffset = 0;
    KernelMetadata metadata;
    std::string diagnostic;
    if (!ResolveTraceArgumentOffset(data, size, metadata, traceArgumentOffset, diagnostic)) {
        ASCTOOL_ERROR(
            "[aclpti] kernel-end argument metadata is invalid: %s"
            " code=%u",
            diagnostic.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    fs::path assets;
    if (!PrepareProbe(tools, assets)) {
        ASCTOOL_ERROR(
            "[aclpti] failed to generate kernel-end assets"
            " code=%u",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    char pattern[] = "/tmp/npu-compute-kernel-end-XXXXXX";
    if (mkdtemp(pattern) == nullptr) {
        return false;
    }
    TemporaryDirectory temporary{pattern};
    const auto input = temporary.path / "input.o";
    const auto linked = temporary.path / "linked.o";
    const auto patched = temporary.path / "patched.o";
    {
        std::ofstream file(input.string(), std::ios::binary);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        file.close();
        if (!file) {
            return false;
        }
    }
    if (!Run(
            {tools.linker.string(), "-m", "aicorelinux", "-Ttext=0", "-execute-probe",
             (assets / PROBE_OBJECT_NAME).string(), input.string(), "-static", "-q", "-o", linked.string()}) ||
        !Run(
            {tools.tuner.string(), "--action=instru-probe", "--tune-argsize=" + std::to_string(traceArgumentOffset),
             "--instru-memprobe", linked.string(), "--dbi-config=" + (assets / CONTROL_RECORD_NAME).string(),
             "-o=" + patched.string(), "--append-hbmout-paraminfo"})) {
        return false;
    }
    std::ifstream file(patched.string(), std::ios::binary);
    if (!file) {
        return false;
    }
    std::string patchedImage{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) {
        return false;
    }
    if (!PatchKernelParamMetadata(metadata, patchedImage, traceArgumentOffset, diagnostic)) {
        ASCTOOL_ERROR(
            "[aclpti] kernel-end parameter metadata update failed: %s"
            " code=%u",
            diagnostic.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    std::vector<char> result(patchedImage.begin(), patchedImage.end());
    if (pmSamplingMap != nullptr) {
        try {
            const std::vector<char> original(static_cast<const char*>(data), static_cast<const char*>(data) + size);
            if (!BuildPmSamplingFunctionMap(original, result, *pmSamplingMap, diagnostic)) {
                throw std::runtime_error(diagnostic);
            }
            fs::path objdump;
            Require(
                FindTool(tools.tuner.parent_path(), "llvm-objdump", objdump),
                "cannot find llvm-objdump for PmSampling instruction mapping");
            const auto oldDisasm = temporary.path / "original.txt";
            const auto newDisasm = temporary.path / "companion.txt";
            Require(
                Run({objdump.string(), "-d", input.string()}, oldDisasm) &&
                    Run({objdump.string(), "-d", patched.string()}, newDisasm),
                "llvm-objdump failed during PmSampling instruction mapping");
            const auto read = [](const fs::path& path) {
                std::ifstream stream(path.string(), std::ios::binary);
                std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                Require(!stream.bad(), "cannot read disassembly");
                return text;
            };
            if (!BuildPmSamplingInstructionMap(
                    original, result, read(oldDisasm), read(newDisasm), *pmSamplingMap, diagnostic)) {
                ASCTOOL_DEBUG("PmSampling instruction map failed: %s", diagnostic.c_str());
            }
        } catch (const std::exception& error) {
            pmSamplingMap->business.clear();
            pmSamplingMap->excluded.clear();
            pmSamplingMap->error = error.what();
            ASCTOOL_DEBUG("PmSampling disassembly failed: %s", error.what());
        }
    }
    output.swap(result);
    if (outputTraceArgumentOffset != nullptr) {
        *outputTraceArgumentOffset = traceArgumentOffset;
    }
    return true;
}
} // namespace aclpti::profiling
