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
#include <string>
#include <vector>

namespace aclsan::test {

inline std::vector<uint8_t> MakeKernelArgumentSizeElf(uint32_t argumentSize)
{
    constexpr char kSectionNames[] =
        "\0.shstrtab\0__CCE_KernelArgSize\0.ascend.meta.FullFlowKernel\0.strtab\0.symtab\0.text";
    const std::string sectionNames(kSectionNames, sizeof(kSectionNames));
    constexpr char kSymbolNames[] = "\0kernel";
    // One valid dav-3510 instruction for the real HiBit decoder.
    constexpr std::array<uint8_t, 4> text{0x0d, 0x81, 0x00, 0xc4};
    constexpr size_t sectionCount = 7;
    const size_t sectionHeadersOffset = sizeof(Elf64_Ehdr);
    const size_t sectionNamesOffset = sectionHeadersOffset + sectionCount * sizeof(Elf64_Shdr);
    const size_t argumentSizeOffset = sectionNamesOffset + sectionNames.size();
    const size_t metadataOffset = argumentSizeOffset + sizeof(argumentSize);
    const size_t symbolNamesOffset = metadataOffset + 16;
    const size_t symbolsOffset = (symbolNamesOffset + sizeof(kSymbolNames) + 7U) & ~size_t{7};
    const size_t textOffset = symbolsOffset + 2 * sizeof(Elf64_Sym);
    std::vector<uint8_t> image(textOffset + text.size() + 1, 0);

    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_REL;
    header.e_machine = 0x1029;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(Elf64_Ehdr);
    header.e_shoff = sectionHeadersOffset;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = sectionCount;
    header.e_shstrndx = 1;
    std::memcpy(image.data(), &header, sizeof(header));

    Elf64_Shdr sectionNamesHeader{};
    sectionNamesHeader.sh_name = 1;
    sectionNamesHeader.sh_type = SHT_STRTAB;
    sectionNamesHeader.sh_offset = sectionNamesOffset;
    sectionNamesHeader.sh_size = sectionNames.size();
    std::memcpy(
        image.data() + sectionHeadersOffset + sizeof(Elf64_Shdr), &sectionNamesHeader, sizeof(sectionNamesHeader));

    Elf64_Shdr argumentSizeHeader{};
    argumentSizeHeader.sh_name = 11;
    argumentSizeHeader.sh_type = SHT_NOTE;
    argumentSizeHeader.sh_offset = argumentSizeOffset;
    argumentSizeHeader.sh_size = sizeof(argumentSize);
    std::memcpy(
        image.data() + sectionHeadersOffset + 2 * sizeof(Elf64_Shdr), &argumentSizeHeader, sizeof(argumentSizeHeader));
    std::memcpy(image.data() + sectionNamesOffset, sectionNames.data(), sectionNames.size());
    std::memcpy(image.data() + argumentSizeOffset, &argumentSize, sizeof(argumentSize));
    Elf64_Shdr metadataHeader{};
    metadataHeader.sh_name = 31;
    metadataHeader.sh_type = SHT_NOTE;
    metadataHeader.sh_offset = metadataOffset;
    metadataHeader.sh_size = 16;
    std::memcpy(image.data() + sectionHeadersOffset + 3 * sizeof(Elf64_Shdr), &metadataHeader, sizeof(metadataHeader));
    std::array<Elf64_Shdr, 3> deviceSections{};
    deviceSections[0].sh_name = sectionNames.find(".strtab");
    deviceSections[0].sh_type = SHT_STRTAB;
    deviceSections[0].sh_offset = symbolNamesOffset;
    deviceSections[0].sh_size = sizeof(kSymbolNames);
    deviceSections[0].sh_addralign = 1;
    deviceSections[1].sh_name = sectionNames.find(".symtab");
    deviceSections[1].sh_type = SHT_SYMTAB;
    deviceSections[1].sh_offset = symbolsOffset;
    deviceSections[1].sh_size = 2 * sizeof(Elf64_Sym);
    deviceSections[1].sh_link = 4;
    deviceSections[1].sh_info = 1;
    deviceSections[1].sh_addralign = 8;
    deviceSections[1].sh_entsize = sizeof(Elf64_Sym);
    deviceSections[2].sh_name = sectionNames.find(".text");
    deviceSections[2].sh_type = SHT_PROGBITS;
    deviceSections[2].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    deviceSections[2].sh_offset = textOffset;
    deviceSections[2].sh_size = text.size();
    deviceSections[2].sh_addralign = 4;
    std::memcpy(
        image.data() + sectionHeadersOffset + 4 * sizeof(Elf64_Shdr), deviceSections.data(), sizeof(deviceSections));
    Elf64_Sym symbol{};
    symbol.st_name = 1;
    symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    symbol.st_shndx = 6;
    symbol.st_size = text.size();
    std::memcpy(image.data() + symbolNamesOffset, kSymbolNames, sizeof(kSymbolNames));
    std::memcpy(image.data() + symbolsOffset + sizeof(Elf64_Sym), &symbol, sizeof(symbol));
    std::memcpy(image.data() + textOffset, text.data(), text.size());
    // 零参数 ParamSummary；fake DBI 保留该节，生产元数据修正负责追加隐藏指针。
    image[metadataOffset] = 16;
    image[metadataOffset + 2] = 12;
    return image;
}

} // namespace aclsan::test
