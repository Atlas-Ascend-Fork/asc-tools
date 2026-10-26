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

std::vector<char> MakeCodeElf(std::uint64_t address, std::uint64_t size)
{
    constexpr char kSectionNames[] = "\0.shstrtab\0.text\0.strtab\0.symtab\0";
    constexpr char kSymbolNames[] = "\0kernel\0";
    constexpr std::size_t kNamesSection = 1;
    constexpr std::size_t kCodeSection = 2;
    constexpr std::size_t kStringsSection = 3;
    constexpr std::size_t kSymbolsSection = 4;
    constexpr std::size_t kSectionCount = kSymbolsSection + 1;
    constexpr std::size_t kSymbolCount = 2; // Null symbol followed by the kernel symbol.
    const std::string names(kSectionNames, sizeof(kSectionNames));
    const std::string symbols(kSymbolNames, sizeof(kSymbolNames) - 1);
    const std::size_t data = sizeof(Elf64_Ehdr) + kSectionCount * sizeof(Elf64_Shdr);
    const auto strings = data + names.size() + size;
    const auto table = strings + symbols.size();
    std::vector<char> image(table + kSymbolCount * sizeof(Elf64_Sym), 0);
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
    section(kSymbolsSection, ".symtab", SHT_SYMTAB, table, kSymbolCount * sizeof(Elf64_Sym));
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
    function.st_size = size;
    std::copy_n(
        reinterpret_cast<const unsigned char*>(&function), sizeof(function),
        reinterpret_cast<unsigned char*>(image.data() + table + sizeof(function)));
    return image;
}

bool TestInstructionMap()
{
    using aclpti::profiling::BuildPmSamplingInstructionMap;
    using aclpti::profiling::PmSamplingInstructionMap;
    const auto original = MakeCodeElf(0, 8);
    const auto companion = MakeCodeElf(0x100, 16);
    const std::string before = "Disassembly of section .text:\n0: 00000000 MOV X0, X1\n4: 00000000 END\n";
    const std::string after = "Disassembly of section .text:\n100: 00000000 MOV X0, X1\n"
                              "104: 00000000 CALL probe\n108: 00000000 NOP\n10c: 00000000 END\n";
    PmSamplingInstructionMap map;
    std::string error;
    if (!BuildPmSamplingInstructionMap(original, companion, before, after, map, error) ||
        map.business != std::map<std::uint64_t, std::uint64_t>{{0x100, 0}, {0x10c, 4}} ||
        map.excluded != std::set<std::uint64_t>{0x104, 0x108}) {
        std::cerr << error << '\n';
        return false;
    }
    auto reject = [&](std::vector<char> elf, std::string text) {
        const bool validElf = elf == companion;
        return !BuildPmSamplingInstructionMap(original, elf, before, text, map, error) && map.business.empty() &&
               !error.empty() && map.error == error && (!validElf || map.relocatedEntries.at(0) == 0x100);
    };
    auto ambiguous = after;
    ambiguous.replace(ambiguous.find("CALL probe"), std::string_view("CALL probe").size(), "MOV X0, X1");
    auto deleted = after;
    deleted.replace(deleted.find("MOV X0, X1"), std::string_view("MOV X0, X1").size(), "SUB X0, X1");
    auto wrongBytes = after;
    wrongBytes.replace(wrongBytes.find("00000000"), std::string_view("00000000").size(), "00000001");
    auto gap = after;
    gap.replace(gap.find("104:"), std::string_view("104:").size(), "106:");
    return reject(companion, ambiguous) && reject(companion, deleted) && reject(companion, wrongBytes) &&
           reject(companion, gap) && reject(std::vector<char>(10), after);
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

void CheckToolCallsAndCleanup(const boost::filesystem::path& root, bool toolsUsed)
{
    const auto calls = ReadLines(root / "calls");
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "bisheng"), toolsUsed ? 1 : 0);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "ld.lld"), toolsUsed ? 7 : 0);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "bisheng-tune"), toolsUsed ? 6 : 0);
    EXPECT_EQ(calls.size(), toolsUsed ? 14U : 0U);
    const auto caches = ReadLines(root / "caches");
    const auto workdirs = ReadLines(root / "workdirs");
    const auto sources = ReadLines(root / "probe_sources");
    const std::set<std::string> uniqueCaches(caches.begin(), caches.end());
    const std::set<std::string> uniqueWorkdirs(workdirs.begin(), workdirs.end());
    const std::set<std::string> uniqueSources(sources.begin(), sources.end());
    EXPECT_EQ(caches.size(), toolsUsed ? 7U : 0U);
    EXPECT_EQ(workdirs.size(), toolsUsed ? 7U : 0U);
    EXPECT_EQ(sources.size(), toolsUsed ? 1U : 0U);
    EXPECT_EQ(uniqueCaches.size(), toolsUsed ? 1U : 0U);
    EXPECT_EQ(uniqueWorkdirs.size(), toolsUsed ? 7U : 0U);
    EXPECT_EQ(uniqueSources, uniqueCaches);
    for (const auto& paths : {caches, workdirs, sources}) {
        for (const auto& path : paths) {
            EXPECT_FALSE(boost::filesystem::exists(path)) << "Temporary asset survived child exit: " << path;
        }
    }
    EXPECT_FALSE(boost::filesystem::exists(root / "kernel_end"));
}

void RunIsolatedScenario(bool toolsUsed, const std::function<void()>& assertions)
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
    for (const auto* name : {"bisheng", "ld.lld", "bisheng-tune"}) {
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
    const auto result = npucompute::test::RunProcess(options);
    ASSERT_EQ(result.launchError, 0);
    ASSERT_FALSE(result.timedOut);
    ASSERT_EQ(result.signal, 0) << result.standardOutput << result.standardError;
    ASSERT_EQ(result.exitCode, 0) << result.standardOutput << result.standardError;
    CheckToolCallsAndCleanup(root, toolsUsed);
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

TEST(NpuComputeBinaryInstrumenter, MapsBusinessInstructionsAndExcludesProbes) { EXPECT_TRUE(TestInstructionMap()); }

namespace {
struct InstructionMapInput {
    std::vector<char> image;
    std::string disassembly;
};

InstructionMapInput ScalarInstructions(std::uint64_t entry, const std::vector<std::string>& operations)
{
    constexpr std::size_t kInstructionBytes = sizeof(std::uint32_t);
    std::ostringstream text;
    text << "Disassembly of section .text:\n";
    for (std::size_t index = 0; index < operations.size(); ++index) {
        text << std::hex << entry + index * kInstructionBytes << ": 00000000 " << operations[index] << '\n';
    }
    return {MakeCodeElf(entry, operations.size() * kInstructionBytes), text.str()};
}
} // namespace

TEST(NpuComputeBinaryInstrumenter, MapsExpandedForwardAndBackwardBranches)
{
    const auto original = ScalarInstructions(0, {"MOV X2, X3", "JUMPCI #16", "MOV X4, X5", "JUMPI #0", "END"});
    const auto companion = ScalarInstructions(
        0x100, {"MOV X2, X3", "ST_IO.b64 X0, [X30], #-1960, #0", "MOVI X0, #8", "MOVK X0, #0, #1", "MOVK X0, #0, #2",
                "JUMPC X0", "LD_IO.b64 X0, [X30], #-1960, #0", "MOV X4, X5", "ST_IO.b64 X0, [X30], #-1960, #0",
                "MOVI X0, #65524", "MOVK X0, #65535, #1", "MOVK X0, #65535, #2", "JUMP X0",
                "LD_IO.b64 X0, [X30], #-1960, #0", "NOP", "END"});
    aclpti::profiling::PmSamplingInstructionMap map;
    std::string error;
    ASSERT_TRUE(aclpti::profiling::BuildPmSamplingInstructionMap(
        original.image, companion.image, original.disassembly, companion.disassembly, map, error))
        << error;
    const std::map<std::uint64_t, std::uint64_t> expected{{0x100, 0}, {0x114, 4}, {0x11c, 8}, {0x130, 12}, {0x13c, 16}};
    EXPECT_EQ(map.business, expected);
    EXPECT_EQ(map.excluded.size(), 11U);
    // Malformed expansions and altered targets must not become plausible PC maps.
    for (const auto& replacement : std::vector<std::pair<std::string, std::string>>{
             {"MOVI X0, #8", "MOVI X0, #7"},
             {"MOVK X0, #0, #1", "MOVK X0, #0, #2"},
             {"JUMPC X0", "JUMP X0"},
             {"ST_IO.b64 X0, [X30], #-1960, #0", "ST_IO.b64 X0, [X30], #-1968, #0"}}) {
        auto invalid = companion.disassembly;
        invalid.replace(invalid.find(replacement.first), replacement.first.size(), replacement.second);
        EXPECT_FALSE(aclpti::profiling::BuildPmSamplingInstructionMap(
            original.image, companion.image, original.disassembly, invalid, map, error))
            << invalid;
        EXPECT_TRUE(map.business.empty());
        EXPECT_FALSE(error.empty());
    }
}

TEST(NpuComputeBinaryInstrumenter, ValidatesRelocatedDirectBranchTargets)
{
    const auto original = ScalarInstructions(0, {"MOV X0, X1", "JUMPCI #12", "MOV X2, X3", "END"});
    const auto companion = ScalarInstructions(0x100, {"MOV X0, X1", "JUMPCI #272", "MOV X2, X3", "NOP", "END"});
    aclpti::profiling::PmSamplingInstructionMap map;
    std::string error;
    ASSERT_TRUE(aclpti::profiling::BuildPmSamplingInstructionMap(
        original.image, companion.image, original.disassembly, companion.disassembly, map, error))
        << error;
    auto wrongTarget = companion.disassembly;
    wrongTarget.replace(wrongTarget.find("#272"), std::string("#272").size(), "#264");
    EXPECT_FALSE(aclpti::profiling::BuildPmSamplingInstructionMap(
        original.image, companion.image, original.disassembly, wrongTarget, map, error));
    EXPECT_TRUE(map.business.empty());
    EXPECT_NE(error.find("branch target"), std::string::npos) << error;
}

TEST(NpuComputeBinaryInstrumenter, MapsRelocatedVectorEntryAddress)
{
    const std::vector<std::string> setup{"MOV X0, PC",      "MOVI X2, #7",        "NOP",      "NOP",
                                         "SHLI.b64 X2, #2", "ADD.s64 X2, X2, X0", "VF X2, #9"};
    auto before = setup;
    before.insert(before.end(), {"MOV X4, X5", "END"});
    auto after = setup;
    after[1] = "MOVI X2, #9";
    after.insert(after.end(), {"CALL probe", "NOP", "MOV X4, X5", "END"});
    const auto original = ScalarInstructions(0, before);
    const auto companion = ScalarInstructions(0x100, after);
    aclpti::profiling::PmSamplingInstructionMap map;
    std::string error;
    ASSERT_TRUE(aclpti::profiling::BuildPmSamplingInstructionMap(
        original.image, companion.image, original.disassembly, companion.disassembly, map, error))
        << error;
    EXPECT_EQ(map.business.at(0x104), 4U);
    after[1] = "MOVI X2, #10";
    const auto wrong = ScalarInstructions(0x100, after);
    EXPECT_FALSE(aclpti::profiling::BuildPmSamplingInstructionMap(
        original.image, wrong.image, original.disassembly, wrong.disassembly, map, error));
}
