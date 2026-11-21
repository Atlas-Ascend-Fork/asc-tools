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
#include "kernel_instrumenter.h"
#include "injection/injection_hook.h"
#include "npu_compute_log.h"
#include "npubit_device.h"

#include <npubit.h>

#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace aclpti::profiling {
namespace {
constexpr std::size_t kBytesPerMiB = 1024U * 1024U;
} // namespace

std::size_t SourceCounterBufferSize(std::size_t reported, std::size_t defaultTraceMiB)
{
    return reported == std::numeric_limits<std::size_t>::max() ? defaultTraceMiB * kBytesPerMiB : reported;
}

namespace {
constexpr std::size_t kDefaultTraceMiB = 4;
constexpr std::size_t kLargeTraceMiB = 64;

template <typename Function>
Function Original(aclrtApiId id)
{
    return reinterpret_cast<Function>(acltoolGetOriginalRuntimeApi(id));
}

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

bool SdkSucceeded(NpubitStatus status, const char* operation, std::string& error)
{
    if (status == NPUBIT_SUCCESS) {
        return true;
    }
    error = std::string(operation) + " failed: status=" + std::to_string(status);
    return false;
}

struct SdkHandle {
    NpubitHandle value = nullptr;
    ~SdkHandle()
    {
        if (value != nullptr) {
            (void)NpubitReleaseHandle(value);
        }
    }
};

struct SdkBuffer {
    void* data = nullptr;
    std::size_t bytes = 0;
    ~SdkBuffer()
    {
        if (data != nullptr) {
            (void)NpubitFree(&data);
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
        case NPUBIT_SCALAR_BB_COUNT:
            return processed.HasRecordsOf<NpubitScalarBBCountRecord>();
        case NPUBIT_SIMT_BB_COUNT:
            return processed.HasRecordsOf<NpubitSimtBBCountRecord>();
        case NPUBIT_SIMT_THREAD_EXE_COUNT:
            return processed.HasRecordsOf<NpubitSimtThreadExeCountRecord>();
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
        bool canRelease = streamDrained;
        if (streamDrained && binary != nullptr) {
            canRelease = runtime.unload(binary) == ACL_SUCCESS;
        }
        if (!canRelease) {
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
    if (round.traceBytes == 0) {
        error = "invalid SourceCounter trace size";
        return false;
    }
    const auto allocateStatus = runtime.allocate(&round.trace, round.traceBytes, ACL_MEM_MALLOC_HUGE_FIRST);
    if (allocateStatus != ACL_SUCCESS || round.trace == nullptr) {
        error = "cannot allocate SourceCounter trace";
        return false;
    }
    if (runtime.setMemory(round.trace, round.traceBytes, 0, round.traceBytes) != ACL_SUCCESS) {
        error = "cannot initialize SourceCounter trace";
        return false;
    }
    return true;
}

bool InstrumentRound(NpubitHandle handle, NpubitInstrumentationType type, DeviceRound& round, std::string& error)
{
    SdkBuffer hostMap{};
    if (!SdkSucceeded(
            NpubitAllocateBasicBlockBuffer(handle, &hostMap.data, &hostMap.bytes), "NpubitAllocateBasicBlockBuffer",
            error)) {
        return false;
    }
    std::string args;
    if (hostMap.bytes != 0) {
        if (hostMap.data == nullptr) {
            error = "missing SourceCounter host basic-block map";
            return false;
        }
        const auto allocateStatus = round.runtime.allocate(&round.map, hostMap.bytes, ACL_MEM_MALLOC_HUGE_FIRST);
        if (allocateStatus != ACL_SUCCESS || round.map == nullptr) {
            error = "cannot allocate SourceCounter basic-block map";
            return false;
        }
        args = "bb-map-address:" + std::to_string(reinterpret_cast<std::uintptr_t>(round.map)) +
               ",bb-map-section-size:" + std::to_string(hostMap.bytes);
    }
    std::size_t reported = 0;
    if (!SdkSucceeded(NpubitGetBufferSize(handle, type, &reported), "NpubitGetBufferSize", error)) {
        return false;
    }
    if (!AllocateTrace(round.runtime, type, reported, round, error)) {
        return false;
    }
    if (!SdkSucceeded(
            NpubitInstrument(handle, type, round.trace, round.traceBytes, args.empty() ? nullptr : args.c_str()),
            "NpubitInstrument", error)) {
        return false;
    }
    if (hostMap.bytes != 0) {
        if (!SdkSucceeded(
                NpubitInitializeBasicBlockBufferMap(handle, hostMap.data, hostMap.bytes),
                "NpubitInitializeBasicBlockBufferMap", error)) {
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
    NpubitHandle handle, const BinarySource& source, const std::string& symbol, DeviceRound& round, std::string& error)
{
    std::size_t bytes = 0;
    if (!SdkSucceeded(NpubitGetObjectSize(handle, NPUBIT_OBJECT_INSTRUMENTED, &bytes), "NpubitGetObjectSize", error)) {
        return nullptr;
    }
    if (bytes == 0) {
        error = "NPUbit returned an empty companion ELF";
        return nullptr;
    }
    round.image = std::make_unique<std::vector<char>>(bytes);
    if (!SdkSucceeded(
            NpubitGetObject(handle, NPUBIT_OBJECT_INSTRUMENTED, round.image->data(), bytes), "NpubitGetObject",
            error)) {
        return nullptr;
    }
    auto options = source.options;
    aclrtBinaryLoadOptions loadOptions{options.data(), options.size()};
    aclrtFuncHandle function = nullptr;
    const auto loadStatus = round.runtime.load(round.image->data(), bytes, &loadOptions, &round.binary);
    if (loadStatus != ACL_SUCCESS || round.binary == nullptr) {
        error = "cannot load SourceCounter companion ELF";
        return nullptr;
    }
    if (!source.name.empty() && source.name != symbol) {
        const auto lookupStatus = round.runtime.getFunction(round.binary, source.name.c_str(), &function);
        if (lookupStatus == ACL_SUCCESS && function != nullptr) {
            return function;
        }
    }
    function = nullptr;
    const auto lookupStatus = round.runtime.getFunction(round.binary, symbol.c_str(), &function);
    if (lookupStatus != ACL_SUCCESS || function == nullptr) {
        error = "cannot resolve SourceCounter companion function " + symbol;
        return nullptr;
    }
    return function;
}

bool ReadRound(
    NpubitHandle handle, NpubitInstrumentationType type, const DeviceRound& round, SdkBuffer& processed,
    SdkBuffer& analyzed, std::string& error)
{
    std::vector<std::uint8_t> storage(round.traceBytes);
    // The copy fills the host buffer before the SDK inspects it.
    if (round.runtime.copy(storage.data(), storage.size(), round.trace, round.traceBytes, ACL_MEMCPY_DEVICE_TO_HOST) !=
        ACL_SUCCESS) {
        error = "cannot read SourceCounter trace";
        return false;
    }
    if (!SdkSucceeded(
            NpubitProcess(handle, type, storage.data(), storage.size(), &processed.data, &processed.bytes),
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
        NpubitAnalyze(handle, type, processed.data, processed.bytes, nullptr, 0, &analyzed.data, &analyzed.bytes),
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

template <typename Record>
bool AppendCounts(
    const SdkBuffer& analyzed, std::optional<uint64_t> aclptiSourceCounterRow::*field, aclptiSourceCounterData& data,
    std::string& error)
{
    if (!analyzed.HasRecordsOf<Record>()) {
        error = "NPUbit returned an invalid SourceCounter count buffer";
        return false;
    }
    const auto* records = static_cast<const Record*>(analyzed.data);
    const auto count = analyzed.bytes / sizeof(Record);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = records[index];
        aclptiSourceCounterRow row;
        row.*field = record.execution_count;
        data.rows.emplace_back(record.instruction_address, row);
    }
    return true;
}

struct CollectionContext {
    const RuntimeApi& runtime;
    const NpubitDevice& device;
    const SourceCounterReplay& replay;
    const BinarySource& source;
    const SourceCounterInstructions& instructions;
};

bool AppendRound(
    NpubitInstrumentationType type, const SdkBuffer& processed, const SdkBuffer& analyzed,
    const SourceCounterInstructions& instructions, aclptiSourceCounterData& data, std::string& error)
{
    switch (type) {
        case NPUBIT_SCALAR_BB_COUNT:
            return AppendCounts<ScalarInstructionCountAnalyzeData>(
                analyzed, &aclptiSourceCounterRow::instructionsExecuted, data, error);
        case NPUBIT_SIMT_BB_COUNT:
            return AppendCounts<SimtInstructionCountAnalyzeData>(
                analyzed, &aclptiSourceCounterRow::instructionsExecuted, data, error);
        case NPUBIT_SIMT_THREAD_EXE_COUNT:
            return AppendCounts<SimtThreadExecutionCountAnalyzeData>(
                analyzed, &aclptiSourceCounterRow::activeThreads, data, error);
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
    const auto& source = context.source;
    const auto& instructions = context.instructions;
    const auto& topology = context.device.topology;
    const NpubitHwContext hardwareContext{topology.dieCount, topology.coresPerDie};
    SdkHandle handle{};
    if (!SdkSucceeded(
            NpubitCreateHandle(
                source.image->data(), source.image->size(), &handle.value, context.device.platform, true),
            "NpubitCreateHandle", error)) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    if (!SdkSucceeded(NpubitSetHardwareContext(handle.value, &hardwareContext), "NpubitSetHardwareContext", error)) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    DeviceRound round(context.runtime);
    if (!InstrumentRound(handle.value, type, round, error)) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const auto function = LoadRound(handle.value, source, instructions.launchSymbol, round, error);
    if (function == nullptr) {
        return ACLPTI_ERROR_PROFILING_FAILED;
    }
    const auto replayed = context.replay(function, replayIndex);
    round.streamDrained = replayed.streamDrained;
    if (replayed.status != ACLPTI_SUCCESS) {
        error = "SourceCounter replay failed: status=" + std::to_string(replayed.status);
        return replayed.status;
    }
    SdkBuffer processed{};
    SdkBuffer analyzed{};
    if (!ReadRound(handle.value, type, round, processed, analyzed, error)) {
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
        {NPUBIT_SCALAR_BB_COUNT, true, "Scalar BB Count"},
        {NPUBIT_SIMT_BB_COUNT, instructions.simtVf, "SIMT BB Count"},
        {NPUBIT_SIMT_THREAD_EXE_COUNT, instructions.simtVf, "SIMT Thread Execution Count"},
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

aclptiResult CollectSourceCounterData(
    const BinarySource& source, std::int32_t deviceId, const SourceCounterReplay& replay, aclptiSourceCounterData& data)
{
    if (!source.image) {
        data.errors.emplace_back("original ELF/function unavailable; enable SourceCounter before binary load");
        return ACLPTI_SUCCESS;
    }
    std::string error;
    SourceCounterInstructions instructions;
    std::string disassembly;
    if (!ReadOriginalDisassembly(
            source.image->data(), source.image->size(), disassembly, error, DisassemblyDetail::WithSource)) {
        data.errors.push_back(std::move(error));
        return ACLPTI_SUCCESS;
    }
    if (!BuildSourceCounterInstructions(*source.image, disassembly, source.name, source.entry, instructions, error)) {
        data.errors.push_back(std::move(error));
        return ACLPTI_SUCCESS;
    }
    data.originalDisassembly = std::move(disassembly);
    if (instructions.incomplete) {
        data.errors.emplace_back("not all VF targets could be classified for SourceCounter");
    }
    const RuntimeApi runtime;
    if (!runtime.Ready()) {
        data.errors.emplace_back("original Runtime APIs for SourceCounter are unavailable");
        return ACLPTI_SUCCESS;
    }
    NpubitDevice device;
    if (!ReadNpubitDevice(deviceId, device, error)) {
        data.errors.push_back(std::move(error));
        return ACLPTI_SUCCESS;
    }
    return CollectRounds({runtime, device, replay, source, instructions}, data);
}

} // namespace aclpti::profiling
