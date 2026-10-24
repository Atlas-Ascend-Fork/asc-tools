/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
/**
 * @file replay_memory.h
 * @brief Mirrors device memory operations and restores captured state before kernel replay.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_REPLAY_MEMORY_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_REPLAY_MEMORY_H

#include "aclpti/aclpti_types.h"
#include "injection/injection_hook.h"

#include <cstddef>
#include <cstdint>
#include <map>

namespace aclpti::profiling {

class ReplayMemory {
public:
    /// Allocates a shadow buffer using the original allocation API, policy, and configuration.
    aclptiResult MirrorMalloc(
        void** devPtr, std::size_t size, aclrtMemMallocPolicy policy, aclrtApiId api, aclrtMallocConfig* cfg = nullptr);

    /// Releases and forgets the shadow buffer associated with a device allocation.
    aclptiResult MirrorFree(void* devPtr);

    /// Mirrors a supported copy into the corresponding shadow buffer.
    aclptiResult MirrorMemcpy(
        void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind kind);

    /// Mirrors a device memset into the corresponding shadow buffer.
    aclptiResult MirrorMemset(void* devPtr, std::size_t maxCount, std::int32_t value, std::size_t count);

    /// Restores all tracked device allocations from their shadow buffers.
    aclptiResult Restore() const;

private:
    struct ShadowBuffer {
        void* shadow;
        std::size_t size;
        bool orphaned;
    };

    /// Finds the shadow buffer and byte offset covering a device memory range.
    bool FindShadowBuffer(const void* pointer, std::size_t count, ShadowBuffer* buffer, std::size_t* offset) const;

    /// Retries shadow frees that previously failed after the user allocation was released.
    aclptiResult RetryOrphanedShadows(aclrtFreeFunc freeFunction);

    std::map<std::uintptr_t, ShadowBuffer> shadowBuffers_;
};

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_REPLAY_MEMORY_H
