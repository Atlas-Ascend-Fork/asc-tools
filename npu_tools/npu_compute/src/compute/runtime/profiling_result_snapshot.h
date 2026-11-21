/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_PROFILING_RESULT_SNAPSHOT_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_PROFILING_RESULT_SNAPSHOT_H

#include "aclpti/aclpti_data.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace npucompute {
namespace detail {

template <class T>
using SnapshotCopies = std::unordered_map<const T*, std::shared_ptr<const T>>;

template <class T>
std::shared_ptr<const T> CopySharedValue(const std::shared_ptr<const T>& source, SnapshotCopies<T>& copies)
{
    if (!source) {
        return {};
    }
    auto [entry, inserted] = copies.try_emplace(source.get());
    if (inserted) {
        entry->second = std::make_shared<const T>(*source);
    }
    return entry->second;
}

} // namespace detail

// Copy all data used by the asynchronous report while the ACLPTI view is valid.
inline aclptiProfilingDataResult SnapshotProfilingResult(const aclptiProfilingDataResult& source)
{
    auto snapshot = source;
    detail::SnapshotCopies<aclptiPmSamplingContext> contexts;
    detail::SnapshotCopies<std::vector<char>> elfImages;
    detail::SnapshotCopies<std::map<uint64_t, uint64_t>> businessInstructions;
    detail::SnapshotCopies<std::set<uint64_t>> simtInstructions;
    for (auto& entry : snapshot.pmSamplingContexts) {
        auto& context = entry.second;
        if (!context) {
            continue;
        }
        auto [copy, inserted] = contexts.try_emplace(context.get());
        if (inserted) {
            auto owned = std::make_shared<aclptiPmSamplingContext>(*context);
            owned->originalElf = detail::CopySharedValue(context->originalElf, elfImages);
            owned->companionElf = detail::CopySharedValue(context->companionElf, elfImages);
            owned->businessInstructions = detail::CopySharedValue(context->businessInstructions, businessInstructions);
            owned->simtInstructions = detail::CopySharedValue(context->simtInstructions, simtInstructions);
            copy->second = std::move(owned);
        }
        context = copy->second;
    }
    return snapshot;
}

} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_RUNTIME_PROFILING_RESULT_SNAPSHOT_H
