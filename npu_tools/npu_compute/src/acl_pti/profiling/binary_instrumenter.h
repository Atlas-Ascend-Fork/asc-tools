/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_BINARY_INSTRUMENTER_H
#define NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_BINARY_INSTRUMENTER_H

#include "pm_sampling_instruction_map.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aclpti::profiling {

// Runs the kernel-end (Pipeline) toolchain and returns a complete instrumented ELF image.
// If requested, returns the exact --tune-argsize offset on success, or zero on failure.
// A PmSampling mapping failure is returned in pmSamplingMap->error; it does not
// discard a successfully rewritten companion or fail binary registration.
bool InstrumentKernelEnd(
    const void* data, std::size_t size, std::vector<char>& output, std::uint32_t* outputTraceArgumentOffset = nullptr,
    PmSamplingInstructionMap* pmSamplingMap = nullptr);

// Builds an independent SIMT sampling companion with a start probe.
bool InstrumentSimtSampling(
    const void* data, std::size_t size, std::vector<char>& output, std::uint32_t* probeArgumentOffset = nullptr,
    PmSamplingInstructionMap* pmSamplingMap = nullptr);

// Builds the mapping for a replay that launches the original ELF. No probe or
// replacement kernel is generated; the map still classifies SIMT business PCs.
bool BuildOriginalPmSamplingInstructionMap(const void* data, std::size_t size, PmSamplingInstructionMap& output);

} // namespace aclpti::profiling

#endif // NPU_TOOLS_NPU_COMPUTE_SRC_ACL_PTI_PROFILING_BINARY_INSTRUMENTER_H
