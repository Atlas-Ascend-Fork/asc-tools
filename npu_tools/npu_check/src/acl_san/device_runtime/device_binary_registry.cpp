/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "device_binary_registry.h"
#include "file_io.h"
#include "securec.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <limits>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

namespace aclsan::device_runtime {
namespace {

namespace fs = boost::filesystem;

std::string SymbolizerPath()
{
    if (const char* ascendHome = std::getenv("ASCEND_HOME_PATH"); ascendHome != nullptr && ascendHome[0] != '\0') {
        const fs::path root(ascendHome);
        const std::vector<fs::path> candidates{
            root / "tools/mssanitizer/bin/llvm-symbolizer", root / "tools/msopprof/bin/llvm-symbolizer"};
        for (const fs::path& candidate : candidates) {
            if (access(candidate.c_str(), X_OK) == 0) {
                return candidate.string();
            }
        }
    }
    return "llvm-symbolizer";
}

fs::path WorkRoot()
{
    return fs::temp_directory_path() / ("npu-check-" + std::to_string(static_cast<unsigned long long>(geteuid()))) /
           "symbolizer";
}

bool WriteImage(const fs::path& destination, const void* image, size_t imageBytes)
{
    if (image == nullptr || imageBytes == 0) {
        return false;
    }
    return asctools::security::WriteFile(
        destination.string(), std::string_view(static_cast<const char*>(image), imageBytes));
}

bool ParseLoadSegments(const void* image, size_t imageBytes, std::vector<BinarySegment>& segments)
{
    constexpr size_t kMaxElfProgramHeaders = 4096;
    // Observed Ascend device ELF machine ID; current binutils does not name it.
    constexpr uint16_t kAscendDeviceElfMachine = 0x1029;
    segments.clear();
    std::vector<BinarySegment> parsed;
    if (image == nullptr || imageBytes < sizeof(Elf64_Ehdr)) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(image);
    Elf64_Ehdr header{};
    if (memcpy_s(&header, sizeof(header), bytes, sizeof(header)) != EOK) {
        return false;
    }
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        (header.e_machine != EM_AARCH64 && header.e_machine != kAscendDeviceElfMachine) || header.e_phnum == 0 ||
        header.e_phnum > kMaxElfProgramHeaders || header.e_phentsize != sizeof(Elf64_Phdr)) {
        return false;
    }
    if (header.e_phoff > imageBytes || header.e_phnum > (imageBytes - header.e_phoff) / sizeof(Elf64_Phdr)) {
        return false;
    }
    for (uint16_t index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr program{};
        if (memcpy_s(&program, sizeof(program), bytes + header.e_phoff + index * sizeof(program), sizeof(program)) !=
            EOK) {
            return false;
        }
        if (program.p_type != PT_LOAD) {
            continue;
        }
        if (program.p_filesz > program.p_memsz || program.p_offset > imageBytes ||
            program.p_filesz > imageBytes - program.p_offset ||
            program.p_vaddr > std::numeric_limits<uint64_t>::max() - program.p_memsz) {
            return false;
        }
        if (program.p_memsz == 0) {
            continue;
        }
        parsed.push_back({program.p_vaddr, program.p_memsz, program.p_flags});
    }
    if (parsed.empty()) {
        return false;
    }
    std::sort(parsed.begin(), parsed.end(), [](const BinarySegment& left, const BinarySegment& right) {
        return left.offset < right.offset || (left.offset == right.offset && left.bytes < right.bytes);
    });
    for (size_t index = 1; index < parsed.size(); ++index) {
        const auto& previous = parsed[index - 1];
        const auto& current = parsed[index];
        const uint64_t previousEnd = previous.offset + previous.bytes;
        const uint64_t currentEnd = current.offset + current.bytes;
        if (current.offset < previousEnd &&
            (current.offset != previous.offset || currentEnd != previousEnd || current.flags != previous.flags)) {
            return false;
        }
    }
    parsed.erase(
        std::unique(
            parsed.begin(), parsed.end(),
            [](const BinarySegment& left, const BinarySegment& right) {
                return left.offset == right.offset && left.bytes == right.bytes && left.flags == right.flags;
            }),
        parsed.end());
    segments = std::move(parsed);
    return true;
}

void RemoveDirectory(const std::string& directory) noexcept
{
    if (directory.empty()) {
        return;
    }
    boost::system::error_code error;
    fs::remove_all(directory, error);
}

} // namespace

struct DeviceBinaryRegistry::Impl {
    struct BinaryEntry {
        uint64_t binaryId = 0;
        bool instrumented = false;
        uint32_t traceArgumentOffset = 0;
        std::string sessionDirectory;
        std::unique_ptr<DeviceSymbolizer> symbolizer;
        std::string symbolizerError = "invalid_state";
        std::vector<BinarySegment> actualSegments;
    };

    struct LoadedImage {
        std::vector<BinarySegment> segments;
    };

    mutable std::mutex mutex;
    uint64_t nextBinaryId = 1;
    uintptr_t latestBinary = 0;
    std::unordered_map<uintptr_t, BinaryEntry> binaries;
    std::unordered_map<uintptr_t, uintptr_t> functionBinaries;
    std::unordered_map<uintptr_t, std::string> functionNames;
    std::unordered_map<uintptr_t, std::vector<BinarySectionRange>> readOnlySections;
    std::unordered_map<uintptr_t, LoadedImage> actualSegments;

    uint64_t AllocateBinaryId() noexcept
    {
        if (nextBinaryId == 0) {
            return 0;
        }
        return nextBinaryId++;
    }

    void RemoveOwnedFunctions(uintptr_t binary) noexcept
    {
        for (auto it = functionBinaries.begin(); it != functionBinaries.end();) {
            if (it->second == binary) {
                functionNames.erase(it->first);
                it = functionBinaries.erase(it);
            } else {
                ++it;
            }
        }
    }

    void RemoveFunctionOwnership(uintptr_t function) noexcept
    {
        functionBinaries.erase(function);
        functionNames.erase(function);
    }
};

DeviceBinaryRegistry::DeviceBinaryRegistry() : impl_(std::make_unique<Impl>()) {}

DeviceBinaryRegistry::~DeviceBinaryRegistry() { Reset(); }

bool DeviceBinaryRegistry::RecordBinaryLoadFromData(
    uintptr_t binary, bool instrumented, uint32_t traceArgumentOffset, const void* image, size_t imageBytes) noexcept
{
    if (binary == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto actual = impl_->actualSegments.find(binary);
        Impl::BinaryEntry entry;
        entry.binaryId = impl_->AllocateBinaryId();
        if (entry.binaryId == 0) {
            return false;
        }
        entry.instrumented = instrumented;
        entry.traceArgumentOffset = traceArgumentOffset;
        if (instrumented && actual != impl_->actualSegments.end()) {
            entry.actualSegments = std::move(actual->second.segments);
        }
        if (actual != impl_->actualSegments.end()) {
            impl_->actualSegments.erase(actual);
        }
        if (instrumented) {
            const fs::path workRoot = WorkRoot();
            const fs::path session =
                workRoot / ("aclsan-symbolizer-" + std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
                            std::to_string(entry.binaryId));
            boost::system::error_code error;
            fs::create_directories(workRoot, error);
            if (!error) {
                fs::permissions(workRoot, fs::perms::owner_all, error);
            }
            if (!error) {
                fs::create_directories(session, error);
            }
            if (!error) {
                fs::permissions(session, fs::perms::owner_all, error);
            }
            std::string sessionDirectory;
            const bool validSession =
                !error && asctools::security::NormalizePath(
                              session.string(), asctools::security::PathKind::ExistingDirectory, sessionDirectory);
            const fs::path sourceImage = fs::path(sessionDirectory) / "original_device.elf";
            if (validSession && WriteImage(sourceImage, image, imageBytes)) {
                entry.sessionDirectory = sessionDirectory;
                entry.symbolizer = std::make_unique<DeviceSymbolizer>(
                    DeviceSymbolizerConfig{SymbolizerPath(), sourceImage.string(), sessionDirectory});
                entry.symbolizerError.clear();
            } else {
                RemoveDirectory(session.string());
                entry.symbolizerError = "source_image_unavailable";
            }
        }
        if (!instrumented) {
            (void)ParseLoadSegments(image, imageBytes, entry.actualSegments);
        }
        const auto replaced = impl_->binaries.find(binary);
        if (replaced != impl_->binaries.end()) {
            RemoveDirectory(replaced->second.sessionDirectory);
            impl_->RemoveOwnedFunctions(binary);
        }
        impl_->binaries[binary] = std::move(entry);
        if (!instrumented) {
            impl_->readOnlySections.erase(binary);
        }
        impl_->latestBinary = binary;
        return impl_->binaries[binary].symbolizer != nullptr || !instrumented;
    } catch (...) {
        return false;
    }
}

void DeviceBinaryRegistry::RecordBinaryUnload(uintptr_t binary) noexcept
{
    if (binary == 0) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->binaries.find(binary);
        impl_->readOnlySections.erase(binary);
        impl_->actualSegments.erase(binary);
        if (found == impl_->binaries.end()) {
            return;
        }
        RemoveDirectory(found->second.sessionDirectory);
        impl_->binaries.erase(found);
        impl_->RemoveOwnedFunctions(binary);
        if (impl_->latestBinary == binary) {
            impl_->latestBinary = 0;
        }
    } catch (...) {
    }
}

void DeviceBinaryRegistry::RecordBinaryFunctionLookup(
    uintptr_t binary, uintptr_t function, const char* functionName) noexcept
{
    if (binary == 0 || function == 0) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->RemoveFunctionOwnership(function);
        const auto loaded = impl_->binaries.find(binary);
        if (loaded != impl_->binaries.end()) {
            impl_->functionBinaries[function] = binary;
            if (functionName != nullptr) {
                impl_->functionNames[function] = functionName;
            }
        }
    } catch (...) {
    }
}

void DeviceBinaryRegistry::RecordLatestBinaryFunctionLookup(uintptr_t function) noexcept
{
    if (function == 0) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->RemoveFunctionOwnership(function);
        const auto loaded = impl_->binaries.find(impl_->latestBinary);
        if (loaded != impl_->binaries.end()) {
            impl_->functionBinaries[function] = impl_->latestBinary;
        }
    } catch (...) {
    }
}

bool DeviceBinaryRegistry::RecordLoadedImage(uintptr_t binary, const void* image, size_t bytes) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->readOnlySections.erase(binary);
        impl_->actualSegments.erase(binary);
        if (binary == 0 || image == nullptr || bytes == 0) {
            return false;
        }
        std::vector<BinarySectionRange> sections;
        std::string diagnostic;
        const bool sectionsValid =
            GetReadOnlyBinarySections(std::string(static_cast<const char*>(image), bytes), sections, diagnostic);
        if (sectionsValid) {
            impl_->readOnlySections[binary] = std::move(sections);
        }
        std::vector<BinarySegment> actual;
        if (ParseLoadSegments(image, bytes, actual)) {
            impl_->actualSegments[binary] = {std::move(actual)};
        }
        return sectionsValid;
    } catch (...) {
        return false;
    }
}

bool DeviceBinaryRegistry::GetFunctionReadOnlySections(
    uintptr_t function, uintptr_t& binary, std::vector<BinarySectionRange>& sections) const
{
    binary = 0;
    sections.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto owner = impl_->functionBinaries.find(function);
    if (owner == impl_->functionBinaries.end()) {
        return false;
    }
    const auto found = impl_->readOnlySections.find(owner->second);
    if (found == impl_->readOnlySections.end()) {
        return false;
    }
    binary = owner->second;
    sections = found->second;
    return true;
}

bool DeviceBinaryRegistry::GetFunctionActualBinary(
    uintptr_t function, uintptr_t& binary, std::vector<BinarySegment>& segments) const noexcept
{
    binary = 0;
    segments.clear();
    if (function == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto owner = impl_->functionBinaries.find(function);
        if (owner == impl_->functionBinaries.end()) {
            return false;
        }
        const auto loaded = impl_->binaries.find(owner->second);
        if (loaded == impl_->binaries.end() || loaded->second.actualSegments.empty()) {
            return false;
        }
        binary = owner->second;
        segments = loaded->second.actualSegments;
        return true;
    } catch (...) {
        return false;
    }
}

bool DeviceBinaryRegistry::GetFunctionName(uintptr_t function, std::string& functionName) const noexcept
{
    if (function == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->functionNames.find(function);
        if (found == impl_->functionNames.end()) {
            return false;
        }
        functionName = found->second;
        return true;
    } catch (...) {
        return false;
    }
}

bool DeviceBinaryRegistry::GetFunctionTraceArgumentOffset(
    uintptr_t function, uint32_t& traceArgumentOffset) const noexcept
{
    if (function == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // 根据aclrtFuncHandle查询aclrtBinHandle
        const auto ownership = impl_->functionBinaries.find(function);
        if (ownership == impl_->functionBinaries.end()) {
            return false;
        }
        // 根据aclrtBinHandle查询BinaryEntry
        const auto binary = impl_->binaries.find(ownership->second);
        if (binary == impl_->binaries.end() || !binary->second.instrumented ||
            binary->second.traceArgumentOffset == 0) {
            return false;
        }
        traceArgumentOffset = binary->second.traceArgumentOffset;
        return true;
    } catch (...) {
        return false;
    }
}

void DeviceBinaryRegistry::Reset() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& [binary, entry] : impl_->binaries) {
            (void)binary;
            RemoveDirectory(entry.sessionDirectory);
        }
        impl_->binaries.clear();
        impl_->readOnlySections.clear();
        impl_->actualSegments.clear();
        impl_->functionBinaries.clear();
        impl_->latestBinary = 0;
    } catch (...) {
    }
}

CallStackResult DeviceBinaryRegistry::ResolveCallStack(uint64_t pc) const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto active = impl_->binaries.find(impl_->latestBinary);
        if (active == impl_->binaries.end()) {
            return {pc, 0, false, "invalid_state", {}};
        }
        if (active->second.symbolizer == nullptr) {
            return {pc, active->second.binaryId, false, active->second.symbolizerError, {}};
        }
        CallStackResult result = active->second.symbolizer->ResolveCallStack(pc);
        result.binaryId = active->second.binaryId;
        return result;
    } catch (...) {
        return {pc, 0, false, "symbolizer_runtime_failure", {}};
    }
}

CallStackResult DeviceBinaryRegistry::ResolveCallStackWithRunner(
    uint64_t pc, const CommandRunner& runner) const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto active = impl_->binaries.find(impl_->latestBinary);
        if (active == impl_->binaries.end()) {
            return {pc, 0, false, "invalid_state", {}};
        }
        if (active->second.symbolizer == nullptr) {
            return {pc, active->second.binaryId, false, active->second.symbolizerError, {}};
        }
        CallStackResult result = active->second.symbolizer->ResolveCallStackWithRunner(pc, runner);
        result.binaryId = active->second.binaryId;
        return result;
    } catch (...) {
        return {pc, 0, false, "symbolizer_runtime_failure", {}};
    }
}

} // namespace aclsan::device_runtime
