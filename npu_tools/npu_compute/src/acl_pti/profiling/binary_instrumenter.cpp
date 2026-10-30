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
#include <cstdio>
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
#include <sstream>
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

constexpr char SIMT_START_PROBE_SOURCE[] = R"PROBE(#include "__clang_cce_defines.h"
#define NPU_COMPUTE_SIMT_PROBE_BLOCK_COUNT 2048
#define NPU_COMPUTE_SIMT_PROBE_NOP_GROUP_COUNT 18
#define NPU_COMPUTE_SIMT_PROBE_NOP_COUNT_PER_GROUP 10
#define NPU_COMPUTE_SIMT_PROBE_STALL_CYCLES 15
#define NPU_COMPUTE_STRINGIFY_VALUE(value) #value
#define NPU_COMPUTE_STRINGIFY(value) NPU_COMPUTE_STRINGIFY_VALUE(value)
inline __attribute__((weak)) __simt_vf__ LAUNCH_BOUND(NPU_COMPUTE_SIMT_PROBE_BLOCK_COUNT)[aicore]
void __npu_compute_simt_start(__gm__ unsigned char*)
{
    #pragma unroll
    for (unsigned int group = 0; group < NPU_COMPUTE_SIMT_PROBE_NOP_GROUP_COUNT; ++group) {
        #pragma unroll
        for (unsigned int i = 0; i < NPU_COMPUTE_SIMT_PROBE_NOP_COUNT_PER_GROUP; ++i) {
            asm volatile("NOP wait:0b0000000 stall:" NPU_COMPUTE_STRINGIFY(NPU_COMPUTE_SIMT_PROBE_STALL_CYCLES));
        }
    }
}
extern __attribute__((noinline)) __attribute__((weak)) __aicore__ void __npu_compute_after_kernel_stack_frame(
    __gm__ unsigned char* memory, unsigned long long pc, unsigned int blockId, unsigned long long pcOffset)
{
    (void)pc;
    (void)blockId;
    cce::async_invoke<__npu_compute_simt_start>(cce::dim3{NPU_COMPUTE_SIMT_PROBE_BLOCK_COUNT, 1, 1}, memory);
    __gm__ unsigned long long* pcOffsetAddr = reinterpret_cast<__gm__ unsigned long long*>(memory);
    *pcOffsetAddr = pcOffset;
    using namespace __cce_scalar;
    dcci((__gm__ unsigned long long*)memory, ENTIRE_DATA_CACHE, CACHELINE_ALL);
}
)PROBE";

// Keep this name in sync with the symbol in KERNEL_END_PROBE_SOURCE.
constexpr char KERNEL_END_PROBE_SYMBOL[] = "__npu_compute_before_kernel_end";
constexpr char SIMT_START_PROBE_SYMBOL[] = "__npu_compute_after_kernel_stack_frame";

// Semantic fields for npu_compute's fixed probe record; its instruction ID is not shared with npu_check.
constexpr std::uint16_t CONTROL_RECORD_VERSION = 0;
constexpr std::uint16_t KERNEL_END_INSTRUCTION_ID = 397; // before kernel end
constexpr std::uint16_t SIMT_START_INSTRUCTION_ID = 398; // before kernel stack frame
constexpr std::uint16_t KERNEL_END_PROBE_FUNCTION_INDEX = 0;
constexpr std::uint16_t KERNEL_END_PROBE_PARAMETER_COUNT = 0;
constexpr std::uint16_t KERNEL_END_PROBE_BINDING_COUNT = 1;
constexpr std::uint16_t SIMT_START_PROBE_FUNCTION_INDEX = 0;
constexpr std::uint16_t SIMT_START_PROBE_PARAMETER_COUNT = 1;
constexpr std::uint16_t SIMT_START_PROBE_PARAMETER_INDEX = 0;
constexpr std::uint16_t CONTROL_RECORD_RESERVED_VALUE = 0;
constexpr std::size_t FUNCTION_NAME_LIST_PADDING_SIZE = sizeof(std::uint32_t);
constexpr char FUNCTION_NAME_LIST_PADDING[FUNCTION_NAME_LIST_PADDING_SIZE] = {};
constexpr std::size_t CONTROL_RECORD_HEADER_SIZE = sizeof(std::uint32_t) + 4 * sizeof(std::uint16_t);
constexpr std::size_t PROBE_BINDING_SIZE = 3 * sizeof(std::uint16_t);

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

struct ElfSectionTable {
    std::uint64_t offset;
    std::uint64_t count;
    std::uint64_t namesOffset;
    std::uint64_t namesSize;
};

ElfSectionTable ReadElfSectionTable(const std::string& data)
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
    return {sectionTable, sectionCount, sectionNamesOffset, sectionNamesSize};
}

ElfSection ReadElfSection(const std::string& data, const ElfSectionTable& table, std::uint64_t index)
{
    const std::uint64_t headerOffset = table.offset + index * sizeof(Elf64_Shdr);
    const std::uint64_t nameOffset =
        ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_name), sizeof(Elf64_Shdr::sh_name));
    const std::uint32_t type = static_cast<std::uint32_t>(
        ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_type), sizeof(Elf64_Shdr::sh_type)));
    ElfSection section{
        static_cast<std::size_t>(headerOffset),
        type,
        ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_offset), sizeof(Elf64_Shdr::sh_offset)),
        ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_size), sizeof(Elf64_Shdr::sh_size)),
        ReadValue(data, headerOffset + offsetof(Elf64_Shdr, sh_addralign), sizeof(Elf64_Shdr::sh_addralign)),
        {},
    };
    Require(
        type == SHT_NOBITS || InRange(data.size(), section.offset, section.size), "ELF section exceeds file bounds");
    Require(nameOffset < table.namesSize, "invalid ELF section name offset");
    const std::uint64_t nameStart = table.namesOffset + nameOffset;
    const std::size_t nameEnd = data.find('\0', static_cast<std::size_t>(nameStart));
    Require(
        nameEnd != std::string::npos && static_cast<std::uint64_t>(nameEnd) < table.namesOffset + table.namesSize,
        "unterminated ELF section name");
    section.name = data.substr(static_cast<std::size_t>(nameStart), nameEnd - static_cast<std::size_t>(nameStart));
    return section;
}

void AddMetadataSection(ParsedElf& elf, const ElfSection& section, const std::string& data)
{
    const auto flags = ReadValue(data, section.header + offsetof(Elf64_Shdr, sh_flags), sizeof(Elf64_Shdr::sh_flags));
    if (section.name.compare(0, sizeof(KERNEL_METADATA_PREFIX) - 1, KERNEL_METADATA_PREFIX) == 0) {
        Require(
            section.type == SHT_NOTE && (flags & SHF_ALLOC) == 0,
            "kernel metadata must be a non-allocated NOTE section");
        Require(
            section.alignment == 0 ||
                ((section.alignment & (section.alignment - 1)) == 0 && section.alignment <= data.size()),
            "invalid metadata alignment");
        Require(
            elf.metadataSections.emplace(section.name, elf.sections.size()).second,
            "duplicate kernel metadata section");
    }
}

ParsedElf ReadElfSections(const std::string& data)
{
    const ElfSectionTable table = ReadElfSectionTable(data);
    ParsedElf elf;
    for (std::uint64_t index = 0; index < table.count; ++index) {
        ElfSection section = ReadElfSection(data, table, index);
        AddMetadataSection(elf, section, data);
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

    const std::string symbols(KERNEL_END_PROBE_SYMBOL);
    const auto bindingCount = KERNEL_END_PROBE_BINDING_COUNT;
    const auto bindingsSize = static_cast<std::uint16_t>(PROBE_BINDING_SIZE * bindingCount);
    const auto nameListSize = static_cast<std::uint32_t>(sizeof(std::uint32_t) + symbols.size());
    const auto controlSize = static_cast<std::uint32_t>(
        CONTROL_RECORD_HEADER_SIZE + bindingsSize + nameListSize + FUNCTION_NAME_LIST_PADDING_SIZE);
    if (!WriteLittleEndian(file, controlSize) || !WriteLittleEndian(file, CONTROL_RECORD_VERSION) ||
        !WriteLittleEndian(file, bindingsSize) || !WriteLittleEndian(file, CONTROL_RECORD_RESERVED_VALUE) ||
        !WriteLittleEndian(file, CONTROL_RECORD_RESERVED_VALUE) ||
        !WriteLittleEndian(file, KERNEL_END_INSTRUCTION_ID) ||
        !WriteLittleEndian(file, KERNEL_END_PROBE_FUNCTION_INDEX) ||
        !WriteLittleEndian(file, KERNEL_END_PROBE_PARAMETER_COUNT)) {
        return false;
    }
    if (!WriteLittleEndian(file, nameListSize)) {
        return false;
    }
    file.write(symbols.data(), static_cast<std::streamsize>(symbols.size()));
    file.write(FUNCTION_NAME_LIST_PADDING, sizeof(FUNCTION_NAME_LIST_PADDING));
    file.close();
    return static_cast<bool>(file);
}

bool WriteSimtStartControl(const fs::path& directory)
{
    std::ofstream file((directory / CONTROL_RECORD_NAME).string(), std::ios::binary);
    if (!file) {
        return false;
    }
    const auto bindingsSize = static_cast<std::uint16_t>(4 * sizeof(std::uint16_t));
    const auto nameListSize = static_cast<std::uint32_t>(sizeof(std::uint32_t) + std::strlen(SIMT_START_PROBE_SYMBOL));
    const auto controlSize = static_cast<std::uint32_t>(
        CONTROL_RECORD_HEADER_SIZE + bindingsSize + nameListSize + FUNCTION_NAME_LIST_PADDING_SIZE);
    if (!WriteLittleEndian(file, controlSize) || !WriteLittleEndian(file, CONTROL_RECORD_VERSION) ||
        !WriteLittleEndian(file, bindingsSize) || !WriteLittleEndian(file, CONTROL_RECORD_RESERVED_VALUE) ||
        !WriteLittleEndian(file, CONTROL_RECORD_RESERVED_VALUE) ||
        !WriteLittleEndian(file, SIMT_START_INSTRUCTION_ID) ||
        !WriteLittleEndian(file, SIMT_START_PROBE_FUNCTION_INDEX) ||
        !WriteLittleEndian(file, SIMT_START_PROBE_PARAMETER_COUNT) ||
        !WriteLittleEndian(file, SIMT_START_PROBE_PARAMETER_INDEX) || !WriteLittleEndian(file, nameListSize)) {
        return false;
    }
    file.write(SIMT_START_PROBE_SYMBOL, static_cast<std::streamsize>(std::strlen(SIMT_START_PROBE_SYMBOL)));
    file.write(FUNCTION_NAME_LIST_PADDING, sizeof(FUNCTION_NAME_LIST_PADDING));
    file.close();
    return static_cast<bool>(file);
}

bool WriteProbeFiles(const fs::path& directory, bool simtStart)
{
    const auto write = [&directory](const char* name, const void* bytes, std::size_t size) {
        std::ofstream file((directory / name).string(), std::ios::binary);
        file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        file.close();
        return static_cast<bool>(file);
    };
    const std::string source(simtStart ? SIMT_START_PROBE_SOURCE : KERNEL_END_PROBE_SOURCE);
    return write(PROBE_SOURCE_NAME, source.data(), source.size()) &&
           (simtStart ? WriteSimtStartControl(directory) : WriteKernelEndControl(directory));
}

bool Run(const std::vector<std::string>& arguments, const fs::path& stdoutFile = {}, bool captureStderr = false)
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
    if (!stdoutFile.empty()) {
        if (posix_spawn_file_actions_addopen(
                &actions, STDOUT_FILENO, stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR) != 0 ||
            (captureStderr && posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) != 0)) {
            posix_spawn_file_actions_destroy(&actions);
            return false;
        }
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
    if (captureStderr && !stdoutFile.empty()) {
        std::ifstream log(stdoutFile.string(), std::ios::binary);
        const std::string output{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        std::fwrite(output.data(), 1, output.size(), stderr);
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

bool ParseUnsignedField(
    const std::string& text, std::size_t start, std::size_t end, const std::string& field, std::uint64_t& value)
{
    const auto fieldPosition = text.find(field, start);
    if (fieldPosition == std::string::npos || fieldPosition >= end) {
        return false;
    }
    auto first = fieldPosition + field.size();
    while (first < end && (text[first] == ' ' || text[first] == '\t')) {
        ++first;
    }
    try {
        std::size_t parsedLength = 0;
        const auto parsedValue = std::stoull(text.substr(first), &parsedLength, 10);
        if (first + parsedLength > end) {
            return false;
        }
        value = parsedValue;
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    } catch (const std::out_of_range&) {
        return false;
    }
}

bool HasSimtStartProbeInsertion(const std::string& tunerOutput)
{
    const std::string probeReport = std::string("probeFunc: ") + SIMT_START_PROBE_SYMBOL + ", ManglingName:";
    const auto probePosition = tunerOutput.find(probeReport);
    if (probePosition == std::string::npos) {
        return false;
    }
    const auto apiPosition = tunerOutput.find("ApiId:", probePosition + probeReport.size());
    const auto nextProbePosition = tunerOutput.find("probeFunc:", probePosition + probeReport.size());
    if (apiPosition == std::string::npos ||
        (nextProbePosition != std::string::npos && apiPosition > nextProbePosition)) {
        return false;
    }
    auto lineEnd = tunerOutput.find_first_of("\r\n", apiPosition);
    if (lineEnd == std::string::npos) {
        lineEnd = tunerOutput.size();
    }
    std::uint64_t apiId = 0;
    std::uint64_t stubTimes = 0;
    return ParseUnsignedField(tunerOutput, apiPosition, lineEnd, "ApiId:", apiId) &&
           ParseUnsignedField(tunerOutput, apiPosition, lineEnd, "StubTimes:", stubTimes) &&
           apiId == SIMT_START_INSTRUCTION_ID && stubTimes > 0;
}

bool CompileProbe(const fs::path& directory, const fs::path& compiler, bool simtStart)
{
    return Run(
        // SIMT sampling requires a vector-only callback. A general NPU target also emits
        // a cube callback without async_invoke, which the tuner can silently bind instead.
        {compiler.string(), "-std=c++17", "-x", "cce", "--cce-aicore-only",
         simtStart ? "--cce-aicore-arch=dav-c310-vec" : "--npu-arch=dav-3510", "-O2", "-g", "-c",
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

bool PrepareProbe(const KernelEndTools& tools, fs::path& assets, bool simtStart)
{
    static std::mutex mutex;
    static TemporaryDirectory caches[2];
    auto& cache = caches[simtStart ? 1 : 0];
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.path.empty()) {
        char pattern[] = "/tmp/npu-compute-probe-XXXXXX";
        if (mkdtemp(pattern) == nullptr) {
            return false;
        }
        TemporaryDirectory temporary{pattern};
        if (!WriteProbeFiles(temporary.path, simtStart) || !CompileProbe(temporary.path, tools.compiler, simtStart)) {
            ASCTOOL_ERROR(
                "[aclpti] probe_compile failed for the %s probe code=%u", simtStart ? "SIMT start" : "kernel-end",
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

static bool WriteSamplingSymbolOrdering(
    const fs::path& toolDirectory, const fs::path& kernel, const fs::path& probe, const fs::path& orderingFile)
{
    fs::path objdump;
    if (!FindTool(toolDirectory, "llvm-objdump", objdump)) {
        ASCTOOL_ERROR("llvm-objdump is unavailable for PmSampling symbol ordering");
        return false;
    }
    std::string ordering;
    for (const auto& object : {std::make_pair(kernel, std::string("g")), std::make_pair(probe, std::string("w"))}) {
        const auto symbolsFile = orderingFile.parent_path() / "symbols.txt";
        if (!Run({objdump.string(), "--syms", object.first.string()}, symbolsFile)) {
            return false;
        }
        std::ifstream symbols(symbolsFile.string());
        std::string line, symbol;
        while (std::getline(symbols, line)) {
            std::istringstream fields(line);
            std::string address, binding, type, section, bytes, name;
            if ((fields >> address >> binding >> type >> section >> bytes >> name) && binding == object.second &&
                type == "F" && section == ".text") {
                symbol = name;
                break;
            }
        }
        if (symbol.empty()) {
            ASCTOOL_ERROR("PmSampling linker ordering symbol is missing: %s", object.first.c_str());
            return false;
        }
        ordering += symbol + "\n";
    }
    std::ofstream file(orderingFile.string());
    file << ordering;
    file.close();
    if (!file) {
        return false;
    }
    return true;
}

static bool LinkProbeImage(
    const KernelEndTools& tools, const fs::path& assets, const fs::path& directory, bool simtStart)
{
    const auto input = directory / "input.o";
    const auto linked = directory / "linked.o";
    std::vector<std::string> linkArguments{tools.linker.string(), "-m", "aicorelinux", "-Ttext=0"};
    if (simtStart) {
        const auto orderingFile = directory / "symbol_ordering.txt";
        if (!WriteSamplingSymbolOrdering(tools.tuner.parent_path(), input, assets / PROBE_OBJECT_NAME, orderingFile)) {
            return false;
        }
        linkArguments.insert(linkArguments.end(), {"--symbol-ordering-file", orderingFile.string()});
    }
    linkArguments.insert(
        linkArguments.end(), {"-execute-probe", (assets / PROBE_OBJECT_NAME).string(), input.string(), "-static", "-q",
                              "-o", linked.string()});
    return Run(linkArguments);
}

static bool TuneProbeImage(
    const KernelEndTools& tools, const fs::path& assets, const fs::path& directory, std::uint32_t traceArgumentOffset,
    bool simtStart, PmSamplingInstructionMap* pmSamplingMap)
{
    const auto linked = directory / "linked.o";
    const auto patched = directory / "patched.o";
    // A successful tuner exit alone does not guarantee an entry hook was inserted.
    // Capture its binding report for verification; the temporary log is cleaned up below.
    const auto tuneLog = directory / "tune.log";
    std::vector<std::string> tuneArguments{
        tools.tuner.string(),
        "--action=instru-probe",
        "--tune-argsize=" + std::to_string(traceArgumentOffset),
        "--instru-memprobe",
        linked.string(),
        "--dbi-config=" + (assets / CONTROL_RECORD_NAME).string(),
        "-o=" + patched.string(),
        "--append-hbmout-paraminfo"};
    if (simtStart) {
        // Keep business SIMT scheduling intact; generic instrumentation rewrites
        // stack initialization and introduces artificial entry stall samples.
        tuneArguments.emplace_back("--kernel-bounded-pcstb-mode");
        tuneArguments.emplace_back("--probe-verb=2");
    }
    if (!Run(tuneArguments, simtStart ? tuneLog : fs::path{}, simtStart)) {
        return false;
    }
    if (simtStart) {
        std::ifstream log(tuneLog.string(), std::ios::binary);
        const std::string tunerOutput{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        if (!log.is_open() || log.bad() || !HasSimtStartProbeInsertion(tunerOutput)) {
            const std::string failure = "bisheng-tune did not report an inserted SIMT stack-frame probe (ApiId 398)";
            if (pmSamplingMap != nullptr) {
                pmSamplingMap->error = failure;
            }
            ASCTOOL_ERROR("[aclpti] %s code=%u", failure.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
            return false;
        }
    }
    return true;
}

static bool ReadPatchedProbeImage(
    const fs::path& patched, const KernelMetadata& metadata, std::uint32_t traceArgumentOffset,
    std::vector<char>& result)
{
    std::string diagnostic;
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
    result.assign(patchedImage.begin(), patchedImage.end());
    return true;
}

static void MapCompanionFunctions(
    const void* data, std::size_t size, const std::vector<char>& result, PmSamplingInstructionMap* pmSamplingMap)
{
    std::string diagnostic;
    if (pmSamplingMap != nullptr) {
        try {
            const std::vector<char> original(static_cast<const char*>(data), static_cast<const char*>(data) + size);
            if (!BuildPmSamplingFunctionMap(original, result, *pmSamplingMap, diagnostic)) {
                throw std::runtime_error(diagnostic);
            }
        } catch (const std::exception& error) {
            pmSamplingMap->business.clear();
            pmSamplingMap->error = error.what();
            ASCTOOL_DEBUG("PmSampling disassembly failed: %s", error.what());
        }
    }
}

static bool WriteProbeInput(const fs::path& input, const void* data, std::size_t size)
{
    std::ofstream file(input.string(), std::ios::binary);
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    file.close();
    if (!file) {
        return false;
    }
    return true;
}

static bool PrepareProbeBuild(
    const void* data, std::size_t size, bool simtStart, KernelEndTools& tools, KernelMetadata& metadata,
    std::uint32_t& traceArgumentOffset, fs::path& assets)
{
    if (!ResolveTools(tools)) {
        return false;
    }
    std::string diagnostic;
    if (!ResolveTraceArgumentOffset(data, size, metadata, traceArgumentOffset, diagnostic)) {
        ASCTOOL_ERROR(
            "[aclpti] kernel-end argument metadata is invalid: %s"
            " code=%u",
            diagnostic.c_str(), NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    if (!PrepareProbe(tools, assets, simtStart)) {
        ASCTOOL_ERROR(
            "[aclpti] failed to generate the %s probe assets code=%u", simtStart ? "SIMT start" : "kernel-end",
            NPU_COMPUTE_LOG_CODE_UNSPECIFIED);
        return false;
    }
    return true;
}

static bool InstrumentProbe(
    const void* data, std::size_t size, std::vector<char>& output, std::uint32_t* outputTraceArgumentOffset,
    PmSamplingInstructionMap* pmSamplingMap, bool simtStart)
{
    output.clear();
    if (pmSamplingMap != nullptr) {
        *pmSamplingMap = {};
    }
    if (outputTraceArgumentOffset != nullptr) {
        *outputTraceArgumentOffset = 0;
    }
    KernelEndTools tools;
    KernelMetadata metadata;
    std::uint32_t traceArgumentOffset = 0;
    fs::path assets;
    if (!PrepareProbeBuild(data, size, simtStart, tools, metadata, traceArgumentOffset, assets)) {
        return false;
    }
    char pattern[] = "/tmp/npu-compute-kernel-end-XXXXXX";
    if (mkdtemp(pattern) == nullptr) {
        return false;
    }
    TemporaryDirectory temporary{pattern};
    const auto input = temporary.path / "input.o";
    const auto patched = temporary.path / "patched.o";
    if (!WriteProbeInput(input, data, size) || !LinkProbeImage(tools, assets, temporary.path, simtStart) ||
        !TuneProbeImage(tools, assets, temporary.path, traceArgumentOffset, simtStart, pmSamplingMap)) {
        return false;
    }
    std::vector<char> result;
    if (!ReadPatchedProbeImage(patched, metadata, traceArgumentOffset, result)) {
        return false;
    }
    MapCompanionFunctions(data, size, result, pmSamplingMap);
    output.swap(result);
    if (outputTraceArgumentOffset != nullptr) {
        *outputTraceArgumentOffset = traceArgumentOffset;
    }
    return true;
}

bool InstrumentKernelEnd(
    const void* data, std::size_t size, std::vector<char>& output, std::uint32_t* outputTraceArgumentOffset,
    PmSamplingInstructionMap* pmSamplingMap)
{
    return InstrumentProbe(data, size, output, outputTraceArgumentOffset, pmSamplingMap, false);
}

bool InstrumentSimtSampling(
    const void* data, std::size_t size, std::vector<char>& output, std::uint32_t* probeArgumentOffset,
    PmSamplingInstructionMap* pmSamplingMap)
{
    return InstrumentProbe(data, size, output, probeArgumentOffset, pmSamplingMap, true);
}

bool BuildOriginalPmSamplingInstructionMap(const void* data, std::size_t size, PmSamplingInstructionMap& output)
{
    output = {};
    if (data == nullptr || size == 0) {
        output.error = "empty device ELF";
        return false;
    }
    const char* cann = std::getenv("ASCEND_HOME_PATH");
    if (cann == nullptr || cann[0] == '\0') {
        output.error = "ASCEND_HOME_PATH is unavailable";
        return false;
    }
    fs::path objdump;
    if (!FindTool(fs::path(cann) / "tools/bisheng_compiler/bin", "llvm-objdump", objdump)) {
        output.error = "cannot find llvm-objdump for PmSampling instruction mapping";
        return false;
    }
    char pattern[] = "/tmp/npu-compute-pm-sampling-map-XXXXXX";
    if (mkdtemp(pattern) == nullptr) {
        output.error = "cannot create PmSampling mapping directory";
        return false;
    }
    TemporaryDirectory temporary{pattern};
    const auto input = temporary.path / "input.o";
    {
        std::ofstream file(input.string(), std::ios::binary);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file) {
            output.error = "cannot write device ELF for PmSampling mapping";
            return false;
        }
    }
    const auto disassembly = temporary.path / "original.txt";
    if (!Run({objdump.string(), "-d", input.string()}, disassembly)) {
        output.error = "llvm-objdump failed during PmSampling instruction mapping";
        return false;
    }
    std::ifstream file(disassembly.string(), std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) {
        output.error = "cannot read PmSampling disassembly";
        return false;
    }
    std::vector<char> image(static_cast<const char*>(data), static_cast<const char*>(data) + size);
    std::string diagnostic;
    if (!BuildPmSamplingOriginalInstructionMap(image, text, output, diagnostic)) {
        output.error = diagnostic;
        return false;
    }
    return true;
}
} // namespace aclpti::profiling
