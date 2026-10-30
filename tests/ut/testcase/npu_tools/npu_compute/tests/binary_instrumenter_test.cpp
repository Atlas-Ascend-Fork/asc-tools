/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "acl_pti/profiling/binary_instrumenter.h"
#include "isolated_gtest.h"
#include "plog_capture.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <iostream>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <string>
#include <stdexcept>
#include <string_view>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::vector<char> MakeElfWithKernelArgumentSizes(
    const std::vector<std::uint32_t>& argumentSizes,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& parameters = {})
{
    constexpr char sectionNames[] = "\0.shstrtab\0__CCE_KernelArgSize\0.ascend.meta.test_kernel\0";
    const std::string names(sectionNames, sizeof(sectionNames));
    constexpr std::size_t kNamesSection = 1;
    constexpr std::size_t kArgumentsSection = 2;
    constexpr std::size_t kMetadataSection = 3;
    constexpr std::size_t kMetadataAlignment = sizeof(std::uint32_t);
    constexpr std::size_t kSummaryWordCount = 3;
    constexpr std::size_t kParameterWordCount = 9;
    constexpr std::uint16_t kSummaryTag = 16;
    constexpr std::uint16_t kParameterTag = 17;
    const std::size_t sectionCount =
        argumentSizes.empty() && parameters.empty() ? kNamesSection + 1 : kMetadataSection + 1;
    const std::size_t sectionHeadersOffset = sizeof(Elf64_Ehdr);
    const std::size_t sectionNamesOffset = sectionHeadersOffset + sectionCount * sizeof(Elf64_Shdr);
    const std::size_t argumentSizesOffset = sectionNamesOffset + names.size();
    const std::size_t argumentSizesBytes = argumentSizes.size() * sizeof(std::uint32_t);
    const std::size_t metadataOffset =
        (argumentSizesOffset + argumentSizesBytes + kMetadataAlignment - 1) & ~(kMetadataAlignment - 1);
    std::string metadata;
    if (!argumentSizes.empty() || !parameters.empty()) {
        auto append = [&](std::uint16_t tag, const std::string& value) {
            const std::uint16_t length = static_cast<std::uint16_t>(value.size());
            metadata.append(reinterpret_cast<const char*>(&tag), sizeof(tag));
            metadata.append(reinterpret_cast<const char*>(&length), sizeof(length));
            metadata += value;
        };
        std::string summary(kSummaryWordCount * sizeof(std::uint32_t), '\0');
        const std::uint32_t parameterCount = static_cast<std::uint32_t>(parameters.size());
        std::uint32_t argumentBytes = 0;
        for (const auto& parameter : parameters) {
            argumentBytes = std::max(argumentBytes, parameter.first + parameter.second);
        }
        std::copy_n(
            reinterpret_cast<const unsigned char*>(&parameterCount), sizeof(parameterCount),
            reinterpret_cast<unsigned char*>(summary.data()));
        std::copy_n(
            reinterpret_cast<const unsigned char*>(&argumentBytes), sizeof(argumentBytes),
            reinterpret_cast<unsigned char*>(summary.data() + sizeof(parameterCount)));
        append(kSummaryTag, summary);

        for (std::uint32_t index = 0; index < parameterCount; ++index) {
            std::string parameter(kParameterWordCount * sizeof(std::uint32_t), '\0');
            const auto& source = parameters[index];
            std::copy_n(
                reinterpret_cast<const unsigned char*>(&index), sizeof(index),
                reinterpret_cast<unsigned char*>(parameter.data() + sizeof(std::uint32_t)));
            std::copy_n(
                reinterpret_cast<const unsigned char*>(&source.first), sizeof(source.first),
                reinterpret_cast<unsigned char*>(parameter.data() + 2 * sizeof(std::uint32_t)));
            std::copy_n(
                reinterpret_cast<const unsigned char*>(&source.second), sizeof(source.second),
                reinterpret_cast<unsigned char*>(parameter.data() + 3 * sizeof(std::uint32_t)));
            append(kParameterTag, parameter);
        }
    }
    const std::size_t metadataEnd = metadataOffset + metadata.size();
    std::vector<char> image(metadataEnd, 0);

    Elf64_Ehdr header{};
    std::copy_n(
        reinterpret_cast<const unsigned char*>(ELFMAG), SELFMAG, reinterpret_cast<unsigned char*>(header.e_ident));
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(Elf64_Ehdr);
    header.e_shoff = sectionHeadersOffset;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = static_cast<Elf64_Half>(sectionCount);
    header.e_shstrndx = kNamesSection;
    std::copy_n(
        reinterpret_cast<const unsigned char*>(&header), sizeof(header),
        reinterpret_cast<unsigned char*>(image.data()));

    Elf64_Shdr namesHeader{};
    namesHeader.sh_name = static_cast<Elf64_Word>(names.find(".shstrtab"));
    namesHeader.sh_type = SHT_STRTAB;
    namesHeader.sh_offset = sectionNamesOffset;
    namesHeader.sh_size = names.size();
    std::copy_n(
        reinterpret_cast<const unsigned char*>(&namesHeader), sizeof(namesHeader),
        reinterpret_cast<unsigned char*>(image.data() + sectionHeadersOffset + kNamesSection * sizeof(Elf64_Shdr)));

    if (!argumentSizes.empty()) {
        Elf64_Shdr argumentSizesHeader{};
        argumentSizesHeader.sh_name = static_cast<Elf64_Word>(names.find("__CCE_KernelArgSize"));
        argumentSizesHeader.sh_type = SHT_NOTE;
        argumentSizesHeader.sh_offset = argumentSizesOffset;
        argumentSizesHeader.sh_size = argumentSizesBytes;
        std::copy_n(
            reinterpret_cast<const unsigned char*>(&argumentSizesHeader), sizeof(argumentSizesHeader),
            reinterpret_cast<unsigned char*>(
                image.data() + sectionHeadersOffset + kArgumentsSection * sizeof(Elf64_Shdr)));
    }
    if (!metadata.empty()) {
        Elf64_Shdr metadataHeader{};
        metadataHeader.sh_name = static_cast<Elf64_Word>(names.find(".ascend.meta.test_kernel"));
        metadataHeader.sh_type = SHT_NOTE;
        metadataHeader.sh_offset = metadataOffset;
        metadataHeader.sh_size = metadata.size();
        metadataHeader.sh_addralign = kMetadataAlignment;
        std::copy_n(
            reinterpret_cast<const unsigned char*>(&metadataHeader), sizeof(metadataHeader),
            reinterpret_cast<unsigned char*>(
                image.data() + sectionHeadersOffset + kMetadataSection * sizeof(Elf64_Shdr)));
    }

    std::copy_n(
        reinterpret_cast<const unsigned char*>(names.data()), names.size(),
        reinterpret_cast<unsigned char*>(image.data() + sectionNamesOffset));
    for (std::size_t index = 0; index < argumentSizes.size(); ++index) {
        std::copy_n(
            reinterpret_cast<const unsigned char*>(&argumentSizes[index]), sizeof(std::uint32_t),
            reinterpret_cast<unsigned char*>(image.data() + argumentSizesOffset + index * sizeof(std::uint32_t)));
    }
    if (!metadata.empty()) {
        std::copy_n(
            reinterpret_cast<const unsigned char*>(metadata.data()), metadata.size(),
            reinterpret_cast<unsigned char*>(image.data() + metadataOffset));
    }
    return image;
}

} // namespace

namespace {

std::vector<char> MakeCodeElf(
    std::uint64_t address, std::uint64_t size, std::uint64_t dataAddress = 0x2000, char dataValue = 42,
    std::uint64_t simtEntryOffset = 0)
{
    constexpr char kSectionNames[] = "\0.shstrtab\0.text\0.strtab\0.symtab\0.data\0";
    constexpr char kSymbolNames[] = "\0kernel\0data\0";
    constexpr std::size_t kNamesSection = 1;
    constexpr std::size_t kCodeSection = 2;
    constexpr std::size_t kStringsSection = 3;
    constexpr std::size_t kSymbolsSection = 4;
    constexpr std::size_t kDataSection = 5;
    constexpr std::size_t kSectionCount = kDataSection + 1;
    const std::string names(kSectionNames, sizeof(kSectionNames));
    std::string symbols(kSymbolNames, sizeof(kSymbolNames) - 1);
    if (simtEntryOffset != 0) {
        if (simtEntryOffset >= size) {
            throw std::invalid_argument("SIMT entry must split the executable function");
        }
        symbols += "simt_entry";
        symbols.push_back('\0');
    }
    const std::size_t symbolCount = simtEntryOffset == 0 ? 3 : 4;
    const std::size_t data = sizeof(Elf64_Ehdr) + kSectionCount * sizeof(Elf64_Shdr);
    const auto strings = data + names.size() + size;
    const auto table = strings + symbols.size();
    const auto dataOffset = table + symbolCount * sizeof(Elf64_Sym);
    std::vector<char> image(dataOffset + 1, 0);
    image[dataOffset] = dataValue;
    Elf64_Ehdr h{};
    std::copy_n(reinterpret_cast<const unsigned char*>(ELFMAG), SELFMAG, reinterpret_cast<unsigned char*>(h.e_ident));
    h.e_ident[EI_CLASS] = ELFCLASS64;
    h.e_ident[EI_DATA] = ELFDATA2LSB;
    h.e_shoff = sizeof(h);
    h.e_shentsize = sizeof(Elf64_Shdr);
    h.e_shnum = kSectionCount;
    h.e_shstrndx = kNamesSection;
    std::copy_n(reinterpret_cast<const unsigned char*>(&h), sizeof(h), reinterpret_cast<unsigned char*>(image.data()));
    const auto section = [&](std::size_t index, const char* name, std::uint32_t type, std::uint64_t offset,
                             std::uint64_t length, std::uint64_t flags = 0) {
        Elf64_Shdr entry{};
        entry.sh_name = names.find(name);
        entry.sh_type = type;
        entry.sh_offset = offset;
        entry.sh_size = length;
        entry.sh_flags = flags;
        if (index == kCodeSection) {
            entry.sh_addr = address;
        }
        if (index == kDataSection) {
            entry.sh_addr = dataAddress;
        }
        if (index == kSymbolsSection) {
            entry.sh_link = kStringsSection;
        }
        std::copy_n(
            reinterpret_cast<const unsigned char*>(&entry), sizeof(entry),
            reinterpret_cast<unsigned char*>(image.data() + sizeof(h) + index * sizeof(entry)));
    };
    section(kNamesSection, ".shstrtab", SHT_STRTAB, data, names.size());
    section(kCodeSection, ".text", SHT_PROGBITS, data + names.size(), size, SHF_ALLOC | SHF_EXECINSTR);
    section(kStringsSection, ".strtab", SHT_STRTAB, strings, symbols.size());
    section(kSymbolsSection, ".symtab", SHT_SYMTAB, table, symbolCount * sizeof(Elf64_Sym));
    section(kDataSection, ".data", SHT_PROGBITS, dataOffset, 1, SHF_ALLOC | SHF_WRITE);
    std::copy_n(
        reinterpret_cast<const unsigned char*>(names.data()), names.size(),
        reinterpret_cast<unsigned char*>(image.data() + data));
    std::copy_n(
        reinterpret_cast<const unsigned char*>(symbols.data()), symbols.size(),
        reinterpret_cast<unsigned char*>(image.data() + strings));
    Elf64_Sym function{};
    function.st_name = static_cast<Elf64_Word>(symbols.find("kernel"));
    function.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    function.st_shndx = kCodeSection;
    function.st_value = address;
    function.st_size = simtEntryOffset == 0 ? size : simtEntryOffset;
    std::copy_n(
        reinterpret_cast<const unsigned char*>(&function), sizeof(function),
        reinterpret_cast<unsigned char*>(image.data() + table + sizeof(function)));
    Elf64_Sym object{};
    object.st_name = symbols.find("data");
    object.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    object.st_shndx = kDataSection;
    object.st_value = dataAddress;
    object.st_size = 256; // AICore pointer symbols can include alignment padding.
    std::memcpy(image.data() + table + 2 * sizeof(object), &object, sizeof(object));
    if (simtEntryOffset != 0) {
        function.st_name = static_cast<Elf64_Word>(symbols.find("simt_entry"));
        function.st_info = ELF64_ST_INFO(STB_LOCAL, STT_FUNC);
        function.st_value = address + simtEntryOffset;
        function.st_size = size - simtEntryOffset;
        std::memcpy(image.data() + table + 3 * sizeof(function), &function, sizeof(function));
    }
    return image;
}

std::vector<std::string> ReadLines(const boost::filesystem::path& path)
{
    std::ifstream input(path.string());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

void CheckToolCallsAndCleanup(const boost::filesystem::path& root, bool toolsUsed, bool simtStart)
{
    const std::size_t linkAttempts = toolsUsed ? (simtStart ? 2U : 7U) : 0U;
    const std::size_t tuneAttempts = toolsUsed ? linkAttempts - 1 : 0U;
    const auto calls = ReadLines(root / "calls");
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "bisheng"), toolsUsed ? 1 : 0);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "ld.lld"), linkAttempts);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "bisheng-tune"), tuneAttempts);
    EXPECT_EQ(calls.size(), toolsUsed ? 1 + linkAttempts + tuneAttempts + (simtStart ? 2 * linkAttempts : 0U) : 0U);
    const auto caches = ReadLines(root / "caches");
    const auto workdirs = ReadLines(root / "workdirs");
    const auto sources = ReadLines(root / "probe_sources");
    const std::set<std::string> uniqueCaches(caches.begin(), caches.end());
    const std::set<std::string> uniqueWorkdirs(workdirs.begin(), workdirs.end());
    const std::set<std::string> uniqueSources(sources.begin(), sources.end());
    EXPECT_EQ(caches.size(), linkAttempts);
    EXPECT_EQ(workdirs.size(), linkAttempts);
    EXPECT_EQ(sources.size(), toolsUsed ? 1U : 0U);
    EXPECT_EQ(uniqueCaches.size(), toolsUsed ? 1U : 0U);
    EXPECT_EQ(uniqueWorkdirs.size(), linkAttempts);
    EXPECT_EQ(uniqueSources, uniqueCaches);
    for (const auto& paths : {caches, workdirs, sources}) {
        for (const auto& path : paths) {
            EXPECT_FALSE(boost::filesystem::exists(path)) << "Temporary asset survived child exit: " << path;
        }
    }
    EXPECT_FALSE(boost::filesystem::exists(root / "kernel_end"));
}

void RunIsolatedScenario(
    bool toolsUsed, const std::function<void()>& assertions, bool simtStart = false, bool simtProbeInserted = true)
{
    if (npucompute::test::IsIsolatedTestChild()) {
        std::string plog;
        npu_compute_test::CaptureScope capture(&plog, DLOG_DEBUG);
        assertions();
        return;
    }
    npucompute::test::TemporaryDirectory temporary;
    const boost::filesystem::path root(temporary.Path());
    const auto tools = root / "tools/bisheng_compiler/bin";
    ASSERT_TRUE(boost::filesystem::create_directories(tools));
    for (const auto* name : {"bisheng", "ld.lld", "bisheng-tune", "llvm-objdump"}) {
        boost::filesystem::create_symlink(NPU_COMPUTE_INSTRUMENTER_TOOL_STUB_PATH, tools / name);
    }
    const auto name = npucompute::test::CurrentTestName();
    npucompute::test::ProcessOptions options;
    options.arguments = {"/proc/self/exe", "--gtest_filter=" + name};
    options.environment["NPU_COMPUTE_ISOLATED_GTEST"] = name;
    options.environment["NPU_COMPUTE_INSTRUMENTER_TEST_ROOT"] = root.string();
    options.environment["ASCEND_HOME_PATH"] = root.string();
    options.environment["PATH"] = tools.string();
    options.environment["NPU_COMPUTE_BISHENG"] = std::nullopt;
    if (!simtProbeInserted) {
        options.environment["NPU_COMPUTE_TEST_SIMT_PROBE_NOT_INSERTED"] = "1";
    }
    const auto result = npucompute::test::RunProcess(options);
    ASSERT_EQ(result.launchError, 0);
    ASSERT_FALSE(result.timedOut);
    ASSERT_EQ(result.signal, 0) << result.standardOutput << result.standardError;
    ASSERT_EQ(result.exitCode, 0) << result.standardOutput << result.standardError;
    if (toolsUsed && simtStart) {
        EXPECT_NE(result.standardError.find("TUNE-INFO: probeFunc:"), std::string::npos);
        EXPECT_NE(result.standardError.find(simtProbeInserted ? "StubTimes: 1" : "StubTimes: 0"), std::string::npos);
    }
    CheckToolCallsAndCleanup(root, toolsUsed, simtStart);
}

} // namespace

TEST(NpuComputeBinaryInstrumenter, RejectsMissingArgumentMetadataBeforeInvokingTools)
{
    RunIsolatedScenario(false, [] {
        const auto input = MakeElfWithKernelArgumentSizes({});
        std::vector<char> output;
        std::uint32_t traceOffset = 123;
        EXPECT_FALSE(aclpti::profiling::InstrumentKernelEnd(input.data(), input.size(), output, &traceOffset));
        EXPECT_EQ(traceOffset, 0U);
        EXPECT_TRUE(output.empty());
    });
}

TEST(NpuComputeBinaryInstrumenter, ReusesProbeAcrossLinkFailureAndConcurrentInstrumentation)
{
    RunIsolatedScenario(true, [] {
        const auto input = MakeElfWithKernelArgumentSizes({40, 31, 16}, {{0, 232}});
        const auto aggregateInput = MakeElfWithKernelArgumentSizes({}, {{24, 24}, {0, 24}});
        std::vector<char> output;
        // The tool stub fails the first link; later calls must reuse the compiled probe.
        ASSERT_FALSE(aclpti::profiling::InstrumentKernelEnd(input.data(), input.size(), output));
        std::atomic<bool> success{true};
        std::vector<std::thread> workers;
        for (int index = 0; index < 4; ++index) {
            workers.emplace_back([&] {
                std::vector<char> patched;
                if (!aclpti::profiling::InstrumentKernelEnd(input.data(), input.size(), patched) ||
                    patched.size() <= input.size()) {
                    success = false;
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        EXPECT_TRUE(success.load()) << "Concurrent instrumentation failed or did not append parameter metadata";
        std::uint32_t traceOffset = 123;
        ASSERT_TRUE(aclpti::profiling::InstrumentKernelEnd(input.data(), input.size(), output, &traceOffset));
        EXPECT_EQ(traceOffset, 232U);
        std::vector<char> aggregateOutput;
        ASSERT_TRUE(aclpti::profiling::InstrumentKernelEnd(
            aggregateInput.data(), aggregateInput.size(), aggregateOutput, &traceOffset));
        EXPECT_EQ(traceOffset, 48U);
    });
}

TEST(NpuComputeBinaryInstrumenter, BuildsIndependentSimtStartProbe)
{
    RunIsolatedScenario(
        true,
        [] {
            const auto input = MakeElfWithKernelArgumentSizes({40, 31, 16}, {{0, 232}});
            std::vector<char> output;
            std::uint32_t probeArgumentOffset = 123;
            ASSERT_FALSE(
                aclpti::profiling::InstrumentSimtSampling(input.data(), input.size(), output, &probeArgumentOffset));
            EXPECT_TRUE(output.empty());
            EXPECT_EQ(probeArgumentOffset, 0U);
            ASSERT_TRUE(
                aclpti::profiling::InstrumentSimtSampling(input.data(), input.size(), output, &probeArgumentOffset));
            EXPECT_GT(output.size(), input.size());
            EXPECT_EQ(probeArgumentOffset, 232U);
        },
        true);
}

TEST(NpuComputeBinaryInstrumenter, RejectsSimtProbeWhenTunerDidNotInsertEntryHook)
{
    RunIsolatedScenario(
        true,
        [] {
            const auto input = MakeElfWithKernelArgumentSizes({40, 31, 16}, {{0, 232}});
            std::vector<char> output;
            std::uint32_t probeArgumentOffset = 123;
            aclpti::profiling::PmSamplingInstructionMap map;
            ASSERT_FALSE(aclpti::profiling::InstrumentSimtSampling(
                input.data(), input.size(), output, &probeArgumentOffset, &map));
            EXPECT_TRUE(output.empty());
            EXPECT_EQ(probeArgumentOffset, 0U);
            map = {};
            probeArgumentOffset = 123;
            ASSERT_FALSE(aclpti::profiling::InstrumentSimtSampling(
                input.data(), input.size(), output, &probeArgumentOffset, &map));
            EXPECT_TRUE(output.empty());
            EXPECT_EQ(probeArgumentOffset, 0U);
            EXPECT_NE(map.error.find("ApiId 398"), std::string::npos);
        },
        true, false);
}

namespace {
struct InstructionMapInput {
    std::vector<char> image;
    std::string disassembly;
};
InstructionMapInput ScalarInstructions(
    std::uint64_t entry, const std::vector<std::string>& operations, std::uint64_t simtEntryOffset = 0)
{
    constexpr std::size_t kInstructionBytes = sizeof(std::uint32_t);
    std::ostringstream text;
    text << "Disassembly of section .text:\n";
    for (std::size_t index = 0; index < operations.size(); ++index) {
        text << std::hex << entry + index * kInstructionBytes << ": 00000000 " << operations[index] << '\n';
    }
    return {MakeCodeElf(entry, operations.size() * kInstructionBytes, 0x2000, 42, simtEntryOffset), text.str()};
}
} // namespace

TEST(NpuComputeBinaryInstrumenter, ClassifiesOriginalSimtInstructionsWithoutCompanionMatching)
{
    for (const auto* vf : {"VF X2, #9", "VF_SIMT X2, X3, #9, #0, #1"}) {
        const auto input = ScalarInstructions(
            0x100,
            {"MOV X0, PC", "MOVI X2, #7", "MOVK X2, #0, #1", "MOVK X2, #0, #2", "SHLI.b64 X2, #2", "ADD.s64 X2, X2, X0",
             vf, "MOV X4, X5", "END"},
            28);
        aclpti::profiling::PmSamplingInstructionMap table;
        std::string error;
        ASSERT_TRUE(
            aclpti::profiling::BuildPmSamplingOriginalInstructionMap(input.image, input.disassembly, table, error))
            << error;
        EXPECT_EQ(table.business.size(), 9U);
        EXPECT_EQ(table.business.at(0x11c), 0x11cU);
        EXPECT_EQ(
            table.simt, std::string_view(vf).find("VF_SIMT") == 0 ? (std::set<std::uint64_t>{0x11c, 0x120}) :
                                                                    std::set<std::uint64_t>{});
    }
}

TEST(NpuComputeBinaryInstrumenter, RejectsIncompleteOrMismatchedOriginalDisassembly)
{
    const auto input = ScalarInstructions(0, {"MOV X0, X1", "END"});
    for (const auto* text :
         {"Disassembly of section .text:\n0: 00000000 MOV X0, X1\n",
          "Disassembly of section .text:\n0: 00000001 MOV X0, X1\n4: 00000000 END\n"}) {
        aclpti::profiling::PmSamplingInstructionMap table;
        std::string error;
        EXPECT_FALSE(aclpti::profiling::BuildPmSamplingOriginalInstructionMap(input.image, text, table, error));
        EXPECT_TRUE(table.business.empty());
        EXPECT_FALSE(error.empty());
    }
}
