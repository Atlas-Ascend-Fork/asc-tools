/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "source_counter_collector.h"

#if defined(NPUCOMPUTE_NPUBIT_SONAME)
#include "binary_instrumenter.h"
#include "injection/injection_hook.h"
#include "npu_compute_log.h"
#include "npubit_collector.h"

#include <npubit.h>

#include <cmath>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#endif

namespace aclpti::profiling {
namespace {
constexpr std::size_t kBytesPerMiB = 1024U * 1024U;
} // namespace

std::size_t SourceCounterBufferSize(std::size_t reported, std::size_t defaultTraceMiB)
{
    return reported == std::numeric_limits<std::size_t>::max() ? defaultTraceMiB * kBytesPerMiB : reported;
}

#if defined(NPUCOMPUTE_NPUBIT_SONAME)
namespace {
constexpr std::size_t kDefaultTraceMiB = 4;
constexpr std::size_t kLargeTraceMiB = 64;

struct CloseLibrary {
    void operator()(void* library) const { (void)dlclose(library); }
};
using Library = std::unique_ptr<void, CloseLibrary>;

template <typename Function>
Function LibrarySymbol(const Library& library, const char* name)
{
    return library ? reinterpret_cast<Function>(dlsym(library.get(), name)) : nullptr;
}

template <typename Function>
Function Original(aclrtApiId id)
{
    return reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(id));
}

struct NpubitApi {
    Library library{dlopen(NPUCOMPUTE_NPUBIT_SONAME, RTLD_NOW | RTLD_LOCAL)};
    decltype(&NpubitGetVersion) getVersion = LibrarySymbol<decltype(getVersion)>(library, "NpubitGetVersion");
    decltype(&NpubitCreateHandle) create = LibrarySymbol<decltype(create)>(library, "NpubitCreateHandle");
    decltype(&NpubitReleaseHandle) release = LibrarySymbol<decltype(release)>(library, "NpubitReleaseHandle");
    decltype(&NpubitSetHardwareContext) setHardware =
        LibrarySymbol<decltype(setHardware)>(library, "NpubitSetHardwareContext");
    decltype(&NpubitAllocateBasicBlockBuffer) allocateMap =
        LibrarySymbol<decltype(allocateMap)>(library, "NpubitAllocateBasicBlockBuffer");
    decltype(&NpubitInitializeBasicBlockBufferMap) initializeMap =
        LibrarySymbol<decltype(initializeMap)>(library, "NpubitInitializeBasicBlockBufferMap");
    decltype(&NpubitGetBufferSize) getBufferSize =
        LibrarySymbol<decltype(getBufferSize)>(library, "NpubitGetBufferSize");
    decltype(&NpubitInstrument) instrument = LibrarySymbol<decltype(instrument)>(library, "NpubitInstrument");
    decltype(&NpubitGetObjectSize) getObjectSize =
        LibrarySymbol<decltype(getObjectSize)>(library, "NpubitGetObjectSize");
    decltype(&NpubitGetObject) getObject = LibrarySymbol<decltype(getObject)>(library, "NpubitGetObject");
    decltype(&NpubitProcess) process = LibrarySymbol<decltype(process)>(library, "NpubitProcess");
    decltype(&NpubitAnalyze) analyze = LibrarySymbol<decltype(analyze)>(library, "NpubitAnalyze");
    decltype(&NpubitFree) free = LibrarySymbol<decltype(free)>(library, "NpubitFree");

    bool Open(std::string& error) const
    {
        if (!library) {
            error = "cannot load " NPUCOMPUTE_NPUBIT_SONAME;
            return false;
        }
        if (!(getVersion && create && release && setHardware && allocateMap && initializeMap && getBufferSize &&
              instrument && getObjectSize && getObject && process && analyze && free)) {
            error = "NPUbit shared library lacks required SourceCounter APIs";
            return false;
        }
        std::uint32_t major = 0;
        std::uint32_t minor = 0;
        std::uint32_t patch = 0;
        if (getVersion(&major, &minor, &patch) != NPUBIT_SUCCESS ||
            std::tie(major, minor, patch) !=
                std::make_tuple(NPUBIT_VERSION_MAJOR, NPUBIT_VERSION_MINOR, NPUBIT_VERSION_PATCH)) {
            error = "NPUbit headers and runtime library versions do not match";
            return false;
        }
        return true;
    }
};

struct RuntimeApi {
    aclrtMallocFunc allocate = Original<aclrtMallocFunc>(ACL_RT_API_aclrtMalloc);
    aclrtFreeFunc free = Original<aclrtFreeFunc>(ACL_RT_API_aclrtFree);
    aclrtMemsetFunc setMemory = Original<aclrtMemsetFunc>(ACL_RT_API_aclrtMemset);
    aclrtMemcpyFunc copy = Original<aclrtMemcpyFunc>(ACL_RT_API_aclrtMemcpy);
    aclrtBinaryLoadFromDataFunc load = Original<aclrtBinaryLoadFromDataFunc>(ACL_RT_API_aclrtBinaryLoadFromData);
    aclrtBinaryUnLoadFunc unload = Original<aclrtBinaryUnLoadFunc>(ACL_RT_API_aclrtBinaryUnLoad);
    aclrtBinaryGetFunctionFunc getFunction = Original<aclrtBinaryGetFunctionFunc>(ACL_RT_API_aclrtBinaryGetFunction);

    bool Ready() const { return allocate && free && setMemory && copy && load && unload && getFunction; }
};

struct SourceDevice {
    NpubitPlatformType platform = UNDEFINED;
    AiCoreTopology topology{};
};

bool ReadSourceDevice(std::int32_t deviceId, SourceDevice& device, std::string& error)
{
    const auto getSoc = Original<aclrtGetSocNameFunc>(ACL_RT_API_aclrtGetSocName);
    const auto* name = getSoc == nullptr ? nullptr : getSoc();
    const std::string_view soc = name == nullptr ? "" : name;
    constexpr std::string_view prPrefix = "Ascend950PR";
    constexpr std::string_view dtPrefix = "Ascend950DT";
    if (soc.compare(0, prPrefix.size(), prPrefix) != 0 && soc.compare(0, dtPrefix.size(), dtPrefix) != 0) {
        error = "current Runtime SoC is not supported by NPUbit V100: " + std::string(soc);
        return false;
    }
    device.platform = V100;
    if (!ReadNpubitTopology(deviceId, device.topology)) {
        error = "cannot determine NPUbit AI core topology";
        return false;
    }
    return true;
}

bool SdkSucceeded(NpubitStatus status, const char* operation, std::string& error)
{
    if (status == NPUBIT_SUCCESS) {
        return true;
    }
    error = std::string(operation) + " failed: status=" + std::to_string(status);
    return false;
}

struct SdkHandle {
    decltype(&NpubitReleaseHandle) release;
    NpubitHandle value = nullptr;
    ~SdkHandle()
    {
        if (value != nullptr) {
            (void)release(value);
        }
    }
};

struct SdkBuffer {
    decltype(&NpubitFree) free;
    void* data = nullptr;
    std::size_t bytes = 0;
    ~SdkBuffer()
    {
        if (data != nullptr) {
            (void)free(&data);
        }
    }

    template <typename Record>
    bool HasRecordsOf() const
    {
        return bytes == 0 || (data != nullptr && bytes % sizeof(Record) == 0 &&
                              reinterpret_cast<std::uintptr_t>(data) % alignof(Record) == 0);
    }
};

bool ValidProcessedRecords(NpubitInstrumentationType type, const SdkBuffer& processed)
{
    switch (type) {
        case NPUBIT_SIMT_GLOBAL_MEMORY_PATTERNS:
            return processed.HasRecordsOf<NpubitSimtGmPatternsRecord>();
        case NPUBIT_SIMT_BANK_CONFLICTS:
            return processed.HasRecordsOf<NpubitSimtBankConflictsRecord>();
        case NPUBIT_PUSH_PB_STATE:
            return processed.HasRecordsOf<NpubitPushPBStateRecord>();
        default:
            return false;
    }
}

struct DeviceRound {
    explicit DeviceRound(const RuntimeApi& api) : runtime(api) {}
    DeviceRound(const DeviceRound&) = delete;
    DeviceRound& operator=(const DeviceRound&) = delete;
    ~DeviceRound()
    {
        // An undrained launch may still reference the companion ELF, trace and map.
        if (!streamDrained || (binary != nullptr && runtime.unload(binary) != ACL_SUCCESS)) {
            (void)image.release();
            ASCTOOL_WARNING("[aclpti] SourceCounter retaining resources for a companion that could not be unloaded");
            return;
        }
        if (trace != nullptr) {
            (void)runtime.free(trace);
        }
        if (map != nullptr) {
            (void)runtime.free(map);
        }
    }

    const RuntimeApi& runtime;
    void* trace = nullptr;
    std::size_t traceBytes = 0;
    void* map = nullptr;
    aclrtBinHandle binary = nullptr;
    bool streamDrained = true;
    std::unique_ptr<std::vector<char>> image;
};

bool AllocateTrace(
    const RuntimeApi& runtime, NpubitInstrumentationType type, std::size_t reported, DeviceRound& round,
    std::string& error)
{
    const auto defaultTraceMiB =
        (type == NPUBIT_SIMT_BANK_CONFLICTS || type == NPUBIT_PUSH_PB_STATE) ? kLargeTraceMiB : kDefaultTraceMiB;
    round.traceBytes = SourceCounterBufferSize(reported, defaultTraceMiB);
    if (round.traceBytes == 0 ||
        runtime.allocate(&round.trace, round.traceBytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS ||
        round.trace == nullptr ||
        runtime.setMemory(round.trace, round.traceBytes, 0, round.traceBytes) != ACL_SUCCESS) {
        error = "cannot allocate or initialize SourceCounter trace";
        return false;
    }
    return true;
}

bool InstrumentRound(
    const NpubitApi& sdk, NpubitHandle handle, NpubitInstrumentationType type, DeviceRound& round, std::string& error)
{
    SdkBuffer hostMap{sdk.free};
    if (!SdkSucceeded(
            sdk.allocateMap(handle, &hostMap.data, &hostMap.bytes), "NpubitAllocateBasicBlockBuffer", error)) {
        return false;
    }
    std::string args;
    if (hostMap.bytes != 0) {
        if (hostMap.data == nullptr ||
            round.runtime.allocate(&round.map, hostMap.bytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS ||
            round.map == nullptr) {
            error = "cannot allocate SourceCounter basic-block map";
            return false;
        }
        args = "bb-map-address:" + std::to_string(reinterpret_cast<std::uintptr_t>(round.map)) +
               ",bb-map-section-size:" + std::to_string(hostMap.bytes);
    }
    std::size_t reported = 0;
    if (!SdkSucceeded(sdk.getBufferSize(handle, type, &reported), "NpubitGetBufferSize", error)) {
        return false;
    }
    if (!AllocateTrace(round.runtime, type, reported, round, error)) {
        return false;
    }
    if (!SdkSucceeded(
            sdk.instrument(handle, type, round.trace, round.traceBytes, args.empty() ? nullptr : args.c_str()),
            "NpubitInstrument", error)) {
        return false;
    }
    if (hostMap.bytes != 0) {
        if (!SdkSucceeded(
                sdk.initializeMap(handle, hostMap.data, hostMap.bytes), "NpubitInitializeBasicBlockBufferMap", error)) {
            return false;
        }
        if (round.runtime.copy(round.map, hostMap.bytes, hostMap.data, hostMap.bytes, ACL_MEMCPY_HOST_TO_DEVICE) !=
            ACL_SUCCESS) {
            error = "cannot upload SourceCounter basic-block map";
            return false;
        }
    }
    return true;
}

aclrtFuncHandle LoadRound(
    const NpubitApi& sdk, NpubitHandle handle, const BinarySource& source, const std::string& symbol,
    DeviceRound& round, std::string& error)
{
    std::size_t bytes = 0;
    if (!SdkSucceeded(sdk.getObjectSize(handle, NPUBIT_OBJECT_INSTRUMENTED, &bytes), "NpubitGetObjectSize", error)) {
        return nullptr;
    }
    if (bytes == 0) {
        error = "NPUbit returned an empty companion ELF";
        return nullptr;
    }
    round.image = std::make_unique<std::vector<char>>(bytes);
    if (!SdkSucceeded(
            sdk.getObject(handle, NPUBIT_OBJECT_INSTRUMENTED, round.image->data(), bytes), "NpubitGetObject", error)) {
        return nullptr;
    }
    auto options = source.options;
    aclrtBinaryLoadOptions loadOptions{options.data(), options.size()};
    aclrtFuncHandle function = nullptr;
    if (round.runtime.load(round.image->data(), bytes, &loadOptions, &round.binary) != ACL_SUCCESS ||
        round.binary == nullptr) {
        error = "cannot load SourceCounter companion ELF";
        return nullptr;
    }
    if (!source.name.empty() && source.name != symbol &&
        round.runtime.getFunction(round.binary, source.name.c_str(), &function) == ACL_SUCCESS && function != nullptr) {
        return function;
    }
    function = nullptr;
    if (round.runtime.getFunction(round.binary, symbol.c_str(), &function) != ACL_SUCCESS || function == nullptr) {
        error = "cannot resolve SourceCounter companion function " + symbol;
        return nullptr;
    }
    return function;
}

bool ReadRound(
    const NpubitApi& sdk, NpubitHandle handle, NpubitInstrumentationType type, const DeviceRound& round,
    SdkBuffer& processed, SdkBuffer& analyzed, std::string& error)
{
    std::vector<std::uint8_t> storage(round.traceBytes);
    // The copy fills the host buffer before the SDK inspects it.
    if (round.runtime.copy(storage.data(), storage.size(), round.trace, round.traceBytes, ACL_MEMCPY_DEVICE_TO_HOST) !=
        ACL_SUCCESS) {
        error = "cannot read SourceCounter trace";
        return false;
    }
    if (!SdkSucceeded(
            sdk.process(handle, type, storage.data(), storage.size(), &processed.data, &processed.bytes),
            "NpubitProcess", error)) {
        return false;
    }
    if (!ValidProcessedRecords(type, processed)) {
        error = "NPUbit returned an invalid SourceCounter sample buffer";
        return false;
    }
    ASCTOOL_DEBUG(
        "[aclpti] SourceCounter type=%d traceBytes=%zu processedBytes=%zu", type, storage.size(), processed.bytes);
    if (processed.bytes == 0) {
        return true;
    }
    return SdkSucceeded(
        sdk.analyze(handle, type, processed.data, processed.bytes, nullptr, 0, &analyzed.data, &analyzed.bytes),
        "NpubitAnalyze", error);
}

bool AppendSimtGm(
    const SdkBuffer& processed, const SdkBuffer& analyzed, const SourceCounterInstructions& instructions,
    aclptiSourceCounterData& data, std::string& error)
{
    if (!analyzed.HasRecordsOf<SimtMemoryAccessAnalyzeData>()) {
        error = "NPUbit returned an invalid SIMT GM result buffer";
        return false;
    }
    // NPUbit reports a mean per core, subcore and original PC across that group's warp samples.
    using SampleGroup = std::tuple<std::uint8_t, int, std::uint64_t>;
    std::map<SampleGroup, std::uint64_t> sampleCounts;
    const auto* samples = static_cast<const NpubitSimtGmPatternsRecord*>(processed.data);
    const auto sampleCount = processed.bytes / sizeof(NpubitSimtGmPatternsRecord);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const auto& sample = samples[index];
        ++sampleCounts[{sample.aicore_id, sample.subcore_type, sample.instr_address}];
    }
    const auto* records = static_cast<const SimtMemoryAccessAnalyzeData*>(analyzed.data);
    const auto count = analyzed.bytes / sizeof(SimtMemoryAccessAnalyzeData);
    const auto firstRow = data.rows.size();
    constexpr double maxPercent = 100.0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = records[index];
        const auto address = record.instruction_address;
        const auto samplesForGroup = sampleCounts.find({record.aicore_id, record.subcore_id, address});
        if (instructions.simtGm.count(address) == 0 || samplesForGroup == sampleCounts.end()) {
            data.rows.resize(firstRow);
            error = "SIMT GM result does not match an original global-memory instruction and sample group";
            return false;
        }
        const double lines = record.avg_cache_lines_num;
        const double utilization = record.avg_cache_line_utilization;
        if (!std::isfinite(lines) || lines < 0 || !(utilization >= 0 && utilization <= maxPercent)) {
            continue;
        }
        aclptiSourceCounterRow row;
        row.memorySampleCount = samplesForGroup->second;
        const auto weight = static_cast<double>(row.memorySampleCount);
        row.cacheLinesTotal = lines * weight;
        row.cacheLineUtilizationTotal = utilization * weight;
        data.rows.emplace_back(address, row);
    }
    return true;
}

bool AppendSimtBank(
    const SdkBuffer& processed, const SdkBuffer& analyzed, const SourceCounterInstructions& instructions,
    aclptiSourceCounterData& data, std::string& error)
{
    if (!analyzed.HasRecordsOf<SimtBankConflictsAnalyzeData>()) {
        error = "NPUbit returned an invalid SIMT bank result buffer";
        return false;
    }
    const auto* samples = static_cast<const NpubitSimtBankConflictsRecord*>(processed.data);
    const auto sampleCount = processed.bytes / sizeof(NpubitSimtBankConflictsRecord);
    std::set<std::uint64_t> observed;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        observed.insert(samples[index].instr_address);
    }
    const auto* records = static_cast<const SimtBankConflictsAnalyzeData*>(analyzed.data);
    const auto count = analyzed.bytes / sizeof(SimtBankConflictsAnalyzeData);
    const auto firstRow = data.rows.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = records[index];
        if (instructions.simtBank.count(record.instruction_address) == 0 ||
            observed.count(record.instruction_address) == 0) {
            data.rows.resize(firstRow);
            error = "SIMT bank result PC does not match an original shared-memory instruction";
            return false;
        }
        aclptiSourceCounterRow row;
        row.ubBankConflict = record.bank_conflicts;
        data.rows.emplace_back(record.instruction_address, row);
    }
    return true;
}

bool AppendSimdBank(const SdkBuffer& analyzed, aclptiSourceCounterData& data, std::string& error)
{
    if (!analyzed.HasRecordsOf<SimdPushPBStateAnalyzeData>()) {
        error = "NPUbit returned an invalid PUSH_PB result buffer";
        return false;
    }
    const auto* records = static_cast<const SimdPushPBStateAnalyzeData*>(analyzed.data);
    const auto count = analyzed.bytes / sizeof(SimdPushPBStateAnalyzeData);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = records[index];
        if (record.execution_count == 0 || record.bank_conflicts < 0) {
            continue;
        }
        aclptiSourceCounterRow row;
        row.ubBankConflict = static_cast<std::uint64_t>(record.bank_conflicts);
        data.rows.emplace_back(record.instruction_address, row);
    }
    return true;
}

struct CollectionContext {
    const NpubitApi& sdk;
    const RuntimeApi& runtime;
    const SourceDevice& device;
    const SourceCounterReplay& replay;
    const BinarySource& source;
    const SourceCounterInstructions& instructions;
};

bool AppendRound(
    NpubitInstrumentationType type, const SdkBuffer& processed, const SdkBuffer& analyzed,
    const SourceCounterInstructions& instructions, aclptiSourceCounterData& data, std::string& error)
{
    switch (type) {
        case NPUBIT_SIMT_GLOBAL_MEMORY_PATTERNS:
            return AppendSimtGm(processed, analyzed, instructions, data, error);
        case NPUBIT_SIMT_BANK_CONFLICTS:
            return AppendSimtBank(processed, analyzed, instructions, data, error);
        case NPUBIT_PUSH_PB_STATE:
            return AppendSimdBank(analyzed, data, error);
        default:
            return false;
    }
}

aclptiResult CollectRound(
    const CollectionContext& context, NpubitInstrumentationType type, std::size_t replayIndex,
    aclptiSourceCounterData& data, std::string& error)
{
    const auto& sdk = context.sdk;
    const auto& source = context.source;
    const auto& instructions = context.instructions;
    const auto& topology = context.device.topology;
    const NpubitHwContext hardwareContext{topology.dieCount, topology.coresPerDie};
    SdkHandle handle{sdk.release};
    if (!SdkSucceeded(
            sdk.create(source.image->data(), source.image->size(), &handle.value, context.device.platform, true),
            "NpubitCreateHandle", error) ||
        !SdkSucceeded(sdk.setHardware(handle.value, &hardwareContext), "NpubitSetHardwareContext", error)) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    DeviceRound round(context.runtime);
    if (!InstrumentRound(sdk, handle.value, type, round, error)) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const auto function = LoadRound(sdk, handle.value, source, instructions.launchSymbol, round, error);
    if (function == nullptr) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const auto replayed = context.replay(function, replayIndex);
    round.streamDrained = replayed.streamDrained;
    if (replayed.status != ACLPTI_SUCCESS) {
        error = "SourceCounter replay failed: status=" + std::to_string(replayed.status);
        return replayed.status;
    }
    SdkBuffer processed{sdk.free};
    SdkBuffer analyzed{sdk.free};
    if (!ReadRound(sdk, handle.value, type, round, processed, analyzed, error)) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    ASCTOOL_DEBUG("[aclpti] SourceCounter type=%d analyzedBytes=%zu", type, analyzed.bytes);
    return AppendRound(type, processed, analyzed, instructions, data, error) ? ACLPTI_SUCCESS :
                                                                               ACLPTI_ERROR_PROFILING_FAILED;
}

aclptiResult CollectRounds(const CollectionContext& context, aclptiSourceCounterData& data)
{
    const auto& instructions = context.instructions;
    struct Round {
        NpubitInstrumentationType type;
        bool enabled;
        const char* name;
    };
    const Round rounds[] = {
        {NPUBIT_SIMT_GLOBAL_MEMORY_PATTERNS, !instructions.simtGm.empty(), "SIMT GM Pattern"},
        {NPUBIT_SIMT_BANK_CONFLICTS, !instructions.simtBank.empty(), "SIMT Bank Conflict"},
        {NPUBIT_PUSH_PB_STATE, instructions.simdVf, "SIMD VF PUSH_PB"},
    };
    auto collectionStatus = ACLPTI_SUCCESS;
    std::size_t replayIndex = 0;
    for (const auto& round : rounds) {
        if (!round.enabled) {
            continue;
        }
        std::string error;
        const auto status = CollectRound(context, round.type, replayIndex, data, error);
        ++replayIndex;
        if (status == ACLPTI_SUCCESS) {
            continue;
        }
        data.errors.emplace_back(std::string(round.name) + ": " + error);
        if (status == ACLPTI_ERROR_RESULT_UNRELIABLE) {
            data.status = status;
            return status;
        }
        if (collectionStatus == ACLPTI_SUCCESS) {
            collectionStatus = status;
        }
    }
    data.status =
        collectionStatus == ACLPTI_SUCCESS && !data.errors.empty() ? ACLPTI_ERROR_NOT_SUPPORTED : collectionStatus;
    return ACLPTI_SUCCESS;
}
} // namespace
#endif

aclptiResult CollectSourceCounterData(
    const BinarySource& source, std::int32_t deviceId, const SourceCounterReplay& replay, aclptiSourceCounterData& data)
{
    if (!source.image) {
        data.errors.emplace_back("original ELF/function unavailable; enable SourceCounter before binary load");
        return ACLPTI_SUCCESS;
    }
#if defined(NPUCOMPUTE_NPUBIT_SONAME)
    NpubitApi sdk;
    std::string error;
    if (!sdk.Open(error)) {
        data.errors.push_back(std::move(error));
        return ACLPTI_SUCCESS;
    }
    SourceCounterInstructions instructions;
    std::string disassembly;
    if (!ReadOriginalDisassembly(
            source.image->data(), source.image->size(), disassembly, error, DisassemblyDetail::WithSource) ||
        !BuildSourceCounterInstructions(*source.image, disassembly, source.name, source.entry, instructions, error)) {
        data.errors.push_back(std::move(error));
        return ACLPTI_SUCCESS;
    }
    data.originalDisassembly = std::move(disassembly);
    if (instructions.incomplete) {
        data.errors.emplace_back("not all VF targets could be classified for SourceCounter");
    }
    if (instructions.simtGm.empty() && instructions.simtBank.empty() && !instructions.simdVf) {
        data.status = data.errors.empty() ? ACLPTI_SUCCESS : ACLPTI_ERROR_NOT_SUPPORTED;
        return ACLPTI_SUCCESS;
    }
    const RuntimeApi runtime;
    SourceDevice device;
    if (!runtime.Ready()) {
        data.errors.emplace_back("original Runtime APIs for SourceCounter are unavailable");
        return ACLPTI_SUCCESS;
    }
    if (!ReadSourceDevice(deviceId, device, error)) {
        data.errors.push_back(std::move(error));
        return ACLPTI_SUCCESS;
    }
    return CollectRounds({sdk, runtime, device, replay, source, instructions}, data);
#else
    (void)deviceId;
    (void)replay;
    data.errors.emplace_back("CANN NPUbit SDK unavailable for SourceCounter");
    return ACLPTI_SUCCESS;
#endif
}

} // namespace aclpti::profiling
