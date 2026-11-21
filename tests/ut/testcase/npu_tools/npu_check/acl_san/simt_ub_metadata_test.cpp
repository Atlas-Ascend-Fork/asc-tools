// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include "dbi/kernel_param_metadata.h"
#include "device_runtime/device_binary_registry.h"
#include <gtest/gtest.h>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <iterator>

namespace {
std::string Image(uint32_t staticBytes, uint32_t compilerBytes, uint16_t type = ET_EXEC, bool multiEntry = false)
{
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = type;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header);
    header.e_shoff = sizeof(header);
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = multiEntry ? 6 : 4;
    header.e_shstrndx = 1;
    const char singleNames[] = "\0.shstrtab\0.ubuf\0.ascend.meta.kernel\0";
    const char multiNames[] =
        "\0.shstrtab\0.ubuf\0.ascend.meta.kernel_0_mix_aiv\0.ascend.meta.kernel_17\0.ascend.meta.kernel_0_mix_aic\0";
    const auto* names = multiEntry ? multiNames : singleNames;
    const size_t namesSize = multiEntry ? sizeof(multiNames) : sizeof(singleNames);
    const uint32_t metadata[] = {0x4000c, 3, 0x40007, compilerBytes};
    const uint32_t simdMetadata[] = {0x4000c, 2, 0x40007, 8192};
    const size_t namesOffset = sizeof(header) + header.e_shnum * sizeof(Elf64_Shdr);
    const size_t ubOffset = namesOffset + namesSize;
    const size_t metaOffset = ubOffset + staticBytes;
    Elf64_Shdr sections[6]{};
    sections[1].sh_name = 1;
    sections[1].sh_type = SHT_STRTAB;
    sections[1].sh_offset = namesOffset;
    sections[1].sh_size = namesSize;
    sections[2].sh_name = 11;
    sections[2].sh_type = SHT_PROGBITS;
    sections[2].sh_flags = SHF_ALLOC | SHF_WRITE;
    sections[2].sh_offset = ubOffset;
    sections[2].sh_size = staticBytes;
    sections[3].sh_name = 17;
    sections[3].sh_type = SHT_NOTE;
    sections[3].sh_offset = metaOffset;
    sections[3].sh_size = sizeof(metadata);
    sections[4] = sections[3];
    sections[4].sh_name = 17 + sizeof(".ascend.meta.kernel_0_mix_aiv");
    sections[4].sh_offset = metaOffset + sizeof(metadata);
    sections[5] = sections[4];
    sections[5].sh_name += sizeof(".ascend.meta.kernel_17");
    sections[5].sh_offset += sizeof(simdMetadata);
    std::string image(metaOffset + sizeof(metadata) + (multiEntry ? 2 * sizeof(simdMetadata) : 0), '\0');
    std::memcpy(image.data(), &header, sizeof(header));
    std::memcpy(image.data() + sizeof(header), sections, header.e_shnum * sizeof(Elf64_Shdr));
    std::memcpy(image.data() + namesOffset, names, namesSize);
    std::memcpy(image.data() + metaOffset, metadata, sizeof(metadata));
    if (multiEntry) {
        std::memcpy(image.data() + sections[4].sh_offset, simdMetadata, sizeof(simdMetadata));
        std::memcpy(image.data() + sections[5].sh_offset, simdMetadata, sizeof(simdMetadata));
    }
    return image;
}

TEST(SimtUbMetadataTest, DistinguishesDefaultReservationFromUserStatic)
{
    aclsan::KernelUbMetadataMap kernels;
    std::string error;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(Image(0x3000, 0x5000), kernels, error)) << error;
    ASSERT_EQ(kernels.size(), 1U);
    EXPECT_EQ(kernels.at("kernel").launch.compilerAllocatedBytes, 0x5000U);
    EXPECT_EQ(kernels.at("kernel").launch.userStaticBytes, 0x3000U);
    EXPECT_EQ(kernels.at("kernel").launch.kernelVfType, 3U);
    EXPECT_EQ(kernels.at("kernel").launch.flags, ACLSAN_UB_METADATA_COMPILER | ACLSAN_UB_METADATA_STATIC);
}

TEST(SimtUbMetadataTest, RequiresLinkedTagLargeEnoughForDefaultReservation)
{
    for (const auto& image : {Image(0x3000, 0x2000, ET_REL), Image(0, 0x1fff)}) {
        aclsan::KernelUbMetadataMap kernels;
        std::string error;
        ASSERT_TRUE(aclsan::GetKernelUbMetadata(image, kernels, error)) << error;
        EXPECT_EQ(kernels.at("kernel").launch.flags & ACLSAN_UB_METADATA_STATIC, 0U);
    }
}

TEST(SimtUbMetadataTest, LinkedSectionVirtualAddressIsNotTheUbByteOffset)
{
    auto image = Image(0x3000, 0x5000);
    // The real CCE linked sample has .ubuf sh_addr=0x11000 but UB static base=0.
    const uint64_t address = 0x11000;
    std::memcpy(
        image.data() + sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Shdr) + offsetof(Elf64_Shdr, sh_addr), &address,
        sizeof(address));
    aclsan::KernelUbMetadataMap kernels;
    std::string error;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(image, kernels, error)) << error;
    EXPECT_NE(kernels.at("kernel").launch.flags & ACLSAN_UB_METADATA_STATIC, 0U);
    EXPECT_EQ(kernels.at("kernel").launch.userStaticBytes, 0x3000U);
    EXPECT_NE(kernels.at("kernel").launch.flags & ACLSAN_UB_METADATA_COMPILER, 0U);
}

TEST(SimtUbMetadataTest, IncludesActualCompilerAlignment)
{
    aclsan::KernelUbMetadataMap kernels;
    std::string error;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(Image(33, 8192 + 64), kernels, error)) << error;
    EXPECT_EQ(kernels.at("kernel").launch.userStaticBytes, 64U);
    EXPECT_NE(kernels.at("kernel").launch.flags & ACLSAN_UB_METADATA_STATIC, 0U);
}

TEST(SimtUbMetadataTest, RejectsTruncatedElfWithoutPartialMetadata)
{
    auto image = Image(0x3000, 0x5000);
    image.pop_back();
    aclsan::KernelUbMetadataMap kernels{{"old", {}}};
    std::string error;
    EXPECT_FALSE(aclsan::GetKernelUbMetadata(image, kernels, error));
    EXPECT_TRUE(kernels.empty());
    EXPECT_FALSE(error.empty());
}

TEST(SimtUbMetadataTest, MissingVfTagUsesRuntimeNonSimtDefault)
{
    auto image = Image(0x3000, 0x5000);
    const uint32_t unknownTag = 0x400ff;
    std::memcpy(image.data() + image.size() - 16, &unknownTag, sizeof(unknownTag));
    aclsan::KernelUbMetadataMap kernels;
    std::string error;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(image, kernels, error)) << error;
    ASSERT_EQ(kernels.count("kernel"), 1U);
    EXPECT_EQ(kernels.at("kernel").launch.kernelVfType, 0U);
    EXPECT_EQ(kernels.at("kernel").launch.flags & ACLSAN_UB_METADATA_UNAVAILABLE, 0U);
    EXPECT_EQ(kernels.at("kernel").launch.flags & ACLSAN_UB_METADATA_STATIC, 0U);
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    AclsanUbLaunchMetadata metadata{};
    EXPECT_FALSE(registry.GetFunctionUbMetadata(1, metadata));
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, image.data(), image.size()));
    registry.RecordBinaryFunctionLookup(1, 2, "kernel");
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.kernelVfType, 0U);
    EXPECT_EQ(metadata.flags & ACLSAN_UB_METADATA_UNAVAILABLE, 0U);
}

TEST(SimtUbMetadataTest, RegistryRetainsFinalImageFactsAndClearsOnUnloadAndReuse)
{
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    const auto object = Image(0x3000, 8192, ET_REL);
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, object.data(), object.size()));
    registry.RecordBinaryFunctionLookup(1, 2, "kernel");
    AclsanUbLaunchMetadata metadata{};
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.flags & ACLSAN_UB_METADATA_STATIC, 0U);
    const auto linked = Image(0x3000, 0x5000);
    ASSERT_TRUE(registry.RecordLoadedImage(1, linked.data(), linked.size()));
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
    EXPECT_NE(metadata.flags & ACLSAN_UB_METADATA_STATIC, 0U);
    registry.RecordBinaryFunctionLookup(1, 3, "different_entry");
    EXPECT_FALSE(registry.GetFunctionUbMetadata(3, metadata));
    registry.RecordBinaryUnload(1);
    EXPECT_FALSE(registry.GetFunctionUbMetadata(2, metadata));
    const auto replacement = Image(0, 8192);
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, replacement.data(), replacement.size()));
    registry.RecordBinaryFunctionLookup(1, 2, "kernel");
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0U);
    EXPECT_EQ(metadata.dynamicBytes, 0U);
}

TEST(SimtUbMetadataTest, EntryLookupSelectsAivMetadataInMixedElf)
{
    const auto image = Image(0x3000, 0x5000, ET_EXEC, true);
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, true, 8, image.data(), image.size()));
    AclsanUbLaunchMetadata metadata{};
    registry.RecordBinaryFunctionLookup(1, 2, nullptr, 0);
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
    EXPECT_NE(metadata.flags & ACLSAN_UB_METADATA_STATIC, 0U);
    registry.RecordBinaryFunctionLookup(1, 3, nullptr, 17);
    ASSERT_TRUE(registry.GetFunctionUbMetadata(3, metadata));
    EXPECT_EQ(metadata.kernelVfType, 2U);
    registry.RecordBinaryFunctionLookup(1, 4, "kernel_0");
    ASSERT_TRUE(registry.GetFunctionUbMetadata(4, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
    EXPECT_NE(metadata.flags & ACLSAN_UB_METADATA_STATIC, 0U);
}

void SetFunctionEntry(std::string& image, size_t sectionIndex, uint64_t entry, bool enabled = true)
{
    struct EntryInfo {
        uint16_t tag = 14;
        uint16_t length = 12;
        uint32_t flags = 0;
        uint64_t value = 0;
    } info;
    static_assert(sizeof(info) == 16);
    info.flags = enabled ? 0U : 0x80U;
    info.value = entry;
    const size_t sectionOffset = sizeof(Elf64_Ehdr) + sectionIndex * sizeof(Elf64_Shdr);
    Elf64_Shdr section{};
    std::memcpy(&section, image.data() + sectionOffset, sizeof(section));
    auto payload = image.substr(section.sh_offset, section.sh_size);
    payload.append(reinterpret_cast<const char*>(&info), sizeof(info));
    section.sh_offset = image.size();
    section.sh_size = payload.size();
    image += payload;
    std::memcpy(image.data() + sectionOffset, &section, sizeof(section));
}

TEST(SimtUbMetadataTest, ExplicitEntryOverridesMisleadingNumericSuffix)
{
    auto image = Image(0x3000, 0x5000, ET_EXEC, true);
    SetFunctionEntry(image, 3, 17);
    SetFunctionEntry(image, 4, 0);
    SetFunctionEntry(image, 5, 17);
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, true, 8, image.data(), image.size()));
    registry.RecordBinaryFunctionLookup(1, 2, nullptr, 17);
    AclsanUbLaunchMetadata metadata{};
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY);
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
    registry.RecordBinaryFunctionLookup(1, 3, nullptr, 0);
    ASSERT_TRUE(registry.GetFunctionUbMetadata(3, metadata));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY);
}

TEST(SimtUbMetadataTest, LegacyNameWithoutNumericSuffixUsesDefaultEntryZero)
{
    const auto image = Image(0x3000, 0x5000);
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, true, 8, image.data(), image.size()));
    registry.RecordBinaryFunctionLookup(1, 2, nullptr, 0);
    AclsanUbLaunchMetadata metadata{};
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
}

TEST(SimtUbMetadataTest, DisabledEntryDoesNotFallBackToNumericSuffix)
{
    auto image = Image(0x3000, 0x5000, ET_EXEC, true);
    SetFunctionEntry(image, 3, 0, false);
    SetFunctionEntry(image, 5, 0, false);
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, true, 8, image.data(), image.size()));
    registry.RecordBinaryFunctionLookup(1, 2, nullptr, 0);
    AclsanUbLaunchMetadata metadata{};
    EXPECT_FALSE(registry.GetFunctionUbMetadata(2, metadata));
    registry.RecordBinaryFunctionLookup(1, 3, "kernel_0");
    ASSERT_TRUE(registry.GetFunctionUbMetadata(3, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
}

TEST(SimtUbMetadataTest, EntryLookupUsesFinalLinkedImageFacts)
{
    auto original = Image(0, 8192, ET_REL);
    SetFunctionEntry(original, 3, 42);
    auto linked = Image(0x3000, 0x5000);
    SetFunctionEntry(linked, 3, 42);
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordLoadedImage(1, linked.data(), linked.size()));
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, true, 8, original.data(), original.size()));
    registry.RecordBinaryFunctionLookup(1, 2, nullptr, 42);
    AclsanUbLaunchMetadata metadata{};
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
    EXPECT_NE(metadata.flags & ACLSAN_UB_METADATA_STATIC, 0U);
}

std::string ImageWithTwoLegacyMixedPairs()
{
    auto image = Image(0x3000, 0x5000, ET_EXEC, true);
    Elf64_Ehdr header{};
    std::memcpy(&header, image.data(), sizeof(header));
    std::vector<Elf64_Shdr> sections(header.e_shnum);
    std::memcpy(sections.data(), image.data() + header.e_shoff, sections.size() * sizeof(Elf64_Shdr));
    auto names = image.substr(sections[1].sh_offset, sections[1].sh_size);
    const auto addName = [&names](const char* name) {
        const auto offset = static_cast<uint32_t>(names.size());
        names.append(name);
        names.push_back('\0');
        return offset;
    };
    sections[4].sh_name = addName(".ascend.meta.kernel_17_mix_aiv");
    auto secondAic = sections[5];
    secondAic.sh_name = addName(".ascend.meta.kernel_17_mix_aic");
    sections.push_back(secondAic);
    sections[1].sh_offset = image.size();
    sections[1].sh_size = names.size();
    image += names;
    header.e_shoff = image.size();
    header.e_shnum = sections.size();
    image.append(reinterpret_cast<const char*>(sections.data()), sections.size() * sizeof(Elf64_Shdr));
    std::memcpy(image.data(), &header, sizeof(header));
    return image;
}

TEST(SimtUbMetadataTest, MultipleLegacyMixedPairsDoNotMakeEntryZeroAmbiguous)
{
    const auto image = ImageWithTwoLegacyMixedPairs();
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, true, 8, image.data(), image.size()));
    registry.RecordBinaryFunctionLookup(1, 2, nullptr, 0);
    AclsanUbLaunchMetadata metadata{};
    ASSERT_TRUE(registry.GetFunctionUbMetadata(2, metadata));
    EXPECT_EQ(metadata.userStaticBytes, 0x3000U);
    registry.RecordBinaryFunctionLookup(1, 3, nullptr, 17);
    ASSERT_TRUE(registry.GetFunctionUbMetadata(3, metadata));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY);
}

TEST(SimtUbMetadataTest, RealCompilerLinkedImageWhenProvided)
{
    const char* path = std::getenv("ACLSAN_TEST_SIMT_ELF");
    if (path == nullptr)
        GTEST_SKIP() << "Set ACLSAN_TEST_SIMT_ELF to the real linked 12KiB sample";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::string image{std::istreambuf_iterator<char>(file), {}};
    aclsan::KernelUbMetadataMap kernels;
    std::string error;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(image, kernels, error)) << error;
    ASSERT_EQ(kernels.count("SimtUbKernel"), 1U);
    EXPECT_EQ(kernels.at("SimtUbKernel").launch.compilerAllocatedBytes, 0x5000U);
    EXPECT_EQ(kernels.at("SimtUbKernel").launch.userStaticBytes, 0x3000U);
    EXPECT_NE(kernels.at("SimtUbKernel").launch.flags & ACLSAN_UB_METADATA_STATIC, 0U);
}
} // namespace
