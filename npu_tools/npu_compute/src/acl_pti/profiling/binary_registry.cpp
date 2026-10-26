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
    const auto resolve = reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(id));
    if (resolve == nullptr || found->second.companion == nullptr || function == nullptr) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    found->second.functions.erase(function);
    found->second.elfEntries.erase(function);
    aclrtFuncHandle companion = nullptr;
    Key companionKey = key;
    if constexpr (!std::is_convertible<Key, const char*>::value) {
        if (found->second.pmSamplingImage) {
            const auto& map = found->second.pmSamplingImage->instructions.relocatedEntries;
            const auto entry = map.find(key);
            if (entry == map.end()) {
                if (!found->second.pmSamplingImage->instructions.error.empty()) {
                    ASCTOOL_DEBUG("companion function entry unavailable after PmSampling mapping failure");
                    found->second.functions[function] = nullptr;
                    return ACLPTI_SUCCESS;
                }
                return ACLPTI_ERROR_PROFILING_FAILED;
            }
            companionKey = entry->second;
        }
    }
    const aclError status = resolve(found->second.companion, companionKey, &companion);
    if (status != ACL_SUCCESS || companion == nullptr) {
        found->second.functions.erase(function);
        ASCTOOL_ERROR(
            "[aclpti] error operation=companion_function status=%d"
            " code=%u",
            status, static_cast<unsigned>(static_cast<uint32_t>(status)));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    try {
        if (found->second.pmSamplingImage) {
            const auto& entries = found->second.pmSamplingImage->instructions.functionEntries;
            std::uint64_t entry = 0;
            if constexpr (std::is_convertible<Key, const char*>::value) {
                const auto named = key == nullptr ? entries.end() : entries.find(key);
                if (named == entries.end()) {
                    // A named Runtime lookup remains valid for Pipeline even when
                    // PmSampling could not recover the ELF function entry.
                    if (found->second.pmSamplingImage->instructions.error.empty()) {
                        return ACLPTI_ERROR_PROFILING_FAILED;
                    }
                } else {
                    entry = named->second;
                    found->second.elfEntries[function] = entry;
                }
            } else {
                // GetFunctionByEntry uses an original ELF entry; use the verified original/companion mapping.
                const auto& map = found->second.pmSamplingImage->instructions.relocatedEntries;
                const auto translated = map.find(key);
                if (translated == map.end()) {
                    return ACLPTI_ERROR_PROFILING_FAILED;
                }
                entry = translated->second;
                found->second.elfEntries[function] = entry;
            }
        }
        found->second.functions[function] = companion;
        ASCTOOL_DEBUG(
            "[aclpti] function mapping registered: binary=%p original=%p companion=%p", binary, function, companion);
    } catch (const std::bad_alloc&) {
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult BinaryRegistry::RegisterBinary(
    const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary, bool pmSampling)
{
    return RegisterInstrumentedBinary(data, size, options, binary, pmSampling);
}

aclptiResult BinaryRegistry::RegisterBinaryFromFile(
    const char* path, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary, bool pmSampling)
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
        return RegisterInstrumentedBinary(data.data(), data.size(), options, binary, pmSampling);
    } catch (const std::bad_alloc&) {
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
}

aclptiResult BinaryRegistry::RegisterInstrumentedBinary(
    const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary, bool pmSampling)
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
            return existing->second.companion != nullptr && (!pmSampling || existing->second.pmSamplingImage) ?
                       ACLPTI_SUCCESS :
                       ACLPTI_ERROR_PROFILING_FAILED;
        }
        std::vector<char> patched;
        auto image = pmSampling ? std::make_shared<PmSamplingBinaryImage>() : nullptr;
        std::uint32_t traceArgumentOffset = 0;
        if (!InstrumentKernelEnd(data, size, patched, &traceArgumentOffset, image ? &image->instructions : nullptr) ||
            patched.empty() || traceArgumentOffset == 0 || traceArgumentOffset % sizeof(void*) != 0) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        Binary state;
        state.traceArgumentOffset = traceArgumentOffset;
        if (image) {
            if (image->instructions.error.empty() &&
                (image->instructions.business.empty() || image->instructions.functionEntries.empty())) {
                image->instructions.error = "PmSampling instruction mapping is incomplete";
            }
            image->originalElf.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
            image->companionElf = std::move(patched);
            state.pmSamplingImage = image;
            state.instrumentedImage = std::shared_ptr<const std::vector<char>>(image, &image->companionElf);
        } else {
            state.instrumentedImage = std::make_shared<const std::vector<char>>(std::move(patched));
        }
        const auto inserted = binaries_.emplace(binary, std::move(state));
        if (!inserted.second) {
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        auto& registered = inserted.first->second;
        const aclError status = load(
            registered.instrumentedImage->data(), registered.instrumentedImage->size(), options, &registered.companion);
        if (status != ACL_SUCCESS || registered.companion == nullptr) {
            ASCTOOL_ERROR(
                "[aclpti] error operation=companion_load status=%d code=%u", status,
                static_cast<unsigned>(static_cast<uint32_t>(status)));
            binaries_.erase(inserted.first);
            return ACLPTI_ERROR_PROFILING_FAILED;
        }
        return ACLPTI_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
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

aclptiResult BinaryRegistry::RegisterSymbolFunction(aclrtFuncHandle function, bool pmSampling)
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
            bool registered = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                registered = binaries_.count(binary) != 0;
            }
            if (!registered) {
                // Some binaries are loaded before hooks are enabled. Only accept a complete, copied ELF.
                using GetBinaryAddress = aclError (*)(aclrtBinHandle, void**, std::size_t*);
                const auto getAddress =
                    reinterpret_cast<GetBinaryAddress>(dlsym(RTLD_DEFAULT, "aclrtBinaryGetDevAddress"));
                const auto copy =
                    reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
                void* address = nullptr;
                std::size_t size = 0;
                if (!getAddress || !copy || getAddress(binary, &address, &size) != ACL_SUCCESS || address == nullptr ||
                    size < sizeof(Elf64_Ehdr)) {
                    return ACLPTI_ERROR_PROFILING_FAILED;
                }
                std::vector<char> image(size);
                if (copy(image.data(), image.size(), address, size, ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS ||
                    std::memcmp(image.data(), ELFMAG, SELFMAG) != 0) {
                    ASCTOOL_DEBUG(
                        "PmSampling preloaded binary has no recoverable ELF: binary=%p size=%zu", binary, size);
                    return ACLPTI_ERROR_PROFILING_FAILED;
                }
                const auto status = RegisterBinary(image.data(), image.size(), nullptr, binary, true);
                if (status != ACLPTI_SUCCESS) {
                    return status;
                }
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
        const auto found = binary.second.functions.find(original);
        if (found != binary.second.functions.end()) {
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
        snapshot.error = snapshot.image->instructions.error;
        if (entry == binary.second.elfEntries.end()) {
            if (snapshot.error.empty()) {
                return false;
            }
            snapshot.error = "cannot resolve companion ELF function entry: " + snapshot.error;
            return true;
        }
        snapshot.elfEntry = entry->second;
        if (!snapshot.error.empty())
            return true;
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
        for (const auto& mapping : binary.second.functions) {
            if (mapping.second == function) {
                offset = binary.second.traceArgumentOffset;
                return offset != 0;
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
    if (found == binaries_.end() || found->second.companion == nullptr) {
        return ACLPTI_SUCCESS;
    }
    const auto unload =
        reinterpret_cast<aclrtBinaryUnLoadFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryUnLoad));
    const aclError status = unload == nullptr ? ACL_ERROR_INTERNAL_ERROR : unload(found->second.companion);
    if (status != ACL_SUCCESS) {
        ASCTOOL_ERROR(
            "[aclpti] error operation=companion_unload status=%d"
            " code=%u",
            status, static_cast<unsigned>(static_cast<uint32_t>(status)));
        context.lock_.unlock();
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    found->second.companion = nullptr;
    found->second.functions.clear();
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
