/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_BINARY_REGISTRY_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_BINARY_REGISTRY_H

#include "injection/injection_hook.h"
#include "aclpti/aclpti_types.h"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include "pm_sampling_instruction_map.h"
#include <unordered_map>
#include <vector>

namespace aclpti::profiling {

struct PmSamplingBinaryImage {
    std::vector<char> originalElf;
    std::vector<char> companionElf;
    PmSamplingInstructionMap instructions;
};

struct PmSamplingBinarySnapshot {
    std::shared_ptr<const PmSamplingBinaryImage> image;
    std::uint64_t elfEntry = 0;
    std::uint64_t aicRuntimeEntry = 0;
    std::uint64_t aivRuntimeEntry = 0;
    std::string error;
};

class BinaryRegistry {
public:
    // Holds the registry lock through the original Runtime unload. Destroy without
    // completing on original failure; complete only after original unload succeeds.
    class UnloadContext {
        friend class BinaryRegistry;
        std::unique_lock<std::mutex> lock_;
        aclrtBinHandle binary_ = nullptr;
    };
    aclptiResult RegisterBinary(
        const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary,
        bool pmSampling = false);
    aclptiResult RegisterBinaryFromFile(
        const char* path, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary, bool pmSampling = false);
    aclptiResult RegisterBinaryFunction(aclrtBinHandle binary, const char* name, aclrtFuncHandle function);
    aclptiResult RegisterBinaryFunction(aclrtBinHandle binary, std::uint64_t entry, aclrtFuncHandle function);
    aclptiResult RegisterSymbolFunction(aclrtFuncHandle function, bool pmSampling = false);
    aclptiResult PrepareBinaryUnload(aclrtBinHandle binary, UnloadContext& context);
    aclptiResult CompleteBinaryUnload(UnloadContext& context);
    // Capture while Runtime handles are valid. Shared immutable images outlive binary unload.
    bool GetPmSamplingSnapshot(aclrtFuncHandle original, PmSamplingBinarySnapshot& snapshot);
    // Null means the function was not loaded with instrumentation enabled.
    aclrtFuncHandle FindInstrumentedFunction(aclrtFuncHandle original);
    // Lookup by companion function; valid only while its instrumented binary is registered.
    bool GetInstrumentedTraceArgumentOffset(aclrtFuncHandle function, std::uint32_t& offset);

private:
    struct Binary {
        aclrtBinHandle companion = nullptr;
        std::unordered_map<aclrtFuncHandle, aclrtFuncHandle> functions;
        // Runtime may retain the image pointer; keep it alive through companion unload.
        std::shared_ptr<const std::vector<char>> instrumentedImage;
        std::shared_ptr<const PmSamplingBinaryImage> pmSamplingImage;
        std::unordered_map<aclrtFuncHandle, std::uint64_t> elfEntries;
        std::uint32_t traceArgumentOffset = 0;
    };
    template <typename Function, typename Key>
    aclptiResult ResolveBinaryFunction(aclrtApiId id, aclrtBinHandle binary, Key key, aclrtFuncHandle function);
    aclptiResult RegisterInstrumentedBinary(
        const void* data, std::size_t size, const aclrtBinaryLoadOptions* options, aclrtBinHandle binary,
        bool pmSampling = false);
    std::mutex mutex_;
    std::unordered_map<aclrtBinHandle, Binary> binaries_;
};

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_BINARY_REGISTRY_H
