/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "ub_launch_config.h"
#include "acl_san/aclsan_api.h"
#include "dbi/binary_instrumenter.h"
#include "device_runtime/device_symbolizer.h"
#include "aclsan_active_probe_plan.h"
#include "aclsan_dispatch.h"
#include "npu_tool_log.h"
#include "aclsan_runtime_hook.h"
#include "aclsan_trace_runtime.h"
#include "aclsan_trace_status.h"
#include "injection/injection_hook.h"
#include "file_io.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <tuple>
#include <mutex>
#include <unistd.h>
#include <dlfcn.h>
#include <limits>
#include <new>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace aclsan {
namespace {

bool IsHookRequired(const std::set<aclrtApiId>& requiredHooks, aclrtApiId apiId) noexcept
{
    return requiredHooks.find(apiId) != requiredHooks.end();
}

} // namespace

} // namespace aclsan

namespace {

using aclsan::AbortHookFailure;
using aclsan::GetOriginalRuntimeFunction;

uint64_t AlignDeviceMemorySize(uint64_t bytes) noexcept
{
    constexpr uint64_t deviceMemoryAlignment = 32U;
    constexpr uint64_t alignmentMask = deviceMemoryAlignment - 1U;
    if (bytes > std::numeric_limits<uint64_t>::max() - alignmentMask) {
        return bytes;
    }
    return (bytes + alignmentMask) & ~alignmentMask;
}

// 复制业务参数地址，再追加调用方保存的隐藏指针的地址；其存储需保持有效直到 launch 返回。
// 基于originalArgs更新增加参数隐藏trace指针的存储地址，最终aclrtLaunchKernelWithArgsArray使用的是args
// 将原有的args复制一份后，增加一个trace指针
aclError BuildInstrumentedArgsArray(
    const void* function, void*& deviceBuffer, size_t traceArgumentOffset, void* const* originalArgs,
    std::vector<void*>& args)
{
    if (function == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    const auto getCount = GetOriginalRuntimeFunction<aclrtFunctionGetParamCountFunc>(
        ACL_RT_API_aclrtFunctionGetParamCount, "aclrtFunctionGetParamCount");
    const auto getInfo = GetOriginalRuntimeFunction<aclrtFunctionGetParamInfoFunc>(
        ACL_RT_API_aclrtFunctionGetParamInfo, "aclrtFunctionGetParamInfo");
    size_t count = 0;
    ACLSAN_RETURN_IF_ACL_ERROR(getCount(function, &count), "Failed to get kernel parameter count");
    // 因为插桩的kernel已经增加隐藏trace参数，所以不应该为0
    if (count == 0) {
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }

    // 校验funcHandle对应paramInfo符号预期  参数个数和偏移都得符合预期
    const size_t originalCount = count - 1;
    size_t offset = 0;
    size_t size = 0;
    ACLSAN_RETURN_IF_ACL_ERROR(
        getInfo(function, originalCount, &offset, &size), "Failed to get hidden trace parameter info");
    // 只校验 sanitizer 追加的隐藏 GM 指针，业务参数地址由调用者直接提供。
    if (size != sizeof(void*) || offset != traceArgumentOffset) {
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }
    if (originalCount != 0 && originalArgs == nullptr) { // 仅有0参数场景才可能originalArgs为nullptr
        return ACL_ERROR_INVALID_PARAM;
    }
    for (size_t index = 0; index < originalCount; ++index) {
        args.push_back(originalArgs[index]); // hostArgs在host内存中的存储地址
    }
    args.push_back(&deviceBuffer); // 插入隐藏指针的存储地址
    return ACL_SUCCESS;
}

thread_local bool g_binaryLoadInProgress = false;
thread_local bool g_mallocInProgress = false;

class MallocGuard {
public:
    MallocGuard() { g_mallocInProgress = true; }
    ~MallocGuard() { g_mallocInProgress = false; }
};

struct PatchedFile {
    std::string path;
    aclrtBinHandle binary = nullptr;
    int32_t deviceId = -1;
    ~PatchedFile()
    {
        if (!path.empty()) {
            (void)unlink(path.c_str());
        }
    }
};

std::mutex g_patchedFileMutex;
std::list<PatchedFile> g_patchedFiles;

bool AdoptPatchedFile(aclrtBinHandle binary, int32_t deviceId)
{
    if (deviceId < 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_patchedFileMutex);
    auto unknown = g_patchedFiles.end();
    size_t matches = 0;
    for (auto it = g_patchedFiles.begin(); it != g_patchedFiles.end(); ++it) {
        if (it->binary != binary) {
            continue;
        }
        ++matches;
        if (it->deviceId < 0) {
            unknown = it;
        }
    }
    if (unknown == g_patchedFiles.end()) {
        return true;
    }
    if (matches != 1U || aclsan::TraceHasKnownBinaryHandle(binary)) {
        aclsan::TraceIntegrityMark(0U, 0U, true);
        return false;
    }
    unknown->deviceId = deviceId;
    return true;
}

class BinaryLoadGuard {
public:
    BinaryLoadGuard() { g_binaryLoadInProgress = true; }
    ~BinaryLoadGuard() { g_binaryLoadInProgress = false; }

    BinaryLoadGuard(const BinaryLoadGuard&) = delete;
    BinaryLoadGuard& operator=(const BinaryLoadGuard&) = delete;
};

struct DeferredVariant {
    std::mutex mutex;
    aclrtBinHandle binary = nullptr;
    aclrtFuncHandle function = nullptr;
    PatchedFile file;
    aclError status = ACL_ERROR_FAILURE;
    bool attempted = false;
    bool registered = false;
    int32_t deviceId = -1;
    uint32_t traceOffset = 0;
};

struct DeferredBinary {
    std::shared_mutex lifecycle;
    bool closing = false;
    std::vector<uint8_t> image;
    std::vector<aclrtBinaryLoadOption> options;
    bool nullOptions = true;
    bool fromFile = false;
    // A separate object per load is the generation; handles may be reused.
    std::mutex mutex;
    // Key fields: byEntry, entry, name, argumentMode, plan, context, traceOffset.
    using Key = std::tuple<bool, uint64_t, std::string, int, uint32_t, uintptr_t, uint32_t>;
    static constexpr std::size_t CONTEXT_KEY_INDEX = 5;
    std::map<Key, std::shared_ptr<DeferredVariant>> variants;
};

struct DeferredFunction {
    std::shared_ptr<DeferredBinary> source;
    bool byEntry = false;
    uint64_t entry = 0;
    std::string name;
};

using DeferredHandleKey = std::pair<int32_t, uintptr_t>;
constexpr int32_t UNKNOWN_DEFERRED_DEVICE = -1;
std::mutex g_deferredMutex;
std::map<DeferredHandleKey, std::shared_ptr<DeferredBinary>> g_deferredBinaries;
std::map<DeferredHandleKey, DeferredFunction> g_deferredFunctions;
std::list<std::shared_ptr<DeferredBinary>> g_retiredBinaries;

int32_t DeferredDeviceId() noexcept { return aclsan::TraceCurrentDeviceId(); }

std::shared_ptr<DeferredBinary> DeferredSource(const std::shared_ptr<DeferredBinary>& source) { return source; }
std::shared_ptr<DeferredBinary> DeferredSource(const DeferredFunction& function) { return function.source; }

template <typename Map>
bool DeferredSourceConflicts(const Map& entries, const std::shared_ptr<DeferredBinary>& source)
{
    for (const auto& [key, value] : entries) {
        if (key.first != UNKNOWN_DEFERRED_DEVICE || DeferredSource(value) != source) {
            continue;
        }
        for (const auto& [otherKey, otherValue] : entries) {
            (void)otherValue;
            if (otherKey.second == key.second && otherKey != key) {
                return true;
            }
        }
    }
    return false;
}

template <typename Map>
void AdoptDeferredEntries(Map& entries, int32_t deviceId, const std::shared_ptr<DeferredBinary>& source)
{
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->first.first != UNKNOWN_DEFERRED_DEVICE || DeferredSource(it->second) != source) {
            ++it;
            continue;
        }
        auto current = it++;
        auto node = entries.extract(current);
        node.key().first = deviceId;
        entries.insert(std::move(node));
    }
}

template <typename Map>
auto FindDeferred(Map& entries, int32_t deviceId, uintptr_t handle, bool& ambiguous)
{
    ambiguous = false;
    if (deviceId != UNKNOWN_DEFERRED_DEVICE) {
        auto exact = entries.find({deviceId, handle});
        if (exact != entries.end()) {
            return exact;
        }
        const auto unknown = entries.find({UNKNOWN_DEFERRED_DEVICE, handle});
        if (unknown == entries.end()) {
            return entries.end();
        }
        const auto source = DeferredSource(unknown->second);
        if (DeferredSourceConflicts(g_deferredBinaries, source) ||
            DeferredSourceConflicts(g_deferredFunctions, source)) {
            ambiguous = true;
            aclsan::TraceIntegrityMark(0, 0, true);
            return entries.end();
        }
        AdoptDeferredEntries(g_deferredBinaries, deviceId, source);
        AdoptDeferredEntries(g_deferredFunctions, deviceId, source);
        return entries.find({deviceId, handle});
    }
    auto found = entries.end();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->first.second != handle) {
            continue;
        }
        if (found != entries.end()) {
            ambiguous = true;
            aclsan::TraceIntegrityMark(0, 0, true);
            return entries.end();
        }
        found = it;
    }
    return found;
}

bool IsDeferredLoad(const aclrtBinaryLoadOptions* options)
{
    if (options == nullptr || options->options == nullptr) {
        return false;
    }
    for (size_t i = 0; i < options->numOpt; ++i) {
        if (options->options[i].type == ACL_RT_BINARY_LOAD_OPT_LAZY_LOAD && options->options[i].value.isLazyLoad != 0) {
            return true;
        }
        if (options->options[i].type == ACL_RT_BINARY_LOAD_OPT_MAGIC &&
            (options->options[i].value.magic == ACL_RT_BINARY_MAGIC_ELF_AICORE ||
             options->options[i].value.magic == ACL_RT_BINARY_MAGIC_ELF_CUBE_CORE ||
             options->options[i].value.magic == ACL_RT_BINARY_MAGIC_ELF_VECTOR_CORE)) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<DeferredBinary> CopyDeferredBinary(
    const void* data, size_t length, const aclrtBinaryLoadOptions* options, bool fromFile)
{
    auto source = std::make_shared<DeferredBinary>();
    const auto* bytes = static_cast<const uint8_t*>(data);
    source->image.assign(bytes, bytes + length);
    source->nullOptions = options == nullptr;
    source->fromFile = fromFile;
    if (options != nullptr && options->numOpt != 0) {
        source->options.assign(options->options, options->options + options->numOpt);
    }
    return source;
}

aclError RecordDeferredFunction(
    aclrtBinHandle binary, const void* function, bool byEntry, uint64_t entry, const char* name)
{
    const int32_t deviceId = DeferredDeviceId();
    std::lock_guard<std::mutex> lock(g_deferredMutex);
    bool ambiguous = false;
    const auto it = FindDeferred(g_deferredBinaries, deviceId, reinterpret_cast<uintptr_t>(binary), ambiguous);
    if (it != g_deferredBinaries.end()) {
        g_deferredFunctions[{it->first.first, reinterpret_cast<uintptr_t>(function)}] = {
            it->second, byEntry, entry, name == nullptr ? "" : name};
    } else if (ambiguous || deviceId == UNKNOWN_DEFERRED_DEVICE) {
        if (!ambiguous) {
            aclsan::TraceIntegrityMark(0, 0, true);
        }
        return ACL_ERROR_FAILURE;
    }
    return ACL_SUCCESS;
}

aclError RecordSymbolFunction(aclrtFuncHandle function)
{
    if (function == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    const auto getBinary =
        reinterpret_cast<aclError (*)(aclrtFuncHandle, aclrtBinHandle*)>(dlsym(RTLD_DEFAULT, "aclrtFunctionGetBinary"));
    const auto getName =
        reinterpret_cast<aclError (*)(aclrtFuncHandle, uint32_t, char*)>(dlsym(RTLD_DEFAULT, "aclrtGetFunctionName"));
    if (getBinary == nullptr || getName == nullptr) {
        return ACL_ERROR_FEATURE_UNSUPPORTED;
    }
    aclrtBinHandle binary = nullptr;
    char name[4096]{};
    aclError status = getBinary(function, &binary);
    if (status != ACL_SUCCESS) {
        return status;
    }
    status = getName(function, sizeof(name), name);
    if (status != ACL_SUCCESS) {
        return status;
    }
    if (binary == nullptr || name[0] == '\0' || name[sizeof(name) - 1] != '\0') {
        return ACL_ERROR_INVALID_PARAM;
    }
    status = RecordDeferredFunction(binary, function, false, 0, name);
    if (status != ACL_SUCCESS) {
        return status;
    }
    aclsan::RecordTraceBinaryFunctionLookup(binary, function, name);
    return ACL_SUCCESS;
}

struct DeferredLaunch {
    std::shared_ptr<DeferredBinary> source;
    std::shared_lock<std::shared_mutex> lock;
    std::shared_ptr<DeferredVariant> variant;

    // Failure and completion callbacks may re-enter binary unload.
    void Release() noexcept
    {
        if (lock.owns_lock()) {
            lock.unlock();
        }
    }
};

aclError PrepareDeferredLaunch(
    aclrtFuncHandle& function, int argumentMode, DeferredLaunch& launch, uint32_t traceOffset = 0) noexcept;

// 获取当前运行的deviceId用于记录
bool GetCurrentDeviceId(uint32_t& deviceId) noexcept
{
    const auto function = GetOriginalRuntimeFunction<aclrtGetDeviceFunc>(ACL_RT_API_aclrtGetDevice, "aclrtGetDevice");
    int32_t currentDeviceId = -1;
    const aclError result = function(&currentDeviceId);
    if (result != ACL_SUCCESS || currentDeviceId < 0) {
        ASCTOOL_ERROR("acl_san: aclrtGetDevice failed: result=%d deviceId=%d", result, currentDeviceId);
        return false;
    }
    deviceId = static_cast<uint32_t>(currentDeviceId);
    return true;
}

// ==============================================
// =============     创建cbdata     =============
// ==============================================
AclsanCallbackCommonData MakeCallbackCommonData(const char* apiName, int result, uint32_t size) noexcept
{
    return {ACLSAN_API_VERSION, size, apiName, result, 0};
}

AclsanResourceData MakeDeviceResourceData(
    const char* apiName, int result, void* deviceAddress, uint64_t bytes, uint32_t deviceId) noexcept
{
    return {
        MakeCallbackCommonData(apiName, result, static_cast<uint32_t>(sizeof(AclsanResourceData))),
        deviceAddress,
        bytes,
        ACLSAN_MEMORY_SPACE_DEVICE,
        deviceId,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(deviceAddress))};
}

AclsanSynchronizeData MakeSynchronizeData(const char* apiName, aclrtStream stream, int result) noexcept
{
    return {MakeCallbackCommonData(apiName, result, static_cast<uint32_t>(sizeof(AclsanSynchronizeData))), stream};
}

AclsanLaunchData MakeLaunchData(
    uint64_t launchId, aclrtFuncHandle function, aclrtStream stream, const std::string& functionName,
    uint32_t numBlocks, aclError launchResult, const char* apiName, const aclrtLaunchKernelCfg* config) noexcept
{
    const char* name = functionName.empty() ? nullptr : functionName.data();
    AclsanLaunchData result{};
    result.common = MakeCallbackCommonData(apiName, launchResult, static_cast<uint32_t>(sizeof(AclsanLaunchData)));
    result.launchId = launchId;
    result.function = function;
    result.stream = stream;
    result.functionName = name;
    result.numBlocks = numBlocks;
    const bool hasMetadata =
        aclsan::GetTraceFunctionUbMetadata(function, result.kernelVfType, result.compilerAllocUbBytes);
    const bool hasDynamicBytes = aclsan::ReadDynamicUbBytes(config, result.dynamicUbBytes);
    result.ubMetadataValid = hasMetadata && hasDynamicBytes;
    ASCTOOL_DEBUG(
        "acl_san UB launch: launch=%llu metadataValid=%u kernelVfType=%u compilerAllocUb=%llu dynamicUb=%llu "
        "metadataAvailable=%u dynamicUbValid=%u",
        static_cast<unsigned long long>(launchId), result.ubMetadataValid, result.kernelVfType,
        static_cast<unsigned long long>(result.compilerAllocUbBytes),
        static_cast<unsigned long long>(result.dynamicUbBytes), static_cast<unsigned>(hasMetadata),
        static_cast<unsigned>(hasDynamicBytes));
    return result;
}

// ==============================================
// =========    aclrt接口的hook逻辑    ===========
// ==============================================

// 基本逻辑: 如果获取原始aclrt函数指针失败，那么直接abort。否则把cbdata传回去

struct InstrumentedBinaryLoadContext {
    aclrtBinaryLoadFromDataFunc original = nullptr;
    const aclrtBinaryLoadOptions* options = nullptr;
    aclrtBinHandle* binHandle = nullptr;
};

int32_t LoadInstrumentedBinary(const void* data, size_t length, void* userdata)
{
    auto& context = *static_cast<InstrumentedBinaryLoadContext*>(userdata);
    const auto status = context.original(data, length, context.options, context.binHandle);
    if (status == ACL_SUCCESS && context.binHandle != nullptr && *context.binHandle != nullptr) {
        aclsan::RecordTraceLoadedImage(*context.binHandle, data, length);
    }
    return status;
}

// DONE
aclError aclrtMallocHook(void** deviceAddress, std::size_t size, aclrtMemMallocPolicy policy) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtMallocFunc>(ACL_RT_API_aclrtMalloc, "aclrtMalloc");
    if (g_mallocInProgress) {
        ASCTOOL_DEBUG("acl_san resource alloc skipped: api=aclrtMalloc reason=reentrant allocation");
        return original(deviceAddress, size, policy);
    }
    uint32_t deviceId;
    const bool hasDeviceId = GetCurrentDeviceId(deviceId);
    aclError result;
    {
        MallocGuard guard;
        result = original(deviceAddress, size, policy);
    }
    if (!hasDeviceId) {
        ASCTOOL_DEBUG("acl_san resource alloc skipped: api=aclrtMalloc reason=no current device");
        return result;
    }
    void* allocatedAddress = nullptr;
    if (result == ACL_SUCCESS && deviceAddress != nullptr) {
        allocatedAddress = *deviceAddress;
    }
    const AclsanResourceData callbackData = MakeDeviceResourceData(
        "aclrtMalloc", result, allocatedAddress, AlignDeviceMemorySize(static_cast<uint64_t>(size)), deviceId);
    aclsan::AclsanCallbackDispatcher::DispatchResource(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, callbackData);
    return result;
}

aclError aclrtMallocAlign32Hook(void** deviceAddress, std::size_t size, aclrtMemMallocPolicy policy) noexcept
{
    const auto original =
        GetOriginalRuntimeFunction<aclrtMallocAlign32Func>(ACL_RT_API_aclrtMallocAlign32, "aclrtMallocAlign32");
    if (g_mallocInProgress) {
        ASCTOOL_DEBUG("acl_san resource alloc skipped: api=aclrtMallocAlign32 reason=reentrant allocation");
        return original(deviceAddress, size, policy);
    }
    uint32_t deviceId;
    const bool hasDeviceId = GetCurrentDeviceId(deviceId);
    aclError result;
    {
        MallocGuard guard;
        result = original(deviceAddress, size, policy);
    }
    if (!hasDeviceId) {
        ASCTOOL_DEBUG("acl_san resource alloc skipped: api=aclrtMallocAlign32 reason=no current device");
        return result;
    }
    void* allocatedAddress = nullptr;
    if (result == ACL_SUCCESS && deviceAddress != nullptr) {
        allocatedAddress = *deviceAddress;
    }
    const AclsanResourceData callbackData = MakeDeviceResourceData(
        "aclrtMallocAlign32", result, allocatedAddress, AlignDeviceMemorySize(static_cast<uint64_t>(size)), deviceId);
    aclsan::AclsanCallbackDispatcher::DispatchResource(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, callbackData);
    return result;
}

aclError aclrtMallocWithCfgHook(void** ptr, size_t bytes, aclrtMemMallocPolicy policy, aclrtMallocConfig* cfg) noexcept
{
    const auto original =
        GetOriginalRuntimeFunction<aclrtMallocWithCfgFunc>(ACL_RT_API_aclrtMallocWithCfg, "aclrtMallocWithCfg");
    if (g_mallocInProgress) {
        ASCTOOL_DEBUG("acl_san resource alloc skipped: api=aclrtMallocWithCfg reason=reentrant allocation");
        return original(ptr, bytes, policy, cfg);
    }
    uint32_t device = 0;
    const bool hasDevice = GetCurrentDeviceId(device);
    aclError result;
    {
        MallocGuard guard;
        result = original(ptr, bytes, policy, cfg);
    }
    if (hasDevice) {
        const auto callback = MakeDeviceResourceData(
            "aclrtMallocWithCfg", result, result == ACL_SUCCESS && ptr != nullptr ? *ptr : nullptr, bytes, device);
        aclsan::AclsanCallbackDispatcher::DispatchResource(ACLSAN_CBID_RESOURCE_MEMORY_ALLOC, callback);
    } else {
        ASCTOOL_DEBUG("acl_san resource alloc skipped: api=aclrtMallocWithCfg reason=no current device");
    }
    return result;
}

void ReportHostWrite(
    const char* apiName, aclError result, const void* destination, uint64_t bytes, bool hasDevice,
    uint32_t deviceId) noexcept
{
    // Missing device identity must reach the checker as an incomplete operation,
    // rather than making a completed write look like uninitialized memory.
    const AclsanMemoryWriteData data{
        MakeCallbackCommonData(apiName, hasDevice ? result : ACL_ERROR_FAILURE, sizeof(AclsanMemoryWriteData)),
        destination, bytes, deviceId, 0};
    aclsan::AclsanCallbackDispatcher::DispatchMemoryWrite(data);
}

aclError aclrtMemsetHook(void* destination, size_t capacity, int32_t value, size_t count) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtMemsetFunc>(ACL_RT_API_aclrtMemset, "aclrtMemset");
    uint32_t deviceId = 0;
    const bool hasDevice = GetCurrentDeviceId(deviceId);
    const aclError result = original(destination, capacity, value, count);
    ReportHostWrite("aclrtMemset", result, destination, count, hasDevice, deviceId);
    return result;
}

aclError aclrtMemcpyHook(
    void* destination, size_t capacity, const void* source, size_t count, aclrtMemcpyKind kind) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtMemcpyFunc>(ACL_RT_API_aclrtMemcpy, "aclrtMemcpy");
    if (kind != ACL_MEMCPY_HOST_TO_DEVICE && kind != ACL_MEMCPY_DEVICE_TO_HOST && kind != ACL_MEMCPY_DEVICE_TO_DEVICE)
        return original(destination, capacity, source, count, kind);
    uint32_t deviceId = 0;
    const bool hasDevice = GetCurrentDeviceId(deviceId);
    const aclError result = original(destination, capacity, source, count, kind);
    const auto transferKind = kind == ACL_MEMCPY_HOST_TO_DEVICE ? ACLSAN_MEMORY_TRANSFER_HOST_TO_DEVICE :
                              kind == ACL_MEMCPY_DEVICE_TO_HOST ? ACLSAN_MEMORY_TRANSFER_DEVICE_TO_HOST :
                                                                  ACLSAN_MEMORY_TRANSFER_DEVICE_TO_DEVICE;
    const AclsanMemoryTransferData transfer{
        MakeCallbackCommonData("aclrtMemcpy", hasDevice ? result : ACL_ERROR_FAILURE, sizeof(AclsanMemoryTransferData)),
        source,
        destination,
        count,
        deviceId,
        transferKind};
    aclsan::AclsanCallbackDispatcher::DispatchMemoryTransfer(transfer);
    // Old consumers still receive their H2D-only notification. The unified
    // consumer uses TRANSFER_END for memcpy and WRITE_END for memset.
    if (kind == ACL_MEMCPY_HOST_TO_DEVICE)
        ReportHostWrite("aclrtMemcpy", result, destination, count, hasDevice, deviceId);
    return result;
}

// DONE
aclError aclrtFreeHook(void* deviceAddress) noexcept
{
    uint32_t deviceId;
    const bool hasDeviceId = GetCurrentDeviceId(deviceId);
    const auto original = GetOriginalRuntimeFunction<aclrtFreeFunc>(ACL_RT_API_aclrtFree, "aclrtFree");
    const aclError result = original(deviceAddress);
    if (!hasDeviceId) {
        return result;
    }
    const AclsanResourceData callbackData = MakeDeviceResourceData("aclrtFree", result, deviceAddress, 0, deviceId);
    aclsan::AclsanCallbackDispatcher::DispatchResource(ACLSAN_CBID_RESOURCE_MEMORY_FREE, callbackData);
    return result;
}

static aclError LoadDeferredBinary(
    aclrtBinaryLoadFromDataFunc original, const void* data, size_t length, const aclrtBinaryLoadOptions* options,
    aclrtBinHandle* binHandle) noexcept
{
    if (data == nullptr || length == 0 || binHandle == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    try {
        auto source = CopyDeferredBinary(data, length, options, false);
        // Allocate the registry node before Runtime creates a handle.
        std::map<DeferredHandleKey, std::shared_ptr<DeferredBinary>> staging;
        staging.emplace(DeferredHandleKey{DeferredDeviceId(), 0U}, source);
        const auto result = original(data, length, options, binHandle);
        if (result != ACL_SUCCESS) {
            return result;
        }
        if (*binHandle == nullptr) {
            return ACL_ERROR_FAILURE;
        }
        auto node = staging.extract(staging.begin());
        node.key().second = reinterpret_cast<uintptr_t>(*binHandle);
        std::unique_lock<std::mutex> lock(g_deferredMutex);
        if (!g_deferredBinaries.insert(std::move(node)).inserted) {
            lock.unlock();
            const auto unload =
                GetOriginalRuntimeFunction<aclrtBinaryUnLoadFunc>(ACL_RT_API_aclrtBinaryUnLoad, "aclrtBinaryUnLoad");
            if (unload(*binHandle) != ACL_SUCCESS) {
                AbortHookFailure("aclrtBinaryLoadFromData", "rollback_load", "cannot release duplicate handle");
            }
            *binHandle = nullptr;
            return ACL_ERROR_FAILURE;
        }
        aclsan::RecordTraceBinaryLoadFromData(*binHandle, false, 0, data, length);
        return result;
    } catch (const std::bad_alloc&) {
        return ACL_ERROR_BAD_ALLOC;
    } catch (...) {
        return ACL_ERROR_FAILURE;
    }
}

aclError aclrtBinaryLoadFromDataHook(
    const void* data, size_t length, const aclrtBinaryLoadOptions* options, aclrtBinHandle* binHandle) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtBinaryLoadFromDataFunc>(
        ACL_RT_API_aclrtBinaryLoadFromData, "aclrtBinaryLoadFromData");

    if (g_binaryLoadInProgress) {
        return original(data, length, options, binHandle);
    }

    uint32_t probePlan = 0;
    {
        const std::shared_lock<std::shared_mutex> planLock(aclsan::ActiveProbePlanMutex());
        probePlan = aclsan::SnapshotActiveProbePlan();
    }
    if (probePlan == 0) {
        const aclError result = original(data, length, options, binHandle);
        if (result == ACL_SUCCESS && binHandle != nullptr) {
            aclsan::RecordTraceBinaryLoadFromData(*binHandle, false, 0, data, length);
        }
        return result;
    }
    const BinaryLoadGuard guard;
    if (IsDeferredLoad(options)) {
        return LoadDeferredBinary(original, data, length, options, binHandle);
    }
    bool loadedPatched = false;
    InstrumentedBinaryLoadContext loadContext{original, options, binHandle};
    const auto getSocName =
        GetOriginalRuntimeFunction<aclrtGetSocNameFunc>(ACL_RT_API_aclrtGetSocName, "aclrtGetSocName");
    Dl_info runtimeInfo{};
    const char* runtimeLibrary =
        dladdr(reinterpret_cast<const void*>(getSocName), &runtimeInfo) != 0 ? runtimeInfo.dli_fname : nullptr;
    const aclsan::RuntimeBinaryInstrumentationResult instrumentation = aclsan::InstrumentRuntimeBinary(
        data, length, probePlan, getSocName(), runtimeLibrary, &LoadInstrumentedBinary, &loadContext);
    aclError result = ACL_ERROR_FAILURE;
    if (instrumentation.status == aclsan::BinaryInstrumentationStatus::Instrumented) {
        result = instrumentation.consumerStatus;
        loadedPatched = result == ACL_SUCCESS;
    } else if (instrumentation.status == aclsan::BinaryInstrumentationStatus::Failed) {
        result = instrumentation.strict != 0 ? ACL_ERROR_FAILURE : original(data, length, options, binHandle);
    } else {
        result = original(data, length, options, binHandle);
    }
    if (result == ACL_SUCCESS && binHandle != nullptr) {
        aclsan::RecordTraceBinaryLoadFromData(
            *binHandle, loadedPatched, instrumentation.traceArgumentOffset, data, length);
    }
    return result;
}

struct FileLoadContext {
    aclrtBinaryLoadFromFileFunc original;
    aclrtBinaryLoadOptions* options;
    aclrtBinHandle* binary;
    PatchedFile& file;
};

int32_t LoadInstrumentedFile(const void* data, size_t bytes, void* opaque)
{
    auto& context = *static_cast<FileLoadContext*>(opaque);
    char path[] = "/tmp/npu-check-kernel-XXXXXX";
    context.file.path.reserve(sizeof(path));
    const int fd = mkstemp(path);
    if (fd < 0) {
        return ACL_ERROR_FAILURE;
    }
    context.file.path = path;
    size_t written = 0;
    while (written < bytes) {
        const auto count = write(fd, static_cast<const char*>(data) + written, bytes - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            (void)close(fd);
            return ACL_ERROR_FAILURE;
        }
        written += static_cast<size_t>(count);
    }
    if (close(fd) != 0) {
        return ACL_ERROR_FAILURE;
    }
    const auto status = context.original(context.file.path.c_str(), context.options, context.binary);
    if (status == ACL_SUCCESS && context.binary != nullptr && *context.binary != nullptr) {
        aclsan::RecordTraceLoadedImage(*context.binary, data, bytes);
    }
    return status;
}

aclError aclrtBinaryLoadFromFileHook(const char* path, aclrtBinaryLoadOptions* options, aclrtBinHandle* binary) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtBinaryLoadFromFileFunc>(
        ACL_RT_API_aclrtBinaryLoadFromFile, "aclrtBinaryLoadFromFile");
    if (g_binaryLoadInProgress) {
        return original(path, options, binary);
    }
    uint32_t plan = 0;
    {
        const std::shared_lock<std::shared_mutex> lock(aclsan::ActiveProbePlanMutex());
        plan = aclsan::SnapshotActiveProbePlan();
    }
    if (plan == 0) {
        return original(path, options, binary);
    }
    if (path == nullptr || binary == nullptr) {
        return ACL_ERROR_INVALID_PARAM;
    }
    try {
        std::string normalizedPath;
        std::string image;
        if (!asctools::security::NormalizePath(path, asctools::security::PathKind::ExistingFile, normalizedPath) ||
            !asctools::security::ReadTextFile(normalizedPath, image)) {
            ASCTOOL_ERROR("acl_san: cannot read kernel file %s", path);
            return ACL_ERROR_FAILURE;
        }
        if (image.empty()) {
            return ACL_ERROR_INVALID_PARAM;
        }
        if (IsDeferredLoad(options)) {
            auto source = CopyDeferredBinary(image.data(), image.size(), options, true);
            std::map<DeferredHandleKey, std::shared_ptr<DeferredBinary>> staging;
            staging.emplace(DeferredHandleKey{DeferredDeviceId(), 0U}, source);
            const BinaryLoadGuard guard;
            const auto result = original(normalizedPath.c_str(), options, binary);
            if (result == ACL_SUCCESS) {
                if (*binary == nullptr) {
                    return ACL_ERROR_FAILURE;
                }
                auto node = staging.extract(staging.begin());
                node.key().second = reinterpret_cast<uintptr_t>(*binary);
                std::unique_lock<std::mutex> lock(g_deferredMutex);
                if (!g_deferredBinaries.insert(std::move(node)).inserted) {
                    lock.unlock();
                    const auto unload = GetOriginalRuntimeFunction<aclrtBinaryUnLoadFunc>(
                        ACL_RT_API_aclrtBinaryUnLoad, "aclrtBinaryUnLoad");
                    if (unload(*binary) != ACL_SUCCESS) {
                        AbortHookFailure("aclrtBinaryLoadFromFile", "rollback_load", "cannot release duplicate handle");
                    }
                    *binary = nullptr;
                    return ACL_ERROR_FAILURE;
                }
                aclsan::RecordTraceBinaryLoadFromData(*binary, false, 0, image.data(), image.size());
            }
            return result;
        }
        std::list<PatchedFile> files(1);
        FileLoadContext context{original, options, binary, files.front()};
        const auto soc = GetOriginalRuntimeFunction<aclrtGetSocNameFunc>(ACL_RT_API_aclrtGetSocName, "aclrtGetSocName");
        Dl_info library{};
        const char* runtime = dladdr(reinterpret_cast<const void*>(soc), &library) != 0 ? library.dli_fname : nullptr;
        const BinaryLoadGuard guard;
        const auto result = aclsan::InstrumentRuntimeBinary(
            image.data(), image.size(), plan, soc(), runtime, &LoadInstrumentedFile, &context);
        if (result.status != aclsan::BinaryInstrumentationStatus::Instrumented) {
            return ACL_ERROR_FAILURE;
        }
        if (result.consumerStatus != ACL_SUCCESS) {
            return result.consumerStatus;
        }
        files.front().binary = *binary;
        files.front().deviceId = DeferredDeviceId();
        {
            // Retain the transformed file for lazy loading until successful unload.
            std::lock_guard<std::mutex> lock(g_patchedFileMutex);
            g_patchedFiles.splice(g_patchedFiles.end(), files);
        }
        aclsan::RecordTraceBinaryLoadFromData(*binary, true, result.traceArgumentOffset, image.data(), image.size());
        return ACL_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ACL_ERROR_BAD_ALLOC;
    } catch (...) {
        return ACL_ERROR_FAILURE;
    }
}

aclError aclrtBinaryGetFunctionHook(
    const aclrtBinHandle binHandle, const char* kernelName, aclrtFuncHandle* funcHandle) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtBinaryGetFunctionFunc>(
        ACL_RT_API_aclrtBinaryGetFunction, "aclrtBinaryGetFunction");
    const aclError result = original(binHandle, kernelName, funcHandle);
    if (result == ACL_SUCCESS && (funcHandle == nullptr || *funcHandle == nullptr)) {
        return ACL_ERROR_FAILURE;
    }
    if (result == ACL_SUCCESS && funcHandle != nullptr) {
        if (!AdoptPatchedFile(binHandle, DeferredDeviceId())) {
            return ACL_ERROR_FAILURE;
        }
        try {
            if (RecordDeferredFunction(binHandle, *funcHandle, false, 0, kernelName) != ACL_SUCCESS) {
                return ACL_ERROR_FAILURE;
            }
        } catch (...) {
            return ACL_ERROR_BAD_ALLOC;
        }
        aclsan::RecordTraceBinaryFunctionLookup(binHandle, *funcHandle, kernelName);
    }
    return result;
}

void ResolveDeferredVariantFunction(const DeferredFunction& selected, DeferredVariant& variant)
{
    if (selected.byEntry) {
        const auto lookup = GetOriginalRuntimeFunction<aclrtBinaryGetFunctionByEntryFunc>(
            ACL_RT_API_aclrtBinaryGetFunctionByEntry, "aclrtBinaryGetFunctionByEntry");
        variant.status = lookup(variant.binary, selected.entry, &variant.function);
    } else {
        const auto lookup = GetOriginalRuntimeFunction<aclrtBinaryGetFunctionFunc>(
            ACL_RT_API_aclrtBinaryGetFunction, "aclrtBinaryGetFunction");
        variant.status = lookup(variant.binary, selected.name.c_str(), &variant.function);
    }
    if (variant.status == ACL_SUCCESS && variant.function == nullptr) {
        variant.status = ACL_ERROR_FAILURE;
    }
    if (variant.status != ACL_SUCCESS && variant.binary != nullptr) {
        const auto unload =
            GetOriginalRuntimeFunction<aclrtBinaryUnLoadFunc>(ACL_RT_API_aclrtBinaryUnLoad, "aclrtBinaryUnLoad");
        if (unload(variant.binary) == ACL_SUCCESS) {
            aclsan::RecordTraceBinaryUnload(variant.binary);
            variant.binary = nullptr;
            if (!variant.file.path.empty()) {
                (void)unlink(variant.file.path.c_str());
                variant.file.path.clear();
            }
        }
        variant.function = nullptr;
    }
}

aclError BuildDeferredVariant(
    const DeferredFunction& selected, int argumentMode, uint32_t plan, uint32_t traceOffset, DeferredVariant& variant)
{
    variant.attempted = true;
    const auto getDevice = GetOriginalRuntimeFunction<aclrtGetDeviceFunc>(ACL_RT_API_aclrtGetDevice, "aclrtGetDevice");
    if (getDevice(&variant.deviceId) != ACL_SUCCESS || variant.deviceId < 0) {
        return ACL_ERROR_FAILURE;
    }
    auto optionsCopy = selected.source->options;
    aclrtBinaryLoadOptions options{optionsCopy.data(), optionsCopy.size()};
    auto* optionPtr = selected.source->nullOptions ? nullptr : &options;
    const auto dataLoad = GetOriginalRuntimeFunction<aclrtBinaryLoadFromDataFunc>(
        ACL_RT_API_aclrtBinaryLoadFromData, "aclrtBinaryLoadFromData");
    const auto fileLoad = selected.source->fromFile ?
                              GetOriginalRuntimeFunction<aclrtBinaryLoadFromFileFunc>(
                                  ACL_RT_API_aclrtBinaryLoadFromFile, "aclrtBinaryLoadFromFile") :
                              nullptr;
    InstrumentedBinaryLoadContext dataContext{dataLoad, optionPtr, &variant.binary};
    FileLoadContext fileContext{fileLoad, optionPtr, &variant.binary, variant.file};
    const auto soc = GetOriginalRuntimeFunction<aclrtGetSocNameFunc>(ACL_RT_API_aclrtGetSocName, "aclrtGetSocName");
    Dl_info library{};
    const char* runtime = dladdr(reinterpret_cast<const void*>(soc), &library) != 0 ? library.dli_fname : nullptr;
    const BinaryLoadGuard guard;
    const auto result = aclsan::InstrumentRuntimeBinaryForEntry(
        selected.source->image.data(), selected.source->image.size(), plan, soc(), runtime,
        selected.source->fromFile ? &LoadInstrumentedFile : &LoadInstrumentedBinary,
        selected.source->fromFile ? static_cast<void*>(&fileContext) : static_cast<void*>(&dataContext),
        selected.byEntry ? &selected.entry : nullptr, traceOffset, argumentMode != 0, argumentMode == 0);
    variant.status =
        result.status == aclsan::BinaryInstrumentationStatus::Instrumented ? result.consumerStatus : ACL_ERROR_FAILURE;
    if (variant.status == ACL_SUCCESS) {
        ResolveDeferredVariantFunction(selected, variant);
        variant.traceOffset = result.traceArgumentOffset;
    }
    ASCTOOL_INFO(
        "deferred DBI entry=%llu by_entry=%u abi=%d result=%d", static_cast<unsigned long long>(selected.entry),
        selected.byEntry, argumentMode, variant.status);
    return ACL_SUCCESS;
}

aclError PrepareDeferredLaunch(
    aclrtFuncHandle& function, int argumentMode, DeferredLaunch& launch, uint32_t traceOffset) noexcept
{
    try {
        DeferredFunction selected;
        {
            std::lock_guard<std::mutex> lock(g_deferredMutex);
            bool ambiguous = false;
            const int32_t deviceId = DeferredDeviceId();
            const auto it =
                FindDeferred(g_deferredFunctions, deviceId, reinterpret_cast<uintptr_t>(function), ambiguous);
            if (it == g_deferredFunctions.end()) {
                if (ambiguous || deviceId == UNKNOWN_DEFERRED_DEVICE) {
                    if (!ambiguous) {
                        aclsan::TraceIntegrityMark(0, 0, true);
                    }
                    return ACL_ERROR_FAILURE;
                }
                return ACL_SUCCESS;
            }
            selected = it->second;
        }
        launch.source = selected.source;
        // Keep the lock local until preparation succeeds; failure callbacks may unload this binary.
        std::shared_lock<std::shared_mutex> lifecycleLock(launch.source->lifecycle);
        if (launch.source->closing) {
            return ACL_ERROR_INVALID_PARAM;
        }
        uint32_t plan;
        {
            std::shared_lock<std::shared_mutex> lock(aclsan::ActiveProbePlanMutex());
            plan = aclsan::SnapshotActiveProbePlan();
        }
        if (plan == 0) {
            return ACL_SUCCESS;
        }
        // Resolve only on this path; do not add a mandatory API to injection initialization.
        const auto getContext =
            reinterpret_cast<aclError (*)(aclrtContext*)>(dlsym(RTLD_DEFAULT, "aclrtGetCurrentContext"));
        aclrtContext context = nullptr;
        if (getContext == nullptr || getContext(&context) != ACL_SUCCESS || context == nullptr) {
            return ACL_ERROR_FEATURE_UNSUPPORTED;
        }
        const DeferredBinary::Key key{selected.byEntry, selected.entry, selected.name,
                                      argumentMode,     plan,           reinterpret_cast<uintptr_t>(context),
                                      traceOffset};
        {
            std::lock_guard<std::mutex> lock(launch.source->mutex);
            auto& variant = launch.source->variants[key];
            if (!variant) {
                variant = std::make_shared<DeferredVariant>();
            }
            launch.variant = variant;
        }
        // Only callers of the same variant wait for its build; no registry lock crosses Runtime/DBI.
        std::lock_guard<std::mutex> buildLock(launch.variant->mutex);
        auto& variant = *launch.variant;
        if (!variant.attempted) {
            const auto buildStatus = BuildDeferredVariant(selected, argumentMode, plan, traceOffset, variant);
            if (buildStatus != ACL_SUCCESS) {
                return buildStatus;
            }
        }
        if (variant.status == ACL_SUCCESS) {
            if (!variant.registered) {
                aclsan::RecordTraceBinaryLoadFromData(
                    variant.binary, true, variant.traceOffset, launch.source->image.data(),
                    launch.source->image.size());
                aclsan::RecordTraceBinaryFunctionLookup(
                    variant.binary, variant.function, selected.byEntry ? nullptr : selected.name.c_str(),
                    selected.byEntry ? std::optional<uint64_t>{selected.entry} : std::nullopt);
                variant.registered = true;
            }
            function = variant.function;
            launch.lock = std::move(lifecycleLock);
        }
        return variant.status;
    } catch (const std::bad_alloc&) {
        return ACL_ERROR_BAD_ALLOC;
    } catch (...) {
        return ACL_ERROR_FAILURE;
    }
}

void CollectRetiredVariants() noexcept
{
    try {
        const auto getContext =
            reinterpret_cast<aclError (*)(aclrtContext*)>(dlsym(RTLD_DEFAULT, "aclrtGetCurrentContext"));
        aclrtContext context = nullptr;
        if (getContext == nullptr || getContext(&context) != ACL_SUCCESS) {
            return;
        }
        std::list<std::shared_ptr<DeferredBinary>> retired;
        {
            std::lock_guard<std::mutex> lock(g_deferredMutex);
            retired.splice(retired.end(), g_retiredBinaries);
        }
        const auto unload =
            GetOriginalRuntimeFunction<aclrtBinaryUnLoadFunc>(ACL_RT_API_aclrtBinaryUnLoad, "aclrtBinaryUnLoad");
        for (auto source = retired.begin(); source != retired.end();) {
            auto& variants = (*source)->variants;
            for (auto it = variants.begin(); it != variants.end();) {
                auto& variant = *it->second;
                if (it->second.use_count() != 1 ||
                    std::get<DeferredBinary::CONTEXT_KEY_INDEX>(it->first) != reinterpret_cast<uintptr_t>(context)) {
                    ++it;
                    continue;
                }
                if (variant.binary != nullptr && unload(variant.binary) != ACL_SUCCESS) {
                    ++it;
                    continue;
                }
                aclsan::RecordTraceBinaryUnload(variant.binary);
                it = variants.erase(it);
            }
            source = variants.empty() ? retired.erase(source) : std::next(source);
        }
        std::lock_guard<std::mutex> lock(g_deferredMutex);
        g_retiredBinaries.splice(g_retiredBinaries.end(), retired);
    } catch (...) {
        ASCTOOL_ERROR("failed to collect deferred DBI variants");
    }
}

void ResetDeferredDevice(int32_t deviceId) noexcept
{
    try {
        std::vector<std::shared_ptr<DeferredBinary>> sources;
        {
            std::lock_guard<std::mutex> lock(g_deferredMutex);
            size_t unknown = 0;
            for (const auto& [key, source] : g_deferredBinaries) {
                if (key.first == UNKNOWN_DEFERRED_DEVICE) {
                    ++unknown;
                } else if (key.first == deviceId) {
                    sources.push_back(source);
                }
            }
            if (unknown != 0U) {
                ASCTOOL_WARNING("acl_san: reset retained %zu unknown-device deferred binaries", unknown);
            }
        }
        for (const auto& source : sources) {
            auto retired = std::make_shared<DeferredBinary>();
            std::list<std::shared_ptr<DeferredBinary>> staging{retired};
            std::unique_lock<std::shared_mutex> lock(source->lifecycle);
            for (auto it = source->variants.begin(); it != source->variants.end();) {
                if (it->second->deviceId == deviceId) {
                    it->second->registered = false;
                    auto current = it++;
                    retired->variants.insert(source->variants.extract(current));
                } else {
                    ++it;
                }
            }
            if (!retired->variants.empty()) {
                std::lock_guard<std::mutex> registryLock(g_deferredMutex);
                g_retiredBinaries.splice(g_retiredBinaries.end(), staging);
            }
        }
    } catch (...) {
        AbortHookFailure("aclrtResetDevice", "invalidate_deferred_cache", "cannot invalidate deferred DBI cache");
    }
}

aclError aclrtBinaryGetFunctionByEntryHook(
    aclrtBinHandle binHandle, uint64_t functionEntry, aclrtFuncHandle* funcHandle) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtBinaryGetFunctionByEntryFunc>(
        ACL_RT_API_aclrtBinaryGetFunctionByEntry, "aclrtBinaryGetFunctionByEntry");
    const aclError result = original(binHandle, functionEntry, funcHandle);
    if (result == ACL_SUCCESS && (funcHandle == nullptr || *funcHandle == nullptr)) {
        return ACL_ERROR_FAILURE;
    }
    if (result == ACL_SUCCESS && funcHandle != nullptr) {
        if (!AdoptPatchedFile(binHandle, DeferredDeviceId())) {
            return ACL_ERROR_FAILURE;
        }
        try {
            if (RecordDeferredFunction(binHandle, *funcHandle, true, functionEntry, nullptr) != ACL_SUCCESS) {
                return ACL_ERROR_FAILURE;
            }
        } catch (...) {
            return ACL_ERROR_BAD_ALLOC;
        }
        aclsan::RecordTraceBinaryFunctionLookup(binHandle, *funcHandle, nullptr, functionEntry);
    }
    return result;
}

aclError aclrtGetFuncBySymbolHook(const void* symbol, aclrtFuncHandle* funcHandle) noexcept
{
    const auto original =
        GetOriginalRuntimeFunction<aclrtGetFuncBySymbolFunc>(ACL_RT_API_aclrtGetFuncBySymbol, "aclrtGetFuncBySymbol");
    const aclError result = original(symbol, funcHandle);
    if (result == ACL_SUCCESS && funcHandle != nullptr) {
        try {
            return RecordSymbolFunction(*funcHandle);
        } catch (...) {
            return ACL_ERROR_BAD_ALLOC;
        }
    }
    return result;
}

template <typename Launch>
aclError LaunchKernelWithHostArgs(
    aclrtFuncHandle funcHandle, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* config, void* hostArgs,
    size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, size_t placeHolderNum, const char* apiName,
    Launch&& launch, uint32_t parameterPrefixBytes = 0) noexcept
{
    DeferredLaunch deferred;
    const auto fail = [&deferred, &funcHandle, &stream, &numBlocks, apiName](aclError status) {
        deferred.Release();
        AclsanLaunchData callback{};
        callback.common = MakeCallbackCommonData(apiName, status, sizeof(AclsanLaunchData));
        callback.function = funcHandle;
        callback.stream = stream;
        callback.numBlocks = numBlocks;
        aclsan::AclsanCallbackDispatcher::DispatchLaunch(callback);
        return status;
    };
    uint32_t traceOffset = 0;
    if (placeHolderNum != 0) {
        if (placeHolderArray == nullptr || hostArgs == nullptr) {
            return fail(ACL_ERROR_INVALID_PARAM);
        }
        for (size_t i = 0; i < placeHolderNum; ++i) {
            const auto& placeholder = placeHolderArray[i];
            if (placeholder.addrOffset > argsSize || argsSize - placeholder.addrOffset < sizeof(void*) ||
                placeholder.addrOffset > UINT32_MAX - sizeof(void*) || placeholder.dataOffset > argsSize) {
                return fail(ACL_ERROR_INVALID_PARAM);
            }
            traceOffset = std::max(traceOffset, placeholder.addrOffset + static_cast<uint32_t>(sizeof(void*)));
        }
    }
    const auto deferredStatus = PrepareDeferredLaunch(funcHandle, 0, deferred, traceOffset);
    if (deferredStatus != ACL_SUCCESS) {
        return fail(deferredStatus);
    }
    aclsan::PreparedTraceLaunch prepared;
    const auto traceStatus = aclsan::PrepareTraceLaunch(
        funcHandle, numBlocks, hostArgs, argsSize, placeHolderArray, placeHolderNum,
        aclsan::TraceArgumentMode::HOST_ARGS, prepared, config);
    if (traceStatus != ACL_SUCCESS) {
        return fail(traceStatus);
    }
    prepared.binaryLease = deferred.variant;
    prepared.parameterPrefixBytes = parameterPrefixBytes;

    const auto materializeStatus = aclsan::MaterializeTraceHostInputs(prepared);
    if (materializeStatus != ACL_SUCCESS) {
        aclsan::CompleteTraceLaunch(std::move(prepared), funcHandle, stream, materializeStatus);
        return fail(materializeStatus);
    }

    void* launchArguments = prepared.instrumented ? prepared.arguments.data() : hostArgs;
    const size_t launchArgumentBytes = prepared.instrumented ? prepared.arguments.size() : argsSize;
    aclrtPlaceHolderInfo* launchPlaceholders =
        prepared.instrumented ? (prepared.placeholders.empty() ? nullptr : prepared.placeholders.data()) :
                                placeHolderArray;
    const size_t launchPlaceholderCount = prepared.instrumented ? prepared.placeholders.size() : placeHolderNum;
    const uint64_t launchId = prepared.launchId;
    ASCTOOL_DEBUG(
        "%s: launch=%llu function=%p blocks=%u originalArgs=%zu traceOffset=%u "
        "originalPlaceholders=%zu materializedInputs=%zu launchArgs=%zu launchPlaceholders=%zu",
        apiName, static_cast<unsigned long long>(launchId), funcHandle, numBlocks, argsSize,
        prepared.traceArgumentOffset, placeHolderNum, prepared.hostInputs.size(), launchArgumentBytes,
        launchPlaceholderCount);
    AclsanLaunchData beforeCallback =
        MakeLaunchData(launchId, funcHandle, stream, apiName, numBlocks, ACL_SUCCESS, apiName, config);
    aclsan::AclsanCallbackDispatcher::DispatchLaunchBefore(beforeCallback);
    const aclError result = launch(
        funcHandle, numBlocks, stream, config, launchArguments, launchArgumentBytes, launchPlaceholders,
        launchPlaceholderCount);
    deferred.Release();
    // loaded-image 区间只有 launch 之后才能取到 base；登记发生在同一 stream 的 LAUNCH 事件
    // 分发之前，因此 ALLOC 仍满足普通 allocation 的 HostOrder。
    if (result == ACL_SUCCESS) {
        aclsan::AcquireTraceLaunchResources(prepared, funcHandle, stream);
    }
    ASCTOOL_DEBUG(
        "%s: function=%p blocks=%u stream=%p instrumented=%u result=%d", apiName, funcHandle, numBlocks, stream,
        static_cast<unsigned>(prepared.instrumented), result);
    std::string functionName;
    (void)aclsan::GetTraceFunctionName(funcHandle, functionName);
    const AclsanLaunchData callbackData =
        MakeLaunchData(launchId, funcHandle, stream, functionName, numBlocks, result, apiName, config);
    aclsan::AclsanCallbackDispatcher::DispatchLaunch(callbackData);
    aclsan::CompleteTraceLaunch(std::move(prepared), funcHandle, stream, result);
    return result;
}

template <typename Launch>
aclError LaunchKernelWithArgsArray(
    void* func, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* config, void** args, const char* apiName,
    Launch&& launch, uint32_t parameterPrefixBytes = 0) noexcept
{
    DeferredLaunch deferred;
    const auto fail = [&deferred, &func, &stream, &numBlocks, apiName](aclError status) {
        deferred.Release();
        AclsanLaunchData callback{};
        callback.common = MakeCallbackCommonData(apiName, status, sizeof(AclsanLaunchData));
        callback.function = func;
        callback.stream = stream;
        callback.numBlocks = numBlocks;
        aclsan::AclsanCallbackDispatcher::DispatchLaunch(callback);
        return status;
    };
    const auto deferredStatus = PrepareDeferredLaunch(func, 1, deferred);
    if (deferredStatus != ACL_SUCCESS) {
        return fail(deferredStatus);
    }
    aclsan::PreparedTraceLaunch prepared;
    // 原始参数由 ArgsArray 直接传递，这里只准备隐藏参数及其所属的 trace buffer。
    const auto traceStatus = aclsan::PrepareTraceLaunch(
        func, numBlocks, nullptr, 0, nullptr, 0, aclsan::TraceArgumentMode::ARGS_ARRAY, prepared, config);
    if (traceStatus != ACL_SUCCESS) {
        return fail(traceStatus);
    }
    prepared.binaryLease = deferred.variant;
    prepared.parameterPrefixBytes = parameterPrefixBytes;

    aclError result = ACL_SUCCESS;
    try {
        std::vector<void*> launchArgs;
        if (prepared.instrumented) {
            result =
                BuildInstrumentedArgsArray(func, prepared.deviceBuffer, prepared.traceArgumentOffset, args, launchArgs);
            if (result != ACL_SUCCESS) {
                ASCTOOL_ERROR("BuildInstrumentedArgsArray failed in %s: result=%d", apiName, result);
            }
        }
        if (result == ACL_SUCCESS) {
            AclsanLaunchData beforeCallback =
                MakeLaunchData(prepared.launchId, func, stream, apiName, numBlocks, ACL_SUCCESS, apiName, config);
            aclsan::AclsanCallbackDispatcher::DispatchLaunchBefore(beforeCallback);
            result = launch(func, numBlocks, stream, config, prepared.instrumented ? launchArgs.data() : args);
        }
    } catch (const std::bad_alloc&) {
        result = ACL_ERROR_BAD_ALLOC;
    } catch (...) {
        result = ACL_ERROR_FAILURE;
    }
    deferred.Release();
    if (result == ACL_SUCCESS) {
        aclsan::AcquireTraceLaunchResources(prepared, func, stream);
    }
    std::string functionName;
    (void)aclsan::GetTraceFunctionName(func, functionName);
    const AclsanLaunchData callbackData =
        MakeLaunchData(prepared.launchId, func, stream, functionName, numBlocks, result, apiName, config);
    aclsan::AclsanCallbackDispatcher::DispatchLaunch(callbackData);
    aclsan::CompleteTraceLaunch(std::move(prepared), func, stream, result);
    return result;
}

aclError aclrtLaunchKernelWithHostArgsHook(
    aclrtFuncHandle funcHandle, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* config, void* hostArgs,
    size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, size_t placeHolderNum) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtLaunchKernelWithHostArgsFunc>(
        ACL_RT_API_aclrtLaunchKernelWithHostArgs, "aclrtLaunchKernelWithHostArgs");
    return LaunchKernelWithHostArgs(
        funcHandle, numBlocks, stream, config, hostArgs, argsSize, placeHolderArray, placeHolderNum,
        "aclrtLaunchKernelWithHostArgs", original);
}

aclError aclrtLaunchKernelWithArgsArrayHook(
    void* func, uint32_t numBlocks, aclrtStream stream, aclrtLaunchKernelCfg* config, void** args) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtLaunchKernelWithArgsArrayFunc>(
        ACL_RT_API_aclrtLaunchKernelWithArgsArray, "aclrtLaunchKernelWithArgsArray");
    return LaunchKernelWithArgsArray(func, numBlocks, stream, config, args, "aclrtLaunchKernelWithArgsArray", original);
}

// On dav-3510, pure SIMT entry code reads a 24-byte Runtime launch header
// immediately before get_para_base(). This prefix is
// internal only for the pure SIMT launch APIs; ordinary launches keep no prefix.
constexpr uint32_t PURE_SIMT_PARAMETER_PREFIX_BYTES = 6U * sizeof(uint32_t);

// Trace records carry a linear logical block id. Keep the original three-dimensional
// launch geometry for Runtime and use its checked product for trace bounds/lifecycle.
aclError GetSimtBlockCount(dim3 grid, void* function, aclrtStream stream, const char* apiName, uint32_t& count) noexcept
{
    count = 1;
    for (const uint32_t dimension : {grid.x, grid.y, grid.z}) {
        if (dimension == 0 || count > UINT32_MAX / dimension) {
            count = 0;
            const auto callback = MakeLaunchData(0, function, stream, {}, 0, ACL_ERROR_INVALID_PARAM, apiName, nullptr);
            aclsan::AclsanCallbackDispatcher::DispatchLaunch(callback);
            return ACL_ERROR_INVALID_PARAM;
        }
        count *= dimension;
    }
    return ACL_SUCCESS;
}

aclError aclrtLaunchSIMTKernelWithHostArgsHook(
    void* func, dim3 gridDim, dim3 blockDim, size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* config,
    void* hostArgs, size_t argsSize, aclrtPlaceHolderInfo* placeHolderArray, size_t placeHolderNum) noexcept
{
    constexpr const char* apiName = "aclrtLaunchSIMTKernelWithHostArgs";
    uint32_t numBlocks = 0;
    ACLSAN_RETURN_IF_ACL_ERROR(GetSimtBlockCount(gridDim, func, stream, apiName, numBlocks), "Invalid SIMT grid");
    const auto original = GetOriginalRuntimeFunction<aclrtLaunchSIMTKernelWithHostArgsFunc>(
        ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs, apiName);
    const auto launch = [original, gridDim, blockDim, dynUbufSize](
                            void* function, uint32_t, aclrtStream launchStream, aclrtLaunchKernelCfg* cfg,
                            void* arguments, size_t bytes, aclrtPlaceHolderInfo* placeholders,
                            size_t placeholderCount) {
        return original(
            function, gridDim, blockDim, dynUbufSize, launchStream, cfg, arguments, bytes, placeholders,
            placeholderCount);
    };
    return LaunchKernelWithHostArgs(
        func, numBlocks, stream, config, hostArgs, argsSize, placeHolderArray, placeHolderNum, apiName, launch,
        PURE_SIMT_PARAMETER_PREFIX_BYTES);
}

aclError aclrtLaunchSIMTKernelWithArgsArrayHook(
    void* func, dim3 gridDim, dim3 blockDim, size_t dynUbufSize, aclrtStream stream, aclrtLaunchKernelCfg* config,
    void** args) noexcept
{
    constexpr const char* apiName = "aclrtLaunchSIMTKernelWithArgsArray";
    uint32_t numBlocks = 0;
    ACLSAN_RETURN_IF_ACL_ERROR(GetSimtBlockCount(gridDim, func, stream, apiName, numBlocks), "Invalid SIMT grid");
    const auto original = GetOriginalRuntimeFunction<aclrtLaunchSIMTKernelWithArgsArrayFunc>(
        ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray, apiName);
    const auto launch = [original, gridDim, blockDim, dynUbufSize](
                            void* function, uint32_t, aclrtStream launchStream, aclrtLaunchKernelCfg* cfg,
                            void** arguments) {
        return original(function, gridDim, blockDim, dynUbufSize, launchStream, cfg, arguments);
    };
    return LaunchKernelWithArgsArray(
        func, numBlocks, stream, config, args, apiName, launch, PURE_SIMT_PARAMETER_PREFIX_BYTES);
}

// DONE
aclError aclrtSynchronizeStreamHook(aclrtStream stream) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtSynchronizeStreamFunc>(
        ACL_RT_API_aclrtSynchronizeStream, "aclrtSynchronizeStream");
    const uint64_t frontier = aclsan::TraceStreamFrontier(stream);
    const aclError result = original(stream);
    ASCTOOL_DEBUG("aclrtSynchronizeStream: stream=%p result=%d", stream, result);
    if (result == ACL_SUCCESS) {
        (void)aclsan::CollectTraceStream(stream, true);
        aclsan::CollectRetiredTraceVariants();
    } else if (result == ACL_ERROR_RT_AICORE_EXCEPTION) {
        aclsan::CollectTraceAfterExecutionException(stream, frontier);
    }
    AclsanSynchronizeData callbackData = MakeSynchronizeData("aclrtSynchronizeStream", stream, result);
    callbackData.common.correlationId = frontier;
    aclsan::AclsanCallbackDispatcher::DispatchSynchronizeEnd(callbackData);
    return result;
}

// DONE
aclError aclrtSynchronizeStreamWithTimeoutHook(aclrtStream stream, int32_t timeout) noexcept
{
    const auto original = GetOriginalRuntimeFunction<aclrtSynchronizeStreamWithTimeoutFunc>(
        ACL_RT_API_aclrtSynchronizeStreamWithTimeout, "aclrtSynchronizeStreamWithTimeout");
    const uint64_t frontier = aclsan::TraceStreamFrontier(stream);
    const aclError result = original(stream, timeout);
    ASCTOOL_DEBUG("aclrtSynchronizeStreamWithTimeout: stream=%p timeout=%d result=%d", stream, timeout, result);
    if (result == ACL_SUCCESS) {
        (void)aclsan::CollectTraceStream(stream, true);
        aclsan::CollectRetiredTraceVariants();
    } else if (result == ACL_ERROR_RT_AICORE_EXCEPTION) {
        aclsan::CollectTraceAfterExecutionException(stream, frontier);
    }
    AclsanSynchronizeData callbackData = MakeSynchronizeData("aclrtSynchronizeStreamWithTimeout", stream, result);
    callbackData.common.correlationId = frontier;
    aclsan::AclsanCallbackDispatcher::DispatchSynchronizeEnd(callbackData);
    return result;
}

aclError aclrtBinaryUnLoadHook(aclrtBinHandle binHandle) noexcept
{
    const auto original =
        GetOriginalRuntimeFunction<aclrtBinaryUnLoadFunc>(ACL_RT_API_aclrtBinaryUnLoad, "aclrtBinaryUnLoad");
    std::shared_ptr<DeferredBinary> source;
    DeferredHandleKey sourceKey{DeferredDeviceId(), reinterpret_cast<uintptr_t>(binHandle)};
    if (!AdoptPatchedFile(binHandle, sourceKey.first)) {
        return ACL_ERROR_FAILURE;
    }
    {
        std::lock_guard<std::mutex> lock(g_deferredMutex);
        bool ambiguous = false;
        auto it = FindDeferred(g_deferredBinaries, sourceKey.first, sourceKey.second, ambiguous);
        if (ambiguous) {
            return ACL_ERROR_FAILURE;
        }
        if (it != g_deferredBinaries.end()) {
            sourceKey = it->first;
            source = it->second;
        }
    }
    if (sourceKey.first == UNKNOWN_DEFERRED_DEVICE) {
        std::lock_guard<std::mutex> lock(g_patchedFileMutex);
        size_t matches = 0;
        for (const auto& file : g_patchedFiles) {
            if (file.binary == binHandle) {
                ++matches;
                sourceKey.first = file.deviceId;
            }
        }
        if (matches > 1U) {
            aclsan::TraceIntegrityMark(0, 0, true);
            return ACL_ERROR_FAILURE;
        }
    }
    std::unique_lock<std::shared_mutex> lifecycle;
    // Preallocate retirement storage before the irreversible Runtime unload.
    std::list<std::shared_ptr<DeferredBinary>> retired;
    if (source) {
        try {
            retired.push_back(source);
            lifecycle = std::unique_lock<std::shared_mutex>(source->lifecycle);
        } catch (...) {
            return ACL_ERROR_BAD_ALLOC;
        }
    }
    const aclError result = original(binHandle);
    if (result == ACL_SUCCESS) {
        if (source) {
            source->closing = true;
            std::lock_guard<std::mutex> lock(g_deferredMutex);
            g_deferredBinaries.erase(sourceKey);
            for (auto it = g_deferredFunctions.begin(); it != g_deferredFunctions.end();) {
                it = it->second.source == source ? g_deferredFunctions.erase(it) : std::next(it);
            }
            g_retiredBinaries.splice(g_retiredBinaries.end(), retired);
        }
        aclsan::RecordTraceBinaryUnload(binHandle);
        std::lock_guard<std::mutex> lock(g_patchedFileMutex);
        g_patchedFiles.remove_if([binHandle, &sourceKey](const auto& file) {
            return file.binary == binHandle && file.deviceId == sourceKey.first;
        });
    }
    if (lifecycle.owns_lock()) {
        lifecycle.unlock();
    }
    CollectRetiredVariants();
    return result;
}

aclError aclrtResetDeviceHook(int32_t deviceId) noexcept
{
    using GetContext = aclError (*)(aclrtContext*);
    using SetContext = aclError (*)(aclrtContext);
    const auto getContext = reinterpret_cast<GetContext>(dlsym(RTLD_DEFAULT, "aclrtGetCurrentContext"));
    const auto setContext = reinterpret_cast<SetContext>(dlsym(RTLD_DEFAULT, "aclrtSetCurrentContext"));
    aclrtContext savedContext = nullptr;
    const aclError contextResult = getContext == nullptr ? ACL_ERROR_FAILURE : getContext(&savedContext);
    if ((contextResult != ACL_SUCCESS && contextResult != ACL_ERROR_RT_CONTEXT_NULL) ||
        (savedContext != nullptr && setContext == nullptr)) {
        aclsan::TraceIntegrityMark(0, 0, true);
        return ACL_ERROR_FAILURE;
    }
    const bool hasContext = contextResult == ACL_SUCCESS && savedContext != nullptr;
    std::shared_ptr<void> resetGuard;
    const bool quiesced = aclsan::QuiesceTraceDevice(deviceId, hasContext, resetGuard);
    const bool restored = !hasContext || setContext(savedContext) == ACL_SUCCESS;
    if (!quiesced || !restored) {
        if (resetGuard) {
            aclsan::FinishTraceDeviceReset(deviceId, false);
        }
        aclsan::TraceIntegrityMark(0, 0, true);
        return ACL_ERROR_FAILURE;
    }
    const auto original =
        GetOriginalRuntimeFunction<aclrtResetDeviceFunc>(ACL_RT_API_aclrtResetDevice, "aclrtResetDevice");
    const aclError result = original(deviceId);
    aclsan::FinishTraceDeviceReset(deviceId, result == ACL_SUCCESS);
    if (result == ACL_SUCCESS) {
        ResetDeferredDevice(deviceId);
    }
    return result;
}

using ConfigureHook = int32_t (*)(bool enable) noexcept;

struct RuntimeHookBinding {
    aclrtApiId apiId;
    const char* hookName;
    ConfigureHook configure;
};

// 如果enable，那么注册hook; 反之清除hook
template <aclrtApiId ApiId, auto Register, auto Hook>
int32_t ConfigureRuntimeHook(bool enable) noexcept
{
    return enable ? Register(Hook) : acltoolClearCallback(ApiId);
}

template <aclrtApiId ApiId, auto Register, auto Hook>
constexpr RuntimeHookBinding MakeRuntimeHookBinding(const char* hookName) noexcept
{
    return {ApiId, hookName, ConfigureRuntimeHook<ApiId, Register, Hook>};
}

// aclrtApiId + acl_tool_inject提供的注册aclrt的函数 + 我们实现的hook函数
const std::array<RuntimeHookBinding, 19> RUNTIME_HOOK_BINDINGS = {{
    MakeRuntimeHookBinding<ACL_RT_API_aclrtMemset, acltoolRegisterAclrtMemsetCallbacks, aclrtMemsetHook>("aclrtMemset"),
    MakeRuntimeHookBinding<ACL_RT_API_aclrtMemcpy, acltoolRegisterAclrtMemcpyCallbacks, aclrtMemcpyHook>("aclrtMemcpy"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtMallocWithCfg, acltoolRegisterAclrtMallocWithCfgCallbacks, aclrtMallocWithCfgHook>(
        "aclrtMallocWithCfg"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtBinaryLoadFromFile, acltoolRegisterAclrtBinaryLoadFromFileCallbacks,
        aclrtBinaryLoadFromFileHook>("aclrtBinaryLoadFromFile"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtLaunchKernelWithHostArgs, acltoolRegisterAclrtLaunchKernelWithHostArgsCallbacks,
        aclrtLaunchKernelWithHostArgsHook>("aclrtLaunchKernelWithHostArgs"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtLaunchKernelWithArgsArray, acltoolRegisterAclrtLaunchKernelWithArgsArrayCallbacks,
        aclrtLaunchKernelWithArgsArrayHook>("aclrtLaunchKernelWithArgsArray"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtLaunchSIMTKernelWithHostArgs, acltoolRegisterAclrtLaunchSIMTKernelWithHostArgsCallbacks,
        aclrtLaunchSIMTKernelWithHostArgsHook>("aclrtLaunchSIMTKernelWithHostArgs"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtLaunchSIMTKernelWithArgsArray, acltoolRegisterAclrtLaunchSIMTKernelWithArgsArrayCallbacks,
        aclrtLaunchSIMTKernelWithArgsArrayHook>("aclrtLaunchSIMTKernelWithArgsArray"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtBinaryLoadFromData, acltoolRegisterAclrtBinaryLoadFromDataCallbacks,
        aclrtBinaryLoadFromDataHook>("aclrtBinaryLoadFromData"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtBinaryGetFunction, acltoolRegisterAclrtBinaryGetFunctionCallbacks, aclrtBinaryGetFunctionHook>(
        "aclrtBinaryGetFunction"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtBinaryGetFunctionByEntry, acltoolRegisterAclrtBinaryGetFunctionByEntryCallbacks,
        aclrtBinaryGetFunctionByEntryHook>("aclrtBinaryGetFunctionByEntry"),
    MakeRuntimeHookBinding<ACL_RT_API_aclrtMalloc, acltoolRegisterAclrtMallocCallbacks, aclrtMallocHook>("aclrtMalloc"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtMallocAlign32, acltoolRegisterAclrtMallocAlign32Callbacks, aclrtMallocAlign32Hook>(
        "aclrtMallocAlign32"),
    MakeRuntimeHookBinding<ACL_RT_API_aclrtFree, acltoolRegisterAclrtFreeCallbacks, aclrtFreeHook>("aclrtFree"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtSynchronizeStream, acltoolRegisterAclrtSynchronizeStreamCallbacks, aclrtSynchronizeStreamHook>(
        "aclrtSynchronizeStream"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtSynchronizeStreamWithTimeout, acltoolRegisterAclrtSynchronizeStreamWithTimeoutCallbacks,
        aclrtSynchronizeStreamWithTimeoutHook>("aclrtSynchronizeStreamWithTimeout"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtGetFuncBySymbol, acltoolRegisterAclrtGetFuncBySymbolCallbacks, aclrtGetFuncBySymbolHook>(
        "aclrtGetFuncBySymbol"),
    MakeRuntimeHookBinding<
        ACL_RT_API_aclrtBinaryUnLoad, acltoolRegisterAclrtBinaryUnLoadCallbacks, aclrtBinaryUnLoadHook>(
        "aclrtBinaryUnLoad"),
    MakeRuntimeHookBinding<ACL_RT_API_aclrtResetDevice, acltoolRegisterAclrtResetDeviceCallbacks, aclrtResetDeviceHook>(
        "aclrtResetDevice"),
}};

} // namespace

namespace aclsan {

void CollectRetiredTraceVariants() noexcept { CollectRetiredVariants(); }

AclsanStatus ResolveActiveDeviceCallStack(uint64_t pc, device_runtime::CallStackResult* result) noexcept
{
    if (result == nullptr) {
        ASCTOOL_ERROR("%s: %s is nullptr", "ResolveActiveDeviceCallStack", "result");
        return ACLSAN_STATUS_ERROR_INVALID_PARAMETER;
    }
    *result = ResolveTraceDeviceCallStack(pc);
    return ACLSAN_STATUS_SUCCESS;
}

// 针对所有hook相关的aclrt函数，不在requiredHooks中的统一清除hook，反之注册hook
void ApplyRuntimeHooks(const std::set<aclrtApiId>& requiredHooks) noexcept
{
    if (!requiredHooks.empty()) {
        StartTraceWorkerAcceptance();
    } else {
        StopTraceWorker();
    }
    for (const RuntimeHookBinding& binding : RUNTIME_HOOK_BINDINGS) {
        const bool enable = aclsan::IsHookRequired(requiredHooks, binding.apiId);
        if (binding.configure(enable) != 0) {
            AbortHookFailure(
                binding.hookName, enable ? "register_runtime_hook" : "clear_runtime_hook",
                "Runtime hook configuration returned nonzero");
        }
    }
}

} // namespace aclsan
