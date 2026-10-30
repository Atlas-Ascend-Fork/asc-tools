/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_ARTIFACTS_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_ARTIFACTS_H

#include "aclpti/aclpti_data.h"
#include "pm_sampling_types.h"
#include <cstdint>
#include <string>

namespace npucompute {
enum class PmSamplingElfSource { Original, Companion };
inline constexpr std::uint32_t kPmSamplingToolTimeoutMs = 60000;

// Internal input policy, not additional user-facing CLI flags. Change either
// source here without changing the LLVM invocation or the sampling execution.
// Sampling rows and JSON Address always use original ELF addresses.
struct PmSamplingArtifactOptions {
    PmSamplingElfSource disassemblySource = PmSamplingElfSource::Original;
    PmSamplingElfSource pcStackSource = PmSamplingElfSource::Original;
    // Directory containing llvm-objdump and llvm-symbolizer. If empty, resolve
    // it from ASCEND_HOME_PATH/tools/bisheng_compiler/bin.
    std::string toolDirectory;
    std::uint32_t timeoutMs = kPmSamplingToolTimeoutMs; // Per tool.
};
struct PmSamplingArtifacts {
    std::string disassemblyPath;
    std::string pcStackPath;
    std::uint64_t unresolvedPcCount = 0; // No positive source line; data is still retained.
};

// Caller owns the private staging directory and excludes failed output from
// packaging. Temporary ELF/tool files are cleaned here on both success and failure.
// Outputs are objdump.txt and symbolizer.json. On partial failure, successful
// paths remain available and error identifies each failed output.
bool GeneratePmSamplingArtifacts(
    const std::string& stagingDirectory, const aclptiPmSamplingContext& context, const PmSamplingResult& sampling,
    const PmSamplingArtifactOptions& options, PmSamplingArtifacts& artifacts, std::string& error);
// A null context writes an unavailable-disassembly marker and an empty symbolizer array.
bool GeneratePmSamplingArtifacts(
    const std::string& stagingDirectory, const aclptiPmSamplingContext* context, const PmSamplingResult& sampling,
    const PmSamplingArtifactOptions& options, PmSamplingArtifacts& artifacts, std::string& error);
} // namespace npucompute

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_COMPUTE_PM_SAMPLING_PM_SAMPLING_ARTIFACTS_H
