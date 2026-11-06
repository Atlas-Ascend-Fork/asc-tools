/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_BINARY_INSTRUMENTER_H
#define NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_BINARY_INSTRUMENTER_H

#include "dbi_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace aclsan {

struct BinaryInstrumentationConfig {
    std::string arch;
    // Runtime 观测到的 kernel 参数区下界；插桩阶段还会与 Device ELF 元数据边界取最大值。
    uint32_t traceArgumentOffset = 0;
    std::vector<ProbeGroup> probeGroups;
    std::string toolchainRoot;
    std::string workDirectory;
    std::string cacheDirectory;
    bool strict = false;
    bool keepTemp = false;
    bool useCompleteProbeSet = false;
    std::vector<std::string> tuneArgs;
};

enum class BinaryInstrumentationStatus : uint32_t { Skipped, Instrumented, Failed };

struct BinaryInstrumentationResult {
    BinaryInstrumentationStatus status = BinaryInstrumentationStatus::Skipped;
    std::vector<uint8_t> binary;
    std::string stage;
    std::string diagnostic;
    // 插桩后确定的隐藏参数偏移，供 launch hook 为每次 launch 注入独立的 Device GM buffer 指针。
    uint32_t traceArgumentOffset = 0;
};

using DbiPipelineRunner = DbiResult (*)(const DbiRequest&, const void*);
using InstrumentedBinaryConsumer = int32_t (*)(const void*, size_t, void*);

// acl_san and the DBI engine may use different libstdc++ ABIs. Keep this runtime boundary POD-only.
struct RuntimeBinaryInstrumentationResult {
    BinaryInstrumentationStatus status = BinaryInstrumentationStatus::Skipped;
    uint32_t strict = 0;
    int32_t consumerStatus = 0;
    // 跨 ABI 返回给 binary load hook 的已解析隐藏参数偏移。
    uint32_t traceArgumentOffset = 0;
};

static_assert(std::is_trivially_copyable_v<RuntimeBinaryInstrumentationResult>);
static_assert(sizeof(RuntimeBinaryInstrumentationResult) == 16);

bool BuildRuntimeInstrumentationConfig(
    const char* socName, const char* runtimeLibrary, uint32_t probeGroupMask, BinaryInstrumentationConfig& config,
    std::string& diagnostic, bool useCompleteProbeSet = false);
BinaryInstrumentationResult InstrumentBinary(
    const BinaryInstrumentationConfig& config, const void* data, size_t length, DbiPipelineRunner runner,
    const void* runnerData = nullptr);
BinaryInstrumentationResult InstrumentBinary(
    const BinaryInstrumentationConfig& config, const void* data, size_t length);
RuntimeBinaryInstrumentationResult InstrumentRuntimeBinary(
    const void* data, size_t length, uint32_t probeGroupMask, const char* socName, const char* runtimeLibrary,
    InstrumentedBinaryConsumer consumer, void* consumerData, DbiPipelineRunner runner, const void* runnerData = nullptr,
    uint32_t traceOffset = 0, bool appendParamInfo = true, bool useCompleteProbeSet = false) noexcept;
// Cross-target callers use this overload; the runner overload above is a same-ABI test seam.
RuntimeBinaryInstrumentationResult InstrumentRuntimeBinary(
    const void* data, size_t length, uint32_t probeGroupMask, const char* socName, const char* runtimeLibrary,
    InstrumentedBinaryConsumer consumer, void* consumerData) noexcept;
// Pointer presence distinguishes no selection from the valid tiling key zero.
RuntimeBinaryInstrumentationResult InstrumentRuntimeBinaryForEntry(
    const void* data, size_t length, uint32_t probeGroupMask, const char* socName, const char* runtimeLibrary,
    InstrumentedBinaryConsumer consumer, void* consumerData, const uint64_t* entry, uint32_t traceOffset = 0,
    bool appendParamInfo = true, bool useCompleteProbeSet = false) noexcept;

} // namespace aclsan

#endif // NPU_TOOLS_NPU_CHECK_SRC_ACL_SAN_DBI_BINARY_INSTRUMENTER_H
