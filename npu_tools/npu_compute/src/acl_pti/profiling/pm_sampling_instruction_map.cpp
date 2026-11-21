/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "pm_sampling_instruction_map.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <elf.h>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace aclpti::profiling {
namespace {
// AICore ELF relocation identifiers for the SIMT-entry MOVI/MOVK sequence.
constexpr std::uint32_t kSimtPcRelativeLow = 296;
constexpr std::uint32_t kSimtPcRelativeMiddle = 297;
constexpr std::uint32_t kSimtPcRelativeHigh = 298;
constexpr std::size_t kScalarInstructionBytes = sizeof(std::uint32_t);
constexpr std::size_t kHexDigitsPerByte = 2;
constexpr int kHexBase = 16;
constexpr std::size_t kAddressMatch = 1;
constexpr std::size_t kEncodingMatch = 2;
constexpr std::size_t kOperationMatch = 3;
constexpr std::string_view kVectorSimtPrefix = "VF_SIMT ";
void Require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
struct Elf {
    const std::vector<char>& bytes;
    struct Section {
        std::uint64_t address, offset, size, flags;
        std::uint32_t type, link;
        std::string name;
    };
    struct Function {
        std::uint64_t address, size;
        std::uint16_t section;
        bool local = false;
    };
    struct SectionTable {
        std::uint64_t offset, count, names;
    };
    std::vector<Section> sections;
    std::map<std::string, Function> functions;
    std::map<std::string, Function> objects;
    std::set<std::uint64_t> simtAddressRelocations;
    std::uint64_t Read(std::uint64_t offset, unsigned size) const
    {
        Require(
            size <= sizeof(std::uint64_t) && offset <= bytes.size() && size <= bytes.size() - offset,
            "truncated device ELF");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i) {
            const auto byte = static_cast<unsigned char>(bytes[offset + i]);
            value |= std::uint64_t(byte) << (static_cast<unsigned int>(CHAR_BIT) * i);
        }
        return value;
    }
    std::string String(const Section& section, std::uint64_t offset) const
    {
        Require(offset < section.size, "ELF string offset is invalid");
        std::string result;
        for (; offset < section.size; ++offset) {
            const auto value = Read(section.offset + offset, 1);
            if (value == 0) {
                return result;
            }
            result += static_cast<char>(value);
        }
        throw std::runtime_error("unterminated ELF string");
    }
    SectionTable ReadSectionTable() const
    {
        Require(
            bytes.size() >= SELFMAG && std::equal(ELFMAG, ELFMAG + SELFMAG, bytes.begin()) &&
                Read(EI_CLASS, sizeof(unsigned char)) == ELFCLASS64 &&
                Read(EI_DATA, sizeof(unsigned char)) == ELFDATA2LSB,
            "expected a little-endian ELF64 image");
        const auto table = Read(offsetof(Elf64_Ehdr, e_shoff), sizeof(Elf64_Off));
        const auto count = Read(offsetof(Elf64_Ehdr, e_shnum), sizeof(Elf64_Half));
        const auto names = Read(offsetof(Elf64_Ehdr, e_shstrndx), sizeof(Elf64_Half));
        Require(
            count > 0 && names < count &&
                Read(offsetof(Elf64_Ehdr, e_shentsize), sizeof(Elf64_Half)) == sizeof(Elf64_Shdr),
            "unsupported ELF section table");
        Require(
            table <= bytes.size() && count <= (bytes.size() - table) / sizeof(Elf64_Shdr),
            "ELF section table exceeds file bounds");
        return {table, count, names};
    }
    void ReadSections(const SectionTable& table)
    {
        for (std::uint64_t i = 0; i < table.count; ++i) {
            const auto h = table.offset + i * sizeof(Elf64_Shdr);
            Section section{
                Read(h + offsetof(Elf64_Shdr, sh_addr), sizeof(Elf64_Addr)),
                Read(h + offsetof(Elf64_Shdr, sh_offset), sizeof(Elf64_Off)),
                Read(h + offsetof(Elf64_Shdr, sh_size), sizeof(Elf64_Xword)),
                Read(h + offsetof(Elf64_Shdr, sh_flags), sizeof(Elf64_Xword)),
                static_cast<std::uint32_t>(Read(h + offsetof(Elf64_Shdr, sh_type), sizeof(Elf64_Word))),
                static_cast<std::uint32_t>(Read(h + offsetof(Elf64_Shdr, sh_link), sizeof(Elf64_Word))),
                {}};
            Require(
                section.address <= std::numeric_limits<std::uint64_t>::max() - section.size,
                "ELF address range overflow");
            if (section.type != SHT_NOBITS && section.size != 0) {
                Require(
                    section.offset <= bytes.size() && section.size <= bytes.size() - section.offset,
                    "ELF section exceeds file bounds");
            }
            sections.push_back(std::move(section));
        }
        for (std::uint64_t i = 0; i < table.count; ++i) {
            sections[i].name = String(
                sections[table.names],
                Read(table.offset + i * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_name), sizeof(Elf64_Word)));
        }
    }
    explicit Elf(const std::vector<char>& image) : bytes(image)
    {
        const auto table = ReadSectionTable();
        ReadSections(table);
        ReadSymbols();
        ReadSimtRelocations();
        Require(!functions.empty(), "device ELF has no function symbols");
    }
    using SymbolGroups = std::map<std::string, std::vector<Function>>;
    void ReadObjectSymbol(
        std::uint64_t h, const std::string& name, std::uint64_t address, std::uint64_t size, std::uint64_t index,
        SymbolGroups& symbols)
    {
        const auto& code = sections[index];
        if ((code.flags & SHF_ALLOC) != 0 && (code.flags & SHF_EXECINSTR) == 0) {
            Require(address >= code.address && address - code.address < code.size, "data symbol exceeds its section");
            // AICore may declare an alignment-sized object backed by a single pointer.
            const auto backedSize = std::min(size, code.size - (address - code.address));
            symbols[name].push_back(Function{
                address, backedSize, static_cast<std::uint16_t>(index),
                ELF64_ST_BIND(Read(h + offsetof(Elf64_Sym, st_info), sizeof(unsigned char))) == STB_LOCAL});
        }
    }
    void ReadSymbol(const Section& section, std::uint64_t h, SymbolGroups& functionSymbols, SymbolGroups& objectSymbols)
    {
        const auto symbolType = ELF64_ST_TYPE(Read(h + offsetof(Elf64_Sym, st_info), sizeof(unsigned char)));
        if (symbolType != STT_FUNC && symbolType != STT_OBJECT) {
            return;
        }
        const auto index = Read(h + offsetof(Elf64_Sym, st_shndx), sizeof(Elf64_Half));
        const auto address = Read(h + offsetof(Elf64_Sym, st_value), sizeof(Elf64_Addr));
        const auto size = Read(h + offsetof(Elf64_Sym, st_size), sizeof(Elf64_Xword));
        if (index >= sections.size() || size == 0) {
            return;
        }
        const auto& code = sections[index];
        const auto name = String(sections[section.link], Read(h + offsetof(Elf64_Sym, st_name), sizeof(Elf64_Word)));
        if (symbolType == STT_OBJECT) {
            ReadObjectSymbol(h, name, address, size, index, objectSymbols);
            return;
        }
        if ((code.flags & SHF_EXECINSTR) == 0) {
            return;
        }
        Require(
            address >= code.address && address - code.address <= code.size &&
                size <= code.size - (address - code.address),
            "function exceeds executable section");
        functionSymbols[name].push_back(Function{
            address, size, static_cast<std::uint16_t>(index),
            ELF64_ST_BIND(Read(h + offsetof(Elf64_Sym, st_info), sizeof(unsigned char))) == STB_LOCAL});
    }
    void ReadSymbols()
    {
        SymbolGroups functionSymbols;
        SymbolGroups objectSymbols;
        for (const auto& section : sections) {
            if (section.type != SHT_SYMTAB) {
                continue;
            }
            Require(
                section.link < sections.size() && section.size % sizeof(Elf64_Sym) == 0, "invalid ELF symbol table");
            for (std::uint64_t i = 0; i < section.size; i += sizeof(Elf64_Sym)) {
                ReadSymbol(section, section.offset + i, functionSymbols, objectSymbols);
            }
        }
        CollectFunctionSymbols(functionSymbols);
        CollectObjectSymbols(objectSymbols);
    }
    void CollectFunctionSymbols(SymbolGroups& functionSymbols)
    {
        for (auto& named : functionSymbols) {
            auto& group = named.second;
            std::sort(
                group.begin(), group.end(), [](const Function& a, const Function& b) { return a.address < b.address; });
            for (std::size_t i = 0; i < group.size(); ++i) {
                Require(group.size() == 1 || group[i].local, "duplicate global function symbol");
                Require(i == 0 || group[i - 1].address != group[i].address, "duplicate function address");
                // Linked ACLNN binaries retain same-named local functions from
                // separate translation units. Address order distinguishes these symbols.
                const auto identity =
                    group.size() == 1 ? named.first : named.first + "@local[" + std::to_string(i) + "]";
                Require(functions.emplace(identity, group[i]).second, "ambiguous function identity");
            }
        }
    }
    void CollectObjectSymbols(SymbolGroups& objectSymbols)
    {
        for (auto& named : objectSymbols) {
            auto& group = named.second;
            std::sort(
                group.begin(), group.end(), [](const Function& a, const Function& b) { return a.address < b.address; });
            group.erase(
                std::unique(
                    group.begin(), group.end(),
                    [](const Function& a, const Function& b) {
                        return a.address == b.address && a.size == b.size && a.section == b.section;
                    }),
                group.end());
            for (std::size_t i = 0; i < group.size(); ++i) {
                Require(group.size() == 1 || group[i].local, "duplicate global data symbol");
                const auto identity =
                    group.size() == 1 ? named.first : named.first + "@local[" + std::to_string(i) + "]";
                Require(objects.emplace(identity, group[i]).second, "ambiguous data identity");
            }
        }
    }
    void ReadSimtRelocations()
    {
        for (const auto& section : sections) {
            if (section.type != SHT_RELA) {
                continue;
            }
            Require(section.size % sizeof(Elf64_Rela) == 0, "invalid ELF relocation table");
            for (std::uint64_t i = 0; i < section.size; i += sizeof(Elf64_Rela)) {
                const auto type =
                    ELF64_R_TYPE(Read(section.offset + i + offsetof(Elf64_Rela, r_info), sizeof(Elf64_Xword)));
                // AICore PC-relative MOVI/MOVK relocation triplet used for a SIMT entry address.
                if (type == kSimtPcRelativeLow || type == kSimtPcRelativeMiddle || type == kSimtPcRelativeHigh) {
                    simtAddressRelocations.insert(
                        Read(section.offset + i + offsetof(Elf64_Rela, r_offset), sizeof(Elf64_Addr)));
                }
            }
        }
    }
};
struct Instruction {
    std::uint64_t address;
    std::string operation, section, bits;
};
struct BranchTarget {
    std::string kind;
    std::uint64_t target;
};
std::vector<Instruction> ReadInstructions(const std::string& text)
{
    const std::regex instruction(R"(^\s*([0-9a-fA-F]+):\s+([0-9a-fA-F]{8,32})\s+(.+)$)");
    std::istringstream stream(text);
    std::string line, section;
    std::vector<Instruction> result;
    while (std::getline(stream, line)) {
        const std::string prefix = "Disassembly of section ";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            section = line.substr(prefix.size());
            if (!section.empty() && section.back() == ':') {
                section.pop_back();
            }
        }
        std::smatch match;
        if (!std::regex_match(line, match, instruction)) {
            continue;
        }
        auto operation = match[kOperationMatch].str();
        // Symbol comments on non-branch instructions are objdump decoration, not provenance.
        operation = operation.substr(0, operation.find('<'));
        operation = operation.substr(0, operation.find("wait:"));
        std::istringstream words(operation);
        operation.clear();
        for (std::string word; words >> word;) {
            if (!operation.empty()) {
                operation += ' ';
            }
            operation += word;
        }
        result.push_back(
            {std::stoull(match[kAddressMatch].str(), nullptr, kHexBase), operation, section,
             match[kEncodingMatch].str()});
    }
    Require(!result.empty(), "objdump returned no instructions");
    return result;
}
std::vector<Instruction> FunctionInstructions(
    const std::vector<Instruction>& instructions, const Elf& elf, const Elf::Function& function)
{
    std::vector<Instruction> result;
    for (const auto& instruction : instructions) {
        if (instruction.section == elf.sections[function.section].name && instruction.address >= function.address &&
            instruction.address - function.address < function.size) {
            result.push_back(instruction);
        }
    }
    Require(!result.empty() && result.front().address == function.address, "function disassembly is incomplete");
    auto cursor = function.address;
    for (const auto& instruction : result) {
        Require(
            instruction.address == cursor && instruction.bits.size() % kHexDigitsPerByte == 0,
            "function disassembly has a gap or invalid encoding");
        cursor += instruction.bits.size() / kHexDigitsPerByte;
    }
    Require(cursor == function.address + function.size, "function disassembly does not cover its ELF size");
    return result;
}
constexpr unsigned kBranchOffsetBits = 48;
constexpr unsigned kAddressImmediateBits = 16;
constexpr std::uint64_t kBranchImmediateMax = (std::uint64_t{1} << kAddressImmediateBits) - std::uint64_t{1};
constexpr int kDecimalBase = 10;
using AddressReferences = std::map<std::uint64_t, BranchTarget>;
struct NormalizedInstructions {
    AddressReferences references;
    std::set<std::uint64_t> vectorCalls;
};
bool ParseUnsigned(const std::string& text, std::uint64_t& value)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        value = std::stoull(text, nullptr, kDecimalBase);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
std::uint64_t AddWordOffset(std::uint64_t address, std::uint64_t encoded)
{
    constexpr auto kSignBit = std::uint64_t{1} << (kBranchOffsetBits - 1U);
    constexpr auto kOffsetRange = std::uint64_t{1} << kBranchOffsetBits;
    const bool negative = (encoded & kSignBit) != 0;
    const auto distance = (negative ? kOffsetRange - encoded : encoded) * kScalarInstructionBytes;
    Require(
        negative ? address >= distance : address <= std::numeric_limits<std::uint64_t>::max() - distance,
        "rewritten branch target address overflows");
    return negative ? address - distance : address + distance;
}
void NormalizeVectorEntry(const std::vector<Instruction>& input, std::size_t index, NormalizedInstructions& output)
{
    constexpr std::size_t kRegisterCapture = 1;
    constexpr std::size_t kImmediateCapture = 2;
    constexpr std::size_t kMovImmediateIndex = 1;
    constexpr std::size_t kLowPaddingIndex = 2;
    constexpr std::size_t kHighPaddingIndex = 3;
    constexpr std::size_t kShiftIndex = 4;
    constexpr std::size_t kAddIndex = 5;
    constexpr std::size_t kVectorIndex = 6;
    if (index + kVectorIndex >= input.size() || input[index].operation.find(", PC") == std::string::npos) {
        return;
    }
    const std::regex pcMove(R"(^MOV (X(?:[0-9]|[12][0-9]|3[01])), PC$)");
    const std::regex immediate(R"(^MOVI (X(?:[0-9]|[12][0-9]|3[01])), #([0-9]+)$)");
    std::smatch pc, address;
    if (!std::regex_match(input[index].operation, pc, pcMove) ||
        !std::regex_match(input[index + kMovImmediateIndex].operation, address, immediate)) {
        return;
    }
    const auto reg = address[kRegisterCapture].str();
    const auto& vectorOperation = input[index + kVectorIndex].operation;
    const bool simt = vectorOperation.compare(0, kVectorSimtPrefix.size(), kVectorSimtPrefix) == 0;
    static const std::regex vf(R"(^VF (X(?:[0-9]|[12][0-9]|3[01])), #[0-9]+$)");
    static const std::regex vfSimt(
        R"(^VF_SIMT (X(?:[0-9]|[12][0-9]|3[01])), (X(?:[0-9]|[12][0-9]|3[01])), #[0-9]+, #[0-9]+, #[0-9]+$)");
    std::smatch vector;
    const bool validVector =
        simt ? std::regex_match(vectorOperation, vector, vfSimt) : std::regex_match(vectorOperation, vector, vf);
    const auto& lowPadding = input[index + kLowPaddingIndex].operation;
    const auto& highPadding = input[index + kHighPaddingIndex].operation;
    const bool nopPadding = lowPadding == "NOP" && highPadding == "NOP";
    const bool movkPadding = lowPadding == "MOVK " + reg + ", #0, #1" && highPadding == "MOVK " + reg + ", #0, #2";
    if ((!nopPadding && !movkPadding) || input[index + kShiftIndex].operation != "SHLI.b64 " + reg + ", #2" ||
        input[index + kAddIndex].operation != "ADD.s64 " + reg + ", " + reg + ", " + pc[kRegisterCapture].str() ||
        !validVector || vector[1].str() != reg) {
        return;
    }
    std::uint64_t offset = 0;
    Require(
        ParseUnsigned(address[kImmediateCapture].str(), offset) && offset <= kBranchImmediateMax,
        "invalid vector entry offset");
    const auto target = AddWordOffset(input[index].address, offset);
    const auto instructionPc = input[index + kMovImmediateIndex].address;
    output.references.emplace(instructionPc, BranchTarget{simt ? "SIMT entry" : "vector entry", target});
    output.vectorCalls.insert(input[index + kVectorIndex].address);
}
const std::map<std::string, Elf::Function>::value_type& FunctionAtEntry(const Elf& elf, std::uint64_t address)
{
    const auto found = std::find_if(elf.functions.begin(), elf.functions.end(), [address](const auto& item) {
        return item.second.address == address;
    });
    Require(found != elf.functions.end(), "SIMT entry does not match an ELF function symbol");
    Require(
        std::find_if(
            std::next(found), elf.functions.end(),
            [address](const auto& item) { return item.second.address == address; }) == elf.functions.end(),
        "SIMT entry matches multiple ELF function symbols");
    return *found;
}
void ValidateDisassembly(const std::vector<Instruction>& instructions, const Elf& elf)
{
    for (const auto& code : elf.sections) {
        if ((code.flags & SHF_EXECINSTR) == 0 || code.size == 0) {
            continue;
        }
        auto cursor = code.address;
        for (const auto& instruction : instructions) {
            if (instruction.section == code.name) {
                Require(instruction.address == cursor, "executable disassembly has a gap");
                cursor += instruction.bits.size() / kHexDigitsPerByte;
            }
        }
        Require(cursor == code.address + code.size, "executable disassembly is incomplete");
    }
    for (const auto& instruction : instructions) {
        const auto section = std::find_if(
            elf.sections.begin(), elf.sections.end(),
            [&instruction](const Elf::Section& value) { return value.name == instruction.section; });
        Require(
            section != elf.sections.end() && (section->flags & SHF_EXECINSTR) != 0 &&
                instruction.address >= section->address,
            "objdump address is not executable");
        const auto offset = instruction.address - section->address;
        const auto length = instruction.bits.size() / kHexDigitsPerByte;
        Require(offset <= section->size && length <= section->size - offset, "instruction exceeds section");
        for (std::size_t i = 0; i < length; ++i) {
            const auto printed = std::stoul(
                instruction.bits.substr((length - i - 1) * kHexDigitsPerByte, kHexDigitsPerByte), nullptr, kHexBase);
            Require(elf.Read(section->offset + offset + i, 1) == printed, "objdump encoding differs from ELF");
        }
        Require(
            instruction.operation.find("unknown") == std::string::npos,
            "objdump could not decode an executable instruction");
    }
}
} // namespace

bool BuildPmSamplingFunctionMap(
    const std::vector<char>& original, const std::vector<char>& companion, PmSamplingInstructionMap& output,
    std::string& diagnostic)
{
    output = {};
    try {
        const Elf originalElf(original), companionElf(companion);
        PmSamplingInstructionMap result;
        for (const auto& named : originalElf.functions) {
            const auto found = companionElf.functions.find(named.first);
            Require(
                found != companionElf.functions.end() &&
                    companionElf.sections[found->second.section].name != ".bolt.org.text",
                "companion business function is missing");
            result.functionEntries.emplace(named.first, found->second.address);
            Require(
                result.relocatedEntries.emplace(named.second.address, found->second.address).second,
                "ambiguous companion function entry");
        }
        output = std::move(result);
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        output.error = diagnostic;
        return false;
    }
}

bool BuildPmSamplingOriginalInstructionMap(
    const std::vector<char>& original, const std::string& disassembly, PmSamplingInstructionMap& output,
    std::string& diagnostic)
{
    output = {};
    try {
        const Elf originalElf(original);
        const auto instructions = ReadInstructions(disassembly);
        ValidateDisassembly(instructions, originalElf);
        PmSamplingInstructionMap result;
        NormalizedInstructions entries;
        for (const auto& named : originalElf.functions) {
            result.functionEntries.emplace(named.first, named.second.address);
            result.relocatedEntries.emplace(named.second.address, named.second.address);
            const auto function = FunctionInstructions(instructions, originalElf, named.second);
            for (std::size_t index = 0; index < function.size(); ++index) {
                Require(
                    result.business.emplace(function[index].address, function[index].address).second,
                    "overlapping original functions");
                NormalizeVectorEntry(function, index, entries);
            }
        }
        for (const auto& entry : entries.references) {
            if (entry.second.kind != "SIMT entry") {
                continue;
            }
            const auto& function = FunctionAtEntry(originalElf, entry.second.target).second;
            for (const auto& instruction : FunctionInstructions(instructions, originalElf, function)) {
                result.simt.insert(instruction.address);
            }
        }
        output = std::move(result);
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        output.error = diagnostic;
        return false;
    }
}

namespace {
std::string_view Opcode(std::string_view operation)
{
    if (!operation.empty() && operation.front() == '@') {
        const auto end = operation.find(' ');
        if (end == std::string_view::npos) {
            return {};
        }
        operation.remove_prefix(end + 1);
    }
    return operation.substr(0, operation.find_first_of(". "));
}

std::string SourceLaunchSymbol(const Elf& elf, const std::string& name, std::optional<std::uint64_t> entry)
{
    // ACLNN Runtime entries can be tiling keys. Use the name of the function
    // Runtime selected before interpreting the entry as an ELF address.
    if (!name.empty()) {
        if (elf.functions.count(name) != 0) {
            return name;
        }
        for (const auto* suffix : {"_mix_aic", "_aic"}) {
            const auto candidate = name + suffix;
            if (elf.functions.count(candidate) != 0) {
                return candidate;
            }
        }
    }
    if (entry) {
        return FunctionAtEntry(elf, *entry).first;
    }
    throw std::runtime_error("launch function does not match an original ELF symbol");
}

std::optional<std::uint64_t> SimtCallTarget(const Instruction& instruction)
{
    constexpr std::string_view callPrefix = "CALL #";
    std::string_view operand = instruction.operation;
    if (operand.substr(0, callPrefix.size()) != callPrefix) {
        return std::nullopt;
    }
    operand.remove_prefix(callPrefix.size());
    operand = operand.substr(0, operand.find(' '));
    const bool negative = !operand.empty() && operand.front() == '-';
    if (negative) {
        operand.remove_prefix(1);
    }
    std::uint64_t encodedDistance = 0;
    const auto maximumMagnitude = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1U;
    if (!ParseUnsigned(std::string(operand), encodedDistance)) {
        return std::nullopt;
    }
    if (encodedDistance > (negative ? maximumMagnitude : std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    // llvm-objdump prints SIMT CALL displacements in bytes relative to the CALL PC.
    std::int64_t displacement = 0;
    if (negative) {
        displacement = -static_cast<std::int64_t>(encodedDistance);
    } else if (encodedDistance <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        displacement = static_cast<std::int64_t>(encodedDistance);
    } else {
        displacement = static_cast<std::int64_t>(encodedDistance) -
                       (static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) + 1);
    }
    if (displacement < 0) {
        const auto distance = static_cast<std::uint64_t>(-displacement);
        return distance <= instruction.address ? std::optional{instruction.address - distance} : std::nullopt;
    }
    const auto distance = static_cast<std::uint64_t>(displacement);
    return distance <= std::numeric_limits<std::uint64_t>::max() - instruction.address ?
               std::optional{instruction.address + distance} :
               std::nullopt;
}

void ReadSourceSimtFunctions(
    const Elf& elf, const std::vector<Instruction>& instructions, std::vector<std::uint64_t>& pending,
    SourceCounterInstructions& output)
{
    std::set<std::uint64_t> visited;
    while (!pending.empty()) {
        const auto pc = pending.back();
        pending.pop_back();
        if (!visited.insert(pc).second) {
            continue;
        }
        std::vector<Instruction> body;
        try {
            body = FunctionInstructions(instructions, elf, FunctionAtEntry(elf, pc).second);
        } catch (const std::runtime_error&) {
            output.incomplete = true;
            continue;
        }
        for (const auto& instruction : body) {
            const auto opcode = Opcode(instruction.operation);
            if (opcode == "LDS" || opcode == "STS") {
                output.simtBank.insert(instruction.address);
            } else if (opcode == "LDG" || opcode == "STG") {
                output.simtGm.insert(instruction.address);
            } else if (opcode == "CALL") {
                const auto target = SimtCallTarget(instruction);
                if (target) {
                    pending.push_back(*target);
                } else {
                    output.incomplete = true;
                }
            }
        }
    }
}

void ReadSourceVectorCalls(
    const Elf& elf, const std::vector<Instruction>& instructions, const Elf::Function& caller,
    SourceCounterInstructions& output)
{
    const auto body = FunctionInstructions(instructions, elf, caller);
    NormalizedInstructions calls;
    for (std::size_t index = 0; index < body.size(); ++index) {
        NormalizeVectorEntry(body, index, calls);
    }
    for (const auto& instruction : body) {
        const auto opcode = Opcode(instruction.operation);
        if (opcode == "VF") {
            output.simdVf = true;
        } else if (opcode == "VF_SIMT") {
            output.simtVf = true;
            if (calls.vectorCalls.count(instruction.address) == 0) {
                output.incomplete = true;
            }
        }
    }
    std::vector<std::uint64_t> simtTargets;
    for (const auto& call : calls.references) {
        if (call.second.kind == "SIMT entry") {
            simtTargets.push_back(call.second.target);
        }
    }
    ReadSourceSimtFunctions(elf, instructions, simtTargets, output);
}
} // namespace

bool BuildSourceCounterInstructions(
    const std::vector<char>& image, const std::string& disassembly, const std::string& name,
    std::optional<std::uint64_t> entry, SourceCounterInstructions& output, std::string& diagnostic)
{
    output = {};
    try {
        const Elf elf(image);
        const auto instructions = ReadInstructions(disassembly);
        ValidateDisassembly(instructions, elf);
        output.launchSymbol = SourceLaunchSymbol(elf, name, entry);
        ReadSourceVectorCalls(elf, instructions, elf.functions.at(output.launchSymbol), output);
        for (const auto suffix : {std::string_view{"_mix_aic"}, std::string_view{"_aic"}}) {
            const auto& symbol = output.launchSymbol;
            if (symbol.size() <= suffix.size()) {
                continue;
            }
            if (symbol.compare(symbol.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            const auto partner = elf.functions.find(symbol.substr(0, symbol.size() - 1) + "v");
            if (partner != elf.functions.end()) {
                ReadSourceVectorCalls(elf, instructions, partner->second, output);
            } else if (suffix == "_mix_aic") {
                output.incomplete = true;
            }
            break;
        }
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = error.what();
        return false;
    }
}
} // namespace aclpti::profiling
