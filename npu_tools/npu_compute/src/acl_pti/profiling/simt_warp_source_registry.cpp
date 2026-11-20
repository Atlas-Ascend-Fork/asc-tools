/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "simt_warp_source_registry.h"
#include <new>
#include <utility>

namespace aclpti::profiling {
bool SimtWarpSourceRegistry::RegisterBinary(
    aclrtBinHandle binary, const void* data, std::size_t size, const aclrtBinaryLoadOptions* options)
{
    if (binary == nullptr || data == nullptr || size == 0 ||
        (options != nullptr && options->numOpt != 0 && options->options == nullptr)) {
        return false;
    }
    try {
        Entry entry;
        const auto* begin = static_cast<const char*>(data);
        entry.source.image = std::make_shared<const std::vector<char>>(begin, begin + size);
        if (options != nullptr && options->numOpt != 0) {
            entry.source.options.assign(options->options, options->options + options->numOpt);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return binaries_.emplace(binary, std::move(entry)).second;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool SimtWarpSourceRegistry::RegisterFunction(aclrtBinHandle binary, aclrtFuncHandle function, const char* name)
{
    if (binary == nullptr || function == nullptr || name == nullptr || name[0] == '\0')
        return false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = binaries_.find(binary);
        if (found == binaries_.end())
            return false;
        found->second.names[function] = name;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool SimtWarpSourceRegistry::GetSource(aclrtFuncHandle function, SimtWarpBinarySource& source)
{
    if (function == nullptr)
        return false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : binaries_) {
            const auto found = item.second.names.find(function);
            if (found == item.second.names.end())
                continue;
            SimtWarpBinarySource result = item.second.source;
            result.name = found->second;
            source = std::move(result);
            return true;
        }
        return false;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void SimtWarpSourceRegistry::UnregisterBinary(aclrtBinHandle binary)
{
    std::lock_guard<std::mutex> lock(mutex_);
    binaries_.erase(binary);
}
} // namespace aclpti::profiling
