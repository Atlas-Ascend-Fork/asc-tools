/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
// 参考实现：函数级 SIMT Warp Occupancy 的一次完整采集。
// 真实设备执行需要在 Linux/CANN/npu_compute 环境中验证；本文件不依赖 main.cpp。

#include "npubit_simt_warp_collector.h"

#include "injection/injection_hook.h"
#include "common/npu_compute_log.h"

#include <algorithm>
#include <securec.h>
#include <cstdlib>
#include <cstring>
#include <acl/error_codes/rt_error_codes.h>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace aclpti::profiling {
namespace {

constexpr NpubitInstrumentationType kWarpPass = NPUBIT_SIMT_WARP_OCCUPANCY;
constexpr NpubitInstrumentationType kVfAnchorPass = NPUBIT_VECTOR_FUNCTION_UTILIZATION;

bool CheckNpubit(NpubitStatus status, const char* api, std::string& error)
{
    ASCTOOL_DEBUG("[simt-warp] api=%s status=%d", api, static_cast<int>(status));
    if (status == NPUBIT_SUCCESS) {
        return true;
    }
    error = std::string(api) + " failed, status=" + std::to_string(static_cast<int>(status));
    return false;
}

struct NpubitBufferGuard {
    void* data = nullptr;

    ~NpubitBufferGuard()
    {
        if (data != nullptr) {
            (void)NpubitFree(&data);
        }
    }
};

struct WarpResources {
    void* traceDevice = nullptr;
    std::size_t traceBytes = 0;
    void* bbMapHost = nullptr;
    std::size_t bbMapBytes = 0;
    void* bbMapDevice = nullptr;
    aclrtBinHandle binary = nullptr;
    aclrtFuncHandle function = nullptr;
    std::unique_ptr<std::vector<char>> patchedObject = std::make_unique<std::vector<char>>();

    ~WarpResources()
    {
        const auto unload =
            reinterpret_cast<aclrtBinaryUnLoadFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryUnLoad));
        if (binary != nullptr && (unload == nullptr || unload(binary) != ACL_SUCCESS)) {
            // Runtime 可能仍持有 ELF 内存；保留 image 直至进程退出。
            (void)patchedObject.release();
        }

        const auto freeDevice = reinterpret_cast<aclrtFreeFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFree));
        if (freeDevice != nullptr) {
            if (traceDevice != nullptr) {
                (void)freeDevice(traceDevice);
            }
            if (bbMapDevice != nullptr) {
                (void)freeDevice(bbMapDevice);
            }
        }

        if (bbMapHost != nullptr) {
            (void)NpubitFree(&bbMapHost);
        }
    }
};

struct HandleGuard {
    NpubitHandle handle = nullptr;

    ~HandleGuard()
    {
        if (handle != nullptr) {
            (void)NpubitReleaseHandle(handle);
        }
    }
};

bool AllocateBbMap(NpubitHandle handle, WarpResources& resources, std::string& args, std::string& error)
{
    if (!CheckNpubit(
            NpubitAllocateBasicBlockBuffer(handle, &resources.bbMapHost, &resources.bbMapBytes),
            "NpubitAllocateBasicBlockBuffer", error)) {
        return false;
    }
    if (resources.bbMapBytes == 0) {
        resources.bbMapHost = nullptr;
        return true;
    }
    if (resources.bbMapHost == nullptr) {
        error = "BB map returned non-zero size with null host buffer";
        return false;
    }

    const auto allocate = reinterpret_cast<aclrtMallocFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMalloc));
    if (allocate == nullptr ||
        allocate(&resources.bbMapDevice, resources.bbMapBytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS ||
        resources.bbMapDevice == nullptr) {
        error = "device BB map allocation failed";
        return false;
    }

    args = "bb-map-address:" + std::to_string(reinterpret_cast<std::uintptr_t>(resources.bbMapDevice)) +
           ",bb-map-section-size:" + std::to_string(resources.bbMapBytes);
    return true;
}

bool InitializeBbMap(NpubitHandle handle, WarpResources& resources, std::string& error)
{
    if (resources.bbMapBytes == 0) {
        return true;
    }
    if (!CheckNpubit(
            NpubitInitializeBasicBlockBufferMap(handle, resources.bbMapHost, resources.bbMapBytes),
            "NpubitInitializeBasicBlockBufferMap", error)) {
        return false;
    }

    const auto copy = reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
    if (copy == nullptr || copy(
                               resources.bbMapDevice, resources.bbMapBytes, resources.bbMapHost, resources.bbMapBytes,
                               ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        error = "device BB map upload failed";
        return false;
    }
    (void)NpubitFree(&resources.bbMapHost);
    return true;
}

bool ExtractInstrumentedObject(NpubitHandle handle, std::vector<char>& object, std::string& error)
{
    std::size_t objectBytes = 0;
    if (!CheckNpubit(
            NpubitGetObjectSize(handle, NPUBIT_OBJECT_INSTRUMENTED, &objectBytes), "NpubitGetObjectSize", error)) {
        return false;
    }
    if (objectBytes == 0) {
        error = "instrumented object size is zero";
        return false;
    }
    object.resize(objectBytes);
    return CheckNpubit(
        NpubitGetObject(handle, NPUBIT_OBJECT_INSTRUMENTED, object.data(), object.size()), "NpubitGetObject", error);
}

bool LoadInstrumentedFunction(const SimtWarpBinarySource& source, WarpResources& resources, std::string& error)
{
    if (source.name.empty()) {
        error = "SimtWarpBinarySource.name is empty; this reference uses name-based lookup";
        return false;
    }

    const auto load =
        reinterpret_cast<aclrtBinaryLoadFromDataFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryLoadFromData));
    const auto get =
        reinterpret_cast<aclrtBinaryGetFunctionFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryGetFunction));
    if (load == nullptr || get == nullptr) {
        error = "original binary load/get API is unavailable";
        return false;
    }

    // Runtime 的 options 字段是非 const 指针；保留 source 不变，使用局部副本。
    auto copiedOptions = source.options;
    aclrtBinaryLoadOptions loadOptions{copiedOptions.data(), copiedOptions.size()};
    if (load(resources.patchedObject->data(), resources.patchedObject->size(), &loadOptions, &resources.binary) !=
            ACL_SUCCESS ||
        get(resources.binary, source.name.c_str(), &resources.function) != ACL_SUCCESS ||
        resources.function == nullptr) {
        error = "instrumented binary load or function lookup failed";
        return false;
    }
    return true;
}

bool ClearTrace(const WarpResources& resources, std::string& error)
{
    const auto memsetDevice = reinterpret_cast<aclrtMemsetFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemset));
    if (memsetDevice == nullptr || resources.traceDevice == nullptr || resources.traceBytes == 0 ||
        memsetDevice(resources.traceDevice, resources.traceBytes, 0, resources.traceBytes) != ACL_SUCCESS) {
        error = "trace buffer clear failed";
        return false;
    }
    return true;
}

bool CopyTrace(const WarpResources& resources, std::vector<std::uint8_t>& raw, std::string& error)
{
    raw.resize(resources.traceBytes);
    const auto copy = reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
    if (copy == nullptr ||
        copy(raw.data(), raw.size(), resources.traceDevice, resources.traceBytes, ACL_MEMCPY_DEVICE_TO_HOST) !=
            ACL_SUCCESS) {
        raw.clear();
        error = "trace Device-to-Host copy failed";
        return false;
    }
    return true;
}

bool DecodeWarpAnalysis(
    const void* data, std::size_t bytes, const std::string& functionName, std::vector<SimtWarpRecord>& records,
    std::string& error)
{
    if (data == nullptr && bytes != 0) {
        error = "Analyze returned null data with non-zero size";
        return false;
    }
    if (bytes % sizeof(WarpOccupancyAnalyzeData) != 0) {
        error = "Analyze output is not aligned to WarpOccupancyAnalyzeData";
        return false;
    }

    const auto* input = static_cast<const WarpOccupancyAnalyzeData*>(data);
    const std::size_t count = bytes / sizeof(WarpOccupancyAnalyzeData);
    records.reserve(records.size() + count);
    for (std::size_t index = 0; index < count; ++index) {
        SimtWarpRecord record;
        record.functionName = functionName;
        record.aicoreId = input[index].aicore_id;
        record.subcoreId = input[index].subcore_id;
        record.vfInstrAddress = input[index].vf_instr_address;
        record.warpId = input[index].warp_id;
        record.startTime = input[index].start_time;
        record.endTime = input[index].end_time;
        record.duration = input[index].duration;
        records.push_back(std::move(record));
    }
    return true;
}

aclptiSimtVfKind DecodeVfKind(NpubitInstructionName name)
{
    switch (name) {
        case NPUBIT_INSTRUCTION_NAME_SCALAR_VF:
        case NPUBIT_INSTRUCTION_NAME_SCALAR_VFI:
            return aclptiSimtVfKind::Simd;
        case NPUBIT_INSTRUCTION_NAME_SCALAR_VF_SIMT:
            return aclptiSimtVfKind::Simt;
        default:
            return aclptiSimtVfKind::Unknown;
    }
}

void ClassifyVfAnchors(NpubitHandle handle, std::vector<aclptiSimtVfAnchor>& anchors)
{
    std::map<uint64_t, size_t> addressIndices;
    std::vector<uint64_t> addresses;
    for (const auto& anchor : anchors) {
        if (addressIndices.emplace(anchor.vfInstrAddress, addresses.size()).second) {
            addresses.push_back(anchor.vfInstrAddress);
        }
    }
    NpubitInstructionName* names = nullptr;
    // The VF pass reports PCs from the original object owned by this handle.
    const auto status = NpubitGetInstructionNames(handle, addresses.data(), addresses.size(), &names);
    const NpubitBufferGuard buffer{names};
    if (status != NPUBIT_SUCCESS || names == nullptr) {
        ASCTOOL_DEBUG("[simt-warp] cannot classify VF call sites status=%d", static_cast<int>(status));
        return;
    }
    for (auto& anchor : anchors) {
        anchor.kind = DecodeVfKind(names[addressIndices.at(anchor.vfInstrAddress)]);
    }
}

bool DecodeVfAnchors(NpubitHandle handle, const void* data, std::size_t bytes, SimtWarpData& output, std::string& error)
{
    if (data == nullptr || bytes == 0 || bytes % sizeof(NpubitVectorFunctionUtilizationRecord) != 0) {
        error = "invalid or empty Vector Function Utilization output";
        return false;
    }
    std::vector<aclptiSimtVfAnchor> anchors;
    anchors.reserve(bytes / sizeof(NpubitVectorFunctionUtilizationRecord));
    const auto* raw = static_cast<const unsigned char*>(data);
    for (std::size_t offset = 0; offset < bytes; offset += sizeof(NpubitVectorFunctionUtilizationRecord)) {
        NpubitVectorFunctionUtilizationRecord record{};
        if (memcpy_s(&record, sizeof(record), raw + offset, sizeof(record)) != EOK) {
            error = "cannot read Vector Function Utilization record";
            return false;
        }
        if (record.start_time == 0 || record.end_time < record.start_time) {
            error = "invalid Vector Function Utilization counter range";
            return false;
        }
        const auto type = static_cast<unsigned>(record.subcore_type);
        if (type > static_cast<unsigned>(NPUBIT_SUBCORE_TYPE_CUBE)) {
            error = "invalid Vector Function Utilization subcore";
            return false;
        }
        anchors.push_back(
            {record.aicore_id, static_cast<uint8_t>(type), record.vf_instr_address, record.start_time,
             record.end_time});
    }
    ClassifyVfAnchors(handle, anchors);
    output.vfAnchors = std::move(anchors);
    return true;
}

bool ProcessTrace(
    NpubitHandle handle, NpubitInstrumentationType pass, const std::vector<std::uint8_t>& raw,
    const std::string& functionName, SimtWarpData& output, std::string& error)
{
    void* processedData = nullptr;
    std::size_t processedBytes = 0;
    if (!CheckNpubit(
            NpubitProcess(handle, pass, raw.data(), raw.size(), &processedData, &processedBytes), "NpubitProcess",
            error)) {
        return false;
    }
    NpubitBufferGuard processed{processedData};
    if (pass == kVfAnchorPass) {
        return DecodeVfAnchors(handle, processedData, processedBytes, output, error);
    }
    void* analyzedData = nullptr;
    std::size_t analyzedBytes = 0;
    if (!CheckNpubit(
            NpubitAnalyze(handle, pass, processedData, processedBytes, nullptr, 0, &analyzedData, &analyzedBytes),
            "NpubitAnalyze", error)) {
        return false;
    }
    NpubitBufferGuard analyzed{analyzedData};
    return DecodeWarpAnalysis(analyzedData, analyzedBytes, functionName, output.records, error);
}

bool PreparePass(
    const std::vector<char>& image, NpubitInstrumentationType pass, const SimtWarpCollectOptions& options,
    HandleGuard& handle, WarpResources& resources, std::string& error)
{
    if (!CheckNpubit(
            NpubitCreateHandle(image.data(), image.size(), &handle.handle, options.platform, options.needsVerify),
            "NpubitCreateHandle", error)) {
        return false;
    }

    if (!CheckNpubit(NpubitSetHardwareContext(handle.handle, &options.hardware), "NpubitSetHardwareContext", error)) {
        return false;
    }

    std::string instrumentationArgs;
    if (!AllocateBbMap(handle.handle, resources, instrumentationArgs, error) ||
        !CheckNpubit(NpubitGetBufferSize(handle.handle, pass, &resources.traceBytes), "NpubitGetBufferSize", error)) {
        return false;
    }
    if (resources.traceBytes == std::numeric_limits<std::size_t>::max()) {
        resources.traceBytes = options.traceFallbackBytes;
        ASCTOOL_DEBUG("[simt-warp] trace buffer uses fallback bytes=%zu", resources.traceBytes);
    } else {
        ASCTOOL_DEBUG("[simt-warp] trace buffer bytes=%zu", resources.traceBytes);
    }
    if (resources.traceBytes == 0) {
        error = "trace buffer size is zero";
        return false;
    }

    const auto allocate = reinterpret_cast<aclrtMallocFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMalloc));
    if (allocate == nullptr ||
        allocate(&resources.traceDevice, resources.traceBytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS ||
        resources.traceDevice == nullptr) {
        error = "trace Device buffer allocation failed";
        return false;
    }

    if (!CheckNpubit(
            NpubitInstrument(
                handle.handle, pass, resources.traceDevice, resources.traceBytes,
                instrumentationArgs.empty() ? nullptr : instrumentationArgs.c_str()),
            "NpubitInstrument", error) ||
        !InitializeBbMap(handle.handle, resources, error) ||
        !ExtractInstrumentedObject(handle.handle, *resources.patchedObject, error) || !ClearTrace(resources, error)) {
        return false;
    }

    return true;
}

bool SynchronizeAndRestoreOriginal(
    const ReplayMemory& replayMemory, aclrtSynchronizeStreamFunc synchronize, aclrtStream stream, std::string& error)
{
    // Complete the original work before replay mutates its device buffers.
    const aclError originalSyncStatus = synchronize(stream);
    ASCTOOL_DEBUG("[simt-warp] original synchronize status=%d", originalSyncStatus);
    if (originalSyncStatus != ACL_SUCCESS) {
        error = "original Kernel synchronize failed: " + std::to_string(originalSyncStatus);
        return false;
    }

    const auto restore = replayMemory.Restore();
    if (restore != ACLPTI_SUCCESS) {
        error = "ReplayMemory::Restore failed: " + std::to_string(restore);
        return false;
    }
    return true;
}

struct ReplayStatus {
    aclError launch;
    aclError synchronize;
};

ReplayStatus RunInstrumentedReplay(
    const ReplayMemory& replayMemory, const ReplayLaunchFunction& launch, aclrtSynchronizeStreamFunc synchronize,
    aclrtStream stream, const WarpResources& vf, const WarpResources& warp)
{
    ReplayStatus status{launch(warp.function, false), synchronize(stream)};
    ASCTOOL_DEBUG("[simt-warp] instrumented launch status=%d synchronize status=%d", status.launch, status.synchronize);

    // Retry once only for the observed Vector Core exception and only when enabled.
    const auto* retryEnv = std::getenv("NPU_COMPUTE_NPUBIT_RETRY_VECTOR_EXCEPTION");
    if (status.launch != ACL_SUCCESS || status.synchronize != ACL_ERROR_RT_VECTOR_CORE_EXCEPTION ||
        retryEnv == nullptr || std::strcmp(retryEnv, "1") != 0) {
        return status;
    }
    std::fprintf(
        stderr,
        "[SimtWarp] warning: first instrumented replay failed with vector core exception "
        "%d; clearing trace and retrying once\n",
        status.synchronize);
    std::string retryError;
    if (!ClearTrace(vf, retryError) || !ClearTrace(warp, retryError)) {
        ASCTOOL_DEBUG("[simt-warp] instrumented retry clear failed: %s", retryError.c_str());
        return status;
    }
    const auto retryRestore = replayMemory.Restore();
    if (retryRestore != ACLPTI_SUCCESS) {
        ASCTOOL_DEBUG("[simt-warp] instrumented retry restore status=%d", static_cast<int>(retryRestore));
        return status;
    }

    status = {launch(warp.function, false), synchronize(stream)};
    ASCTOOL_DEBUG(
        "[simt-warp] instrumented retry launch status=%d synchronize status=%d", status.launch, status.synchronize);
    return status;
}

} // namespace

aclptiResult CollectNpubitSimtWarp(
    const SimtWarpBinarySource& source, const ReplayMemory& replayMemory, const ReplayLaunchFunction& launch,
    aclrtSynchronizeStreamFunc synchronize, aclrtStream stream, const SimtWarpCollectOptions& options,
    SimtWarpData& output)
{
    output = {};
    if (!source.image || source.image->empty() || !launch || !synchronize || options.platform == UNDEFINED ||
        options.hardware.die_count == 0 || options.hardware.aicore_per_die == 0) {
        return output.status = ACLPTI_ERROR_INVALID_PARAMETER;
    }
    std::string error;
    HandleGuard vfHandle{}, warpHandle{};
    auto vf = std::make_unique<WarpResources>();
    auto resources = std::make_unique<WarpResources>();
    if (!PreparePass(*source.image, kVfAnchorPass, options, vfHandle, *vf, error) ||
        !PreparePass(*vf->patchedObject, kWarpPass, options, warpHandle, *resources, error) ||
        !LoadInstrumentedFunction(source, *resources, error)) {
        output.errors.push_back(std::move(error));
        return output.status = ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (!SynchronizeAndRestoreOriginal(replayMemory, synchronize, stream, error)) {
        output.errors.push_back(std::move(error));
        return output.status = ACLPTI_ERROR_RESULT_UNRELIABLE;
    }

    const auto replay = RunInstrumentedReplay(replayMemory, launch, synchronize, stream, *vf, *resources);
    if (replay.synchronize != ACL_SUCCESS) {
        // Device code and buffers may still be in use. Keep them and their npubit handles alive.
        (void)resources.release();
        (void)vf.release();
        vfHandle.handle = nullptr;
        warpHandle.handle = nullptr;
        output.errors.push_back("instrumented Kernel synchronize failed: " + std::to_string(replay.synchronize));
        return output.status = ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    if (replay.launch != ACL_SUCCESS) {
        output.errors.push_back("instrumented Kernel launch failed: " + std::to_string(replay.launch));
        return output.status = ACLPTI_ERROR_RESULT_UNRELIABLE;
    }

    std::vector<std::uint8_t> rawTrace;
    if (!CopyTrace(*resources, rawTrace, error) ||
        !ProcessTrace(warpHandle.handle, kWarpPass, rawTrace, source.name, output, error) ||
        !CopyTrace(*vf, rawTrace, error) ||
        !ProcessTrace(vfHandle.handle, kVfAnchorPass, rawTrace, source.name, output, error)) {
        output.errors.push_back(std::move(error));
        return output.status = ACLPTI_ERROR_PROFILING_FAILED;
    }
    return output.status = ACLPTI_SUCCESS;
}

} // namespace aclpti::profiling
