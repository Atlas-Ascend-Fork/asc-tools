/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include "npubit_collector.h"
#include "npu_compute_log.h"
#include <array>
#include <boost/filesystem.hpp>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <unistd.h>

#include <driver/ascend_hal_base.h>
#include <npubit.h>

namespace aclpti::profiling {

namespace {

constexpr std::uint32_t kB8BitWidth = 8U;
constexpr std::uint32_t kB16BitWidth = 16U;
constexpr std::uint32_t kB32BitWidth = 32U;
constexpr std::uint64_t kSingleInstructionCount = 1U;
constexpr double kFmaOperations = 2.0;
constexpr double kBitsPerByte = 8.0;
constexpr std::size_t kObjdumpLineBufferSize = 4096U;
} // namespace

bool ReadNpubitTopology(std::int32_t deviceId, AiCoreTopology& topology)
{
    if (deviceId < 0) {
        return false;
    }
    const auto close = [](void* library) { (void)dlclose(library); };
    const std::unique_ptr<void, decltype(close)> hal(dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL), close);
    if (!hal) {
        return false;
    }
    const auto getInfo = reinterpret_cast<decltype(&halGetDeviceInfo)>(dlsym(hal.get(), "halGetDeviceInfo"));
    const auto id = static_cast<std::uint32_t>(deviceId);
    std::int64_t dies = 0;
    std::int64_t cores = 0;
    if (!getInfo || getInfo(id, MODULE_TYPE_AICORE, INFO_TYPE_DIE_NUM, &dies) != DRV_ERROR_NONE ||
        getInfo(id, MODULE_TYPE_AICORE, INFO_TYPE_CORE_NUM, &cores) != DRV_ERROR_NONE || dies <= 0 || cores <= 0 ||
        dies > std::numeric_limits<std::uint32_t>::max() || cores % dies != 0 ||
        cores / dies > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    topology = {static_cast<std::uint32_t>(dies), static_cast<std::uint32_t>(cores / dies)};
    return true;
}

std::optional<double> RooflineVectorOperations(std::uint32_t bitWidth, std::uint64_t instructionCount)
{
    constexpr double kB8Operations = 256.0 * kFmaOperations;
    constexpr double kB16Operations = 128.0 * kFmaOperations;
    constexpr double kB32Operations = 64.0 * kFmaOperations;
    switch (bitWidth) {
        case kB8BitWidth:
            return instructionCount * kB8Operations;
        case kB16BitWidth:
            return instructionCount * kB16Operations;
        case kB32BitWidth:
            return instructionCount * kB32Operations;
        default:
            return std::nullopt;
    }
}

namespace {

constexpr std::size_t kDefaultTraceBytes = 4U * 1024U * 1024U;

struct NpubitOutput {
    void* data = nullptr;
    std::size_t size = 0;
    ~NpubitOutput()
    {
        if (data != nullptr) {
            NpubitFree(&data);
        }
    }
};

// Only original Runtime functions are used: tool buffers/images must not enter replay mirroring/hooks.
struct NpubitReplay {
    void* trace = nullptr;
    std::size_t traceBytes = 0;
    std::unique_ptr<std::vector<char>> image = std::make_unique<std::vector<char>>();
    aclrtBinHandle binary = nullptr;
    aclrtFuncHandle function = nullptr;

    // Indirect-jump basic-block map: host buffer owned by npubit (NpubitFree after upload),
    // device buffer owned by us (aclrtFree after the pass). Both are null/0 when the object
    // has no indirect jumps.
    void* bbMapHost = nullptr;
    std::size_t bbMapBytes = 0;
    void* bbMapDevice = nullptr;

    ~NpubitReplay()
    {
        if (binary != nullptr) {
            const auto unload =
                reinterpret_cast<aclrtBinaryUnLoadFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryUnLoad));
            if (unload == nullptr || unload(binary) != ACL_SUCCESS) {
                // Runtime may retain this image until unload succeeds.
                (void)image.release();
                ASCTOOL_WARNING("[aclpti] NPUBIT companion unload failed; retaining image until process exit");
            }
        }
        if (trace != nullptr) {
            const auto free = reinterpret_cast<aclrtFreeFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFree));
            if (free != nullptr && free(trace) != ACL_SUCCESS) {
                ASCTOOL_WARNING("[aclpti] NPUBIT trace buffer release failed");
            }
        }
        if (bbMapDevice != nullptr) {
            const auto free = reinterpret_cast<aclrtFreeFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtFree));
            if (free != nullptr && free(bbMapDevice) != ACL_SUCCESS) {
                ASCTOOL_WARNING("[aclpti] NPUBIT BB-map device buffer release failed");
            }
        }
        if (bbMapHost != nullptr) {
            NpubitFree(&bbMapHost);
        }
    }
};

bool CheckNpubit(int status, const char* api, std::string& error)
{
    if (status == NPUBIT_SUCCESS) {
        return true;
    }
    error = std::string(api) + " failed: status=" + std::to_string(status);
    return false;
}

// Allocate the host basic-block map (only non-empty for objects with indirect jumps) and,
// when required, a device map whose address is passed to NpubitInstrument via args. The
// map is filled and uploaded in InitializeBasicBlockMap AFTER NpubitInstrument.
bool PrepareBasicBlocks(NpubitHandle handle, NpubitReplay& replay, std::string& args, std::string& error)
{
    if (!CheckNpubit(
            NpubitAllocateBasicBlockBuffer(handle, &replay.bbMapHost, &replay.bbMapBytes),
            "NpubitAllocateBasicBlockBuffer", error)) {
        return false;
    }
    if (replay.bbMapBytes == 0) {
        replay.bbMapHost = nullptr;
        return true; // no indirect jumps: no Device map, no instrumentation args
    }
    if (replay.bbMapHost == nullptr) {
        error = "npubit BB-map allocation returned non-zero size with null host buffer";
        return false;
    }
    const auto allocate = reinterpret_cast<aclrtMallocFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMalloc));
    if (!allocate || allocate(&replay.bbMapDevice, replay.bbMapBytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS ||
        replay.bbMapDevice == nullptr) {
        error = "cannot allocate npubit BB-map Device buffer";
        return false;
    }
    args = "bb-map-address:" + std::to_string(reinterpret_cast<std::uintptr_t>(replay.bbMapDevice)) +
           ",bb-map-section-size:" + std::to_string(replay.bbMapBytes);
    return true;
}

// Fill the host map (original<->instrumented blocks) after NpubitInstrument, then upload it
// to the device map whose address was baked into the object during NpubitInstrument.
bool InitializeBasicBlockMap(NpubitHandle handle, NpubitReplay& replay, std::string& error)
{
    if (replay.bbMapBytes == 0) {
        return true; // no indirect jumps: no op
    }
    if (!CheckNpubit(
            NpubitInitializeBasicBlockBufferMap(handle, replay.bbMapHost, replay.bbMapBytes),
            "NpubitInitializeBasicBlockBufferMap", error)) {
        return false;
    }
    const auto copy = reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
    if (!copy ||
        copy(replay.bbMapDevice, replay.bbMapBytes, replay.bbMapHost, replay.bbMapBytes, ACL_MEMCPY_HOST_TO_DEVICE) !=
            ACL_SUCCESS) {
        error = "cannot upload npubit BB map to Device";
        return false;
    }
    if (replay.bbMapHost != nullptr) {
        NpubitFree(&replay.bbMapHost);
        replay.bbMapHost = nullptr;
    }
    return true;
}

bool ExtractObject(NpubitHandle handle, std::vector<char>& image, std::string& error)
{
    std::size_t objectSize = 0;
    if (!CheckNpubit(
            NpubitGetObjectSize(handle, NPUBIT_OBJECT_INSTRUMENTED, &objectSize), "NpubitGetObjectSize", error) ||
        objectSize == 0) {
        error = "npubit instrumented object is empty";
        return false;
    }
    image.resize(objectSize);
    if (!CheckNpubit(
            NpubitGetObject(handle, NPUBIT_OBJECT_INSTRUMENTED, image.data(), objectSize), "NpubitGetObject", error)) {
        image.clear();
        return false;
    }
    return true;
}

// Seed the device trace buffer so every per-core 'written_bytes' header starts at zero before
// the instrumented kernel runs; npubit owns the rest of the protocol layout.
bool InitializeTrace(NpubitReplay& replay, std::string& error)
{
    if (replay.trace == nullptr || replay.traceBytes == 0) {
        error = "npubit trace buffer is not allocated";
        return false;
    }
    const auto memsetFunction = reinterpret_cast<aclrtMemsetFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemset));
    if (!memsetFunction || memsetFunction(replay.trace, replay.traceBytes, 0, replay.traceBytes) != ACL_SUCCESS) {
        error = "cannot initialize npubit trace buffer";
        return false;
    }
    return true;
}

bool ValidateTrace(const std::vector<std::uint8_t>& raw, std::string& error)
{
    if (raw.empty()) {
        error = "npubit trace buffer is empty";
        return false;
    }
    // Per-core slices start with a uint64_t 'written_bytes' header; npubit's NpubitProcess
    // parses the layout and reports truncation/overflow itself.
    return true;
}

bool LoadFunction(const BinarySource& source, NpubitReplay& replay, std::string& error)
{
    if (source.name.empty()) {
        error = "original kernel name unavailable for npubit companion";
        return false;
    }
    const auto load =
        reinterpret_cast<aclrtBinaryLoadFromDataFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryLoadFromData));
    const auto get =
        reinterpret_cast<aclrtBinaryGetFunctionFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtBinaryGetFunction));
    auto options = source.options;
    aclrtBinaryLoadOptions loadOptions{options.data(), options.size()};
    if (!load || !get ||
        load(replay.image->data(), replay.image->size(), &loadOptions, &replay.binary) != ACL_SUCCESS ||
        get(replay.binary, source.name.c_str(), &replay.function) != ACL_SUCCESS) {
        error = "cannot load/resolve npubit companion";
        return false;
    }
    return true;
}

bool AnalyzeTrace(
    NpubitHandle handle, NpubitInstrumentationType type, const std::vector<std::uint8_t>& raw, NpubitOutput& analysis,
    std::string& error)
{
    if (!ValidateTrace(raw, error)) {
        return false;
    }
    NpubitOutput records;
    return CheckNpubit(
               NpubitProcess(handle, type, raw.data(), raw.size(), &records.data, &records.size), "NpubitProcess",
               error) &&
           CheckNpubit(
               NpubitAnalyze(handle, type, records.data, records.size, nullptr, 0, &analysis.data, &analysis.size),
               "NpubitAnalyze", error);
}

// Only GM loads/stores contribute to the SIMT GM q; shared/constant accesses
// (LD/STS/LDK/STK) are intentionally excluded.
bool IsSimtGlobalMemoryAccess(NpubitInstructionName name)
{
    switch (name) {
        case NPUBIT_INSTRUCTION_NAME_SIMT_LDG:
        case NPUBIT_INSTRUCTION_NAME_SIMT_STG:
            return true;
        default:
            return false;
    }
}

// Map a npubit data type to the roofline bit width (b8/b16/b32). Newer npubit
// exposes data_type on SIMD push-PB records and a/b_data_type on MMAD records.
std::optional<std::uint32_t> NpubitDataTypeToBitWidth(NpubitDataType type)
{
    switch (type) {
        case NPUBIT_DATA_TYPE_S8:
        case NPUBIT_DATA_TYPE_U8:
        case NPUBIT_DATA_TYPE_F8:
        case NPUBIT_DATA_TYPE_HIF8:
        case NPUBIT_DATA_TYPE_MX_F8:
        case NPUBIT_DATA_TYPE_B8:
            return kB8BitWidth;
        case NPUBIT_DATA_TYPE_S16:
        case NPUBIT_DATA_TYPE_U16:
        case NPUBIT_DATA_TYPE_F16:
        case NPUBIT_DATA_TYPE_BF16:
        case NPUBIT_DATA_TYPE_B16:
        case NPUBIT_DATA_TYPE_F8_X2:
        case NPUBIT_DATA_TYPE_HIF8_X2:
            return kB16BitWidth;
        case NPUBIT_DATA_TYPE_S32:
        case NPUBIT_DATA_TYPE_U32:
        case NPUBIT_DATA_TYPE_F32:
        case NPUBIT_DATA_TYPE_TF32:
        case NPUBIT_DATA_TYPE_B32:
        case NPUBIT_DATA_TYPE_F16_X2:
        case NPUBIT_DATA_TYPE_BF16_X2:
            return kB32BitWidth;
        default:
            return std::nullopt;
    }
}

bool IsComputeInstruction(NpubitInstructionName name)
{
    switch (name) {
        case NPUBIT_INSTRUCTION_NAME_SIMD_VADD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSUB:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMUL:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMULL:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMULA:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMADD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_WMULA:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VAXPY:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VDIV:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VDIVF:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSQRT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VREC:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VRSQRT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMOD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VEXP:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VLN:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VEXP2:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VEXPDIF:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMAX:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMIN:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VABS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VABS_F:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VNEG:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VNEG_F:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMAXRELU:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMINRELU:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VRELU:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VLRELU:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VPRELU:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VAVG:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VADIF:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSAD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VABSDIF:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSADD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSSUB:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VADD3:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VADDC:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VADDCS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSUBC:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSUBCS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VADDS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMAXS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMINS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSADDS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMULS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VRND:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VRNDI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VRNDS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SADD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SSUB:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SMUL:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SMULL:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SMULLI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SMULI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SADDI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SSUBI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SABS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SMAX:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SMIN:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VAND:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VOR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VXOR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SAND:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SOR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SXOR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SNOT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_PAND:
        case NPUBIT_INSTRUCTION_NAME_SIMD_POR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_PXOR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_PNOT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSHL:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSHR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSHLI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSHRI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSHLS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VSHRS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SSHL:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SSHR:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SSHLI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SSHRI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCMP:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCMPS:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SCMP:
        case NPUBIT_INSTRUCTION_NAME_SIMD_PLT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_PLTM:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCADD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCMAX:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCMIN:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCGADD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCGMAX:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCGMIN:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCPADD:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCBMAX:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCBMIN:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCVTFI:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCVTFF:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCVTIF:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VMULSCVT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SZEROEXT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_SSIGNEXT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VBCNT:
        case NPUBIT_INSTRUCTION_NAME_SIMD_VCLS:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IADD:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IADDX:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IADDI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IADDXI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IMAD:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IMADI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IMUL:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IMULI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IMNMX:
        case NPUBIT_INSTRUCTION_NAME_SIMT_IMNMXI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_CLZ:
        case NPUBIT_INSTRUCTION_NAME_SIMT_POPC:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FADD:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FADDI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FMUL:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FFMA:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FFMA_I:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FMULI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FMNMX:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FMNMXI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FDIV:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FDIVI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FEXPDIF:
        case NPUBIT_INSTRUCTION_NAME_SIMT_MUFU:
        case NPUBIT_INSTRUCTION_NAME_SIMT_LOP3:
        case NPUBIT_INSTRUCTION_NAME_SIMT_LOP3I:
        case NPUBIT_INSTRUCTION_NAME_SIMT_SHF:
        case NPUBIT_INSTRUCTION_NAME_SIMT_SHFI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_ISETP:
        case NPUBIT_INSTRUCTION_NAME_SIMT_ISETPI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FSETP:
        case NPUBIT_INSTRUCTION_NAME_SIMT_FSETPI:
        case NPUBIT_INSTRUCTION_NAME_SIMT_I2F:
        case NPUBIT_INSTRUCTION_NAME_SIMT_F2I:
        case NPUBIT_INSTRUCTION_NAME_SIMT_F2F:
        case NPUBIT_INSTRUCTION_NAME_SIMT_REDUX:
            return true;
        default:
            return false;
    }
}

// 返回该 PC 对应指令名（查不到 UNKNOWN）。
NpubitInstructionName InstructionNameAt(NpubitHandle handle, std::uint64_t pc)
{
    NpubitInstructionName name = NPUBIT_INSTRUCTION_NAME_UNKNOWN;
    (void)NpubitGetInstructionName(handle, pc, &name);
    return name;
}

void DecodeRoofline(
    NpubitHandle handle, NpubitInstrumentationType type, const NpubitOutput& analysis, aclptiRooflineData& data,
    std::map<std::uint64_t, NpubitDataType>* simtDtypeByAddr)
{
    // 按位宽输出：SIMD/Cube 用真实 dtype/维度分桶；SIMT 的位宽
    // 从 NPUBIT_SIMT_THREAD_EXE_COUNT 的 data_type 按原始 PC 关联得到。
    if (type == NPUBIT_PUSH_PB_STATE) {
        const auto* arr = static_cast<const SimdPushPBStateAnalyzeData*>(analysis.data);
        const std::size_t count = analysis.size / sizeof(SimdPushPBStateAnalyzeData);
        std::map<std::uint32_t, double> perBw;
        for (std::size_t i = 0; i < count; ++i) {
            // 只统计「计算指令」，排除 VLD/VST/寻址等搬运/控制指令。
            if (!IsComputeInstruction(InstructionNameAt(handle, arr[i].instruction_address))) {
                continue;
            }
            const auto bw = NpubitDataTypeToBitWidth(arr[i].data_type);
            if (!bw) {
                continue;
            }
            // 每条 SIMD 指令按其真实 dtype 的每指令计算量累加。
            const auto perInst = RooflineVectorOperations(*bw, kSingleInstructionCount);
            if (perInst) {
                perBw[*bw] += static_cast<double>(arr[i].execution_count) * *perInst;
            }
        }
        for (const auto& [bw, ops] : perBw) {
            if (ops > 0.0) {
                data.work.push_back(aclptiRooflineWork{aclptiRooflineChannel::Simd, bw, ops});
            }
        }
    } else if (type == NPUBIT_SIMT_BB_COUNT) {
        const auto* arr = static_cast<const SimtInstructionCountAnalyzeData*>(analysis.data);
        const std::size_t count = analysis.size / sizeof(SimtInstructionCountAnalyzeData);
        // 本 PASS 只带 warp 级执行计数（instruction_count），不带data_type；位宽从
        // NPUBIT_SIMT_THREAD_EXE_COUNT 按原始 PC 关联得到，
        // 与 SIMD 一样按 b8/b16/b32 分桶。
        std::map<std::uint32_t, double> perBw;
        for (std::size_t i = 0; i < count; ++i) {
            if (!IsComputeInstruction(InstructionNameAt(handle, arr[i].instruction_address))) {
                continue;
            }
            std::uint32_t bitWidth = kB32BitWidth;
            if (simtDtypeByAddr != nullptr) {
                const auto it = simtDtypeByAddr->find(arr[i].instruction_address);
                if (it != simtDtypeByAddr->end()) {
                    const auto bw = NpubitDataTypeToBitWidth(it->second);
                    if (bw) {
                        bitWidth = *bw;
                    }
                }
            }
            const auto perInst = RooflineVectorOperations(bitWidth, kSingleInstructionCount);
            if (perInst) {
                perBw[bitWidth] += static_cast<double>(arr[i].execution_count) * *perInst;
            }
        }
        for (const auto& [bw, ops] : perBw) {
            if (ops > 0.0) {
                data.work.push_back(aclptiRooflineWork{aclptiRooflineChannel::Simt, bw, ops});
            }
        }
    } else if (type == NPUBIT_MMAD_CONFIG_COLLECTION) {
        const auto* arr = static_cast<const MmadProcessedBytesAnalyzeData*>(analysis.data);
        const std::size_t count = analysis.size / sizeof(MmadProcessedBytesAnalyzeData);
        std::map<std::uint32_t, double> perBw;
        for (std::size_t i = 0; i < count; ++i) {
            // 新版 npubit 直接给 M/N/K，2MNK 精确；位宽按 A 矩阵类型取。
            const auto bw = NpubitDataTypeToBitWidth(arr[i].a_data_type);
            if (!bw) {
                continue;
            }
            perBw[*bw] += kFmaOperations * static_cast<double>(arr[i].m) * static_cast<double>(arr[i].n) *
                          static_cast<double>(arr[i].k);
        }
        for (const auto& [bw, ops] : perBw) {
            if (ops > 0.0) {
                data.work.push_back(aclptiRooflineWork{aclptiRooflineChannel::Cube, bw, ops});
            }
        }
    } else if (type == NPUBIT_MTE_CONFIG_COLLECTION) {
        // MTE 负责 cube 批量搬运(GM<->L1/UB 等 DMA)，累加进 q。
        const auto* arr = static_cast<const DmaUtilizationAnalyzeData*>(analysis.data);
        const std::size_t count = analysis.size / sizeof(DmaUtilizationAnalyzeData);
        std::uint64_t bytes = 0;
        for (std::size_t i = 0; i < count; ++i) {
            bytes += arr[i].processed_bytes;
        }
        data.gmBytes = data.gmBytes.value_or(0.0) + static_cast<double>(bytes);
    } else if (type == NPUBIT_SIMT_THREAD_EXE_COUNT) {
        // 记录原始 PC -> dtype 供 SIMT_BB_COUNT 按位宽分桶，并用 LDG/STG 的
        // 线程执行次数 x 元素字节计算 SIMT 的 GM 访存 q
        const auto* arr = static_cast<const SimtThreadExecutionCountAnalyzeData*>(analysis.data);
        const std::size_t count = analysis.size / sizeof(SimtThreadExecutionCountAnalyzeData);
        std::uint64_t globalMemoryBits = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (simtDtypeByAddr != nullptr && arr[i].data_type != NPUBIT_DATA_TYPE_UNKNOWN) {
                (*simtDtypeByAddr)[arr[i].instruction_address] = arr[i].data_type;
            }
            if (!IsSimtGlobalMemoryAccess(InstructionNameAt(handle, arr[i].instruction_address))) {
                continue;
            }
            const auto bits = NpubitDataTypeToBitWidth(arr[i].data_type);
            if (!bits) {
                continue;
            }
            globalMemoryBits += arr[i].execution_count * static_cast<std::uint64_t>(*bits);
        }
        if (globalMemoryBits > 0U) {
            data.gmBytes = data.gmBytes.value_or(0.0) + static_cast<double>(globalMemoryBits) / kBitsPerByte;
        }
    }
}

// Resolve llvm-objdump from the CANN toolchain (same location as bisheng-tune).
bool FindObjdump(boost::filesystem::path& objdump)
{
    const char* cann = std::getenv("ASCEND_HOME_PATH");
    if (cann == nullptr || cann[0] == '\0') {
        return false;
    }
    const auto directory = boost::filesystem::path(cann) / "tools/bisheng_compiler/bin";
    const auto candidate = directory / "llvm-objdump";
    if (!boost::filesystem::is_regular_file(candidate)) {
        return false;
    }
    objdump = candidate;
    return true;
}

// Statically classify the original kernel ELF by disassembling it: presence of plain
// "VF" (SIMD vecotr-function) vs "VF_SIMT" (warp) instructions decides which npubit
// passes to run. A pure-SIMT kernel must not run PUSH_PB, which faults on the device.
bool ClassifyKernel(const std::vector<char>& elf, bool& hasSimd, bool& hasSimt)
{
    boost::filesystem::path objdump;
    if (!FindObjdump(objdump)) {
        return false;
    }
    char tmpl[] = "/tmp/npu-compute-npubit-XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        return false;
    }
    bool written = true;
    const char* bytes = elf.data();
    std::size_t remaining = elf.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd, bytes, remaining);
        if (n <= 0) {
            written = false;
            break;
        }
        bytes += n;
        remaining -= static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (!written) {
        ::unlink(tmpl);
        return false;
    }

    hasSimd = false;
    hasSimt = false;
    const std::string command = "\"" + objdump.string() + "\" -d \"" + std::string(tmpl) + "\"";
    FILE* pipe = ::popen(command.c_str(), "r");
    bool classified = false;
    if (pipe != nullptr) {
        // A bare "VF" denotes an SIMD vector-function instruction; "VF_SIMT a warp op.
        const std::regex simdLine(R"(^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F]+\s+VF\s)");
        const std::regex simtLine(R"(^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F]+\s+VF_SIMT\s)");
        std::array<char, kObjdumpLineBufferSize> buffer{};
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            if (!hasSimd && std::regex_search(buffer.data(), simdLine)) {
                hasSimd = true;
            }
            if (!hasSimt && std::regex_search(buffer.data(), simtLine)) {
                hasSimt = true;
            }
        }
        classified = ::pclose(pipe) == 0;
    }
    ::unlink(tmpl);
    return classified;
}

aclptiResult CollectPass(
    NpubitInstrumentationType type, const AiCoreTopology& topology, const BinarySource& source,
    const ReplayMemory& memory, const ReplayLaunchFunction& launch, aclrtSynchronizeStreamFunc synchronize,
    aclrtStream stream, const std::function<void()>& beforeReplay, NpubitOutput& analysis, std::string& error,
    aclptiRooflineData* rooflineData = nullptr, std::map<std::uint64_t, NpubitDataType>* simtDtypeByAddr = nullptr)
{
    const NpubitHwContext hardwareContext{topology.dieCount, topology.coresPerDie};
    NpubitHandle handle{};
    if (!CheckNpubit(
            NpubitCreateHandle(source.image->data(), source.image->size(), &handle, V100, true), "NpubitCreateHandle",
            error)) {
        return ACLPTI_SUCCESS;
    }
    struct ReleaseHandle {
        NpubitHandle handle;
        ~ReleaseHandle() { NpubitReleaseHandle(handle); }
    } release{handle};
    if (NpubitSetHardwareContext(handle, &hardwareContext) != NPUBIT_SUCCESS) {
        error = "NpubitSetHardwareContext failed";
        return ACLPTI_SUCCESS;
    }
    auto replay = std::make_unique<NpubitReplay>();
    std::string args;
    if (!PrepareBasicBlocks(handle, *replay, args, error)) {
        return ACLPTI_SUCCESS;
    }
    if (!CheckNpubit(NpubitGetBufferSize(handle, type, &replay->traceBytes), "NpubitGetBufferSize", error)) {
        return ACLPTI_SUCCESS;
    }
    if (replay->traceBytes == std::numeric_limits<std::size_t>::max()) {
        replay->traceBytes = kDefaultTraceBytes;
    }
    const auto allocate = reinterpret_cast<aclrtMallocFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMalloc));
    if (!allocate || replay->traceBytes == 0 ||
        allocate(&replay->trace, replay->traceBytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
        error = "cannot allocate npubit Device trace buffer";
        return ACLPTI_SUCCESS;
    }
    if (!CheckNpubit(
            NpubitInstrument(handle, type, replay->trace, replay->traceBytes, args.c_str()), "NpubitInstrument",
            error) ||
        !InitializeBasicBlockMap(handle, *replay, error) || !ExtractObject(handle, *replay->image, error) ||
        !LoadFunction(source, *replay, error) || !InitializeTrace(*replay, error)) {
        return ACLPTI_SUCCESS;
    }
    const auto restore = memory.Restore();
    if (restore != ACLPTI_SUCCESS) {
        error = "NPUBIT input restoration failed: status=" + std::to_string(restore);
        return restore;
    }
    beforeReplay();
    // false selects the original business arguments. npubit embeds trace addresses in
    // the patched ELF; it must NOT use Pipeline's hidden-argument launch convention.
    const auto launchStatus = launch(replay->function, false);
    const auto syncStatus = synchronize(stream);
    if (syncStatus != ACL_SUCCESS) {
        // Kernel may still use Device memory/code. Preserve all replay resources.
        (void)replay.release();
        error = "NPUBIT replay synchronization failed: status=" + std::to_string(syncStatus);
        ASCTOOL_WARNING("[aclpti] NPUBIT synchronization failed; retaining in-flight resources until process exit");
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    if (launchStatus != ACL_SUCCESS) {
        error = "NPUBIT replay launch failed: status=" + std::to_string(launchStatus);
        return ACLPTI_ERROR_RESULT_UNRELIABLE;
    }
    std::vector<std::uint8_t> raw(replay->traceBytes);
    const auto copy = reinterpret_cast<aclrtMemcpyFunc>(acltoolGetOriginalRuntimeApi(ACL_RT_API_aclrtMemcpy));
    if (!copy ||
        copy(raw.data(), raw.size(), replay->trace, replay->traceBytes, ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        error = "cannot copy npubit trace Device to Host";
        return ACLPTI_SUCCESS;
    }
    (void)AnalyzeTrace(handle, type, raw, analysis, error);
    // 在handle释放前解码，过滤计算指令。
    if (rooflineData != nullptr && error.empty()) {
        DecodeRoofline(handle, type, analysis, *rooflineData, simtDtypeByAddr);
    }
    return ACLPTI_SUCCESS;
}
} // namespace

aclptiResult CollectNpubitRoofline(
    const BinarySource& source, const ReplayMemory& memory, const ReplayLaunchFunction& launch,
    aclrtSynchronizeStreamFunc synchronize, aclrtStream stream, std::int32_t deviceId,
    const std::function<void()>& beforeReplay, aclptiRooflineData& data)
{
    data.status = ACLPTI_SUCCESS;
    AiCoreTopology topology{};
    if (!ReadNpubitTopology(deviceId, topology)) {
        data.errors.emplace_back("cannot determine NPUbit AI core topology");
        data.status = ACLPTI_ERROR_PROFILING_FAILED;
        return ACLPTI_SUCCESS;
    }
    // A handle accepts one pass only. Each type owns a separate image, buffer and replay.
    // Choose which passes to run from a static disassembly of the original ELF:
    //  - SIMD (VF) present       -> run PUSH_PB_STATE
    //  - SIMT (VF_SIMT) present       -> run SIMT_BB_COUNT
    //  - MMAD / MTE always run.
    // A pure-SIMT kernel must NOT run PUSH_PB, which faults on the divice. If classification
    // fails we conservatively skip PUSH_PB to avoid that device fault.
    bool hasSimd = false;
    bool hasSimt = false;
    const bool classified = source.image && ClassifyKernel(*source.image, hasSimd, hasSimt);
    const auto include = [&](NpubitInstrumentationType type) -> bool {
        switch (type) {
            case NPUBIT_MTE_CONFIG_COLLECTION:
            case NPUBIT_MMAD_CONFIG_COLLECTION:
                return true; // safe and always needed
            case NPUBIT_PUSH_PB_STATE:
                return classified ? hasSimd : false; // skip for pure-SIMT / unknown
            case NPUBIT_SIMT_BB_COUNT:
                return classified ? hasSimt : false; // skip for pure-SIMD / unknown
            case NPUBIT_SIMT_THREAD_EXE_COUNT:
                return classified ? hasSimt : false;
            default:
                return false;
        }
    };
    // SIMT 位宽分类： 先跑 THREAD_EXE_COUNT 建原始PC -> dtype映射，再跑 BB_COUNT
    // 用该映射按位宽分桶。 两次pass均针对同一原始kernel，PC地址空间一致。
    std::map<std::uint64_t, NpubitDataType> simtDtypeByAddr;
    for (const auto type :
         {NPUBIT_PUSH_PB_STATE, NPUBIT_SIMT_THREAD_EXE_COUNT, NPUBIT_SIMT_BB_COUNT, NPUBIT_MMAD_CONFIG_COLLECTION,
          NPUBIT_MTE_CONFIG_COLLECTION}) {
        if (!include(type)) {
            continue;
        }
        std::string error;
        NpubitOutput analysis;
        const auto status = CollectPass(
            type, topology, source, memory, launch, synchronize, stream, beforeReplay, analysis, error, &data,
            &simtDtypeByAddr);
        if (!error.empty()) {
            data.errors.push_back("pass=" + std::to_string(static_cast<int>(type)) + ": " + error);
            data.status = ACLPTI_ERROR_PROFILING_FAILED;
        }
        if (status != ACLPTI_SUCCESS) {
            data.status = status;
            return status;
        }
    }
    return ACLPTI_SUCCESS;
}

} // namespace aclpti::profiling
