/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "replay_memory.h"

#include "common/debug_log.h"

#include <new>

namespace aclpti::profiling {

aclptiResult ReplayMemory::MirrorMalloc(
    void** devPtr, std::size_t size, aclrtMemMallocPolicy policy, aclrtApiId api, aclrtMallocConfig* cfg)
{
    if (devPtr == nullptr || *devPtr == nullptr || size == 0) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_malloc status=%d reason=invalid_parameter",
            ACLPTI_ERROR_INVALID_PARAMETER);
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    const auto mallocFunction = acltoolGetOriginalRuntimeApi(api);
    const auto freeFunction = reinterpret_cast<aclrtFreeFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFree));
    if (mallocFunction == nullptr || freeFunction == nullptr) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_malloc_lookup status=%d malloc_available=%d free_available=%d",
            ACLPTI_ERROR_PROFILING_FAILED, static_cast<int>(mallocFunction != nullptr),
            static_cast<int>(freeFunction != nullptr));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }

    void* shadow = nullptr;
    const aclError result = api == ACL_RT_API_aclrtMallocWithCfg ?
                                reinterpret_cast<aclrtMallocWithCfgFunc>(mallocFunction)(&shadow, size, policy, cfg) :
                                reinterpret_cast<aclrtMallocFunc>(mallocFunction)(&shadow, size, policy);
    if (result != ACL_SUCCESS) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_malloc status=%d api=%u", result, static_cast<unsigned>(api));
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (shadow == nullptr) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_malloc status=%d reason=null_shadow", ACLPTI_ERROR_PROFILING_FAILED);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const std::uintptr_t origin = reinterpret_cast<std::uintptr_t>(*devPtr);
    try {
        const auto inserted = shadowBuffers_.emplace(origin, ShadowBuffer{shadow, size, false});
        if (inserted.second) {
            npucompute::detail::DebugLog("aclpti", "shadow malloc origin=%p shadow=%p size=%zu", *devPtr, shadow, size);
            return ACLPTI_SUCCESS;
        }
        const aclError freeResult = freeFunction(shadow);
        if (freeResult != ACL_SUCCESS) {
            npucompute::detail::DebugLog(
                "aclpti", "error operation=shadow_duplicate_cleanup status=%d shadow=%p", freeResult, shadow);
        }
        return ACLPTI_ERROR_PROFILING_FAILED;
    } catch (const std::bad_alloc&) {
        const aclError freeStatus = freeFunction(shadow);
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_metadata_alloc status=%d cleanup_status=%d", ACLPTI_ERROR_OUT_OF_MEMORY,
            freeStatus);
        return ACLPTI_ERROR_OUT_OF_MEMORY;
    }
}

aclptiResult ReplayMemory::MirrorFree(void* devPtr)
{
    const auto freeFunction = reinterpret_cast<aclrtFreeFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFree));
    if (freeFunction == nullptr) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_free_lookup status=%d api=aclrtFree", ACLPTI_ERROR_PROFILING_FAILED);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }

    const aclptiResult orphanFailure = RetryOrphanedShadows(freeFunction);
    const auto iterator = shadowBuffers_.find(reinterpret_cast<std::uintptr_t>(devPtr));
    if (iterator == shadowBuffers_.end()) {
        return orphanFailure;
    }
    if (iterator->second.orphaned) {
        return orphanFailure;
    }
    npucompute::detail::DebugLog("aclpti", "shadow free origin=%p shadow=%p", devPtr, iterator->second.shadow);
    const aclError result = freeFunction(iterator->second.shadow);
    if (result != ACL_SUCCESS) {
        iterator->second.orphaned = true;
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_free status=%d shadow=%p", result, iterator->second.shadow);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    shadowBuffers_.erase(iterator);
    return orphanFailure;
}

aclptiResult ReplayMemory::RetryOrphanedShadows(aclrtFreeFunc freeFunction)
{
    aclptiResult status = ACLPTI_SUCCESS;
    auto iterator = shadowBuffers_.begin();
    while (iterator != shadowBuffers_.end()) {
        if (!iterator->second.orphaned) {
            ++iterator;
            continue;
        }
        const aclError result = freeFunction(iterator->second.shadow);
        if (result == ACL_SUCCESS) {
            iterator = shadowBuffers_.erase(iterator);
            continue;
        }
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_orphan_cleanup status=%d shadow=%p", result, iterator->second.shadow);
        status = ACLPTI_ERROR_PROFILING_FAILED;
        ++iterator;
    }
    return status;
}

aclptiResult ReplayMemory::MirrorMemcpy(
    void* destination, std::size_t destinationSize, const void* source, std::size_t count, aclrtMemcpyKind kind)
{
    if (count > destinationSize) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    if (count == 0 || (kind != ACL_MEMCPY_DEVICE_TO_DEVICE && kind != ACL_MEMCPY_HOST_TO_DEVICE)) {
        return ACLPTI_SUCCESS;
    }

    ShadowBuffer destinationBuffer{};
    std::size_t destinationOffset = 0;
    if (!FindShadowBuffer(destination, count, &destinationBuffer, &destinationOffset)) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_memcpy status=%d reason=shadow_not_found destination=%p count=%zu",
            ACLPTI_ERROR_PROFILING_FAILED, destination, count);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const auto memcpyFunction = reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
    if (memcpyFunction == nullptr) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_memcpy_lookup status=%d api=aclrtMemcpy", ACLPTI_ERROR_PROFILING_FAILED);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }

    void* shadowDestination = static_cast<std::uint8_t*>(destinationBuffer.shadow) + destinationOffset;
    const int result =
        memcpyFunction(shadowDestination, destinationBuffer.size - destinationOffset, source, count, kind);
    npucompute::detail::DebugLog(
        "aclpti", "mirror memcpy kind=%d destination=%p shadow=%p count=%zu result=%d", kind, destination,
        shadowDestination, count, result);
    if (result != ACL_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "error operation=shadow_memcpy status=%d api=aclrtMemcpy", result);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult ReplayMemory::MirrorMemset(void* devPtr, std::size_t maxCount, std::int32_t value, std::size_t count)
{
    if (count > maxCount) {
        return ACLPTI_ERROR_INVALID_PARAMETER;
    }
    if (count == 0) {
        return ACLPTI_SUCCESS;
    }

    ShadowBuffer buffer{};
    std::size_t offset = 0;
    if (!FindShadowBuffer(devPtr, count, &buffer, &offset)) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_memset status=%d reason=shadow_not_found origin=%p count=%zu",
            ACLPTI_ERROR_PROFILING_FAILED, devPtr, count);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const auto memsetFunction = reinterpret_cast<aclrtMemsetFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemset));
    if (memsetFunction == nullptr) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=shadow_memset_lookup status=%d api=aclrtMemset", ACLPTI_ERROR_PROFILING_FAILED);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    void* shadow = static_cast<std::uint8_t*>(buffer.shadow) + offset;
    const int result = memsetFunction(shadow, buffer.size - offset, value, count);
    npucompute::detail::DebugLog(
        "aclpti", "mirror memset origin=%p shadow=%p value=%d count=%zu result=%d", devPtr, shadow, value, count,
        result);
    if (result != ACL_SUCCESS) {
        npucompute::detail::DebugLog("aclpti", "error operation=shadow_memset status=%d api=aclrtMemset", result);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    return ACLPTI_SUCCESS;
}

aclptiResult ReplayMemory::Restore() const
{
    const auto memcpyFunction = reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
    if (memcpyFunction == nullptr) {
        npucompute::detail::DebugLog(
            "aclpti", "error operation=replay_restore_lookup status=%d api=aclrtMemcpy", ACLPTI_ERROR_PROFILING_FAILED);
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    for (const auto& entry : shadowBuffers_) {
        if (entry.second.orphaned) {
            continue;
        }
        void* origin = reinterpret_cast<void*>(entry.first);
        const aclError result = memcpyFunction(
            origin, entry.second.size, entry.second.shadow, entry.second.size, ACL_MEMCPY_DEVICE_TO_DEVICE);
        if (result != ACL_SUCCESS) {
            npucompute::detail::DebugLog(
                "aclpti", "error operation=replay_restore status=%d origin=%p shadow=%p size=%zu", result, origin,
                entry.second.shadow, entry.second.size);
            return ACLPTI_ERROR_RESULT_UNRELIABLE;
        }
        npucompute::detail::DebugLog(
            "aclpti", "restore origin=%p shadow=%p size=%zu", origin, entry.second.shadow, entry.second.size);
    }
    return ACLPTI_SUCCESS;
}

bool ReplayMemory::FindShadowBuffer(
    const void* pointer, std::size_t count, ShadowBuffer* buffer, std::size_t* offset) const
{
    if (pointer == nullptr) {
        return false;
    }

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pointer);
    auto iterator = shadowBuffers_.upper_bound(address);
    if (iterator == shadowBuffers_.begin()) {
        return false;
    }
    --iterator;

    const std::size_t bufferOffset = static_cast<std::size_t>(address - iterator->first);
    if (bufferOffset > iterator->second.size || count > iterator->second.size - bufferOffset) {
        return false;
    }
    *buffer = iterator->second;
    *offset = bufferOffset;
    return true;
}

} // namespace aclpti::profiling
