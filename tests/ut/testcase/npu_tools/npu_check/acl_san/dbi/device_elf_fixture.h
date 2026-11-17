/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#pragma once

#include <elf.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace aclsan::test {

inline std::vector<uint8_t> MakeDeviceElf(uint64_t rodataAddress)
{
    constexpr char names[] = "\0.shstrtab\0.strtab\0.symtab\0.text\0.rodata";
    constexpr char strings[] = "\0kernel";
    // One valid dav-3510 scalar instruction for HiBit's real decoder.
    constexpr std::array<uint8_t, 4> text{0x0d, 0x81, 0x00, 0xc4};
    constexpr size_t sectionCount = 6;
    const size_t table = sizeof(Elf64_Ehdr);
    const size_t namesOffset = table + sectionCount * sizeof(Elf64_Shdr);
    const size_t stringsOffset = namesOffset + sizeof(names);
    const size_t symbolsOffset = (stringsOffset + sizeof(strings) + 7U) & ~size_t{7};
    const size_t textOffset = symbolsOffset + 2 * sizeof(Elf64_Sym);
    const size_t rodataOffset = textOffset + text.size();
    std::vector<uint8_t> image(rodataOffset + 17, 0);

    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_REL;
    header.e_machine = 0x1029;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header);
    header.e_shoff = table;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = sectionCount;
    header.e_shstrndx = 1;

    std::array<Elf64_Shdr, sectionCount> sections{};
    sections[1].sh_name = 1;
    sections[1].sh_type = SHT_STRTAB;
    sections[1].sh_offset = namesOffset;
    sections[1].sh_size = sizeof(names);
    sections[1].sh_addralign = 1;
    sections[2].sh_name = 11;
    sections[2].sh_type = SHT_STRTAB;
    sections[2].sh_offset = stringsOffset;
    sections[2].sh_size = sizeof(strings);
    sections[2].sh_addralign = 1;
    sections[3].sh_name = 19;
    sections[3].sh_type = SHT_SYMTAB;
    sections[3].sh_offset = symbolsOffset;
    sections[3].sh_size = 2 * sizeof(Elf64_Sym);
    sections[3].sh_link = 2;
    sections[3].sh_info = 1;
    sections[3].sh_addralign = 8;
    sections[3].sh_entsize = sizeof(Elf64_Sym);
    sections[4].sh_name = 27;
    sections[4].sh_type = SHT_PROGBITS;
    sections[4].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    sections[4].sh_offset = textOffset;
    sections[4].sh_size = text.size();
    sections[4].sh_addralign = 4;
    sections[5].sh_name = 33;
    sections[5].sh_type = SHT_PROGBITS;
    sections[5].sh_flags = SHF_ALLOC;
    sections[5].sh_addr = rodataAddress;
    sections[5].sh_offset = rodataOffset;
    sections[5].sh_size = 16;
    sections[5].sh_addralign = 1;

    Elf64_Sym kernel{};
    kernel.st_name = 1;
    kernel.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    kernel.st_shndx = 4;
    kernel.st_size = text.size();
    std::memcpy(image.data(), &header, sizeof(header));
    std::memcpy(image.data() + table, sections.data(), sizeof(sections));
    std::memcpy(image.data() + namesOffset, names, sizeof(names));
    std::memcpy(image.data() + stringsOffset, strings, sizeof(strings));
    std::memcpy(image.data() + symbolsOffset + sizeof(Elf64_Sym), &kernel, sizeof(kernel));
    std::memcpy(image.data() + textOffset, text.data(), text.size());
    return image;
}

} // namespace aclsan::test
