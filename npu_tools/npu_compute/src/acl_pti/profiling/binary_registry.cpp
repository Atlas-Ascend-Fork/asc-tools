/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npu_compute_log.h"
#include "binary_registry.h"
#include <elf.h>
#include <memory>
#include "binary_instrumenter.h"

#include <algorithm>
#include <fstream>
#include <cstring>
#include <dlfcn.h>
#include <type_traits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aclpti::profiling {

template <typename Function, typename Key>
aclptiResult BinaryRegistry::ResolveBinaryFunction(
    aclrtApiId id, aclrtBinHandle binary, Key key, aclrtFuncHandle function)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = binaries_.find(binary);
    if (found == binaries_.end()) {
        ASCTOOL_DEBUG("[aclpti] function mapping skipped: binary=%p function=%p is not registered", binary, function);
        return ACLPTI_SUCCESS;
    }
    auto status = ResolveFunctionState<Function>(id, binary, found->second, key, function);
    if (status == ACLPTI_SUCCESS && found->second.pipeline) {
        status = ResolveFunctionState<Function>(id, binary, *found->second.pipeline, key, function);
    }
    return status;
}

namespace {
bool FindNamedFunctionEntry(
    const std::map<std::string, std::uint64_t>& entries, const std::string& name, std::uint64_t& entry,
    std::string& alias)
{
    const auto named = entries.find(name);
    if (named != entries.end()) {
        entry = named->second;
        return true;
    }
    for (const auto* suffix : {"_mix_aic", "_aic"}) {
        const auto candidate = name + suffix;
        const auto found = entries.find(candidate);
        if (found != entries.end()) {
            alias = candidate;
            entry = found->second;
            return true;
        }
    }
    return false;
}

bool ReadRuntimeFunctionName(aclrtFuncHandle function, std::string& name)
{
    char buffer[4096]{};
    if (aclrtGetFunctionName(function, sizeof(buffer), buffer) != ACL_SUCCESS || buffer[0] == '\0' ||
        buffer[sizeof(buffer) - 1] != '\0') {
        return false;
    }
    name = buffer;
    return true;
}

template <typename Key>
bool ResolveOriginalEntry(
    const PmSamplingInstructionMap& instructions, Key key, aclrtFuncHandle function, std::uint64_t& entry)
{
    const auto& entries = instructions.functionEntries;
    bool resolved = false;
    std::string alias;
    if constexpr (std::is_convertible<Key, const char*>::value) {
        if (key == nullptr) {
            return false;
        }
        resolved = FindNamedFunctionEntry(entries, key, entry, alias);
    } else {
        const auto found = instructions.relocatedEntries.find(key);
        if (found != instructions.relocatedEntries.end()) {
            entry = found->second;
            resolved = true;
        } else {
            std::string name;
            if (!ReadRuntimeFunctionName(function, name)) {
                return false;
            }
            resolved = FindNamedFunctionEntry(entries, name, entry, alias);
        }
    }
    if (!resolved && entries.count("kernel") != 0) {
        entry = entries.at("kernel");
        resolved = true;
    }
    if (!resolved && !entries.empty()) {
        entry = entries.begin()->second;
        resolved = true;
    }
    return resolved;
}

template <typename Function, typename Key>
aclError ResolveCompanionHandle(
    Function resolve, aclrtBinHandle binary, Key key, aclrtFuncHandle& companion, std::string& mixedAlias)
{
    auto status = resolve(binary, key, &companion);
    if constexpr (std::is_convertible<Key, const char*>::value) {
        if ((status != ACL_SUCCESS || companion == nullptr) && key != nullptr) {
            // Mixed kernels launch through the AIC half, which carries dispatch metadata.
            const std::string logicalName(key);
            for (const auto* suffix : {"_mix_aic", "_aic"}) {
                mixedAlias = logicalName + suffix;
                companion = nullptr;
                status = resolve(binary, mixedAlias.c_str(), &companion);
                if (status == ACL_SUCCESS && companion != nullptr) {
                    break;
                }
            }
        }
    }
    return status;
}
} // namespace

template <typename Function, typename Key>
aclptiResult BinaryRegistry::ResolveFunctionState(
    aclrtApiId id, aclrtBinHandle binary, Binary& state, Key key, aclrtFuncHandle function)
{
    const auto resolve = reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(id));
    if (function == nullptr) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (state.pmSamplingImage && state.companion == nullptr) {
        std::uint64_t entry = 0;
        if (!ResolveOriginalEntry(state.pmSamplingImage->instructions, key, function, entry)) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        state.functions[function] = function;
        state.elfEntries[function] = entry;
        return ACLPTI_SUCCESS;
    }
    if (resolve == nullptr || state.companion == nullptr) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    state.functions.erase(function);
    state.elfEntries.erase(function);
    Key companionKey = key;
    std::uint64_t companionElfEntry = 0;
    bool unavailable = false;
    const auto keyStatus = ResolveCompanionKey(state, key, function, companionKey, companionElfEntry, unavailable);
    if (keyStatus != ACLPTI_SUCCESS || unavailable) {
        return keyStatus;
    }
    aclrtFuncHandle companion = nullptr;
    std::string mixedAlias;
    const auto status = ResolveCompanionHandle(resolve, state.companion, companionKey, companion, mixedAlias);
    if (status != ACL_SUCCESS || companion == nullptr) {
        state.functions.erase(function);
        ASCTOOL_ERROR(
            "[aclpti] error operation=companion_function status=%d"
            " code=%u",
            status, static_cast<unsigned>(static_cast<uint32_t>(status)));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    return RecordCompanionFunction(binary, state, key, function, companion, companionElfEntry, std::move(mixedAlias));
}

template <typename Key>
aclptiResult BinaryRegistry::ResolveCompanionKey(
    Binary& state, Key key, aclrtFuncHandle function, Key& companionKey, std::uint64_t& companionElfEntry,
    bool& unavailable)
{
    if constexpr (!std::is_convertible<Key, const char*>::value) {
        if (state.pmSamplingImage) {
            const auto& map = state.pmSamplingImage->instructions.relocatedEntries;
            const auto entry = map.find(key);
            if (entry == map.end()) {
                if (!state.pmSamplingImage->instructions.error.empty()) {
                    ASCTOOL_DEBUG("companion function entry unavailable after PmSampling mapping failure");
                    state.functions[function] = nullptr;
                    unavailable = true;
                    return ACLPTI_SUCCESS;
                }
                // ACLNN all-kernel binaries use a metadata functionEntry
                // (tiling key), rather than an ELF byte address, in this API.
                // Keep that Runtime key and recover the sampling PC from the
                // function Runtime actually selected in the original binary.
                std::string name;
                const auto& entries = state.pmSamplingImage->instructions.functionEntries;
                if (!ReadRuntimeFunctionName(function, name)) {
                    return ACLPTI_ERROR_PROFILING_FAILED;
                }
                const auto named = entries.find(name);
                if (named == entries.end()) {
                    return ACLPTI_ERROR_PROFILING_FAILED;
                }
                companionElfEntry = named->second;
            } else {
                companionKey = entry->second;
                companionElfEntry = entry->second;
            }
        }
    }
    return ACLPTI_SUCCESS;
}

template <typename Key>
aclptiResult BinaryRegistry::RecordCompanionFunction(
    aclrtBinHandle binary, Binary& state, Key key, aclrtFuncHandle function, aclrtFuncHandle companion,
    std::uint64_t companionElfEntry, std::string mixedAlias)
{
    try {
        if (state.pmSamplingImage) {
            const auto& entries = state.pmSamplingImage->instructions.functionEntries;
            std::uint64_t entry = 0;
            if constexpr (std::is_convertible<Key, const char*>::value) {
                const auto named = key == nullptr ? entries.end() : entries.find(key);
                if (named == entries.end() && mixedAlias.empty() && key != nullptr) {
                    FindNamedFunctionEntry(entries, key, entry, mixedAlias);
                }
                const auto aliased = mixedAlias.empty() ? entries.end() : entries.find(mixedAlias);
                if (named == entries.end() && aliased == entries.end()) {
                    // A named Runtime lookup remains valid for Pipeline even when
                    // PmSampling could not recover the ELF function entry.
                    if (state.pmSamplingImage->instructions.error.empty()) {
                        return ACLPTI_ERROR_PROFILING_FAILED;
                    }
                } else {
                    entry = named != entries.end() ? named->second : aliased->second;
                    state.elfEntries[function] = entry;
                }
            } else {
                entry = companionElfEntry;
                state.elfEntries[function] = entry;
            }
        }
        state.functions[function] = companion;
        ASCTOOL_DEBUG(
            "[aclpti] function mapping registered: binary=%p original=%p companion=%p", binary, function, companion);
    } catch (const std::bad_alloc&) {
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult BinaryRegistry::RegisterBinary(
    const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary, bool pmSampling,
    bool pipeline)
{
    return RegisterInstrumentedBinary(data, size, options, binary, pmSampling, pipeline);
}

aclptiResult BinaryRegistry::RegisterBinaryFromFile(
    const char* path, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary, bool pmSampling, bool pipeline)
{
    if (path == nullptr || path[0] == '\0' || binary == nullptr) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() <= 0) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        const auto size = static_cast<std::size_t>(input.tellg());
        std::vector<char> data(size);
        input.seekg(0);
        if (!input.read(data.data(), static_cast<std::streamsize>(data.size()))) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        return RegisterInstrumentedBinary(data.data(), data.size(), options, binary, pmSampling, pipeline);
    } catch (const std::bad_alloc&) {
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
}

aclptiResult BinaryRegistry::LoadCompanions(
    Binary& state, const aclrtBinaryLoadOptions* options, aclrtBinaryLoadFromDataFunc load)
{
    const aclError status =
        state.instrumentedImage ?
            load(state.instrumentedImage->data(), state.instrumentedImage->size(), options, &state.companion) :
            ACL_SUCCESS;
    if (status != ACL_SUCCESS || (state.instrumentedImage && state.companion == nullptr)) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=companion_load status=%d code=%u", status,
            static_cast<unsigned>(static_cast<uint32_t>(status)));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (state.pipeline) {
        auto& pipeline = *state.pipeline;
        const aclError pipelineStatus =
            load(pipeline.instrumentedImage->data(), pipeline.instrumentedImage->size(), options, &pipeline.companion);
        if (pipelineStatus != ACL_SUCCESS || pipeline.companion == nullptr) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=pipeline_companion_load status=%d code=%u", pipelineStatus,
                static_cast<unsigned>(static_cast<uint32_t>(pipelineStatus)));
            const auto unload =
                reinterpret_cast<aclrtBinaryUnLoadFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryUnLoad));
            if (unload != nullptr && state.companion != nullptr) {
                unload(state.companion);
            }
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
    }
    return ACLPTI_SUCCESS;
}

aclptiResult BinaryRegistry::RegisterInstrumentedBinary(
    const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary, bool pmSampling,
    bool pipeline)
{
    const auto load =
        reinterpret_cast<aclrtBinaryLoadFromDataFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryLoadFromData));
    if (load == nullptr || binary == nullptr || data == nullptr || size == 0) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    try {
        // Serialize the complete operation so two registrations cannot instrument/load the same handle twice.
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = binaries_.find(binary);
        if (existing != binaries_.end()) {
            const bool pipelineReady = existing->second.pipeline ? existing->second.pipeline->companion != nullptr :
                                                                   existing->second.companion != nullptr;
            return (existing->second.companion != nullptr || existing->second.pmSamplingImage) &&
                           (!pmSampling || existing->second.pmSamplingImage) && (!pipeline || pipelineReady) ?
                       ACLPTI_SUCCESS :
                       ACLPTI_ERROR_PROFILING_FAILED;
        }
        Binary state;
        if (!BuildInstrumentedBinaryState(state, data, size, pmSampling, pipeline)) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        const auto inserted = binaries_.emplace(binary, std::move(state));
        if (!inserted.second) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        auto& registered = inserted.first->second;
        const auto status = LoadCompanions(registered, options, load);
        if (status != ACLPTI_SUCCESS) {
            binaries_.erase(inserted.first);
        }
        return status;
    } catch (const std::bad_alloc&) {
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
}

bool BinaryRegistry::BuildInstrumentedBinaryState(
    Binary& state, const void* data, std::size_t size, bool pmSampling, bool pipeline)
{
    if (pmSampling) {
        BuildSamplingBinaryState(state, data, size);
    }
    if (!pmSampling && !pipeline) {
        return false;
    }
    return !pipeline || BuildPipelineBinaryState(state, data, size, pmSampling);
}

void BinaryRegistry::BuildSamplingBinaryState(Binary& state, const void* data, std::size_t size)
{
    auto image = std::make_shared<PmSamplingBinaryImage>();
    image->originalElf.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
    image->companionElf = image->originalElf;
    if (!BuildOriginalPmSamplingInstructionMap(data, size, image->instructions) && image->instructions.error.empty()) {
        image->instructions.error = "failed to map original PmSampling instructions";
    }
    if (image->instructions.error.empty() &&
        (image->instructions.business.empty() || image->instructions.functionEntries.empty())) {
        image->instructions.error = "PmSampling instruction mapping is incomplete";
    }
    BuildSamplingCompanion(state, image, data, size);
    state.pmSamplingImage = image;
}

void BinaryRegistry::BuildSamplingCompanion(
    Binary& state, const std::shared_ptr<PmSamplingBinaryImage>& image, const void* data, std::size_t size)
{
    if (image->instructions.error.empty() && !image->instructions.simt.empty()) {
        std::vector<char> samplingImage;
        std::uint32_t probeArgumentOffset = 0;
        PmSamplingInstructionMap samplingMap;
        if (InstrumentSimtSampling(data, size, samplingImage, &probeArgumentOffset, &samplingMap) &&
            !samplingImage.empty() && probeArgumentOffset != 0 && probeArgumentOffset % sizeof(void*) == 0) {
            if (samplingMap.error.empty() && samplingMap.functionEntries.empty()) {
                samplingMap.error = "PmSampling companion function entries are missing";
            }
            if (samplingMap.error.empty()) {
                image->instructions.functionEntries = std::move(samplingMap.functionEntries);
                image->instructions.relocatedEntries = std::move(samplingMap.relocatedEntries);
                image->companionElf = std::move(samplingImage);
                state.traceArgumentOffset = probeArgumentOffset;
                state.instrumentedImage = std::shared_ptr<const std::vector<char>>(image, &image->companionElf);
            } else {
                image->companionError = samplingMap.error;
            }
        } else {
            image->companionError =
                samplingMap.error.empty() ? "failed to build SIMT PmSampling companion" : samplingMap.error;
        }
        if (!image->companionError.empty()) {
            ASCTOOL_WARNING(
                "[aclpti] PmSampling companion unavailable; replaying original kernel: %s",
                image->companionError.c_str());
        }
    }
}

bool BinaryRegistry::BuildPipelineBinaryState(Binary& state, const void* data, std::size_t size, bool pmSampling)
{
    Binary* pipelineState = &state;
    if (pmSampling) {
        state.pipeline = std::make_unique<Binary>();
        pipelineState = state.pipeline.get();
    }
    std::shared_ptr<PmSamplingBinaryImage> pipelineMap;
    if (pmSampling) {
        pipelineMap = std::make_shared<PmSamplingBinaryImage>();
        pipelineMap->originalElf.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
    }
    std::vector<char> patched;
    std::uint32_t traceArgumentOffset = 0;
    if (!InstrumentKernelEnd(
            data, size, patched, &traceArgumentOffset, pipelineMap == nullptr ? nullptr : &pipelineMap->instructions) ||
        patched.empty() || traceArgumentOffset == 0 || traceArgumentOffset % sizeof(void*) != 0) {
        return false;
    }
    if (pmSampling) {
        pipelineMap->companionElf = patched;
        if (!pipelineMap->instructions.error.empty() || !pipelineMap->instructions.functionEntries.empty()) {
            pipelineState->pmSamplingImage = std::move(pipelineMap);
        }
    }
    pipelineState->traceArgumentOffset = traceArgumentOffset;
    pipelineState->instrumentedImage = std::make_shared<const std::vector<char>>(std::move(patched));
    return true;
}

aclptiResult BinaryRegistry::RegisterBinaryFunction(aclrtBinHandle binary, const char* name, aclrtFuncHandle function)
{
    return ResolveBinaryFunction<aclrtBinaryGetFunctionFunc>(ACL_RT_API_aclrtBinaryGetFunction, binary, name, function);
}

aclptiResult BinaryRegistry::RegisterBinaryFunction(
    aclrtBinHandle binary, std::uint64_t entry, aclrtFuncHandle function)
{
    return ResolveBinaryFunction<aclrtBinaryGetFunctionByEntryFunc>(
        ACL_RT_API_aclrtBinaryGetFunctionByEntry, binary, entry, function);
}

aclptiResult BinaryRegistry::EnsureSamplingBinary(aclrtBinHandle binary, bool pipeline)
{
    bool registered = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registered = binaries_.count(binary) != 0;
    }
    if (!registered) {
        // Some binaries are loaded before hooks are enabled. Only accept a complete, copied ELF.
        using GetBinaryAddress = aclError (*)(aclrtBinHandle, void**, std::size_t*);
        const auto getAddress = reinterpret_cast<GetBinaryAddress>(dlsym(RTLD_DEFAULT, "aclrtBinaryGetDevAddress"));
        const auto copy = reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
        void* address = nullptr;
        std::size_t size = 0;
        if (!getAddress || !copy || getAddress(binary, &address, &size) != ACL_SUCCESS || address == nullptr ||
            size < sizeof(Elf64_Ehdr)) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        std::vector<char> image(size);
        if (copy(image.data(), image.size(), address, size, ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS ||
            std::memcmp(image.data(), ELFMAG, SELFMAG) != 0) {
            ASCTOOL_DEBUG("PmSampling preloaded binary has no recoverable ELF: binary=%p size=%zu", binary, size);
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        const auto status = RegisterBinary(image.data(), image.size(), nullptr, binary, true, pipeline);
        if (status != ACLPTI_SUCCESS) {
            return status;
        }
    }
    return ACLPTI_SUCCESS;
}

aclptiResult BinaryRegistry::RegisterSymbolFunction(aclrtFuncHandle function, bool pmSampling, bool pipeline)
{
    // Symbol lookup returns a Runtime handle, not a name/entry in the companion.
    // Ask Runtime for ownership to avoid matching a same-named kernel in another binary.
    aclrtBinHandle binary = nullptr;
    const aclError binaryStatus = aclrtFunctionGetBinary(function, &binary);
    if (binaryStatus != ACL_SUCCESS || binary == nullptr) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=symbol_function_binary status=%d function=%p"
            " code=%u",
            binaryStatus, function, static_cast<unsigned>(static_cast<uint32_t>(binaryStatus)));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    constexpr std::size_t kFunctionNameCapacity = 4096;
    char name[kFunctionNameCapacity]{};
    const aclError nameStatus = aclrtGetFunctionName(function, sizeof(name), name);
    if (nameStatus != ACL_SUCCESS || name[0] == '\0' || name[sizeof(name) - 1] != '\0') {
        ASCTOOL_ERROR(
            "[aclpti] error operation=symbol_function_name status=%d function=%p"
            " code=%u",
            nameStatus, function, static_cast<unsigned>(static_cast<uint32_t>(nameStatus)));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    try {
        if (pmSampling) {
            const auto status = EnsureSamplingBinary(binary, pipeline);
            if (status != ACLPTI_SUCCESS) {
                return status;
            }
        }
        return RegisterBinaryFunction(binary, name, function);
    } catch (const std::bad_alloc&) {
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
}

aclrtFuncHandle BinaryRegistry::FindInstrumentedFunction(aclrtFuncHandle original)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& binary : binaries_) {
        if (binary.second.pmSamplingImage && !binary.second.pipeline) {
            continue;
        }
        const auto& state = binary.second.pipeline ? *binary.second.pipeline : binary.second;
        const auto found = state.functions.find(original);
        if (found != state.functions.end()) {
            ASCTOOL_DEBUG(
                "[aclpti] instrumented function mapping selected: binary=%p original=%p companion=%p", binary.first,
                original, found->second);
            return found->second;
        }
    }
    ASCTOOL_ERROR(
        "[aclpti] error operation=instrumented_function_lookup original=%p status=%d code=%u; "
        "enable Pipeline or PmSampling before binary load and obtain the function by name or entry",
        original, ACLPTI_ERROR_PROFILING_FAILED,
        static_cast<unsigned>(static_cast<uint32_t>(ACLPTI_ERROR_PROFILING_FAILED)));
    return nullptr;
}

aclrtFuncHandle BinaryRegistry::FindPmSamplingFunction(aclrtFuncHandle original)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& binary : binaries_) {
        const auto& state = binary.second;
        if (!state.pmSamplingImage) {
            continue;
        }
        const auto found = state.functions.find(original);
        if (found != state.functions.end()) {
            return found->second;
        }
    }
    return nullptr;
}

bool BinaryRegistry::NeedsPmSamplingProbeBuffer(aclrtFuncHandle function)
{
    if (function == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& binary : binaries_) {
        const auto& state = binary.second;
        if (!state.companion || !state.pmSamplingImage || state.pmSamplingImage->instructions.simt.empty()) {
            continue;
        }
        for (const auto& mapping : state.functions) {
            if (mapping.second == function) {
                return true;
            }
        }
    }
    return false;
}

bool BinaryRegistry::GetPmSamplingSnapshot(aclrtFuncHandle original, PmSamplingBinarySnapshot& snapshot)
{
    snapshot = {};
    using GetFunctionAddress = aclError (*)(const void*, void**, void**);
    const auto getAddress = reinterpret_cast<GetFunctionAddress>(dlsym(RTLD_DEFAULT, "aclrtGetFunctionAddr"));
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& binary : binaries_) {
        const auto function = binary.second.functions.find(original);
        const auto entry = binary.second.elfEntries.find(original);
        if (function == binary.second.functions.end() || !binary.second.pmSamplingImage) {
            continue;
        }
        snapshot.image = binary.second.pmSamplingImage;
        snapshot.companionError = snapshot.image->companionError;
        char kernelName[4096]{};
        if (aclrtGetFunctionName(original, sizeof(kernelName), kernelName) == ACL_SUCCESS && kernelName[0] != '\0' &&
            kernelName[sizeof(kernelName) - 1] == '\0') {
            snapshot.kernelName = kernelName;
        }
        snapshot.error = snapshot.image->instructions.error;
        if (entry == binary.second.elfEntries.end()) {
            if (snapshot.error.empty()) {
                return false;
            }
            snapshot.error = "cannot resolve companion ELF function entry: " + snapshot.error;
            return true;
        }
        snapshot.elfEntry = entry->second;
        for (const auto& relocation : snapshot.image->instructions.relocatedEntries) {
            if (relocation.second == snapshot.elfEntry) {
                snapshot.originalEntry = relocation.first;
                break;
            }
        }
        if (!snapshot.error.empty()) {
            return true;
        }
        void* aic = nullptr;
        void* aiv = nullptr;
        if (!getAddress || getAddress(function->second, &aic, &aiv) != ACL_SUCCESS || (!aic && !aiv)) {
            snapshot = {};
            return false;
        }
        snapshot.aicRuntimeEntry = reinterpret_cast<std::uint64_t>(aic);
        snapshot.aivRuntimeEntry = reinterpret_cast<std::uint64_t>(aiv);
        return true;
    }
    return false;
}

bool BinaryRegistry::GetInstrumentedTraceArgumentOffset(aclrtFuncHandle function, std::uint32_t& offset)
{
    offset = 0;
    if (function == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& binary : binaries_) {
        for (const Binary* state :
             {static_cast<const Binary*>(&binary.second), static_cast<const Binary*>(binary.second.pipeline.get())}) {
            if (state == nullptr) {
                continue;
            }
            for (const auto& mapping : state->functions) {
                if (mapping.second == function) {
                    offset = state->traceArgumentOffset;
                    return offset != 0;
                }
            }
        }
    }
    return false;
}

aclptiResult BinaryRegistry::PrepareBinaryUnload(aclrtBinHandle binary, UnloadContext& context)
{
    context.lock_ = std::unique_lock<std::mutex>(mutex_);
    context.binary_ = binary;
    const auto found = binaries_.find(binary);
    if (found == binaries_.end()) {
        return ACLPTI_SUCCESS;
    }
    for (Binary* state : {&found->second, found->second.pipeline.get()}) {
        if (state == nullptr || state->companion == nullptr) {
            continue;
        }
        const auto unload =
            reinterpret_cast<aclrtBinaryUnLoadFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryUnLoad));
        const aclError status = unload == nullptr ? ACL_ERROR_INTERNAL_ERROR : unload(state->companion);
        if (status != ACL_SUCCESS) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=companion_unload status=%d code=%u", status,
                static_cast<unsigned>(static_cast<uint32_t>(status)));
            context.lock_.unlock();
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        state->companion = nullptr;
        state->functions.clear();
        state->elfEntries.clear();
    }
    // Keep the lock across the handler's original unload and registry removal:
    // another load may receive the same Runtime handle as soon as unload succeeds.
    return ACLPTI_SUCCESS;
}

aclptiResult BinaryRegistry::CompleteBinaryUnload(UnloadContext& context)
{
    binaries_.erase(context.binary_);
    context.lock_.unlock();
    return ACLPTI_SUCCESS;
}
} // namespace aclpti::profiling
