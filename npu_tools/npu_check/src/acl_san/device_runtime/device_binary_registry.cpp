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
#include "npu_tool_log.h"
#include "aclsan_trace_status.h"
#include "securec.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <limits>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <limits>
#include <map>
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

bool ReadLoadSegmentHeader(const uint8_t* bytes, size_t imageBytes, Elf64_Ehdr& header)
{
    constexpr size_t MAX_ELF_PROGRAM_HEADERS = 4096;
    // Observed Ascend device ELF machine ID; current binutils does not name it.
    constexpr uint16_t ASCEND_DEVICE_ELF_MACHINE = 0x1029;
    if (bytes == nullptr || imageBytes < sizeof(Elf64_Ehdr)) {
        return false;
    }
    if (memcpy_s(&header, sizeof(header), bytes, sizeof(header)) != EOK) {
        return false;
    }
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        (header.e_machine != EM_AARCH64 && header.e_machine != ASCEND_DEVICE_ELF_MACHINE) || header.e_phnum == 0U ||
        header.e_phnum > MAX_ELF_PROGRAM_HEADERS || header.e_phentsize != sizeof(Elf64_Phdr)) {
        return false;
    }
    return header.e_phoff <= imageBytes && header.e_phnum <= (imageBytes - header.e_phoff) / sizeof(Elf64_Phdr);
}

bool ParseLoadSegments(const void* image, size_t imageBytes, std::vector<BinarySegment>& segments)
{
    segments.clear();
    std::vector<BinarySegment> parsed;
    const auto* bytes = static_cast<const uint8_t*>(image);
    Elf64_Ehdr header{};
    if (!ReadLoadSegmentHeader(bytes, imageBytes, header)) {
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

bool EndsWith(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

std::string_view LogicalKernelName(std::string_view name)
{
    for (const std::string_view suffix : {"_mix_aiv", "_mix_aic"}) {
        if (EndsWith(name, suffix)) {
            name.remove_suffix(suffix.size());
            break;
        }
    }
    return name;
}

const AclsanUbLaunchMetadata* FindUbMetadata(
    const KernelUbMetadataMap& kernels, std::string_view name, std::optional<uint64_t> entry)
{
    if (name.empty() && !entry.has_value()) {
        return kernels.size() == 1U ? &kernels.begin()->second.launch : nullptr;
    }
    enum class Priority { DEFAULT, VECTOR };
    const auto logicalName = LogicalKernelName(name);
    std::string_view selectedName;
    const AclsanUbLaunchMetadata* selected = nullptr;
    auto priority = Priority::DEFAULT;
    for (const auto& kernel : kernels) {
        const auto base = LogicalKernelName(kernel.first);
        if ((!logicalName.empty() && base != logicalName) ||
            (entry.has_value() && kernel.second.functionEntry != entry)) {
            continue;
        }
        const auto candidatePriority = EndsWith(kernel.first, "_mix_aiv") ? Priority::VECTOR : Priority::DEFAULT;
        if (selected != nullptr && (selectedName != base || candidatePriority == priority)) {
            return nullptr;
        }
        if (selected == nullptr || candidatePriority > priority) {
            selectedName = base;
            selected = &kernel.second.launch;
            priority = candidatePriority;
        }
    }
    if (selected == nullptr && !logicalName.empty() && entry.has_value()) {
        return FindUbMetadata(kernels, {}, entry);
    }
    return selected;
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
        std::shared_ptr<NpubitUbHandle> hibit;
        uint64_t binaryId = 0;
        bool instrumented = false;
        uint32_t traceArgumentOffset = 0;
        int32_t deviceId = DeviceBinaryRegistry::UNKNOWN_DEVICE;
        std::string sessionDirectory;
        std::string metadataImage;
        std::unique_ptr<DeviceSymbolizer> symbolizer;
        std::string symbolizerError = "invalid_state";
        KernelUbMetadataMap ubMetadata;
        std::vector<BinarySegment> actualSegments;
    };

    static void InitializeSymbolizer(BinaryEntry& entry, const void* image, size_t imageBytes)
    {
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

    struct LoadedImage {
        std::vector<BinarySegment> segments;
    };

    std::vector<std::weak_ptr<NpubitUbHandle>> hibitObjects;

    struct ImageEntry {
        int32_t deviceId = DeviceBinaryRegistry::UNKNOWN_DEVICE;
        std::vector<BinarySectionRange> sections;
    };

    using Key = std::pair<int32_t, uintptr_t>;
    mutable std::mutex mutex;
    uint64_t nextBinaryId = 1;
    std::unordered_map<int32_t, uintptr_t> latestBinaries;
    std::map<Key, BinaryEntry> binaries;
    std::map<Key, Key> functionBinaries;
    std::map<Key, std::string> functionNames;
    std::map<Key, uint64_t> functionEntries;
    std::map<Key, KernelUbMetadata> functionUbMetadata;
    std::map<Key, ImageEntry> readOnlySections;
    std::map<Key, LoadedImage> actualSegments;
    std::map<Key, KernelUbMetadataMap> pendingUbMetadata;

    static void ApplyLoadedUbMetadata(KernelUbMetadataMap& original, const KernelUbMetadataMap& loaded)
    {
        for (auto& kernel : original) {
            const auto final = loaded.find(kernel.first);
            if (final != loaded.end()) {
                kernel.second = final->second;
            } else {
                kernel.second.launch.flags |= static_cast<uint32_t>(ACLSAN_UB_METADATA_UNAVAILABLE);
            }
        }
    }

    template <typename Map>
    static auto Find(Map& entries, int32_t deviceId, uintptr_t handle)
    {
        if (deviceId != DeviceBinaryRegistry::UNKNOWN_DEVICE) {
            return entries.find({deviceId, handle});
        }
        auto found = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->first.second != handle) {
                continue;
            }
            if (found != entries.end()) {
                TraceIntegrityMark(0, 0, true);
                return entries.end();
            }
            found = it;
        }
        return found;
    }

    bool HasFunctionConflict(Key unknown) const
    {
        for (const auto& [function, owner] : functionBinaries) {
            if (owner != unknown) {
                continue;
            }
            for (const auto& [other, otherOwner] : functionBinaries) {
                (void)otherOwner;
                if (other.second == function.second && other != function) {
                    return true;
                }
            }
        }
        return false;
    }

    void AdoptOwnedFunctions(Key unknown, Key adopted)
    {
        for (auto it = functionBinaries.begin(); it != functionBinaries.end();) {
            if (it->second != unknown) {
                ++it;
                continue;
            }
            auto current = it++;
            const Key oldFunction = current->first;
            const Key newFunction{adopted.first, oldFunction.second};
            auto functionNode = functionBinaries.extract(current);
            functionNode.key() = newFunction;
            functionNode.mapped() = adopted;
            functionBinaries.insert(std::move(functionNode));
            auto metadataNode = functionUbMetadata.extract(oldFunction);
            if (!metadataNode.empty()) {
                metadataNode.key() = newFunction;
                functionUbMetadata.insert(std::move(metadataNode));
            }
            auto nameNode = functionNames.extract(oldFunction);
            if (!nameNode.empty()) {
                nameNode.key() = newFunction;
                functionNames.insert(std::move(nameNode));
            }
            auto entryNode = functionEntries.extract(oldFunction);
            if (!entryNode.empty()) {
                entryNode.key() = newFunction;
                functionEntries.insert(std::move(entryNode));
            }
        }
    }

    bool AdoptBinary(int32_t deviceId, uintptr_t handle)
    {
        const Key unknown{DeviceBinaryRegistry::UNKNOWN_DEVICE, handle};
        auto binary = binaries.find(unknown);
        if (deviceId == DeviceBinaryRegistry::UNKNOWN_DEVICE || binary == binaries.end()) {
            return false;
        }
        for (const auto& [key, entry] : binaries) {
            (void)entry;
            if (key.second == handle && key != unknown) {
                TraceIntegrityMark(0, 0, true);
                return false;
            }
        }
        for (const auto& [key, entry] : readOnlySections) {
            (void)entry;
            if (key.second == handle && key != unknown) {
                TraceIntegrityMark(0U, 0U, true);
                return false;
            }
        }
        const Key adopted{deviceId, handle};
        if (HasFunctionConflict(unknown)) {
            TraceIntegrityMark(0, 0, true);
            return false;
        }
        const auto latest = latestBinaries.find(DeviceBinaryRegistry::UNKNOWN_DEVICE);
        if (latest != latestBinaries.end() && latest->second == handle) {
            latestBinaries.insert_or_assign(deviceId, handle);
            latestBinaries.erase(DeviceBinaryRegistry::UNKNOWN_DEVICE);
        }
        auto binaryNode = binaries.extract(binary);
        binaryNode.key() = adopted;
        binaryNode.mapped().deviceId = deviceId;
        binaries.insert(std::move(binaryNode));
        auto imageNode = readOnlySections.extract(unknown);
        if (!imageNode.empty()) {
            imageNode.key() = adopted;
            imageNode.mapped().deviceId = deviceId;
            readOnlySections.insert(std::move(imageNode));
        }
        auto actualNode = actualSegments.extract(unknown);
        if (!actualNode.empty()) {
            actualNode.key() = adopted;
            actualSegments.insert(std::move(actualNode));
        }
        AdoptOwnedFunctions(unknown, adopted);
        return true;
    }

    void AdoptFunction(int32_t deviceId, uintptr_t function)
    {
        const auto unknown = functionBinaries.find({DeviceBinaryRegistry::UNKNOWN_DEVICE, function});
        if (deviceId != DeviceBinaryRegistry::UNKNOWN_DEVICE && unknown != functionBinaries.end()) {
            (void)AdoptBinary(deviceId, unknown->second.second);
        }
    }

    auto FindLatest(int32_t deviceId) const
    {
        if (deviceId == DeviceBinaryRegistry::UNKNOWN_DEVICE && latestBinaries.size() != 1U) {
            if (!latestBinaries.empty()) {
                TraceIntegrityMark(0, 0, true);
            }
            return binaries.end();
        }
        const auto latest =
            deviceId == DeviceBinaryRegistry::UNKNOWN_DEVICE ? latestBinaries.begin() : latestBinaries.find(deviceId);
        return latest == latestBinaries.end() ? binaries.end() : binaries.find({latest->first, latest->second});
    }

    uint64_t AllocateBinaryId() noexcept
    {
        if (nextBinaryId == 0) {
            return 0;
        }
        return nextBinaryId++;
    }

    void RemoveLatestBinary(Key binary) noexcept
    {
        const auto latest = latestBinaries.find(binary.first);
        if (latest != latestBinaries.end() && latest->second == binary.second) {
            latestBinaries.erase(latest);
        }
    }

    void RemoveOwnedFunctions(Key binary) noexcept
    {
        for (auto it = functionBinaries.begin(); it != functionBinaries.end();) {
            if (it->second == binary) {
                functionNames.erase(it->first);
                functionEntries.erase(it->first);
                functionUbMetadata.erase(it->first);
                it = functionBinaries.erase(it);
            } else {
                ++it;
            }
        }
    }

    void RecordUbMetadata(
        Key key, uintptr_t binaryHandle, const BinaryEntry& binary, const char* functionName, const uint64_t* entry)
    {
        KernelUbMetadata metadata;
        std::string diagnostic;
        if (GetKernelUbMetadata(
                binary.metadataImage, functionName == nullptr ? "" : functionName, metadata, diagnostic, entry)) {
            functionUbMetadata[key] = metadata;
        } else {
            ASCTOOL_DEBUG(
                "acl_san UB metadata unavailable: function=0x%llx binary=0x%llx name=%s byEntry=%u entry=%llu "
                "reason=%s",
                static_cast<unsigned long long>(key.second), static_cast<unsigned long long>(binaryHandle),
                functionName != nullptr ? functionName : "", static_cast<unsigned>(entry != nullptr),
                static_cast<unsigned long long>(entry != nullptr ? *entry : 0), diagnostic.c_str());
        }
    }

    void RemoveFunctionOwnership(Key function) noexcept
    {
        functionBinaries.erase(function);
        functionNames.erase(function);
        functionEntries.erase(function);
        functionUbMetadata.erase(function);
    }
};

DeviceBinaryRegistry::DeviceBinaryRegistry() : impl_(std::make_unique<Impl>()) {}

DeviceBinaryRegistry::~DeviceBinaryRegistry() { Reset(); }

bool DeviceBinaryRegistry::RecordBinaryLoadFromData(
    uintptr_t binary, bool instrumented, uint32_t traceArgumentOffset, const void* image, size_t imageBytes,
    int32_t deviceId) noexcept
{
    if (binary == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Impl::BinaryEntry entry;
        entry.binaryId = impl_->AllocateBinaryId();
        if (entry.binaryId == 0) {
            return false;
        }
        entry.instrumented = instrumented;
        entry.traceArgumentOffset = traceArgumentOffset;
        entry.deviceId = deviceId;
        if (image != nullptr && imageBytes != 0U) {
            std::string diagnostic;
            (void)GetKernelUbMetadata(
                std::string(static_cast<const char*>(image), imageBytes), entry.ubMetadata, diagnostic);
        }
        const Impl::Key metadataKey{deviceId, binary};
        const auto loadedUb = impl_->pendingUbMetadata.find(metadataKey);
        if (loadedUb != impl_->pendingUbMetadata.end()) {
            if (instrumented) {
                Impl::ApplyLoadedUbMetadata(entry.ubMetadata, loadedUb->second);
            }
            impl_->pendingUbMetadata.erase(loadedUb);
        }
        const auto actual = Impl::Find(impl_->actualSegments, deviceId, binary);
        if (instrumented && actual != impl_->actualSegments.end()) {
            entry.actualSegments = std::move(actual->second.segments);
        }
        if (actual != impl_->actualSegments.end()) {
            impl_->actualSegments.erase(actual);
        }
        if (image != nullptr && imageBytes != 0) {
            entry.metadataImage.assign(static_cast<const char*>(image), imageBytes);
        }
        if (instrumented) {
            for (auto it = impl_->hibitObjects.begin(); it != impl_->hibitObjects.end();) {
                auto handle = it->lock();
                if (!handle) {
                    it = impl_->hibitObjects.erase(it);
                    continue;
                }
                if (handle->Matches(image, imageBytes)) {
                    entry.hibit = std::move(handle);
                    break;
                }
                ++it;
            }
            if (!entry.hibit) {
                entry.hibit = NpubitUbHandle::Create(image, imageBytes);
                if (entry.hibit) {
                    impl_->hibitObjects.push_back(entry.hibit);
                }
            }
            Impl::InitializeSymbolizer(entry, image, imageBytes);
        }
        if (!instrumented) {
            (void)ParseLoadSegments(image, imageBytes, entry.actualSegments);
        }

        const Impl::Key key{deviceId, binary};
        const auto existing = impl_->binaries.find(key);
        if (existing != impl_->binaries.end()) {
            RemoveDirectory(existing->second.sessionDirectory);
            impl_->RemoveOwnedFunctions(key);
            impl_->RemoveLatestBinary(key);
        }
        impl_->binaries[key] = std::move(entry);
        if (!instrumented) {
            impl_->readOnlySections.erase(key);
        }
        impl_->latestBinaries[deviceId] = binary;
        return impl_->binaries[key].symbolizer != nullptr || !instrumented;
    } catch (...) {
        return false;
    }
}

bool DeviceBinaryRegistry::HasKnownBinaryHandle(uintptr_t binary) const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& [key, entry] : impl_->binaries) {
            (void)entry;
            if (key.second == binary && key.first != UNKNOWN_DEVICE) {
                return true;
            }
        }
        for (const auto& [key, entry] : impl_->readOnlySections) {
            (void)entry;
            if (key.second == binary && key.first != UNKNOWN_DEVICE) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return true;
    }
}

void DeviceBinaryRegistry::RecordBinaryUnload(uintptr_t binary, int32_t deviceId) noexcept
{
    if (binary == 0) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto actual = Impl::Find(impl_->actualSegments, deviceId, binary);
        if (actual != impl_->actualSegments.end()) {
            impl_->actualSegments.erase(actual);
        }
        auto found = Impl::Find(impl_->binaries, deviceId, binary);
        if (found == impl_->binaries.end() && deviceId != UNKNOWN_DEVICE && impl_->AdoptBinary(deviceId, binary)) {
            found = impl_->binaries.find({deviceId, binary});
        }
        if (found == impl_->binaries.end()) {
            return;
        }
        const Impl::Key key = found->first;
        impl_->readOnlySections.erase(key);
        impl_->pendingUbMetadata.erase(key);
        RemoveDirectory(found->second.sessionDirectory);
        impl_->RemoveLatestBinary(key);
        impl_->binaries.erase(found);
        impl_->RemoveOwnedFunctions(key);
    } catch (...) {
    }
}

void DeviceBinaryRegistry::RecordBinaryFunctionLookup(
    uintptr_t binary, uintptr_t function, const char* functionName, std::optional<uint64_t> functionEntry,
    int32_t deviceId) noexcept
{
    if (binary == 0 || function == 0) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto loaded = Impl::Find(impl_->binaries, deviceId, binary);
        if (loaded == impl_->binaries.end() && deviceId != UNKNOWN_DEVICE && impl_->AdoptBinary(deviceId, binary)) {
            loaded = impl_->binaries.find({deviceId, binary});
        }
        if (loaded != impl_->binaries.end()) {
            const Impl::Key key{loaded->first.first, function};
            impl_->RemoveFunctionOwnership(key);
            impl_->functionBinaries[key] = loaded->first;
            if (functionName != nullptr) {
                impl_->functionNames[key] = functionName;
            }
            if (functionEntry.has_value()) {
                impl_->functionEntries[key] = *functionEntry;
            }
            const uint64_t* entry = functionEntry.has_value() ? &*functionEntry : nullptr;
            impl_->RecordUbMetadata(key, binary, loaded->second, functionName, entry);
        } else {
            for (const auto& [key, entry] : impl_->binaries) {
                if (key.second == binary && entry.instrumented) {
                    TraceIntegrityMark(0U, 0U, true);
                    break;
                }
            }
        }
    } catch (...) {
        TraceIntegrityMark(0U, 0U, true);
    }
}

bool DeviceBinaryRegistry::GetFunctionUbMetadata(
    uintptr_t function, KernelUbMetadata& metadata, int32_t deviceId) const noexcept
{
    metadata = {};
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = Impl::Find(impl_->functionUbMetadata, deviceId, function);
        if (found == impl_->functionUbMetadata.end()) {
            return false;
        }
        metadata = found->second;
        return true;
    } catch (...) {
        return false;
    }
}

void DeviceBinaryRegistry::RecordLatestBinaryFunctionLookup(uintptr_t function, int32_t deviceId) noexcept
{
    if (function == 0) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (deviceId == UNKNOWN_DEVICE && impl_->latestBinaries.size() != 1U) {
            if (!impl_->latestBinaries.empty()) {
                TraceIntegrityMark(0, 0, true);
            }
            return;
        }
        const auto latest =
            deviceId == UNKNOWN_DEVICE ? impl_->latestBinaries.begin() : impl_->latestBinaries.find(deviceId);
        if (latest != impl_->latestBinaries.end()) {
            const Impl::Key binary{latest->first, latest->second};
            const Impl::Key key{latest->first, function};
            if (impl_->binaries.find(binary) != impl_->binaries.end()) {
                impl_->RemoveFunctionOwnership(key);
                impl_->functionBinaries[key] = binary;
            }
        }
    } catch (...) {
    }
}

bool DeviceBinaryRegistry::RecordLoadedImage(
    uintptr_t binary, const void* image, size_t bytes, int32_t deviceId) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const Impl::Key key{deviceId, binary};
        impl_->readOnlySections.erase(key);
        impl_->actualSegments.erase(key);
        if (binary == 0 || image == nullptr || bytes == 0) {
            return false;
        }
        std::vector<BinarySectionRange> sections;
        std::string diagnostic;
        KernelUbMetadataMap loadedMetadata;
        (void)GetKernelUbMetadata(std::string(static_cast<const char*>(image), bytes), loadedMetadata, diagnostic);
        const auto entry = impl_->binaries.find(key);
        if (entry != impl_->binaries.end()) {
            Impl::ApplyLoadedUbMetadata(entry->second.ubMetadata, loadedMetadata);
        } else {
            impl_->pendingUbMetadata[key] = std::move(loadedMetadata);
        }
        const bool sectionsValid =
            GetReadOnlyBinarySections(std::string(static_cast<const char*>(image), bytes), sections, diagnostic);
        if (sectionsValid) {
            impl_->readOnlySections[key] = {deviceId, std::move(sections)};
        }
        std::vector<BinarySegment> actual;
        if (ParseLoadSegments(image, bytes, actual)) {
            impl_->actualSegments[key] = {std::move(actual)};
        }
        return sectionsValid;
    } catch (...) {
        return false;
    }
}

bool DeviceBinaryRegistry::GetFunctionReadOnlySections(
    uintptr_t function, uintptr_t& binary, std::vector<BinarySectionRange>& sections, int32_t deviceId) const
{
    binary = 0;
    sections.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto owner = Impl::Find(impl_->functionBinaries, deviceId, function);
    if (owner == impl_->functionBinaries.end()) {
        return false;
    }
    const auto found = impl_->readOnlySections.find(owner->second);
    if (found == impl_->readOnlySections.end() || impl_->binaries.find(owner->second) == impl_->binaries.end()) {
        return false;
    }
    binary = owner->second.second;
    sections = found->second.sections;
    return true;
}

bool DeviceBinaryRegistry::GetFunctionActualBinary(
    uintptr_t function, uintptr_t& binary, std::vector<BinarySegment>& segments, int32_t deviceId) const noexcept
{
    binary = 0;
    segments.clear();
    if (function == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto owner = Impl::Find(impl_->functionBinaries, deviceId, function);
        if (owner == impl_->functionBinaries.end()) {
            return false;
        }
        const auto loaded = impl_->binaries.find(owner->second);
        if (loaded == impl_->binaries.end() || loaded->second.actualSegments.empty()) {
            return false;
        }
        binary = owner->second.second;
        segments = loaded->second.actualSegments;
        return true;
    } catch (...) {
        return false;
    }
}

bool DeviceBinaryRegistry::GetFunctionName(
    uintptr_t function, std::string& functionName, int32_t deviceId) const noexcept
{
    if (function == 0) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = Impl::Find(impl_->functionNames, deviceId, function);
        if (found == impl_->functionNames.end()) {
            return false;
        }
        functionName = found->second;
        return true;
    } catch (...) {
        return false;
    }
}

bool DeviceBinaryRegistry::GetFunctionUbMetadata(
    uintptr_t function, AclsanUbLaunchMetadata& metadata, int32_t deviceId) const noexcept
{
    metadata = {};
    if (function == 0U) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto owner = Impl::Find(impl_->functionBinaries, deviceId, function);
        if (owner == impl_->functionBinaries.end()) {
            return false;
        }
        const auto entry = impl_->binaries.find(owner->second);
        if (entry == impl_->binaries.end()) {
            return false;
        }
        const auto name = impl_->functionNames.find(owner->first);
        const auto offset = impl_->functionEntries.find(owner->first);
        const auto* selected = FindUbMetadata(
            entry->second.ubMetadata, name == impl_->functionNames.end() ? std::string_view{} : name->second,
            offset == impl_->functionEntries.end() ? std::nullopt : std::optional<uint64_t>{offset->second});
        if (selected == nullptr) {
            return false;
        }
        metadata = *selected;
        return true;
    } catch (...) {
        return false;
    }
}

DeviceBinaryRegistry::TraceLookup DeviceBinaryRegistry::LookupFunctionTraceArgumentOffset(
    uintptr_t function, uint32_t& traceArgumentOffset, int32_t deviceId) const noexcept
{
    if (function == 0) {
        return TraceLookup::NotInstrumented;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (deviceId != UNKNOWN_DEVICE && impl_->functionBinaries.count({deviceId, function}) == 0U) {
            impl_->AdoptFunction(deviceId, function);
        }
        const auto ownership = Impl::Find(impl_->functionBinaries, deviceId, function);
        if (ownership != impl_->functionBinaries.end()) {
            const auto binary = impl_->binaries.find(ownership->second);
            if (binary == impl_->binaries.end()) {
                return TraceLookup::Unresolved;
            }
            if (!binary->second.instrumented) {
                return TraceLookup::NotInstrumented;
            }
            if (binary->second.traceArgumentOffset == 0U) {
                return TraceLookup::Unresolved;
            }
            traceArgumentOffset = binary->second.traceArgumentOffset;
            return TraceLookup::Instrumented;
        }
        for (const auto& [key, owner] : impl_->functionBinaries) {
            if (key.second == function) {
                const auto binary = impl_->binaries.find(owner);
                if (binary == impl_->binaries.end() || binary->second.instrumented) {
                    return TraceLookup::Unresolved;
                }
            }
        }
        return TraceLookup::NotInstrumented;
    } catch (...) {
        return TraceLookup::Unresolved;
    }
}

bool DeviceBinaryRegistry::GetFunctionTraceArgumentOffset(
    uintptr_t function, uint32_t& traceArgumentOffset, int32_t deviceId) const noexcept
{
    return LookupFunctionTraceArgumentOffset(function, traceArgumentOffset, deviceId) == TraceLookup::Instrumented;
}

size_t DeviceBinaryRegistry::ResetDevice(int32_t deviceId) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        size_t unknownEntries = 0;
        for (auto it = impl_->binaries.begin(); it != impl_->binaries.end();) {
            if (it->second.deviceId == UNKNOWN_DEVICE) {
                ++unknownEntries;
            }
            if (deviceId != UNKNOWN_DEVICE && it->second.deviceId == deviceId) {
                RemoveDirectory(it->second.sessionDirectory);
                impl_->RemoveOwnedFunctions(it->first);
                impl_->RemoveLatestBinary(it->first);
                it = impl_->binaries.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = impl_->readOnlySections.begin(); it != impl_->readOnlySections.end();) {
            if (it->second.deviceId == UNKNOWN_DEVICE) {
                ++unknownEntries;
            }
            if (deviceId != UNKNOWN_DEVICE && it->second.deviceId == deviceId) {
                it = impl_->readOnlySections.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = impl_->actualSegments.begin(); it != impl_->actualSegments.end();) {
            if (it->first.first == UNKNOWN_DEVICE) {
                ++unknownEntries;
            }
            if (deviceId != UNKNOWN_DEVICE && it->first.first == deviceId) {
                it = impl_->actualSegments.erase(it);
            } else {
                ++it;
            }
        }
        return unknownEntries;
    } catch (...) {
        return std::numeric_limits<size_t>::max();
    }
}

std::shared_ptr<NpubitUbHandle> DeviceBinaryRegistry::GetFunctionHibitHandle(
    uintptr_t function, int32_t deviceId) const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto owner = Impl::Find(impl_->functionBinaries, deviceId, function);
        if (owner == impl_->functionBinaries.end()) {
            return {};
        }
        const auto binary = impl_->binaries.find(owner->second);
        return binary == impl_->binaries.end() ? nullptr : binary->second.hibit;
    } catch (...) {
        return {};
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
        impl_->pendingUbMetadata.clear();
        impl_->functionBinaries.clear();
        impl_->functionNames.clear();
        impl_->functionEntries.clear();
        impl_->functionUbMetadata.clear();
        impl_->latestBinaries.clear();
    } catch (...) {
    }
}

CallStackResult DeviceBinaryRegistry::ResolveCallStack(uint64_t pc, int32_t deviceId) const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto active = impl_->FindLatest(deviceId);
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
    uint64_t pc, const CommandRunner& runner, int32_t deviceId) const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto active = impl_->FindLatest(deviceId);
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
