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
#include "aclsan_vf.h"

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
#include "aclsan_trace_completion_event.h"
#include "aclsan_trace_status.h"
#include "injection/injection_hook.h"
#include "dbi/trace_buffer_abi.h"
#include "dbi/ctrlbin/ctrlbin_bindings.h"

#include <stdexcept>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <thread>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <glob.h>
#include <sys/stat.h>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace aclsan {
namespace {

constexpr size_t TRACE_GLOBAL_BUDGET = 512U * 1024U * 1024U;
constexpr uint32_t TRACE_SLOT_DEFAULT_RINGS = 4U;
constexpr uint32_t TRACE_SLOT_MAX_RINGS = 16U;
constexpr uint32_t TRACE_SLOT_DEFAULT_RECORD_LIMIT = TRACE_SLOT_DEFAULT_RINGS * ASCSAN_TRACE_RECORDS_PER_CORE;
constexpr uint32_t TRACE_ENQUEUE_FAILURE_LIMIT = 3U;
constexpr int32_t TRACE_SYNC_ATTEMPT_LIMIT = 3;
constexpr uint32_t TRACE_SLOTS = ASCSAN_PHYSICAL_CORE_SLOT_COUNT;
constexpr auto TRACE_POLL_INTERVAL = std::chrono::milliseconds(20);
constexpr int32_t TRACE_COPY_TIMEOUT_MS = 1000;
constexpr auto TRACE_DRAIN_STALL_TIMEOUT = std::chrono::seconds(30);
constexpr auto TRACE_EXCEPTION_QUIET_PERIOD = std::chrono::minutes(1);
constexpr auto TRACE_EXCEPTION_WAIT_LIMIT = 2 * TRACE_EXCEPTION_QUIET_PERIOD;

struct TraceBatch {
    uint32_t slot = 0;
    uint32_t first = 0;
    std::vector<AclsanRawTraceRecord> records;
    bool ready = false;
};

struct PendingTrace {
    std::mutex mutex;
    std::mutex dispatchMutex;
    std::recursive_mutex collectionMutex;
    uint64_t launchId = 0;
    const void* function = nullptr;
    const void* stream = nullptr;
    aclrtContext context = nullptr;
    aclrtStream copyStream = nullptr;
    void* deviceBuffer = nullptr;
    uint32_t blockCount = 0;
    uint32_t physicalCoreSlotCount = 0;
    uint32_t deviceId = 0;
    uintptr_t loadedImageBinary = 0;
    const DeviceInstructionDecoder* decoder = nullptr;
    std::shared_ptr<void> binaryLease;
    std::vector<uint8_t> arguments;
    std::vector<aclrtPlaceHolderInfo> placeholders;
    std::vector<PreparedTraceLaunch::HostInput> hostInputs;
    std::unique_ptr<dav3510::Dav3510RegisterStateManager> registerState;
    uint64_t parameterBytes = 0;
    std::vector<InternalMemoryRange> readOnlyBinaryRanges;
    std::shared_ptr<NpubitUbHandle> hibitHandle;
    VfPbState pbState;
    std::array<uint32_t, TRACE_SLOTS> copied{};
    std::array<uint32_t, TRACE_SLOTS> acked{};
    std::array<uint32_t, TRACE_SLOTS> dispatched{};
    std::array<uint32_t, TRACE_SLOTS> lastWrite{};
    std::array<uint32_t, TRACE_SLOTS> lastDropped{};
    uint32_t slotRecordLimit = TRACE_SLOT_DEFAULT_RECORD_LIMIT;
    std::array<uint32_t, TRACE_SLOTS> queued{};
    std::array<std::deque<TraceBatch>, TRACE_SLOTS> batches;
    std::array<AclsanTraceControl, TRACE_SLOTS> controls{};
    std::array<AclsanTraceReadLine, TRACE_SLOTS> reads{};
    AclsanTraceBufferHeader segment{};
    TraceKernelEndTracker kernelEndTracker;
    TraceCompletionEvent completionEvent;
    bool completionSnapshotRead = false;
    bool completionQueryReported = false;
    uint64_t dropped = 0;
    size_t delivered = 0;
    uint32_t nextSlot = 0;
    uint32_t enqueueFailures = 0;
    bool collecting = false;
    bool committed = false;
    bool draining = false;
    bool abandoned = false;
    bool failed = false;
    bool quarantined = false;
    bool resetQuiesced = false;
    bool retired = false;
    bool resetRetired = false;
    bool kernelEndReported = false;
    bool operatorExecutionReported = false;
    bool executionException = false;
    bool exceptionTraceStable = false;
    bool partialExecutionReported = false;
    uint64_t exceptionWriteSequence = 0;
    std::optional<std::chrono::steady_clock::time_point> exceptionQuietSince;
    // 尾部稳定门控：kernel 结束标记后，写序号需连续一轮轮询保持不变才发布算子执行完毕事件，
    // 避免 kernel 尾部记录晚于标记被写入时分析侧提前收尾（racecheck 会判 malformed）。
    uint64_t operatorQuietWrite = 0;
    uint32_t operatorQuietPolls = 0;
    std::chrono::steady_clock::time_point operatorQuietSince{};
    uint64_t budgetBackpressure = 0;
    uint64_t slotBackpressure = 0;
    size_t queueHighWaterBytes = 0;
    size_t queuedRecordCount = 0;
    uint64_t pollCount = 0;
    uint64_t maxPollGapUs = 0;
    uint64_t maxDispatchBatchUs = 0;
    std::chrono::steady_clock::time_point lastPoll{};
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

using LoadedImageKey = std::pair<uint32_t, uintptr_t>;
using LoadedStreamKey = std::pair<uint32_t, uintptr_t>;
using LoadedImageLaunchCounts = std::map<LoadedImageKey, size_t>;

struct TraceRuntimeState {
    std::mutex mutex;
    std::mutex workerMutex;
    std::condition_variable wake;
    std::condition_variable dispatchWake;
    std::condition_variable lifecycleWake;
    std::condition_variable loadedImageWake;
    uint64_t nextLaunchId = 1;
    std::vector<std::shared_ptr<PendingTrace>> pending;
    std::unordered_set<uint32_t> quiesced;
    std::unordered_map<uint32_t, std::shared_ptr<std::shared_mutex>> deviceGates;
    std::atomic<size_t> queuedBytes{0};
    std::atomic<size_t> queueHighWaterBytes{0};
    std::thread worker;
    std::thread dispatcher;
    bool dispatchReady = false;
    bool pollRequested = false;
    bool joining = false;
    bool stopping = false;
    bool accepting = true;
    size_t activeLoadedImageDispatches = 0;
    std::set<LoadedImageKey> loadedImageDispatching;
    std::map<LoadedImageKey, LoadedImageResource> loadedImages;
    std::map<LoadedImageKey, size_t> loadedImageLaunches;
    std::map<LoadedStreamKey, LoadedImageLaunchCounts> streamLoadedImageLaunches;
    std::map<uintptr_t, uint64_t> streamFrontiers;
    uint64_t nextResourceId = 1;
};

TraceRuntimeState& State()
{
    // The runtime can unload with unresolved DMA. Keep quarantined ownership
    // alive until process exit instead of running static destructors on it.
    static TraceRuntimeState* state = new TraceRuntimeState;
    return *state;
}

thread_local bool traceDispatchActive = false;

struct TraceDispatchScope {
    bool previous = traceDispatchActive;
    TraceDispatchScope() { traceDispatchActive = true; }
    ~TraceDispatchScope() { traceDispatchActive = previous; }
};

struct TraceCollectionGuard {
    bool& active;
    ~TraceCollectionGuard() { active = false; }
};

void RequestTracePoll() noexcept
{
    auto& state = State();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pollRequested = true;
    }
    state.wake.notify_one();
}

void RequestTraceDispatch() noexcept
{
    auto& state = State();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.dispatchReady = true;
    }
    state.dispatchWake.notify_one();
}

std::shared_ptr<std::shared_mutex> DeviceGate(uint32_t deviceId)
{
    TraceRuntimeState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto& gate = state.deviceGates[deviceId];
    if (!gate) {
        gate = std::make_shared<std::shared_mutex>();
    }
    return gate;
}

device_runtime::DeviceBinaryRegistry& DeviceBinaries()
{
    static device_runtime::DeviceBinaryRegistry binaries;
    return binaries;
}

int32_t CurrentTraceDeviceId() noexcept
{
    auto getDevice = reinterpret_cast<aclrtGetDeviceFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtGetDevice));
    if (getDevice == nullptr) {
        getDevice =
            reinterpret_cast<aclrtGetDeviceFunc>(VerifiedTraceGetDeviceSymbol(dlsym(RTLD_DEFAULT, "aclrtGetDevice")));
    }
    int32_t deviceId = device_runtime::DeviceBinaryRegistry::UNKNOWN_DEVICE;
    const aclError status = getDevice == nullptr ? ACL_ERROR_FAILURE : getDevice(&deviceId);
    if (status != ACL_SUCCESS || deviceId < 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            ASCTOOL_WARNING("acl_san trace: binary registry device is unknown (getDevice result=%d)", status);
        }
        return device_runtime::DeviceBinaryRegistry::UNKNOWN_DEVICE;
    }
    return deviceId;
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

// 在修改账本前准备所有事件和占位；任何分配失败都保留完整账本供重试。
void PrepareLoadedImageRelease(
    TraceRuntimeState& state, const LoadedImageLaunchCounts& launches, std::vector<AclsanResourceData>& events,
    std::vector<LoadedImageKey>& images)
{
    for (const auto& entry : launches) {
        const auto total = state.loadedImageLaunches.find(entry.first);
        if (total == state.loadedImageLaunches.end() || entry.second < total->second) {
            continue;
        }
        const auto image = state.loadedImages.find(entry.first);
        if (image != state.loadedImages.end()) {
            AppendLoadedImageEvents(image->second, events);
            images.push_back(entry.first);
        }
    }
    try {
        for (const auto& image : images) {
            state.loadedImageDispatching.insert(image);
        }
    } catch (...) {
        for (const auto& image : images) {
            state.loadedImageDispatching.erase(image);
        }
        throw;
    }
}

// 调用方持有 state.mutex；提交阶段只执行计数和非分配性擦除。
void CommitLoadedImageRelease(TraceRuntimeState& state, const LoadedImageLaunchCounts& launches)
{
    for (const auto& entry : launches) {
        const auto total = state.loadedImageLaunches.find(entry.first);
        if (total == state.loadedImageLaunches.end()) {
            continue;
        }
        total->second -= std::min(entry.second, total->second);
        if (total->second != 0) {
            continue;
        }
        const auto image = state.loadedImages.find(entry.first);
        if (image != state.loadedImages.end()) {
            state.loadedImages.erase(image);
            state.loadedImageLaunches.erase(total);
        }
    }
}

void DispatchLoadedImageEvents(
    AclsanCallbackId callbackId, const std::vector<AclsanResourceData>& events, const LoadedImageKey* images,
    size_t imageCount) noexcept
{
    if (events.empty()) {
        return;
    }
    TraceDispatchScope callbackScope;
    for (const AclsanResourceData& event : events) {
        AclsanCallbackDispatcher::DispatchResource(callbackId, event);
    }
    TraceRuntimeState& state = State();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (size_t i = 0; i < imageCount; ++i) {
            state.loadedImageDispatching.erase(images[i]);
        }
        --state.activeLoadedImageDispatches;
    }
    state.loadedImageWake.notify_all();
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
    if (!DeviceBinaries().GetFunctionReadOnlySections(
            reinterpret_cast<uintptr_t>(function), binary, sections, CurrentTraceDeviceId()) ||
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

void FailTrace(PendingTrace& pending, const char* reason) noexcept;

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

void EraseTrace(const std::shared_ptr<PendingTrace>& pending);
bool FinalizeCollectedTrace(PendingTrace& pending, TraceCollectionResult& collection, bool executionComplete);

// 首次成功 launch 后登记 loaded-image 区间；重复 launch 只增加在途计数，不重复发 alloc。
static bool RetainLoadedImageLocked(
    TraceRuntimeState& state, const LoadedImageKey& imageKey, const LoadedStreamKey& streamKey)
{
    const auto total = state.loadedImageLaunches.find(imageKey);
    if (total == state.loadedImageLaunches.end() || total->second == std::numeric_limits<size_t>::max()) {
        return false;
    }
    auto& streamImages = state.streamLoadedImageLaunches[streamKey];
    const auto count = streamImages.find(imageKey);
    if (count != streamImages.end() && count->second == std::numeric_limits<size_t>::max()) {
        return false;
    }
    ++streamImages[imageKey];
    ++total->second;
    return true;
}

bool RegisterTraceLoadedImageResources(
    uint32_t deviceId, uintptr_t binary, uintptr_t base, const std::vector<BinarySegment>& segments,
    const void* stream) noexcept
{
    if (binary == 0 || base == 0 || segments.empty() || !ValidateLoadedImageSegments(base, segments)) {
        return false;
    }
    const LoadedImageKey imageKey{deviceId, binary};
    const LoadedStreamKey streamKey{deviceId, reinterpret_cast<uintptr_t>(stream)};
    std::vector<AclsanResourceData> allocated;
    bool insertedImage = false;
    try {
        TraceRuntimeState& state = State();
        std::unique_lock<std::mutex> lock(state.mutex);
        try {
            if (state.loadedImageDispatching.count(imageKey) != 0U && traceDispatchActive)
                return false;
            state.loadedImageWake.wait(lock, [&state, &imageKey] {
                return !state.accepting || state.loadedImageDispatching.count(imageKey) == 0U;
            });
            if (!state.accepting)
                return false;
            const auto existing = state.loadedImages.find(imageKey);
            if (existing != state.loadedImages.end()) {
                return RetainLoadedImageLocked(state, imageKey, streamKey);
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
            const auto inserted = state.loadedImages.emplace(imageKey, std::move(image)).first;
            insertedImage = true;
            state.loadedImageLaunches[imageKey] = 1;
            state.streamLoadedImageLaunches[streamKey][imageKey] = 1;
            AppendLoadedImageEvents(inserted->second, allocated);
            if (!allocated.empty()) {
                state.loadedImageDispatching.insert(imageKey);
                ++state.activeLoadedImageDispatches;
            }
        } catch (...) {
            // Roll back while the registration lock is still held.
            if (insertedImage) {
                state.loadedImages.erase(imageKey);
                state.loadedImageLaunches.erase(imageKey);
            }
            const auto streamEntry = state.streamLoadedImageLaunches.find(streamKey);
            if (streamEntry != state.streamLoadedImageLaunches.end()) {
                if (insertedImage) {
                    streamEntry->second.erase(imageKey);
                }
                if (streamEntry->second.empty()) {
                    state.streamLoadedImageLaunches.erase(streamEntry);
                }
            }
            throw;
        }
    } catch (...) {
        ASCTOOL_ERROR("acl_san loaded image registration failed: binary=%p", reinterpret_cast<void*>(binary));
        return false;
    }
    DispatchLoadedImageEvents(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, allocated, &imageKey, 1U);
    return true;
}

void AcquireTraceLaunchResources(const PreparedTraceLaunch& prepared, const void* function, const void* stream) noexcept
{
    // 只有插桩 launch 才会产生设备侧分析记录，因此只在该分析窗口内登记 loaded-image 区间。
    if (!prepared.instrumented || function == nullptr) {
        return;
    }
    try {
        auto pending = std::static_pointer_cast<PendingTrace>(prepared.reservation);
        if (pending == nullptr) {
            TraceIntegrityMark(prepared.launchId, 0, true);
            return;
        }
        std::lock_guard<std::recursive_mutex> collectionLock(pending->collectionMutex);
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->abandoned || pending->draining || pending->retired) {
                return;
            }
        }
        uintptr_t binary = 0;
        std::vector<BinarySegment> segments;
        if (!DeviceBinaries().GetFunctionActualBinary(
                reinterpret_cast<uintptr_t>(function), binary, segments, static_cast<int32_t>(prepared.deviceId))) {
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
        if (!RegisterTraceLoadedImageResources(prepared.deviceId, binary, base, segments, stream)) {
            std::lock_guard<std::mutex> lock(pending->mutex);
            FailTrace(*pending, "cannot register loaded image resources");
            return;
        }
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->loadedImageBinary = binary;
    } catch (...) {
        TraceIntegrityMark(prepared.launchId, 0, true);
        ASCTOOL_ERROR("acl_san loaded image acquire failed: function=%p", function);
    }
}

// stream 同步后按 stream 扣减在途计数；某个 binary 计数归零时发 free，下次 launch 重新 alloc。
void ReleaseTraceStreamResources(const void* stream) noexcept
{
    const uintptr_t streamAddress = reinterpret_cast<uintptr_t>(stream);
    std::vector<AclsanResourceData> released;
    std::vector<LoadedImageKey> releasedImages;
    try {
        TraceRuntimeState& state = State();
        const int32_t deviceId = CurrentTraceDeviceId();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.accepting) {
            return;
        }
        if (deviceId < 0) {
            for (const auto& entry : state.streamLoadedImageLaunches) {
                if (entry.first.second == streamAddress) {
                    TraceIntegrityMark(0, 0, true);
                    ASCTOOL_ERROR("acl_san loaded image release requires a known device: stream=%p", stream);
                    break;
                }
            }
            return;
        }
        const LoadedStreamKey streamKey{static_cast<uint32_t>(deviceId), streamAddress};
        const auto streamEntry = state.streamLoadedImageLaunches.find(streamKey);
        if (streamEntry == state.streamLoadedImageLaunches.end()) {
            return;
        }
        for (const auto& entry : streamEntry->second) {
            if (state.loadedImageDispatching.count(entry.first) != 0U) {
                TraceIntegrityMark(0, 0, true);
                ASCTOOL_ERROR("acl_san loaded image release overlaps its resource callback: stream=%p", stream);
                return;
            }
        }
        PrepareLoadedImageRelease(state, streamEntry->second, released, releasedImages);
        CommitLoadedImageRelease(state, streamEntry->second);
        state.streamLoadedImageLaunches.erase(streamEntry);
        if (!released.empty()) {
            ++state.activeLoadedImageDispatches;
        }
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
        ASCTOOL_ERROR("acl_san loaded image release failed: stream=%p", stream);
        return;
    }
    DispatchLoadedImageEvents(ACLSAN_CBID_RESOURCE_MEMORY_FREE, released, releasedImages.data(), releasedImages.size());
}

namespace {

bool ReleaseTraceLaunchResources(PendingTrace& pending) noexcept
{
    if (pending.loadedImageBinary == 0) {
        return true;
    }
    const LoadedImageKey imageKey{pending.deviceId, pending.loadedImageBinary};
    const LoadedStreamKey streamKey{pending.deviceId, reinterpret_cast<uintptr_t>(pending.stream)};
    std::vector<AclsanResourceData> released;
    std::vector<LoadedImageKey> releasedImages;
    bool invalid = false;
    try {
        TraceRuntimeState& state = State();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            const auto stream = state.streamLoadedImageLaunches.find(streamKey);
            if (stream == state.streamLoadedImageLaunches.end()) {
                invalid = true;
            } else {
                const auto count = stream->second.find(imageKey);
                if (count == stream->second.end() || count->second == 0) {
                    invalid = true;
                } else if (state.loadedImageDispatching.count(imageKey) != 0U) {
                    return false;
                } else {
                    LoadedImageLaunchCounts oneLaunch{{imageKey, 1U}};
                    PrepareLoadedImageRelease(state, oneLaunch, released, releasedImages);
                    CommitLoadedImageRelease(state, oneLaunch);
                    if (--count->second == 0) {
                        stream->second.erase(count);
                    }
                    if (stream->second.empty()) {
                        state.streamLoadedImageLaunches.erase(stream);
                    }
                    if (!released.empty()) {
                        ++state.activeLoadedImageDispatches;
                    }
                }
            }
        }
        if (invalid) {
            std::lock_guard<std::mutex> lock(pending.mutex);
            FailTrace(pending, "loaded image launch accounting missing");
            return false;
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(pending.mutex);
        FailTrace(pending, "loaded image launch release failed");
        return false;
    }
    {
        TraceDispatchScope callbackScope;
        DispatchLoadedImageEvents(
            ACLSAN_CBID_RESOURCE_MEMORY_FREE, released, releasedImages.data(), releasedImages.size());
    }
    pending.loadedImageBinary = 0;
    return true;
}

std::optional<size_t> FindTraceArgumentInsertionOffset(
    size_t argsSize, const aclrtPlaceHolderInfo* placeholders, size_t placeholderCount)
{
    if (placeholderCount == 0) {
        constexpr size_t kArgumentAlignmentMask = 7U;
        if (argsSize > std::numeric_limits<size_t>::max() - kArgumentAlignmentMask) {
            return std::nullopt;
        }
        return (argsSize + kArgumentAlignmentMask) & ~kArgumentAlignmentMask;
    }
    uint32_t lastAddressOffset = 0;
    for (size_t i = 0; i < placeholderCount; ++i) {
        lastAddressOffset = std::max(lastAddressOffset, placeholders[i].addrOffset);
    }
    if (lastAddressOffset > std::numeric_limits<uint32_t>::max() - sizeof(void*)) {
        return std::nullopt;
    }
    const size_t insertionOffset = static_cast<size_t>(lastAddressOffset) + sizeof(void*);
    if (insertionOffset > argsSize) {
        return std::nullopt;
    }
    return insertionOffset;
}

// 为已经DBI插桩的kernel扩展HostArgs，在原始参数中插入隐藏的device trace buffer指针
aclError ExpandArguments(
    const void* hostArgs, size_t argsSize, const aclrtPlaceHolderInfo* placeholders, size_t placeholderCount,
    uint32_t traceArgumentOffset, PreparedTraceLaunch& prepared)
{
    if ((argsSize != 0 && hostArgs == nullptr) || (placeholderCount != 0 && placeholders == nullptr)) {
        return ACL_ERROR_INVALID_PARAM;
    }
    const auto insertion = FindTraceArgumentInsertionOffset(argsSize, placeholders, placeholderCount);
    if (!insertion.has_value()) {
        return ACL_ERROR_INVALID_PARAM;
    }
    const size_t insertionOffset = *insertion;

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

using AsyncCopy = aclError (*)(void*, size_t, const void*, size_t, aclrtMemcpyKind, aclrtStream);
using ContextFunction = aclError (*)(aclrtContext);
using GetContextFunction = aclError (*)(aclrtContext*);

bool ComposeTracePath(const char* root, const char* suffix, char (&path)[PATH_MAX]) noexcept
{
    const int length = std::snprintf(path, sizeof(path), "%s/%s", root, suffix);
    return length >= 0 && static_cast<size_t>(length) < sizeof(path);
}

bool HasCannInstallMarker(const char* root) noexcept
{
    constexpr const char* markers[] = {
        "version.cfg", "ascend_toolkit_install.info", "x86_64-linux/version.cfg",
        "x86_64-linux/ascend_toolkit_install.info"};
    for (const char* marker : markers) {
        char path[PATH_MAX];
        struct stat info {};
        if (ComposeTracePath(root, marker, path) && stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
            return true;
        }
    }
    return false;
}

bool IsTraceProviderInRoot(const char* providerDirectory, const char* root) noexcept
{
    char canonicalRoot[PATH_MAX];
    if (root == nullptr || realpath(root, canonicalRoot) == nullptr || !HasCannInstallMarker(canonicalRoot)) {
        return false;
    }
    constexpr const char* libraryDirectories[] = {"lib64", "x86_64-linux/lib64"};
    for (const char* suffix : libraryDirectories) {
        char path[PATH_MAX];
        char canonicalDirectory[PATH_MAX];
        const size_t rootLength = std::strlen(canonicalRoot);
        if (ComposeTracePath(canonicalRoot, suffix, path) && realpath(path, canonicalDirectory) != nullptr &&
            std::strncmp(canonicalDirectory, canonicalRoot, rootLength) == 0 && canonicalDirectory[rootLength] == '/' &&
            std::strcmp(providerDirectory, canonicalDirectory) == 0) {
            return true;
        }
    }
    return false;
}

bool IsOfficialTraceProviderName(const char* name) noexcept
{
    constexpr const char* providers[] = {"libascendcl.so", "libacl_rt.so", "libruntime.so"};
    for (const char* provider : providers) {
        const size_t length = std::strlen(provider);
        if (std::strncmp(name, provider, length) == 0 && (name[length] == '\0' || name[length] == '.')) {
            return true;
        }
    }
    return false;
}

bool IsTraceProviderInDefaultRoot(const char* directory) noexcept
{
    glob_t matches{};
    const int result = glob("/usr/local/Ascend/cann-*", GLOB_ONLYDIR, nullptr, &matches);
    bool accepted = false;
    if (result == 0) {
        for (size_t i = 0; i < matches.gl_pathc && !accepted; ++i) {
            accepted = IsTraceProviderInRoot(directory, matches.gl_pathv[i]);
        }
    }
    globfree(&matches);
    return accepted;
}

bool ValidateTraceProviderPath(const char* path, char* canonicalPath, size_t canonicalPathSize) noexcept
{
    if (canonicalPath == nullptr || canonicalPathSize == 0U) {
        return false;
    }
    canonicalPath[0] = '\0';
    char resolved[PATH_MAX];
    if (path == nullptr || realpath(path, resolved) == nullptr) {
        return false;
    }
    const size_t length = std::strlen(resolved);
    if (length >= canonicalPathSize) {
        return false;
    }
    std::memcpy(canonicalPath, resolved, length + 1U);
    char* name = std::strrchr(resolved, '/');
    if (name == nullptr || !IsOfficialTraceProviderName(name + 1)) {
        return false;
    }
    *name = '\0';
    const char* root = std::getenv("ASCEND_HOME_PATH");
    return root != nullptr && root[0] != '\0' ? IsTraceProviderInRoot(resolved, root) :
                                                IsTraceProviderInDefaultRoot(resolved);
}

struct RuntimeFunctionMetaApi {
    int32_t (*getSize)(const void*, uint32_t, size_t*) = nullptr;
    int32_t (*getData)(const void*, uint32_t, void*, uint32_t) = nullptr;
};

uint32_t RuntimeFunctionMetaTypeForKernelType() noexcept { return 1U; }

uint32_t RuntimeFunctionMetaTypeForMixTaskRation() noexcept { return 3U; }

const RuntimeFunctionMetaApi& TraceRuntimeFunctionMetaApi() noexcept
{
    static const RuntimeFunctionMetaApi api = []() -> RuntimeFunctionMetaApi {
        RuntimeFunctionMetaApi resolved{};
        void* sizeSymbol = dlsym(RTLD_DEFAULT, "rtFunctionGetMetaInfoSize");
        void* dataSymbol = dlsym(RTLD_DEFAULT, "rtFunctionGetMetaInfo");
        if (sizeSymbol == nullptr || dataSymbol == nullptr) {
            ASCTOOL_WARNING("acl_san trace: runtime function meta API unavailable; kernel end detection disabled");
            return resolved;
        }
        Dl_info info{};
        char canonical[PATH_MAX]{};
        if (dladdr(dataSymbol, &info) == 0 || info.dli_fname == nullptr ||
            !IsTrustedTraceCopyProvider(info.dli_fname, canonical, sizeof(canonical))) {
            ASCTOOL_WARNING("acl_san trace: untrusted runtime function meta provider; kernel end detection disabled");
            return resolved;
        }
        resolved.getSize = reinterpret_cast<int32_t (*)(const void*, uint32_t, size_t*)>(sizeSymbol);
        resolved.getData = reinterpret_cast<int32_t (*)(const void*, uint32_t, void*, uint32_t)>(dataSymbol);
        ASCTOOL_INFO("acl_san trace: runtime function meta provider=%s", canonical);
        return resolved;
    }();
    return api;
}

template <typename Value>
bool ReadRuntimeFunctionMeta(const void* function, uint32_t metaType, Value& value) noexcept
{
    const RuntimeFunctionMetaApi& api = TraceRuntimeFunctionMetaApi();
    if (api.getSize == nullptr || api.getData == nullptr || function == nullptr) {
        return false;
    }
    size_t size = 0U;
    if (api.getSize(function, metaType, &size) != 0 || size != sizeof(Value)) {
        return false;
    }
    Value loaded{};
    if (api.getData(function, metaType, &loaded, sizeof(Value)) != 0) {
        return false;
    }
    value = loaded;
    return true;
}

struct RuntimeMixTaskRation {
    uint16_t aicRatio;
    uint16_t aivRatio;
};

TraceKernelCoreType TraceKernelCoreTypeFromMeta(uint32_t metaKernelType) noexcept
{
    switch (metaKernelType) {
        case static_cast<uint32_t>(TraceKernelCoreType::Aicore):
            return TraceKernelCoreType::Aicore;
        case static_cast<uint32_t>(TraceKernelCoreType::Aic):
            return TraceKernelCoreType::Aic;
        case static_cast<uint32_t>(TraceKernelCoreType::Aiv):
            return TraceKernelCoreType::Aiv;
        case static_cast<uint32_t>(TraceKernelCoreType::MixCubeMain):
            return TraceKernelCoreType::MixCubeMain;
        case static_cast<uint32_t>(TraceKernelCoreType::MixVectorMain):
            return TraceKernelCoreType::MixVectorMain;
        default:
            return TraceKernelCoreType::Unknown;
    }
}

void ResolveKernelEndExpectation(void* function, uint32_t blockCount, PreparedTraceLaunch& prepared) noexcept
{
    uint32_t metaKernelType = 0U;
    if (!ReadRuntimeFunctionMeta(function, RuntimeFunctionMetaTypeForKernelType(), metaKernelType)) {
        return;
    }
    const TraceKernelCoreType type = TraceKernelCoreTypeFromMeta(metaKernelType);
    if (type == TraceKernelCoreType::Unknown) {
        return;
    }
    uint32_t aicRatio = 0U;
    uint32_t aivRatio = 0U;
    if (type == TraceKernelCoreType::MixCubeMain || type == TraceKernelCoreType::MixVectorMain) {
        RuntimeMixTaskRation ration{};
        if (!ReadRuntimeFunctionMeta(function, RuntimeFunctionMetaTypeForMixTaskRation(), ration)) {
            ASCTOOL_WARNING(
                "acl_san trace: mix kernel task ration unavailable; kernel end detection disabled for launch=%llu",
                prepared.launchId);
            return;
        }
        aicRatio = ration.aicRatio;
        aivRatio = ration.aivRatio;
    }
    const TraceKernelCoreExpectation expectation = MakeTraceKernelCoreExpectation(type, blockCount, aicRatio, aivRatio);
    if (!expectation.known) {
        ASCTOOL_DEBUG(
            "acl_san trace: kernel end detection disabled for launch=%llu (meta_type=%u blockCount=%u aicRatio=%u "
            "aivRatio=%u)",
            prepared.launchId, metaKernelType, blockCount, aicRatio, aivRatio);
        return;
    }
    prepared.kernelEndExpectation = expectation;
}

template <typename Function>
Function ResolveTraceCompletionFunction(const char* name) noexcept
{
    void* symbol = dlsym(RTLD_DEFAULT, name);
    Dl_info info{};
    char canonical[PATH_MAX]{};
    if (symbol == nullptr || dladdr(symbol, &info) == 0 || info.dli_fname == nullptr ||
        !IsTrustedTraceCopyProvider(info.dli_fname, canonical, sizeof(canonical))) {
        return nullptr;
    }
    return reinterpret_cast<Function>(symbol);
}

const TraceCompletionEventApi& CompletionEventApi() noexcept
{
    static const TraceCompletionEventApi api = [] {
        TraceCompletionEventApi resolved{};
        resolved.create = ResolveTraceCompletionFunction<decltype(resolved.create)>("aclrtCreateEvent");
        resolved.record = ResolveTraceCompletionFunction<decltype(resolved.record)>("aclrtRecordEvent");
        resolved.query = ResolveTraceCompletionFunction<decltype(resolved.query)>("aclrtQueryEventStatus");
        resolved.destroy = ResolveTraceCompletionFunction<decltype(resolved.destroy)>("aclrtDestroyEvent");
        if (!resolved.Available()) {
            ASCTOOL_WARNING("acl_san trace: completion event unavailable; waiting for stream synchronization");
        }
        return resolved;
    }();
    return api;
}

AsyncCopy ResolveAsyncCopy() noexcept
{
    static const AsyncCopy copy = []() -> AsyncCopy {
        void* symbol = dlsym(RTLD_DEFAULT, "aclrtMemcpyAsync");
        Dl_info info{};
        if (symbol == nullptr || dladdr(symbol, &info) == 0 || info.dli_fname == nullptr) {
            ASCTOOL_ERROR("acl_san trace: cannot resolve aclrtMemcpyAsync provider (canonical=<unresolved>); "
                          "accepted providers: libascendcl.so*, libacl_rt.so* in a CANN installation lib64");
            return nullptr;
        }
        char canonical[PATH_MAX]{};
        if (!IsTrustedTraceCopyProvider(info.dli_fname, canonical, sizeof(canonical))) {
            ASCTOOL_ERROR(
                "acl_san trace: unexpected aclrtMemcpyAsync provider %s (canonical=%s); "
                "accepted providers: libascendcl.so*, libacl_rt.so* in a CANN installation lib64",
                info.dli_fname, canonical[0] == '\0' ? "<unresolved>" : canonical);
            return nullptr;
        }
        ASCTOOL_INFO("acl_san trace: aclrtMemcpyAsync=%p provider=%s", symbol, canonical);
        return reinterpret_cast<AsyncCopy>(symbol);
    }();
    return copy;
}

void FailTrace(PendingTrace& pending, const char* reason) noexcept
{
    if (!pending.failed) {
        pending.failed = true;
        TraceIntegrityMark(pending.launchId, 0U, true);
        ASCTOOL_ERROR(
            "acl_san trace: launch=%llu failed: %s", static_cast<unsigned long long>(pending.launchId), reason);
    }
}

bool BindTraceContext(PendingTrace& pending) noexcept
{
    const auto setContext = reinterpret_cast<ContextFunction>(dlsym(RTLD_DEFAULT, "aclrtSetCurrentContext"));
    if (setContext == nullptr || pending.context == nullptr || setContext(pending.context) != ACL_SUCCESS) {
        FailTrace(pending, "cannot bind launch context");
        return false;
    }
    return true;
}

bool EnsureCopyStream(PendingTrace& pending) noexcept
{
    if (pending.copyStream != nullptr) {
        return true;
    }
    const auto create =
        reinterpret_cast<aclrtCreateStreamFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtCreateStream));
    if (create == nullptr || ResolveAsyncCopy() == nullptr || create(&pending.copyStream) != ACL_SUCCESS ||
        pending.copyStream == nullptr) {
        FailTrace(pending, "cannot create trace copy stream");
        return false;
    }
    return true;
}

bool CopyTraceBytes(
    PendingTrace& pending, void* destination, size_t destinationBytes, const void* source, size_t bytes,
    aclrtMemcpyKind kind) noexcept
{
    const auto sync = reinterpret_cast<aclrtSynchronizeStreamWithTimeoutFunc>(
        acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtSynchronizeStreamWithTimeout));
    const AsyncCopy copy = ResolveAsyncCopy();
    if (sync == nullptr || copy == nullptr || !EnsureCopyStream(pending)) {
        FailTrace(pending, "copy API unavailable");
        return false;
    }
    if (copy(destination, destinationBytes, source, bytes, kind, pending.copyStream) != ACL_SUCCESS) {
        ++pending.enqueueFailures;
        if (pending.enqueueFailures >= TRACE_ENQUEUE_FAILURE_LIMIT) {
            FailTrace(pending, "asynchronous copy enqueue failed for three rounds");
        }
        return false;
    }
    aclError status = ACL_ERROR_FAILURE;
    for (int32_t multiplier = 1; multiplier <= TRACE_SYNC_ATTEMPT_LIMIT; ++multiplier) {
        status = sync(pending.copyStream, TRACE_COPY_TIMEOUT_MS * multiplier);
        if (status == ACL_SUCCESS) {
            return true;
        }
        if (status != ACL_ERROR_RT_STREAM_SYNC_TIMEOUT) {
            break;
        }
    }
    pending.quarantined = true;
    FailTrace(pending, "copy completion not established; stream quarantined");
    return false;
}

const uint8_t* DeviceBytes(const PendingTrace& pending, size_t offset) noexcept
{
    return static_cast<const uint8_t*>(pending.deviceBuffer) + offset;
}

bool ReserveTraceBytes(size_t bytes) noexcept
{
    auto& used = State().queuedBytes;
    size_t old = used.load(std::memory_order_relaxed);
    do {
        if (old > TRACE_GLOBAL_BUDGET || bytes > TRACE_GLOBAL_BUDGET - old) {
            return false;
        }
    } while (!used.compare_exchange_weak(old, old + bytes, std::memory_order_relaxed));
    auto& highWater = State().queueHighWaterBytes;
    size_t previous = highWater.load(std::memory_order_relaxed);
    while (previous < old + bytes &&
           !highWater.compare_exchange_weak(previous, old + bytes, std::memory_order_relaxed)) {
    }
    return true;
}

bool ReadTraceControls(PendingTrace& pending) noexcept
{
    const size_t bytes = sizeof(pending.controls);
    if (!CopyTraceBytes(
            pending, pending.controls.data(), bytes, DeviceBytes(pending, TraceWriteControlOffset(0)), bytes,
            ACL_MEMCPY_DEVICE_TO_HOST)) {
        return false;
    }
    for (uint32_t slot = 0; slot < TRACE_SLOTS; ++slot) {
        const auto& value = pending.controls[slot].write;
        if (value.writeSequence < pending.lastWrite[slot] || value.writeSequence < pending.acked[slot] ||
            value.writeSequence - pending.acked[slot] > ASCSAN_TRACE_RECORDS_PER_CORE ||
            value.droppedCount < pending.lastDropped[slot] ||
            !std::all_of(
                std::begin(value.reserved), std::end(value.reserved), [](uint8_t byte) { return byte == 0; })) {
            FailTrace(pending, "invalid write control");
            return false;
        }
        const uint32_t delta = value.droppedCount - pending.lastDropped[slot];
        pending.dropped += delta;
        pending.lastDropped[slot] = value.droppedCount;
        pending.lastWrite[slot] = value.writeSequence;
        if (delta != 0U) {
            TraceIntegrityMark(pending.launchId, delta, false);
        }
    }
    return true;
}

bool AckTrace(PendingTrace& pending) noexcept
{
    for (uint32_t slot = 0; slot < TRACE_SLOTS; ++slot) {
        if (pending.copied[slot] == pending.acked[slot]) {
            continue;
        }
        pending.reads[slot].readSequence = pending.copied[slot];
        void* destination = static_cast<uint8_t*>(pending.deviceBuffer) + TraceReadControlOffset(slot);
        if (!CopyTraceBytes(
                pending, destination, sizeof(pending.reads[slot]), &pending.reads[slot], sizeof(pending.reads[slot]),
                ACL_MEMCPY_HOST_TO_DEVICE)) {
            return false;
        }
        pending.acked[slot] = pending.copied[slot];
    }
    return true;
}

bool IsTraceBoundaryMarkerRecord(const AclsanRawTraceRecord& record) noexcept
{
    const uint32_t instrId = record.instrId;
    return instrId == static_cast<uint32_t>(InstrType::BEFORE_KERNEL_END) ||
           instrId == static_cast<uint32_t>(InstrType::BEFORE_KERNEL_STACK_FRAME);
}

void TrackTraceKernelMarkers(
    PendingTrace& pending, uint32_t slot, const AclsanRawTraceRecord* records, uint32_t count) noexcept
{
    if (!pending.kernelEndTracker.Known()) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t instrId = records[i].instrId;
        if (instrId == static_cast<uint32_t>(InstrType::BEFORE_KERNEL_STACK_FRAME)) {
            pending.kernelEndTracker.Observe(TraceKernelMarker::BeforeKernelStackFrame, slot, records[i].blockId);
        } else if (instrId == static_cast<uint32_t>(InstrType::BEFORE_KERNEL_END)) {
            pending.kernelEndTracker.Observe(TraceKernelMarker::BeforeKernelEnd, slot, records[i].blockId);
        }
    }
}

void DispatchTraceRecordsBeforeSync(PendingTrace& pending);

bool HasQueuedTraceRecords(const PendingTrace& pending) noexcept
{
    for (const auto& batches : pending.batches) {
        if (!batches.empty() && batches.front().ready) {
            return true;
        }
    }
    return false;
}

bool AllTraceRecordsDispatched(const PendingTrace& pending) noexcept
{
    for (uint32_t slot = 0; slot < TRACE_SLOTS; ++slot) {
        if (pending.dispatched[slot] != pending.copied[slot] || !pending.batches[slot].empty()) {
            return false;
        }
    }
    return true;
}

// 返回各 slot 设备侧已写记录总数之和（writeSequence 单调递增），用于尾部稳定判定。
uint64_t TotalWriteSequences(const PendingTrace& pending) noexcept
{
    uint64_t total = 0;
    for (uint32_t slot = 0; slot < TRACE_SLOTS; ++slot) {
        total += pending.controls[slot].write.writeSequence;
    }
    return total;
}

bool TraceFullyCopied(const PendingTrace& pending) noexcept
{
    for (uint32_t slot = 0; slot < TRACE_SLOTS; ++slot) {
        if (pending.copied[slot] != pending.controls[slot].write.writeSequence ||
            pending.acked[slot] != pending.copied[slot]) {
            return false;
        }
    }
    return true;
}

bool CanPublishBeforeFinalization(const PendingTrace& pending, bool fullyCopied, bool dispatchedAll) noexcept
{
    return pending.completionEvent.Complete() && pending.completionSnapshotRead &&
           pending.kernelEndTracker.Complete() && fullyCopied && dispatchedAll;
}

aclError OperatorExecutionResult(bool failed, bool partial) noexcept
{
    if (partial) {
        return ACL_ERROR_RT_AICORE_EXCEPTION;
    }
    return failed ? ACL_ERROR_FAILURE : ACL_SUCCESS;
}

// Publish only after a recorded runtime event completes and a later control
// snapshot has been fully copied and dispatched. Stream synchronization remains
// the fallback if the event API is unavailable or cannot confirm completion.
void PublishOperatorExecution(PendingTrace& pending, bool finalizing, bool partial = false) noexcept
{
    TraceDispatchScope callbackScope;
    AclsanOperatorExecutionData data{};
    uint32_t aicEnds = 0U;
    uint32_t aivEnds = 0U;
    uint32_t aicExpected = 0U;
    uint32_t aivExpected = 0U;
    {
        std::lock_guard<std::mutex> lock(pending.mutex);
        if (pending.abandoned || pending.operatorExecutionReported) {
            return;
        }
        const bool emptyTrace = pending.delivered == 0U && pending.dropped == 0U && !pending.failed;
        const bool dispatchedAll = AllTraceRecordsDispatched(pending);
        const bool fullyCopied = TraceFullyCopied(pending);
        const bool requiresCompletionEvent = !finalizing && !partial;
        if (requiresCompletionEvent && !CanPublishBeforeFinalization(pending, fullyCopied, dispatchedAll)) {
            return;
        }
        if (requiresCompletionEvent && !pending.completionEvent.Release(CompletionEventApi(), false)) {
            FailTrace(pending, "cannot destroy completion event");
        }
        const bool completeBoundary = pending.kernelEndTracker.Complete() && pending.dropped == 0U && !pending.failed &&
                                      fullyCopied && dispatchedAll;
        pending.operatorExecutionReported = true;
        data.common.version = ACLSAN_API_VERSION;
        data.common.size = static_cast<uint32_t>(sizeof(AclsanOperatorExecutionData));
        data.common.apiName = "operatorExecutionComplete";
        data.common.result = OperatorExecutionResult(pending.failed, partial);
        data.common.correlationId = pending.launchId;
        data.launchId = pending.launchId;
        data.function = const_cast<void*>(pending.function);
        data.stream = const_cast<void*>(pending.stream);
        data.deviceId = pending.deviceId;
        data.numBlocks = pending.blockCount;
        data.flags = 0U;
        if (partial || pending.failed || (!emptyTrace && !completeBoundary)) {
            data.flags |= ACLSAN_OPERATOR_EXECUTION_FLAG_INCOMPLETE;
        }
        if (pending.dropped != 0U) {
            data.flags |= ACLSAN_OPERATOR_EXECUTION_FLAG_DROPPED;
        }
        aicEnds = pending.kernelEndTracker.AicEnds();
        aivEnds = pending.kernelEndTracker.AivEnds();
        aicExpected = pending.kernelEndTracker.Expectation().aicCores;
        aivExpected = pending.kernelEndTracker.Expectation().aivCores;
    }
    AclsanCallbackDispatcher::DispatchOperatorExecution(data);
    ASCTOOL_INFO(
        "acl_san trace: launch=%llu operator execution complete: aic=%u/%u aiv=%u/%u flags=0x%x",
        static_cast<unsigned long long>(data.launchId), aicEnds, aicExpected, aivEnds, aivExpected, data.flags);
}

uint32_t TraceSlotRecordLimit(const TraceKernelCoreExpectation& expectation) noexcept
{
    const uint32_t activeSlots = expectation.aicCores + expectation.aivCores;
    if (!expectation.known || activeSlots == 0U) {
        return TRACE_SLOT_DEFAULT_RECORD_LIMIT;
    }
    constexpr size_t ringBytes = ASCSAN_TRACE_RECORDS_PER_CORE * sizeof(AclsanRawTraceRecord);
    const size_t fairRings = TRACE_GLOBAL_BUDGET / ringBytes / activeSlots;
    return static_cast<uint32_t>(std::clamp<size_t>(fairRings, TRACE_SLOT_DEFAULT_RINGS, TRACE_SLOT_MAX_RINGS)) *
           ASCSAN_TRACE_RECORDS_PER_CORE;
}

bool CopyTraceSlot(PendingTrace& pending, uint32_t slot, uint32_t count)
{
    if (count == 0U || count > pending.slotRecordLimit - pending.queued[slot]) {
        return true;
    }
    const size_t bytes = static_cast<size_t>(count) * sizeof(AclsanRawTraceRecord);
    if (!ReserveTraceBytes(bytes)) {
        ++pending.budgetBackpressure;
        return true;
    }
    try {
        pending.batches[slot].emplace_back();
        TraceBatch& batch = pending.batches[slot].back();
        batch.slot = slot;
        batch.first = pending.copied[slot];
        batch.records.resize(count);
    } catch (...) {
        if (!pending.batches[slot].empty() && pending.batches[slot].back().records.empty()) {
            pending.batches[slot].pop_back();
        }
        State().queuedBytes.fetch_sub(bytes, std::memory_order_relaxed);
        throw;
    }
    TraceBatch& batch = pending.batches[slot].back();
    const uint32_t firstCount =
        std::min(count, ASCSAN_TRACE_RECORDS_PER_CORE - batch.first % ASCSAN_TRACE_RECORDS_PER_CORE);
    const size_t firstBytes = static_cast<size_t>(firstCount) * sizeof(AclsanRawTraceRecord);
    const size_t secondBytes = bytes - firstBytes;
    bool ok = CopyTraceBytes(
        pending, batch.records.data(), bytes, DeviceBytes(pending, TraceRecordOffset(slot, batch.first)), firstBytes,
        ACL_MEMCPY_DEVICE_TO_HOST);
    if (ok && secondBytes != 0U) {
        ok = CopyTraceBytes(
            pending, batch.records.data() + firstCount, secondBytes,
            DeviceBytes(pending, TraceRecordOffset(slot, batch.first + firstCount)), secondBytes,
            ACL_MEMCPY_DEVICE_TO_HOST);
    }
    if (ok) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!ValidateTraceFifoRecord(batch.records[i], slot, batch.first + i, pending.blockCount)) {
                FailTrace(pending, "invalid FIFO record");
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        TrackTraceKernelMarkers(pending, slot, batch.records.data(), count);
    }
    if (!ok) {
        // On an unresolved DMA the in-flight destination belongs to this batch.
        if (!pending.quarantined) {
            pending.batches[slot].pop_back();
            State().queuedBytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
        return false;
    }
    batch.ready = true;
    pending.copied[slot] += count;
    pending.queued[slot] += count;
    pending.queuedRecordCount += count;
    pending.queueHighWaterBytes =
        std::max(pending.queueHighWaterBytes, pending.queuedRecordCount * sizeof(AclsanRawTraceRecord));
    return true;
}

bool PollTrace(PendingTrace& pending)
{
    if (pending.failed || pending.quarantined || !BindTraceContext(pending) || !EnsureCopyStream(pending)) {
        return false;
    }
    if (!AckTrace(pending)) {
        return false;
    }
    if (!CopyTraceBytes(
            pending, &pending.segment, sizeof(pending.segment), DeviceBytes(pending, 0), sizeof(pending.segment),
            ACL_MEMCPY_DEVICE_TO_HOST)) {
        return false;
    }
    if (!ValidateTraceSegmentHeader(pending.segment, pending.blockCount, pending.launchId)) {
        FailTrace(pending, "invalid segment header");
        return false;
    }
    if (!ReadTraceControls(pending)) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (pending.pollCount != 0U) {
        const auto gap = std::chrono::duration_cast<std::chrono::microseconds>(now - pending.lastPoll).count();
        pending.maxPollGapUs = std::max(pending.maxPollGapUs, static_cast<uint64_t>(gap));
    }
    pending.lastPoll = now;
    ++pending.pollCount;
    for (uint32_t offset = 0; offset < TRACE_SLOTS; ++offset) {
        const uint32_t slot = (pending.nextSlot + offset) % TRACE_SLOTS;
        const uint32_t available = pending.controls[slot].write.writeSequence - pending.copied[slot];
        if (available == 0U) {
            continue;
        }
        const uint32_t count = std::min(available, pending.slotRecordLimit - pending.queued[slot]);
        if (count == 0U) {
            ++pending.slotBackpressure;
            continue;
        }
        if (!CopyTraceSlot(pending, slot, count) || !AckTrace(pending)) {
            return false;
        }
    }
    pending.nextSlot = (pending.nextSlot + 1U) % TRACE_SLOTS;
    if (!AckTrace(pending)) {
        return false;
    }
    pending.enqueueFailures = 0;
    return true;
}

void FailContextPeers(
    const std::vector<std::shared_ptr<PendingTrace>>& work, const std::shared_ptr<PendingTrace>& failed,
    aclrtContext context)
{
    for (const auto& other : work) {
        if (other == failed) {
            continue;
        }
        std::lock_guard<std::mutex> lock(other->mutex);
        if (other->context == context) {
            FailTrace(*other, "another launch in this context failed");
        }
    }
}

void UpdateTraceBoundary(PendingTrace& pending) noexcept
{
    if (!pending.kernelEndReported && pending.dropped == 0U && pending.kernelEndTracker.Complete()) {
        pending.kernelEndReported = true;
        pending.operatorQuietSince = std::chrono::steady_clock::now();
        pending.operatorQuietPolls = 0U;
        ASCTOOL_INFO(
            "acl_san trace: launch=%llu kernel end detected from boundary markers: aic=%u/%u aiv=%u/%u",
            static_cast<unsigned long long>(pending.launchId), pending.kernelEndTracker.AicEnds(),
            pending.kernelEndTracker.Expectation().aicCores, pending.kernelEndTracker.AivEnds(),
            pending.kernelEndTracker.Expectation().aivCores);
    }
    const auto now = std::chrono::steady_clock::now();
    const uint64_t writeTotal = TotalWriteSequences(pending);
    if (!pending.kernelEndReported || writeTotal != pending.operatorQuietWrite) {
        pending.operatorQuietWrite = writeTotal;
        pending.operatorQuietSince = now;
        pending.operatorQuietPolls = 0U;
    } else if (now - pending.operatorQuietSince >= TRACE_POLL_INTERVAL) {
        pending.operatorQuietPolls = 1U;
    }
}

void ObserveExceptionTrace(PendingTrace& pending, bool polled) noexcept
{
    if (!pending.executionException) {
        return;
    }
    if (!polled) {
        pending.exceptionQuietSince.reset();
        pending.exceptionTraceStable = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto writeSequence = TotalWriteSequences(pending);
    if (!pending.exceptionQuietSince || writeSequence != pending.exceptionWriteSequence) {
        pending.exceptionWriteSequence = writeSequence;
        pending.exceptionQuietSince = now;
    }
    pending.exceptionTraceStable = now - *pending.exceptionQuietSince >= TRACE_EXCEPTION_QUIET_PERIOD;
}

bool PollPendingTrace(
    const std::shared_ptr<PendingTrace>& pending, const std::vector<std::shared_ptr<PendingTrace>>& work)
{
    bool progress = false;
    bool contextFailed = false;
    aclrtContext context = nullptr;
    {
        std::lock_guard<std::mutex> launchLock(pending->mutex);
        if (!pending->committed || pending->abandoned || pending->draining || pending->resetRetired ||
            pending->retired) {
            return false;
        }
        const auto previous = pending->copied;
        const bool completedBeforePoll = pending->completionEvent.Complete();
        const bool polled = PollTrace(*pending);
        ObserveExceptionTrace(*pending, polled);
        progress = previous != pending->copied;
        contextFailed = pending->failed;
        context = pending->context;
        if (polled && !pending->failed && !pending->quarantined) {
            if (completedBeforePoll) {
                pending->completionSnapshotRead = true;
            } else if (pending->completionEvent.Poll(CompletionEventApi())) {
                // A fresh control read after this observation is required before publishing.
                progress = true;
            } else if (pending->completionEvent.QueryFailed() && !pending->completionQueryReported) {
                pending->completionQueryReported = true;
                ASCTOOL_WARNING("acl_san trace: completion query failed; launch remains incomplete");
            }
            UpdateTraceBoundary(*pending);
        }
    }
    if (contextFailed) {
        FailContextPeers(work, pending, context);
    }
    return progress;
}

void FailTraceWorker(const std::vector<std::shared_ptr<PendingTrace>>& work, const char* reason) noexcept
{
    for (const auto& pending : work) {
        std::lock_guard<std::mutex> lock(pending->mutex);
        FailTrace(*pending, reason);
    }
}

void TraceWorker() noexcept
{
    TraceRuntimeState& state = State();
    bool progress = false;
    for (;;) {
        std::vector<std::shared_ptr<PendingTrace>> work;
        try {
            std::unique_lock<std::mutex> lock(state.mutex);
            if (!progress) {
                state.wake.wait_for(
                    lock, TRACE_POLL_INTERVAL, [&state] { return state.stopping || state.pollRequested; });
            }
            if (state.stopping) {
                return;
            }
            state.pollRequested = false;
            work = state.pending;
            lock.unlock();
            progress = false;
            for (const auto& pending : work) {
                progress = PollPendingTrace(pending, work) || progress;
            }
            RequestTraceDispatch();
        } catch (...) {
            FailTraceWorker(work, "collector exception");
            progress = false;
        }
    }
}

bool PublishStableExceptionTrace(PendingTrace& pending) noexcept
{
    {
        std::lock_guard<std::mutex> lock(pending.mutex);
        if (!pending.exceptionTraceStable || pending.failed || pending.quarantined ||
            pending.operatorExecutionReported || !TraceFullyCopied(pending) || !AllTraceRecordsDispatched(pending)) {
            return false;
        }
        // Analysis can end before the device releases ownership of its buffers.
        pending.draining = true;
    }
    PublishOperatorExecution(pending, false, true);
    {
        std::lock_guard<std::mutex> lock(pending.mutex);
        pending.partialExecutionReported = true;
    }
    State().wake.notify_all();
    return true;
}

void DispatchPendingTrace(const std::shared_ptr<PendingTrace>& item, bool hasPredecessor)
{
    PendingTrace& pending = *item;
    std::unique_lock<std::recursive_mutex> collectionLock(pending.collectionMutex, std::try_to_lock);
    if (!collectionLock.owns_lock() || pending.collecting) {
        return;
    }
    pending.collecting = true;
    TraceCollectionGuard guard{pending.collecting};
    bool hasQueuedRecords = false;
    {
        std::lock_guard<std::mutex> lock(pending.mutex);
        if (!pending.committed || pending.draining || pending.resetQuiesced || pending.resetRetired ||
            pending.retired || pending.failed || pending.quarantined) {
            return;
        }
        if (!BindTraceContext(pending)) {
            return;
        }
        hasQueuedRecords = HasQueuedTraceRecords(pending);
    }
    if (hasQueuedRecords) {
        DispatchTraceRecordsBeforeSync(pending);
    }
    if (hasPredecessor)
        return;
    PublishOperatorExecution(pending, false);
    if (PublishStableExceptionTrace(pending)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pending.mutex);
        if (!pending.operatorExecutionReported || !pending.completionEvent.Complete() || pending.quarantined) {
            return;
        }
    }
    if (!ReleaseTraceLaunchResources(pending)) {
        return;
    }
    TraceCollectionResult collection;
    if (!FinalizeCollectedTrace(pending, collection, true)) {
        return;
    }
    EraseTrace(item);
    ReleaseDeviceBuffer(pending.deviceBuffer);
    ReleaseHostInputs(pending.hostInputs);
    pending.binaryLease.reset();
    CollectRetiredTraceVariants();
}

void TraceDispatchWorker() noexcept
{
    TraceRuntimeState& state = State();
    for (;;) {
        std::vector<std::shared_ptr<PendingTrace>> work;
        try {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.dispatchWake.wait(lock, [&state] { return state.stopping || state.dispatchReady; });
            if (state.stopping) {
                return;
            }
            state.dispatchReady = false;
            work = state.pending;
            lock.unlock();
            std::set<std::pair<uintptr_t, uintptr_t>> pendingStreams;
            for (const auto& pending : work) {
                const auto stream = std::make_pair(
                    reinterpret_cast<uintptr_t>(pending->context), reinterpret_cast<uintptr_t>(pending->stream));
                DispatchPendingTrace(pending, pendingStreams.count(stream) != 0U);
                std::lock_guard<std::mutex> launchLock(pending->mutex);
                if (!pending->retired && !pending->abandoned && !pending->partialExecutionReported)
                    pendingStreams.insert(stream);
            }
        } catch (...) {
            FailTraceWorker(work, "dispatcher exception");
        }
    }
}

void JoinTraceThreads(std::unique_lock<std::mutex>& lifecycle)
{
    TraceRuntimeState& state = State();
    state.joining = true;
    std::thread collector = std::move(state.worker);
    std::thread dispatcher = std::move(state.dispatcher);
    lifecycle.unlock();
    state.wake.notify_all();
    state.dispatchWake.notify_all();
    if (collector.joinable()) {
        collector.join();
    }
    if (dispatcher.joinable()) {
        dispatcher.join();
    }
    lifecycle.lock();
    state.joining = false;
    state.lifecycleWake.notify_all();
}

bool EnsureTraceWorker() noexcept
{
    TraceRuntimeState& state = State();
    std::unique_lock<std::mutex> lifecycle(state.workerMutex);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.accepting || state.joining) {
            return false;
        }
        state.stopping = false;
    }
    if (state.worker.joinable() && state.dispatcher.joinable()) {
        return true;
    }
    try {
        state.worker = std::thread(TraceWorker);
        state.dispatcher = std::thread(TraceDispatchWorker);
        return true;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.stopping = true;
        }
        JoinTraceThreads(lifecycle);
        return false;
    }
}

void StopTraceWorkerIfIdle() noexcept
{
    if (traceDispatchActive) {
        return;
    }
    TraceRuntimeState& state = State();
    std::unique_lock<std::mutex> lifecycle(state.workerMutex);
    if (state.joining) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.pending.empty()) {
            return;
        }
        state.stopping = true;
    }
    JoinTraceThreads(lifecycle);
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

    const int32_t deviceId = CurrentTraceDeviceId();
    if (deviceId < 0) {
        ASCTOOL_ERROR("acl_san trace: cannot query current Device ID");
        return ACL_ERROR_RT_INTERNAL_ERROR;
    }
    prepared.deviceId = static_cast<uint32_t>(deviceId);
    const auto getContext = reinterpret_cast<GetContextFunction>(dlsym(RTLD_DEFAULT, "aclrtGetCurrentContext"));
    if (getContext == nullptr || getContext(&prepared.context) != ACL_SUCCESS || prepared.context == nullptr) {
        ASCTOOL_ERROR("acl_san trace: cannot query current context");
        return ACL_ERROR_RT_INTERNAL_ERROR;
    }
    // dav-3510 keeps the physical ID space and its trace slots even when some cores are disabled.
    prepared.physicalCoreSlotCount = aclsan::ASCSAN_PHYSICAL_CORE_SLOT_COUNT;
    return ACL_SUCCESS;
}

} // namespace

bool IsTrustedTraceCopyProvider(const char* providerPath, char* canonicalPath, size_t canonicalPathSize) noexcept
{
    return ValidateTraceProviderPath(providerPath, canonicalPath, canonicalPathSize);
}

bool TraceHasKnownBinaryHandle(aclrtBinHandle binary) noexcept
{
    return DeviceBinaries().HasKnownBinaryHandle(reinterpret_cast<uintptr_t>(binary));
}

int32_t TraceCurrentDeviceId() noexcept { return CurrentTraceDeviceId(); }

void* VerifiedTraceGetDeviceSymbol(void* symbol) noexcept
{
    Dl_info info{};
    if (symbol == nullptr || dladdr(symbol, &info) == 0 || info.dli_fname == nullptr) {
        return nullptr;
    }
    const char* name = std::strrchr(info.dli_fname, '/');
    name = name == nullptr ? info.dli_fname : name + 1;
    constexpr char LIBRARY_NAME[] = "libascendcl.so";
    constexpr size_t NAME_LENGTH = sizeof(LIBRARY_NAME) - 1U;
    return std::strncmp(name, LIBRARY_NAME, NAME_LENGTH) == 0 &&
                   (name[NAME_LENGTH] == '\0' || name[NAME_LENGTH] == '.') ?
               symbol :
               nullptr;
}

void DispatchUndecodedTrace(const ParsedTraceRecord& parsed)
{
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
}

void RetainTracePb(const ParsedTraceRecord& parsed, const PushPbParamField& push, VfPbState& pbState)
{
    bool retained = false;
    try {
        retained = pbState.Push(parsed, push.values);
    } catch (...) {
        ASCTOOL_ERROR("acl_san VF: cannot retain PB state");
    }
    if (!retained) {
        AclsanDeviceMemoryAccessData lost{};
        lost.header = MakeVfCompletion(parsed, ACLSAN_DEVICE_VF_INPUT_MISSING).header;
        lost.header.size = sizeof(lost);
        lost.header.flags = ACLSAN_DEVICE_EVENT_FLAG_DROPPED_PRIOR;
        AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(lost);
    }
}

void DispatchTraceVf(
    const ParsedTraceRecord& parsed, const VfParamField& vf, const dav3510::Dav3510CoreKey& key,
    const dav3510::Dav3510RegisterStateManager& registerState, VfPbState& pbState, NpubitUbHandle* hibitHandle)
{
    auto completion = MakeVfCompletion(parsed, ACLSAN_DEVICE_VF_INPUT_MISSING);
    const auto current = registerState.Get(key);
    const bool maskRefreshed = current && current->vectorMask.has_value();
    try {
        const auto states = pbState.Consume(parsed);
        if (states && hibitHandle != nullptr && vf.instructionCount != 0) {
            const auto masks = maskRefreshed ? *current->vectorMask : VectorMaskParamField{UINT64_MAX, UINT64_MAX};
            const auto events =
                hibitHandle->Query(parsed, *states, {masks.vectorMask0, masks.vectorMask1}, &completion);
            for (const auto& event : events) {
                std::visit(
                    [](const auto& value) {
                        LogCallbackData(value);
                        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, AclsanDeviceMemoryAccessData>) {
                            AclsanCallbackDispatcher::DispatchDeviceMemoryAccess(value);
                        } else {
                            AclsanCallbackDispatcher::DispatchDeviceBarrier(value);
                        }
                    },
                    event);
            }
        } else {
            ASCTOOL_ERROR("acl_san VF: unavailable handle/PB state or zero instruction count");
        }
    } catch (...) {
        completion.status = ACLSAN_DEVICE_VF_QUERY_FAILED;
        ASCTOOL_ERROR("acl_san VF: failed to expand parent VF");
    }
    if (!maskRefreshed) {
        completion.warningFlags |= static_cast<uint32_t>(ACLSAN_DEVICE_VF_MASK_NOT_REFRESHED);
    }
    LogCallbackData(completion);
    AclsanCallbackDispatcher::DispatchDeviceVf(completion);
}

bool UpdateTraceRegisterState(
    const DeviceInstructionParamField& params, const dav3510::Dav3510CoreKey& key,
    dav3510::Dav3510RegisterStateManager& registerState)
{
    bool stateInstruction = true;
    if (const auto* value = std::get_if<CtrlRegisterParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<VectorMaskWriteParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<FmatrixParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<L3dRptParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<Mte2SourceParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<NdDmaPadCountParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<NdDmaLoopStrideParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<Mte2NzParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<FpcParamField>(&params)) {
        // FPC is published as a state event; it does not change address reconstruction.
        (void)value;
    } else if (const auto* value = std::get_if<ChannelParaParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<Loop3ParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<DmaLoopSizeParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<DmaLoopStrideParamField>(&params)) {
        registerState.Update(key, *value);
    } else if (const auto* value = std::get_if<SetPaddingParamField>(&params)) {
        registerState.Update(key, *value);
    } else {
        stateInstruction = false;
    }
    return stateInstruction;
}

void DispatchTranslatedTrace(
    const ParsedTraceRecord& parsed, const DecodedInstruction& decoded, const dav3510::Dav3510CoreKey& key,
    const dav3510::Dav3510RegisterStateManager& registerState,
    const std::vector<PreparedTraceLaunch::HostInput>* internalInputs,
    const std::vector<InternalMemoryRange>* readOnlyBinaryRanges)
{
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
        TranslateDecodedTraceToCallbackData(parsed, decoded, memoryState, internalInputs, readOnlyBinaryRanges);
    if (!callbackData.has_value()) {
        return;
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

void DispatchTraceRecords(
    const std::vector<ParsedTraceRecord>& records, const aclsan::DeviceInstructionDecoder& decoder,
    const std::vector<PreparedTraceLaunch::HostInput>* internalInputs,
    // NOLINTNEXTLINE(readability-non-const-parameter): registerState aliases and updates the retained state.
    dav3510::Dav3510RegisterStateManager* retainedRegisterState,
    const std::vector<InternalMemoryRange>* readOnlyBinaryRanges, NpubitUbHandle* hibitHandle,
    VfPbState* retainedPbState) noexcept
{
    if (records.empty()) {
        return;
    }

    dav3510::Dav3510RegisterStateManager localRegisterState(records.front().launchId);
    dav3510::Dav3510RegisterStateManager& registerState =
        retainedRegisterState != nullptr ? *retainedRegisterState : localRegisterState;
    VfPbState localPbState;
    VfPbState& pbState = retainedPbState != nullptr ? *retainedPbState : localPbState;
    for (const ParsedTraceRecord& parsed : records) {
        if (!IsDefinedInstructionId(parsed.record.instrId)) {
            continue;
        }
        const auto decoded = decoder.decode(parsed.record);
        if (!decoded.has_value()) {
            DispatchUndecodedTrace(parsed);
            continue;
        }

        const dav3510::Dav3510CoreKey key{parsed.blockType, parsed.blockId, parsed.phyCoreId, parsed.deviceId};
        if (const auto* push = std::get_if<PushPbParamField>(&decoded->params); push != nullptr) {
            LogRawRecord(parsed);
            LogParamField(decoded->params);
            RetainTracePb(parsed, *push, pbState);
            continue;
        }
        if (const auto* vf = std::get_if<VfParamField>(&decoded->params); vf != nullptr) {
            LogRawRecord(parsed);
            LogParamField(decoded->params);
            DispatchTraceVf(parsed, *vf, key, registerState, pbState, hibitHandle);
            continue;
        }
        if (UpdateTraceRegisterState(decoded->params, key, registerState)) {
            LogRawRecord(parsed);
            LogParamField(decoded->params);
            if (const auto stateData = MakeRegisterStateData(parsed, *decoded); stateData.has_value()) {
                LogCallbackData(*stateData);
                AclsanCallbackDispatcher::DispatchDeviceState(*stateData);
            }
            continue;
        }
        DispatchTranslatedTrace(parsed, *decoded, key, registerState, internalInputs, readOnlyBinaryRanges);
    }
}

void RecordTraceLoadedImage(aclrtBinHandle binary, const void* image, size_t bytes) noexcept
{
    if (!DeviceBinaries().RecordLoadedImage(
            reinterpret_cast<uintptr_t>(binary), image, bytes, CurrentTraceDeviceId())) {
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
                reinterpret_cast<uintptr_t>(binary), instrumented, traceArgumentOffset, image, imageBytes,
                CurrentTraceDeviceId())) {
            ASCTOOL_ERROR("acl_san trace: failed to preserve device source for binary %p", binary);
        }
    } catch (...) {
        ASCTOOL_ERROR("acl_san trace: failed to record binary load for %p", binary);
    }
}

void RecordTraceBinaryUnload(aclrtBinHandle binary) noexcept
{
    DeviceBinaries().RecordBinaryUnload(reinterpret_cast<uintptr_t>(binary), CurrentTraceDeviceId());
}

void RecordTraceBinaryFunctionLookup(aclrtBinHandle binary, aclrtFuncHandle function, const char* functionName) noexcept
{
    if (binary == nullptr || function == nullptr) {
        return;
    }
    try {
        DeviceBinaries().RecordBinaryFunctionLookup(
            reinterpret_cast<uintptr_t>(binary), reinterpret_cast<uintptr_t>(function), functionName,
            CurrentTraceDeviceId());
    } catch (...) {
        ASCTOOL_ERROR("acl_san trace: failed to record function %p for binary %p", function, binary);
    }
}

bool GetTraceFunctionName(aclrtFuncHandle function, std::string& functionName) noexcept
{
    return DeviceBinaries().GetFunctionName(
        reinterpret_cast<uintptr_t>(function), functionName, CurrentTraceDeviceId());
}

void RecordTraceFunctionLookup(aclrtFuncHandle function) noexcept
{
    if (function == nullptr) {
        return;
    }
    try {
        DeviceBinaries().RecordLatestBinaryFunctionLookup(
            reinterpret_cast<uintptr_t>(function), CurrentTraceDeviceId());
    } catch (...) {
        ASCTOOL_ERROR("acl_san trace: failed to record function lookup for %p", function);
    }
}

static aclError RecheckTraceLaunchContext(PreparedTraceLaunch& prepared)
{
    const uint32_t originalDevice = prepared.deviceId;
    const aclrtContext originalContext = prepared.context;
    ACLSAN_RETURN_IF_ACL_ERROR(ResolveLaunchContext(prepared), "Failed to recheck trace launch context");
    if (prepared.deviceId != originalDevice || prepared.context != originalContext) {
        return ACL_ERROR_FAILURE;
    }
    return ACL_SUCCESS;
}

aclError PrepareTraceLaunch(
    aclrtFuncHandle function, uint32_t blockCount, const void* hostArgs, size_t argsSize,
    const aclrtPlaceHolderInfo* placeholders, size_t placeholderCount, TraceArgumentMode argumentMode,
    PreparedTraceLaunch& prepared) noexcept
{
    try {
        prepared = {};
        prepared.launchId = AllocateLaunchId();
        const auto lookup = DeviceBinaries().LookupFunctionTraceArgumentOffset(
            reinterpret_cast<uintptr_t>(function), prepared.traceArgumentOffset, CurrentTraceDeviceId());
        if (lookup == device_runtime::DeviceBinaryRegistry::TraceLookup::Unresolved) {
            TraceIntegrityMark(prepared.launchId, 0U, true);
            return ACL_ERROR_FAILURE;
        }
        if (lookup == device_runtime::DeviceBinaryRegistry::TraceLookup::NotInstrumented) {
            return ACL_SUCCESS;
        }
        prepared.instrumented = true;
        prepared.blockCount = blockCount;
        ResolveKernelEndExpectation(reinterpret_cast<void*>(function), blockCount, prepared);
        ACLSAN_RETURN_IF_ACL_ERROR(ResolveLaunchContext(prepared), "Failed to resolve trace launch context");
        const auto gate = DeviceGate(prepared.deviceId);
        auto gateLock = std::make_shared<std::shared_lock<std::shared_mutex>>(*gate, std::try_to_lock);
        if (!gateLock->owns_lock()) {
            return ACL_ERROR_FAILURE;
        }
        prepared.deviceGate = gateLock;
        const aclError contextStatus = RecheckTraceLaunchContext(prepared);
        if (contextStatus != ACL_SUCCESS) {
            return contextStatus;
        }
        prepared.readOnlyBinaryRanges = ResolveReadOnlyBinaryRanges(function);
        prepared.hibitHandle = DeviceBinaries().GetFunctionHibitHandle(
            reinterpret_cast<uintptr_t>(function), static_cast<int32_t>(prepared.deviceId));

        if (argumentMode == TraceArgumentMode::HOST_ARGS) {
            ACLSAN_RETURN_IF_ACL_ERROR(
                ExpandArguments(
                    hostArgs, argsSize, placeholders, placeholderCount, prepared.traceArgumentOffset, prepared),
                "Failed to expand trace launch arguments");
        }

        std::string error;
        // StrictModeEnabled is currently always true. Fail closed before enabling a non-strict path.
        if (!InitializeTraceBuffer(
                prepared.hostBuffer, prepared.physicalCoreSlotCount, blockCount, prepared.launchId, error)) {
            ASCTOOL_ERROR("acl_san trace: cannot initialize launch buffer: %s", error.c_str());
            StoreHiddenPointer(prepared, argumentMode);
            return StrictModeEnabled() ? ACL_ERROR_FAILURE : ACL_SUCCESS;
        }
        const auto mallocFunction = GetOriginalRuntimeFunction<aclrtMallocFunc>(ACL_RT_API_aclrtMalloc, "aclrtMalloc");
        const auto memcpyFunction = GetOriginalRuntimeFunction<aclrtMemcpyFunc>(ACL_RT_API_aclrtMemcpy, "aclrtMemcpy");
        aclError status = mallocFunction(&prepared.deviceBuffer, prepared.hostBuffer.size(), ACL_MEM_MALLOC_HUGE_FIRST);
        if (status == ACL_SUCCESS &&
            (prepared.deviceBuffer == nullptr ||
             reinterpret_cast<uintptr_t>(prepared.deviceBuffer) % alignof(AclsanTraceBufferHeader) != 0U)) {
            status = ACL_ERROR_INVALID_PARAM;
        }
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
        auto pending = std::make_shared<PendingTrace>();
        pending->launchId = prepared.launchId;
        pending->context = prepared.context;
        pending->deviceBuffer = prepared.deviceBuffer;
        pending->blockCount = prepared.blockCount;
        pending->physicalCoreSlotCount = prepared.physicalCoreSlotCount;
        pending->deviceId = prepared.deviceId;
        pending->decoder = prepared.decoder;
        pending->registerState = std::make_unique<dav3510::Dav3510RegisterStateManager>(prepared.launchId);
        bool quiesced = false;
        {
            TraceRuntimeState& state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            quiesced = state.quiesced.count(prepared.deviceId) != 0U;
            if (!quiesced) {
                state.pending.push_back(pending);
            }
        }
        if (quiesced) {
            ReleaseDeviceBuffer(prepared.deviceBuffer);
            prepared.deviceBuffer = nullptr;
            return ACL_ERROR_FAILURE;
        }
        prepared.reservation = pending;
        if (!EnsureTraceWorker()) {
            bool accepting = false;
            {
                TraceRuntimeState& state = State();
                std::lock_guard<std::mutex> lock(state.mutex);
                accepting = state.accepting;
                state.pending.erase(
                    std::remove(state.pending.begin(), state.pending.end(), pending), state.pending.end());
            }
            prepared.reservation.reset();
            ReleaseDeviceBuffer(prepared.deviceBuffer);
            prepared.deviceBuffer = nullptr;
            prepared.instrumented = false;
            prepared.kernelEndExpectation = {};
            if (accepting) {
                // trace 采集已启用却无法启动 worker，按采集失败处理，不能静默丢失本次 launch。
                TraceIntegrityMark(prepared.launchId, 0U, true);
                ASCTOOL_ERROR(
                    "acl_san trace: cannot start trace worker for launch %llu",
                    static_cast<unsigned long long>(prepared.launchId));
            }
            return ACL_ERROR_FAILURE;
        }
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

static bool IsValidHostInputPlaceholder(
    const PreparedTraceLaunch& prepared, const aclrtPlaceHolderInfo& placeholder) noexcept
{
    return placeholder.addrOffset % sizeof(void*) == 0U && placeholder.addrOffset <= prepared.traceArgumentOffset &&
           prepared.traceArgumentOffset - placeholder.addrOffset >= sizeof(void*) &&
           placeholder.dataOffset >= prepared.traceArgumentOffset + sizeof(void*) &&
           placeholder.dataOffset < prepared.arguments.size();
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
            if (!IsValidHostInputPlaceholder(prepared, placeholder)) {
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
        ReleaseHostInputs(prepared.hostInputs);
        return;
    }
    auto pending = std::static_pointer_cast<PendingTrace>(prepared.reservation);
    if (pending == nullptr) {
        TraceIntegrityMark(prepared.launchId, 0U, true);
        ASCTOOL_ERROR("acl_san trace: missing preallocated launch reservation");
        if (launchResult != ACL_SUCCESS) {
            ReleaseDeviceBuffer(prepared.deviceBuffer);
            ReleaseHostInputs(prepared.hostInputs);
        }
        return;
    }
    if (launchResult != ACL_SUCCESS) {
        {
            TraceRuntimeState& state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.pending.erase(std::remove(state.pending.begin(), state.pending.end(), pending), state.pending.end());
        }
        ReleaseDeviceBuffer(prepared.deviceBuffer);
        ReleaseHostInputs(prepared.hostInputs);
        StopTraceWorkerIfIdle();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->function = function;
        pending->stream = stream;
        pending->parameterBytes = prepared.arguments.empty() ?
                                      static_cast<uint64_t>(prepared.traceArgumentOffset) + sizeof(void*) :
                                      prepared.arguments.size();
        pending->binaryLease = std::move(prepared.binaryLease);
        pending->arguments = std::move(prepared.arguments);
        pending->placeholders = std::move(prepared.placeholders);
        pending->hostInputs = std::move(prepared.hostInputs);
        pending->readOnlyBinaryRanges = std::move(prepared.readOnlyBinaryRanges);
        pending->hibitHandle = std::move(prepared.hibitHandle);
        pending->slotRecordLimit = TraceSlotRecordLimit(prepared.kernelEndExpectation);
        pending->kernelEndTracker.Reset(prepared.kernelEndExpectation);
        const auto& completionApi = CompletionEventApi();
        if (!pending->completionEvent.Record(completionApi, reinterpret_cast<aclrtStream>(const_cast<void*>(stream))) &&
            completionApi.Available()) {
            ASCTOOL_WARNING(
                "acl_san trace: launch=%llu cannot record completion event; waiting for stream synchronization",
                static_cast<unsigned long long>(pending->launchId));
        }
        pending->committed = true;
        TraceIntegrityRegisterLaunch(pending->launchId);
    }
    try {
        TraceRuntimeState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.streamFrontiers[reinterpret_cast<uintptr_t>(stream)] = pending->launchId;
    } catch (...) {
        std::lock_guard<std::mutex> lock(pending->mutex);
        FailTrace(*pending, "cannot record stream launch frontier");
    }
    RequestTracePoll();
}

uint64_t TraceStreamFrontier(const void* stream) noexcept
{
    TraceRuntimeState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.streamFrontiers.find(reinterpret_cast<uintptr_t>(stream));
    return it == state.streamFrontiers.end() ? 0U : it->second;
}

namespace {
struct TraceBatchOwner {
    PendingTrace& pending;
    TraceBatch batch;
    ~TraceBatchOwner()
    {
        const size_t count = batch.records.size();
        if (count == 0U) {
            return;
        }
        std::vector<AclsanRawTraceRecord>().swap(batch.records);
        {
            std::lock_guard<std::mutex> lock(pending.mutex);
            pending.queued[batch.slot] -= static_cast<uint32_t>(count);
            pending.queuedRecordCount -= count;
        }
        State().queuedBytes.fetch_sub(count * sizeof(AclsanRawTraceRecord), std::memory_order_relaxed);
        RequestTracePoll();
    }
};

bool TakeTraceBatch(PendingTrace& pending, uint32_t slot, TraceBatch& batch)
{
    std::lock_guard<std::mutex> lock(pending.mutex);
    if (pending.batches[slot].empty() || !pending.batches[slot].front().ready) {
        return false;
    }
    if (pending.batches[slot].front().first != pending.dispatched[slot]) {
        FailTrace(pending, "queued FIFO sequence is not contiguous");
        return false;
    }
    batch = std::move(pending.batches[slot].front());
    pending.batches[slot].pop_front();
    return true;
}

bool DispatchTraceBatch(PendingTrace& pending, const TraceBatch& batch)
{
    const auto started = std::chrono::steady_clock::now();
    auto parsed = DecodeTraceFifoRange(
        reinterpret_cast<const uint8_t*>(batch.records.data()), batch.records.size(), batch.slot, batch.first,
        pending.blockCount, pending.launchId, pending.deviceId);
    if (!parsed.ok) {
        std::lock_guard<std::mutex> lock(pending.mutex);
        FailTrace(pending, "cannot decode queued FIFO records");
        return false;
    }
    parsed.records.erase(
        std::remove_if(
            parsed.records.begin(), parsed.records.end(),
            [](const ParsedTraceRecord& record) { return IsTraceBoundaryMarkerRecord(record.record); }),
        parsed.records.end());
    for (auto& record : parsed.records) {
        record.parameterBytes = pending.parameterBytes;
    }
    if (pending.decoder != nullptr) {
        DispatchTraceRecords(
            parsed.records, *pending.decoder, &pending.hostInputs, pending.registerState.get(),
            &pending.readOnlyBinaryRanges, pending.hibitHandle.get(), &pending.pbState);
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    std::lock_guard<std::mutex> lock(pending.mutex);
    pending.dispatched[batch.slot] += static_cast<uint32_t>(batch.records.size());
    pending.delivered += parsed.records.size();
    pending.maxDispatchBatchUs = std::max(pending.maxDispatchBatchUs, static_cast<uint64_t>(elapsed));
    return true;
}
} // namespace

void DrainTraceQueue(PendingTrace& pending)
{
    std::lock_guard<std::mutex> dispatchLock(pending.dispatchMutex);
    TraceDispatchScope callbackScope;
    for (uint32_t slot = 0; slot < TRACE_SLOTS; ++slot) {
        size_t remaining = 0;
        {
            std::lock_guard<std::mutex> lock(pending.mutex);
            remaining = pending.queued[slot];
        }
        while (remaining != 0U) {
            TraceBatchOwner owner{pending, {}};
            if (!TakeTraceBatch(pending, slot, owner.batch)) {
                break;
            }
            remaining -= owner.batch.records.size();
            if (!DispatchTraceBatch(pending, owner.batch)) {
                return;
            }
        }
    }
}

namespace {
// DrainTraceQueue 在匿名 namespace 之后定义；此处提供 pre-sync 派发入口。
void DispatchTraceRecordsBeforeSync(PendingTrace& pending) { DrainTraceQueue(pending); }
} // namespace

bool DestroyTraceCopyStream(PendingTrace& pending) noexcept
{
    if (pending.copyStream == nullptr) {
        return true;
    }
    const auto destroy =
        reinterpret_cast<aclrtDestroyStreamFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtDestroyStream));
    if (destroy == nullptr || destroy(pending.copyStream) != ACL_SUCCESS) {
        FailTrace(pending, "cannot destroy trace copy stream");
        return false;
    }
    pending.copyStream = nullptr;
    return true;
}

bool RetryTraceQuarantine(PendingTrace& pending) noexcept
{
    if (!pending.quarantined) {
        return true;
    }
    const auto sync = reinterpret_cast<aclrtSynchronizeStreamWithTimeoutFunc>(
        acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtSynchronizeStreamWithTimeout));
    if (!BindTraceContext(pending) || sync == nullptr || pending.copyStream == nullptr ||
        sync(pending.copyStream, TRACE_COPY_TIMEOUT_MS) != ACL_SUCCESS) {
        return false;
    }
    pending.quarantined = false;
    for (uint32_t slot = 0; slot < TRACE_SLOTS; ++slot) {
        if (!pending.batches[slot].empty() && !pending.batches[slot].back().ready) {
            State().queuedBytes.fetch_sub(
                pending.batches[slot].back().records.size() * sizeof(AclsanRawTraceRecord), std::memory_order_relaxed);
            pending.batches[slot].pop_back();
        }
    }
    return true;
}

void ClearTraceBatches(PendingTrace& pending) noexcept
{
    for (auto& batches : pending.batches) {
        for (const auto& batch : batches) {
            State().queuedBytes.fetch_sub(
                batch.records.size() * sizeof(AclsanRawTraceRecord), std::memory_order_relaxed);
        }
        batches.clear();
    }
    pending.queued.fill(0U);
    pending.queuedRecordCount = 0;
}

void EraseTrace(const std::shared_ptr<PendingTrace>& pending)
{
    TraceRuntimeState& state = State();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending.erase(std::remove(state.pending.begin(), state.pending.end(), pending), state.pending.end());
    }
    state.wake.notify_all();
}

void FinalDrainTrace(PendingTrace& pending)
{
    auto progressAt = std::chrono::steady_clock::now();
    for (;;) {
        bool done = false;
        bool stalled = false;
        {
            std::lock_guard<std::mutex> lock(pending.mutex);
            if (!RetryTraceQuarantine(pending) || pending.failed) {
                break;
            }
            const auto previous = pending.copied;
            if (!PollTrace(pending)) {
                if (!pending.failed && pending.enqueueFailures != 0U) {
                    continue;
                }
                break;
            }
            done = TraceFullyCopied(pending);
            stalled = !done && pending.copied == previous && !HasQueuedTraceRecords(pending);
            if (!stalled) {
                progressAt = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - progressAt >= TRACE_DRAIN_STALL_TIMEOUT) {
                FailTrace(pending, "trace queue budget made no progress during final drain");
                break;
            }
        }
        DrainTraceQueue(pending);
        {
            std::lock_guard<std::mutex> lock(pending.mutex);
            done = done && AllTraceRecordsDispatched(pending);
        }
        if (done) {
            break;
        }
        if (stalled) {
            RequestTraceDispatch();
            auto& state = State();
            std::unique_lock<std::mutex> lock(state.mutex);
            const size_t previousBytes = state.queuedBytes.load(std::memory_order_relaxed);
            state.wake.wait_for(lock, TRACE_POLL_INTERVAL, [&state, previousBytes] {
                return state.stopping || state.queuedBytes.load(std::memory_order_relaxed) < previousBytes;
            });
            if (state.stopping) {
                lock.unlock();
                std::lock_guard<std::mutex> launchLock(pending.mutex);
                FailTrace(pending, "trace collection stopped while waiting for final drain budget");
                break;
            }
        }
    }
    DrainTraceQueue(pending);
}

void LogTraceQueueStats(const PendingTrace& pending) noexcept
{
    ASCTOOL_INFO(
        "acl_san trace: launch=%llu queue_high_water_bytes=%zu global_queue_high_water_bytes=%zu "
        "raw_budget_bytes=%zu slot_record_limit=%u dropped=%llu slot_backpressure=%llu budget_backpressure=%llu "
        "polls=%llu max_poll_gap_us=%llu max_dispatch_batch_us=%llu",
        static_cast<unsigned long long>(pending.launchId), pending.queueHighWaterBytes,
        State().queueHighWaterBytes.load(std::memory_order_relaxed), TRACE_GLOBAL_BUDGET, pending.slotRecordLimit,
        static_cast<unsigned long long>(pending.dropped), static_cast<unsigned long long>(pending.slotBackpressure),
        static_cast<unsigned long long>(pending.budgetBackpressure), static_cast<unsigned long long>(pending.pollCount),
        static_cast<unsigned long long>(pending.maxPollGapUs),
        static_cast<unsigned long long>(pending.maxDispatchBatchUs));
}

bool FinalizeCollectedTrace(PendingTrace& pending, TraceCollectionResult& collection, bool executionComplete)
{
    std::lock_guard<std::mutex> lock(pending.mutex);
    collection.recordCount += pending.delivered;
    collection.droppedRecordCount += pending.dropped;
    if (!executionComplete || pending.quarantined || !BindTraceContext(pending)) {
        return false;
    }
    if (!pending.completionEvent.Release(CompletionEventApi(), true)) {
        FailTrace(pending, "cannot destroy completion event");
        return false;
    }
    const bool release = DestroyTraceCopyStream(pending);
    pending.retired = release;
    if (!release) {
        return false;
    }
    if (pending.abandoned) {
        ClearTraceBatches(pending);
        return true;
    }
    if (pending.partialExecutionReported) {
        ClearTraceBatches(pending);
        return true;
    }
    if (!pending.failed && pending.dropped == 0U && pending.delivered == 0U) {
        // 部分 kernel 不含任何可插桩指令，空 trace 属正常情况；与上游语义一致，不计采集失败。
        TraceStatusCompleteLaunch();
        ASCTOOL_WARNING(
            "acl_san trace: launch=%llu produced no trace records (kernel has no instrumentation points)",
            static_cast<unsigned long long>(pending.launchId));
    } else if (
        !pending.failed && pending.dropped == 0U && TraceFullyCopied(pending) && pending.dispatched == pending.copied) {
        ++collection.completeLaunchCount;
        TraceStatusCompleteLaunch();
        ASCTOOL_INFO(
            "acl_san trace: launch=%llu status=complete records=%zu newRecords=%zu dropped=%llu "
            "physicalCoreSlots=%u blocks=%u",
            static_cast<unsigned long long>(pending.launchId), static_cast<size_t>(pending.delivered),
            static_cast<size_t>(pending.delivered), static_cast<unsigned long long>(pending.dropped),
            pending.physicalCoreSlotCount, pending.blockCount);
    } else if (!pending.failed && pending.dropped == 0U) {
        FailTrace(pending, "trace collection was not fully dispatched");
    }
    LogTraceQueueStats(pending);
    ClearTraceBatches(pending);
    return true;
}

static bool CollectPendingTrace(
    PendingTrace& pending, TraceCollectionResult& collection, bool abandoned, bool executionComplete)
{
    if (abandoned) {
        if (!executionComplete) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(pending.mutex);
            (void)RetryTraceQuarantine(pending);
        }
        TraceCollectionResult discarded;
        return FinalizeCollectedTrace(pending, discarded, true);
    }
    DrainTraceQueue(pending);
    if (executionComplete) {
        if (!pending.partialExecutionReported) {
            FinalDrainTrace(pending);
        }
        // Checkers need the image ranges until they finish analyzing this launch.
        PublishOperatorExecution(pending, true);
        if (!ReleaseTraceLaunchResources(pending)) {
            return false;
        }
    }
    return FinalizeCollectedTrace(pending, collection, executionComplete);
}

TraceCollectionResult CollectTraceStream(aclrtStream stream, bool executionComplete) noexcept
{
    TraceCollectionResult collection;
    if (traceDispatchActive) {
        TraceIntegrityMark(0, 0, true);
        ASCTOOL_ERROR("acl_san trace: collection reentered from a trace callback");
        return collection;
    }
    try {
        const auto getContext = reinterpret_cast<GetContextFunction>(dlsym(RTLD_DEFAULT, "aclrtGetCurrentContext"));
        aclrtContext context = nullptr;
        if (getContext == nullptr || getContext(&context) != ACL_SUCCESS || context == nullptr) {
            TraceIntegrityMark(0, 0, true);
            return collection;
        }
        std::vector<std::shared_ptr<PendingTrace>> work;
        {
            TraceRuntimeState& state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            work = state.pending;
        }
        for (const auto& pending : work) {
            if (pending->context != context) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                if (!pending->committed || pending->stream != stream) {
                    continue;
                }
            }
            std::lock_guard<std::recursive_mutex> collectionLock(pending->collectionMutex);
            if (pending->collecting) {
                continue;
            }
            pending->collecting = true;
            TraceCollectionGuard guard{pending->collecting};
            bool abandoned = false;
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                if (!pending->committed || pending->retired || pending->resetQuiesced || pending->stream != stream ||
                    pending->context != context) {
                    continue;
                }
                if (executionComplete) {
                    pending->draining = true;
                }
                abandoned = pending->abandoned;
                if (!abandoned) {
                    ++collection.launchCount;
                }
            }
            if (CollectPendingTrace(*pending, collection, abandoned, executionComplete)) {
                EraseTrace(pending);
                ReleaseDeviceBuffer(pending->deviceBuffer);
                ReleaseHostInputs(pending->hostInputs);
            }
        }
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
        ASCTOOL_ERROR("acl_san trace: collection failed");
    }
    StopTraceWorkerIfIdle();
    return collection;
}

namespace {
std::vector<std::shared_ptr<PendingTrace>> MarkExceptionTraces(
    aclrtContext context, aclrtStream stream, uint64_t frontier)
{
    std::vector<std::shared_ptr<PendingTrace>> work;
    auto& state = State();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        work = state.pending;
    }
    work.erase(
        std::remove_if(
            work.begin(), work.end(),
            [context, stream, frontier](const auto& item) {
                std::lock_guard<std::mutex> lock(item->mutex);
                if (item->context != context || item->stream != stream || item->launchId > frontier ||
                    !item->committed || item->retired || item->abandoned || item->draining ||
                    item->operatorExecutionReported) {
                    return true;
                }
                item->executionException = true;
                return false;
            }),
        work.end());
    return work;
}

bool ExceptionTraceSettled(const std::shared_ptr<PendingTrace>& pending)
{
    std::lock_guard<std::mutex> lock(pending->mutex);
    return pending->partialExecutionReported || pending->retired || pending->abandoned || pending->failed ||
           pending->quarantined || pending->resetRetired;
}
} // namespace

void CollectTraceAfterExecutionException(aclrtStream stream, uint64_t frontier) noexcept
{
    if (traceDispatchActive || frontier == 0U) {
        return;
    }
    try {
        const auto getContext = reinterpret_cast<GetContextFunction>(dlsym(RTLD_DEFAULT, "aclrtGetCurrentContext"));
        aclrtContext context = nullptr;
        if (getContext == nullptr || getContext(&context) != ACL_SUCCESS || context == nullptr) {
            return;
        }
        const auto work = MarkExceptionTraces(context, stream, frontier);
        auto& state = State();
        const auto deadline = std::chrono::steady_clock::now() + TRACE_EXCEPTION_WAIT_LIMIT;
        while (!std::all_of(work.begin(), work.end(), ExceptionTraceSettled)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                ASCTOOL_WARNING("acl_san trace: exception trace did not settle before the collection deadline");
                break;
            }
            std::unique_lock<std::mutex> lock(state.mutex);
            if (state.stopping) {
                break;
            }
            state.wake.wait_until(lock, std::min(deadline, now + TRACE_POLL_INTERVAL));
        }
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
        ASCTOOL_ERROR("acl_san trace: exception trace collection failed");
    }
}

bool IsTraceDispatchActive() noexcept { return traceDispatchActive; }

void StartTraceWorkerAcceptance() noexcept
{
    TraceRuntimeState& state = State();
    std::lock_guard<std::mutex> lifecycle(state.workerMutex);
    if (state.joining) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    state.accepting = true;
}

namespace {
bool HasTraceWorkToDrain()
{
    std::vector<std::shared_ptr<PendingTrace>> work;
    {
        TraceRuntimeState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        work = state.pending;
    }
    for (const auto& pending : work) {
        std::lock_guard<std::mutex> lock(pending->mutex);
        if (!pending->retired && !pending->abandoned && !pending->partialExecutionReported)
            return true;
    }
    return false;
}
} // namespace

void StopTraceWorker() noexcept
{
    if (traceDispatchActive) {
        TraceIntegrityMark(0, 0, true);
        return;
    }
    TraceRuntimeState& state = State();
    std::unique_lock<std::mutex> lifecycle(state.workerMutex);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.accepting = false;
        state.pollRequested = true;
        state.dispatchReady = true;
    }
    state.wake.notify_all();
    state.dispatchWake.notify_all();
    state.loadedImageWake.notify_all();
    try {
        const auto deadline = std::chrono::steady_clock::now() + TRACE_DRAIN_STALL_TIMEOUT;
        while (state.worker.joinable() && state.dispatcher.joinable() && HasTraceWorkToDrain()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                break;
            std::unique_lock<std::mutex> lock(state.mutex);
            state.wake.wait_until(lock, std::min(deadline, now + TRACE_POLL_INTERVAL));
        }
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.stopping = true;
    }
    state.lifecycleWake.wait(lifecycle, [&state] { return !state.joining; });
    JoinTraceThreads(lifecycle);
    lifecycle.unlock();
    try {
        std::vector<std::shared_ptr<PendingTrace>> work;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            work = state.pending;
        }
        for (const auto& pending : work) {
            std::lock_guard<std::recursive_mutex> collectionLock(pending->collectionMutex);
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (!pending->retired) {
                pending->draining = true;
                pending->abandoned = true;
                FailTrace(*pending, "trace collection stopped before final drain");
                LogTraceQueueStats(*pending);
            }
        }
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.loadedImageWake.wait(lock, [&state] { return state.activeLoadedImageDispatches == 0; });
            state.loadedImages.clear();
            state.loadedImageLaunches.clear();
            state.streamLoadedImageLaunches.clear();
            state.streamFrontiers.clear();
        }
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
    }
}

bool QuiesceTraceDevice(int32_t deviceId, bool canBindContext, std::shared_ptr<void>& resetGuard) noexcept
{
    if (deviceId < 0 || traceDispatchActive) {
        return false;
    }
    try {
        const auto gate = DeviceGate(static_cast<uint32_t>(deviceId));
        auto gateLock = std::make_shared<std::unique_lock<std::shared_mutex>>(*gate, std::try_to_lock);
        if (!gateLock->owns_lock()) {
            return false;
        }
        resetGuard = gateLock;
        TraceRuntimeState& state = State();
        std::vector<std::shared_ptr<PendingTrace>> work;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.quiesced.insert(static_cast<uint32_t>(deviceId));
            work = state.pending;
        }
        for (const auto& pending : work) {
            if (pending->deviceId != static_cast<uint32_t>(deviceId)) {
                continue;
            }
            std::lock_guard<std::recursive_mutex> collectionLock(pending->collectionMutex);
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->retired) {
                continue;
            }
            if (!canBindContext) {
                return false;
            }
            pending->draining = true;
            pending->resetQuiesced = true;
            if (!RetryTraceQuarantine(*pending) || !BindTraceContext(*pending) || !DestroyTraceCopyStream(*pending)) {
                return false;
            }
        }
        return true;
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
        return false;
    }
}

void FinishTraceDeviceReset(int32_t deviceId, bool success) noexcept
{
    try {
        TraceRuntimeState& state = State();
        std::vector<std::shared_ptr<PendingTrace>> work;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            work = state.pending;
        }
        for (const auto& pending : work) {
            if (pending->deviceId != static_cast<uint32_t>(deviceId)) {
                continue;
            }
            std::lock_guard<std::recursive_mutex> collectionLock(pending->collectionMutex);
            bool publish = false;
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                if (success) {
                    pending->resetRetired = true;
                    pending->draining = true;
                    if (pending->committed && !pending->abandoned) {
                        // A successful reset may only decrement a device reference.
                        const bool observedTraceDrained =
                            pending->completionEvent.Complete() && pending->completionSnapshotRead &&
                            TraceFullyCopied(*pending) && AllTraceRecordsDispatched(*pending);
                        if (!pending->failed && pending->dropped == 0U && observedTraceDrained) {
                            TraceStatusCompleteLaunch();
                        } else {
                            FailTrace(*pending, "device reset before final collection");
                        }
                        publish = true;
                    } else {
                        pending->abandoned = true;
                        pending->resetQuiesced = false;
                    }
                } else {
                    pending->draining = pending->abandoned;
                    pending->resetQuiesced = false;
                }
            }
            if (publish) {
                PublishOperatorExecution(*pending, true);
                std::lock_guard<std::mutex> lock(pending->mutex);
                pending->abandoned = true;
                pending->resetQuiesced = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.quiesced.erase(static_cast<uint32_t>(deviceId));
        }
        state.wake.notify_all();
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
    }
}

void ResetTraceRuntimeState() noexcept
{
    if (traceDispatchActive) {
        TraceIntegrityMark(0, 0, true);
        return;
    }
    StopTraceWorker();
    try {
        TraceRuntimeState& state = State();
        std::unique_lock<std::mutex> lifecycle(state.workerMutex);
        std::vector<std::shared_ptr<PendingTrace>> work;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            work = state.pending;
        }
        for (const auto& pending : work) {
            std::lock_guard<std::recursive_mutex> collectionLock(pending->collectionMutex);
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->committed && !pending->completionEvent.Complete()) {
                // Retain the launch while the device may still use its resources.
                continue;
            }
            if (!pending->quarantined && !pending->completionEvent.HasEvent()) {
                ClearTraceBatches(*pending);
                pending->retired = true;
            }
        }
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending.erase(
            std::remove_if(
                state.pending.begin(), state.pending.end(), [](const auto& pending) { return pending->retired; }),
            state.pending.end());
        state.loadedImages.clear();
        state.loadedImageLaunches.clear();
        state.streamLoadedImageLaunches.clear();
        state.queueHighWaterBytes.store(state.queuedBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        state.stopping = false;
        state.accepting = true;
        DeviceBinaries().Reset();
    } catch (...) {
        TraceIntegrityMark(0, 0, true);
    }
}

device_runtime::CallStackResult ResolveTraceDeviceCallStack(uint64_t pc) noexcept
{
    return DeviceBinaries().ResolveCallStack(pc, CurrentTraceDeviceId());
}

} // namespace aclsan
