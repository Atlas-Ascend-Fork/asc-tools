/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "aclsan_trace_runtime.h"
#include "securec.h"

#include "device_instr/decoder_registry.h"
#include "device_instr/arch/dav_3510/register_state_manager.h"
#include "device_instr/common/instruction_id.h"
#include "device_runtime/device_binary_registry.h"
#include "aclsan_device_data.h"
#include "aclsan_device_data_log.h"
#include "aclsan_dispatch.h"
#include "aclsan_register_dependency.h"
#include "npu_tool_log.h"
#include "aclsan_runtime_hook.h"
#include "aclsan_trace_buffer.h"
#include "injection/injection_hook.h"
#include "dbi/trace_buffer_abi.h"

#include <stdexcept>
#include <algorithm>
#include <climits>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <variant>

namespace aclsan {
namespace {

struct PendingTrace {
    uint64_t launchId = 0;
    const void* function = nullptr;
    const void* stream = nullptr;
    void* deviceBuffer = nullptr;
    uint32_t blockCount = 0;
    uint32_t physicalCoreSlotCount = 0;
    uint32_t deviceId = 0;
    const aclsan::DeviceInstructionDecoder* decoder = nullptr;
    std::vector<uint8_t> hostBuffer;
    std::shared_ptr<void> binaryLease;
    std::vector<uint8_t> arguments;
    std::vector<aclrtPlaceHolderInfo> placeholders;
    std::vector<PreparedTraceLaunch::HostInput> hostInputs;
    std::unique_ptr<dav3510::Dav3510RegisterStateManager> registerState;
    std::vector<uint32_t> dispatchedRecordCounts;
    uint64_t parameterBytes = 0;
    std::vector<InternalMemoryRange> readOnlyBinaryRanges;
};

struct LoadedImageRegion {
    void* address = nullptr;
    uint64_t bytes = 0;
    uint64_t resourceId = 0;
};

// 运行时为已加载 device ELF 占用的 GM 区间，通过公共 RESOURCE 事件登记给分析侧。
struct LoadedImageResource {
    uint32_t deviceId = 0;
    uintptr_t binary = 0;
    std::vector<LoadedImageRegion> regions;
};

struct TraceRuntimeState {
    std::mutex mutex;
    uint64_t nextLaunchId = 1;
    std::vector<PendingTrace> pending;
    std::map<uintptr_t, LoadedImageResource> loadedImages;
    std::map<uintptr_t, size_t> loadedImageLaunches;
    std::map<uintptr_t, std::map<uintptr_t, size_t>> streamLoadedImageLaunches;
    uint64_t nextResourceId = 1;
};

TraceRuntimeState& State()
{
    static TraceRuntimeState state;
    return state;
}

device_runtime::DeviceBinaryRegistry& DeviceBinaries()
{
    static device_runtime::DeviceBinaryRegistry binaries;
    return binaries;
}

constexpr const char* kLoadedImageApiName = "aclsanLoadedImage";

AclsanResourceData MakeLoadedImageResourceData(
    const LoadedImageResource& image, const LoadedImageRegion& region) noexcept
{
    AclsanResourceData data{};
    data.common.version = ACLSAN_API_VERSION;
    data.common.size = static_cast<uint32_t>(sizeof(AclsanResourceData));
    data.common.apiName = kLoadedImageApiName;
    data.common.result = ACL_SUCCESS;
    data.common.correlationId = 0;
    data.ptr = region.address;
    data.bytes = region.bytes;
    data.memorySpace = ACLSAN_MEMORY_SPACE_DEVICE;
    data.deviceId = image.deviceId;
    data.resourceId = region.resourceId;
    return data;
}

void AppendLoadedImageEvents(const LoadedImageResource& image, std::vector<AclsanResourceData>& events)
{
    events.reserve(events.size() + image.regions.size());
    for (const LoadedImageRegion& region : image.regions) {
        events.push_back(MakeLoadedImageResourceData(image, region));
    }
}

void DispatchLoadedImageEvents(AclsanCallbackId callbackId, const std::vector<AclsanResourceData>& events)
{
    for (const AclsanResourceData& event : events) {
        AclsanCallbackDispatcher::DispatchResource(callbackId, event);
    }
}

bool ValidateLoadedImageSegments(uintptr_t base, const std::vector<BinarySegment>& segments) noexcept
{
    for (const BinarySegment& segment : segments) {
        if (segment.offset > std::numeric_limits<uintptr_t>::max() - base ||
            segment.bytes > std::numeric_limits<uint64_t>::max() - segment.offset) {
            return false;
        }
    }
    return true;
}

using BinaryAddressQuery = aclError (*)(aclrtBinHandle, void**, size_t*);

bool QueryBinaryAddress(uintptr_t binary, uintptr_t& base, uint64_t& runtimeBytes) noexcept
{
    base = 0;
    runtimeBytes = 0;
    if (binary == 0) {
        return false;
    }
    const auto invoke = [binary](BinaryAddressQuery query, void*& address, size_t& bytes) noexcept {
        address = nullptr;
        bytes = 0;
        if (query == nullptr) {
            return ACL_ERROR_FAILURE;
        }
        return query(reinterpret_cast<aclrtBinHandle>(binary), &address, &bytes);
    };
    void* address = nullptr;
    size_t bytes = 0;
    aclError status = ACL_ERROR_FAILURE;
    const auto exported = reinterpret_cast<BinaryAddressQuery>(dlsym(RTLD_DEFAULT, "aclrtBinaryGetDevAddress"));
    if (exported != nullptr) {
        status = invoke(exported, address, bytes);
    } else {
        // 兼容旧版 runtime 未导出该符号：从已加载的 libacl_rt.so 中解析。
        void* library = dlopen("libacl_rt.so", RTLD_NOW | RTLD_NOLOAD);
        if (library != nullptr) {
            status = invoke(
                reinterpret_cast<BinaryAddressQuery>(dlsym(library, "aclrtBinaryGetDevAddress")), address, bytes);
            dlclose(library);
        }
    }
    const auto addressValue = reinterpret_cast<uintptr_t>(address);
    if (status != ACL_SUCCESS || addressValue == 0 || bytes > UINTPTR_MAX - addressValue) {
        ASCTOOL_DEBUG(
            "acl_san binary mapping unavailable: binary=%p result=%d", reinterpret_cast<void*>(binary), status);
        return false;
    }
    base = addressValue;
    runtimeBytes = bytes;
    return true;
}

std::vector<InternalMemoryRange> ResolveReadOnlyBinaryRanges(aclrtFuncHandle function)
{
    uintptr_t binary = 0;
    std::vector<BinarySectionRange> sections;
    if (!DeviceBinaries().GetFunctionReadOnlySections(reinterpret_cast<uintptr_t>(function), binary, sections) ||
        sections.empty()) {
        return {};
    }
    // This read-only query is not hooked and needs no public callback/API-table entry.
    uintptr_t base = 0;
    uint64_t bytes = 0;
    if (!QueryBinaryAddress(binary, base, bytes)) {
        return {};
    }
    std::vector<InternalMemoryRange> ranges;
    for (const auto& section : sections) {
        if (section.offset > bytes || section.bytes > bytes - section.offset) {
            ASCTOOL_WARNING(
                "acl_san read-only section exceeds binary mapping: binary=%p", reinterpret_cast<void*>(binary));
            return {};
        }
        ranges.push_back({reinterpret_cast<void*>(base + section.offset), static_cast<size_t>(section.bytes)});
        ASCTOOL_DEBUG(
            "acl_san read-only section: binary=%p base=%p offset=%llu bytes=%llu", reinterpret_cast<void*>(binary),
            reinterpret_cast<void*>(base), static_cast<unsigned long long>(section.offset),
            static_cast<unsigned long long>(section.bytes));
    }
    return ranges;
}

constexpr bool StrictModeEnabled() { return true; }

uint64_t AllocateLaunchId()
{
    TraceRuntimeState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    uint64_t id = state.nextLaunchId++;
    if (id == 0) {
        id = state.nextLaunchId++;
    }
    return id;
}

} // namespace

// 首次成功 launch 后登记 loaded-image 区间；重复 launch 只增加在途计数，不重复发 alloc。
bool RegisterTraceLoadedImageResources(
    uint32_t deviceId, uintptr_t binary, uintptr_t base, const std::vector<BinarySegment>& segments,
    const void* stream) noexcept
{
    if (binary == 0 || base == 0 || segments.empty() || !ValidateLoadedImageSegments(base, segments)) {
        return false;
    }
    const uintptr_t streamKey = reinterpret_cast<uintptr_t>(stream);
    std::vector<AclsanResourceData> allocated;
    bool insertedImage = false;
    try {
        TraceRuntimeState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto existing = state.loadedImages.find(binary);
        if (existing != state.loadedImages.end()) {
            ++state.loadedImageLaunches[binary];
            ++state.streamLoadedImageLaunches[streamKey][binary];
            return true;
        }
        LoadedImageResource image;
        image.deviceId = deviceId;
        image.binary = binary;
        image.regions.reserve(segments.size());
        for (const BinarySegment& segment : segments) {
            if (state.nextResourceId == 0) {
                state.nextResourceId = 1;
            }
            image.regions.push_back(
                {reinterpret_cast<void*>(base + static_cast<uintptr_t>(segment.offset)), segment.bytes,
                 state.nextResourceId++});
        }
        const auto inserted = state.loadedImages.emplace(binary, std::move(image)).first;
        insertedImage = true;
        state.loadedImageLaunches[binary] = 1;
        state.streamLoadedImageLaunches[streamKey][binary] = 1;
        AppendLoadedImageEvents(inserted->second, allocated);
    } catch (...) {
        if (insertedImage) {
            // 事件生成失败时不保留登记，避免后续 release 找不到配对 alloc 而漏报。
            TraceRuntimeState& state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.loadedImages.erase(binary);
            state.loadedImageLaunches.erase(binary);
            const auto streamEntry = state.streamLoadedImageLaunches.find(streamKey);
            if (streamEntry != state.streamLoadedImageLaunches.end()) {
                streamEntry->second.erase(binary);
                if (streamEntry->second.empty()) {
                    state.streamLoadedImageLaunches.erase(streamEntry);
                }
            }
        }
        ASCTOOL_ERROR("acl_san loaded image registration failed: binary=%p", reinterpret_cast<void*>(binary));
        return false;
    }
    DispatchLoadedImageEvents(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, allocated);
    return true;
}

void AcquireTraceLaunchResources(const PreparedTraceLaunch& prepared, const void* function, const void* stream) noexcept
{
    // 只有插桩 launch 才会产生设备侧分析记录，因此只在该分析窗口内登记 loaded-image 区间。
    if (!prepared.instrumented || function == nullptr) {
        return;
    }
    try {
        uintptr_t binary = 0;
        std::vector<BinarySegment> segments;
        if (!DeviceBinaries().GetFunctionActualBinary(reinterpret_cast<uintptr_t>(function), binary, segments)) {
            ASCTOOL_DEBUG("acl_san loaded image unavailable: function=%p", function);
            return;
        }
        uintptr_t base = 0;
        uint64_t runtimeBytes = 0;
        if (!QueryBinaryAddress(binary, base, runtimeBytes)) {
            return;
        }
        for (const BinarySegment& segment : segments) {
            if (segment.offset > runtimeBytes || segment.bytes > runtimeBytes - segment.offset) {
                ASCTOOL_WARNING(
                    "acl_san loaded image segment exceeds binary mapping: binary=%p", reinterpret_cast<void*>(binary));
                return;
            }
        }
        (void)RegisterTraceLoadedImageResources(prepared.deviceId, binary, base, segments, stream);
    } catch (...) {
        ASCTOOL_ERROR("acl_san loaded image acquire failed: function=%p", function);
    }
}

// stream 同步后按 stream 扣减在途计数；某个 binary 计数归零时发 free，下次 launch 重新 alloc。
void ReleaseTraceStreamResources(const void* stream) noexcept
{
    const uintptr_t streamKey = reinterpret_cast<uintptr_t>(stream);
    std::vector<AclsanResourceData> released;
    try {
        TraceRuntimeState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        const auto streamEntry = state.streamLoadedImageLaunches.find(streamKey);
        if (streamEntry == state.streamLoadedImageLaunches.end()) {
            return;
        }
        for (const auto& entry : streamEntry->second) {
            const uintptr_t binary = entry.first;
            const auto total = state.loadedImageLaunches.find(binary);
            if (total == state.loadedImageLaunches.end()) {
                continue;
            }
            total->second -= std::min(entry.second, total->second);
            if (total->second != 0) {
                continue;
            }
            const auto image = state.loadedImages.find(binary);
            if (image == state.loadedImages.end()) {
                continue;
            }
            AppendLoadedImageEvents(image->second, released);
            state.loadedImages.erase(image);
            state.loadedImageLaunches.erase(total);
        }
        state.streamLoadedImageLaunches.erase(streamEntry);
    } catch (...) {
        ASCTOOL_ERROR("acl_san loaded image release failed: stream=%p", stream);
    }
    DispatchLoadedImageEvents(ACLSAN_CBID_RESOURCE_MEMORY_FREE, released);
}

// reset 兜底：设备内存整体失效时释放该 device 上所有 loaded-image 登记。
void ReleaseTraceDeviceResources(uint32_t deviceId) noexcept
{
    std::vector<AclsanResourceData> released;
    try {
        TraceRuntimeState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        for (auto image = state.loadedImages.begin(); image != state.loadedImages.end();) {
            if (image->second.deviceId != deviceId) {
                ++image;
                continue;
            }
            const uintptr_t binary = image->first;
            AppendLoadedImageEvents(image->second, released);
            state.loadedImageLaunches.erase(binary);
            for (auto& stream : state.streamLoadedImageLaunches) {
                stream.second.erase(binary);
            }
            image = state.loadedImages.erase(image);
        }
        for (auto stream = state.streamLoadedImageLaunches.begin(); stream != state.streamLoadedImageLaunches.end();) {
            if (stream->second.empty()) {
                stream = state.streamLoadedImageLaunches.erase(stream);
            } else {
                ++stream;
            }
        }
    } catch (...) {
        ASCTOOL_ERROR("acl_san loaded image device release failed: device=%u", deviceId);
    }
    DispatchLoadedImageEvents(ACLSAN_CBID_RESOURCE_MEMORY_FREE, released);
}

namespace {

// 为已经DBI插桩的kernel扩展HostArgs，在原始参数中插入隐藏的device trace buffer指针
aclError ExpandArguments(
    const void* hostArgs, size_t argsSize, const aclrtPlaceHolderInfo* placeholders, size_t placeholderCount,
    uint32_t traceArgumentOffset, PreparedTraceLaunch& prepared)
{
    if ((argsSize != 0 && hostArgs == nullptr) || (placeholderCount != 0 && placeholders == nullptr)) {
        return ACL_ERROR_INVALID_PARAM;
    }

    size_t insertionOffset = 0;
    if (placeholderCount != 0) {
        uint32_t lastAddressOffset = 0;
        for (size_t i = 0; i < placeholderCount; ++i) {
            lastAddressOffset = std::max(lastAddressOffset, placeholders[i].addrOffset);
        }
        if (lastAddressOffset > std::numeric_limits<uint32_t>::max() - sizeof(void*)) {
            return ACL_ERROR_INVALID_PARAM;
        }
        insertionOffset = static_cast<size_t>(lastAddressOffset) + sizeof(void*);
        if (insertionOffset > argsSize) {
            return ACL_ERROR_INVALID_PARAM;
        }
    } else {
        if (argsSize > std::numeric_limits<size_t>::max() - 7U) {
            return ACL_ERROR_INVALID_PARAM;
        }
        insertionOffset = (argsSize + 7U) & ~static_cast<size_t>(7U);
    }

    if (insertionOffset > traceArgumentOffset ||
        traceArgumentOffset > std::numeric_limits<size_t>::max() - sizeof(void*)) {
        return ACL_ERROR_INVALID_PARAM;
    }

    const size_t paddingBytes = traceArgumentOffset - insertionOffset;
    if (std::max(argsSize, insertionOffset) > std::numeric_limits<size_t>::max() - paddingBytes - sizeof(void*)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    const size_t expandedSize = std::max(argsSize, insertionOffset) + paddingBytes + sizeof(void*);
    prepared.arguments.assign(expandedSize, 0);
    if (insertionOffset != 0 && argsSize != 0) {
        if (memcpy_s(
                prepared.arguments.data(), prepared.arguments.size(), hostArgs, std::min(argsSize, insertionOffset)) !=
            EOK) {
            return ACL_ERROR_INVALID_PARAM;
        }
    }
    if (argsSize > insertionOffset) {
        if (memcpy_s(
                prepared.arguments.data() + traceArgumentOffset + sizeof(void*),
                prepared.arguments.size() - traceArgumentOffset - sizeof(void*),
                static_cast<const uint8_t*>(hostArgs) + insertionOffset, argsSize - insertionOffset) != EOK) {
            return ACL_ERROR_INVALID_PARAM;
        }
    }

    if (placeholderCount != 0) {
        prepared.placeholders.assign(placeholders, placeholders + placeholderCount);
    }
    for (aclrtPlaceHolderInfo& placeholder : prepared.placeholders) {
        if (placeholder.dataOffset >= insertionOffset) {
            const size_t shiftBytes = paddingBytes + sizeof(void*);
            if (placeholder.dataOffset > std::numeric_limits<uint32_t>::max() - shiftBytes) {
                return ACL_ERROR_INVALID_PARAM;
            }
            placeholder.dataOffset += static_cast<uint32_t>(shiftBytes);
        }
    }
    return ACL_SUCCESS;
}

void StoreHiddenPointer(PreparedTraceLaunch& prepared, TraceArgumentMode argumentMode)
{
    if (argumentMode == TraceArgumentMode::HOST_ARGS) {
        if (memcpy_s(
                prepared.arguments.data() + prepared.traceArgumentOffset,
                prepared.arguments.size() - prepared.traceArgumentOffset, &prepared.deviceBuffer,
                sizeof(void*)) != EOK) {
            throw std::runtime_error("cannot store trace buffer pointer");
        }
    }
}

void ReleaseDeviceBuffer(void* buffer) noexcept
{
    if (buffer == nullptr) {
        return;
    }
    const auto freeFunction = GetOriginalRuntimeFunction<aclrtFreeFunc>(ACL_RT_API_aclrtFree, "aclrtFree");
    if (freeFunction(buffer) != ACL_SUCCESS) {
        ASCTOOL_ERROR("acl_san trace: failed to release launch-owned GM buffer %p", buffer);
    }
}

void ReleaseHostInputs(std::vector<PreparedTraceLaunch::HostInput>& inputs) noexcept
{
    for (const auto& input : inputs) {
        ReleaseDeviceBuffer(input.address);
    }
    inputs.clear();
}

bool DispatchTraceSnapshot(PendingTrace& pending, bool executionComplete, TraceCollectionResult& collection)
{
    const auto memcpyFunction = GetOriginalRuntimeFunction<aclrtMemcpyFunc>(ACL_RT_API_aclrtMemcpy, "aclrtMemcpy");
    if (memcpyFunction(
            pending.hostBuffer.data(), pending.hostBuffer.size(), pending.deviceBuffer, pending.hostBuffer.size(),
            ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        ASCTOOL_ERROR("acl_san trace: D2H failed for launch=%llu", static_cast<unsigned long long>(pending.launchId));
        return false;
    }

    TraceBufferParseResult parsed = ParseTraceBuffer(
        pending.hostBuffer.data(), pending.hostBuffer.size(), pending.physicalCoreSlotCount, pending.blockCount,
        pending.launchId, pending.deviceId);
    if (!parsed.ok) {
        ASCTOOL_ERROR(
            "acl_san trace: malformed buffer for launch=%llu: %s", static_cast<unsigned long long>(pending.launchId),
            parsed.error.c_str());
        return false;
    }

    collection.recordCount += parsed.records.size();
    collection.droppedRecordCount += parsed.overflowCount;
    if (executionComplete && !parsed.records.empty() && parsed.overflowCount == 0) {
        ++collection.completeLaunchCount;
    }

    if (pending.dispatchedRecordCounts.size() != pending.physicalCoreSlotCount) {
        pending.dispatchedRecordCounts.assign(pending.physicalCoreSlotCount, 0U);
    }
    std::vector<uint32_t> observedRecordCounts(pending.physicalCoreSlotCount, 0U);
    std::vector<ParsedTraceRecord> newRecords;
    try {
        newRecords.reserve(parsed.records.size());
        for (const ParsedTraceRecord& record : parsed.records) {
            if (record.phyCoreId >= observedRecordCounts.size()) {
                ASCTOOL_ERROR(
                    "acl_san trace: launch=%llu produced out-of-range physical core ID %u",
                    static_cast<unsigned long long>(pending.launchId), record.phyCoreId);
                return false;
            }
            const uint32_t recordIndex = observedRecordCounts[record.phyCoreId]++;
            if (recordIndex >= pending.dispatchedRecordCounts[record.phyCoreId]) {
                newRecords.push_back(record);
                newRecords.back().parameterBytes = pending.parameterBytes;
            }
        }
    } catch (...) {
        ASCTOOL_ERROR(
            "acl_san trace: failed to stage records for launch=%llu",
            static_cast<unsigned long long>(pending.launchId));
        return false;
    }
    for (uint32_t phyCoreId = 0; phyCoreId < pending.physicalCoreSlotCount; ++phyCoreId) {
        if (observedRecordCounts[phyCoreId] < pending.dispatchedRecordCounts[phyCoreId]) {
            ASCTOOL_ERROR(
                "acl_san trace: launch=%llu physical core %u record count regressed from %u to %u",
                static_cast<unsigned long long>(pending.launchId), phyCoreId, pending.dispatchedRecordCounts[phyCoreId],
                observedRecordCounts[phyCoreId]);
            return false;
        }
    }
    pending.dispatchedRecordCounts = std::move(observedRecordCounts);

    ASCTOOL_INFO(
        "acl_san trace: launch=%llu status=%s records=%zu newRecords=%zu dropped=%llu physicalCoreSlots=%u blocks=%u",
        static_cast<unsigned long long>(pending.launchId), executionComplete ? "complete" : "snapshot",
        parsed.records.size(), newRecords.size(), static_cast<unsigned long long>(parsed.overflowCount),
        pending.physicalCoreSlotCount, pending.blockCount);
    if (!newRecords.empty() && pending.decoder != nullptr) {
        DispatchTraceRecords(
            newRecords, *pending.decoder, &pending.hostInputs, pending.registerState.get(),
            &pending.readOnlyBinaryRanges);
    }
    if (parsed.overflowCount != 0) {
        ASCTOOL_ERROR(
            "acl_san trace: launch=%llu dropped %llu records", static_cast<unsigned long long>(pending.launchId),
            static_cast<unsigned long long>(parsed.overflowCount));
    }
    return true;
}

aclError ResolveLaunchContext(PreparedTraceLaunch& prepared) noexcept
{
    const auto getSocName =
        GetOriginalRuntimeFunction<aclrtGetSocNameFunc>(ACL_RT_API_aclrtGetSocName, "aclrtGetSocName");

    const char* socName = getSocName();
    const std::optional<aclsan::SocVersion> socVersion = aclsan::ResolveSocVersion(socName);
    if (!socVersion.has_value()) {
        ASCTOOL_ERROR("acl_san trace: get unsupported soc name %s.", socName != nullptr ? socName : "<null>");
        return ACL_ERROR_RT_INTERNAL_ERROR;
    }

    prepared.decoder = aclsan::FindDeviceInstructionDecoder(*socVersion);
    if (prepared.decoder == nullptr) {
        ASCTOOL_ERROR("acl_san trace: cannot select Device instruction decoder for soc %s.", socName);
        return ACL_ERROR_RT_INTERNAL_ERROR;
    }

    const auto getDevice = GetOriginalRuntimeFunction<aclrtGetDeviceFunc>(ACL_RT_API_aclrtGetDevice, "aclrtGetDevice");
    int32_t deviceId = -1;
    if (getDevice(&deviceId) != ACL_SUCCESS || deviceId < 0) {
        ASCTOOL_ERROR("acl_san trace: cannot query current Device ID");
        return ACL_ERROR_RT_INTERNAL_ERROR;
    }
    prepared.deviceId = static_cast<uint32_t>(deviceId);
    // dav-3510 即使只有 96 个可用核，物理 ID 空间仍保留 108 个槽位；
    // trace 按物理 ID 索引 slice，不能用 aclrt 返回的可用核数缩减布局。
    prepared.physicalCoreSlotCount = aclsan::ASCSAN_PHYSICAL_CORE_SLOT_COUNT;
    return ACL_SUCCESS;
}

} // namespace

void DispatchTraceRecords(
    const std::vector<ParsedTraceRecord>& records, const aclsan::DeviceInstructionDecoder& decoder,
    const std::vector<PreparedTraceLaunch::HostInput>* internalInputs,
    // NOLINTNEXTLINE(readability-non-const-parameter): registerState aliases and updates the retained state.
    dav3510::Dav3510RegisterStateManager* retainedRegisterState,
    const std::vector<InternalMemoryRange>* readOnlyBinaryRanges) noexcept
{
    if (records.empty()) {
        return;
    }

    dav3510::Dav3510RegisterStateManager localRegisterState(records.front().launchId);
    dav3510::Dav3510RegisterStateManager& registerState =
        retainedRegisterState != nullptr ? *retainedRegisterState : localRegisterState;
    for (const ParsedTraceRecord& parsed : records) {
        if (!IsDefinedInstructionId(parsed.record.instrId)) {
            continue;
        }
        std::optional<aclsan::DecodedInstruction> decoded = decoder.decode(parsed.record);
        if (!decoded.has_value()) {
            ASCTOOL_ERROR(
                "acl_san trace: unsupported raw trace instrId=%u pc=0x%llx block=%u", parsed.record.instrId,
                static_cast<unsigned long long>(parsed.record.pc), parsed.blockId);
            // The translator emits a coverage marker for an undecoded L0 operand
            // instruction, keeping this gap visible to initialization checking.
            const std::optional<DeviceCallbackData> fallback =
                TranslateDecodedTraceToCallbackData(parsed, DecodedInstruction{});
            if (fallback.has_value()) {
                const auto* accesses = std::get_if<DeviceMemoryAccessDataList>(&*fallback);
                if (accesses != nullptr) {
                    for (const auto& access : *accesses) {
                        AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(access);
                    }
                }
            }
            continue;
        }

        const dav3510::Dav3510CoreKey key{parsed.blockType, parsed.blockId, parsed.phyCoreId, parsed.deviceId};
        if (std::holds_alternative<PushPbParamField>(decoded->params) ||
            std::holds_alternative<VfParamField>(decoded->params)) {
            LogRawRecord(parsed);
            LogParamField(decoded->params);
            continue;
        }
        bool stateInstruction = true;
        if (const auto* value = std::get_if<CtrlRegisterParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<VectorMaskParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<VectorMaskWriteParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<FmatrixParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<L3dRptParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<Mte2SourceParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<NdDmaPadCountParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<NdDmaLoopStrideParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<Mte2NzParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<FpcParamField>(&decoded->params)) {
            // FPC is published as a state event; it does not change address reconstruction.
            (void)value;
        } else if (const auto* value = std::get_if<ChannelParaParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<Loop3ParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<DmaLoopSizeParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<DmaLoopStrideParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else if (const auto* value = std::get_if<SetPaddingParamField>(&decoded->params)) {
            registerState.Update(key, *value);
        } else {
            stateInstruction = false;
        }
        if (stateInstruction) {
            LogRawRecord(parsed);
            LogParamField(decoded->params);
            if (const auto* mask = std::get_if<VectorMaskParamField>(&decoded->params)) {
                for (const auto& stateData : MakeRegisterStateDataForVecMask(parsed, *mask)) {
                    LogCallbackData(stateData);
                    AclsanCallbackDispatcher::DispatchDeviceState(stateData);
                }
            } else if (const auto stateData = MakeRegisterStateData(parsed, *decoded); stateData.has_value()) {
                LogCallbackData(*stateData);
                AclsanCallbackDispatcher::DispatchDeviceState(*stateData);
            }
            continue;
        }

        const std::optional<dav3510::Dav3510CoreRegisterState> state = registerState.Get(key);
        MemoryRegisterState memoryState{};
        if (state.has_value()) {
            memoryState.ctrl = state->ctrl;
            memoryState.fmatrix = state->fmatrix;
            memoryState.l3dRpt = state->l3dRpt;
            memoryState.mte2Source = state->mte2Source;
            memoryState.ndDmaPadCount = state->ndDmaPadCount;
            memoryState.ndDmaLoopStrides = state->ndDmaLoopStrides;
            memoryState.mte2Nz = state->mte2Nz;
            memoryState.loop3 = state->loop3;
            memoryState.channelPara = state->channelPara;
            memoryState.dmaLoopSizes = state->dmaLoopSizes;
            memoryState.dmaLoopStrides = state->dmaLoopStrides;
        }

        const auto callbackData =
            TranslateDecodedTraceToCallbackData(parsed, *decoded, memoryState, internalInputs, readOnlyBinaryRanges);
        if (!callbackData.has_value()) {
            continue;
        }
        if (const auto* memory = std::get_if<DeviceMemoryAccessDataList>(&*callbackData)) {
            if (memory->empty()) {
                ASCTOOL_DEBUG(
                    "acl_san trace cbdata skipped: launch=%llu instrId=%u reason=no GM access",
                    static_cast<unsigned long long>(parsed.launchId), parsed.record.instrId);
            }
            for (const AclsanDeviceMemoryAccessData& access : *memory) {
                AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(access);
            }
        } else if (const auto* sync = std::get_if<AclsanDeviceSyncData>(&*callbackData)) {
            AclsanCallbackDispatcher::DispatchDeviceSync(*sync);
        } else if (const auto* barrier = std::get_if<AclsanDeviceBarrierData>(&*callbackData)) {
            AclsanCallbackDispatcher::DispatchDeviceBarrier(*barrier);
        }
    }
}

void RecordTraceLoadedImage(aclrtBinHandle binary, const void* image, size_t bytes) noexcept
{
    if (!DeviceBinaries().RecordLoadedImage(reinterpret_cast<uintptr_t>(binary), image, bytes)) {
        ASCTOOL_WARNING("acl_san cannot preserve read-only sections for binary=%p", binary);
    }
}

void RecordTraceBinaryLoadFromData(
    aclrtBinHandle binary, bool instrumented, uint32_t traceArgumentOffset, const void* image,
    size_t imageBytes) noexcept
{
    if (binary == nullptr) {
        return;
    }
    try {
        if (!DeviceBinaries().RecordBinaryLoadFromData(
                reinterpret_cast<uintptr_t>(binary), instrumented, traceArgumentOffset, image, imageBytes)) {
            ASCTOOL_ERROR("acl_san trace: failed to preserve device source for binary %p", binary);
        }
    } catch (...) {
        ASCTOOL_ERROR("acl_san trace: failed to record binary load for %p", binary);
    }
}

void RecordTraceBinaryUnload(aclrtBinHandle binary) noexcept
{
    DeviceBinaries().RecordBinaryUnload(reinterpret_cast<uintptr_t>(binary));
}

void RecordTraceBinaryFunctionLookup(aclrtBinHandle binary, aclrtFuncHandle function, const char* functionName) noexcept
{
    if (binary == nullptr || function == nullptr) {
        return;
    }
    try {
        DeviceBinaries().RecordBinaryFunctionLookup(
            reinterpret_cast<uintptr_t>(binary), reinterpret_cast<uintptr_t>(function), functionName);
    } catch (...) {
        ASCTOOL_ERROR("acl_san trace: failed to record function %p for binary %p", function, binary);
    }
}

bool GetTraceFunctionName(aclrtFuncHandle function, std::string& functionName) noexcept
{
    return DeviceBinaries().GetFunctionName(reinterpret_cast<uintptr_t>(function), functionName);
}

void RecordTraceFunctionLookup(aclrtFuncHandle function) noexcept
{
    if (function == nullptr) {
        return;
    }
    try {
        DeviceBinaries().RecordLatestBinaryFunctionLookup(reinterpret_cast<uintptr_t>(function));
    } catch (...) {
        ASCTOOL_ERROR("acl_san trace: failed to record function lookup for %p", function);
    }
}

aclError PrepareTraceLaunch(
    aclrtFuncHandle function, uint32_t blockCount, const void* hostArgs, size_t argsSize,
    const aclrtPlaceHolderInfo* placeholders, size_t placeholderCount, TraceArgumentMode argumentMode,
    PreparedTraceLaunch& prepared) noexcept
{
    try {
        prepared = {};
        prepared.launchId = AllocateLaunchId();
        if (!DeviceBinaries().GetFunctionTraceArgumentOffset(
                reinterpret_cast<uintptr_t>(function), prepared.traceArgumentOffset)) {
            return ACL_SUCCESS;
        }
        prepared.instrumented = true;
        prepared.blockCount = blockCount;
        ACLSAN_RETURN_IF_ACL_ERROR(ResolveLaunchContext(prepared), "Failed to resolve trace launch context");
        prepared.readOnlyBinaryRanges = ResolveReadOnlyBinaryRanges(function);

        if (argumentMode == TraceArgumentMode::HOST_ARGS) {
            ACLSAN_RETURN_IF_ACL_ERROR(
                ExpandArguments(
                    hostArgs, argsSize, placeholders, placeholderCount, prepared.traceArgumentOffset, prepared),
                "Failed to expand trace launch arguments");
        }

        std::string error;
        if (!InitializeTraceBuffer(
                prepared.hostBuffer, prepared.physicalCoreSlotCount, blockCount, prepared.launchId, error)) {
            ASCTOOL_ERROR("acl_san trace: cannot initialize launch buffer: %s", error.c_str());
            StoreHiddenPointer(prepared, argumentMode);
            return StrictModeEnabled() ? ACL_ERROR_FAILURE : ACL_SUCCESS;
        }
        const auto mallocFunction = GetOriginalRuntimeFunction<aclrtMallocFunc>(ACL_RT_API_aclrtMalloc, "aclrtMalloc");
        const auto memcpyFunction = GetOriginalRuntimeFunction<aclrtMemcpyFunc>(ACL_RT_API_aclrtMemcpy, "aclrtMemcpy");
        aclError status = mallocFunction(&prepared.deviceBuffer, prepared.hostBuffer.size(), ACL_MEM_MALLOC_HUGE_FIRST);
        if (status == ACL_SUCCESS) {
            status = memcpyFunction(
                prepared.deviceBuffer, prepared.hostBuffer.size(), prepared.hostBuffer.data(),
                prepared.hostBuffer.size(), ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (status != ACL_SUCCESS) {
            ASCTOOL_ERROR("acl_san trace: cannot allocate or initialize launch GM buffer, status=%d", status);
            ReleaseDeviceBuffer(prepared.deviceBuffer);
            prepared.deviceBuffer = nullptr;
            StoreHiddenPointer(prepared, argumentMode);
            return StrictModeEnabled() ? status : ACL_SUCCESS;
        }

        StoreHiddenPointer(prepared, argumentMode);
        return ACL_SUCCESS;
    } catch (const std::bad_alloc&) {
        ReleaseDeviceBuffer(prepared.deviceBuffer);
        prepared.deviceBuffer = nullptr;
        ASCTOOL_ERROR("acl_san trace: out of memory while preparing launch");
        return ACL_ERROR_BAD_ALLOC;
    } catch (...) {
        ReleaseDeviceBuffer(prepared.deviceBuffer);
        prepared.deviceBuffer = nullptr;
        ASCTOOL_ERROR("acl_san trace: unexpected failure while preparing launch");
        return ACL_ERROR_FAILURE;
    }
}

aclError MaterializeTraceHostInputs(PreparedTraceLaunch& prepared) noexcept
{
    if (!prepared.instrumented || prepared.placeholders.empty()) {
        return ACL_SUCCESS;
    }
    std::vector<PreparedTraceLaunch::HostInput> inputs;
    const auto fail = [&inputs](aclError status) {
        ReleaseHostInputs(inputs);
        return status;
    };
    try {
        const auto allocate = GetOriginalRuntimeFunction<aclrtMallocFunc>(ACL_RT_API_aclrtMalloc, "aclrtMalloc");
        const auto copy = GetOriginalRuntimeFunction<aclrtMemcpyFunc>(ACL_RT_API_aclrtMemcpy, "aclrtMemcpy");
        inputs.reserve(prepared.placeholders.size());
        for (size_t index = 0; index < prepared.placeholders.size(); ++index) {
            const auto& placeholder = prepared.placeholders[index];
            if (placeholder.addrOffset % sizeof(void*) != 0 || placeholder.addrOffset > prepared.traceArgumentOffset ||
                prepared.traceArgumentOffset - placeholder.addrOffset < sizeof(void*) ||
                placeholder.dataOffset < prepared.traceArgumentOffset + sizeof(void*) ||
                placeholder.dataOffset >= prepared.arguments.size()) {
                return fail(ACL_ERROR_INVALID_PARAM);
            }
            bool alias = false;
            for (size_t previous = 0; previous < index; ++previous) {
                const auto& earlier = prepared.placeholders[previous];
                if (earlier.addrOffset == placeholder.addrOffset) {
                    return fail(ACL_ERROR_INVALID_PARAM);
                }
                if (earlier.dataOffset == placeholder.dataOffset) {
                    if (memcpy_s(
                            prepared.arguments.data() + placeholder.addrOffset,
                            prepared.arguments.size() - placeholder.addrOffset,
                            prepared.arguments.data() + earlier.addrOffset, sizeof(void*)) != EOK) {
                        return fail(ACL_ERROR_INVALID_PARAM);
                    }
                    alias = true;
                }
            }
            if (alias) {
                continue;
            }
            size_t end = prepared.arguments.size();
            for (const auto& next : prepared.placeholders) {
                if (next.dataOffset > placeholder.dataOffset) {
                    end = std::min(end, static_cast<size_t>(next.dataOffset));
                }
            }
            const size_t bytes = end - placeholder.dataOffset;
            if (bytes > SIZE_MAX - 31U) {
                return fail(ACL_ERROR_INVALID_PARAM);
            }
            std::vector<uint8_t> payload((bytes + 31U) & ~size_t(31U), 0);
            ASCTOOL_DEBUG(
                "acl_san HostInput: launch=%llu index=%zu addrOffset=%u dataOffset=%u payloadBytes=%zu "
                "allocationBytes=%zu",
                static_cast<unsigned long long>(prepared.launchId), index, placeholder.addrOffset,
                placeholder.dataOffset, bytes, payload.size());
            if (memcpy_s(payload.data(), payload.size(), prepared.arguments.data() + placeholder.dataOffset, bytes) !=
                EOK) {
                return fail(ACL_ERROR_INVALID_PARAM);
            }
            void* address = nullptr;
            aclError status = allocate(&address, payload.size(), ACL_MEM_MALLOC_HUGE_FIRST);
            if (status != ACL_SUCCESS) {
                return fail(status);
            }
            inputs.push_back({address, payload.size()});
            status = copy(address, payload.size(), payload.data(), payload.size(), ACL_MEMCPY_HOST_TO_DEVICE);
            if (status != ACL_SUCCESS) {
                return fail(status);
            }
            if (memcpy_s(
                    prepared.arguments.data() + placeholder.addrOffset,
                    prepared.arguments.size() - placeholder.addrOffset, &address, sizeof(void*)) != EOK) {
                return fail(ACL_ERROR_INVALID_PARAM);
            }
        }
        prepared.arguments.resize(prepared.traceArgumentOffset + sizeof(void*));
        prepared.placeholders.clear();
        prepared.hostInputs = std::move(inputs);
        return ACL_SUCCESS;
    } catch (const std::bad_alloc&) {
        return fail(ACL_ERROR_BAD_ALLOC);
    } catch (...) {
        return fail(ACL_ERROR_FAILURE);
    }
}

void CompleteTraceLaunch(
    PreparedTraceLaunch&& prepared, const void* function, const void* stream, aclError launchResult) noexcept
{
    if (!prepared.instrumented || prepared.deviceBuffer == nullptr) {
        return;
    }
    if (launchResult != ACL_SUCCESS) {
        ReleaseDeviceBuffer(prepared.deviceBuffer);
        ReleaseHostInputs(prepared.hostInputs);
        return;
    }

    try {
        const uint64_t parameterBytes = prepared.arguments.empty() ?
                                            static_cast<uint64_t>(prepared.traceArgumentOffset) + sizeof(void*) :
                                            prepared.arguments.size();
        PendingTrace pending{
            prepared.launchId,
            function,
            stream,
            prepared.deviceBuffer,
            prepared.blockCount,
            prepared.physicalCoreSlotCount,
            prepared.deviceId,
            prepared.decoder,
            std::move(prepared.hostBuffer),
            std::move(prepared.binaryLease),
            std::move(prepared.arguments),
            std::move(prepared.placeholders),
            std::move(prepared.hostInputs),
            std::make_unique<dav3510::Dav3510RegisterStateManager>(prepared.launchId),
            std::vector<uint32_t>(prepared.physicalCoreSlotCount, 0U),
            parameterBytes,
            std::move(prepared.readOnlyBinaryRanges)};
        TraceRuntimeState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending.push_back(std::move(pending));
    } catch (...) {
        ASCTOOL_ERROR(
            "acl_san trace: failed to retain launch=%llu", static_cast<unsigned long long>(prepared.launchId));
        AbortHookFailure("launch", "retain_trace", "cannot retain resources for an in-flight kernel");
    }
}

TraceCollectionResult CollectTraceStream(aclrtStream stream, bool executionComplete) noexcept
{
    TraceCollectionResult collection;
    std::vector<PendingTrace> completed;
    try {
        {
            TraceRuntimeState& state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            for (auto it = state.pending.begin(); it != state.pending.end();) {
                if (it->stream != stream) {
                    ++it;
                    continue;
                }
                // A timed-out deferred kernel may still own HostInput and variant resources. Read a trace snapshot so
                // diagnostics emitted before the blocking instruction are visible, but keep all launch resources
                // until Runtime confirms completion or resets the Device.
                if (!executionComplete && it->binaryLease) {
                    ++collection.launchCount;
                    (void)DispatchTraceSnapshot(*it, false, collection);
                    ++it;
                } else {
                    completed.push_back(std::move(*it));
                    it = state.pending.erase(it);
                }
            }
        }
    } catch (...) {
        ASCTOOL_ERROR("acl_san trace: failed to detach completed launches for stream=%p", stream);
        return collection;
    }

    collection.launchCount += completed.size();
    for (PendingTrace& pending : completed) {
        try {
            (void)DispatchTraceSnapshot(pending, true, collection);
        } catch (...) {
            ASCTOOL_ERROR(
                "acl_san trace: unexpected D2H processing failure for launch=%llu",
                static_cast<unsigned long long>(pending.launchId));
        }
        ReleaseDeviceBuffer(pending.deviceBuffer);
        ReleaseHostInputs(pending.hostInputs);
    }
    return collection;
}

void ResetTraceRuntimeState() noexcept
{
    {
        TraceRuntimeState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending.clear();
        state.loadedImages.clear();
        state.loadedImageLaunches.clear();
        state.streamLoadedImageLaunches.clear();
    }
    DeviceBinaries().Reset();
}

device_runtime::CallStackResult ResolveTraceDeviceCallStack(uint64_t pc) noexcept
{
    return DeviceBinaries().ResolveCallStack(pc);
}

} // namespace aclsan
