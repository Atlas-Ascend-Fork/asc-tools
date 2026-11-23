// Copyright (c) 2025 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.

#include <gtest/gtest.h>

#include "dbi/kernel_param_metadata.h"
#include "device_runtime/device_binary_registry.h"

#include <cassert>
#include <cstring>
#include <elf.h>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {
template <class T>
void Put(std::string& data, size_t offset, T value)
{
    assert(offset + sizeof(value) <= data.size());
    std::memcpy(&data[offset], &value, sizeof(value));
}

template <class T>
T Get(const std::string& data, size_t offset)
{
    T value{};
    assert(offset + sizeof(value) <= data.size());
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::string Metadata(uint32_t count, uint32_t lastOffset)
{
    std::string data(16 + 40 * count, '\0');
    Put<uint16_t>(data, 0, 16);
    Put<uint16_t>(data, 2, 12);
    Put<uint32_t>(data, 4, count);
    Put<uint32_t>(data, 8, count == 0 ? 0 : lastOffset + 8);
    for (uint32_t i = 0; i < count; ++i) {
        size_t pos = 16 + i * 40;
        Put<uint16_t>(data, pos, 17);
        Put<uint16_t>(data, pos + 2, 36);
        Put<uint32_t>(data, pos + 8, i);
        Put<uint32_t>(data, pos + 12, i + 1 == count ? lastOffset : i * 8);
        Put<uint32_t>(data, pos + 16, 8);
        Put<uint16_t>(data, pos + 20, 8);
        Put<uint16_t>(data, pos + 22, 2);
    }
    // 未知 TLV 必须原样保留。
    data += std::string("\x63\x00\x03\x00xyz", 7);
    return data;
}

std::string ReorderParameters(std::string data, const std::vector<uint32_t>& order)
{
    assert(Get<uint32_t>(data, 4) == order.size());
    const auto original = data;
    for (size_t i = 0; i < order.size(); ++i) {
        assert(order[i] < order.size());
        data.replace(16 + i * 40, 40, original.substr(16 + order[i] * 40, 40));
    }
    return data;
}

std::string ControlMetadata(uint32_t kernelType)
{
    std::string data(8, '\0');
    Put<uint16_t>(data, 0, 1);
    Put<uint16_t>(data, 2, 4);
    Put<uint32_t>(data, 4, kernelType);
    return data;
}

std::string Elf(const std::vector<std::string>& metadata, const std::vector<std::string>& kernelNames = {})
{
    const size_t count = metadata.size() + 3;
    std::string data(sizeof(Elf64_Ehdr) + count * sizeof(Elf64_Shdr), '\0');
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header);
    header.e_shoff = sizeof(header);
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = count;
    header.e_shstrndx = 1;
    Put(data, 0, header);
    std::string names(1, '\0');
    const auto append = [&](size_t index, const std::string& name, const std::string& content, uint64_t flags) {
        Elf64_Shdr section{};
        section.sh_name = names.size();
        names += name + '\0';
        section.sh_type = index >= 3 ? SHT_NOTE : SHT_PROGBITS;
        section.sh_flags = flags;
        section.sh_offset = data.size();
        section.sh_size = content.size();
        section.sh_addralign = 1;
        data += content;
        Put(data, header.e_shoff + index * sizeof(section), section);
    };
    append(2, ".text", "executable-bytes", SHF_ALLOC | SHF_EXECINSTR);
    for (size_t i = 0; i < metadata.size(); ++i) {
        append(
            i + 3, ".ascend.meta." + (kernelNames.empty() ? "kernel" + std::to_string(i) : kernelNames.at(i)),
            metadata[i], 0);
    }
    Elf64_Shdr strings{};
    strings.sh_type = SHT_STRTAB;
    strings.sh_offset = data.size();
    strings.sh_size = names.size();
    data += names;
    Put(data, header.e_shoff + sizeof(strings), strings);
    return data;
}

void CheckRejected(const std::string& original, const std::string& input, uint32_t offset)
{
    std::string output = input;
    std::string diagnostic;
    assert(!aclsan::ModifyKernelParamMetadata(original, output, offset, diagnostic));
    assert(!diagnostic.empty());
    assert(output == input);
}

void CheckMixedAndIdempotent()
{
    const auto original = Elf({Metadata(0, 0), Metadata(1, 0), Metadata(3, 16)});
    auto patched = Elf({Metadata(0, 0), Metadata(2, 8), Metadata(4, 24)});
    const auto before = patched;
    std::string diagnostic;
    assert(aclsan::ModifyKernelParamMetadata(original, patched, 24, diagnostic));
    const uint32_t counts[] = {1, 2, 4};
    for (size_t i = 0; i < 3; ++i) {
        const auto section = Get<Elf64_Shdr>(patched, 64 + (i + 3) * 64);
        const size_t pos = section.sh_offset;
        assert(patched.substr(pos + section.sh_size - 7, 7) == std::string("\x63\x00\x03\x00xyz", 7));
        assert(Get<uint32_t>(patched, pos + 4) == counts[i]);
        assert(Get<uint32_t>(patched, pos + 8) == 32);
        const size_t pointer = pos + 16 + (counts[i] - 1) * 40;
        assert(Get<uint32_t>(patched, pointer + 12) == 24);
        assert(Get<uint32_t>(patched, pointer + 16) == 8);
        assert(Get<uint16_t>(patched, pointer + 22) == 2);
    }
    auto text = Get<Elf64_Shdr>(before, 64 + 2 * 64);
    assert(before.substr(text.sh_offset, text.sh_size) == patched.substr(text.sh_offset, text.sh_size));
    const auto normalized = patched;
    assert(aclsan::ModifyKernelParamMetadata(original, patched, 24, diagnostic));
    assert(patched == normalized);
    CheckRejected(original, before, 8);
    CheckRejected(original, before, std::numeric_limits<uint32_t>::max());
    CheckRejected(original, before.substr(0, 63), 24);
    auto invalid = before;
    Put<uint64_t>(invalid, 40, std::numeric_limits<uint64_t>::max());
    CheckRejected(original, invalid, 24);
    invalid = before;
    text = Get<Elf64_Shdr>(invalid, 64 + 3 * 64);
    Put<uint16_t>(invalid, text.sh_offset + 2, 65535);
    CheckRejected(original, invalid, 24);
    text.sh_flags = SHF_ALLOC;
    invalid = before;
    Put(invalid, 64 + 3 * 64, text);
    CheckRejected(original, invalid, 24);
    CheckRejected(original, Elf({Metadata(0, 0)}), 24);
    auto metadata = Metadata(1, 0);
    Put<uint32_t>(metadata, 4, 2);
    CheckRejected(Elf({metadata}), before, 24);
    metadata = Metadata(2, 0);
    CheckRejected(Elf({metadata}), before, 24);
    metadata = Metadata(0, 0);
    metadata += metadata.substr(0, 16);
    CheckRejected(Elf({metadata}), before, 24);
    invalid = before;
    invalid[EI_DATA] = ELFDATA2MSB;
    CheckRejected(original, invalid, 24);
    invalid = before;
    const auto first = Get<Elf64_Shdr>(invalid, 64 + 3 * 64);
    auto second = Get<Elf64_Shdr>(invalid, 64 + 4 * 64);
    second.sh_name = first.sh_name;
    Put(invalid, 64 + 4 * 64, second);
    CheckRejected(original, invalid, 24);
}

void CheckControlMetadataIsIgnoredAndPreserved()
{
    const auto original = Elf({ControlMetadata(3), Metadata(1, 0)});
    auto patched = Elf({ControlMetadata(7), Metadata(2, 8)});
    const auto before = patched;
    std::string diagnostic;
    assert(aclsan::ModifyKernelParamMetadata(original, patched, 24, diagnostic));

    const auto control = Get<Elf64_Shdr>(patched, 64 + 3 * 64);
    const auto controlBefore = Get<Elf64_Shdr>(before, 64 + 3 * 64);
    assert(
        patched.substr(control.sh_offset, control.sh_size) ==
        before.substr(controlBefore.sh_offset, controlBefore.sh_size));
    assert(Get<uint32_t>(patched, control.sh_offset + 4) == 7);

    const auto parameters = Get<Elf64_Shdr>(patched, 64 + 4 * 64);
    assert(Get<uint32_t>(patched, parameters.sh_offset + 4) == 2);
    assert(Get<uint32_t>(patched, parameters.sh_offset + 8) == 32);

    uint32_t maximumArea = 0;
    bool found = false;
    assert(aclsan::GetMaximumKernelArgumentArea(original, maximumArea, found, diagnostic));
    assert(found);
    assert(maximumArea == 8);

    assert(aclsan::GetMaximumKernelArgumentArea(Elf({ControlMetadata(3)}), maximumArea, found, diagnostic));
    assert(!found);
    assert(maximumArea == 0);
}

void CheckRuntimeOrdinalSemantics()
{
    auto originalMetadata = Metadata(3, 16);
    Put<uint32_t>(originalMetadata, 16 + 12, 16);
    Put<uint32_t>(originalMetadata, 16 + 40 + 12, 0);
    Put<uint32_t>(originalMetadata, 16 + 80 + 12, 8);
    originalMetadata = ReorderParameters(std::move(originalMetadata), {2, 0, 1});

    const auto original = Elf({originalMetadata});
    auto patched = Elf({ReorderParameters(Metadata(4, 24), {3, 1, 0, 2})});
    std::string diagnostic;
    assert(aclsan::ModifyKernelParamMetadata(original, patched, 24, diagnostic));

    const auto section = Get<Elf64_Shdr>(patched, 64 + 3 * 64);
    const uint32_t expectedOffsets[] = {16, 0, 8, 24};
    for (uint32_t ordinal = 0; ordinal < 4; ++ordinal) {
        const size_t parameter = section.sh_offset + 16 + ordinal * 40;
        assert(Get<uint32_t>(patched, parameter + 8) == ordinal);
        assert(Get<uint32_t>(patched, parameter + 12) == expectedOffsets[ordinal]);
    }

    uint32_t maximumArea = 0;
    bool found = false;
    assert(aclsan::GetMaximumKernelArgumentArea(original, maximumArea, found, diagnostic));
    assert(found);
    assert(maximumArea == 24);

    auto invalid = Metadata(2, 8);
    Put<uint32_t>(invalid, 16 + 40 + 8, 0);
    CheckRejected(Elf({invalid}), patched, 24);
    invalid = Metadata(2, 8);
    Put<uint32_t>(invalid, 16 + 40 + 8, 2);
    CheckRejected(Elf({invalid}), patched, 24);
}
} // namespace

TEST(KernelParamMetadata, ReadOnlySectionsUseDeviceOffsetsAndExcludeWritableStorage)
{
    auto elf = Elf({Metadata(1, 0)});
    const size_t textHeader = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Shdr);
    auto section = Get<Elf64_Shdr>(elf, textHeader);
    section.sh_addr = 0x9000;
    Put(elf, textHeader, section);
    std::vector<aclsan::BinarySectionRange> ranges;
    std::string diagnostic;
    ASSERT_TRUE(aclsan::GetReadOnlyBinarySections(elf, ranges, diagnostic));
    ASSERT_EQ(ranges.size(), 1U);
    EXPECT_EQ(ranges[0].offset, 0x9000U);
    EXPECT_EQ(ranges[0].bytes, section.sh_size);
    for (uint64_t flags : {uint64_t(SHF_ALLOC | SHF_WRITE), uint64_t(SHF_ALLOC | SHF_TLS), uint64_t(0)}) {
        section.sh_flags = flags;
        Put(elf, textHeader, section);
        ASSERT_TRUE(aclsan::GetReadOnlyBinarySections(elf, ranges, diagnostic));
        EXPECT_TRUE(ranges.empty());
    }
    section.sh_flags = SHF_ALLOC;
    section.sh_addr = UINT64_MAX;
    Put(elf, textHeader, section);
    EXPECT_FALSE(aclsan::GetReadOnlyBinarySections(elf, ranges, diagnostic));
    EXPECT_TRUE(ranges.empty());
    section.sh_addr = 0;
    section.sh_offset = elf.size();
    Put(elf, textHeader, section);
    EXPECT_FALSE(aclsan::GetReadOnlyBinarySections(elf, ranges, diagnostic));
    EXPECT_TRUE(ranges.empty());
}

TEST(KernelParamMetadata, Main)
{
    {
        const auto original =
            Elf({Metadata(0, 0), Metadata(0, 0), Metadata(0, 0)}, {"kernel_0_mix_aic", "kernel_0_mix_aiv", "kernel_1"});
        auto patched = Elf({Metadata(0, 0), Metadata(0, 0)}, {"kernel_0_mix_aic", "kernel_0_mix_aiv"});
        std::string diagnostic;
        const uint64_t key = 0;
        assert(aclsan::ModifyKernelParamMetadata(original, patched, 16, diagnostic, &key));
        auto missingHalf = Elf({Metadata(0, 0)}, {"kernel_0_mix_aic"});
        const auto unchanged = missingHalf;
        assert(!aclsan::ModifyKernelParamMetadata(original, missingHalf, 16, diagnostic, &key));
        assert(missingHalf == unchanged);
        const uint64_t missing = 2;
        assert(!aclsan::ModifyKernelParamMetadata(original, patched, 16, diagnostic, &missing));
        CheckRejected(original, patched, 16);
    }
    CheckMixedAndIdempotent();
    CheckControlMetadataIsIgnoredAndPreserved();
    CheckRuntimeOrdinalSemantics();
    // Old ACLNN binaries can carry tiling/shape TLVs without Runtime parameter descriptors.
    const std::string legacy("\x01\x00\x04\x00\x03\x00\x00\x00", 8);
    std::string legacyDiagnostic;
    const auto legacyOriginal = Elf({legacy});
    auto legacyPatched = legacyOriginal;
    assert(aclsan::ModifyKernelParamMetadata(legacyOriginal, legacyPatched, 24, legacyDiagnostic));
    assert(legacyPatched == legacyOriginal);
    legacyPatched = Elf({legacy + Metadata(1, 24)});
    assert(aclsan::ModifyKernelParamMetadata(legacyOriginal, legacyPatched, 24, legacyDiagnostic));
    const auto legacySection = Get<Elf64_Shdr>(legacyPatched, 64 + 3 * 64);
    assert(
        legacyPatched.substr(legacySection.sh_offset, legacySection.sh_size) ==
        legacy + std::string("\x63\x00\x03\x00xyz", 7));
    CheckRejected(Elf({Metadata(1, 0).substr(16)}), legacyPatched, 24);
    CheckRejected(Elf({Metadata(1, 0)}), Elf({legacy}), 24);
    const auto original = Elf({Metadata(0, 0)});
    auto patched = original;
    std::string diagnostic;
    assert(aclsan::ModifyKernelParamMetadata(original, patched, 8, diagnostic));
    const auto section = Get<Elf64_Shdr>(patched, 64 + 3 * 64);
    assert(Get<uint32_t>(patched, section.sh_offset + 4) == 1);
    assert(Get<uint32_t>(patched, section.sh_offset + 28) == 8);
    std::cout << "kernel parameter metadata tests passed\n";
}

namespace {
std::string UbMetadata(uint32_t type, uint32_t bytes = 65536)
{
    std::string data(16, '\0');
    Put<uint16_t>(data, 0, 12);
    Put<uint16_t>(data, 2, 4);
    Put<uint32_t>(data, 4, type);
    Put<uint16_t>(data, 8, 7);
    Put<uint16_t>(data, 10, 4);
    Put<uint32_t>(data, 12, bytes);
    return data;
}
} // namespace

TEST(KernelUbMetadata, SelectsNamedKernelAndEntry)
{
    auto elf = Elf({UbMetadata(1), UbMetadata(4)}, {"plain_0", "mixed_1"});
    aclsan::KernelUbMetadata result;
    std::string diagnostic;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(elf, "mixed_1", result, diagnostic));
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF);
    EXPECT_TRUE(result.compilerAllocUbValid);
    EXPECT_EQ(result.compilerAllocUbBytes, 65536U);
    const uint64_t entry = 0;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic, &entry));
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_NO_VF);
    EXPECT_FALSE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic));
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_UNKNOWN);
    EXPECT_FALSE(aclsan::GetKernelUbMetadata(elf, "absent", result, diagnostic));
}

TEST(KernelUbMetadata, AcceptsOnlyZeroPaddingThatAlignsTheNote)
{
    const auto metadata = UbMetadata(3, 8192) + std::string("\x14\x00\x02\x00\x00\x04", 6);
    const auto makeElf = [&](const std::string& tail) {
        auto elf = Elf({metadata + tail});
        constexpr size_t sectionOffset = sizeof(Elf64_Ehdr) + 3 * sizeof(Elf64_Shdr);
        auto section = Get<Elf64_Shdr>(elf, sectionOffset);
        section.sh_addralign = 4;
        Put(elf, sectionOffset, section);
        return elf;
    };

    aclsan::KernelUbMetadata result;
    std::string diagnostic;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(makeElf(std::string(2, '\0')), "kernel0", result, diagnostic))
        << diagnostic;
    EXPECT_EQ(result.kernelVfType, 3U);
    EXPECT_TRUE(result.compilerAllocUbValid);
    EXPECT_EQ(result.compilerAllocUbBytes, 8192U);

    EXPECT_FALSE(aclsan::GetKernelUbMetadata(makeElf(std::string("\0\x01", 2)), "kernel0", result, diagnostic));
    EXPECT_FALSE(aclsan::GetKernelUbMetadata(makeElf(std::string(1, '\0')), "kernel0", result, diagnostic));
}

TEST(KernelUbMetadata, RejectsMissingMalformedAndConflictingValues)
{
    aclsan::KernelUbMetadata result;
    std::string diagnostic;
    for (const auto& raw :
         {UbMetadata(0), UbMetadata(5), UbMetadata(2) + UbMetadata(4), UbMetadata(2).substr(0, 7), Metadata(0, 0)}) {
        EXPECT_FALSE(aclsan::GetKernelUbMetadata(Elf({raw}), "kernel0", result, diagnostic));
        EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_UNKNOWN);
    }
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(Elf({UbMetadata(1).substr(0, 8)}), "kernel0", result, diagnostic));
    EXPECT_FALSE(result.compilerAllocUbValid);
    EXPECT_FALSE(aclsan::GetKernelUbMetadata("bad", "kernel0", result, diagnostic));
}

TEST(KernelUbMetadata, ExplicitEntryTakesPrecedenceOverNameSuffix)
{
    const auto functionEntry = [](uint64_t entry, uint8_t flag = 0) {
        std::string data(16, '\0');
        Put<uint16_t>(data, 0, 14);
        Put<uint16_t>(data, 2, 12);
        Put<uint8_t>(data, 4, flag);
        Put<uint64_t>(data, 8, entry);
        return data;
    };
    const auto elf = Elf(
        {UbMetadata(3) + functionEntry(42), UbMetadata(1) + functionEntry(99), UbMetadata(2) + functionEntry(91, 0x80)},
        {"actual", "unrelated_42", "disabled_7"});
    aclsan::KernelUbMetadata result;
    std::string diagnostic;
    uint64_t entry = 42;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic, &entry)) << diagnostic;
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMT_VF_ONLY);
    entry = 99;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic, &entry)) << diagnostic;
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_NO_VF);
    entry = 7;
    EXPECT_FALSE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic, &entry));
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(elf, "disabled_7", result, diagnostic)) << diagnostic;
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY);
    entry = 0;
    ASSERT_TRUE(
        aclsan::GetKernelUbMetadata(Elf({UbMetadata(4)}, {"no_numeric_suffix"}), "", result, diagnostic, &entry))
        << diagnostic;
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF);
}

TEST(KernelUbMetadata, SelectsAivHalfWithoutConflatingOtherEntry)
{
    const uint64_t entry = 0;
    const auto elf =
        Elf({Metadata(0, 0), UbMetadata(4), UbMetadata(2)}, {"kernel_0_mix_aic", "kernel_0_mix_aiv", "kernel_1"});
    aclsan::KernelUbMetadata result;
    std::string diagnostic;
    ASSERT_TRUE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic, &entry));
    EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF);
    const auto ambiguous = Elf({UbMetadata(2), UbMetadata(4)}, {"one_0", "two_0"});
    EXPECT_FALSE(aclsan::GetKernelUbMetadata(ambiguous, "", result, diagnostic, &entry));
}

TEST(KernelUbMetadata, LegacyEntrySuffixRequiresCompleteDecimalWithoutOverflow)
{
    const std::vector<std::pair<std::string, uint64_t>> suffixes = {
        {"", 0},
        {"0", 0},
        {"0005", 5},
        {"18446744073709551615", UINT64_MAX},
        {"18446744073709551616", 0},
        {"999999999999999999999999999999", 0},
        {"+5", 0},
        {"-5", 0},
        {"5tail", 0},
        {" 5", 0},
        {"5 ", 0},
        {"0x5", 0}};
    for (const auto& suffix : suffixes) {
        for (const std::string ending : {"", "_mix_aiv"}) {
            const std::string name = "kernel_" + suffix.first + ending;
            SCOPED_TRACE(name);
            const auto elf = Elf({UbMetadata(2)}, {name});
            aclsan::KernelUbMetadata result;
            std::string diagnostic;
            ASSERT_TRUE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic, &suffix.second)) << diagnostic;
            EXPECT_EQ(result.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_VF_ONLY);
            const uint64_t otherEntry = suffix.second == 0 ? 1 : 0;
            EXPECT_FALSE(aclsan::GetKernelUbMetadata(elf, "", result, diagnostic, &otherEntry));
        }
    }
}

TEST(KernelUbMetadata, RegistryScopesMetadataToFunctionAndBinaryLifetime)
{
    const auto image = Elf({UbMetadata(1), UbMetadata(4)}, {"plain_0", "mixed_1"});
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, image.data(), image.size()));
    registry.RecordBinaryFunctionLookup(1, 10, "plain_0");
    const uint64_t entry = 1;
    registry.RecordBinaryFunctionLookup(1, 11, nullptr, entry);
    aclsan::KernelUbMetadata metadata;
    ASSERT_TRUE(registry.GetFunctionUbMetadata(10, metadata));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_NO_VF);
    ASSERT_TRUE(registry.GetFunctionUbMetadata(11, metadata));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF);
    registry.RecordBinaryFunctionLookup(1, 10, "missing");
    EXPECT_FALSE(registry.GetFunctionUbMetadata(10, metadata));
    registry.RecordBinaryUnload(1);
    EXPECT_FALSE(registry.GetFunctionUbMetadata(11, metadata));
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, image.data(), image.size()));
    EXPECT_FALSE(registry.GetFunctionUbMetadata(11, metadata));
    registry.RecordBinaryFunctionLookup(1, 11, "plain_0");
    registry.Reset();
    EXPECT_FALSE(registry.GetFunctionUbMetadata(11, metadata));
}

TEST(KernelUbMetadata, RegistryKeepsDeviceMetadataIndependentAcrossReset)
{
    const auto image = Elf({UbMetadata(1), UbMetadata(4)}, {"plain_0", "mixed_1"});
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, image.data(), image.size(), 0));
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, image.data(), image.size(), 1));
    registry.RecordBinaryFunctionLookup(1, 10, "plain_0", std::nullopt, 0);
    registry.RecordBinaryFunctionLookup(1, 10, "mixed_1", std::nullopt, 1);
    aclsan::KernelUbMetadata metadata;
    ASSERT_TRUE(registry.GetFunctionUbMetadata(10, metadata, 0));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_NO_VF);
    ASSERT_TRUE(registry.GetFunctionUbMetadata(10, metadata, 1));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF);
    registry.ResetDevice(0);
    EXPECT_FALSE(registry.GetFunctionUbMetadata(10, metadata, 0));
    ASSERT_TRUE(registry.GetFunctionUbMetadata(10, metadata, 1));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF);
}

TEST(KernelUbMetadata, RegistryMovesMetadataWithAdoptedBinary)
{
    const auto image = Elf({UbMetadata(4)}, {"mixed_1"});
    aclsan::device_runtime::DeviceBinaryRegistry registry;
    ASSERT_TRUE(registry.RecordBinaryLoadFromData(1, false, 0, image.data(), image.size()));
    registry.RecordBinaryFunctionLookup(1, 10, "mixed_1");
    uint32_t offset = 0;
    EXPECT_EQ(
        registry.LookupFunctionTraceArgumentOffset(10, offset, 0),
        aclsan::device_runtime::DeviceBinaryRegistry::TraceLookup::NotInstrumented);
    aclsan::KernelUbMetadata metadata;
    ASSERT_TRUE(registry.GetFunctionUbMetadata(10, metadata, 0));
    EXPECT_EQ(metadata.kernelVfType, ACLSAN_KERNEL_VF_TYPE_SIMD_SIMT_MIX_VF);
    registry.RecordBinaryUnload(1, 0);
    EXPECT_FALSE(registry.GetFunctionUbMetadata(10, metadata, 0));
}

#include "ub_launch_config.h"
TEST(UbLaunchConfig, DynamicAllocationIsReadWithoutInferringKernelType)
{
    uint64_t bytes = 1;
    ASSERT_TRUE(aclsan::ReadDynamicUbBytes(nullptr, bytes));
    EXPECT_EQ(bytes, 0U);
    aclrtLaunchKernelAttr attr{};
    attr.id = ACL_RT_LAUNCH_KERNEL_ATTR_DYN_UBUF_SIZE;
    attr.value.dynUBufSize = 32768;
    aclrtLaunchKernelCfg config{&attr, 1};
    ASSERT_TRUE(aclsan::ReadDynamicUbBytes(&config, bytes));
    EXPECT_EQ(bytes, 32768U);
    config.attrs = nullptr;
    EXPECT_FALSE(aclsan::ReadDynamicUbBytes(&config, bytes));
    aclrtLaunchKernelAttr duplicate[2]{attr, attr};
    duplicate[1].value.dynUBufSize = 65536;
    config = {duplicate, 2};
    ASSERT_TRUE(aclsan::ReadDynamicUbBytes(&config, bytes));
    EXPECT_EQ(bytes, 65536U);
}
