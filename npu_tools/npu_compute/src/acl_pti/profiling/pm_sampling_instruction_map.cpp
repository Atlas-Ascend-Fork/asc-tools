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
constexpr std::string_view kMovImmediatePrefix = "MOVI ";
constexpr std::string_view kMovKeepPrefix = "MOVK ";
constexpr std::string_view kVectorSimtPrefix = "VF_SIMT ";
constexpr std::string_view kScalarNopEncoding = "41400000";
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
    };
    std::vector<Section> sections;
    std::map<std::string, Function> functions;
    std::set<std::uint64_t> simtAddressRelocations;
    std::uint64_t Read(std::uint64_t offset, unsigned size) const
    {
        Require(
            size <= sizeof(std::uint64_t) && offset <= bytes.size() && size <= bytes.size() - offset,
            "truncated device ELF");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i) {
            value |= std::uint64_t(static_cast<unsigned char>(bytes[offset + i])) << (CHAR_BIT * i);
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
    explicit Elf(const std::vector<char>& image) : bytes(image)
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
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto h = table + i * sizeof(Elf64_Shdr);
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
            sections.push_back(section);
        }
        for (std::uint64_t i = 0; i < count; ++i) {
            sections[i].name = String(
                sections[names],
                Read(table + i * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_name), sizeof(Elf64_Word)));
        }
        for (const auto& section : sections) {
            if (section.type != SHT_SYMTAB) {
                continue;
            }
            Require(section.link < count && section.size % sizeof(Elf64_Sym) == 0, "invalid ELF symbol table");
            for (std::uint64_t i = 0; i < section.size; i += sizeof(Elf64_Sym)) {
                const auto h = section.offset + i;
                if (ELF64_ST_TYPE(Read(h + offsetof(Elf64_Sym, st_info), sizeof(unsigned char))) != STT_FUNC) {
                    continue;
                }
                const auto index = Read(h + offsetof(Elf64_Sym, st_shndx), sizeof(Elf64_Half));
                const auto address = Read(h + offsetof(Elf64_Sym, st_value), sizeof(Elf64_Addr));
                const auto size = Read(h + offsetof(Elf64_Sym, st_size), sizeof(Elf64_Xword));
                if (index >= count || size == 0 || !(sections[index].flags & SHF_EXECINSTR)) {
                    continue;
                }
                const auto& code = sections[index];
                Require(
                    address >= code.address && address - code.address <= code.size &&
                        size <= code.size - (address - code.address),
                    "function exceeds executable section");
                const auto name =
                    String(sections[section.link], Read(h + offsetof(Elf64_Sym, st_name), sizeof(Elf64_Word)));
                Require(
                    functions.emplace(name, Function{address, size, static_cast<std::uint16_t>(index)}).second,
                    "duplicate function symbol");
            }
        }
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
        Require(!functions.empty(), "device ELF has no function symbols");
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
// Scalar branches printed by objdump contain absolute byte addresses. The tune
// pass may replace them with a signed 48-bit word offset loaded into X0.
constexpr unsigned kBranchImmediateBits = 16;
constexpr unsigned kBranchOffsetBits = 3 * kBranchImmediateBits;
constexpr std::uint64_t kBranchImmediateMax = (std::uint64_t{1} << kBranchImmediateBits) - 1;
constexpr std::size_t kBranchSaveDistance = 4;
constexpr std::size_t kBranchLowDistance = 3;
constexpr std::size_t kBranchMiddleDistance = 2;
constexpr std::size_t kBranchHighDistance = 1;
constexpr int kDecimalBase = 10;
using AddressReferences = std::map<std::uint64_t, BranchTarget>;
struct NormalizedInstructions {
    std::vector<Instruction> instructions;
    AddressReferences references;
    std::set<std::uint64_t> helpers;
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
bool ParseImmediate(
    const std::string& operation, const std::string& prefix, const std::string& suffix, std::uint64_t& value)
{
    if (operation.size() < prefix.size() + suffix.size() || operation.compare(0, prefix.size(), prefix) != 0 ||
        operation.compare(operation.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    return ParseUnsigned(operation.substr(prefix.size(), operation.size() - prefix.size() - suffix.size()), value) &&
           value <= kBranchImmediateMax;
}
std::uint64_t AddWordOffset(std::uint64_t address, std::uint64_t encoded)
{
    constexpr auto kSignBit = std::uint64_t{1} << (kBranchOffsetBits - 1);
    constexpr auto kOffsetRange = std::uint64_t{1} << kBranchOffsetBits;
    const bool negative = (encoded & kSignBit) != 0;
    const auto distance = (negative ? kOffsetRange - encoded : encoded) * kScalarInstructionBytes;
    Require(
        negative ? address >= distance : address <= std::numeric_limits<std::uint64_t>::max() - distance,
        "rewritten branch target address overflows");
    return negative ? address - distance : address + distance;
}
bool ReadExpandedBranch(const std::vector<Instruction>& input, std::size_t index, BranchTarget& output)
{
    constexpr std::size_t kMemoryMnemonicBytes = std::string_view("ST").size();
    const auto& operation = input[index].operation;
    if (operation != "JUMPC X0" && operation != "JUMP X0") {
        return false;
    }
    const auto& store = input[index - kBranchSaveDistance].operation;
    const std::regex save(R"(^ST_IO\.b64 X0, \[X30\], #-[0-9]+, #0$)");
    if (!std::regex_match(store, save) || input[index + 1].operation != "LD" + store.substr(kMemoryMnemonicBytes)) {
        return false;
    }
    std::uint64_t low = 0, middle = 0, high = 0;
    if (!ParseImmediate(input[index - kBranchLowDistance].operation, "MOVI X0, #", "", low) ||
        !ParseImmediate(input[index - kBranchMiddleDistance].operation, "MOVK X0, #", ", #1", middle) ||
        !ParseImmediate(input[index - kBranchHighDistance].operation, "MOVK X0, #", ", #2", high)) {
        return false;
    }
    const auto encoded = low | (middle << kBranchImmediateBits) | (high << (2 * kBranchImmediateBits));
    output = {operation == "JUMPC X0" ? "JUMPCI" : "JUMPI", AddWordOffset(input[index].address, encoded)};
    return true;
}
void FindExpandedBranches(
    const std::vector<Instruction>& input, const std::vector<Instruction>& original, NormalizedInstructions& output)
{
    std::set<std::string> restores;
    for (std::size_t index = kBranchSaveDistance; index + 1 < input.size(); ++index) {
        BranchTarget branch;
        if (!ReadExpandedBranch(input, index, branch)) {
            continue;
        }
        const auto& restore = input[index + 1].operation;
        const auto& save = input[index - kBranchSaveDistance].operation;
        Require(
            std::none_of(
                original.begin(), original.end(),
                [&restore, &save](const Instruction& instruction) {
                    return instruction.operation == restore || instruction.operation == save;
                }),
            "rewritten branch scratch slot overlaps original instructions");
        output.references.emplace(input[index].address, std::move(branch));
        restores.insert(restore);
        for (std::size_t distance = 1; distance <= kBranchSaveDistance; ++distance) {
            output.helpers.insert(input[index - distance].address);
        }
    }
    // Taken and fallthrough paths both restore the scratch register. Some target
    // blocks share one restore, so identify it by the verified, newly allocated slot.
    for (const auto& instruction : input) {
        if (restores.count(instruction.operation) != 0) {
            output.helpers.insert(instruction.address);
        }
    }
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
    const std::regex pcMove(R"(^MOV (X[0-9]+), PC$)");
    const std::regex immediate(R"(^MOVI (X[0-9]+), #([0-9]+)$)");
    std::smatch pc, address;
    if (!std::regex_match(input[index].operation, pc, pcMove) ||
        !std::regex_match(input[index + kMovImmediateIndex].operation, address, immediate)) {
        return;
    }
    const auto reg = address[kRegisterCapture].str();
    const auto vectorPrefix = "VF " + reg + ", #";
    if (input[index + kLowPaddingIndex].operation != "NOP" || input[index + kHighPaddingIndex].operation != "NOP" ||
        input[index + kShiftIndex].operation != "SHLI.b64 " + reg + ", #2" ||
        input[index + kAddIndex].operation != "ADD.s64 " + reg + ", " + reg + ", " + pc[kRegisterCapture].str() ||
        input[index + kVectorIndex].operation.compare(0, vectorPrefix.size(), vectorPrefix) != 0) {
        return;
    }
    std::uint64_t offset = 0;
    Require(
        ParseUnsigned(address[kImmediateCapture].str(), offset) && offset <= kBranchImmediateMax,
        "invalid vector entry offset");
    const auto target = AddWordOffset(input[index].address, offset);
    const auto instructionPc = input[index + kMovImmediateIndex].address;
    output.references.emplace(instructionPc, BranchTarget{"vector entry", target});
}
NormalizedInstructions NormalizeInstructions(
    const std::vector<Instruction>& input, const std::vector<Instruction>* original = nullptr)
{
    NormalizedInstructions output;
    if (original != nullptr) {
        FindExpandedBranches(input, *original, output);
    }
    for (std::size_t index = 0; index < input.size(); ++index) {
        NormalizeVectorEntry(input, index, output);
        const auto& instruction = input[index];
        if (output.helpers.count(instruction.address) != 0) {
            continue;
        }
        Instruction normalized = instruction;
        for (const auto* kind : {"JUMPI", "JUMPCI"}) {
            const auto prefix = std::string(kind) + " #";
            if (instruction.operation.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            std::uint64_t target = 0;
            Require(ParseUnsigned(instruction.operation.substr(prefix.size()), target), "invalid direct branch target");
            output.references.emplace(instruction.address, BranchTarget{kind, target});
        }
        const auto reference = output.references.find(instruction.address);
        if (reference != output.references.end()) {
            // The target is checked against the completed PC map after matching.
            normalized.operation =
                reference->second.kind == "vector entry" ?
                    instruction.operation.substr(0, instruction.operation.find('#')) + "@vector-entry" :
                    reference->second.kind;
        }
        output.instructions.push_back(std::move(normalized));
    }
    return output;
}
void ValidateReferences(
    const AddressReferences& original, const AddressReferences& companion, const std::vector<Instruction>& instructions,
    const std::set<std::uint64_t>& helpers, const PmSamplingInstructionMap& mapping)
{
    std::map<std::uint64_t, std::uint64_t> originalToCompanion;
    for (const auto& pair : mapping.business) {
        Require(originalToCompanion.emplace(pair.second, pair.first).second, "duplicate original business PC");
    }
    for (const auto& reference : original) {
        const auto pc = originalToCompanion.find(reference.first);
        Require(pc != originalToCompanion.end(), "address reference instruction has no PC mapping");
        const auto rewritten = companion.find(pc->second);
        Require(
            rewritten != companion.end() && rewritten->second.kind == reference.second.kind,
            "branch target or vector entry reference changed during rewrite");
        auto target = std::find_if(
            instructions.begin(), instructions.end(),
            [&rewritten](const Instruction& instruction) { return instruction.address == rewritten->second.target; });
        // A rewritten branch can land on a scratch restore or alignment NOP.
        // Only verified helpers may precede its first mapped business instruction.
        while (target != instructions.end() && mapping.business.count(target->address) == 0 &&
               (helpers.count(target->address) != 0 || target->operation == "NOP")) {
            const auto nextAddress = target->address + target->bits.size() / kHexDigitsPerByte;
            const auto section = target->section;
            ++target;
            Require(
                target != instructions.end() && target->address == nextAddress && target->section == section,
                "rewritten branch target falls outside executable code");
        }
        if (target == instructions.end() || !mapping.business.count(target->address) ||
            mapping.business.at(target->address) != reference.second.target) {
            std::ostringstream message;
            message << "rewritten branch target or vector entry does not match the original target: original PC 0x"
                    << std::hex << reference.first << ", target 0x" << reference.second.target << "; companion PC 0x"
                    << pc->second << ", target 0x" << rewritten->second.target;
            throw std::runtime_error(message.str());
        }
    }
}
void ValidateDisassembly(const std::vector<Instruction>& instructions, const Elf& elf)
{
    for (const auto& code : elf.sections) {
        if (!(code.flags & SHF_EXECINSTR) || code.size == 0) {
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
        const auto section = std::find_if(elf.sections.begin(), elf.sections.end(), [&](const Elf::Section& value) {
            return value.name == instruction.section;
        });
        Require(
            section != elf.sections.end() && (section->flags & SHF_EXECINSTR) &&
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
std::string BeforeImmediate(const std::string& operation) { return operation.substr(0, operation.find('#')); }
bool Equal(const Instruction& old, const Instruction& fresh, const Elf& original)
{
    if (old.operation == fresh.operation) {
        return true;
    }
    if (original.simtAddressRelocations.count(old.address) &&
        old.operation.compare(0, kMovImmediatePrefix.size(), kMovImmediatePrefix) == 0 &&
        BeforeImmediate(old.operation) == BeforeImmediate(fresh.operation)) {
        return true;
    }
    // The tune pass changes only the VF code-length immediate; registers and mode must agree.
    if (old.operation.compare(0, kVectorSimtPrefix.size(), kVectorSimtPrefix) == 0 &&
        BeforeImmediate(old.operation) == BeforeImmediate(fresh.operation)) {
        const auto oldComma = old.operation.find(',', old.operation.find('#'));
        const auto newComma = fresh.operation.find(',', fresh.operation.find('#'));
        return oldComma != std::string::npos && newComma != std::string::npos &&
               old.operation.substr(oldComma) == fresh.operation.substr(newComma);
    }
    return false;
}
std::vector<std::size_t> Match(
    const std::vector<Instruction>& old, const std::vector<Instruction>& fresh, const Elf& original, bool reverse)
{
    std::vector<std::size_t> result(old.size());
    std::size_t cursor = 0;
    for (std::size_t step = 0; step < old.size(); ++step) {
        const auto i = reverse ? old.size() - step - 1 : step;
        while (cursor < fresh.size() && !Equal(old[i], fresh[reverse ? fresh.size() - cursor - 1 : cursor], original)) {
            ++cursor;
        }
        if (cursor == fresh.size()) {
            std::ostringstream message;
            message << "unsupported business instruction rewrite or deletion at original PC 0x" << std::hex
                    << old[i].address << " (" << old[i].operation << ")";
            throw std::runtime_error(message.str());
        }
        result[i] = reverse ? fresh.size() - cursor - 1 : cursor;
        ++cursor;
    }
    return result;
}
} // namespace

bool BuildPmSamplingFunctionMap(
    const std::vector<char>& originalImage, const std::vector<char>& companionImage, PmSamplingInstructionMap& output,
    std::string& diagnostic)
{
    output = {};
    try {
        const Elf original(originalImage), companion(companionImage);
        PmSamplingInstructionMap result;
        for (const auto& named : original.functions) {
            const auto found = companion.functions.find(named.first);
            Require(
                found != companion.functions.end() &&
                    companion.sections[found->second.section].name != ".bolt.org.text",
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

bool BuildPmSamplingInstructionMap(
    const std::vector<char>& originalImage, const std::vector<char>& companionImage,
    const std::string& originalDisassembly, const std::string& companionDisassembly, PmSamplingInstructionMap& output,
    std::string& diagnostic)
{
    if (!BuildPmSamplingFunctionMap(originalImage, companionImage, output, diagnostic)) {
        return false;
    }
    std::string functionContext;
    try {
        const Elf original(originalImage), companion(companionImage);
        const auto oldInstructions = ReadInstructions(originalDisassembly);
        const auto newInstructions = ReadInstructions(companionDisassembly);
        ValidateDisassembly(oldInstructions, original);
        ValidateDisassembly(newInstructions, companion);
        PmSamplingInstructionMap result = output;
        std::set<std::uint64_t> all;
        for (const auto& instruction : newInstructions) {
            Require(all.insert(instruction.address).second, "overlapping executable address domains");
            result.excluded.insert(instruction.address);
        }
        std::set<std::uint64_t> coveredOriginal;
        AddressReferences originalReferences, companionReferences;
        std::set<std::uint64_t> helpers;
        for (const auto& named : original.functions) {
            const auto found = companion.functions.find(named.first);
            std::ostringstream location;
            location << "function '" << named.first << "', original entry 0x" << std::hex << named.second.address;
            if (found != companion.functions.end()) {
                location << ", companion entry 0x" << found->second.address;
            }
            functionContext = location.str() + ": ";
            Require(
                found != companion.functions.end() &&
                    companion.sections[found->second.section].name != ".bolt.org.text",
                "companion business function is missing");
            const auto oldRaw = FunctionInstructions(oldInstructions, original, named.second);
            const auto freshRaw = FunctionInstructions(newInstructions, companion, found->second);
            const auto oldNormalized = NormalizeInstructions(oldRaw);
            const auto freshNormalized = NormalizeInstructions(freshRaw, &oldRaw);
            const auto& old = oldNormalized.instructions;
            const auto& fresh = freshNormalized.instructions;
            for (const auto& instruction : old) {
                Require(coveredOriginal.insert(instruction.address).second, "overlapping original functions");
            }
            std::vector<Instruction> surviving;
            std::vector<std::uint64_t> retired;
            for (const auto& instruction : old) {
                if (original.simtAddressRelocations.count(instruction.address) != 0 &&
                    instruction.operation.compare(0, kMovKeepPrefix.size(), kMovKeepPrefix) == 0) {
                    retired.push_back(instruction.address);
                } else {
                    surviving.push_back(instruction);
                }
            }
            const auto earliest = Match(surviving, fresh, original, false);
            Require(earliest == Match(surviving, fresh, original, true), "ambiguous business instruction alignment");
            // Only the known paired MOVK -> NOP relaxation is allowed to remove an original instruction.
            for (const auto address : retired) {
                const auto base = original.simtAddressRelocations.find(address - kScalarInstructionBytes);
                Require(base != original.simtAddressRelocations.end(), "unsupported MOVK relaxation");
                auto parent = address - kScalarInstructionBytes;
                while (parent >= kScalarInstructionBytes &&
                       original.simtAddressRelocations.count(parent - kScalarInstructionBytes)) {
                    parent -= kScalarInstructionBytes;
                }
                const auto source = std::find_if(
                    surviving.begin(), surviving.end(),
                    [parent](const Instruction& instruction) { return instruction.address == parent; });
                Require(source != surviving.end(), "MOVK has no matched MOVI relocation");
                const auto index =
                    earliest[std::distance(surviving.begin(), source)] + (address - parent) / kScalarInstructionBytes;
                Require(index < fresh.size() && fresh[index].operation == "NOP", "MOVK relaxation is not a NOP");
            }
            for (std::size_t i = 0; i < surviving.size(); ++i) {
                const auto pc = fresh[earliest[i]].address;
                Require(result.business.emplace(pc, surviving[i].address).second, "overlapping business functions");
                Require(result.excluded.erase(pc) == 1, "business PC is absent from full disassembly");
            }
            originalReferences.insert(oldNormalized.references.begin(), oldNormalized.references.end());
            companionReferences.insert(freshNormalized.references.begin(), freshNormalized.references.end());
            helpers.insert(freshNormalized.helpers.begin(), freshNormalized.helpers.end());
        }
        functionContext.clear();
        ValidateReferences(originalReferences, companionReferences, newInstructions, helpers, result);
        for (const auto& instruction : oldInstructions) {
            if (coveredOriginal.count(instruction.address)) {
                continue;
            }
            // Objdump can decode scalar NOP padding as a SIMT LDG. Check its actual bytes.
            std::string padding;
            for (std::size_t i = 0; i < instruction.bits.size(); i += kScalarNopEncoding.size()) {
                padding += kScalarNopEncoding;
            }
            Require(instruction.bits == padding, "unattributed executable original instruction");
        }
        Require(
            !result.business.empty() && result.business.size() + result.excluded.size() == all.size(),
            "instruction classification is incomplete");
        output = std::move(result);
        diagnostic.clear();
        return true;
    } catch (const std::exception& error) {
        diagnostic = functionContext + error.what();
        output.error = diagnostic;
        return false;
    }
}
} // namespace aclpti::profiling
