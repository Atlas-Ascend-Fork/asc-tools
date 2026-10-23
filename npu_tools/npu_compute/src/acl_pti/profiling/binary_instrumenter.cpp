/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "binary_instrumenter.h"
#include <sys/stat.h>
#include "common/debug_log.h"

#include <algorithm>
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
constexpr char kKernelEndProbeSource[] =
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

// Keep this name in sync with the symbol in kKernelEndProbeSource.
constexpr char kKernelEndProbeSymbol[] = "__npu_compute_before_kernel_end";

// Semantic fields for npu_compute's fixed probe record; its instruction ID is not shared with npu_check.
constexpr std::uint16_t kControlRecordVersion = 0;
constexpr std::uint16_t kKernelEndInstructionId = 397; // before kernel end
constexpr std::uint16_t kKernelEndProbeFunctionIndex = 0;
constexpr std::uint16_t kKernelEndProbeParameterCount = 0;
constexpr std::uint16_t kControlRecordReservedValue = 0;
constexpr std::size_t kKernelEndProbeSymbolSize = sizeof(kKernelEndProbeSymbol) - 1;
constexpr std::size_t kFunctionNameListPaddingSize = sizeof(std::uint32_t);
constexpr char kFunctionNameListPadding[kFunctionNameListPaddingSize] = {};

constexpr std::size_t kControlRecordHeaderSize = sizeof(std::uint32_t) + 4 * sizeof(std::uint16_t);
constexpr std::size_t kProbeBindingSize = 3 * sizeof(std::uint16_t);
constexpr std::uint32_t kKernelEndFunctionNameListSize =
    static_cast<std::uint32_t>(sizeof(std::uint32_t) + kKernelEndProbeSymbolSize);
constexpr std::uint32_t kKernelEndControlSize = static_cast<std::uint32_t>(
    kControlRecordHeaderSize + kProbeBindingSize + kKernelEndFunctionNameListSize + kFunctionNameListPaddingSize);

constexpr const char* kProbeSourceName = "probe.cpp";
constexpr const char* kProbeObjectName = "kernel_end.o";
constexpr const char* kControlRecordName = "ctrl.bin";

struct KernelEndTools {
    fs::path linker;   // ld.lld
    fs::path tuner;    // bisheng-tune
    fs::path compiler; // bisheng
};

constexpr std::uint16_t kParamSummary = 16;
constexpr std::uint16_t kParamInfo = 17;
constexpr std::size_t kSummarySize = 12;
constexpr std::size_t kParamInfoSize = 36;
constexpr std::uint32_t kPointerSize = 8;
constexpr char kKernelMetadataPrefix[] = ".ascend.meta.";

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
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[start + index])) << (index * 8);
    }
    return value;
}

void WriteValue(std::string& data, std::uint64_t offset, std::uint64_t value, std::size_t size)
{
    Require(size <= sizeof(std::uint64_t), "metadata value is too large");
    Require(InRange(data.size(), offset, size), "invalid metadata write range");
    const auto start = static_cast<std::size_t>(offset);
    for (std::size_t index = 0; index < size; ++index) {
        data[start + index] = static_cast<char>((value >> (index * 8)) & 0xffU);
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
            ReadValue(data, 20, 4) == EV_CURRENT && ReadValue(data, 52, 2) == sizeof(Elf64_Ehdr),
        "expected ELF64 little-endian version 1");

    const std::uint64_t sectionTable = ReadValue(data, 40, 8);
    const std::uint64_t sectionCount = ReadValue(data, 60, 2);
    const std::uint64_t sectionNamesIndex = ReadValue(data, 62, 2);
    Require(
        ReadValue(data, 58, 2) == sizeof(Elf64_Shdr) && sectionCount > 0 && sectionNamesIndex > 0 &&
            sectionNamesIndex < sectionCount && sectionTable >= sizeof(Elf64_Ehdr) &&
            InRange(data.size(), sectionTable, sectionCount * sizeof(Elf64_Shdr)),
        "invalid or unsupported ELF section table");

    const std::uint64_t sectionNamesHeader = sectionTable + sectionNamesIndex * sizeof(Elf64_Shdr);
    const std::uint64_t sectionNamesOffset = ReadValue(data, sectionNamesHeader + 24, 8);
    const std::uint64_t sectionNamesSize = ReadValue(data, sectionNamesHeader + 32, 8);
    Require(
        ReadValue(data, sectionNamesHeader + 4, 4) == SHT_STRTAB &&
            InRange(data.size(), sectionNamesOffset, sectionNamesSize),
        "invalid ELF section names");

    ParsedElf elf;
    const std::uint64_t sectionNamesEnd = sectionNamesOffset + sectionNamesSize;
    for (std::uint64_t index = 0; index < sectionCount; ++index) {
        const std::uint64_t headerOffset = sectionTable + index * sizeof(Elf64_Shdr);
        const std::uint64_t nameOffset = ReadValue(data, headerOffset, 4);
        const std::uint32_t type = static_cast<std::uint32_t>(ReadValue(data, headerOffset + 4, 4));
        const auto flags = ReadValue(data, headerOffset + 8, 8);
        ElfSection section{
            static_cast<std::size_t>(headerOffset), type,
            ReadValue(data, headerOffset + 24, 8),  ReadValue(data, headerOffset + 32, 8),
            ReadValue(data, headerOffset + 48, 8),  {},
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
        if (section.name.compare(0, sizeof(kKernelMetadataPrefix) - 1, kKernelMetadataPrefix) == 0) {
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
        const auto tag = static_cast<std::uint16_t>(ReadValue(bytes, offset, 2));
        const std::size_t length = static_cast<std::size_t>(ReadValue(bytes, offset + 2, 2));
        offset += 4;
        Require(InRange(bytes.size(), offset, length), "truncated parameter TLV");
        if (tag == kParamSummary) {
            Require(length == kSummarySize, "unsupported ParamSummary layout");
            ++summaryCount;
            parameterCount = static_cast<std::uint32_t>(ReadValue(bytes, offset, 4));
        } else if (tag == kParamInfo) {
            Require(length == kParamInfoSize, "unsupported ParamInfo layout");
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
        if (item.tag != kParamInfo) {
            continue;
        }
        const std::uint64_t ordinal = ReadValue(item.value, 4, 4);
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
            if (item.tag == kParamSummary) {
                summaryArea = static_cast<std::uint32_t>(ReadValue(item.value, 4, 4));
            }
        }
        for (const auto& parameter : metadata.parameters) {
            const std::uint64_t parameterOffset = ReadValue(parameter, 8, 4);
            const std::uint64_t parameterSize = ReadValue(parameter, 12, 4);
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
    output.resize(header + 4);
    WriteValue(output, header, tag, 2);
    WriteValue(output, header + 2, value.size(), 2);
    output += value;
}

std::string NormalizeMetadata(const Metadata& original, const Metadata& patched, std::uint32_t traceArgumentOffset)
{
    if (!original.hasParameterLayout) {
        // Legacy HostArgs kernels have no ArgsArray ABI. Preserve non-parameter TLVs only.
        std::string output;
        for (const auto& item : patched.values) {
            if (item.tag != kParamSummary && item.tag != kParamInfo) {
                AppendTlv(output, item.tag, item.value);
            }
        }
        return output;
    }
    Require(patched.hasParameterLayout, "instrumented parameter summary is missing");
    Require(original.parameters.size() < std::numeric_limits<std::uint32_t>::max(), "too many kernel parameters");
    for (const auto& item : original.values) {
        if (item.tag == kParamSummary) {
            Require(ReadValue(item.value, 4, 4) <= traceArgumentOffset, "original argument area exceeds trace offset");
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto& parameter : original.parameters) {
        const std::uint64_t offset = ReadValue(parameter, 8, 4);
        const std::uint64_t size = ReadValue(parameter, 12, 4);
        Require(
            size > 0 && offset <= traceArgumentOffset && size <= traceArgumentOffset - offset,
            "original parameter overlaps trace pointer");
        ranges.emplace_back(offset, offset + size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        Require(ranges[index - 1].second <= ranges[index].first, "original parameters overlap");
    }

    std::string pointer(kParamInfoSize, '\0');
    WriteValue(pointer, 4, original.parameters.size(), 4);
    WriteValue(pointer, 8, traceArgumentOffset, 4);
    WriteValue(pointer, 12, kPointerSize, 4);
    WriteValue(pointer, 16, kPointerSize, 2);
    WriteValue(pointer, 18, 2, 2);

    std::string output;
    for (const auto& item : patched.values) {
        if (item.tag == kParamInfo) {
            continue;
        }
        if (item.tag != kParamSummary) {
            AppendTlv(output, item.tag, item.value);
            continue;
        }
        const std::uint64_t count = ReadValue(item.value, 0, 4);
        Require(
            count == original.parameters.size() || count == original.parameters.size() + 1,
            "unexpected instrumented parameter count");
        std::string summary = item.value;
        WriteValue(summary, 0, original.parameters.size() + 1, 4);
        WriteValue(summary, 4, traceArgumentOffset + kPointerSize, 4);
        AppendTlv(output, kParamSummary, summary);
        for (const auto& parameter : original.parameters) {
            AppendTlv(output, kParamInfo, parameter);
        }
        AppendTlv(output, kParamInfo, pointer);
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
        constexpr std::uint32_t kMinimumTraceArgumentOffset = sizeof(std::uint64_t);
        maximumArgumentSize = std::max(maximumArgumentSize, kMinimumTraceArgumentOffset);
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
    std::ofstream file((directory / kControlRecordName).string(), std::ios::binary);
    if (!file) {
        return false;
    }

    if (!WriteLittleEndian(file, kKernelEndControlSize) || !WriteLittleEndian(file, kControlRecordVersion) ||
        !WriteLittleEndian(file, static_cast<std::uint16_t>(kProbeBindingSize)) ||
        !WriteLittleEndian(file, kControlRecordReservedValue) ||
        !WriteLittleEndian(file, kControlRecordReservedValue) || !WriteLittleEndian(file, kKernelEndInstructionId) ||
        !WriteLittleEndian(file, kKernelEndProbeFunctionIndex) ||
        !WriteLittleEndian(file, kKernelEndProbeParameterCount) ||
        !WriteLittleEndian(file, kKernelEndFunctionNameListSize)) {
        return false;
    }

    file.write(kKernelEndProbeSymbol, static_cast<std::streamsize>(kKernelEndProbeSymbolSize));
    file.write(kFunctionNameListPadding, sizeof(kFunctionNameListPadding));
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
    return write(kProbeSourceName, kKernelEndProbeSource, sizeof(kKernelEndProbeSource) - 1) &&
           WriteKernelEndControl(directory);
}

bool Run(std::initializer_list<std::string> arguments, const fs::path& stdoutFile = {})
{
    std::vector<char*> argv;
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
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
        npucompute::detail::DebugLog("aclpti", "kernel-end tool failed: %s spawn=%d status=%d", argv[0], error, status);
        return false;
    }
    return true;
}

bool CompileProbe(const fs::path& directory, const fs::path& compiler)
{
    return Run(
        {compiler.string(), "-x", "cce", "--cce-aicore-only", "--npu-arch=dav-3510", "-O2", "-g", "-c",
         (directory / kProbeSourceName).string(), "-o", (directory / kProbeObjectName).string()});
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
        npucompute::detail::DebugLog("aclpti", "kernel-end requires ASCEND_HOME_PATH");
        return false;
    }
    const fs::path directory = fs::path(cann) / "tools/bisheng_compiler/bin";
    const char* override = std::getenv("NPU_COMPUTE_BISHENG");
    if (override != nullptr && override[0] != '\0') {
        tools.compiler = override;
    }
    if (!FindTool(directory, "ld.lld", tools.linker) || !FindTool(directory, "bisheng-tune", tools.tuner) ||
        (tools.compiler.empty() && !FindTool(directory, "bisheng", tools.compiler))) {
        npucompute::detail::DebugLog("aclpti", "kernel-end toolchain is incomplete under %s", directory.c_str());
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
            npucompute::detail::DebugLog("aclpti", "probe_compile failed for the kernel-end probe");
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
            traceArgumentOffset >= kPointerSize && traceArgumentOffset % kPointerSize == 0 &&
                traceArgumentOffset <= std::numeric_limits<std::uint32_t>::max() - kPointerSize,
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
            WriteValue(result, destinationSection.header + 24, offset, 8);
            WriteValue(result, destinationSection.header + 32, metadata.size(), 8);
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
        npucompute::detail::DebugLog("aclpti", "kernel-end argument metadata is invalid: %s", diagnostic.c_str());
        return false;
    }
    fs::path assets;
    if (!PrepareProbe(tools, assets)) {
        npucompute::detail::DebugLog("aclpti", "failed to generate kernel-end assets");
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
             (assets / kProbeObjectName).string(), input.string(), "-static", "-q", "-o", linked.string()}) ||
        !Run(
            {tools.tuner.string(), "--action=instru-probe", "--tune-argsize=" + std::to_string(traceArgumentOffset),
             "--instru-memprobe", linked.string(), "--dbi-config=" + (assets / kControlRecordName).string(),
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
        npucompute::detail::DebugLog("aclpti", "kernel-end parameter metadata update failed: %s", diagnostic.c_str());
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
                npucompute::detail::DebugLog("aclpti", "PmSampling instruction map failed: %s", diagnostic.c_str());
            }
        } catch (const std::exception& error) {
            pmSamplingMap->business.clear();
            pmSamplingMap->excluded.clear();
            pmSamplingMap->error = error.what();
            npucompute::detail::DebugLog("aclpti", "PmSampling disassembly failed: %s", error.what());
        }
    }
    output.swap(result);
    if (outputTraceArgumentOffset != nullptr) {
        *outputTraceArgumentOffset = traceArgumentOffset;
    }
    return true;
}
} // namespace aclpti::profiling
