/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "runtime/kernel_metadata_collector.h"

#include <array>
#include <limits>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fstream>
#include <string_view>

namespace npucompute {
namespace {

constexpr uint32_t KERNEL_TYPE_METADATA_TAG = 1U;
constexpr uint32_t MIX_RATIO_METADATA_TAG = 3U;
constexpr uint32_t CUBE_KERNEL_TYPE = 2U;
constexpr uint32_t VECTOR_KERNEL_TYPE = 3U;
constexpr uint32_t MIX_CUBE_MAIN_KERNEL_TYPE = 4U;
constexpr uint32_t MIX_VECTOR_MAIN_KERNEL_TYPE = 5U;
constexpr uint32_t CUBE_ROLLBACK_KERNEL_TYPE = 6U;
constexpr uint32_t VECTOR_ROLLBACK_KERNEL_TYPE = 7U;
constexpr std::size_t SOC_NAME_PREFIX_BYTES = 11U;
constexpr std::size_t MIX_RATIO_COUNT = 2U;
constexpr uint16_t PURE_KERNEL_RATIO = 1U;
constexpr uint16_t DISABLED_KERNEL_RATIO = 0U;
constexpr uint16_t MIX_VECTOR_RATIO = 2U;

template <typename Value>
bool ReadElfValue(std::ifstream& input, uint64_t offset, uint64_t fileSize, Value& value)
{
    if (offset > fileSize || sizeof(value) > fileSize - offset) {
        return false;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

std::string ResolveSymbolName(const void* symbol)
{
    Dl_info info{};
    if (symbol == nullptr || ::dladdr(symbol, &info) == 0) {
        return {};
    }
    if (info.dli_saddr == symbol && info.dli_sname != nullptr) {
        return info.dli_sname;
    }
    if (info.dli_fname == nullptr) {
        return {};
    }
    std::ifstream input(info.dli_fname, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0) {
        return {};
    }
    const auto fileSize = static_cast<uint64_t>(input.tellg());
    Elf64_Ehdr header{};
    if (!ReadElfValue(input, 0, fileSize, header) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_shentsize != sizeof(Elf64_Shdr) || (header.e_type != ET_DYN && header.e_type != ET_EXEC) ||
        header.e_shoff > fileSize || header.e_shnum > (fileSize - header.e_shoff) / sizeof(Elf64_Shdr)) {
        return {};
    }
    const uint64_t address = reinterpret_cast<uintptr_t>(symbol) -
                             (header.e_type == ET_DYN ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0);
    for (uint16_t sectionIndex = 0; sectionIndex < header.e_shnum; ++sectionIndex) {
        Elf64_Shdr section{};
        if (!ReadElfValue(input, header.e_shoff + sectionIndex * sizeof(section), fileSize, section) ||
            (section.sh_type != SHT_SYMTAB && section.sh_type != SHT_DYNSYM) ||
            section.sh_entsize != sizeof(Elf64_Sym) || section.sh_link >= header.e_shnum ||
            section.sh_offset > fileSize || section.sh_size > fileSize - section.sh_offset) {
            continue;
        }
        Elf64_Shdr strings{};
        if (!ReadElfValue(input, header.e_shoff + section.sh_link * sizeof(strings), fileSize, strings) ||
            strings.sh_type != SHT_STRTAB || strings.sh_offset > fileSize ||
            strings.sh_size > fileSize - strings.sh_offset) {
            continue;
        }
        for (uint64_t index = 0; index < section.sh_size / sizeof(Elf64_Sym); ++index) {
            Elf64_Sym entry{};
            if (!ReadElfValue(input, section.sh_offset + index * sizeof(entry), fileSize, entry) ||
                entry.st_value != address || ELF64_ST_TYPE(entry.st_info) != STT_FUNC || entry.st_shndx == SHN_UNDEF ||
                entry.st_name >= strings.sh_size) {
                continue;
            }
            input.seekg(static_cast<std::streamoff>(strings.sh_offset + entry.st_name));
            std::string name;
            for (uint64_t remaining = strings.sh_size - entry.st_name; remaining > 0; --remaining) {
                char character = 0;
                if (!input.get(character)) {
                    break;
                }
                if (character == '\0') {
                    return name;
                }
                name += character;
            }
        }
    }
    return {};
}

std::optional<uint64_t> GridSize(const dim3& grid)
{
    uint64_t size = grid.x;
    for (const uint64_t dimension : {static_cast<uint64_t>(grid.y), static_cast<uint64_t>(grid.z)}) {
        if (dimension != 0 && size > std::numeric_limits<uint64_t>::max() / dimension) {
            return std::nullopt;
        }
        size *= dimension;
    }
    return size;
}

std::optional<std::string> ResolveRuntimeFunctionName(aclrtFuncHandle function)
{
    constexpr std::size_t kFunctionNameCapacity = 4096;
    std::array<char, kFunctionNameCapacity> name{};
    const aclError status = aclrtGetFunctionName(function, static_cast<uint32_t>(name.size()), name.data());
    if (status != ACL_SUCCESS || name.front() == '\0' || name.back() != '\0') {
        return std::nullopt;
    }
    return std::string(name.data());
}

template <typename Function>
Function OriginalRuntimeFunction(const char* name)
{
    aclrtApiFunc original = nullptr;
    return aclrtApiInjectionGetFunc(name, &original, nullptr) == ACL_SUCCESS ? reinterpret_cast<Function>(original) :
                                                                               nullptr;
}

// rtFunctionMetaType and payloads from CANN rt_external_base.h and kernel metadata:
// tag 1 is a uint32 kernel type; tag 3 is uint16 AIC ratio followed by uint16 AIV ratio.
template <typename Value>
std::optional<Value> FunctionMetadata(aclrtFuncHandle function, uint32_t tag)
{
    using GetSize = int32_t (*)(const void*, uint32_t, std::size_t*);
    using GetData = int32_t (*)(const void*, uint32_t, void*, uint32_t);
    const auto getSize = reinterpret_cast<GetSize>(::dlsym(RTLD_DEFAULT, "rtFunctionGetMetaInfoSize"));
    const auto getData = reinterpret_cast<GetData>(::dlsym(RTLD_DEFAULT, "rtFunctionGetMetaInfo"));
    std::size_t size = 0;
    Value value{};
    if (getSize == nullptr || getData == nullptr || getSize(function, tag, &size) != 0 || size != sizeof(Value) ||
        getData(function, tag, &value, sizeof(Value)) != 0) {
        return std::nullopt;
    }
    return value;
}

bool SupportsMixDimension(std::string_view soc)
{
    return soc.compare(0, SOC_NAME_PREFIX_BYTES, "Ascend950PR") == 0 ||
           soc.compare(0, SOC_NAME_PREFIX_BYTES, "Ascend950DT") == 0;
}

std::optional<uint64_t> MixDimension(uint64_t blocks, uint16_t aivRatio)
{
    if (blocks == 0 || aivRatio == 0 || blocks > std::numeric_limits<uint64_t>::max() / aivRatio) {
        return std::nullopt;
    }
    return blocks * aivRatio;
}

void CollectExecutionMetadata(aclrtFuncHandle function, KernelMetadata& metadata)
{
    if (function == nullptr) {
        return;
    }
    using GetAttribute = aclError (*)(const void*, aclrtFuncAttribute, int64_t*);
    const auto getAttribute = OriginalRuntimeFunction<GetAttribute>("aclrtGetFunctionAttribute");
    int64_t type = -1;
    if (getAttribute != nullptr && getAttribute(function, ACL_FUNC_ATTR_KERNEL_TYPE, &type) == ACL_SUCCESS) {
        if (type == ACL_KERNEL_TYPE_CUBE) {
            metadata.kind = KernelKind::Cube;
        } else if (type == ACL_KERNEL_TYPE_VECTOR) {
            metadata.kind = KernelKind::Vector;
        } else if (type == ACL_KERNEL_TYPE_MIX) {
            metadata.kind = KernelKind::Mix;
        }
    }
    const auto rawType = FunctionMetadata<uint32_t>(function, KERNEL_TYPE_METADATA_TAG);
    if (!metadata.kind) {
        if (rawType) {
            if (*rawType == CUBE_KERNEL_TYPE || *rawType == CUBE_ROLLBACK_KERNEL_TYPE) {
                metadata.kind = KernelKind::Cube;
            } else if (*rawType == VECTOR_KERNEL_TYPE || *rawType == VECTOR_ROLLBACK_KERNEL_TYPE) {
                metadata.kind = KernelKind::Vector;
            } else if (*rawType == MIX_CUBE_MAIN_KERNEL_TYPE || *rawType == MIX_VECTOR_MAIN_KERNEL_TYPE) {
                metadata.kind = KernelKind::Mix;
            }
        }
    }
    // Pure kernels need not contain MIX_TASK_RATION. Querying this absent tag
    // produces an EE1014 runtime error even though the kernel itself is valid.
    if (rawType && (*rawType == CUBE_KERNEL_TYPE || *rawType == VECTOR_KERNEL_TYPE ||
                    *rawType == CUBE_ROLLBACK_KERNEL_TYPE || *rawType == VECTOR_ROLLBACK_KERNEL_TYPE)) {
        return;
    }
    using GetSoc = const char* (*)();
    const auto getSoc = OriginalRuntimeFunction<GetSoc>("aclrtGetSocName");
    const char* soc = getSoc != nullptr ? getSoc() : nullptr;
    if (soc == nullptr || !SupportsMixDimension(soc)) {
        return;
    }
    // npu-compute supports 950 only; ACL_FUNC_ATTR_KERNEL_RATIO is unsupported on this product.
    const auto rawRatio = FunctionMetadata<std::array<uint16_t, MIX_RATIO_COUNT>>(function, MIX_RATIO_METADATA_TAG);
    if (!rawRatio) {
        return;
    }
    const uint16_t aicRatio = (*rawRatio)[0];
    const uint16_t aivRatio = (*rawRatio)[1];
    // The supported 950 ratios are {1,0}, {0,1}, {1,1} and {1,2}.
    // Pure kernels keep their launch Block Dim; only MIX has an additional AIV dimension.
    if (aicRatio == PURE_KERNEL_RATIO && aivRatio == DISABLED_KERNEL_RATIO) {
        metadata.kind = KernelKind::Cube;
    } else if (aicRatio == DISABLED_KERNEL_RATIO && aivRatio == PURE_KERNEL_RATIO) {
        metadata.kind = KernelKind::Vector;
    } else if (aicRatio == PURE_KERNEL_RATIO && (aivRatio == PURE_KERNEL_RATIO || aivRatio == MIX_VECTOR_RATIO)) {
        metadata.kind = KernelKind::Mix;
        if (metadata.blockDim) {
            metadata.mixBlockDim = MixDimension(*metadata.blockDim, aivRatio);
        }
    }
}

} // namespace

void KernelMetadataCollector::OnCallback(aclptiCallbackId cbid, const aclptiCallbackData& data)
{
    if (data.functionParams == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (frozen_) {
        return;
    }
    if (cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction ||
        cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry || cbid == ACLPTI_RUNTIME_CBID_aclrtGetFuncBySymbol) {
        aclrtBinHandle binHandle = nullptr;
        aclrtFuncHandle* funcHandle = nullptr;
        const char* kernelName = nullptr;
        std::string symbolName;
        if (cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunction) {
            const auto& lookup = *static_cast<const aclptiAclrtBinaryGetFunctionParams*>(data.functionParams);
            binHandle = lookup.binHandle;
            kernelName = lookup.kernelName;
            funcHandle = lookup.funcHandle;
        } else if (cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry) {
            const auto& lookup = *static_cast<const aclptiAclrtBinaryGetFunctionByEntryParams*>(data.functionParams);
            binHandle = lookup.binHandle;
            funcHandle = lookup.funcHandle;
            if (data.callbackSite == ACLPTI_API_ENTER) {
                // Entry lookup has no kernel name parameter; retain a stable diagnostic name for OpInfoSummary.
                symbolName = "entry_" + std::to_string(lookup.funcEntry);
                kernelName = symbolName.c_str();
            }
        } else {
            const auto& lookup = *static_cast<const aclptiAclrtGetFuncBySymbolParams*>(data.functionParams);
            funcHandle = lookup.funcHandle;
            if (data.callbackSite == ACLPTI_API_ENTER) {
                symbolName = ResolveSymbolName(lookup.symbol);
                kernelName = symbolName.empty() ? nullptr : symbolName.c_str();
            }
        }
        if (funcHandle == nullptr) {
            return;
        }
        if (data.callbackSite == ACLPTI_API_ENTER) {
            pendingNames_.erase(funcHandle);
            if (kernelName != nullptr) {
                pendingNames_[funcHandle] = {binHandle, kernelName};
            }
        } else if (data.callbackSite == ACLPTI_API_EXIT) {
            const auto pending = pendingNames_.find(funcHandle);
            if (pending != pendingNames_.end()) {
                if (data.retval == ACL_SUCCESS && *funcHandle != nullptr) {
                    auto resolved = pending->second;
                    if (cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryGetFunctionByEntry) {
                        const auto runtimeName = ResolveRuntimeFunctionName(*funcHandle);
                        if (runtimeName.has_value()) {
                            resolved.second = *runtimeName;
                        }
                    }
                    names_[*funcHandle] = std::move(resolved);
                }
                pendingNames_.erase(pending);
            }
        }
        return;
    }
    if (data.callbackSite == ACLPTI_API_EXIT) {
        if (launchPending_ && cbid == pendingLaunchCbid_) {
            if (data.retval == ACL_SUCCESS) {
                frozen_ = true;
            } else {
                metadata_ = {};
            }
            launchPending_ = false;
        }
        if (data.retval == ACL_SUCCESS && cbid == ACLPTI_RUNTIME_CBID_aclrtBinaryUnLoad) {
            const auto& params = *static_cast<const aclptiAclrtBinaryUnLoadParams*>(data.functionParams);
            for (auto iterator = names_.begin(); iterator != names_.end();) {
                if (iterator->second.first == params.binHandle) {
                    iterator = names_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }
        return;
    }
    if (data.callbackSite != ACLPTI_API_ENTER) {
        return;
    }
    aclrtFuncHandle handle = nullptr;
    KernelMetadata candidate;
    switch (cbid) {
        case ACLPTI_RUNTIME_CBID_aclrtLaunchKernel: {
            const auto& params = *static_cast<const aclptiAclrtLaunchKernelParams*>(data.functionParams);
            handle = params.funcHandle;
            candidate.blockDim = params.numBlocks;
            break;
        }
        case ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithHostArgs: {
            const auto& params = *static_cast<const aclptiAclrtLaunchKernelWithHostArgsParams*>(data.functionParams);
            handle = params.funcHandle;
            candidate.blockDim = params.numBlocks;
            break;
        }
        case ACLPTI_RUNTIME_CBID_aclrtLaunchKernelWithArgsArray: {
            const auto& params = *static_cast<const aclptiAclrtLaunchKernelWithArgsArrayParams*>(data.functionParams);
            handle = static_cast<aclrtFuncHandle>(params.func);
            candidate.blockDim = params.numBlocks;
            break;
        }
        case ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithHostArgs: {
            const auto& params =
                *static_cast<const aclptiAclrtLaunchSIMTKernelWithHostArgsParams*>(data.functionParams);
            handle = static_cast<aclrtFuncHandle>(params.func);
            candidate.blockDim = GridSize(params.gridDim);
            break;
        }
        case ACLPTI_RUNTIME_CBID_aclrtLaunchSIMTKernelWithArgsArray: {
            const auto& params =
                *static_cast<const aclptiAclrtLaunchSIMTKernelWithArgsArrayParams*>(data.functionParams);
            handle = static_cast<aclrtFuncHandle>(params.func);
            candidate.blockDim = GridSize(params.gridDim);
            break;
        }
        default:
            return;
    }
    int32_t deviceId = 0;
    if (aclrtGetDevice(&deviceId) == ACL_SUCCESS) {
        candidate.deviceId = deviceId;
    }
    const auto found = names_.find(handle);
    if (found != names_.end()) {
        candidate.name = found->second.second;
    }
    CollectExecutionMetadata(handle, candidate);
    metadata_ = std::move(candidate);
    pendingLaunchCbid_ = cbid;
    launchPending_ = true;
}

KernelMetadata KernelMetadataCollector::Snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    frozen_ = true;
    return metadata_;
}

} // namespace npucompute
